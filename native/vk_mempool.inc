// VkDeviceMemory recycling (included from loadshift.cpp after vk_idle.inc).
//
// Allocating device memory is a kernel call plus fresh, not-yet-faulted pages: an 8 MB
// allocation costs ~0.3 ms and its first write another ~1.6 ms of page faults on the calling
// thread. Games that stream textures/buffers free and re-allocate similar blocks all the time.
// Freed blocks are kept in a bounded pool and handed back for the next allocation of the same
// memory type and a similar size (like a PC Vulkan memory allocator does), so neither the
// kernel call nor the page faults happen again.
//
// Only plain allocations are pooled (no pNext: no dedicated/imported/exported memory, no custom
// allocator). Contents of a recycled block are whatever was there before, which the Vulkan spec
// allows ("undefined"); apps that wrongly expect zeroed memory could show garbage, so this is an
// aggressive option.

namespace mempool {

static PFN_vkAllocateMemory o_Alloc;
static PFN_vkFreeMemory o_Free;
static PFN_vkMapMemory o_Map;
static PFN_vkUnmapMemory o_Unmap;

static size_t g_cap;  // bytes, 0 = off

struct Track { VkDeviceMemory mem; VkDevice dev; uint32_t type; VkDeviceSize size; bool mapped; };
static const VkDeviceMemory kTomb = (VkDeviceMemory)~0ull;
static Track g_t[16384];
struct Pooled { VkDevice dev; VkDeviceMemory mem; uint32_t type; VkDeviceSize size; uint64_t stamp; };
static Pooled g_pool[512];
static int g_np;
static size_t g_bytes;
static uint64_t g_clock;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static std::atomic<uint64_t> st_hits{0}, st_misses{0}, st_pooled{0}, st_trimmed{0}, st_savedBytes{0};

static size_t slot_of(VkDeviceMemory m) { return (size_t)(((uint64_t)m >> 4) * 0x9E3779B97F4A7C15ull >> 50) % 16384; }

static Track *find(VkDeviceMemory m) {
    size_t h = slot_of(m);
    for (int p = 0; p < 16384; p++) {
        Track &t = g_t[(h + p) % 16384];
        if (t.mem == m) return &t;
        if (!t.mem) return nullptr;
    }
    return nullptr;
}

static void put(VkDeviceMemory m, VkDevice d, uint32_t type, VkDeviceSize size) {
    size_t h = slot_of(m);
    for (int p = 0; p < 16384; p++) {
        Track &t = g_t[(h + p) % 16384];
        if (!t.mem || t.mem == kTomb || t.mem == m) { t = {m, d, type, size, false}; return; }
    }
}

static void untrack(Track *t) { if (t) t->mem = kTomb; }

// Frees pooled blocks (oldest first) until `need` more bytes fit. Caller holds g_lock.
static void trim_locked(size_t need) {
    while (g_np && g_bytes + need > g_cap) {
        int old = 0;
        for (int i = 1; i < g_np; i++) if (g_pool[i].stamp < g_pool[old].stamp) old = i;
        Pooled p = g_pool[old];
        g_pool[old] = g_pool[--g_np];
        g_bytes -= p.size;
        untrack(find(p.mem));
        o_Free(p.dev, p.mem, nullptr);
        st_trimmed++;
    }
}

// Final destination of every vkFreeMemory (direct or deferred by lazy WaitIdle).
static VKAPI_ATTR void VKAPI_CALL release(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) {
    if (!m) return;
    pthread_mutex_lock(&g_lock);
    Track *t = find(m);
    bool pool = t && !a && g_cap && t->size <= g_cap / 4 && g_np < 512;
    if (!pool) {
        untrack(t);
        pthread_mutex_unlock(&g_lock);
        o_Free(d, m, a);
        return;
    }
    bool mapped = t->mapped;
    t->mapped = false;
    trim_locked(t->size);
    g_pool[g_np++] = {d, m, t->type, t->size, ++g_clock};
    g_bytes += t->size;
    st_pooled++;
    pthread_mutex_unlock(&g_lock);
    if (mapped) o_Unmap(d, m);  // vkFreeMemory would have unmapped it implicitly
}

static VKAPI_ATTR VkResult VKAPI_CALL my_Alloc(VkDevice d, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *a, VkDeviceMemory *out) {
    bool eligible = g_cap && !a && !info->pNext;
    if (eligible) {
        pthread_mutex_lock(&g_lock);
        int best = -1;
        VkDeviceSize want = info->allocationSize, maxOk = want + want / 4;
        for (int i = 0; i < g_np; i++) {
            const Pooled &p = g_pool[i];
            if (p.dev != d || p.type != info->memoryTypeIndex || p.size < want || p.size > maxOk) continue;
            if (best < 0 || p.size < g_pool[best].size) best = i;
        }
        if (best >= 0) {
            Pooled p = g_pool[best];
            g_pool[best] = g_pool[--g_np];
            g_bytes -= p.size;
            pthread_mutex_unlock(&g_lock);
            *out = p.mem;
            st_hits++;
            st_savedBytes += p.size;
            return VK_SUCCESS;
        }
        pthread_mutex_unlock(&g_lock);
        st_misses++;
    }
    VkResult r = o_Alloc(d, info, a, out);
    if (r == VK_SUCCESS && eligible) {
        pthread_mutex_lock(&g_lock);
        put(*out, d, info->memoryTypeIndex, info->allocationSize);
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL my_Free(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a) { release(d, m, a); }

static VKAPI_ATTR VkResult VKAPI_CALL my_Map(VkDevice d, VkDeviceMemory m, VkDeviceSize off, VkDeviceSize size, VkMemoryMapFlags f, void **pp) {
    VkResult r = o_Map(d, m, off, size, f, pp);
    if (r == VK_SUCCESS) {
        pthread_mutex_lock(&g_lock);
        if (Track *t = find(m)) t->mapped = true;
        pthread_mutex_unlock(&g_lock);
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL my_Unmap(VkDevice d, VkDeviceMemory m) {
    pthread_mutex_lock(&g_lock);
    if (Track *t = find(m)) t->mapped = false;
    pthread_mutex_unlock(&g_lock);
    o_Unmap(d, m);
}

static void teardown(VkDevice d) {
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_np;) {
        if (g_pool[i].dev != d) { i++; continue; }
        Pooled p = g_pool[i];
        g_pool[i] = g_pool[--g_np];
        g_bytes -= p.size;
        untrack(find(p.mem));
        o_Free(p.dev, p.mem, nullptr);
    }
    for (auto &t : g_t) if (t.mem && t.mem != kTomb && t.dev == d) t.mem = kTomb;
    pthread_mutex_unlock(&g_lock);
}

#define QG(fn) "_ZN11qglinternal" fn
// Installs the pool. When lazy WaitIdle owns vkFreeMemory / vkMapMemory, the pool plugs into its
// "next" pointers instead of the table, so deferred frees also end up in the pool.
static int install(const LoadedLib &drv, size_t capBytes, bool idleOwnsFreeMap) {
    g_cap = capBytes;
    if (!g_cap) return 0;
    const char *names[] = {
        QG("16vkAllocateMemoryEP10VkDevice_TPK20VkMemoryAllocateInfoPK21VkAllocationCallbacksPP16VkDeviceMemory_T"),
        QG("12vkFreeMemoryEP10VkDevice_TP16VkDeviceMemory_TPK21VkAllocationCallbacks"),
        QG("11vkMapMemoryEP10VkDevice_TP16VkDeviceMemory_TmmjPPv"),
        QG("13vkUnmapMemoryEP10VkDevice_TP16VkDeviceMemory_T"),
    };
    void *a[4] = {};
    if (resolve_syms(drv, names, a, 4) != 4) return 0;
    // Free first: if another module owns it, recycling is not possible (nothing else installed).
    int nf;
    if (idleOwnsFreeMap) {
        o_Free = vkidle::o_FreeMemory;
        __atomic_store_n(&vkidle::o_FreeMemory, (PFN_vkFreeMemory)release, __ATOMIC_RELEASE);
        nf = 1;
    } else {
        nf = vkH(drv, a[1], (void *)my_Free, (void **)&o_Free, true);
    }
    if (nf <= 0) { g_cap = 0; return 0; }
    int na = vkH(drv, a[0], (void *)my_Alloc, (void **)&o_Alloc, true);
    if (na <= 0) {  // allocation hook not possible: blocks would never be reused, so pool nothing
        g_cap = 0;
        return 0;
    }
    if (idleOwnsFreeMap) {
        o_Map = vkidle::o_MapMemory;
        __atomic_store_n(&vkidle::o_MapMemory, (PFN_vkMapMemory)my_Map, __ATOMIC_RELEASE);
    } else {
        vkH(drv, a[2], (void *)my_Map, (void **)&o_Map);
    }
    vkH(drv, a[3], (void *)my_Unmap, (void **)&o_Unmap);
    if (!o_Unmap) o_Unmap = (PFN_vkUnmapMemory)a[3];
    return 1;
}
#undef QG

}  // namespace mempool
