// Lazy WaitIdle for Vulkan (included from loadshift.cpp).
//
// vkQueueWaitIdle / vkDeviceWaitIdle normally stall the calling (game) thread until the GPU has
// drained. Here they submit an empty batch with a private fence ("marker") and return at once,
// so game logic runs while the GPU is still busy. The wait is postponed to the first moment
// the app does something that really needs the GPU to be finished:
//   * re-recording / resetting a command buffer still in flight  -> waits only for ITS marker
//   * resetting/freeing command pools, descriptor sets, fences    -> waits for all markers
//   * mapping / invalidating memory, reading query results        -> waits for all markers
// Object destruction does not wait at all: destroy calls are queued and executed by a reaper
// thread once the GPU has passed the marker.
//
// Not covered (documented risk): reading persistently-mapped HOST_COHERENT memory right after
// WaitIdle without any API call in between.

namespace vkidle {

typedef void(VKAPI_PTR *PFN_Generic)(VkDevice, uint64_t, const VkAllocationCallbacks *);

static PFN_vkQueueWaitIdle o_QueueWaitIdle;
static PFN_vkDeviceWaitIdle o_DeviceWaitIdle;
static PFN_vkGetDeviceQueue o_GetDeviceQueue;
static PFN_vkGetDeviceQueue2 o_GetDeviceQueue2;
static PFN_vkQueueSubmit o_QueueSubmit;
static PFN_vkQueueSubmit2 o_QueueSubmit2;
static PFN_vkBeginCommandBuffer o_BeginCommandBuffer;
static PFN_vkResetCommandBuffer o_ResetCommandBuffer;
static PFN_vkResetCommandPool o_ResetCommandPool;
static PFN_vkFreeCommandBuffers o_FreeCommandBuffers;
static PFN_vkMapMemory o_MapMemory;
static PFN_vkInvalidateMappedMemoryRanges o_InvalidateMappedMemoryRanges;
static PFN_vkGetQueryPoolResults o_GetQueryPoolResults;
static PFN_vkResetFences o_ResetFences;
static PFN_vkUpdateDescriptorSets o_UpdateDescriptorSets;
static PFN_vkResetDescriptorPool o_ResetDescriptorPool;
static PFN_vkFreeDescriptorSets o_FreeDescriptorSets;
static PFN_vkCreateFence o_CreateFence;
static PFN_vkWaitForFences o_WaitForFences;
static PFN_vkDestroyFence o_DestroyFenceRaw;  // for our own fences

static int g_level = 1;   // 1 = safe (wait on descriptor updates), 2 = aggressive
static int g_depth = 3;   // max outstanding markers before WaitIdle really waits

struct Marker {
    VkFence fence;
    uint64_t serial;
    int waiters;
    bool signaled;
};

struct Deferred {
    PFN_Generic fn;
    uint64_t handle;
    uint64_t serial;
};

struct Dev {
    VkDevice dev;
    pthread_mutex_t lock;
    Marker m[64];
    int head, count;
    uint64_t nextSerial;   // serial the next marker will get
    uint64_t doneSerial;   // all markers <= this have completed
    Deferred q[4096];
    int qh, qc;
    VkQueue queues[16];
    int nq;
};
static Dev g_devs[4];
static pthread_mutex_t g_devsLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_wake = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_wakeLock = PTHREAD_MUTEX_INITIALIZER;

// queue -> device
struct QMap { VkQueue q; Dev *d; };
static QMap g_qmap[32];

// command buffer -> serial of the next marker at the time of its last submit
struct CbRec { VkCommandBuffer cb; uint64_t serial; };
static CbRec g_cbs[8192];
static pthread_mutex_t g_cbLock = PTHREAD_MUTEX_INITIALIZER;

static bool g_occlNoWait;
static PFN_vkCreateQueryPool o_CreateQueryPool;
struct PoolRec { VkQueryPool pool; VkQueryType type; };
static PoolRec g_pools[1024];
static pthread_mutex_t g_poolLock = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<uint64_t> st_vkOcclAnswered{0};

static std::atomic<uint64_t> st_idleSkipped{0}, st_idleWaitNs{0}, st_deferred{0}, st_lazyWaits{0}, st_lazyNs{0};

static Dev *find_dev(VkDevice d, bool create) {
    pthread_mutex_lock(&g_devsLock);
    Dev *r = nullptr, *free = nullptr;
    for (auto &x : g_devs) {
        if (x.dev == d) { r = &x; break; }
        if (!free && !x.dev) free = &x;
    }
    if (!r && create && free) {
        memset((void *)free, 0, sizeof(Dev));
        pthread_mutex_init(&free->lock, nullptr);
        free->dev = d;
        free->nextSerial = 1;
        r = free;
    }
    pthread_mutex_unlock(&g_devsLock);
    return r;
}

static Dev *queue_dev(VkQueue q) {
    for (auto &e : g_qmap) if (e.q == q) return e.d;
    return nullptr;
}

static void remember_queue(VkDevice d, VkQueue q) {
    if (!q) return;
    Dev *x = find_dev(d, true);
    if (!x) return;
    pthread_mutex_lock(&g_devsLock);
    bool known = false;
    for (auto &e : g_qmap) if (e.q == q) { e.d = x; known = true; break; }
    if (!known) for (auto &e : g_qmap) if (!e.q) { e = {q, x}; break; }
    pthread_mutex_unlock(&g_devsLock);
    pthread_mutex_lock(&x->lock);
    bool have = false;
    for (int i = 0; i < x->nq; i++) if (x->queues[i] == q) have = true;
    if (!have && x->nq < 16) x->queues[x->nq++] = q;
    pthread_mutex_unlock(&x->lock);
}

// Pops completed markers (in order) and collects deferred destroys that are now safe.
// Caller holds x->lock. Returns number of items moved to out.
static int retire_locked(Dev *x, Deferred *out, int cap) {
    while (x->count) {
        Marker &m = x->m[x->head];
        if (!m.signaled || m.waiters) break;
        x->doneSerial = m.serial;
        o_DestroyFenceRaw(x->dev, m.fence, nullptr);
        x->head = (x->head + 1) % 64;
        x->count--;
    }
    // A signaled marker blocked behind waiters still counts as done for serial purposes.
    for (int i = 0; i < x->count; i++) {
        Marker &m = x->m[(x->head + i) % 64];
        if (!m.signaled) break;
        if (m.serial > x->doneSerial) x->doneSerial = m.serial;
    }
    int n = 0;
    while (x->qc && n < cap && x->q[x->qh].serial <= x->doneSerial) {
        out[n++] = x->q[x->qh];
        x->qh = (x->qh + 1) % 4096;
        x->qc--;
    }
    return n;
}

static void run_deferred(Dev *x, Deferred *items, int n) {
    for (int i = 0; i < n; i++) items[i].fn(x->dev, items[i].handle, nullptr);
}

// Wait until every marker with serial <= upTo has completed (upTo = ~0 for all).
static void wait_upto(Dev *x, uint64_t upTo) {
    VkFence fences[64];
    int idx[64];
    int n = 0;
    pthread_mutex_lock(&x->lock);
    for (int i = 0; i < x->count; i++) {
        int k = (x->head + i) % 64;
        Marker &m = x->m[k];
        if (m.serial > upTo) break;
        if (m.signaled) continue;
        m.waiters++;
        fences[n] = m.fence;
        idx[n++] = k;
    }
    pthread_mutex_unlock(&x->lock);
    if (!n) return;
    uint64_t t0 = now_ns();
    o_WaitForFences(x->dev, n, fences, VK_TRUE, UINT64_MAX);
    st_lazyNs += now_ns() - t0;
    st_lazyWaits++;
    Deferred items[256];
    pthread_mutex_lock(&x->lock);
    for (int i = 0; i < n; i++) { x->m[idx[i]].waiters--; x->m[idx[i]].signaled = true; }
    int r = retire_locked(x, items, 256);
    pthread_mutex_unlock(&x->lock);
    run_deferred(x, items, r);
}

static bool has_pending(Dev *x) { return x && __atomic_load_n(&x->count, __ATOMIC_ACQUIRE) > 0; }

static void wait_all(Dev *x) { if (has_pending(x)) wait_upto(x, ~0ull); }

static void wait_all_devices() {
    for (auto &x : g_devs) if (x.dev && has_pending(&x)) wait_upto(&x, ~0ull);
}

static bool add_marker(Dev *x, VkQueue q) {
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence f;
    if (o_CreateFence(x->dev, &fi, nullptr, &f) != VK_SUCCESS) return false;
    if (o_QueueSubmit(q, 0, nullptr, f) != VK_SUCCESS) { o_DestroyFenceRaw(x->dev, f, nullptr); return false; }
    pthread_mutex_lock(&x->lock);
    if (x->count == 64) {  // ring full: should not happen thanks to depth, but stay safe
        pthread_mutex_unlock(&x->lock);
        o_WaitForFences(x->dev, 1, &f, VK_TRUE, UINT64_MAX);
        wait_all(x);
        o_DestroyFenceRaw(x->dev, f, nullptr);
        return true;
    }
    x->m[(x->head + x->count) % 64] = {f, x->nextSerial++, 0, false};
    __atomic_store_n(&x->count, x->count + 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&x->lock);
    pthread_cond_signal(&g_wake);
    return true;
}

static void enforce_depth(Dev *x) {
    // Keep at most g_depth unfinished markers so the CPU cannot run away from the GPU.
    pthread_mutex_lock(&x->lock);
    int unfinished = 0;
    uint64_t cut = 0;
    for (int i = x->count - 1; i >= 0; i--) {
        Marker &m = x->m[(x->head + i) % 64];
        if (m.signaled) continue;
        if (++unfinished > g_depth && !cut) cut = m.serial;
    }
    pthread_mutex_unlock(&x->lock);
    if (cut) wait_upto(x, cut);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_QueueWaitIdle(VkQueue q) {
    if (LS_FOREIGN()) return o_QueueWaitIdle(q);  // another module's own wait: keep it exact
    Dev *x = queue_dev(q);
    if (!x || !add_marker(x, q)) {
        uint64_t t0 = now_ns();
        VkResult r = o_QueueWaitIdle(q);
        st_idleWaitNs += now_ns() - t0;
        return r;
    }
    st_idleSkipped++;
    enforce_depth(x);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_DeviceWaitIdle(VkDevice d) {
    if (LS_FOREIGN()) return o_DeviceWaitIdle(d);
    Dev *x = find_dev(d, false);
    bool ok = x && x->nq > 0;
    if (ok) for (int i = 0; i < x->nq && ok; i++) ok = add_marker(x, x->queues[i]);
    if (!ok) {
        uint64_t t0 = now_ns();
        VkResult r = o_DeviceWaitIdle(d);
        st_idleWaitNs += now_ns() - t0;
        if (x) wait_all(x);
        return r;
    }
    st_idleSkipped++;
    enforce_depth(x);
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL my_GetDeviceQueue(VkDevice d, uint32_t f, uint32_t i, VkQueue *q) {
    o_GetDeviceQueue(d, f, i, q);
    remember_queue(d, *q);
}

static VKAPI_ATTR void VKAPI_CALL my_GetDeviceQueue2(VkDevice d, const VkDeviceQueueInfo2 *info, VkQueue *q) {
    o_GetDeviceQueue2(d, info, q);
    remember_queue(d, *q);
}

static int g_cbUsed;
static bool g_cbOverflowed;  // table was cleared once: unknown command buffers must wait for all

static void track_cb(VkCommandBuffer cb, uint64_t serial) {
    if (g_cbUsed > 6000) {  // too many distinct command buffers: start over conservatively
        memset(g_cbs, 0, sizeof(g_cbs));
        g_cbUsed = 0;
        g_cbOverflowed = true;
    }
    size_t h = ((uintptr_t)cb >> 4) * 2654435761u % 8192;
    for (int p = 0; p < 8192; p++) {
        CbRec &r = g_cbs[(h + p) % 8192];
        if (r.cb == cb) { r.serial = serial; return; }
        if (!r.cb) { r.cb = cb; r.serial = serial; g_cbUsed++; return; }
    }
}

static uint64_t cb_serial(VkCommandBuffer cb) {
    size_t h = ((uintptr_t)cb >> 4) * 2654435761u % 8192;
    for (int p = 0; p < 8192; p++) {
        CbRec &r = g_cbs[(h + p) % 8192];
        if (r.cb == cb) return r.serial;
        if (!r.cb) return 0;
    }
    return 0;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f) {
    Dev *x = queue_dev(q);
    if (x) {
        uint64_t serial = __atomic_load_n(&x->nextSerial, __ATOMIC_ACQUIRE);
        pthread_mutex_lock(&g_cbLock);
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t k = 0; k < s[i].commandBufferCount; k++) track_cb(s[i].pCommandBuffers[k], serial);
        pthread_mutex_unlock(&g_cbLock);
    }
    return o_QueueSubmit(q, n, s, f);
}

static VKAPI_ATTR VkResult VKAPI_CALL my_QueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f) {
    Dev *x = queue_dev(q);
    if (x) {
        uint64_t serial = __atomic_load_n(&x->nextSerial, __ATOMIC_ACQUIRE);
        pthread_mutex_lock(&g_cbLock);
        for (uint32_t i = 0; i < n; i++)
            for (uint32_t k = 0; k < s[i].commandBufferInfoCount; k++) track_cb(s[i].pCommandBufferInfos[k].commandBuffer, serial);
        pthread_mutex_unlock(&g_cbLock);
    }
    return o_QueueSubmit2(q, n, s, f);
}

// A command buffer submitted before marker S is idle once marker S completed.
static void wait_cb(VkCommandBuffer cb) {
    pthread_mutex_lock(&g_cbLock);
    uint64_t s = cb_serial(cb);
    bool unknownRisky = !s && g_cbOverflowed;
    pthread_mutex_unlock(&g_cbLock);
    if (unknownRisky) { wait_all_devices(); return; }
    // With another graphics hook in the process, a command buffer we never saw submitted may have
    // been submitted through that module's own driver pointer: be conservative.
    if (!s) { if (coexist::g_compat) wait_all_devices(); return; }
    for (auto &x : g_devs) {
        if (!x.dev || !has_pending(&x)) continue;
        if (s < x.nextSerial && s > x.doneSerial) wait_upto(&x, s);
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL my_BeginCommandBuffer(VkCommandBuffer cb, const VkCommandBufferBeginInfo *b) {
    wait_cb(cb);
    return o_BeginCommandBuffer(cb, b);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_ResetCommandBuffer(VkCommandBuffer cb, VkCommandBufferResetFlags f) {
    wait_cb(cb);
    return o_ResetCommandBuffer(cb, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_ResetCommandPool(VkDevice d, VkCommandPool p, VkCommandPoolResetFlags f) {
    wait_all(find_dev(d, false));
    return o_ResetCommandPool(d, p, f);
}
static VKAPI_ATTR void VKAPI_CALL my_FreeCommandBuffers(VkDevice d, VkCommandPool p, uint32_t n, const VkCommandBuffer *c) {
    wait_all(find_dev(d, false));
    o_FreeCommandBuffers(d, p, n, c);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_MapMemory(VkDevice d, VkDeviceMemory m, VkDeviceSize o, VkDeviceSize s, VkMemoryMapFlags f, void **pp) {
    wait_all(find_dev(d, false));
    return o_MapMemory(d, m, o, s, f, pp);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_InvalidateMappedMemoryRanges(VkDevice d, uint32_t n, const VkMappedMemoryRange *r) {
    wait_all(find_dev(d, false));
    return o_InvalidateMappedMemoryRanges(d, n, r);
}
static bool is_occlusion(VkQueryPool p) {
    pthread_mutex_lock(&g_poolLock);
    bool r = false;
    for (auto &e : g_pools) if (e.pool == p) { r = e.type == VK_QUERY_TYPE_OCCLUSION; break; }
    pthread_mutex_unlock(&g_poolLock);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_CreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo *ci, const VkAllocationCallbacks *a, VkQueryPool *out) {
    VkResult r = o_CreateQueryPool(d, ci, a, out);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_poolLock);
        PoolRec *slot = nullptr;
        for (auto &e : g_pools) if (e.pool == *out || (!slot && !e.pool)) { slot = &e; if (e.pool == *out) break; }
        if (!slot) slot = &g_pools[((uintptr_t)*out >> 4) % 1024];
        *slot = {*out, ci->queryType};
        pthread_mutex_unlock(&g_poolLock);
    }
    return r;
}

// Occlusion results never block: unfinished queries read as "visible, available".
static VkResult occlusion_results(VkDevice d, VkQueryPool p, uint32_t first, uint32_t count, size_t size, void *data,
                                  VkDeviceSize stride, VkQueryResultFlags f) {
    uint64_t local[512];
    uint64_t *tmp = count <= 256 ? local : (uint64_t *)malloc((size_t)count * 16);
    if (!tmp) return o_GetQueryPoolResults(d, p, first, count, size, data, stride, f);
    VkResult r = o_GetQueryPoolResults(d, p, first, count, (size_t)count * 16, tmp, 16,
                                       VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
    if (r < 0) { if (tmp != local) free(tmp); return r; }
    bool wide = f & VK_QUERY_RESULT_64_BIT, withAvail = f & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT;
    uint64_t answered = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t *dst = (uint8_t *)data + (size_t)stride * i;
        if ((size_t)stride * i + (wide ? 8 : 4) * (withAvail ? 2 : 1) > size) break;
        uint64_t v = tmp[2 * i + 1] ? tmp[2 * i] : 1;
        if (!tmp[2 * i + 1]) answered++;
        if (wide) { ((uint64_t *)dst)[0] = v; if (withAvail) ((uint64_t *)dst)[1] = 1; }
        else { ((uint32_t *)dst)[0] = (uint32_t)(v > 0xffffffffu ? 0xffffffffu : v); if (withAvail) ((uint32_t *)dst)[1] = 1; }
    }
    st_vkOcclAnswered += answered;
    if (tmp != local) free(tmp);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL my_GetQueryPoolResults(VkDevice d, VkQueryPool p, uint32_t a, uint32_t b, size_t s, void *data, VkDeviceSize st, VkQueryResultFlags f) {
    if (LS_FOREIGN()) { wait_all(find_dev(d, false)); return o_GetQueryPoolResults(d, p, a, b, s, data, st, f); }
    if (g_occlNoWait && is_occlusion(p)) return occlusion_results(d, p, a, b, s, data, st, f);
    wait_all(find_dev(d, false));
    return o_GetQueryPoolResults(d, p, a, b, s, data, st, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_ResetFences(VkDevice d, uint32_t n, const VkFence *f) {
    wait_all(find_dev(d, false));
    return o_ResetFences(d, n, f);
}
static VKAPI_ATTR void VKAPI_CALL my_UpdateDescriptorSets(VkDevice d, uint32_t nw, const VkWriteDescriptorSet *w, uint32_t nc, const VkCopyDescriptorSet *c) {
    if (g_level < 2) wait_all(find_dev(d, false));
    o_UpdateDescriptorSets(d, nw, w, nc, c);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_ResetDescriptorPool(VkDevice d, VkDescriptorPool p, VkDescriptorPoolResetFlags f) {
    wait_all(find_dev(d, false));
    return o_ResetDescriptorPool(d, p, f);
}
static VKAPI_ATTR VkResult VKAPI_CALL my_FreeDescriptorSets(VkDevice d, VkDescriptorPool p, uint32_t n, const VkDescriptorSet *s) {
    wait_all(find_dev(d, false));
    return o_FreeDescriptorSets(d, p, n, s);
}

// Returns true if the destroy was queued for the reaper.
static bool defer(VkDevice d, PFN_Generic fn, uint64_t h, const VkAllocationCallbacks *a) {
    if (!h) return false;
    Dev *x = find_dev(d, false);
    if (!has_pending(x)) return false;
    if (a) { wait_all(x); return false; }  // custom allocators may not outlive the call
    pthread_mutex_lock(&x->lock);
    bool unfinished = false;
    for (int i = 0; i < x->count; i++) if (!x->m[(x->head + i) % 64].signaled) unfinished = true;
    if (!unfinished || x->qc == 4096) {
        pthread_mutex_unlock(&x->lock);
        if (unfinished) wait_all(x);
        return false;
    }
    x->q[(x->qh + x->qc) % 4096] = {fn, h, x->nextSerial - 1};
    x->qc++;
    pthread_mutex_unlock(&x->lock);
    st_deferred++;
    return true;
}

#define LS_DEFER(NAME, T)                                                                         \
    static PFN_vk##NAME o_##NAME;                                                                 \
    static VKAPI_ATTR void VKAPI_CALL my_##NAME(VkDevice d, T h, const VkAllocationCallbacks *a) { \
        if (!defer(d, (PFN_Generic)o_##NAME, (uint64_t)h, a)) o_##NAME(d, h, a);                   \
    }
LS_DEFER(DestroyBuffer, VkBuffer)
LS_DEFER(DestroyImage, VkImage)
LS_DEFER(FreeMemory, VkDeviceMemory)
LS_DEFER(DestroyImageView, VkImageView)
LS_DEFER(DestroyBufferView, VkBufferView)
LS_DEFER(DestroySampler, VkSampler)
LS_DEFER(DestroyFramebuffer, VkFramebuffer)
LS_DEFER(DestroyPipeline, VkPipeline)
LS_DEFER(DestroyRenderPass, VkRenderPass)
LS_DEFER(DestroyQueryPool, VkQueryPool)
LS_DEFER(DestroySemaphore, VkSemaphore)
LS_DEFER(DestroyFence, VkFence)
LS_DEFER(DestroyCommandPool, VkCommandPool)
LS_DEFER(DestroyDescriptorPool, VkDescriptorPool)
#undef LS_DEFER

// Called from the pipeline-cache DestroyDevice hook (or directly) before the device goes away.
static void device_teardown(VkDevice d) {
    Dev *x = find_dev(d, false);
    if (!x) return;
    wait_all(x);
    Deferred items[4096];
    pthread_mutex_lock(&x->lock);
    int n = 0;
    while (x->qc) { items[n++] = x->q[x->qh]; x->qh = (x->qh + 1) % 4096; x->qc--; }
    for (int i = 0; i < x->count; i++) o_DestroyFenceRaw(d, x->m[(x->head + i) % 64].fence, nullptr);
    x->count = 0;
    pthread_mutex_unlock(&x->lock);
    run_deferred(x, items, n);
    pthread_mutex_lock(&g_devsLock);
    for (auto &e : g_qmap) if (e.d == x) e = {};
    x->dev = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_devsLock);
}

// Reaper: retires markers in the background and runs deferred destroys.
static void *reaper(void *) {
    pthread_setname_np(pthread_self(), "LoadShiftReaper");
    for (;;) {
        bool any = false;
        for (auto &x : g_devs) {
            if (!x.dev || !has_pending(&x)) continue;
            pthread_mutex_lock(&x.lock);
            int k = -1;
            for (int i = 0; i < x.count; i++) {
                int j = (x.head + i) % 64;
                if (!x.m[j].signaled) { k = j; break; }
            }
            VkFence f = VK_NULL_HANDLE;
            if (k >= 0) { x.m[k].waiters++; f = x.m[k].fence; }
            pthread_mutex_unlock(&x.lock);
            Deferred items[256];
            if (f) {
                any = true;
                VkResult r = o_WaitForFences(x.dev, 1, &f, VK_TRUE, 50000000ull);
                pthread_mutex_lock(&x.lock);
                x.m[k].waiters--;
                if (r == VK_SUCCESS) x.m[k].signaled = true;
            } else pthread_mutex_lock(&x.lock);
            int n = retire_locked(&x, items, 256);
            pthread_mutex_unlock(&x.lock);
            run_deferred(&x, items, n);
            if (n) any = true;
        }
        if (!any) {
            pthread_mutex_lock(&g_wakeLock);
            timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000;
            if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&g_wake, &g_wakeLock, &ts);
            pthread_mutex_unlock(&g_wakeLock);
        }
    }
    return nullptr;
}

#define QG(fn) "_ZN11qglinternal" fn
static bool g_idleOn = true;

static int install(const LoadedLib &drv, int level, int depth, bool idleOn, bool occlNoWait) {
    g_idleOn = idleOn;
    g_occlNoWait = occlNoWait;
    g_level = level;
    g_depth = depth < 1 ? 1 : (depth > 8 ? 8 : depth);
    struct H { const char *sym; void **orig; void *mine; };
    H hooks[] = {
        {QG("15vkQueueWaitIdleEP9VkQueue_T"), (void **)&o_QueueWaitIdle, (void *)my_QueueWaitIdle},
        {QG("16vkDeviceWaitIdleEP10VkDevice_T"), (void **)&o_DeviceWaitIdle, (void *)my_DeviceWaitIdle},
        {QG("16vkGetDeviceQueueEP10VkDevice_TjjPP9VkQueue_T"), (void **)&o_GetDeviceQueue, (void *)my_GetDeviceQueue},
        {QG("17vkGetDeviceQueue2EP10VkDevice_TPK18VkDeviceQueueInfo2PP9VkQueue_T"), (void **)&o_GetDeviceQueue2, (void *)my_GetDeviceQueue2},
        {QG("13vkQueueSubmitEP9VkQueue_TjPK12VkSubmitInfoP9VkFence_T"), (void **)&o_QueueSubmit, (void *)my_QueueSubmit},
        {QG("14vkQueueSubmit2EP9VkQueue_TjPK13VkSubmitInfo2P9VkFence_T"), (void **)&o_QueueSubmit2, (void *)my_QueueSubmit2},
        {QG("20vkBeginCommandBufferEP17VkCommandBuffer_TPK24VkCommandBufferBeginInfo"), (void **)&o_BeginCommandBuffer, (void *)my_BeginCommandBuffer},
        {QG("20vkResetCommandBufferEP17VkCommandBuffer_Tj"), (void **)&o_ResetCommandBuffer, (void *)my_ResetCommandBuffer},
        {QG("18vkResetCommandPoolEP10VkDevice_TP15VkCommandPool_Tj"), (void **)&o_ResetCommandPool, (void *)my_ResetCommandPool},
        {QG("20vkFreeCommandBuffersEP10VkDevice_TP15VkCommandPool_TjPKP17VkCommandBuffer_T"), (void **)&o_FreeCommandBuffers, (void *)my_FreeCommandBuffers},
        {QG("11vkMapMemoryEP10VkDevice_TP16VkDeviceMemory_TmmjPPv"), (void **)&o_MapMemory, (void *)my_MapMemory},
        {QG("30vkInvalidateMappedMemoryRangesEP10VkDevice_TjPK19VkMappedMemoryRange"), (void **)&o_InvalidateMappedMemoryRanges, (void *)my_InvalidateMappedMemoryRanges},
        {QG("21vkGetQueryPoolResultsEP10VkDevice_TP13VkQueryPool_TjjmPvmj"), (void **)&o_GetQueryPoolResults, (void *)my_GetQueryPoolResults},
        {QG("17vkCreateQueryPoolEP10VkDevice_TPK21VkQueryPoolCreateInfoPK21VkAllocationCallbacksPP13VkQueryPool_T"), (void **)&o_CreateQueryPool, (void *)my_CreateQueryPool},
        {QG("13vkResetFencesEP10VkDevice_TjPKP9VkFence_T"), (void **)&o_ResetFences, (void *)my_ResetFences},
        {QG("22vkUpdateDescriptorSetsEP10VkDevice_TjPK20VkWriteDescriptorSetjPK19VkCopyDescriptorSet"), (void **)&o_UpdateDescriptorSets, (void *)my_UpdateDescriptorSets},
        {QG("21vkResetDescriptorPoolEP10VkDevice_TP18VkDescriptorPool_Tj"), (void **)&o_ResetDescriptorPool, (void *)my_ResetDescriptorPool},
        {QG("20vkFreeDescriptorSetsEP10VkDevice_TP18VkDescriptorPool_TjPKP17VkDescriptorSet_T"), (void **)&o_FreeDescriptorSets, (void *)my_FreeDescriptorSets},
        {QG("15vkDestroyBufferEP10VkDevice_TP10VkBuffer_TPK21VkAllocationCallbacks"), (void **)&o_DestroyBuffer, (void *)my_DestroyBuffer},
        {QG("14vkDestroyImageEP10VkDevice_TP9VkImage_TPK21VkAllocationCallbacks"), (void **)&o_DestroyImage, (void *)my_DestroyImage},
        {QG("12vkFreeMemoryEP10VkDevice_TP16VkDeviceMemory_TPK21VkAllocationCallbacks"), (void **)&o_FreeMemory, (void *)my_FreeMemory},
        {QG("18vkDestroyImageViewEP10VkDevice_TP13VkImageView_TPK21VkAllocationCallbacks"), (void **)&o_DestroyImageView, (void *)my_DestroyImageView},
        {QG("19vkDestroyBufferViewEP10VkDevice_TP14VkBufferView_TPK21VkAllocationCallbacks"), (void **)&o_DestroyBufferView, (void *)my_DestroyBufferView},
        {QG("16vkDestroySamplerEP10VkDevice_TP11VkSampler_TPK21VkAllocationCallbacks"), (void **)&o_DestroySampler, (void *)my_DestroySampler},
        {QG("20vkDestroyFramebufferEP10VkDevice_TP15VkFramebuffer_TPK21VkAllocationCallbacks"), (void **)&o_DestroyFramebuffer, (void *)my_DestroyFramebuffer},
        {QG("17vkDestroyPipelineEP10VkDevice_TP12VkPipeline_TPK21VkAllocationCallbacks"), (void **)&o_DestroyPipeline, (void *)my_DestroyPipeline},
        {QG("19vkDestroyRenderPassEP10VkDevice_TP14VkRenderPass_TPK21VkAllocationCallbacks"), (void **)&o_DestroyRenderPass, (void *)my_DestroyRenderPass},
        {QG("18vkDestroyQueryPoolEP10VkDevice_TP13VkQueryPool_TPK21VkAllocationCallbacks"), (void **)&o_DestroyQueryPool, (void *)my_DestroyQueryPool},
        {QG("18vkDestroySemaphoreEP10VkDevice_TP13VkSemaphore_TPK21VkAllocationCallbacks"), (void **)&o_DestroySemaphore, (void *)my_DestroySemaphore},
        {QG("14vkDestroyFenceEP10VkDevice_TP9VkFence_TPK21VkAllocationCallbacks"), (void **)&o_DestroyFence, (void *)my_DestroyFence},
        {QG("20vkDestroyCommandPoolEP10VkDevice_TP15VkCommandPool_TPK21VkAllocationCallbacks"), (void **)&o_DestroyCommandPool, (void *)my_DestroyCommandPool},
        {QG("23vkDestroyDescriptorPoolEP10VkDevice_TP18VkDescriptorPool_TPK21VkAllocationCallbacks"), (void **)&o_DestroyDescriptorPool, (void *)my_DestroyDescriptorPool},
    };
    const int N = sizeof(hooks) / sizeof(hooks[0]);
    const char *names[N + 3];
    void *addr[N + 3] = {};
    for (int i = 0; i < N; i++) names[i] = hooks[i].sym;
    names[N] = QG("13vkCreateFenceEP10VkDevice_TPK17VkFenceCreateInfoPK21VkAllocationCallbacksPP9VkFence_T");
    names[N + 1] = QG("15vkWaitForFencesEP10VkDevice_TjPKP9VkFence_Tjm");
    names[N + 2] = QG("15vkDestroyDeviceEP10VkDevice_TPK21VkAllocationCallbacks");  // resolved for completeness
    if (resolve_syms(drv, names, addr, N + 3) != N + 3) return -1;
    o_CreateFence = (PFN_vkCreateFence)addr[N];
    o_WaitForFences = (PFN_vkWaitForFences)addr[N + 1];
    for (int i = 0; i < N; i++) *hooks[i].orig = addr[i];
    o_DestroyFenceRaw = o_DestroyFence;
    int patched = 0, wanted = 0;
    for (int i = 0; i < N; i++) {
        bool queryHook = hooks[i].orig == (void **)&o_GetQueryPoolResults || hooks[i].orig == (void **)&o_CreateQueryPool;
        if (!idleOn && !queryHook) continue;  // occlusion-only mode
        wanted++;
        // Lazy WaitIdle and occlusion answers may complete without calling the next function.
        bool consuming = hooks[i].orig == (void **)&o_QueueWaitIdle || hooks[i].orig == (void **)&o_DeviceWaitIdle ||
                         hooks[i].orig == (void **)&o_GetQueryPoolResults;
        patched += vkH(drv, addr[i], hooks[i].mine, hooks[i].orig, consuming) > 0;
    }
    if (!idleOn) return patched;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, reaper, nullptr);
    (void)wanted;
    return patched;
}
#undef QG

}  // namespace vkidle
