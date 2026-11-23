// Coexistence with other in-process graphics hooks (frame generators etc.).
// Included from loadshift.cpp after the ELF helpers.
//
// 1. Slot discovery that does not depend on the slot's current value:
//    - Vulkan: the Adreno driver's name→function table lives in .data.rel.ro; the on-disk file
//      holds each slot's unrelocated value (RELR addend = function vaddr) or a RELATIVE rela
//      entry, so slot locations are computed from the FILE, not from memory.
//    - GLES: every libGLESv2 export is a tiny trampoline `ldr x16,[x16,#off]; br x16` into the
//      current gl_hooks_t. Decoding it yields each function's offset in libEGL's dispatch table;
//      the table base is found by voting over a few reference functions.
//    So LoadShift finds its slots even when another module already replaced them.
// 2. Chaining: if a slot already points to someone else's hook, that pointer becomes LoadShift's
//    "original" — calls flow LoadShift → other hook → driver, and both keep working.
// 3. Install timing: when another graphics hook module is active, LoadShift waits until that
//    module has modified the graphics libraries (or a timeout) and then installs on top of it.
// 4. Foreign callers: calls coming from code that is neither the app nor the system (another
//    module's code in memfd / /data/adb / anonymous memory) are passed straight through, so a
//    frame generator's own glFinish/glReadPixels/WaitIdle keep their exact semantics.

namespace coexist {

static std::atomic<uint64_t> st_foreignSlots{0}, st_foreignCalls{0}, st_yielded{0};
static int g_vkCopies, g_glCopies;
static char g_mode[48] = "early";
static bool g_exempt = true;      // pass foreign callers through
static bool g_compat = false;     // another graphics hook module is active for this app

// ---- Vulkan driver: slots from the file ------------------------------------------------------

struct FileView {
    uint8_t *m = nullptr;
    size_t size = 0;
    struct Seg { uint64_t vaddr, off, filesz; } segs[8];
    int nsegs = 0;
    const ElfW(Rela) *rela = nullptr;
    size_t nrela = 0;
};

static bool open_view(const char *path, FileView *v) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st;
    if (fstat(fd, &st)) { close(fd); return false; }
    v->m = (uint8_t *)mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (v->m == MAP_FAILED) { v->m = nullptr; return false; }
    v->size = st.st_size;
    auto *eh = (ElfW(Ehdr) *)v->m;
    if (v->size < sizeof(*eh) || memcmp(eh->e_ident, ELFMAG, SELFMAG)) return false;
    auto *ph = (ElfW(Phdr) *)(v->m + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum && v->nsegs < 8; i++)
        if (ph[i].p_type == PT_LOAD && (ph[i].p_flags & PF_W))
            v->segs[v->nsegs++] = {ph[i].p_vaddr, ph[i].p_offset, ph[i].p_filesz};
    if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(ElfW(Shdr)) <= v->size) {
        auto *sh = (ElfW(Shdr) *)(v->m + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++)
            if (sh[i].sh_type == SHT_RELA && sh[i].sh_offset + sh[i].sh_size <= v->size && !(sh[i].sh_flags & SHF_INFO_LINK)) {
                v->rela = (const ElfW(Rela) *)(v->m + sh[i].sh_offset);
                v->nrela = sh[i].sh_size / sizeof(ElfW(Rela));
            }
    }
    return true;
}

static void close_view(FileView *v) { if (v->m) munmap(v->m, v->size); v->m = nullptr; }

// vaddrs of slots that are relocated to hold `target` (a function vaddr).
static int file_slots(const FileView &v, uint64_t target, uint64_t *out, int max) {
    int n = 0;
    for (int s = 0; s < v.nsegs && n < max; s++) {
        const auto &g = v.segs[s];
        if (g.off + g.filesz > v.size) continue;
        for (uint64_t o = 0; o + 8 <= g.filesz && n < max; o += 8)
            if (*(const uint64_t *)(v.m + g.off + o) == target) out[n++] = g.vaddr + o;
    }
    for (size_t i = 0; i < v.nrela && n < max; i++)
        if (ELF64_R_TYPE(v.rela[i].r_info) == 1027 /*R_AARCH64_RELATIVE*/ && (uint64_t)v.rela[i].r_addend == target) {
            bool dup = false;
            for (int k = 0; k < n; k++) if (out[k] == v.rela[i].r_offset) dup = true;
            if (!dup) out[n++] = v.rela[i].r_offset;
        }
    return n;
}

static bool in_relro(const LoadedLib &L, uintptr_t a) {
    for (int s = 0; s < L.nsegs; s++) if (a >= L.segs[s].start && a < L.segs[s].end) return L.segs[s].relro;
    return false;
}

// Write `mine` into each slot; *origOut receives what the first slot held (driver or another
// module's hook) BEFORE the slot is published, so concurrent callers never see a null original.
// `consuming` hooks may complete a call without calling the next function (lazy WaitIdle, async
// glFinish, …). If another module already owns such a slot, LoadShift yields it (returns -1) so
// that module keeps seeing every call.
static int write_slots(const LoadedLib *L, uintptr_t *slots, int n, void *drv, void *mine, void **origOut, bool consuming) {
    if (!n) return 0;
    void *cur = __atomic_load_n((void **)slots[0], __ATOMIC_ACQUIRE);
    if (cur == mine) return n;  // already ours
    if (cur != drv) {
        st_foreignSlots++;
        if (consuming) { st_yielded++; return -1; }
    }
    __atomic_store_n(origOut, cur, __ATOMIC_RELEASE);
    long pg = sysconf(_SC_PAGESIZE);
    int done = 0;
    for (int i = 0; i < n; i++) {
        void *v = __atomic_load_n((void **)slots[i], __ATOMIC_ACQUIRE);
        if (v != cur && v != drv) continue;  // a different hook owns this alias: leave it
        bool ro = L && in_relro(*L, slots[i]);
        uintptr_t page = slots[i] & ~(uintptr_t)(pg - 1);
        if (ro && mprotect((void *)page, pg, PROT_READ | PROT_WRITE)) continue;
        __atomic_store_n((void **)slots[i], mine, __ATOMIC_SEQ_CST);
        if (ro) mprotect((void *)page, pg, PROT_READ);
        done++;
    }
    return done;
}

static FileView g_vkView;
static bool g_vkViewOpen;

static int vk_slot_addrs(const LoadedLib &drv, void *fn, uintptr_t *out, int max) {
    if (!g_vkViewOpen) g_vkViewOpen = open_view(drv.path, &g_vkView);
    if (!g_vkViewOpen) return 0;
    uint64_t v[8];
    int n = file_slots(g_vkView, (uint64_t)((uintptr_t)fn - drv.bias), v, max < 8 ? max : 8);
    for (int i = 0; i < n; i++) out[i] = drv.bias + v[i];
    return n;
}

/** Hook a Vulkan driver function: slots written (0 = not found, -1 = yielded); *orig = next. */
static int vk_hook(const LoadedLib &drv, void *fn, void *mine, void **orig, bool consuming) {
    uintptr_t s[8];
    int n = vk_slot_addrs(drv, fn, s, 8);
    return write_slots(&drv, s, n, fn, mine, orig, consuming);
}

// ---- GLES dispatch table: offsets from libGLESv2 trampolines ---------------------------------

static LoadedLib g_glesWrap, g_egl;
static uintptr_t g_glBases[4];
static int g_nGlBases;

// Offset of `name` in gl_hooks_t, decoded from libGLESv2's export (or -1).
static long gl_offset(const char *name) {
    void *fn = nullptr;
    const char *names[1] = {name};
    if (resolve_syms(g_glesWrap, names, &fn, 1) != 1) return -1;
    const uint32_t *ins = (const uint32_t *)fn;
    for (int i = 1; i < 10; i++) {
        if (ins[i] != 0xd61f0200u) continue;                 // br x16
        uint32_t ld = ins[i - 1];
        if ((ld & 0xFFC003FFu) != 0xF9400210u) return -1;    // ldr x16, [x16, #imm]
        return (long)((ld >> 10) & 0xFFF) * 8;
    }
    return -1;
}

// Find libEGL's dispatch table base(s) by voting over reference functions whose slots still
// hold the driver pointer.
static bool gl_find_bases(const LoadedLib &drv) {
    if (!find_lib("libGLESv2.so", &g_glesWrap) || !find_lib("libEGL.so", &g_egl)) return false;
    static const char *ref[] = {"glActiveTexture", "glAttachShader", "glBindAttribLocation", "glBlendColor",
                                "glClearDepthf", "glCompileShader", "glCullFace", "glDepthRangef",
                                "glGetProgramiv", "glLineWidth", "glPolygonOffset", "glSampleCoverage"};
    const int N = sizeof(ref) / sizeof(ref[0]);
    void *addr[N] = {};
    resolve_syms(drv, ref, addr, N);
    uintptr_t cand[64];
    int votes[64] = {}, nc = 0;
    for (int r = 0; r < N; r++) {
        long off = addr[r] ? gl_offset(ref[r]) : -1;
        if (off < 0) continue;
        for (int s = 0; s < g_egl.nsegs; s++)
            for (uintptr_t a = (g_egl.segs[s].start + 7) & ~7ul; a + 8 <= g_egl.segs[s].end; a += 8) {
                if (*(void *volatile *)a != addr[r]) continue;
                uintptr_t base = a - off;
                int k = 0;
                while (k < nc && cand[k] != base) k++;
                if (k == nc && nc < 64) cand[nc++] = base;
                if (k < 64) votes[k]++;
            }
    }
    int best = 0;
    for (int k = 0; k < nc; k++) if (votes[k] > best) best = votes[k];
    g_nGlBases = 0;
    for (int k = 0; k < nc && g_nGlBases < 4; k++)
        if (votes[k] >= 3 && votes[k] * 2 >= best) g_glBases[g_nGlBases++] = cand[k];
    return g_nGlBases > 0;
}

/** Hook a GLES function by name in every dispatch table (0 = not found, -1 = yielded). */
static int gl_hook(const char *name, void *drv, void *mine, void **orig, bool consuming) {
    long off = gl_offset(name);
    if (off < 0 || !g_nGlBases) return 0;
    uintptr_t s[4];
    for (int i = 0; i < g_nGlBases; i++) s[i] = g_glBases[i] + off;
    return write_slots(nullptr, s, g_nGlBases, drv, mine, orig, consuming);  // libEGL table is in .bss (RW)
}

// ---- driver copies (diagnostics) -------------------------------------------------------------

struct CountCtx { const char *suffix; int n; };
static int count_cb(dl_phdr_info *info, size_t, void *data) {
    auto *c = (CountCtx *)data;
    if (info->dlpi_name) {
        size_t ln = strlen(info->dlpi_name), ls = strlen(c->suffix);
        if (ln >= ls && !strcmp(info->dlpi_name + ln - ls, c->suffix)) c->n++;
    }
    return 0;
}
static int count_loaded(const char *suffix) {
    CountCtx c{suffix, 0};
    dl_iterate_phdr(count_cb, &c);
    return c.n;
}

// ---- change watch (for late install) ---------------------------------------------------------

struct Watch { uintptr_t addr; uint64_t v0, v1; };
static Watch g_watch[192];
static int g_nwatch;

static void watch_add(uintptr_t a, int bytes) {
    if (!a || g_nwatch >= 192) return;
    Watch &w = g_watch[g_nwatch++];
    w.addr = a;
    w.v0 = *(volatile uint64_t *)a;
    w.v1 = bytes > 8 ? *(volatile uint64_t *)(a + 8) : 0;
}

static int g_preModified;  // slots already holding something other than the driver function

static void watch_slot(uintptr_t slot, void *driverFn) {
    if (!slot) return;
    if (*(void *volatile *)slot != driverFn) g_preModified++;
    watch_add(slot, 8);
}

static bool watch_changed() {
    for (int i = 0; i < g_nwatch; i++) {
        const Watch &w = g_watch[i];
        if (*(volatile uint64_t *)w.addr != w.v0) return true;
        if (w.v1 && *(volatile uint64_t *)(w.addr + 8) != w.v1) return true;
    }
    return false;
}

// ---- caller classification -------------------------------------------------------------------

struct Range { uintptr_t s, e; bool trusted; };
// Two buffers: rescans fill the inactive one and then publish it, so readers never see a
// half-written table. Ranges are sorted for binary search.
static Range g_rbuf[2][1024];
static std::atomic<int> g_ractive{0}, g_rcount{0};
static std::atomic<uint64_t> g_rgen{0};
static pthread_mutex_t g_rangeLock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_lastScan;
static char g_exe[256];

static bool trusted_path(const char *p) {
    static const char *pre[] = {"/data/app/", "/system/", "/vendor/", "/apex/", "/product/", "/system_ext/", "/odm/"};
    for (auto *x : pre) if (!strncmp(p, x, strlen(x))) return true;
    if (g_exe[0] && !strcmp(p, g_exe)) return true;
    if (strstr(p, "/cache/loadshift") || strstr(p, "libloadshift")) return true;
    return false;
}

static void rescan_maps() {  // caller holds g_rangeLock
    FILE *f = fopen("/proc/self/maps", "re");
    if (!f) return;
    int next = 1 - g_ractive.load();
    Range *r = g_rbuf[next];
    char line[512];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < 1024) {
        unsigned long s, e;
        char perm[8] = {};
        int path = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &s, &e, perm, &path) < 3 || perm[2] != 'x') continue;
        char *p = line + path;
        p[strcspn(p, "\n")] = 0;
        r[n++] = {s, e, trusted_path(p)};
    }
    fclose(f);
    // /proc/self/maps is already sorted by address.
    g_rcount = 0;            // readers fall back to "unknown" during the switch
    g_ractive = next;
    g_rcount = n;
    g_rgen++;
}

// Also treat our own library as trusted (it may be mapped from a memfd by the Zygisk loader).
static uintptr_t g_selfS, g_selfE;
static uintptr_t g_lastUnknown;
struct TlsHit { uintptr_t s, e; uint64_t gen; bool trusted; };
static thread_local TlsHit t_hit;

static int lookup(uintptr_t a) {  // 1 trusted, 0 foreign, -1 unknown
    if (t_hit.gen == g_rgen.load() && a >= t_hit.s && a < t_hit.e) return t_hit.trusted ? 1 : 0;
    const Range *r = g_rbuf[g_ractive.load()];
    int lo = 0, hi = g_rcount.load() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (a < r[mid].s) hi = mid - 1;
        else if (a >= r[mid].e) lo = mid + 1;
        else {
            t_hit = {r[mid].s, r[mid].e, g_rgen.load(), r[mid].trusted};
            return r[mid].trusted ? 1 : 0;
        }
    }
    return -1;
}

static bool foreign_caller(void *ra) {
    uintptr_t a = (uintptr_t)ra;
    if (!g_exempt || !g_compat) return false;
    if (a >= g_selfS && a < g_selfE) return false;
    for (int pass = 0; pass < 2; pass++) {
        int k = lookup(a);
        if (k >= 0) {
            if (!k) st_foreignCalls++;
            return !k;
        }
        // Code we have not seen yet (newly loaded library or newly mapped code): re-read the
        // maps right away, but not repeatedly for the same unknown address.
        uint64_t now = now_ns();
        if (pass || (a == g_lastUnknown && now - g_lastScan < 200000000ull)) break;
        pthread_mutex_lock(&g_rangeLock);
        g_lastScan = now;
        g_lastUnknown = a;
        rescan_maps();
        pthread_mutex_unlock(&g_rangeLock);
    }
    return false;  // still unknown: treat as the app's own
}

static void init_callers() {
    ssize_t n = readlink("/proc/self/exe", g_exe, sizeof(g_exe) - 1);
    g_exe[n > 0 ? n : 0] = 0;
    Dl_info di;
    if (dladdr((void *)&init_callers, &di) && di.dli_fbase) {
        // Our mapping: from the load base up to the end of the executable segment we live in.
        FILE *f = fopen("/proc/self/maps", "re");
        char line[512];
        while (f && fgets(line, sizeof(line), f)) {
            unsigned long s, e;
            if (sscanf(line, "%lx-%lx", &s, &e) == 2 && (uintptr_t)&init_callers >= s && (uintptr_t)&init_callers < e) {
                g_selfS = s; g_selfE = e;
                break;
            }
        }
        if (f) fclose(f);
    }
    pthread_mutex_lock(&g_rangeLock);
    rescan_maps();
    pthread_mutex_unlock(&g_rangeLock);
    g_lastScan = now_ns();
}

}  // namespace coexist

#define LS_FOREIGN() (coexist::g_compat && coexist::foreign_caller(__builtin_return_address(0)))
