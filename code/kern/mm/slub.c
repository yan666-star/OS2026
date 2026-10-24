#include <slub.h>

#include <assert.h>
#include <defs.h>
#include <list.h>
#include <memlayout.h>
#include <mmu.h>
#include <pmm.h>
#include <stdio.h>

/*
 * SLUB 风格两层对象分配器（Challenge）
 * challenge task 2414010
 * ===================== 第一层 / 第二层职责边界 =====================
 * 第一层（已有代码，kern/mm/pmm.c + best_fit_pmm_manager）负责"页"：
 *   本文件中对第一层的调用只有两处（R4）：
 *     1) kmalloc() 的小对象路径里 slub_slab_create() 中的 alloc_pages(1)
 *        —— 为新 slab 申请恰好 1 个物理页；
 *     2) kmalloc() 的大对象路径中的 alloc_pages(ceil(size/PGSIZE))
 *        —— 大对象直通，按页申请连续内存；
 *     3) kfree() 中的 free_pages(page, n)
 *        —— 空 slab（1 页）或大对象（登记的页数）整批归还。
 *   除此之外绝不触碰第一层内部：不读 best_fit/default 的 free_list/nr_free，
 *   不碰 struct Page 的 property/flags，不调用 pmm_manager->xxx。
 *
 * 第二层（本文件）负责"对象"：
 *   - 把第一层给的每个整页切成等长对象，按档（size class）分 cache 管理；
 *   - slab 元数据放在本文件自己的静态结构体池里，绝不给 struct Page 加字段、
 *     绝不复用 Page 的 property/ref；
 *   - 空闲对象链表"内嵌"在空闲对象内部：对象头 8 字节存 next 指针，
 *     分配 O(1) 摘链、释放 O(1) 头插，不额外消耗内存（R1）。
 *
 * 地址换算（本工程事实）：
 *   页 → 可写内核虚址：page2pa(page) + va_pa_offset（slub_page2va）；
 *   对象 → struct Page：pa2page(PADDR(obj))（slub_va2page）。
 *   pmm.h 的 KADDR 已被注释，这里用 static inline 辅助函数代替，不改 pmm.h。
 */

// 已分配对象位图：每 slab 一份，双重复释放检测 + 反查校验。
// 最大对象数 = PGSIZE / 8 = 512 个，512 bit = 8 个 64 位字。
#define SLUB_BITMAP_WORDS ((PGSIZE / SLUB_MIN_OBJ_SIZE + 63) / 64)

// 一个 slab 的元数据（全部位于下面的静态池中，宿主页上不存任何元数据）。
struct slub_slab {
    int used;       // 元数据槽是否被占用
    unsigned int cls;   // 所属档次下标（0 .. SLUB_CLASS_COUNT-1）
    struct Page *page;  // 宿主页：第一层 alloc_pages(1) 返回的页描述符
    void *freelist;     // 内嵌空闲链表头；next 指针存于各空闲对象头 8 字节
    unsigned int inuse;     // 已分配对象数
    unsigned int nr_objs;   // 对象总数 = PGSIZE / 档大小
    uint64_t bits[SLUB_BITMAP_WORDS];  // 已分配对象位图（bit=1 表示已分配）
    list_entry_t link;  // 挂在所属 cache 的 partial / full 链表上
};

// 一档 cache：管理该档所有 slab（partial + full 两条链）。
struct slub_cache {
    size_t obj_size;    // 档大小（字节），用户可见大小 >= 请求 size
    unsigned int nr_objs;   // 每 slab 对象数
    list_entry_t partial;   // 还有空闲对象的 slab（kmalloc 只从这里取）
    list_entry_t full;      // 全满的 slab（kfree 腾出空位后回到 partial）
    size_t slabs;       // 当前持有的 slab 数（== 占用页数，I1）
    size_t inuse;       // 该档当前已分配对象总数
};

// 大对象登记表项：宿主页 + 页数 + 请求大小（kfree 反查用）。
struct slub_large {
    int used;
    struct Page *page;  // 首页描述符
    size_t npages;      // 占用页数 = ceil(size / PGSIZE)
    size_t req_size;    // 用户请求的字节数（仅记录，便于调试）
};

// ---- 静态状态（内核栈仅 2 页，大数组一律 static）----
static struct slub_cache slub_caches[SLUB_CLASS_COUNT];
static struct slub_slab slub_slab_pool[SLUB_MAX_SLABS];
static struct slub_large slub_large_tab[SLUB_MAX_LARGE];
static size_t slub_large_nr;  // 大对象表当前存活项数
static int slub_inited;       // slub_init() 是否已执行（防御性检查）

// 档次大小表（2 的幂，8 .. 2048）
static const size_t slub_class_sizes[SLUB_CLASS_COUNT] = {
    8, 16, 32, 64, 128, 256, 512, 1024, 2048,
};

// ---------------------------------------------------------------------------
// 地址换算辅助（代替被注释掉的 KADDR，不改 pmm.h）
// ---------------------------------------------------------------------------

// 页 → 可写内核虚地址：物理地址 + 固定映射偏移。
static inline void *
slub_page2va(struct Page *page) {
    return (void *)(uintptr_t)(page2pa(page) + va_pa_offset);
}

// 对象指针 → 宿主页描述符（PADDR 要求内核虚址，非内核地址会 panic）。
static inline struct Page *
slub_va2page(void *va) {
    return pa2page(PADDR((uintptr_t)va));
}

// ---------------------------------------------------------------------------
// 内嵌空闲链表：next 指针存于空闲对象头 8 字节，O(1) 摘/挂链（R1）
// ---------------------------------------------------------------------------

// 头插一个空闲对象。
static inline void
slub_freelist_push(struct slub_slab *slab, void *obj) {
    *(void **)obj = slab->freelist;
    slab->freelist = obj;
}

// 摘下链头对象并返回（调用方保证链非空）。
static inline void *
slub_freelist_pop(struct slub_slab *slab) {
    void *obj = slab->freelist;
    assert(obj != NULL);
    slab->freelist = *(void **)obj;  // 对象头 8 字节就是 next 指针
    return obj;
}

// ---------------------------------------------------------------------------
// slab 位图辅助（双重复释放检测 + kfree 反查校验）
// ---------------------------------------------------------------------------

static inline void
slub_bit_set(uint64_t *bits, unsigned int idx) {
    bits[idx >> 6] |= (uint64_t)1 << (idx & 63);
}

static inline void
slub_bit_clear(uint64_t *bits, unsigned int idx) {
    bits[idx >> 6] &= ~((uint64_t)1 << (idx & 63));
}

static inline int
slub_bit_test(const uint64_t *bits, unsigned int idx) {
    return (bits[idx >> 6] >> (idx & 63)) & 1;
}

// ---------------------------------------------------------------------------
// size → 档映射与 slab 生命周期
// ---------------------------------------------------------------------------

// 返回覆盖 size 的最小档下标（1→0，100→3(128B)，2000→8(2048B)）。
// 前置：1 <= size <= SLUB_MAX_OBJ_SIZE。
static unsigned int
slub_size_to_class(size_t size) {
    unsigned int idx = 0;
    assert(size >= 1);
    while (slub_class_sizes[idx] < size) {
        idx++;
        assert(idx < SLUB_CLASS_COUNT);  // size 超过最大档说明走错了分支
    }
    return idx;
}

// 在静态元数据池里找一个空槽；耗尽返回 NULL（kmalloc 据此零副作用失败）。
static struct slub_slab *
slub_slab_slot_alloc(void) {
    for (unsigned int i = 0; i < SLUB_MAX_SLABS; i++) {
        if (!slub_slab_pool[i].used) {
            return &slub_slab_pool[i];
        }
    }
    return NULL;
}

// 取该档 partial 链上的第一个 slab；没有则新建一个：
// 先占元数据槽，再向第一层 alloc_pages(1) 要页（两层调用点之一）。
// 任一步失败都返回 NULL，且不留下任何状态变化（I6）。
static struct slub_slab *
slub_slab_get(struct slub_cache *cache, unsigned int cls) {
    if (!list_empty(&cache->partial)) {
        return to_struct(list_next(&cache->partial), struct slub_slab, link);
    }

    struct slub_slab *slab = slub_slab_slot_alloc();
    if (slab == NULL) {
        return NULL;  // 元数据池耗尽，尚未触碰任何状态
    }

    struct Page *page = alloc_pages(1);  // ← 第一层调用：恰 1 页（I1）
    if (page == NULL) {
        return NULL;  // 第一层无页，槽未标记 used，零副作用
    }

    slab->used = 1;
    slab->cls = cls;
    slab->page = page;
    slab->inuse = 0;
    slab->nr_objs = cache->nr_objs;
    for (unsigned int w = 0; w < SLUB_BITMAP_WORDS; w++) {
        slab->bits[w] = 0;
    }

    // 建内嵌空闲链表：倒序头插，使链头是页内第 0 个对象。
    // 第 i 个对象偏移 = i * 档大小（I2），绝不跨页。
    slab->freelist = NULL;
    uintptr_t base = (uintptr_t)slub_page2va(page);
    for (int i = (int)cache->nr_objs - 1; i >= 0; i--) {
        void *obj = (void *)(base + (size_t)i * cache->obj_size);
        slub_freelist_push(slab, obj);
    }

    list_init(&slab->link);
    list_add(&cache->partial, &slab->link);
    cache->slabs++;
    return slab;
}

// ---------------------------------------------------------------------------
// G1: slub_init / kmalloc / kfree
// ---------------------------------------------------------------------------

// S1: 初始化所有 cache、slab 元数据池、大对象表。
// 不向第一层申请任何页（调用前后 nr_free_pages() 相同）。
void
slub_init(void) {
    for (unsigned int i = 0; i < SLUB_CLASS_COUNT; i++) {
        struct slub_cache *cache = &slub_caches[i];
        cache->obj_size = slub_class_sizes[i];
        cache->nr_objs = PGSIZE / cache->obj_size;
        list_init(&cache->partial);
        list_init(&cache->full);
        cache->slabs = 0;
        cache->inuse = 0;
    }
    for (unsigned int i = 0; i < SLUB_MAX_SLABS; i++) {
        struct slub_slab *slab = &slub_slab_pool[i];
        slab->used = 0;
        slab->page = NULL;
        slab->freelist = NULL;
        slab->inuse = 0;
        slab->nr_objs = 0;
        slab->cls = 0;
        list_init(&slab->link);
    }
    for (unsigned int i = 0; i < SLUB_MAX_LARGE; i++) {
        slub_large_tab[i].used = 0;
        slub_large_tab[i].page = NULL;
        slub_large_tab[i].npages = 0;
        slub_large_tab[i].req_size = 0;
    }
    slub_large_nr = 0;
    slub_inited = 1;

    cprintf("slub: init done, %u classes (%lub..%lub), "
            "slab pool %u, large table %u\n",
            (unsigned)SLUB_CLASS_COUNT, (unsigned long)SLUB_MIN_OBJ_SIZE,
            (unsigned long)SLUB_MAX_OBJ_SIZE, (unsigned)SLUB_MAX_SLABS,
            (unsigned)SLUB_MAX_LARGE);
    slub_dump();  // 启动日志里给出汇总（此时全部为 0）
}

// S2: 分配任意 size 字节；成功返回可写内核虚址，失败返回 NULL（零副作用）。
void *
kmalloc(size_t size) {
    assert(slub_inited);

    if (size == 0) {  // Case 0
        return NULL;
    }

    if (size > SLUB_MAX_OBJ_SIZE) {
        // Case 2: 大对象直通。先查表容量再要页，保证失败时零副作用（I6）。
        if (slub_large_nr >= SLUB_MAX_LARGE) {
            return NULL;  // 大对象表满
        }
        size_t npages = (size + PGSIZE - 1) / PGSIZE;  // = ceil(size/PGSIZE)
        struct Page *page = alloc_pages(npages);  // ← 第一层调用
        if (page == NULL) {
            return NULL;
        }
        for (unsigned int i = 0; i < SLUB_MAX_LARGE; i++) {
            struct slub_large *entry = &slub_large_tab[i];
            if (entry->used) {
                continue;
            }
            entry->used = 1;
            entry->page = page;
            entry->npages = npages;
            entry->req_size = size;
            slub_large_nr++;
            break;  // 槽位必能找到（slub_large_nr < SLUB_MAX_LARGE）
        }
        // 返回页对齐基址（I5），可用字节数 = npages*PGSIZE >= size。
        return slub_page2va(page);
    }

    // Case 1: 走档次路径。cls = 覆盖 size 的最小档；用户可见大小 >= size。
    unsigned int cls = slub_size_to_class(size);
    struct slub_cache *cache = &slub_caches[cls];
    struct slub_slab *slab = slub_slab_get(cache, cls);
    if (slab == NULL) {
        return NULL;  // Case 3：第一层无页或元数据池耗尽，零副作用
    }

    // O(1) 摘链（R1）：链头即返回对象。
    void *obj = slub_freelist_pop(slab);
    uintptr_t base = (uintptr_t)slub_page2va(slab->page);
    size_t off = (uintptr_t)obj - base;
    unsigned int idx = (unsigned int)(off / cache->obj_size);
    assert(off < PGSIZE && off % cache->obj_size == 0);  // I2/I7
    assert(!slub_bit_test(slab->bits, idx));
    slub_bit_set(slab->bits, idx);

    slab->inuse++;
    cache->inuse++;
    if (slab->inuse == slab->nr_objs) {
        // 未满 → 满：从 partial 移到 full。
        list_del_init(&slab->link);
        list_add(&cache->full, &slab->link);
    }
    // 档大小是 2 的幂、页基址页对齐 ⇒ obj % 档大小 == 0，至少 8 字节对齐（I7）。
    return obj;
}

// S3: 释放 kmalloc 返回的对象；NULL 直接返回。
// 非法指针 / 重复释放 / 非本分配器地址 → panic，不静默返回（Case 3）。
void
kfree(void *obj) {
    assert(slub_inited);

    if (obj == NULL) {  // Case 0
        return;
    }
    struct Page *pg = slub_va2page(obj);

    // Case 1: 先在大对象表里线性反查（允许线性查找）。
    for (unsigned int i = 0; i < SLUB_MAX_LARGE; i++) {
        struct slub_large *entry = &slub_large_tab[i];
        if (!entry->used || entry->page != pg) {
            continue;
        }
        // 大对象只接受基址释放；中间地址属于非法指针。
        assert((uintptr_t)obj == (uintptr_t)slub_page2va(entry->page));
        size_t npages = entry->npages;
        entry->used = 0;
        entry->page = NULL;
        entry->npages = 0;
        entry->req_size = 0;
        slub_large_nr--;
        free_pages(pg, npages);  // ← 第一层调用：整批归还
        return;
    }

    // Case 2: 小对象。在 slab 元数据池里线性反查宿主 slab（匹配宿主页）。
    struct slub_slab *slab = NULL;
    for (unsigned int i = 0; i < SLUB_MAX_SLABS; i++) {
        if (slub_slab_pool[i].used && slub_slab_pool[i].page == pg) {
            slab = &slub_slab_pool[i];
            break;
        }
    }
    if (slab == NULL) {
        panic("kfree: address %016lx not allocated by kmalloc",
              (uintptr_t)obj);
    }

    struct slub_cache *cache = &slub_caches[slab->cls];
    uintptr_t base = (uintptr_t)slub_page2va(slab->page);
    size_t off = (uintptr_t)obj - base;
    if (off >= PGSIZE || off % cache->obj_size != 0) {
        panic("kfree: bad object offset %lu (class size %lu)",
              (unsigned long)off, (unsigned long)cache->obj_size);
    }
    unsigned int idx = (unsigned int)(off / cache->obj_size);
    // bit==0 表示该对象当前在空闲链表上或从未分配 → 重复释放/非法指针。
    if (!slub_bit_test(slab->bits, idx)) {
        panic("kfree: double free of object %016lx", (uintptr_t)obj);
    }

    int was_full = (slab->inuse == slab->nr_objs);
    slub_bit_clear(slab->bits, idx);
    slub_freelist_push(slab, obj);  // O(1) 头插回内嵌空闲链表（R1）
    slab->inuse--;
    cache->inuse--;

    if (slab->inuse == 0) {
        // I4: 已用对象数归零 ⇒ 空 slab 不驻留：摘链、还页、归还元数据槽。
        list_del_init(&slab->link);
        cache->slabs--;
        struct Page *page = slab->page;
        slab->used = 0;
        slab->page = NULL;
        slab->freelist = NULL;
        slab->inuse = 0;
        slab->nr_objs = 0;
        slab->cls = 0;
        free_pages(page, 1);  // ← 第一层调用：归还宿主页
        return;
    }
    if (was_full) {
        // 满 → 未满：从 full 移回 partial，恢复可分配。
        list_del_init(&slab->link);
        list_add(&cache->partial, &slab->link);
    }
}

// ---------------------------------------------------------------------------
// G2: 只读统计与调试接口（不修改任何状态；idx 非法返回 0）
// ---------------------------------------------------------------------------

unsigned int
slub_class_count(void) {
    return SLUB_CLASS_COUNT;
}

size_t
slub_class_size(unsigned int idx) {
    if (idx >= SLUB_CLASS_COUNT) {
        return 0;
    }
    return slub_class_sizes[idx];
}

size_t
slub_slabs_of_class(unsigned int idx) {
    if (idx >= SLUB_CLASS_COUNT) {
        return 0;
    }
    return slub_caches[idx].slabs;
}

size_t
slub_objects_of_class(unsigned int idx) {
    if (idx >= SLUB_CLASS_COUNT) {
        return 0;
    }
    return slub_caches[idx].inuse;
}

size_t
slub_large_count(void) {
    return slub_large_nr;
}

// S4: 每档一行（档大小/slab 数/已用对象数/该档容量），加大对象数与第一层空闲页。
void
slub_dump(void) {
    for (unsigned int i = 0; i < SLUB_CLASS_COUNT; i++) {
        struct slub_cache *cache = &slub_caches[i];
        cprintf("slub: class %u size %4lu: slabs=%lu objs=%lu cap=%lu\n",
                i, (unsigned long)cache->obj_size,
                (unsigned long)cache->slabs, (unsigned long)cache->inuse,
                (unsigned long)(cache->slabs * cache->nr_objs));
    }
    cprintf("slub: large objects=%lu, first-layer free pages=%lu\n",
            (unsigned long)slub_large_nr, (unsigned long)nr_free_pages());
}
