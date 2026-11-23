// LoadShift — in-process CPU→GPU offload for selected apps (Zygisk).
//
// No code patching: the module swaps function pointers in tables that already exist
//   * libEGL's GLES dispatch table (gHooks), which points at the Adreno GLES driver,
//   * the Adreno Vulkan driver's own name→function table (.data.rel.ro),
// so it can coexist with inline-hooking modules (e.g. Portable GameFX) in the same game.
//
// Features (per-app config from /data/adb/loadshift/profiles/<pkg>.conf):
//   gl_async_finish : glFinish() no longer blocks the CPU until the GPU drains. It becomes
//                     fence + flush; the CPU only waits for the fence from <depth> calls ago.
//   gl_pbo_upload   : large glTex(Sub)Image2D uploads go through a pixel-unpack buffer, so the
//                     GPU copy engine does the layout/tiling conversion instead of the CPU.
//   vk_pipeline_cache: Vulkan pipelines compiled once are persisted to disk and reused, and
//                     apps that pass VK_NULL_HANDLE get an implicit persistent cache. Removes
//                     repeated shader-compile CPU spikes.

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <vulkan/vulkan.h>

#include <atomic>

#include "zygisk.hpp"

#define TAG "LoadShift"
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)

// ---------------------------------------------------------------------------------------------
// config / stats

struct Config {
    bool enabled = false;
    bool glFinish = false;
    int finishDepth = 2;
    int pbo = 0;  // 0 off, 1 smart, 2 all
    int pboMinKB = 64;
    bool vkCache = false;
    bool vkIdle = false;
    int vkIdleLevel = 1;
    int vkIdleDepth = 3;
    bool occl = false;
    bool readback = false;
    int flushUs = 0;       // 0 = off; drop glFlush calls closer than this (single-context only)
    int bufRingMB = 32;
    int texRingMB = 48;
    int copyThreads = 4;
    int copyThresholdKB = 2048;
    bool glBuf = false;
    int glBufMin = 1024;
    // coexistence with other graphics hook modules
    int compatInstall = 1;    // 0 early (at app start), 1 auto, 2 late (after other hooks)
    int compatTimeoutMs = 3000;
    bool foreignFg = false;   // set by the companion: another graphics hook module is active
    bool exemptForeign = true;
    int pipeSplit = 4;        // threads for splitting batched pipeline creation (0/1 = off)
    int memPoolMB = 0;        // VkDeviceMemory recycling pool size, 0 = off
};
static Config cfg;
static char g_dir[512];   // <app data>/cache/loadshift
static char g_pkg[256];

static std::atomic<uint64_t> st_finish{0}, st_finishWaitNs{0}, st_finishNative{0};
static std::atomic<uint64_t> st_pbo{0}, st_pboBytes{0}, st_pboSkip{0};
static std::atomic<uint64_t> st_vkPipes{0}, st_vkPipeNs{0}, st_vkImplicit{0}, st_vkSeeded{0};
static std::atomic<uint64_t> st_vkSaved{0}, st_vkLoaded{0};
static char st_hooks[320] = "none";
static std::atomic<bool> g_dirty{false};

static uint64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

static void parse_config(const char *text) {
    cfg = Config();
    const char *p = text;
    while (p && *p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[128];
        if (n < sizeof(line)) {
            memcpy(line, p, n);
            line[n] = 0;
            char *eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                int v = atoi(eq + 1);
                if (!strcmp(line, "enabled")) cfg.enabled = v;
                else if (!strcmp(line, "gl_async_finish")) cfg.glFinish = v;
                else if (!strcmp(line, "gl_async_finish_depth")) cfg.finishDepth = v < 1 ? 1 : (v > 6 ? 6 : v);
                else if (!strcmp(line, "gl_pbo_upload")) cfg.pbo = v < 0 ? 0 : (v > 2 ? 2 : v);
                else if (!strcmp(line, "gl_pbo_min_kb")) cfg.pboMinKB = v < 4 ? 4 : v;
                else if (!strcmp(line, "vk_pipeline_cache")) cfg.vkCache = v;
                else if (!strcmp(line, "vk_lazy_idle")) cfg.vkIdle = v;
                else if (!strcmp(line, "vk_idle_level")) cfg.vkIdleLevel = v >= 2 ? 2 : 1;
                else if (!strcmp(line, "vk_idle_depth")) cfg.vkIdleDepth = v;
                else if (!strcmp(line, "gl_buffer_offload")) cfg.glBuf = v;
                else if (!strcmp(line, "occlusion_nowait")) cfg.occl = v;
                else if (!strcmp(line, "gl_readback_async")) cfg.readback = v;
                else if (!strcmp(line, "gl_flush_coalesce_us")) cfg.flushUs = v < 0 ? 0 : (v > 8000 ? 8000 : v);
                else if (!strcmp(line, "gl_buf_ring_mb")) cfg.bufRingMB = v < 8 ? 8 : (v > 128 ? 128 : v);
                else if (!strcmp(line, "gl_tex_ring_mb")) cfg.texRingMB = v < 16 ? 16 : (v > 192 ? 192 : v);
                else if (!strcmp(line, "copy_threads")) cfg.copyThreads = v < 1 ? 1 : (v > 8 ? 8 : v);
                else if (!strcmp(line, "copy_threshold_kb")) cfg.copyThresholdKB = v < 64 ? 64 : v;
                else if (!strcmp(line, "compat_install")) cfg.compatInstall = v < 0 ? 0 : (v > 2 ? 2 : v);
                else if (!strcmp(line, "compat_timeout_ms")) cfg.compatTimeoutMs = v < 200 ? 200 : (v > 30000 ? 30000 : v);
                else if (!strcmp(line, "foreign_fg")) cfg.foreignFg = v;
                else if (!strcmp(line, "compat_exempt")) cfg.exemptForeign = v;
                else if (!strcmp(line, "vk_pipeline_split")) cfg.pipeSplit = v < 0 ? 0 : (v > 8 ? 8 : v);
                else if (!strcmp(line, "vk_mem_pool_mb")) cfg.memPoolMB = v < 0 ? 0 : (v > 1024 ? 1024 : v);
                else if (!strcmp(line, "gl_buffer_min")) cfg.glBufMin = v < 0 ? 0 : v;
            }
        }
        p = e ? e + 1 : nullptr;
    }
}

static bool write_atomic(const char *name, const void *data, size_t len) {
    char path[600], tmp[620];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    snprintf(tmp, sizeof(tmp), "%s.tmp%d", path, getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const char *c = (const char *)data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, c, left);
        if (w <= 0) { close(fd); unlink(tmp); return false; }
        c += w; left -= w;
    }
    fsync(fd);
    close(fd);
    return rename(tmp, path) == 0;
}

static int extra_stats(char *b, size_t cap);
static void write_stats() {
    char b[5000];
    uint64_t f = st_finish.load(), pipes = st_vkPipes.load();
    int n = snprintf(b, sizeof(b),
        "pkg=%s\npid=%d\nhooks=%s\n"
        "gl_finish_async=%llu\ngl_finish_wait_ms=%.1f\ngl_finish_native=%llu\n"
        "gl_pbo_uploads=%llu\ngl_pbo_mb=%.1f\ngl_pbo_skipped=%llu\n"
        "vk_pipelines=%llu\nvk_pipeline_avg_ms=%.2f\nvk_implicit_cache=%llu\nvk_seeded_cache=%llu\n"
        "vk_cache_loaded_kb=%llu\nvk_cache_saved_kb=%llu\nupdated=%ld\n",
        g_pkg, getpid(), st_hooks,
        (unsigned long long)f, st_finishWaitNs.load() / 1e6, (unsigned long long)st_finishNative.load(),
        (unsigned long long)st_pbo.load(), st_pboBytes.load() / 1048576.0, (unsigned long long)st_pboSkip.load(),
        (unsigned long long)pipes, pipes ? st_vkPipeNs.load() / 1e6 / pipes : 0.0,
        (unsigned long long)st_vkImplicit.load(), (unsigned long long)st_vkSeeded.load(),
        (unsigned long long)st_vkLoaded.load() / 1024, (unsigned long long)st_vkSaved.load() / 1024,
        (long)time(nullptr));
    if (n < 0) return;
    if (n >= (int)sizeof(b)) n = sizeof(b) - 1;
    int e = extra_stats(b + n, sizeof(b) - n);
    if (e > 0) n += e;
    if (n >= (int)sizeof(b)) n = sizeof(b) - 1;
    write_atomic("stats.txt", b, n);
}

// ---------------------------------------------------------------------------------------------
// ELF helpers: find a loaded library, resolve its dynsym from the file, scan/patch its data.

struct LoadedLib {
    uintptr_t bias = 0;
    char path[256] = {};
    struct Seg { uintptr_t start, end; bool relro; } segs[8];
    int nsegs = 0;
};

struct FindCtx { const char *suffix; LoadedLib *out; bool found; };

static int phdr_cb(dl_phdr_info *info, size_t, void *data) {
    FindCtx *c = (FindCtx *)data;
    if (!info->dlpi_name) return 0;
    size_t ln = strlen(info->dlpi_name), ls = strlen(c->suffix);
    if (ln < ls || strcmp(info->dlpi_name + ln - ls, c->suffix)) return 0;
    if (ln > ls && info->dlpi_name[ln - ls - 1] != '/') return 0;
    LoadedLib *L = c->out;
    L->bias = info->dlpi_addr;
    snprintf(L->path, sizeof(L->path), "%s", info->dlpi_name);
    uintptr_t relroStart = 0, relroEnd = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) &p = info->dlpi_phdr[i];
        if (p.p_type == PT_GNU_RELRO) { relroStart = L->bias + p.p_vaddr; relroEnd = relroStart + p.p_memsz; }
    }
    for (int i = 0; i < info->dlpi_phnum && L->nsegs < 8; i++) {
        const ElfW(Phdr) &p = info->dlpi_phdr[i];
        if (p.p_type != PT_LOAD || !(p.p_flags & PF_W)) continue;
        uintptr_t s = L->bias + p.p_vaddr, e = s + p.p_memsz;
        L->segs[L->nsegs++] = {s, e, relroStart && s >= relroStart && s < relroEnd};
    }
    c->found = true;
    return 1;
}

static bool find_lib(const char *suffix, LoadedLib *out) {
    FindCtx c{suffix, out, false};
    dl_iterate_phdr(phdr_cb, &c);
    return c.found;
}

// Resolve several symbols from the on-disk .dynsym of a loaded library.
static int resolve_syms(const LoadedLib &L, const char *const *names, void **out, int count) {
    int fd = open(L.path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return 0; }
    uint8_t *m = (uint8_t *)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return 0;
    int found = 0;
    auto *eh = (ElfW(Ehdr) *)m;
    if (st.st_size > (off_t)sizeof(*eh) && !memcmp(eh->e_ident, ELFMAG, SELFMAG) &&
        eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(ElfW(Shdr)) <= (uint64_t)st.st_size) {
        auto *sh = (ElfW(Shdr) *)(m + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
            auto *syms = (ElfW(Sym) *)(m + sh[i].sh_offset);
            const char *strs = (const char *)(m + sh[sh[i].sh_link].sh_offset);
            size_t n = sh[i].sh_size / sizeof(ElfW(Sym));
            for (size_t k = 0; k < n; k++) {
                if (!syms[k].st_value || ELF64_ST_TYPE(syms[k].st_info) != STT_FUNC) continue;
                const char *nm = strs + syms[k].st_name;
                for (int j = 0; j < count; j++)
                    if (!out[j] && !strcmp(nm, names[j])) { out[j] = (void *)(L.bias + syms[k].st_value); found++; }
            }
        }
    }
    munmap(m, st.st_size);
    return found;
}

// Replace every pointer-sized slot in L's writable/RELRO segments equal to `from` with `to`.
static int patch_slots(const LoadedLib &L, void *from, void *to) {
    int n = 0;
    long pg = sysconf(_SC_PAGESIZE);
    for (int s = 0; s < L.nsegs; s++) {
        for (uintptr_t a = (L.segs[s].start + 7) & ~7ul; a + 8 <= L.segs[s].end; a += 8) {
            if (*(void *volatile *)a != from) continue;
            uintptr_t page = a & ~(uintptr_t)(pg - 1);
            if (L.segs[s].relro && mprotect((void *)page, pg, PROT_READ | PROT_WRITE)) continue;
            __atomic_store_n((void **)a, to, __ATOMIC_SEQ_CST);
            if (L.segs[s].relro) mprotect((void *)page, pg, PROT_READ);
            n++;
        }
    }
    return n;
}

#include "coexist.inc"

// Hook a Vulkan driver function: slots from the driver file (order-independent, chains onto an
// existing foreign hook); falls back to value scanning. *orig receives what to call next.
static int vkH(const LoadedLib &drv, void *fn, void *mine, void **orig, bool consuming = false) {
    int n = coexist::vk_hook(drv, fn, mine, orig, consuming);
    if (n < 0) { __atomic_store_n(orig, fn, __ATOMIC_RELEASE); return 0; }  // yielded to another hook
    if (n) return n;
    __atomic_store_n(orig, fn, __ATOMIC_RELEASE);
    return patch_slots(drv, fn, mine);
}

// ---------------------------------------------------------------------------------------------
// GLES

typedef void (*PFN_Finish)();
typedef void (*PFN_TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*PFN_TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
static PFN_Finish o_glFinish;
static PFN_TexImage2D o_glTexImage2D;
static PFN_TexSubImage2D o_glTexSubImage2D;

struct GlCtx {
    EGLContext ctx = EGL_NO_CONTEXT;
    int es3 = -1;               // -1 unknown
    GLsync ring[8] = {};
    int head = 0, count = 0;
    GLuint pbo[4] = {};
    GLsizeiptr pboSize[4] = {};
    int pboNext = 0;
};
static GlCtx g_ctx[16];
static pthread_mutex_t g_ctxLock = PTHREAD_MUTEX_INITIALIZER;
static EGLContext (*p_eglGetCurrentContext)();

static void on_ctx(int idx, bool fresh);
static GlCtx *cur_ctx() {
    EGLContext c = p_eglGetCurrentContext ? p_eglGetCurrentContext() : EGL_NO_CONTEXT;
    if (c == EGL_NO_CONTEXT) return nullptr;
    static thread_local GlCtx *last = nullptr;
    if (last && last->ctx == c) return last;
    pthread_mutex_lock(&g_ctxLock);
    GlCtx *hit = nullptr, *freeSlot = nullptr;
    for (auto &g : g_ctx) {
        if (g.ctx == c) { hit = &g; break; }
        if (!freeSlot && g.ctx == EGL_NO_CONTEXT) freeSlot = &g;
    }
    bool fresh = false;
    if (!hit) {
        hit = freeSlot ? freeSlot : &g_ctx[(uintptr_t)c % 16];
        *hit = GlCtx();
        hit->ctx = c;
        fresh = true;
    }
    pthread_mutex_unlock(&g_ctxLock);
    on_ctx((int)(hit - g_ctx), fresh);
    if (hit->es3 < 0) {
        const char *v = (const char *)glGetString(GL_VERSION);
        hit->es3 = v && strstr(v, "OpenGL ES 3") ? 1 : 0;
    }
    return last = hit;
}

// Bumped by every glFinish the app issues: work started before it must really be complete when
// the app later depends on it (e.g. reading an occlusion query).
static std::atomic<uint32_t> g_finishEpoch{1};
// Set when the app maps a buffer persistently for READ: it may read GPU results straight from
// memory after glFinish, so glFinish has to stay a real wait in that process.
static std::atomic<bool> g_finishUnsafe{false};

static void my_glFinish() {
    if (LS_FOREIGN()) { o_glFinish(); return; }
    g_finishEpoch++;
    GlCtx *g = cfg.glFinish && !g_finishUnsafe.load() ? cur_ctx() : nullptr;
    if (!g || g->es3 != 1) { st_finishNative++; o_glFinish(); return; }
    GLsync s = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    if (!s) { st_finishNative++; o_glFinish(); return; }
    int cap = cfg.finishDepth;
    g->ring[(g->head + g->count) % 8] = s;
    g->count++;
    while (g->count > cap) {
        GLsync old = g->ring[g->head];
        g->head = (g->head + 1) % 8;
        g->count--;
        uint64_t t0 = now_ns();
        glClientWaitSync(old, GL_SYNC_FLUSH_COMMANDS_BIT, 200000000ull);
        st_finishWaitNs += now_ns() - t0;
        glDeleteSync(old);
    }
    st_finish++;
}

static int bytes_per_pixel(GLenum format, GLenum type) {
    int comps;
    switch (format) {
        case GL_RED: case GL_ALPHA: case GL_LUMINANCE: case GL_RED_INTEGER: comps = 1; break;
        case GL_RG: case GL_LUMINANCE_ALPHA: case GL_RG_INTEGER: comps = 2; break;
        case GL_RGB: case GL_RGB_INTEGER: comps = 3; break;
        case GL_RGBA: case GL_RGBA_INTEGER: case 0x80E1 /*GL_BGRA_EXT*/: comps = 4; break;
        default: return 0;
    }
    switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: return comps;
        case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case 0x8D61 /*HALF_FLOAT_OES*/: return comps * 2;
        case GL_UNSIGNED_INT: case GL_INT: case GL_FLOAT: return comps * 4;
        case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_5_5_5_1: return 2;
        case GL_UNSIGNED_INT_2_10_10_10_REV: case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV: return 4;
        default: return 0;
    }
}

#include "gl_buffer.inc"
#include "tex_upload.inc"
#include "occlusion.inc"
#include "readback.inc"

// ---- glFlush coalescing (aggressive, opt-in) --------------------------------------------------
// Every glFlush is a kernel submission (~20 us of CPU on Adreno). With a single GL context and no
// fence created since the last real flush, a flush is only a scheduling hint, so flushes closer
// together than cfg.flushUs are dropped. Multi-context apps keep every flush (cross-context
// visibility relies on it).
namespace gflush {
typedef void (*PFN_Flush)();
typedef GLsync (*PFN_FenceSync)(GLenum, GLbitfield);
static PFN_Flush o_Flush;
static PFN_FenceSync o_FenceSync;
static thread_local uint64_t t_last;
static std::atomic<bool> g_fenced{false};
static std::atomic<uint64_t> st_dropped{0}, st_real{0};

static int live_contexts() {
    int n = 0;
    for (auto &g : g_ctx) if (g.ctx != EGL_NO_CONTEXT) n++;
    return n;
}

static void my_Flush() {
    if (LS_FOREIGN()) { o_Flush(); return; }
    uint64_t now = now_ns();
    if (!g_fenced.load() && live_contexts() <= 1 && now - t_last < (uint64_t)cfg.flushUs * 1000) { st_dropped++; return; }
    g_fenced = false;
    t_last = now;
    st_real++;
    o_Flush();
}

static GLsync my_FenceSync(GLenum c, GLbitfield f) {
    if (LS_FOREIGN()) return o_FenceSync(c, f);
    g_fenced = true;
    return o_FenceSync(c, f);
}
}  // namespace gflush

static void on_ctx(int idx, bool fresh) {
    glbuf::Ring &r = glbuf::g_rings[idx];
    // New context, or an old slot whose context was destroyed and the pointer reused.
    if (fresh || (r.ok && !glIsBuffer(r.buf))) {
        r = glbuf::Ring();
    }
    texup::Ring &t = texup::g_rings[idx];
    if (fresh || (t.ok && !glIsBuffer(t.buf))) t = texup::Ring();
}

static int install_gles(char *log, size_t cap, bool mayLoadDriver) {
    void *egl = dlopen("libEGL.so", RTLD_NOW);
    if (!egl) return 0;
    p_eglGetCurrentContext = (decltype(p_eglGetCurrentContext))dlsym(egl, "eglGetCurrentContext");
    auto getDisplay = (EGLDisplay(*)(EGLNativeDisplayType))dlsym(egl, "eglGetDisplay");
    LoadedLib drv;
    // Zygote normally preloads the driver; only force-load it when installing early.
    if (!find_lib("libGLESv2_adreno.so", &drv) && getDisplay && mayLoadDriver) getDisplay(EGL_DEFAULT_DISPLAY);
    if (!find_lib("libGLESv2_adreno.so", &drv)) { snprintf(log, cap, "gles:no-adreno "); return 0; }
    LoadedLib eglLib;
    if (!find_lib("libEGL.so", &eglLib)) return 0;
    coexist::g_glCopies = coexist::count_loaded("libGLESv2_adreno.so");
    const char *names[] = {"glFinish", "glTexImage2D", "glTexSubImage2D",
                           "glBufferSubData", "glMapBufferRange", "glFlushMappedBufferRange", "glUnmapBuffer",
                           "glTexImage3D", "glTexSubImage3D", "glCompressedTexImage2D", "glCompressedTexSubImage2D",
                           "glBeginQuery", "glGetQueryObjectuiv", "glReadPixels", "glFlush", "glFenceSync"};
    void *addr[16] = {};
    resolve_syms(drv, names, addr, 16);
    glbuf::kChunk = (GLsizeiptr)cfg.bufRingMB * 1048576 / glbuf::kChunks;
    texup::kSize = (GLsizeiptr)cfg.texRingMB * 1048576;
    texup::kWorkers = cfg.copyThreads - 1 > texup::kMaxWorkers ? texup::kMaxWorkers : cfg.copyThreads - 1;
    texup::g_copyThreshold = (size_t)cfg.copyThresholdKB * 1024;
    auto gpa = (void *(*)(const char *))dlsym(egl, "eglGetProcAddress");
    glbuf::p_BufferStorageEXT = gpa ? (glbuf::PFN_BufferStorageEXT)gpa("glBufferStorageEXT") : nullptr;

    // Slot location by trampoline decoding (works even if another module already swapped the
    // slot); falls back to value scanning when the layout can't be decoded.
    bool layout = coexist::gl_find_bases(drv);
    // Hooks that can complete a call without calling the next function.
    // GL hooks transform calls (GPU copies, PBO offsets, async answers), so all of them yield
    // to an existing foreign hook except BeginQuery / FenceSync, which always pass through as-is.
    auto consuming = [](int k) { return !(k == 11 || k == 15); };
    auto H = [&](int k, void *mine, void **orig) -> int {
        if (!addr[k]) return 0;
        if (layout) {
            int n = coexist::gl_hook(names[k], addr[k], mine, orig, consuming(k));
            if (n < 0) { __atomic_store_n(orig, addr[k], __ATOMIC_RELEASE); return 0; }  // yielded
            if (n) return n;
        }
        __atomic_store_n(orig, addr[k], __ATOMIC_RELEASE);
        return patch_slots(eglLib, addr[k], mine);
    };
    int nf = 0, ni = 0, ns = 0;
    if (cfg.glFinish) nf = H(0, (void *)my_glFinish, (void **)&o_glFinish);
    if (cfg.pbo && addr[1] && addr[2]) {
        texup::g_mode = cfg.pbo;
        texup::g_minKB = cfg.pboMinKB;
        ni = H(1, (void *)texup::my_TexImage2D, (void **)&o_glTexImage2D);
        ns = H(2, (void *)texup::my_TexSubImage2D, (void **)&o_glTexSubImage2D);
        if (addr[7] && addr[8]) {
            ni += H(7, (void *)texup::my_TexImage3D, (void **)&texup::o_TexImage3D);
            ns += H(8, (void *)texup::my_TexSubImage3D, (void **)&texup::o_TexSubImage3D);
        }
        if (addr[9] && addr[10]) {
            ni += H(9, (void *)texup::my_CTexImage2D, (void **)&texup::o_CTexImage2D);
            ns += H(10, (void *)texup::my_CTexSubImage2D, (void **)&texup::o_CTexSubImage2D);
        }
    }
    int nb = 0;
    if (cfg.glBuf && addr[3] && addr[4] && addr[5] && addr[6]) {
        glbuf::g_min = cfg.glBufMin;
        nb = (H(3, (void *)glbuf::my_BufferSubData, (void **)&glbuf::o_BufferSubData) > 0) +
             (H(4, (void *)glbuf::my_MapBufferRange, (void **)&glbuf::o_MapBufferRange) > 0) +
             (H(5, (void *)glbuf::my_FlushMappedBufferRange, (void **)&glbuf::o_FlushMappedBufferRange) > 0) +
             (H(6, (void *)glbuf::my_UnmapBuffer, (void **)&glbuf::o_UnmapBuffer) > 0);
    } else if (cfg.glFinish && addr[4] && addr[5] && addr[6]) {
        // Map hook only as a watchdog for persistent READ maps (keeps async glFinish safe).
        glbuf::o_FlushMappedBufferRange = (glbuf::PFN_FlushMappedBufferRange)addr[5];
        glbuf::o_UnmapBuffer = (glbuf::PFN_UnmapBuffer)addr[6];
        H(4, (void *)glbuf::my_MapBufferRange, (void **)&glbuf::o_MapBufferRange);
    }
    int no = 0;
    if (cfg.occl && addr[11] && addr[12])
        no = (H(11, (void *)occl::my_BeginQuery, (void **)&occl::o_BeginQuery) > 0) +
             (H(12, (void *)occl::my_GetQueryObjectuiv, (void **)&occl::o_GetQueryObjectuiv) > 0);
    int nr = 0, nfl = 0;
    if (cfg.readback) nr = H(13, (void *)rback::my_ReadPixels, (void **)&rback::o_ReadPixels) > 0;
    if (cfg.flushUs > 0 && addr[14] && addr[15])
        nfl = (H(14, (void *)gflush::my_Flush, (void **)&gflush::o_Flush) > 0) +
              (H(15, (void *)gflush::my_FenceSync, (void **)&gflush::o_FenceSync) > 0);
    snprintf(log, cap, "gles%s:finish%d,tex%d/%d,buf%d/4,occl%d/2,rb%d,flush%d/2 ", layout ? "" : "(scan)", nf, ni, ns, nb, no, nr, nfl);
    return nf + ni + ns + nb + no + nr + nfl;
}

// ---------------------------------------------------------------------------------------------
// Vulkan pipeline cache

static PFN_vkCreatePipelineCache o_CreatePipelineCache;
static PFN_vkDestroyPipelineCache o_DestroyPipelineCache;
static PFN_vkGetPipelineCacheData o_GetPipelineCacheData;
static PFN_vkCreateGraphicsPipelines o_CreateGraphicsPipelines;
static PFN_vkCreateComputePipelines o_CreateComputePipelines;
static PFN_vkDestroyDevice o_DestroyDevice;
static PFN_vkMergePipelineCaches o_MergePipelineCaches;

static pthread_mutex_t g_vkLock = PTHREAD_MUTEX_INITIALIZER;
// One "master" cache per device: seeded from disk, used for VK_NULL_HANDLE creates, and the
// place every app-owned cache is merged into before saving.
struct Implicit { VkDevice dev; VkPipelineCache cache; };
static Implicit g_impl[8];
// The app's own pipeline caches (e.g. Unreal/Unity PSO caches) — merged into the master.
struct AppCache { VkDevice dev; VkPipelineCache cache; };
static AppCache g_appCaches[64];
static std::atomic<uint64_t> st_vkAppCaches{0}, st_vkInjected{0}, st_vkMerges{0};
// Caches the app created with EXTERNALLY_SYNCHRONIZED: never used from several threads at once.
static VkPipelineCache g_extCaches[32];
static uint8_t *g_blob;
static size_t g_blobSize;

struct BlobHeader { uint32_t magic, version, size, sum; };
static const uint32_t kMagic = 0x4b56534c;  // "LSVK"

static uint32_t checksum(const uint8_t *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

static void load_blob() {
    char path[600];
    snprintf(path, sizeof(path), "%s/vk_pipeline.bin", g_dir);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    BlobHeader h;
    if (read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) && h.magic == kMagic && h.version == 1 &&
        h.size > 32 && h.size < (64u << 20)) {
        uint8_t *b = (uint8_t *)malloc(h.size);
        if (b && read(fd, b, h.size) == (ssize_t)h.size && checksum(b, h.size) == h.sum) {
            g_blob = b; g_blobSize = h.size; st_vkLoaded = h.size;
        } else free(b);
    }
    close(fd);
}

static void save_cache(VkDevice dev, VkPipelineCache cache) {
    size_t size = 0;
    if (o_GetPipelineCacheData(dev, cache, &size, nullptr) != VK_SUCCESS || size <= 32 || size > (64u << 20)) return;
    uint8_t *b = (uint8_t *)malloc(sizeof(BlobHeader) + size);
    if (!b) return;
    if (o_GetPipelineCacheData(dev, cache, &size, b + sizeof(BlobHeader)) == VK_SUCCESS) {
        BlobHeader h{kMagic, 1, (uint32_t)size, checksum(b + sizeof(BlobHeader), size)};
        memcpy(b, &h, sizeof(h));
        if (write_atomic("vk_pipeline.bin", b, sizeof(h) + size)) st_vkSaved = size;
    }
    free(b);
}

static VkResult create_seeded(VkDevice dev, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    if (g_blob && ci->initialDataSize == 0) {
        VkPipelineCacheCreateInfo c = *ci;
        c.initialDataSize = g_blobSize;
        c.pInitialData = g_blob;
        if (o_CreatePipelineCache(dev, &c, a, out) == VK_SUCCESS) { st_vkSeeded++; return VK_SUCCESS; }
    }
    return o_CreatePipelineCache(dev, ci, a, out);
}

static VkPipelineCache master_locked(VkDevice dev) {
    VkPipelineCache r = VK_NULL_HANDLE;
    Implicit *freeSlot = nullptr;
    for (auto &i : g_impl) {
        if (i.dev == dev) { r = i.cache; break; }
        if (!freeSlot && !i.dev) freeSlot = &i;
    }
    if (!r && freeSlot) {
        VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        if (create_seeded(dev, &ci, nullptr, &r) == VK_SUCCESS) { *freeSlot = {dev, r}; st_vkImplicit++; }
        else r = VK_NULL_HANDLE;
    }
    return r;
}

static VkPipelineCache implicit_cache(VkDevice dev) {
    pthread_mutex_lock(&g_vkLock);
    VkPipelineCache r = master_locked(dev);
    pthread_mutex_unlock(&g_vkLock);
    return r;
}

// Merge every app-owned cache of `dev` into its master, then persist the master.
static void merge_and_save_locked(VkDevice dev) {
    VkPipelineCache master = master_locked(dev);
    if (!master) return;
    VkPipelineCache src[64];
    uint32_t n = 0;
    for (auto &c : g_appCaches) if (c.dev == dev && c.cache) src[n++] = c.cache;
    if (n && o_MergePipelineCaches && o_MergePipelineCaches(dev, master, n, src) == VK_SUCCESS) st_vkMerges++;
    save_cache(dev, master);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_CreatePipelineCache(VkDevice d, const VkPipelineCacheCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineCache *out) {
    VkResult r = create_seeded(d, ci, a, out);
    if (r != VK_SUCCESS) return r;
    // The app brought its own data (its PSO cache file): also inject everything learned in
    // earlier sessions. The handle is not visible to the app yet, so merging here is race-free.
    if (ci->initialDataSize && g_blob && o_MergePipelineCaches) {
        pthread_mutex_lock(&g_vkLock);
        VkPipelineCache master = master_locked(d);
        if (master && o_MergePipelineCaches(d, *out, 1, &master) == VK_SUCCESS) st_vkInjected++;
        pthread_mutex_unlock(&g_vkLock);
    }
    if (ci->flags & VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT) {
        pthread_mutex_lock(&g_vkLock);
        for (auto &c : g_extCaches) if (!c) { c = *out; break; }
        pthread_mutex_unlock(&g_vkLock);
    }
    // Remember it as a merge source, unless the app promised to synchronize it itself.
    if (!(ci->flags & VK_PIPELINE_CACHE_CREATE_EXTERNALLY_SYNCHRONIZED_BIT)) {
        pthread_mutex_lock(&g_vkLock);
        for (auto &c : g_appCaches) if (!c.cache) { c = {d, *out}; st_vkAppCaches++; break; }
        pthread_mutex_unlock(&g_vkLock);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL my_DestroyPipelineCache(VkDevice d, VkPipelineCache c, const VkAllocationCallbacks *a) {
    if (c) {
        pthread_mutex_lock(&g_vkLock);
        bool tracked = false;
        for (auto &x : g_appCaches) if (x.cache == c) { x = {}; tracked = true; }
        VkPipelineCache master = master_locked(d);
        if (master && tracked && o_MergePipelineCaches && o_MergePipelineCaches(d, master, 1, &c) == VK_SUCCESS) st_vkMerges++;
        if (master) save_cache(d, master);
        pthread_mutex_unlock(&g_vkLock);
    }
    o_DestroyPipelineCache(d, c, a);
}

// ---- parallel pipeline compilation -------------------------------------------------------------
// The Adreno driver compiles the pipelines of one vkCreate*Pipelines call one after another
// (measured: 48 pipelines 500 ms in one call vs 190 ms split over 4 threads). Batches are split
// into chunks that worker threads hand to the driver concurrently.
namespace psplit {
struct Job {
    bool graphics;
    VkDevice dev;
    VkPipelineCache cache;
    uint32_t n;
    const void *ci;
    const VkAllocationCallbacks *alloc;
    VkPipeline *out;
    VkResult result;
    bool done;
};
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER, g_doneCv = PTHREAD_COND_INITIALIZER;
static Job *g_queue[64];
static int g_qn, g_started;
static std::atomic<uint64_t> st_splitCalls{0}, st_splitPipes{0};

static VkResult run(Job &j) {
    return j.graphics ? o_CreateGraphicsPipelines(j.dev, j.cache, j.n, (const VkGraphicsPipelineCreateInfo *)j.ci, j.alloc, j.out)
                      : o_CreateComputePipelines(j.dev, j.cache, j.n, (const VkComputePipelineCreateInfo *)j.ci, j.alloc, j.out);
}

static void *worker(void *) {
    pthread_setname_np(pthread_self(), "LoadShiftPSO");
    for (;;) {
        pthread_mutex_lock(&g_lock);
        while (!g_qn) pthread_cond_wait(&g_cv, &g_lock);
        Job *j = g_queue[--g_qn];
        pthread_mutex_unlock(&g_lock);
        VkResult r = run(*j);
        pthread_mutex_lock(&g_lock);
        j->result = r;
        j->done = true;
        pthread_cond_broadcast(&g_doneCv);
        pthread_mutex_unlock(&g_lock);
    }
    return nullptr;
}

static bool cache_splittable(VkDevice d, VkPipelineCache c) {
    pthread_mutex_lock(&g_vkLock);
    bool ok = false;
    for (auto &i : g_impl) if (i.dev == d && i.cache == c) ok = true;  // our master
    for (auto &x : g_appCaches) if (x.cache == c) ok = true;            // seen created, internally synced
    for (auto &x : g_extCaches) if (x == c) ok = false;
    pthread_mutex_unlock(&g_vkLock);
    return ok;
}

template <class CI>
static bool splittable(VkDevice d, VkPipelineCache c, uint32_t n, const CI *ci) {
    if (cfg.pipeSplit < 2 || n < 2 || (c && !cache_splittable(d, c))) return false;
    for (uint32_t i = 0; i < n; i++) {
        if ((ci[i].flags & VK_PIPELINE_CREATE_DERIVATIVE_BIT) && ci[i].basePipelineIndex >= 0) return false;
        if (ci[i].flags & 0x200 /*EARLY_RETURN_ON_FAILURE*/) return false;
    }
    return true;
}

// Splits into up to cfg.pipeSplit chunks; the calling thread compiles the first chunk itself.
template <class CI>
static VkResult create(bool graphics, VkDevice d, VkPipelineCache c, uint32_t n, const CI *ci,
                       const VkAllocationCallbacks *a, VkPipeline *out) {
    uint32_t k = (uint32_t)cfg.pipeSplit < n ? (uint32_t)cfg.pipeSplit : n;
    Job jobs[8];
    uint32_t per = (n + k - 1) / k, at = 0, nj = 0;
    for (uint32_t i = 0; i < k && at < n; i++) {
        uint32_t cnt = n - at < per ? n - at : per;
        jobs[nj++] = {graphics, d, c, cnt, ci + at, a, out + at, VK_SUCCESS, false};
        at += cnt;
    }
    pthread_mutex_lock(&g_lock);
    while (g_started < cfg.pipeSplit - 1) {
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, worker, nullptr)) break;
        g_started++;
    }
    uint32_t queued = 1;
    if (g_started > 0)  // without workers everything runs on the calling thread below
        for (uint32_t i = 1; i < nj && g_qn < 64; i++, queued++) g_queue[g_qn++] = &jobs[i];
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_lock);
    for (uint32_t i = queued; i < nj; i++) { jobs[i].result = run(jobs[i]); jobs[i].done = true; }  // queue full
    jobs[0].result = run(jobs[0]);
    jobs[0].done = true;
    pthread_mutex_lock(&g_lock);
    for (;;) {
        bool all = true;
        for (uint32_t i = 1; i < nj; i++) if (!jobs[i].done) all = false;
        if (all) break;
        pthread_cond_wait(&g_doneCv, &g_lock);
    }
    pthread_mutex_unlock(&g_lock);
    // Errors (negative) win over positive non-success codes, which win over success.
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < nj; i++) {
        if (jobs[i].result < 0 && (r >= 0 || jobs[i].result < r)) r = jobs[i].result;
        else if (jobs[i].result > 0 && r == VK_SUCCESS) r = jobs[i].result;
    }
    st_splitCalls++;
    st_splitPipes += n;
    return r;
}
}  // namespace psplit

static VKAPI_ATTR VkResult VKAPI_CALL my_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n,
        const VkGraphicsPipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    if (!c) c = implicit_cache(d);
    uint64_t t0 = now_ns();
    VkResult r = psplit::splittable(d, c, n, ci) ? psplit::create(true, d, c, n, ci, a, out)
                                                 : o_CreateGraphicsPipelines(d, c, n, ci, a, out);
    st_vkPipeNs += now_ns() - t0;
    st_vkPipes += n;
    g_dirty = true;
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n,
        const VkComputePipelineCreateInfo *ci, const VkAllocationCallbacks *a, VkPipeline *out) {
    if (!c) c = implicit_cache(d);
    uint64_t t0 = now_ns();
    VkResult r = psplit::splittable(d, c, n, ci) ? psplit::create(false, d, c, n, ci, a, out)
                                                 : o_CreateComputePipelines(d, c, n, ci, a, out);
    st_vkPipeNs += now_ns() - t0;
    st_vkPipes += n;
    g_dirty = true;
    return r;
}

#include "vk_idle.inc"
#include "vk_mempool.inc"

static VKAPI_ATTR void VKAPI_CALL my_DestroyDevice(VkDevice d, const VkAllocationCallbacks *a) {
    if (cfg.vkIdle) vkidle::device_teardown(d);   // runs deferred frees (into the pool)
    if (mempool::g_cap) mempool::teardown(d);
    pthread_mutex_lock(&g_vkLock);
    if (cfg.vkCache) {
        bool any = false;
        for (auto &i : g_impl) if (i.dev == d) any = true;
        for (auto &c : g_appCaches) if (c.dev == d && c.cache) any = true;
        if (any) merge_and_save_locked(d);
    }
    for (auto &i : g_impl) {
        if (i.dev != d) continue;
        o_DestroyPipelineCache(d, i.cache, nullptr);
        i = {};
    }
    for (auto &c : g_appCaches) if (c.dev == d) c = {};
    pthread_mutex_unlock(&g_vkLock);
    o_DestroyDevice(d, a);
}

static void flush_implicit() {
    pthread_mutex_lock(&g_vkLock);
    VkDevice devs[8];
    int n = 0;
    auto add = [&](VkDevice d) {
        for (int k = 0; k < n; k++) if (devs[k] == d) return;
        if (n < 8) devs[n++] = d;
    };
    for (auto &i : g_impl) if (i.dev) add(i.dev);
    for (auto &c : g_appCaches) if (c.cache) add(c.dev);
    for (int k = 0; k < n; k++) merge_and_save_locked(devs[k]);
    pthread_mutex_unlock(&g_vkLock);
}

static int install_vulkan(char *log, size_t cap) {
    LoadedLib drv;
    if (!find_lib("vulkan.adreno.so", &drv)) {
        // Load the HAL through the platform loader (cheap query, no instance).
        void *vk = dlopen("libvulkan.so", RTLD_NOW);
        auto enumExt = vk ? (PFN_vkEnumerateInstanceExtensionProperties)dlsym(vk, "vkEnumerateInstanceExtensionProperties") : nullptr;
        uint32_t n = 0;
        if (enumExt) enumExt(nullptr, &n, nullptr);
    }
    if (!find_lib("vulkan.adreno.so", &drv)) { snprintf(log, cap, "vk:no-adreno "); return 0; }
    const char *names[] = {
        "_ZN11qglinternal21vkCreatePipelineCacheEP10VkDevice_TPK25VkPipelineCacheCreateInfoPK21VkAllocationCallbacksPP17VkPipelineCache_T",
        "_ZN11qglinternal22vkDestroyPipelineCacheEP10VkDevice_TP17VkPipelineCache_TPK21VkAllocationCallbacks",
        "_ZN11qglinternal22vkGetPipelineCacheDataEP10VkDevice_TP17VkPipelineCache_TPmPv",
        "_ZN11qglinternal25vkCreateGraphicsPipelinesEP10VkDevice_TP17VkPipelineCache_TjPK28VkGraphicsPipelineCreateInfoPK21VkAllocationCallbacksPP12VkPipeline_T",
        "_ZN11qglinternal24vkCreateComputePipelinesEP10VkDevice_TP17VkPipelineCache_TjPK27VkComputePipelineCreateInfoPK21VkAllocationCallbacksPP12VkPipeline_T",
        "_ZN11qglinternal15vkDestroyDeviceEP10VkDevice_TPK21VkAllocationCallbacks",
        "_ZN11qglinternal21vkMergePipelineCachesEP10VkDevice_TP17VkPipelineCache_TjPKS3_",
    };
    void *a[7] = {};
    if (resolve_syms(drv, names, a, 7) < 6 || !a[5]) { snprintf(log, cap, "vk:syms-missing "); return 0; }
    o_MergePipelineCaches = (PFN_vkMergePipelineCaches)a[6];  // optional: only the merge feature needs it
    o_DestroyDevice = (PFN_vkDestroyDevice)a[5];
    int idle = 0;
    if (cfg.vkIdle || cfg.occl) idle = vkidle::install(drv, cfg.vkIdleLevel, cfg.vkIdleDepth, cfg.vkIdle, cfg.occl);
    int mp = mempool::install(drv, (size_t)cfg.memPoolMB << 20, cfg.vkIdle && idle > 0);
    coexist::g_vkCopies = coexist::count_loaded("vulkan.adreno.so");
    int nd = vkH(drv, a[5], (void *)my_DestroyDevice, (void **)&o_DestroyDevice);
    if (!cfg.vkCache) { snprintf(log, cap, "vk:idle%d,pool%d,dd%d ", idle, mp, nd); return idle + mp + nd; }
    o_CreatePipelineCache = (PFN_vkCreatePipelineCache)a[0];
    o_DestroyPipelineCache = (PFN_vkDestroyPipelineCache)a[1];
    o_GetPipelineCacheData = (PFN_vkGetPipelineCacheData)a[2];
    o_CreateGraphicsPipelines = (PFN_vkCreateGraphicsPipelines)a[3];
    o_CreateComputePipelines = (PFN_vkCreateComputePipelines)a[4];
    load_blob();
    int n0 = vkH(drv, a[0], (void *)my_CreatePipelineCache, (void **)&o_CreatePipelineCache);
    int n1 = vkH(drv, a[1], (void *)my_DestroyPipelineCache, (void **)&o_DestroyPipelineCache);
    int n3 = vkH(drv, a[3], (void *)my_CreateGraphicsPipelines, (void **)&o_CreateGraphicsPipelines);
    int n4 = vkH(drv, a[4], (void *)my_CreateComputePipelines, (void **)&o_CreateComputePipelines);
    snprintf(log, cap, "vk:cache%d%d%d%d%s,idle%d,pool%d,dd%d ", n0, n1, n3, n4, o_MergePipelineCaches ? "+merge" : "", idle, mp, nd);
    return n0 + n1 + n3 + n4 + nd + (idle > 0 ? idle : 0);
}

// ---------------------------------------------------------------------------------------------

static void *stats_thread(void *) {
    pthread_setname_np(pthread_self(), "LoadShiftStats");
    int tick = 0;
    for (;;) {
        sleep(2);
        write_stats();
        if (cfg.vkCache && (++tick % 5 == 0) && g_dirty.exchange(false)) flush_implicit();
    }
    return nullptr;
}

static bool g_installed;
static std::atomic<bool> g_hooksReady{false};

static void do_install(bool mayLoadDriver) {
    char a[128] = "", b[128] = "";
    if (cfg.glFinish || cfg.pbo || cfg.glBuf || cfg.occl || cfg.readback || cfg.flushUs) install_gles(a, sizeof(a), mayLoadDriver);
    if (cfg.vkCache || cfg.vkIdle || cfg.occl || cfg.memPoolMB) install_vulkan(b, sizeof(b));
    snprintf(st_hooks, sizeof(st_hooks), "%s%s", a, b);
    LOG("%s: %s [%s]", g_pkg, st_hooks, coexist::g_mode);
    g_hooksReady = true;
    write_stats();
}

// Wait for another graphics hook module to finish patching (or a timeout), then install on top
// of it. Watches the dispatch slots LoadShift will use plus the entry code of the graphics
// libraries' key exports, so table swaps and inline hooks are both noticed.
static void *late_install_thread(void *) {
    pthread_setname_np(pthread_self(), "LoadShiftLate");
    uint64_t t0 = now_ns();
    LoadedLib gdrv, vdrv, lib;
    if (find_lib("libGLESv2_adreno.so", &gdrv) && coexist::gl_find_bases(gdrv)) {
        static const char *gl[] = {"glFinish", "glTexImage2D", "glTexSubImage2D", "glBufferSubData", "glMapBufferRange",
                                   "glUnmapBuffer", "glReadPixels", "glFlush", "glGetQueryObjectuiv", "glBeginQuery"};
        const int NG = sizeof(gl) / sizeof(gl[0]);
        void *ga[NG] = {};
        resolve_syms(gdrv, gl, ga, NG);
        for (int k = 0; k < NG; k++) {
            long off = coexist::gl_offset(gl[k]);
            for (int i = 0; off >= 0 && ga[k] && i < coexist::g_nGlBases; i++) coexist::watch_slot(coexist::g_glBases[i] + off, ga[k]);
        }
    }
    if (!find_lib("vulkan.adreno.so", &vdrv)) {
        void *vk = dlopen("libvulkan.so", RTLD_NOW);
        auto enumExt = vk ? (PFN_vkEnumerateInstanceExtensionProperties)dlsym(vk, "vkEnumerateInstanceExtensionProperties") : nullptr;
        uint32_t n = 0;
        if (enumExt) enumExt(nullptr, &n, nullptr);
    }
    if (find_lib("vulkan.adreno.so", &vdrv)) {
        static const char *vs[] = {
            "_ZN11qglinternal13vkQueueSubmitEP9VkQueue_TjPK12VkSubmitInfoP9VkFence_T",
            "_ZN11qglinternal15vkQueueWaitIdleEP9VkQueue_T",
            "_ZN11qglinternal16vkDeviceWaitIdleEP10VkDevice_T",
            "_ZN11qglinternal16vkGetDeviceQueueEP10VkDevice_TjjPP9VkQueue_T",
            "_ZN11qglinternal19vkGetDeviceProcAddrEP10VkDevice_TPKc",
            "_ZN11qglinternal21vkGetInstanceProcAddrEP12VkInstance_TPKc",
            "_ZN11qglinternal25vkCreateGraphicsPipelinesEP10VkDevice_TP17VkPipelineCache_TjPK28VkGraphicsPipelineCreateInfoPK21VkAllocationCallbacksPP12VkPipeline_T",
            "_ZN11qglinternal21vkCreatePipelineCacheEP10VkDevice_TPK25VkPipelineCacheCreateInfoPK21VkAllocationCallbacksPP17VkPipelineCache_T",
        };
        const int N = sizeof(vs) / sizeof(vs[0]);
        void *fa[N] = {};
        resolve_syms(vdrv, vs, fa, N);
        for (int i = 0; i < N; i++) {
            if (!fa[i]) continue;
            coexist::watch_add((uintptr_t)fa[i], 16);  // inline hook on the driver function
            uintptr_t sl[8];
            int n = coexist::vk_slot_addrs(vdrv, fa[i], sl, 8);
            for (int k = 0; k < n; k++) coexist::watch_slot(sl[k], fa[i]);  // table swap
        }
    }
    struct Ex { const char *lib; const char *fns[8]; };
    static const Ex exports[] = {
        {"libvulkan.so", {"vkGetDeviceProcAddr", "vkGetInstanceProcAddr", "vkCreateDevice", "vkCreateInstance",
                          "vkQueuePresentKHR", "vkQueueSubmit", "vkCreateSwapchainKHR", "vkAcquireNextImageKHR"}},
        {"libEGL.so", {"eglSwapBuffers", "eglSwapBuffersWithDamageKHR", "eglGetProcAddress", "eglMakeCurrent",
                       "eglCreateWindowSurface", "eglCreateContext", nullptr, nullptr}},
    };
    for (auto &e : exports) {
        if (!find_lib(e.lib, &lib)) continue;
        void *fa[8] = {};
        int cnt = 0;
        while (cnt < 8 && e.fns[cnt]) cnt++;
        resolve_syms(lib, e.fns, fa, cnt);
        for (int i = 0; i < cnt; i++) coexist::watch_add((uintptr_t)fa[i], 16);
    }
    const char *why = "timeout";
    if (coexist::g_preModified) why = "other-hook-already-there";  // it went first: install now
    else while (now_ns() - t0 < (uint64_t)cfg.compatTimeoutMs * 1000000ull) {
        if (coexist::watch_changed()) { why = "after-other-hook"; usleep(300000); break; }
        usleep(50000);
    }
    snprintf(coexist::g_mode, sizeof(coexist::g_mode), "late:%s:%llums", why, (unsigned long long)((now_ns() - t0) / 1000000));
    do_install(false);
    return nullptr;
}

static void install(const char *dataDir) {
    if (g_installed || !cfg.enabled) return;
    g_installed = true;
    snprintf(g_dir, sizeof(g_dir), "%s/cache/loadshift", dataDir);
    char parent[560];
    snprintf(parent, sizeof(parent), "%s/cache", dataDir);
    mkdir(parent, 0700);
    mkdir(g_dir, 0700);
    bool late = cfg.compatInstall == 2 || (cfg.compatInstall == 1 && cfg.foreignFg);
    coexist::g_compat = cfg.foreignFg || late;
    coexist::g_exempt = cfg.exemptForeign;
    if (coexist::g_compat) coexist::init_callers();
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (late) {
        pthread_create(&t, &at, late_install_thread, nullptr);
    } else {
        snprintf(coexist::g_mode, sizeof(coexist::g_mode), "early");
        do_install(true);
    }
    pthread_create(&t, &at, stats_thread, nullptr);
}

static int extra_stats(char *b, size_t cap) {
    return snprintf(b, cap,
        "vk_mem_pool_hits=%llu\nvk_mem_pool_misses=%llu\nvk_mem_pooled=%llu\nvk_mem_trimmed=%llu\nvk_mem_saved_mb=%.1f\nvk_mem_pool_now_mb=%.1f\nvk_split_calls=%llu\nvk_split_pipelines=%llu\ncompat=%s\nforeign_module=%d\nforeign_slots=%llu\nyielded_slots=%llu\nforeign_calls=%llu\ndriver_copies=vk%d/gl%d\nvk_app_caches=%llu\nvk_cache_injected=%llu\nvk_cache_merges=%llu\ngl_tex_compressed=%llu\ngl_buf_sub=%llu\ngl_buf_sub_mb=%.1f\ngl_buf_map=%llu\ngl_buf_map_mb=%.1f\ngl_buf_fallback=%llu\n"
        "gl_readback_async=%llu\ngl_readback_exact=%llu\ngl_flush_dropped=%llu\ngl_flush_real=%llu\noccl_gl_answered=%llu\noccl_vk_answered=%llu\nvk_idle_skipped=%llu\nvk_idle_native_ms=%.1f\nvk_lazy_waits=%llu\nvk_lazy_wait_ms=%.1f\nvk_deferred_destroys=%llu\n",
        (unsigned long long)mempool::st_hits.load(), (unsigned long long)mempool::st_misses.load(),
        (unsigned long long)mempool::st_pooled.load(), (unsigned long long)mempool::st_trimmed.load(),
        mempool::st_savedBytes.load() / 1048576.0, mempool::g_bytes / 1048576.0,
        (unsigned long long)psplit::st_splitCalls.load(), (unsigned long long)psplit::st_splitPipes.load(),
        coexist::g_mode, cfg.foreignFg ? 1 : 0, (unsigned long long)coexist::st_foreignSlots.load(),
        (unsigned long long)coexist::st_yielded.load(), (unsigned long long)coexist::st_foreignCalls.load(), coexist::g_vkCopies, coexist::g_glCopies,
        (unsigned long long)st_vkAppCaches.load(), (unsigned long long)st_vkInjected.load(), (unsigned long long)st_vkMerges.load(),
        (unsigned long long)texup::st_compressed.load(), (unsigned long long)glbuf::st_sub.load(), glbuf::st_subBytes.load() / 1048576.0,
        (unsigned long long)glbuf::st_map.load(), glbuf::st_mapBytes.load() / 1048576.0,
        (unsigned long long)glbuf::st_fallback.load(),
        (unsigned long long)rback::st_async.load(), (unsigned long long)rback::st_exact.load(),
        (unsigned long long)gflush::st_dropped.load(), (unsigned long long)gflush::st_real.load(),
        (unsigned long long)occl::st_glAnswered.load(), (unsigned long long)vkidle::st_vkOcclAnswered.load(),
        (unsigned long long)vkidle::st_idleSkipped.load(), vkidle::st_idleWaitNs.load() / 1e6,
        (unsigned long long)vkidle::st_lazyWaits.load(), vkidle::st_lazyNs.load() / 1e6,
        (unsigned long long)vkidle::st_deferred.load());
}

// Lab entry: used by the on-device test binary (no Zygisk).
extern "C" __attribute__((visibility("default"))) const char *loadshift_lab_install(const char *conf, const char *dataDir, const char *pkg) {
    parse_config(conf);
    snprintf(g_pkg, sizeof(g_pkg), "%s", pkg);
    install(dataDir);
    return st_hooks;
}
extern "C" __attribute__((visibility("default"))) void loadshift_lab_flush() { flush_implicit(); write_stats(); }
extern "C" __attribute__((visibility("default"))) const char *loadshift_lab_wait(int ms) {
    for (int i = 0; i < ms / 10 && !g_hooksReady.load(); i++) usleep(10000);
    return st_hooks;
}
extern "C" __attribute__((visibility("default"))) const char *loadshift_lab_mode() { return coexist::g_mode; }

// ---------------------------------------------------------------------------------------------
// Zygisk

static bool xfer(int fd, void *data, size_t len, bool send) {
    char *p = (char *)data;
    while (len) {
        ssize_t n = send ? write(fd, p, len) : read(fd, p, len);
        if (n <= 0) return false;
        p += n; len -= n;
    }
    return true;
}

static bool valid_pkg(const char *p) {
    if (!*p) return false;
    for (; *p; p++)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_' || *p == '.')) return false;
    return true;
}

// Runs as root in the Zygisk companion daemon.
static void companion(int client) {
    char pkg[256] = {};
    uint32_t len = 0;
    if (!xfer(client, pkg, sizeof(pkg), false)) return;
    pkg[255] = 0;
    char buf[4096];
    if (valid_pkg(pkg)) {
        char path[400];
        snprintf(path, sizeof(path), "/data/adb/loadshift/profiles/%s.conf", pkg);
        int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd >= 0) {
            ssize_t n = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (n > 0) len = (uint32_t)n;
        }
    }
    // Tell the app process whether another in-process graphics hook module is active, so it
    // can install after it and leave that module's own calls untouched.
    if (len) {
        static const char *known[] = {"lybfghook", "portable_gamefx"};
        bool active = false;
        for (auto *m : known) {
            char d[160], x[200];
            snprintf(d, sizeof(d), "/data/adb/modules/%s", m);
            struct stat st;
            if (stat(d, &st) || !S_ISDIR(st.st_mode)) continue;
            snprintf(x, sizeof(x), "%s/disable", d);
            if (!access(x, F_OK)) continue;
            snprintf(x, sizeof(x), "%s/remove", d);
            if (!access(x, F_OK)) continue;
            active = true;
        }
        if (active && len + 16 < sizeof(buf)) len += snprintf(buf + len, sizeof(buf) - len, "\nforeign_fg=1\n");
    }
    xfer(client, &len, sizeof(len), true);
    if (len) xfer(client, buf, len, true);
}

class LoadShiftModule : public zygisk::ModuleBase {
    zygisk::Api *api = nullptr;
    JNIEnv *env = nullptr;
    bool selected = false;
    char dataDir[400] = {};

public:
    void onLoad(zygisk::Api *a, JNIEnv *e) override { api = a; env = e; }

    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        int appId = args->uid % 100000;
        const char *name = args->nice_name ? env->GetStringUTFChars(args->nice_name, nullptr) : nullptr;
        char pkg[256] = {};
        if (name) { snprintf(pkg, sizeof(pkg), "%s", name); env->ReleaseStringUTFChars(args->nice_name, name); }
        if (args->app_data_dir) {
            const char *d = env->GetStringUTFChars(args->app_data_dir, nullptr);
            if (d) { snprintf(dataDir, sizeof(dataDir), "%s", d); env->ReleaseStringUTFChars(args->app_data_dir, d); }
        }
        if (appId < 10000 || appId >= 90000 || !pkg[0] || strchr(pkg, ':') || !dataDir[0]) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
            return;
        }
        int s = api->connectCompanion();
        uint32_t len = 0;
        char conf[4096] = {};
        if (s >= 0) {
            if (xfer(s, pkg, sizeof(pkg), true) && xfer(s, &len, sizeof(len), false) && len && len < sizeof(conf))
                xfer(s, conf, len, false);
            else len = 0;
            close(s);
        }
        if (len) parse_config(conf);
        selected = len && cfg.enabled && (cfg.glFinish || cfg.pbo || cfg.vkCache || cfg.vkIdle || cfg.glBuf || cfg.occl || cfg.readback || cfg.flushUs || cfg.memPoolMB);
        if (selected) snprintf(g_pkg, sizeof(g_pkg), "%s", pkg);
        else api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (selected) install(dataDir);
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
};

REGISTER_ZYGISK_MODULE(LoadShiftModule)
REGISTER_ZYGISK_COMPANION(companion)
