// GL buffer updates → GPU copies (included from loadshift.cpp, after GlCtx/cur_ctx).
//
// glBufferSubData / write-only glMapBufferRange on a buffer the GPU is still reading forces the
// driver to either stall the CPU or shadow-copy the whole buffer on the CPU. Instead the data is
// written into a persistently-mapped staging ring (CPU memcpy only, never waits) and the GPU's
// copy engine moves it into place with glCopyBufferSubData, ordered with the app's commands.

namespace glbuf {

typedef void (*PFN_BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void *);
typedef void *(*PFN_MapBufferRange)(GLenum, GLintptr, GLsizeiptr, GLbitfield);
typedef void (*PFN_FlushMappedBufferRange)(GLenum, GLintptr, GLsizeiptr);
typedef GLboolean (*PFN_UnmapBuffer)(GLenum);
typedef void (*PFN_BufferStorageEXT)(GLenum, GLsizeiptr, const void *, GLbitfield);

static PFN_BufferSubData o_BufferSubData;
static PFN_MapBufferRange o_MapBufferRange;
static PFN_FlushMappedBufferRange o_FlushMappedBufferRange;
static PFN_UnmapBuffer o_UnmapBuffer;
static PFN_BufferStorageEXT p_BufferStorageEXT;

static int g_min = 1024;  // bytes; smaller updates are inlined in the command stream anyway

static const int kChunks = 8;
static GLsizeiptr kChunk = 4 << 20;  // ring = 8 chunks per context (gl_buf_ring_mb / 8)

#define LS_MAP_PERSISTENT 0x0040
#define LS_MAP_COHERENT 0x0080

struct PendingMap {
    GLenum target;
    GLuint buffer;
    GLintptr dstOffset;
    GLsizeiptr length;
    GLsizeiptr stagingOffset;
    bool explicitFlush;
    int nr;
    GLintptr rOff[16];
    GLsizeiptr rLen[16];
    bool overflow;
};

struct Ring {
    bool tried, ok;
    GLuint buf;
    uint8_t *ptr;
    int chunk;
    GLsizeiptr off;  // within chunk
    GLsync fence[kChunks];
    PendingMap maps[4];
    int nmaps;
};

static Ring g_rings[16];  // parallel to g_ctx[]
static thread_local bool t_inside;

static std::atomic<uint64_t> st_sub{0}, st_subBytes{0}, st_map{0}, st_mapBytes{0}, st_fallback{0};

static Ring *ring_for(GlCtx *g) { return &g_rings[g - g_ctx]; }

static GLenum binding_enum(GLenum target) {
    switch (target) {
        case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
        case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
        case GL_UNIFORM_BUFFER: return GL_UNIFORM_BUFFER_BINDING;
        case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER_BINDING;
        case GL_PIXEL_UNPACK_BUFFER: return GL_PIXEL_UNPACK_BUFFER_BINDING;
        case GL_TRANSFORM_FEEDBACK_BUFFER: return GL_TRANSFORM_FEEDBACK_BUFFER_BINDING;
        case 0x90D2 /*SHADER_STORAGE*/: return 0x90D3;
        case 0x8F3F /*DRAW_INDIRECT*/: return 0x8F43;
        case 0x90EE /*DISPATCH_INDIRECT*/: return 0x90EF;
        case 0x92C0 /*ATOMIC_COUNTER*/: return 0x92C1;
        case 0x8C2A /*TEXTURE_BUFFER*/: return 0x8C2A;
        default: return 0;  // COPY_READ / PIXEL_PACK etc.: leave alone
    }
}

static bool ring_init(GlCtx *g, Ring *r) {
    if (r->tried) return r->ok;
    r->tried = true;
    const char *ext = (const char *)glGetString(GL_EXTENSIONS);
    if (!p_BufferStorageEXT || !ext || !strstr(ext, "GL_EXT_buffer_storage")) return false;
    GLint prev = 0;
    glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &prev);
    glGenBuffers(1, &r->buf);
    glBindBuffer(GL_COPY_READ_BUFFER, r->buf);
    p_BufferStorageEXT(GL_COPY_READ_BUFFER, kChunk * kChunks, nullptr, GL_MAP_WRITE_BIT | LS_MAP_PERSISTENT | LS_MAP_COHERENT);
    t_inside = true;
    r->ptr = (uint8_t *)glMapBufferRange(GL_COPY_READ_BUFFER, 0, kChunk * kChunks, GL_MAP_WRITE_BIT | LS_MAP_PERSISTENT | LS_MAP_COHERENT);
    t_inside = false;
    glBindBuffer(GL_COPY_READ_BUFFER, prev);
    while (glGetError() != GL_NO_ERROR) {}
    r->ok = r->ptr != nullptr;
    return r->ok;
}

// Reserve `len` bytes of staging; returns absolute ring offset or -1.
static GLsizeiptr ring_alloc(Ring *r, GLsizeiptr len) {
    if (len > kChunk) return -1;
    GLsizeiptr aligned = (r->off + 255) & ~(GLsizeiptr)255;
    if (aligned + len > kChunk) {
        // Close current chunk with a fence, move to the next one (wait if the GPU still uses it).
        if (r->fence[r->chunk]) glDeleteSync(r->fence[r->chunk]);
        r->fence[r->chunk] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        r->chunk = (r->chunk + 1) % kChunks;
        if (r->fence[r->chunk]) {
            glClientWaitSync(r->fence[r->chunk], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
            glDeleteSync(r->fence[r->chunk]);
            r->fence[r->chunk] = nullptr;
        }
        aligned = 0;
    }
    r->off = aligned + len;
    return (GLsizeiptr)r->chunk * kChunk + aligned;
}

static void gpu_copy(Ring *r, GLenum target, GLsizeiptr src, GLintptr dst, GLsizeiptr len) {
    GLint prev = 0;
    glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &prev);
    glBindBuffer(GL_COPY_READ_BUFFER, r->buf);
    glCopyBufferSubData(GL_COPY_READ_BUFFER, target, src, dst, len);
    glBindBuffer(GL_COPY_READ_BUFFER, prev);
}

static Ring *usable(GLenum target, GLsizeiptr size) {
    if (t_inside || size < g_min || !binding_enum(target)) return nullptr;
    GlCtx *g = cur_ctx();
    if (!g || g->es3 != 1) return nullptr;
    Ring *r = ring_for(g);
    return ring_init(g, r) ? r : nullptr;
}

static void my_BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data) {
    Ring *r = data ? usable(target, size) : nullptr;
    GLsizeiptr at = r ? ring_alloc(r, size) : -1;
    if (at < 0) { if (r) st_fallback++; o_BufferSubData(target, offset, size, data); return; }
    memcpy(r->ptr + at, data, size);
    gpu_copy(r, target, at, offset, size);
    st_sub++;
    st_subBytes += size;
}

static void *my_MapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access) {
    if (!t_inside && (access & LS_MAP_PERSISTENT) && (access & GL_MAP_READ_BIT)) g_finishUnsafe = true;
    if (!cfg.glBuf) return o_MapBufferRange(target, offset, length, access);
    bool candidate = (access & GL_MAP_WRITE_BIT) && !(access & (GL_MAP_READ_BIT | GL_MAP_UNSYNCHRONIZED_BIT | LS_MAP_PERSISTENT)) &&
                     (access & (GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT | GL_MAP_FLUSH_EXPLICIT_BIT));
    Ring *r = candidate ? usable(target, length) : nullptr;
    if (r && r->nmaps < 4) {
        for (int i = 0; i < r->nmaps; i++) if (r->maps[i].target == target) r = nullptr;  // already mapped: let GL report it
    } else r = nullptr;
    GLsizeiptr at = r ? ring_alloc(r, length) : -1;
    if (at < 0) return o_MapBufferRange(target, offset, length, access);
    GLint name = 0;
    glGetIntegerv(binding_enum(target), &name);
    if (!name) return o_MapBufferRange(target, offset, length, access);
    PendingMap &m = r->maps[r->nmaps++];
    m = PendingMap();
    m.target = target;
    m.buffer = name;
    m.dstOffset = offset;
    m.length = length;
    m.stagingOffset = at;
    m.explicitFlush = access & GL_MAP_FLUSH_EXPLICIT_BIT;
    st_map++;
    return r->ptr + at;
}

static PendingMap *find_map(Ring **outRing, GLenum target) {
    GlCtx *g = cur_ctx();
    if (!g) return nullptr;
    Ring *r = ring_for(g);
    if (!r->nmaps) return nullptr;
    GLenum be = binding_enum(target);
    GLint name = 0;
    if (be) glGetIntegerv(be, &name);
    for (int i = 0; i < r->nmaps; i++)
        if (r->maps[i].target == target && r->maps[i].buffer == (GLuint)name) { *outRing = r; return &r->maps[i]; }
    return nullptr;
}

static void my_FlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length) {
    Ring *r;
    PendingMap *m = find_map(&r, target);
    if (!m) { o_FlushMappedBufferRange(target, offset, length); return; }
    if (m->nr < 16) { m->rOff[m->nr] = offset; m->rLen[m->nr++] = length; }
    else m->overflow = true;
}

static GLboolean my_UnmapBuffer(GLenum target) {
    Ring *r;
    PendingMap *m = find_map(&r, target);
    if (!m) return o_UnmapBuffer(target);
    if (!m->explicitFlush || m->overflow) {
        gpu_copy(r, target, m->stagingOffset, m->dstOffset, m->length);
        st_mapBytes += m->length;
    } else {
        for (int i = 0; i < m->nr; i++) {
            GLsizeiptr len = m->rLen[i];
            if (m->rOff[i] < 0 || m->rOff[i] + len > m->length) continue;
            gpu_copy(r, target, m->stagingOffset + m->rOff[i], m->dstOffset + m->rOff[i], len);
            st_mapBytes += len;
        }
    }
    *m = r->maps[--r->nmaps];
    return GL_TRUE;
}

}  // namespace glbuf
