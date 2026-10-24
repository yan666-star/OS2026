//lab2 buddy system 2414010
#include <assert.h>
#include <best_fit_pmm.h>
#include <buddy_pmm.h>
#include <buddy_test.h>
#include <defs.h>
#include <memlayout.h>
#include <pmm.h>
#include <stdio.h>

/*
 * Buddy System 单元测试（黑盒 + 不变式）
 *
 * 驱动方式：只调用 buddy_pmm_manager 的成员函数指针，绝不调用全局
 * alloc_pages()/free_pages()（它们会路由到内核当前注册的 best fit），
 * 也不修改全局 pmm_manager，因此对真实物理内存管理器零副作用。
 *
 * 测试区域：一块独立的静态 struct Page 数组 arena（BUDDY_TEST_PAGES 页）。
 * buddy 的全部地址运算都相对"区域基址"，不使用全局 pages/npage/物理地址，
 * 所以这块 arena 可以安全地反复 init/init_memmap，用例之间完全隔离。
 *
 * 判定方式：每个用例内部用 expect_* 收集断言，用例末尾统一打印一行
 * PASS / FAIL(<首个失败细节>)，失败不中断后续用例；最后汇总并 assert。
 */

#define BUDDY_TEST_PAGES        1024    // arena 页数（= 2^10，便于断言完全合并）
#define BUDDY_TEST_ORDER_CAP    32      // 防御性上限：buddy_max_order 不得超过
#define BUDDY_TEST_STRESS_RECS  512     // 压力测试中同时存活的分配记录上限
#define BUDDY_TEST_STRESS_ROUNDS 2000   // 压力测试轮数
#define BUDDY_TEST_LCG_SEED     20241023u

// ---------------------------------------------------------------------------
// 测试状态
// ---------------------------------------------------------------------------

static struct Page arena[BUDDY_TEST_PAGES];
static uint8_t used[BUDDY_TEST_PAGES];         // 影子位图：已分配页的真值来源
static struct Page *drain_pages[BUDDY_TEST_PAGES];

static int fail_count;                          // 失败的用例数
static int case_fail;                           // 当前用例是否已失败
static int fail_has_num;                        // 失败细节是数值型还是文本型
static const char *fail_text;
static unsigned long fail_got, fail_want;

static unsigned int head_counts[BUDDY_TEST_ORDER_CAP];

// ---------------------------------------------------------------------------
// 判定辅助
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static void
case_begin(void) {
    case_fail = 0;
    fail_has_num = 0;
    fail_text = "unknown";
    fail_got = fail_want = 0;
}

// 记录一条布尔断言；只保留用例内首个失败，保证 FAIL 行可读。
//lab2 buddy system 2414010
static void
expect_text(int cond, const char *detail) {
    if (!cond && !case_fail) {
        case_fail = 1;
        fail_has_num = 0;
        fail_text = detail;
    }
}

// 记录一条数值断言：got 必须等于 want。
//lab2 buddy system 2414010
static void
expect_num(const char *what, unsigned long got, unsigned long want) {
    if (got != want && !case_fail) {
        case_fail = 1;
        fail_has_num = 1;
        fail_text = what;
        fail_got = got;
        fail_want = want;
    }
}

// 每个用例打印且仅打印一行判定结果。
//lab2 buddy system 2414010
static void
case_end(const char *name) {
    if (case_fail) {
        fail_count++;
        if (fail_has_num) {
            cprintf("[Buddy Test] %s: FAIL (%s=%lu, expect=%lu)\n", name,
                    fail_text, fail_got, fail_want);
        } else {
            cprintf("[Buddy Test] %s: FAIL (%s)\n", name, fail_text);
        }
    } else {
        cprintf("[Buddy Test] %s: PASS\n", name);
    }
}

// ---------------------------------------------------------------------------
// 驱动封装：只走 buddy_pmm_manager 的成员与三个只读调试接口
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static size_t
bf_nr_free(void) {
    return buddy_pmm_manager.nr_free_pages();
}

//lab2 buddy system 2414010
static struct Page *
bf_alloc(size_t n) {
    return buddy_pmm_manager.alloc_pages(n);
}

//lab2 buddy system 2414010
static void
bf_free(struct Page *base, size_t n) {
    buddy_pmm_manager.free_pages(base, n);
}

//lab2 buddy system 2414010
static size_t
bf_count(unsigned int order) {
    return buddy_free_block_count(order);
}

//lab2 buddy system 2414010
static size_t
off_of(struct Page *p) {
    return (size_t)(p - arena);
}

// ceil(2^k) >= n：n 页请求对应的对齐粒度（C1/C3）。
//lab2 buddy system 2414010
static size_t
ceil_pow2(size_t n) {
    size_t size = 1;
    while (size < n) {
        size <<= 1;
    }
    return size;
}

// ---------------------------------------------------------------------------
// 夹具（F1~F4）
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static void
arena_reset(void) {
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i++) {
        arena[i].flags = 0;
        arena[i].property = 0;
        set_page_ref(&arena[i], 0);
        SetPageReserved(&arena[i]);   // init_memmap 的前置条件
    }
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i++) {
        used[i] = 0;
    }
    buddy_pmm_manager.init();
    buddy_pmm_manager.init_memmap(arena, BUDDY_TEST_PAGES);
}

// ---------------------------------------------------------------------------
// 不变式校验
// ---------------------------------------------------------------------------

// C7：通过只读调试接口交叉校验各阶块数之和 == nr_free。
//lab2 buddy system 2414010
static int
inv_sum_ok(void) {
    unsigned int max_order = buddy_max_order();
    if (max_order == 0 || max_order > BUDDY_TEST_ORDER_CAP) {
        return 0;
    }
    size_t total = 0;
    for (unsigned int k = 0; k < max_order; k++) {
        total += bf_count(k) * ((size_t)1 << k);
    }
    return total == bf_nr_free();
}

// 独立于调试接口的结构校验：直接扫描 arena 页的 flags/property。
// 校验点：块大小是 2 的幂、块相对基址对齐、块不越界、块内页不是块头、
//         空闲页总数 == nr_free、各阶块数与 buddy_free_block_count 一致。
//lab2 buddy system 2414010
static int
arena_invariant_ok(const char **msg) {
    unsigned int max_order = buddy_max_order();
    if (max_order == 0 || max_order > BUDDY_TEST_ORDER_CAP) {
        *msg = "buddy_max_order out of range";
        return 0;
    }
    for (unsigned int k = 0; k < max_order; k++) {
        head_counts[k] = 0;
    }

    size_t sum = 0;
    size_t i = 0;
    while (i < BUDDY_TEST_PAGES) {
        if (PageProperty(&arena[i])) {
            size_t size = arena[i].property;
            if (size == 0 || (size & (size - 1)) != 0) {
                *msg = "free block size is not a power of two";
                return 0;
            }
            if ((i % size) != 0) {
                *msg = "free block is not aligned to its size";
                return 0;
            }
            if (i + size > BUDDY_TEST_PAGES) {
                *msg = "free block exceeds the region";
                return 0;
            }
            for (size_t j = 1; j < size; j++) {
                if (PageProperty(&arena[i + j]) || arena[i + j].property != 0) {
                    *msg = "page inside a free block is marked as a block head";
                    return 0;
                }
            }
            unsigned int k = 0;
            while (((size_t)1 << k) < size) {
                k++;
            }
            if (k >= max_order) {
                *msg = "free block order exceeds max_order";
                return 0;
            }
            head_counts[k]++;
            sum += size;
            i += size;
        } else {
            i++;
        }
    }
    if (sum != bf_nr_free()) {
        *msg = "sum of free block sizes != nr_free";
        return 0;
    }
    for (unsigned int k = 0; k < max_order; k++) {
        if ((size_t)head_counts[k] != bf_count(k)) {
            *msg = "buddy_free_block_count disagrees with page flags";
            return 0;
        }
    }
    return 1;
}

//lab2 buddy system 2414010
static void
expect_invariant(void) {
    const char *msg = "unknown";
    expect_text(arena_invariant_ok(&msg), msg);
}

// 影子位图：[off, off+n) 是否全为 val；越界返回 0。
//lab2 buddy system 2414010
static int
range_all_used(size_t off, size_t n, int val) {
    if (off > BUDDY_TEST_PAGES || n > BUDDY_TEST_PAGES - off) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if ((int)used[off + i] != val) {
            return 0;
        }
    }
    return 1;
}

//lab2 buddy system 2414010
static void
set_range_used(size_t off, size_t n, int val) {
    for (size_t i = 0; i < n; i++) {
        used[off + i] = (uint8_t)val;
    }
}

//lab2 buddy system 2414010
static int
used_all_zero(void) {
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i++) {
        if (used[i] != 0) {
            return 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 状态快照：用于校验"失败分支零副作用"（C5）
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
struct state_snapshot {
    size_t nr;
    size_t counts[BUDDY_TEST_ORDER_CAP];
    unsigned long sum;
    unsigned long xorsum;
};

//lab2 buddy system 2414010
static void
snapshot_take(struct state_snapshot *s) {
    unsigned int max_order = buddy_max_order();
    s->nr = bf_nr_free();
    for (unsigned int k = 0; k < BUDDY_TEST_ORDER_CAP; k++) {
        s->counts[k] = (k < max_order) ? bf_count(k) : 0;
    }
    unsigned long sum = 0, xr = 0;
    for (size_t i = 0; i < BUDDY_TEST_PAGES; i++) {
        unsigned long f = (unsigned long)arena[i].flags;
        unsigned long pr = (unsigned long)arena[i].property;
        sum += f * 31u + pr * 7u;
        xr ^= (f << 5) ^ pr ^ (unsigned long)i;
    }
    s->sum = sum;
    s->xorsum = xr;
}

//lab2 buddy system 2414010
static int
snapshot_equal(const struct state_snapshot *a, const struct state_snapshot *b) {
    if (a->nr != b->nr || a->sum != b->sum || a->xorsum != b->xorsum) {
        return 0;
    }
    for (unsigned int k = 0; k < BUDDY_TEST_ORDER_CAP; k++) {
        if (a->counts[k] != b->counts[k]) {
            return 0;
        }
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 确定性伪随机（固定 seed，保证可复现）
// ---------------------------------------------------------------------------

static uint32_t lcg_state;

//lab2 buddy system 2414010
static uint32_t
lcg_next(void) {
    lcg_state = lcg_state * 1103515245u + 12345u;
    return (lcg_state >> 8) & 0xFFFFu;
}

// ---------------------------------------------------------------------------
// T1 初始化
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static void
test_t1_init(void) {
    case_begin();
    arena_reset();

    expect_num("nr_free", (unsigned long)bf_nr_free(), BUDDY_TEST_PAGES);
    expect_text(buddy_max_order() >= 11, "buddy_max_order < 11");

    unsigned int max_order = buddy_max_order();
    if (max_order == 0 || max_order > BUDDY_TEST_ORDER_CAP) {
        expect_text(0, "buddy_max_order out of range");
        case_end("T1 init");
        return;
    }
    for (unsigned int k = 0; k < max_order; k++) {
        expect_num("count", (unsigned long)bf_count(k), (k == 10) ? 1 : 0);
    }
    expect_num("arena[0].property", (unsigned long)arena[0].property,
               BUDDY_TEST_PAGES);
    expect_text(PageProperty(&arena[0]), "arena[0] is not a free block head");

    int clean = 1;
    for (size_t i = 1; i < BUDDY_TEST_PAGES; i++) {
        if (PageProperty(&arena[i])) {
            clean = 0;
        }
    }
    expect_text(clean, "a non-head page carries PageProperty");

    expect_text(inv_sum_ok(), "sum over orders != nr_free");
    expect_invariant();
    case_end("T1 init");
}

// ---------------------------------------------------------------------------
// T2 单页分配与拆分链（1023 = 512+256+128+64+32+16+8+4+2+1）
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static void
test_t2_split_chain(void) {
    case_begin();
    arena_reset();

    struct Page *p = bf_alloc(1);
    expect_text(p != NULL, "alloc(1) returned NULL");
    if (p != NULL) {
        expect_num("nr_free", (unsigned long)bf_nr_free(), BUDDY_TEST_PAGES - 1);
        unsigned int max_order = buddy_max_order();
        for (unsigned int k = 0; k < max_order && k < 10; k++) {
            expect_num("count(k<10)", (unsigned long)bf_count(k), 1);
        }
        expect_num("count(10)", (unsigned long)bf_count(10), 0);
        expect_text(inv_sum_ok(), "sum over orders != nr_free");
        expect_invariant();
        expect_text(!PageProperty(p), "allocated page is still a free head");
        expect_num("allocated property", (unsigned long)p->property, 0);

        bf_free(p, 1);
        expect_num("nr_free after free", (unsigned long)bf_nr_free(),
                   BUDDY_TEST_PAGES);
        expect_num("count(10) after free", (unsigned long)bf_count(10), 1);
        expect_invariant();
    }
    case_end("T2 split-chain");
}

// ---------------------------------------------------------------------------
// T3 非 2 的幂分配（尾页归还）：n = 3 -> 阶 2，尾页 1 页归还
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static void
test_t3_non_power_of_two(void) {
    case_begin();
    arena_reset();

    struct Page *p = bf_alloc(3);
    expect_text(p != NULL, "alloc(3) returned NULL");
    if (p != NULL) {
        expect_num("nr_free", (unsigned long)bf_nr_free(), BUDDY_TEST_PAGES - 3);
        expect_num("offset % 4", (unsigned long)(off_of(p) % 4), 0);

        int clean = 1;
        for (size_t i = 0; i < 3; i++) {
            if (PageProperty(p + i) || p[i].property != 0) {
                clean = 0;
            }
        }
        expect_text(clean, "a delivered page is still marked free");

        expect_num("count(0)", (unsigned long)bf_count(0), 1);
        expect_num("count(1)", (unsigned long)bf_count(1), 0);
        for (unsigned int k = 2; k < 10; k++) {
            expect_num("count(2..9)", (unsigned long)bf_count(k), 1);
        }
        expect_num("count(10)", (unsigned long)bf_count(10), 0);
        expect_text(inv_sum_ok(), "sum over orders != nr_free");
        expect_invariant();

        bf_free(p, 3);
        expect_num("nr_free after free", (unsigned long)bf_nr_free(),
                   BUDDY_TEST_PAGES);
        expect_num("count(10) after free", (unsigned long)bf_count(10), 1);
        unsigned int max_order = buddy_max_order();
        for (unsigned int k = 0; k < max_order && k < 10; k++) {
            expect_num("count(k<10) after free", (unsigned long)bf_count(k), 0);
        }
        expect_invariant();
    }
    case_end("T3 non-power-of-two");
}

// ---------------------------------------------------------------------------
// T4 伙伴合并：两次 256 页分配后释放，验证中间态不合并、最终合并回整块
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static void
test_t4_buddy_merge(void) {
    case_begin();
    arena_reset();

    struct Page *a = bf_alloc(256);
    struct Page *b = bf_alloc(256);
    expect_text(a != NULL, "alloc(256) #1 returned NULL");
    expect_text(b != NULL, "alloc(256) #2 returned NULL");
    if (a != NULL && b != NULL) {
        size_t oa = off_of(a), ob = off_of(b);
        size_t lo = (oa < ob) ? oa : ob;
        size_t hi = (oa < ob) ? ob : oa;
        expect_num("|oa-ob|", (unsigned long)(hi - lo), 256);
        expect_num("lo % 256", (unsigned long)(lo % 256), 0);
        expect_num("lo ^ 256", (unsigned long)(lo ^ 256), (unsigned long)(lo + 256));

        bf_free(b, 256);
        expect_num("nr_free(mid)", (unsigned long)bf_nr_free(),
                   BUDDY_TEST_PAGES - 256);
        expect_num("count(8)", (unsigned long)bf_count(8), 1);
        expect_num("count(9)", (unsigned long)bf_count(9), 1);
        expect_num("count(10)", (unsigned long)bf_count(10), 0);
        expect_invariant();

        bf_free(a, 256);
        expect_num("nr_free(end)", (unsigned long)bf_nr_free(),
                   BUDDY_TEST_PAGES);
        expect_num("count(10)", (unsigned long)bf_count(10), 1);
        expect_invariant();
    }
    case_end("T4 buddy-merge");
}

// ---------------------------------------------------------------------------
// T5 分配失败：返回 NULL 且状态逐字节不变
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static void
test_t5_alloc_fail(void) {
    case_begin();
    arena_reset();

    struct state_snapshot s0, s1;
    snapshot_take(&s0);
    struct Page *p = bf_alloc(2048);
    expect_text(p == NULL, "alloc(2048) should fail on a 1024-page region");
    snapshot_take(&s1);
    expect_text(snapshot_equal(&s0, &s1), "state changed after failed alloc");

    struct Page *q = bf_alloc(BUDDY_TEST_PAGES);
    expect_text(q != NULL, "alloc(1024) returned NULL");
    if (q != NULL) {
        expect_num("nr_free", (unsigned long)bf_nr_free(), 0);
        unsigned int max_order = buddy_max_order();
        for (unsigned int k = 0; k < max_order; k++) {
            expect_num("count(all)", (unsigned long)bf_count(k), 0);
        }
        struct state_snapshot s2, s3;
        snapshot_take(&s2);
        struct Page *r = bf_alloc(1);
        expect_text(r == NULL, "alloc(1) should fail when nr_free == 0");
        snapshot_take(&s3);
        expect_text(snapshot_equal(&s2, &s3), "state changed after failed alloc");

        bf_free(q, BUDDY_TEST_PAGES);
        expect_num("nr_free after free", (unsigned long)bf_nr_free(),
                   BUDDY_TEST_PAGES);
        expect_num("count(10) after free", (unsigned long)bf_count(10), 1);
        expect_invariant();
    }
    case_end("T5 alloc-fail");
}

// ---------------------------------------------------------------------------
// T6 边界：整区分配、最小分配
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static void
test_t6_boundary(void) {
    case_begin();
    arena_reset();

    struct Page *p = bf_alloc(BUDDY_TEST_PAGES);
    expect_text(p != NULL, "alloc(1024) returned NULL");
    if (p != NULL) {
        expect_num("offset", (unsigned long)off_of(p), 0);
        expect_num("nr_free", (unsigned long)bf_nr_free(), 0);
        bf_free(p, BUDDY_TEST_PAGES);
        expect_num("count(10)", (unsigned long)bf_count(10), 1);
        expect_num("nr_free after free", (unsigned long)bf_nr_free(),
                   BUDDY_TEST_PAGES);
        expect_invariant();
    }

    struct Page *r = bf_alloc(1);
    expect_text(r != NULL, "alloc(1) returned NULL");
    if (r != NULL) {
        expect_num("nr_free", (unsigned long)bf_nr_free(), BUDDY_TEST_PAGES - 1);
        bf_free(r, 1);
        expect_num("nr_free after free", (unsigned long)bf_nr_free(),
                   BUDDY_TEST_PAGES);
        expect_num("count(10) after free", (unsigned long)bf_count(10), 1);
        expect_invariant();
    }
    case_end("T6 boundary");
}

// ---------------------------------------------------------------------------
// T7 穷尽分配（drain）：空闲链表能否被完整拆到 1 页，再合并回整块
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static void
test_t7_drain(void) {
    case_begin();
    arena_reset();

    static const size_t pre_sizes[] = {1, 2, 5, 16};
#define PRE_COUNT (sizeof(pre_sizes) / sizeof(pre_sizes[0]))
    struct Page *pre_pages[PRE_COUNT];

    for (size_t i = 0; i < PRE_COUNT; i++) {
        pre_pages[i] = bf_alloc(pre_sizes[i]);
        if (pre_pages[i] == NULL) {
            expect_text(0, "pre-alloc returned NULL");
            case_end("T7 drain");
            return;
        }
        set_range_used(off_of(pre_pages[i]), pre_sizes[i], 1);
    }
    expect_invariant();

    size_t n0 = bf_nr_free();
    cprintf("[Buddy Test] T7 drain: draining %lu free pages\n",
            (unsigned long)n0);
    buddy_dump();

    size_t drained = 0;
    int fatal = 0;
    while (drained < BUDDY_TEST_PAGES) {
        struct Page *x = bf_alloc(1);
        if (x == NULL) {
            break;
        }
        size_t off = off_of(x);
        if (off >= BUDDY_TEST_PAGES) {
            expect_text(0, "drain returned an out-of-range page");
            fatal = 1;
            break;
        }
        if (used[off] != 0) {
            expect_text(0, "drain returned an already allocated page");
            fatal = 1;
            break;
        }
        used[off] = 1;
        drain_pages[drained++] = x;
        if ((drained % 64) == 0) {
            expect_text(inv_sum_ok(), "sum over orders != nr_free");
        }
    }
    expect_num("drained count", (unsigned long)drained, (unsigned long)n0);
    expect_num("nr_free", (unsigned long)bf_nr_free(), 0);

    if (!fatal) {
        // 按地址升序释放，验证逐级合并。
        for (size_t i = 1; i < drained; i++) {   // 插入排序
            struct Page *key = drain_pages[i];
            size_t j = i;
            while (j > 0 && off_of(drain_pages[j - 1]) > off_of(key)) {
                drain_pages[j] = drain_pages[j - 1];
                j--;
            }
            drain_pages[j] = key;
        }
        for (size_t i = 0; i < drained; i++) {
            size_t off = off_of(drain_pages[i]);
            bf_free(drain_pages[i], 1);
            used[off] = 0;
        }
        for (size_t i = PRE_COUNT; i > 0; i--) {
            size_t idx = i - 1;
            bf_free(pre_pages[idx], pre_sizes[idx]);
            set_range_used(off_of(pre_pages[idx]), pre_sizes[idx], 0);
        }
        expect_num("nr_free after free all", (unsigned long)bf_nr_free(),
                   BUDDY_TEST_PAGES);
        expect_num("count(10) after free all", (unsigned long)bf_count(10), 1);
        expect_text(used_all_zero(), "shadow bitmap not fully cleared");
        expect_invariant();
    }
    case_end("T7 drain");
}

// ---------------------------------------------------------------------------
// T8 随机压力测试（确定性 LCG + 影子位图）
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
static const size_t stress_sizes[16] = {1,   2,   3,   5,   7,   8,   16,  17,
                                        33,  64,  100, 256, 511, 512, 1024,
                                        2048};

//lab2 buddy system 2414010
struct stress_rec {
    struct Page *base;
    size_t n;
};
static struct stress_rec recs[BUDDY_TEST_STRESS_RECS];

//lab2 buddy system 2414010
static void
test_t8_stress(void) {
    case_begin();
    arena_reset();
    lcg_state = BUDDY_TEST_LCG_SEED;

    size_t live = 0;
    size_t live_pages = 0;
    int fatal = 0;

    for (unsigned int round = 0; round < BUDDY_TEST_STRESS_ROUNDS && !fatal;
         round++) {
        if (round == BUDDY_TEST_STRESS_ROUNDS / 2) {
            cprintf("[Buddy Test] T8 stress: round %u\n", round);
            buddy_dump();
        }

        uint32_t r1 = lcg_next();
        int do_alloc;
        if (live == 0) {
            do_alloc = 1;
        } else if (live >= BUDDY_TEST_STRESS_RECS) {
            do_alloc = 0;
        } else {
            do_alloc = (int)(r1 & 1u);
        }

        if (do_alloc) {
            size_t n = stress_sizes[lcg_next() % 16];
            struct state_snapshot before;
            snapshot_take(&before);
            struct Page *p = bf_alloc(n);
            if (p == NULL) {
                struct state_snapshot after;
                snapshot_take(&after);
                expect_text(snapshot_equal(&before, &after),
                            "state changed after failed alloc");
            } else {
                size_t off = off_of(p);
                if (off >= BUDDY_TEST_PAGES || n > BUDDY_TEST_PAGES - off) {
                    expect_text(0, "alloc returned an out-of-range block");
                    fatal = 1;
                    break;
                }
                expect_text(range_all_used(off, n, 0),
                            "alloc handed out an already allocated page (C1/C3)");
                expect_num("offset % ceil_pow2(n)",
                           (unsigned long)(off % ceil_pow2(n)), 0);
                expect_num("nr_free delta",
                           (unsigned long)(before.nr - bf_nr_free()),
                           (unsigned long)n);
                expect_text(inv_sum_ok(), "sum over orders != nr_free");

                if (live < BUDDY_TEST_STRESS_RECS) {
                    recs[live].base = p;
                    recs[live].n = n;
                    live++;
                    live_pages += n;
                    set_range_used(off, n, 1);
                } else {
                    bf_free(p, n);   // 记录槽位已满，立即归还
                }
            }
        } else {
            size_t idx = (size_t)(lcg_next() % (uint32_t)live);
            struct Page *base = recs[idx].base;
            size_t n = recs[idx].n;
            size_t off = off_of(base);
            if (!range_all_used(off, n, 1)) {
                expect_text(0, "freeing a block that is not fully allocated");
                fatal = 1;
                break;
            }
            size_t nr_before = bf_nr_free();
            bf_free(base, n);
            expect_num("nr_free delta", (unsigned long)(bf_nr_free() - nr_before),
                       (unsigned long)n);
            expect_text(inv_sum_ok(), "sum over orders != nr_free");
            set_range_used(off, n, 0);
            live_pages -= n;
            recs[idx] = recs[live - 1];
            live--;
        }

        if ((round % 100) == 0) {
            expect_num("free + live", (unsigned long)(bf_nr_free() + live_pages),
                       BUDDY_TEST_PAGES);
            expect_invariant();
        }
    }

    while (live > 0) {
        live--;
        size_t off = off_of(recs[live].base);
        bf_free(recs[live].base, recs[live].n);
        set_range_used(off, recs[live].n, 0);
        live_pages -= recs[live].n;
    }
    expect_num("nr_free after release all", (unsigned long)bf_nr_free(),
               BUDDY_TEST_PAGES);
    expect_num("count(10) after release all", (unsigned long)bf_count(10), 1);
    expect_num("live pages", (unsigned long)live_pages, 0);
    expect_text(used_all_zero(), "shadow bitmap not fully cleared");
    expect_invariant();
    case_end("T8 stress");
}

// ---------------------------------------------------------------------------
// T9 零副作用（在 buddy_test 中完成，不在此处驱动 buddy）
// ---------------------------------------------------------------------------

//lab2 buddy system 2414010
void
buddy_test(void) {
    size_t global_free_before = nr_free_pages();
    const struct pmm_manager *pm_before = pmm_manager;

    cprintf("[Buddy Test] start: arena = %lu pages, max_order = %u\n",
            (unsigned long)BUDDY_TEST_PAGES, buddy_max_order());

    test_t1_init();
    test_t2_split_chain();
    test_t3_non_power_of_two();
    test_t4_buddy_merge();
    test_t5_alloc_fail();
    test_t6_boundary();
    test_t7_drain();
    test_t8_stress();

    // T9：真实内存管理器必须完全不受影响。
    case_begin();
    expect_text(pmm_manager == pm_before, "pmm_manager was replaced");
    expect_text(pm_before == &best_fit_pmm_manager,
                "pmm_manager is not best_fit");
    expect_num("global nr_free_pages", (unsigned long)nr_free_pages(),
               (unsigned long)global_free_before);
    case_end("T9 no-side-effect");

    if (fail_count == 0) {
        cprintf("[Buddy Test] ALL PASS\n");
    } else {
        cprintf("[Buddy Test] FAILED: %d case(s)\n", fail_count);
    }

    // 汇总已打印，再让模块自身做一次往返自检；最后释放 arena。
    buddy_pmm_manager.check();
    buddy_pmm_manager.init();

    assert(fail_count == 0);
}
