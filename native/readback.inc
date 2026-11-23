// Asynchronous glReadPixels (aggressive, opt-in; included from loadshift.cpp).
//
// A glReadPixels into client memory forces the CPU to wait until the GPU has rendered
// everything up to that point. Engines often do this every frame for the same small region
// (auto-exposure, GPU picking, analytics). For such repeated reads the GPU now writes into a
// pixel-pack buffer asynchronously and the call returns the result of the PREVIOUS identical
// read (one frame of latency) without stalling. The first read of a region, or any one-off read,
// is still exact.

namespace rback {

typedef void (*PFN_ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
static PFN_ReadPixels o_ReadPixels;

struct Slot {
    EGLContext ctx;
    GLint fbo, readBuf, x, y;
    GLsizei w, h;
    GLenum format, type;
    GLint packRow, packAlign, packSkipRows, packSkipPix;
    GLuint pbo[2];
    GLsync fence[2];
    int next;      // PBO the next async read goes into
    bool primed;   // the other PBO holds a finished/queued read
    uint32_t lastUse;
};
static Slot g_slots[16];
static uint32_t g_clock;
static std::atomic<uint64_t> st_async{0}, st_exact{0};
static thread_local bool t_inside;

static Slot *lookup(EGLContext c, GLint fbo, GLint rb, GLint x, GLint y, GLsizei w, GLsizei h, GLenum f, GLenum t,
                    GLint row, GLint al, GLint sr, GLint sp, bool *fresh) {
    Slot *lru = &g_slots[0];
    for (auto &s : g_slots) {
        if (s.ctx == c && s.fbo == fbo && s.readBuf == rb && s.x == x && s.y == y && s.w == w && s.h == h && s.format == f &&
            s.type == t && s.packRow == row && s.packAlign == al && s.packSkipRows == sr && s.packSkipPix == sp) {
            *fresh = false;
            s.lastUse = ++g_clock;
            return &s;
        }
        if (s.lastUse < lru->lastUse) lru = &s;
    }
    // Evict LRU (its GL objects belong to its own context; only delete if that is current).
    if (lru->ctx == c) {
        for (int i = 0; i < 2; i++) {
            if (lru->fence[i]) glDeleteSync(lru->fence[i]);
            if (lru->pbo[i]) glDeleteBuffers(1, &lru->pbo[i]);
        }
    }
    *lru = Slot{c, fbo, rb, x, y, w, h, f, t, row, al, sr, sp, {0, 0}, {nullptr, nullptr}, 0, false, ++g_clock};
    *fresh = true;
    return lru;
}

static void my_ReadPixels(GLint x, GLint y, GLsizei w, GLsizei h, GLenum format, GLenum type, void *out) {
    if (LS_FOREIGN()) { o_ReadPixels(x, y, w, h, format, type, out); return; }
    GlCtx *g = (!t_inside && out && w > 0 && h > 0) ? cur_ctx() : nullptr;
    int bpp = g && g->es3 == 1 ? bytes_per_pixel(format, type) : 0;
    GLint packBuf = 0;
    if (bpp) glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuf);
    if (!bpp || packBuf || (int64_t)w * h * bpp > (16 << 20)) { o_ReadPixels(x, y, w, h, format, type, out); return; }

    GLint fbo = 0, rb = 0, row = 0, al = 4, sr = 0, sp = 0;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &fbo);
    glGetIntegerv(GL_READ_BUFFER, &rb);
    glGetIntegerv(GL_PACK_ROW_LENGTH, &row);
    glGetIntegerv(GL_PACK_ALIGNMENT, &al);
    glGetIntegerv(GL_PACK_SKIP_ROWS, &sr);
    glGetIntegerv(GL_PACK_SKIP_PIXELS, &sp);
    bool fresh;
    Slot *s = lookup(g->ctx, fbo, rb, x, y, w, h, format, type, row, al, sr, sp, &fresh);

    int64_t rowBytes = (int64_t)w * bpp;
    int64_t stride = (int64_t)(row > 0 ? row : w) * bpp;
    if (al > 1) stride = (stride + al - 1) / al * al;
    int64_t span = (int64_t)(sr + h - 1) * stride + (int64_t)(sp + w) * bpp;

    // Queue an async read of the current frame into s->pbo[next].
    int k = s->next;
    if (!s->pbo[k]) glGenBuffers(1, &s->pbo[k]);
    if (s->fence[k]) { glDeleteSync(s->fence[k]); s->fence[k] = nullptr; }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo[k]);
    glBufferData(GL_PIXEL_PACK_BUFFER, span, nullptr, GL_STREAM_READ);
    o_ReadPixels(x, y, w, h, format, type, (void *)0);
    s->fence[k] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();

    int prev = 1 - k;
    if (fresh || !s->primed || !s->fence[prev]) {
        // First time for this region: answer exactly (this read stalls once).
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        o_ReadPixels(x, y, w, h, format, type, out);
        st_exact++;
    } else {
        // Previous identical read: normally finished long ago, so this does not wait.
        glClientWaitSync(s->fence[prev], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s->pbo[prev]);
        t_inside = true;
        const uint8_t *src = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, span, GL_MAP_READ_BIT);
        t_inside = false;
        if (src) {
            // Copy row by row: only the pixel bytes, never the row padding the app may own.
            for (GLsizei r = 0; r < h; r++) {
                int64_t o = (int64_t)(sr + r) * stride + (int64_t)sp * bpp;
                memcpy((uint8_t *)out + o, src + o, (size_t)rowBytes);
            }
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            st_async++;
        } else {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            o_ReadPixels(x, y, w, h, format, type, out);
            st_exact++;
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    s->primed = true;
    s->next = prev;
}

}  // namespace rback
