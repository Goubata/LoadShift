// Occlusion queries without CPU stalls (included from loadshift.cpp).
//
// Reading GL_QUERY_RESULT of an occlusion query that the GPU has not finished blocks the CPU
// until the GPU catches up. When the result is not available yet we answer "visible" (1) right
// away. That is always visually safe (an object is drawn rather than wrongly culled); the cost
// is at most one extra draw for a frame.

namespace occl {

typedef void (*PFN_BeginQuery)(GLenum, GLuint);
typedef void (*PFN_GetQueryObjectuiv)(GLuint, GLenum, GLuint *);
static PFN_BeginQuery o_BeginQuery;
static PFN_GetQueryObjectuiv o_GetQueryObjectuiv;

struct Rec { GLuint id; GLenum target; uint32_t epoch; };
static Rec g_q[8192];
static std::atomic<uint64_t> st_glAnswered{0}, st_glNative{0};

static void remember(GLuint id, GLenum target) {
    uint32_t e = g_finishEpoch.load();
    size_t h = (id * 2654435761u) % 8192;
    for (int p = 0; p < 64; p++) {
        Rec &r = g_q[(h + p) % 8192];
        if (r.id == id || !r.id) { r.id = id; r.target = target; r.epoch = e; return; }
    }
    g_q[h] = {id, target, e};  // probe chain full: overwrite
}

static const Rec *find(GLuint id) {
    size_t h = (id * 2654435761u) % 8192;
    for (int p = 0; p < 64; p++) {
        const Rec &r = g_q[(h + p) % 8192];
        if (r.id == id) return &r;
        if (!r.id) return nullptr;
    }
    return nullptr;
}

static void my_BeginQuery(GLenum target, GLuint id) {
    if (LS_FOREIGN()) { o_BeginQuery(target, id); return; }
    if (id) remember(id, target);
    o_BeginQuery(target, id);
}

static void my_GetQueryObjectuiv(GLuint id, GLenum pname, GLuint *out) {
    if (LS_FOREIGN()) { o_GetQueryObjectuiv(id, pname, out); return; }
    if (pname == GL_QUERY_RESULT && out) {
        const Rec *q = find(id);
        GLenum t = q ? q->target : 0;
        // If the app called glFinish after this query began, it is entitled to the real result.
        if (q && q->epoch != g_finishEpoch.load()) t = 0;
        if (t == GL_ANY_SAMPLES_PASSED || t == GL_ANY_SAMPLES_PASSED_CONSERVATIVE) {
            GLuint avail = 0;
            o_GetQueryObjectuiv(id, GL_QUERY_RESULT_AVAILABLE, &avail);
            if (!avail) { *out = 1; st_glAnswered++; return; }
            st_glNative++;
        }
    }
    o_GetQueryObjectuiv(id, pname, out);
}

}  // namespace occl
