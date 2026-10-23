//lab2 buddy system 2414010
#include <assert.h>
#include <buddy_pmm.h>
#include <list.h>
#include <memlayout.h>
#include <pmm.h>
#include <stdio.h>

/*
 * Buddy System 物理内存管理器
 *
 * 名词约定（本文件全局通用）：
 *  - 阶 order：块大小为 2^order 页，order 取值 0 .. BUDDY_MAX_ORDER-1。
 *  - 伙伴 buddy：大小为 2^k、相对"区域基址"偏移分别为 a 与 a^(2^k) 的两个块互
 *    为伙伴；二者同时空闲时合并为 2^(k+1) 的块，并继续向上尝试合并。
 *  - 区域基址 region base：init_memmap 时记录的第一个页描述符指针。本管理器所有
 *    对齐判断、伙伴计算、阶数推导都只依赖"相对区域基址的页偏移"，绝不依赖页描
 *    述符在 pages 数组中的绝对下标，因此可以被独立单元测试。
 *  - 尾页归还：分配 n 页时实际取出的是 2^k (k = ceil(log2(n))) 页的块，多出来的
 *    [n, 2^k) 尾页不交付调用方，而是按"尽量大且对齐"的规则拆成 2 的幂块重新挂回
 *    空闲链表并参与伙伴合并，从而只交付恰好 n 页、nr_free 也只减少 n。
 */

// 推荐 16 阶：0..15，最大块 2^15 = 32768 页 = 128MiB，足以覆盖真实内存(约 126MB)。
#define BUDDY_MAX_ORDER 16

// 每一阶一条空闲块链表；nr_free 字段记录该阶的空闲块个数。
static free_area_t buddy_free_area[BUDDY_MAX_ORDER];
// 空闲页总数（不在 free_area_t 里累加，避免与"块个数"混淆）。
static size_t buddy_nr_free = 0;
// 区域基址与区域总页数；buddy_region_valid 表示是否已接管区域。
static struct Page *buddy_region_base = NULL;
static size_t buddy_region_pages = 0;
static bool buddy_region_valid = 0;

static void buddy_check(void);

// ---------------------------------------------------------------------------
// 内部辅助函数（一律 static，不对外暴露）
// ---------------------------------------------------------------------------

// 阶 order 对应的块大小（页数）。
//lab2 buddy system 2414010
static size_t
buddy_block_size(unsigned int order) {
    return (size_t)1 << order;
}

// 页相对区域基址的偏移；调用前必须已记录区域基址。
//lab2 buddy system 2414010
static size_t
buddy_page_offset(struct Page *page) {
    return (size_t)(page - buddy_region_base);
}

// 覆盖 n 页所需的最小阶 k = ceil(log2(n))。结果 >= BUDDY_MAX_ORDER 表示放不下。
// 循环次数被 BUDDY_MAX_ORDER 限制，n 很大时也能在有限步内返回。
//lab2 buddy system 2414010
static unsigned int
buddy_ceil_order(size_t n) {
    unsigned int order = 0;
    size_t size = 1;
    while (size < n && order < BUDDY_MAX_ORDER) {
        size <<= 1;
        order++;
    }
    return order;
}

// size 是 2 的幂时，返回它对应的阶（即 log2(size)）。
//lab2 buddy system 2414010
static unsigned int
buddy_floor_order(size_t size) {
    unsigned int order = 0;
    while (order + 1 < BUDDY_MAX_ORDER &&
           buddy_block_size(order + 1) <= size) {
        order++;
    }
    return order;
}

// 在偏移 offset 处、剩余 remaining 页时能放下的最大块大小（页）。
// 约束：不大于 remaining、相对区域基址对齐、阶不超过 BUDDY_MAX_ORDER-1。
// 每轮至少能取 1 页，循环次数不超过 BUDDY_MAX_ORDER。
//lab2 buddy system 2414010
static size_t
buddy_max_aligned_size(size_t offset, size_t remaining) {
    size_t size = 1;
    unsigned int order = 0;
    while (order + 1 < BUDDY_MAX_ORDER) {
        size_t bigger = size << 1;
        if (bigger > remaining) {
            break;
        }
        if ((offset & (bigger - 1)) != 0) {  // 不满足对齐不变式 I2
            break;
        }
        size = bigger;
        order++;
    }
    return size;
}

// 按地址升序插入第 order 阶空闲链表（有序插入保证行为确定、便于调试）。
//lab2 buddy system 2414010
static void
buddy_list_insert(struct Page *block, unsigned int order) {
    assert(order < BUDDY_MAX_ORDER);
    free_area_t *area = &buddy_free_area[order];
    list_entry_t *le = &area->free_list;
    while ((le = list_next(le)) != &area->free_list) {
        if (block < le2page(le, page_link)) {
            break;
        }
    }
    list_add_before(le, &block->page_link);
    area->nr_free++;
}

// 从第 order 阶空闲链表中摘除一个块。
//lab2 buddy system 2414010
static void
buddy_list_remove(struct Page *block, unsigned int order) {
    assert(order < BUDDY_MAX_ORDER);
    assert(buddy_free_area[order].nr_free > 0);
    list_del_init(&block->page_link);
    buddy_free_area[order].nr_free--;
}

// 把一个大小 2^order 的空闲块挂回链表，并逐级向上尝试与伙伴合并。
// 调用前 block->property 必须等于 2^order 且已 SetPageProperty。
// 不修改 buddy_nr_free（由调用者统一维护）。
//lab2 buddy system 2414010
static void
buddy_insert_block(struct Page *block, unsigned int order) {
    // 合并层数每轮 +1，循环次数 <= BUDDY_MAX_ORDER。
    while (order + 1 < BUDDY_MAX_ORDER) {
        size_t size = buddy_block_size(order);
        size_t offset = buddy_page_offset(block);
        assert((offset & (size - 1)) == 0);  // 对齐不变式

        // 伙伴关系：偏移的第 order 位取反。只依赖相对区域基址的偏移。
        size_t buddy_offset = offset ^ size;
        if (buddy_offset + size > buddy_region_pages) {
            break;  // 伙伴落在区域之外，不可能存在
        }
        struct Page *buddy = buddy_region_base + buddy_offset;

        // 伙伴只有在正好是"同阶空闲块首页"时才能合并；若它是更大空闲块的内部
        // 页，或是已被分配出去的页，则 property 不匹配，停止合并。
        if (!PageProperty(buddy) || buddy->property != size) {
            break;
        }

        buddy_list_remove(buddy, order);
        ClearPageProperty(buddy);
        buddy->property = 0;

        // 合并后的块首页取地址较小者。
        struct Page *self = block;
        if (offset & size) {
            block = buddy;  // 自己是右(高)半块，伙伴才是首页
        }
        if (block != self) {
            // 自己不再是首页：必须清掉 property，否则成了新块内部页(违反 I1)。
            ClearPageProperty(self);
            self->property = 0;
        }
        order++;
        block->property = buddy_block_size(order);
        SetPageProperty(block);
    }
    buddy_list_insert(block, order);
}

// 把区间 [base, base+n) 归还空闲链表：按"每次取尽量大且满足对齐"的贪心拆成
// 若干 2 的幂块，逐块插入并做伙伴合并。非 2 的幂的释放区间就是这样处理的。
// 不修改 buddy_nr_free（由调用者统一维护）。
//lab2 buddy system 2414010
static void
buddy_release_range(struct Page *base, size_t n) {
    assert(n > 0);
    assert(buddy_region_valid);

    // 区间内每一页复位：非空闲块首页的 property/flags 必须为 0（I1）。
    for (struct Page *p = base; p != base + n; p++) {
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    size_t done = 0;
    while (done < n) {  // 每轮至少归还 1 页，必然终止
        size_t offset = buddy_page_offset(base + done);
        size_t size = buddy_max_aligned_size(offset, n - done);
        struct Page *block = base + done;
        block->property = size;
        SetPageProperty(block);
        buddy_insert_block(block, buddy_floor_order(size));
        done += size;
    }
}

// 当前能否分配的最大页数（最大空闲块的页数），没有空闲块时返回 0。
//lab2 buddy system 2414010
static size_t
buddy_largest_free_block(void) {
    for (unsigned int order = BUDDY_MAX_ORDER; order > 0; order--) {
        if (!list_empty(&buddy_free_area[order - 1].free_list)) {
            return buddy_block_size(order - 1);
        }
    }
    return 0;
}

// 校验全局不变式 I1~I4（供 buddy_check 复用）。
//lab2 buddy system 2414010
static void
buddy_verify_free_lists(void) {
    size_t total = 0;
    for (unsigned int order = 0; order < BUDDY_MAX_ORDER; order++) {
        size_t size = buddy_block_size(order);
        unsigned int blocks = 0;
        list_entry_t *head = &buddy_free_area[order].free_list;
        list_entry_t *le = head;
        while ((le = list_next(le)) != head) {
            struct Page *block = le2page(le, page_link);
            // I1: 每个空闲块大小恰为 2^order，首页置 PageProperty 且 property 匹配。
            assert(PageProperty(block));
            assert(block->property == size);
            if (buddy_region_valid) {
                // I2: 块相对区域基址的偏移能被自身大小整除。
                assert((buddy_page_offset(block) & (size - 1)) == 0);
                assert(buddy_page_offset(block) + size <= buddy_region_pages);
            }
            // I1: 块内其余页 property == 0 且不带 PageProperty。
            // I3: 每个空闲块首页都带 PageProperty，因此任何相交(嵌套)都会在这里
            //     被"内部页带 PageProperty"暴露出来，无需两两比较区间。
            for (size_t i = 1; i < size; i++) {
                assert(block[i].property == 0);
                assert(!PageProperty(&block[i]));
            }
            blocks++;
            total += size;
        }
        assert(blocks == buddy_free_area[order].nr_free);
    }
    // I4: 所有空闲块大小之和 == nr_free。
    assert(total == buddy_nr_free);
    // I6: nr_free 不超过区域总页数。
    if (buddy_region_valid) {
        assert(buddy_nr_free <= buddy_region_pages);
    } else {
        assert(buddy_nr_free == 0);
    }
}

// ---------------------------------------------------------------------------
// pmm_manager 的六个成员
// ---------------------------------------------------------------------------

// S1: 回到"未接管任何内存"的初始态；可重复调用（幂等）。
//lab2 buddy system 2414010
static void
buddy_init(void) {
    for (unsigned int order = 0; order < BUDDY_MAX_ORDER; order++) {
        list_init(&buddy_free_area[order].free_list);
        buddy_free_area[order].nr_free = 0;
    }
    buddy_nr_free = 0;
    buddy_region_base = NULL;   // 区域基址失效
    buddy_region_pages = 0;
    buddy_region_valid = 0;
}

// S2: 接管 [base, base+n)，并按"从大到小且满足对齐"拆成 2 的幂块插入链表。
//lab2 buddy system 2414010
static void
buddy_init_memmap(struct Page *base, size_t n) {
    assert(n > 0);
    struct Page *p = base;
    for (; p != base + n; p++) {
        assert(PageReserved(p));  // 与 default/best fit 的约定一致
        p->flags = p->property = 0;
        set_page_ref(p, 0);
    }

    // 首次接管时记录区域基址；之后新接管的区间必须位于该基址之上，
    // 否则"相对基址的偏移"没有意义。
    if (!buddy_region_valid) {
        buddy_region_base = base;
        buddy_region_pages = 0;
        buddy_region_valid = 1;
    } else {
        assert(base >= buddy_region_base);
    }
    buddy_region_pages += n;

    buddy_nr_free += n;
    buddy_release_range(base, n);
}

// S3: 分配恰好 n 页。
//lab2 buddy system 2414010
static struct Page *
buddy_alloc_pages(size_t n) {
    assert(n > 0);

    // Case 2: 失败分支必须在改动任何状态之前返回。
    if (!buddy_region_valid || n > buddy_nr_free) {
        return NULL;
    }
    unsigned int order = buddy_ceil_order(n);  // k = ceil(log2(n))
    if (order >= BUDDY_MAX_ORDER) {
        return NULL;
    }

    // 从 k 阶向上找第一个非空阶。
    unsigned int found = order;
    while (found < BUDDY_MAX_ORDER &&
           list_empty(&buddy_free_area[found].free_list)) {
        found++;
    }
    if (found == BUDDY_MAX_ORDER) {
        return NULL;  // 没有足够大的空闲块
    }

    // 取出该块的表头。
    list_entry_t *le = list_next(&buddy_free_area[found].free_list);
    struct Page *block = le2page(le, page_link);
    buddy_list_remove(block, found);

    // 逐级对半拆分到 k 阶：每次把高半(兄弟)块挂到它自己的阶上。
    // 被拆分的块原本就是"不可合并"的空闲块，拆出的兄弟块此刻不可能与块外的
    // 伙伴合并，因此直接插入即可（无需 merge）。层数 <= BUDDY_MAX_ORDER。
    while (found > order) {
        found--;
        size_t half = buddy_block_size(found);
        struct Page *upper = block + half;
        upper->property = half;
        SetPageProperty(upper);
        buddy_list_insert(upper, found);
        block->property = half;  // 低半块继续参与拆分
    }

    size_t block_pages = buddy_block_size(order);

    // 先清掉交付页的标志：块首页从此不再是空闲块首页，后面的"尾页归还"
    // 才不会被误判为可以合并的空闲块。
    for (struct Page *p = block; p != block + n; p++) {
        p->flags = 0;  // I5: 非保留、非空闲块首页
        p->property = 0;
        set_page_ref(p, 0);
    }

    // 尾页归还：[block+n, block+2^order) 是多取的内部浪费，按释放区间规则拆成
    // 2 的幂块挂回链表并参与合并。交付给调用方的恰好 n 页。
    buddy_nr_free -= n;
    if (n < block_pages) {
        buddy_release_range(block + n, block_pages - n);
    }
    return block;
}

// S4: 释放 [base, base+n)，非 2 的幂的 n 同样支持。
//lab2 buddy system 2414010
static void
buddy_free_pages(struct Page *base, size_t n) {
    assert(n > 0);
    assert(buddy_region_valid);
    assert(base >= buddy_region_base);
    assert(buddy_page_offset(base) + n <= buddy_region_pages);

    struct Page *p = base;
    for (; p != base + n; p++) {
        // 重复释放/释放保留页属于调用方错误。
        assert(!PageReserved(p) && !PageProperty(p));
        p->flags = 0;
        p->property = 0;
        set_page_ref(p, 0);
    }

    buddy_nr_free += n;
    // 非 2 的幂：由 buddy_release_range 按"尽量大且对齐"贪心拆块，每块插入后
    // 递归向上合并，因此全部释放后能回到 init_memmap 时的规范分解形态。
    buddy_release_range(base, n);
}

// S5: 当前空闲页总数。
//lab2 buddy system 2414010
static size_t
buddy_nr_free_pages(void) {
    return buddy_nr_free;
}

// 供 buddy_check 使用的探测尺寸，覆盖 1、3、5、非 2 的幂与一个大块。
static const size_t buddy_probe_sizes[] = {1, 3, 5, 6, 9, 17, 33, 1024};
#define BUDDY_CHECK_PROBES \
    (sizeof(buddy_probe_sizes) / sizeof(buddy_probe_sizes[0]))

// S6: 自检。只调用本模块的 static 实现，绝不用全局 alloc_pages/free_pages。
//lab2 buddy system 2414010
static void
buddy_check(void) {
    // 1) I1~I4
    buddy_verify_free_lists();

    // 2) 尚未接管区域时到此结束。
    if (!buddy_region_valid) {
        cprintf("buddy_check: (no managed region)\n");
        return;
    }

    // 3) 在真实空闲区上做往返验证。
    unsigned int blocks_snapshot[BUDDY_MAX_ORDER];
    for (unsigned int order = 0; order < BUDDY_MAX_ORDER; order++) {
        blocks_snapshot[order] = buddy_free_area[order].nr_free;
    }
    size_t nr_free_snapshot = buddy_nr_free;
    size_t expected = nr_free_snapshot;

    struct Page *probes[BUDDY_CHECK_PROBES];
    size_t probe_pages[BUDDY_CHECK_PROBES];
    unsigned int probe_count = 0;

    for (unsigned int i = 0; i < BUDDY_CHECK_PROBES; i++) {
        size_t n = buddy_probe_sizes[i];
        // 最大空闲块都装不下时，更大的尺寸必然也装不下。
        if (n > buddy_largest_free_block()) {
            break;
        }
        struct Page *page = buddy_alloc_pages(n);
        assert(page != NULL);

        unsigned int k = buddy_ceil_order(n);
        assert(k < BUDDY_MAX_ORDER);
        assert(page >= buddy_region_base);
        assert(buddy_page_offset(page) + n <= buddy_region_pages);
        // 返回值相对区域基址的偏移必须能被 2^k 整除。
        assert((buddy_page_offset(page) & (buddy_block_size(k) - 1)) == 0);
        // I5: 交付的 n 页都不是保留页、不是空闲块首页。
        for (size_t j = 0; j < n; j++) {
            assert(!PageReserved(page + j));
            assert(!PageProperty(page + j));
            assert(page[j].property == 0);
        }
        expected -= n;
        assert(buddy_nr_free == expected);  // nr_free 恰好只减少 n
        buddy_verify_free_lists();

        probes[probe_count] = page;
        probe_pages[probe_count] = n;
        probe_count++;
    }

    // 按与分配相反的顺序全部释放。
    while (probe_count > 0) {
        probe_count--;
        buddy_free_pages(probes[probe_count], probe_pages[probe_count]);
        expected += probe_pages[probe_count];
        assert(buddy_nr_free == expected);
        buddy_verify_free_lists();
    }

    // 4) 状态必须与进入 check 前完全一致：不泄漏、不多释放。
    assert(buddy_nr_free == nr_free_snapshot);
    for (unsigned int order = 0; order < BUDDY_MAX_ORDER; order++) {
        assert(buddy_free_area[order].nr_free == blocks_snapshot[order]);
    }
    cprintf("buddy_check() passed\n");
}

//lab2 buddy system 2414010
const struct pmm_manager buddy_pmm_manager = {
    .name = "buddy_pmm_manager",
    .init = buddy_init,
    .init_memmap = buddy_init_memmap,
    .alloc_pages = buddy_alloc_pages,
    .free_pages = buddy_free_pages,
    .nr_free_pages = buddy_nr_free_pages,
    .check = buddy_check,
};

// ---------------------------------------------------------------------------
// 只读调试接口
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
unsigned int
buddy_max_order(void) {
    return BUDDY_MAX_ORDER;
}

//lab2 buddy system 2414010
size_t
buddy_free_block_count(unsigned int order) {
    if (order >= BUDDY_MAX_ORDER) {
        return 0;
    }
    return (size_t)buddy_free_area[order].nr_free;
}

//lab2 buddy system 2414010
void
buddy_dump(void) {
    size_t total = 0;
    if (buddy_region_valid) {
        cprintf("buddy_pmm: max_order=%u, region_pages=%lu, nr_free=%lu\n",
                BUDDY_MAX_ORDER, (unsigned long)buddy_region_pages,
                (unsigned long)buddy_nr_free);
    } else {
        cprintf("buddy_pmm: max_order=%u, no managed region\n",
                BUDDY_MAX_ORDER);
    }
    for (unsigned int order = 0; order < BUDDY_MAX_ORDER; order++) {
        size_t size = buddy_block_size(order);
        unsigned int blocks = buddy_free_area[order].nr_free;
        total += size * blocks;
        cprintf("buddy_pmm: order %u (block=%lu pages): %u blocks\n", order,
                (unsigned long)size, blocks);
    }
    cprintf("buddy_pmm: free pages covered by lists = %lu\n",
            (unsigned long)total);
}
