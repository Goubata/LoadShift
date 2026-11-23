// Texture uploads → GPU (included from loadshift.cpp after gl_buffer.inc).
//
// Client-memory glTex*Image calls block the game thread while the driver converts/copies the
// pixels. Here the bytes go into a persistently-mapped staging ring (pre-faulted, never
// re-allocated; big copies are split across worker cores) and the upload is issued from that
// pixel-unpack buffer, so the GPU performs format conversion, tiling and the copy itself.
//
// Mode (gl_pbo_upload): 1 = smart (formats the driver converts on the CPU + compressed),
//                       2 = everything (incl. RGBA8, which the driver already handles well).

namespace texup {

// ---- parallel memcpy ------------------------------------------------------------------------
struct Job { uint8_t *dst; const uint8_t *src; size_t len; };
static const int kMaxWorkers = 7;
static Job g_jobs[kMaxWorkers + 1];
static std::atomic<int> g_pending{0};
static std::atomic<uint32_t> g_gen{0};
static pthread_mutex_t g_poolLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_poolCond = PTHREAD_COND_INITIALIZER;
static bool g_poolStarted;
static int kWorkers = 3;               // helper threads (copy_threads - 1)
static size_t g_copyThreshold = 2u << 20;

static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    pthread_setname_np(pthread_self(), "LoadShiftCopy");
    uint32_t seen = 0;
    for (;;) {
        pthread_mutex_lock(&g_poolLock);
        while (g_gen.load() == seen) pthread_cond_wait(&g_poolCond, &g_poolLock);
        seen = g_gen.load();
        Job j = g_jobs[id + 1];
        pthread_mutex_unlock(&g_poolLock);
        if (j.len) memcpy(j.dst, j.src, j.len);
        g_pending--;
    }
    return nullptr;
}

static void start_pool() {
    if (g_poolStarted) return;
    g_poolStarted = true;
    int started = 0;
    for (int i = 0; i < kWorkers; i++) {
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, worker, (void *)(intptr_t)i)) break;
        started++;
    }
    kWorkers = started;  // never wait for helpers that do not exist
}

static pthread_mutex_t g_copyLock = PTHREAD_MUTEX_INITIALIZER;  // one parallel copy at a time

static void fast_copy(void *dst, const void *src, size_t len) {
    if (kWorkers <= 0 || len < g_copyThreshold || pthread_mutex_trylock(&g_copyLock)) { memcpy(dst, src, len); return; }
    start_pool();
    if (kWorkers <= 0) { pthread_mutex_unlock(&g_copyLock); memcpy(dst, src, len); return; }
    int parts = kWorkers + 1;
    size_t part = (len / parts + 4095) & ~(size_t)4095;
    pthread_mutex_lock(&g_poolLock);
    for (int i = 0; i < parts; i++) {
        size_t off = part * i;
        size_t l = off >= len ? 0 : (len - off < part ? len - off : part);
        g_jobs[i] = {(uint8_t *)dst + off, (const uint8_t *)src + off, l};
    }
    g_pending = kWorkers;
    g_gen++;
    pthread_cond_broadcast(&g_poolCond);
    pthread_mutex_unlock(&g_poolLock);
    if (g_jobs[0].len) memcpy(g_jobs[0].dst, g_jobs[0].src, g_jobs[0].len);
    while (g_pending.load() > 0) {}  // workers finish within ~1 ms
    pthread_mutex_unlock(&g_copyLock);
}

// ---- staging ring with fenced regions -------------------------------------------------------
static GLsizeiptr kSize = 48 << 20;  // gl_tex_ring_mb

struct Region { GLsync fence; GLsizeiptr start, end; };
struct Ring {
    bool tried, ok;
    GLuint buf;
    uint8_t *ptr;
    GLsizeiptr head;
    Region r[64];
    int rh, rc;
};
static Ring g_rings[16];
static thread_local bool t_inside;

static std::atomic<uint64_t> st_compressed{0};

static bool init(Ring *r) {
    if (r->tried) return r->ok;
    r->tried = true;
    if (!glbuf::p_BufferStorageEXT) return false;
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (!ext || !strstr(ext, "GL_EXT_buffer_storage")) return false;
    GLint prev = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &prev);
    glGenBuffers(1, &r->buf);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, r->buf);
    glbuf::p_BufferStorageEXT(GL_PIXEL_UNPACK_BUFFER, kSize, nullptr, GL_MAP_WRITE_BIT | LS_MAP_PERSISTENT | LS_MAP_COHERENT);
    t_inside = glbuf::t_inside = true;
    r->ptr = (uint8_t *)glMapBufferRange(GL_PIXEL_UNPACK_BUFFER, 0, kSize, GL_MAP_WRITE_BIT | LS_MAP_PERSISTENT | LS_MAP_COHERENT);
    t_inside = glbuf::t_inside = false;
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, prev);
    while (glGetError() != GL_NO_ERROR) {}
    if (r->ptr) for (GLsizeiptr o = 0; o < kSize; o += 4096) r->ptr[o] = 0;  // pre-fault once
    r->ok = r->ptr != nullptr;
    return r->ok;
}

static bool overlaps(const Region &g, GLsizeiptr s, GLsizeiptr e) { return g.start < e && s < g.end; }

// Reserve [start, start+len); waits only if the GPU still reads that part of the ring.
static GLsizeiptr alloc(Ring *r, GLsizeiptr len) {
    if (len > kSize / 2) return -1;
    GLsizeiptr s = (r->head + 255) & ~(GLsizeiptr)255;
    if (s + len > kSize) s = 0;
    GLsizeiptr e = s + len;
    while (r->rc && overlaps(r->r[r->rh], s, e)) {
        Region &g = r->r[r->rh];
        glClientWaitSync(g.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glDeleteSync(g.fence);
        r->rh = (r->rh + 1) % 64;
        r->rc--;
    }
    // Also drop regions that are already done (cheap poll) to keep the queue short.
    while (r->rc) {
        Region &g = r->r[r->rh];
        if (glClientWaitSync(g.fence, 0, 0) == GL_TIMEOUT_EXPIRED) break;
        glDeleteSync(g.fence);
        r->rh = (r->rh + 1) % 64;
        r->rc--;
    }
    r->head = e;
    return s;
}

static void retire_after_use(Ring *r, GLsizeiptr s, GLsizeiptr len) {
    if (r->rc == 64) {  // queue full: wait the oldest
        Region &g = r->r[r->rh];
        glClientWaitSync(g.fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glDeleteSync(g.fence);
        r->rh = (r->rh + 1) % 64;
        r->rc--;
    }
    r->r[(r->rh + r->rc) % 64] = {glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0), s, s + len};
    r->rc++;
}

static int g_mode = 1;
static int g_minKB = 64;

static bool smart_ok(GLenum format, GLenum type) {
    if (g_mode >= 2) return true;
    // Measured on Adreno 829: RGBA8 is already fast in the driver; everything that needs a
    // CPU-side conversion is 3-6x faster through the GPU path.
    return !(format == GL_RGBA && type == GL_UNSIGNED_BYTE);
}

// Byte span of an uncompressed image under the current unpack state.
static int64_t span(GLsizei w, GLsizei h, GLsizei depth, GLenum format, GLenum type) {
    int bpp = bytes_per_pixel(format, type);
    if (!bpp) return -1;
    GLint rowLen = 0, skipRows = 0, skipPix = 0, align = 4, imgH = 0, skipImg = 0;
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &rowLen);
    glGetIntegerv(GL_UNPACK_SKIP_ROWS, &skipRows);
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &skipPix);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    if (depth > 1) {
        glGetIntegerv(GL_UNPACK_IMAGE_HEIGHT, &imgH);
        glGetIntegerv(GL_UNPACK_SKIP_IMAGES, &skipImg);
    }
    int64_t row = (int64_t)(rowLen > 0 ? rowLen : w) * bpp;
    if (align > 1) row = (row + align - 1) / align * align;
    int64_t image = row * (imgH > 0 ? imgH : h);
    return (int64_t)(skipImg + depth - 1) * image + (int64_t)(skipRows + h - 1) * row + (int64_t)(skipPix + w) * bpp;
}

// Stage `len` bytes and bind the ring as PIXEL_UNPACK; returns the offset or -1 (fallback).
static GLsizeiptr stage(const void *pixels, int64_t len, Ring **outRing) {
    if (t_inside || !pixels || len < (int64_t)g_minKB * 1024) return -1;
    GlCtx *g = cur_ctx();
    if (!g || g->es3 != 1) return -1;
    Ring *r = &g_rings[g - g_ctx];
    if (!init(r)) return -1;
    GLint bound = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &bound);
    if (bound) { st_pboSkip++; return -1; }  // app already uploads from its own PBO
    GLsizeiptr at = alloc(r, len);
    if (at < 0) { st_pboSkip++; return -1; }
    fast_copy(r->ptr + at, pixels, (size_t)len);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, r->buf);
    *outRing = r;
    st_pbo++;
    st_pboBytes += len;
    return at;
}

static void done(Ring *r, GLsizeiptr at, int64_t len) {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    retire_after_use(r, at, len);
}

typedef void (*PFN_TexImage3D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
typedef void (*PFN_TexSubImage3D)(GLenum, GLint, GLint, GLint, GLint, GLsizei, GLsizei, GLsizei, GLenum, GLenum, const void *);
typedef void (*PFN_CTexImage2D)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void *);
typedef void (*PFN_CTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void *);
static PFN_TexImage3D o_TexImage3D;
static PFN_TexSubImage3D o_TexSubImage3D;
static PFN_CTexImage2D o_CTexImage2D;
static PFN_CTexSubImage2D o_CTexSubImage2D;

#define OFS(x) ((const void *)(uintptr_t)(x))

static void my_TexImage2D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLint b, GLenum f, GLenum ty, const void *px) {
    if (LS_FOREIGN()) { o_glTexImage2D(t, l, ifmt, w, h, b, f, ty, px); return; }
    Ring *r;
    int64_t len = px && smart_ok(f, ty) ? span(w, h, 1, f, ty) : -1;
    GLsizeiptr at = len > 0 ? stage(px, len, &r) : -1;
    if (at < 0) { o_glTexImage2D(t, l, ifmt, w, h, b, f, ty, px); return; }
    o_glTexImage2D(t, l, ifmt, w, h, b, f, ty, OFS(at));
    done(r, at, len);
}

static void my_TexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum ty, const void *px) {
    if (LS_FOREIGN()) { o_glTexSubImage2D(t, l, x, y, w, h, f, ty, px); return; }
    Ring *r;
    int64_t len = px && smart_ok(f, ty) ? span(w, h, 1, f, ty) : -1;
    GLsizeiptr at = len > 0 ? stage(px, len, &r) : -1;
    if (at < 0) { o_glTexSubImage2D(t, l, x, y, w, h, f, ty, px); return; }
    o_glTexSubImage2D(t, l, x, y, w, h, f, ty, OFS(at));
    done(r, at, len);
}

static void my_TexImage3D(GLenum t, GLint l, GLint ifmt, GLsizei w, GLsizei h, GLsizei d, GLint b, GLenum f, GLenum ty, const void *px) {
    if (LS_FOREIGN()) { o_TexImage3D(t, l, ifmt, w, h, d, b, f, ty, px); return; }
    Ring *r;
    int64_t len = px && smart_ok(f, ty) ? span(w, h, d, f, ty) : -1;
    GLsizeiptr at = len > 0 ? stage(px, len, &r) : -1;
    if (at < 0) { o_TexImage3D(t, l, ifmt, w, h, d, b, f, ty, px); return; }
    o_TexImage3D(t, l, ifmt, w, h, d, b, f, ty, OFS(at));
    done(r, at, len);
}

static void my_TexSubImage3D(GLenum t, GLint l, GLint x, GLint y, GLint z, GLsizei w, GLsizei h, GLsizei d, GLenum f, GLenum ty, const void *px) {
    if (LS_FOREIGN()) { o_TexSubImage3D(t, l, x, y, z, w, h, d, f, ty, px); return; }
    Ring *r;
    int64_t len = px && smart_ok(f, ty) ? span(w, h, d, f, ty) : -1;
    GLsizeiptr at = len > 0 ? stage(px, len, &r) : -1;
    if (at < 0) { o_TexSubImage3D(t, l, x, y, z, w, h, d, f, ty, px); return; }
    o_TexSubImage3D(t, l, x, y, z, w, h, d, f, ty, OFS(at));
    done(r, at, len);
}

static void my_CTexImage2D(GLenum t, GLint l, GLenum ifmt, GLsizei w, GLsizei h, GLint b, GLsizei size, const void *data) {
    if (LS_FOREIGN()) { o_CTexImage2D(t, l, ifmt, w, h, b, size, data); return; }
    Ring *r;
    GLsizeiptr at = size > 0 ? stage(data, size, &r) : -1;
    if (at < 0) { o_CTexImage2D(t, l, ifmt, w, h, b, size, data); return; }
    o_CTexImage2D(t, l, ifmt, w, h, b, size, OFS(at));
    done(r, at, size);
    st_compressed++;
}

static void my_CTexSubImage2D(GLenum t, GLint l, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLsizei size, const void *data) {
    if (LS_FOREIGN()) { o_CTexSubImage2D(t, l, x, y, w, h, f, size, data); return; }
    Ring *r;
    GLsizeiptr at = size > 0 ? stage(data, size, &r) : -1;
    if (at < 0) { o_CTexSubImage2D(t, l, x, y, w, h, f, size, data); return; }
    o_CTexSubImage2D(t, l, x, y, w, h, f, size, OFS(at));
    done(r, at, size);
    st_compressed++;
}

#undef OFS
}  // namespace texup
