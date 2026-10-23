#include <slub_test.h>

#include <assert.h>
#include <best_fit_pmm.h>
#include <defs.h>
#include <mmu.h>
#include <pmm.h>
#include <slub.h>
#include <stdio.h>

/*
 * SLUB 单元测试（黑盒 + 影子记录）
 * challenge task 2414010
 * 驱动方式：只调用 kmalloc/kfree 与 slub.h 的只读统计接口、第一层的
 * nr_free_pages()；绝不触碰 SLUB 与页分配器的内部数据结构。
 *
 * 判定方式：每个用例用 expect_* 收集断言（只记录首个失败），用例末尾
 * 统一打印一行 PASS / FAIL(<关键值>)；失败不中断后续用例；最后汇总，
 * 有失败则 assert 停住内核。
 *
 * 隔离方式：每个用例开头记 base_free = nr_free_pages()，结尾把本用例
 * 分配的对象全部 kfree，并断言 nr_free_pages() 回到 base_free。
 *
 * 档表只通过 slub_class_count()/slub_class_size(idx) 查询（R1），
 * 不硬编码 8/16/.../2048。
 *
 * 负面行为（重复 kfree / 非法指针 / 越界写）会让实现侧 panic 终止内核，
 * 无法在自动化用例里继续运行，因此不纳入用例（R3）；越界踩踏改由
 * T5 的整区间 pattern 回读间接检测（R4：覆盖 [ptr, ptr+size) 全部字节）。
 */

#define SLUB_TEST_MAX_OBJS      512     // 影子记录表容量（> 压力测试存活上限）
#define SLUB_TEST_STRESS_ROUNDS 2000    // T8 轮数
#define SLUB_TEST_LIVE_CAP      256     // T8 存活对象上限（recs/池容量保护）
#define SLUB_TEST_LARGE_CAP     96      // T8 存活大对象上限（大对象表保护）
#define SLUB_TEST_LCG_SEED      20241023u

// ---------------------------------------------------------------------------
// 判定辅助（G3）：每用例只记录首个失败，末尾打印一行
// ---------------------------------------------------------------------------

static int fail_count;      // 失败用例总数
static int case_fail;       // 当前用例是否已失败
static int fail_has_num;    // 失败细节是数值型还是文本型
static const char *fail_text;
static unsigned long fail_got, fail_want;

static void
case_begin(void) {
    case_fail = 0;
    fail_has_num = 0;
    fail_text = "unknown";
    fail_got = fail_want = 0;
}

// 记录一条布尔断言；只保留用例内首个失败，保证 FAIL 行可读。
static void
expect_text(int cond, const char *detail) {
    if (!cond && !case_fail) {
        case_fail = 1;
        fail_has_num = 0;
        fail_text = detail;
    }
}

// 记录一条数值断言；只保留用例内首个失败。
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

// 用例收尾：打印且仅打印一行判定；失败计入 fail_count。
static void
case_end(const char *name) {
    if (!case_fail) {
        cprintf("[SLUB Test] %s: PASS\n", name);
    } else {
        fail_count++;
        if (fail_has_num) {
            cprintf("[SLUB Test] %s: FAIL (%s: got=%lu want=%lu)\n",
                    name, fail_text, fail_got, fail_want);
        } else {
            cprintf("[SLUB Test] %s: FAIL (%s)\n", name, fail_text);
        }
    }
}

// ---------------------------------------------------------------------------
// 随机数（确定性 LCG，固定 seed）
// ---------------------------------------------------------------------------

static uint32_t lcg_state = SLUB_TEST_LCG_SEED;

static uint32_t
lcg_next(void) {
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

// ---------------------------------------------------------------------------
// 档查询（R1：只用只读接口，不硬编码档表）
// ---------------------------------------------------------------------------

// 覆盖 size 的最小档下标；前置 1 <= size <= 最大档大小。
static unsigned int
class_of(size_t size) {
    unsigned int count = slub_class_count();
    for (unsigned int i = 0; i < count; i++) {
        if (slub_class_size(i) >= size) {
            return i;
        }
    }
    return count;  // size 超过最大档（调用方走大对象分支，不会用到）
}

// ---------------------------------------------------------------------------
// 影子记录表 + 区间重叠检测（G4）
// ---------------------------------------------------------------------------

struct obj_rec {
    void *ptr;
    size_t size;        // 请求大小（pattern 覆盖 [ptr, ptr+size)，R4）
    uint32_t seed;
    int live;
    unsigned int cls;   // 小对象所属档；大对象无效
    int is_large;
};

static struct obj_rec recs[SLUB_TEST_MAX_OBJS];

// 清空影子表（用例隔离：上一用例理论上已全部释放，这里防御性复位）。
static void
recs_clear(void) {
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        recs[i].live = 0;
        recs[i].ptr = NULL;
        recs[i].size = 0;
    }
}

static int
ranges_overlap(void *a, size_t asz, void *b, size_t bsz) {
    uintptr_t a0 = (uintptr_t)a, a1 = a0 + asz;
    uintptr_t b0 = (uintptr_t)b, b1 = b0 + bsz;
    return a0 < b1 && b0 < a1;
}

// ---------------------------------------------------------------------------
// 内容校验（R4：确定性 pattern 覆盖对象整个 [ptr, ptr+size) 区间）
// ---------------------------------------------------------------------------

static uint8_t
pat_byte(uint32_t seed, size_t i) {
    uint32_t v = seed * 2654435761u + (uint32_t)i * 40503u;
    return (uint8_t)(v >> 16);
}

static void
fill_pattern(void *ptr, size_t size, uint32_t seed) {
    uint8_t *p = ptr;
    for (size_t i = 0; i < size; i++) {
        p[i] = pat_byte(seed, i);
    }
}

// 返回首个不一致字节下标；完全一致返回 -1。
static long
pattern_mismatch(void *ptr, size_t size, uint32_t seed) {
    uint8_t *p = ptr;
    for (size_t i = 0; i < size; i++) {
        if (p[i] != pat_byte(seed, i)) {
            return (long)i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// 登记分配（重叠检测 + 填 pattern），返回记录下标；失败记入当前用例。
// ---------------------------------------------------------------------------

static int
rec_register(void *ptr, size_t size, uint32_t seed, unsigned int cls,
             int is_large) {
    int slot = -1;
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (!recs[i].live) {
            slot = i;
            break;
        }
    }
    expect_text(slot >= 0, "shadow table exhausted");
    if (slot < 0) {
        return -1;
    }

    // C3：与所有存活区间两两比对。
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (recs[i].live && ranges_overlap(ptr, size, recs[i].ptr,
                                           recs[i].size)) {
            expect_text(0, "object ranges overlap");
            return -1;
        }
    }

    // C1：至少 8 字节对齐；档路径按档对齐；大对象页对齐。
    expect_text(((uintptr_t)ptr % 8) == 0, "ptr not 8-byte aligned");
    if (is_large) {
        expect_text(((uintptr_t)ptr % PGSIZE) == 0, "large ptr not page aligned");
    } else {
        expect_text(((uintptr_t)ptr % slub_class_size(cls)) == 0,
                    "ptr not class-size aligned");
    }

    fill_pattern(ptr, size, seed);
    recs[slot].ptr = ptr;
    recs[slot].size = size;
    recs[slot].seed = seed;
    recs[slot].cls = cls;
    recs[slot].is_large = is_large;
    recs[slot].live = 1;
    return slot;
}

// 释放前校验 pattern 并注销区间；返回 0 正常。
static int
rec_release(int slot) {
    if (!recs[slot].live) {
        expect_text(0, "release of dead record");
        return -1;
    }
    long bad = pattern_mismatch(recs[slot].ptr, recs[slot].size,
                                recs[slot].seed);
    expect_text(bad < 0, "pattern corrupted before kfree");
    kfree(recs[slot].ptr);
    recs[slot].live = 0;
    recs[slot].ptr = NULL;
    recs[slot].size = 0;
    if (bad >= 0) {
        return -1;
    }
    return 0;
}

// 影子统计校验：每档已分配对象数与大对象数必须与只读接口一致。
static void
verify_shadow_counts(void) {
    unsigned int count = slub_class_count();
    for (unsigned int c = 0; c < count; c++) {
        unsigned long shadow = 0;
        for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
            if (recs[i].live && !recs[i].is_large && recs[i].cls == c) {
                shadow++;
            }
        }
        expect_num("slub_objects_of_class", slub_objects_of_class(c), shadow);
    }
    unsigned long shadow_large = 0;
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (recs[i].live && recs[i].is_large) {
            shadow_large++;
        }
    }
    expect_num("slub_large_count", slub_large_count(), shadow_large);
}

// 释放影子表中全部存活对象（用例收尾）。
static void
recs_release_all(void) {
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (recs[i].live) {
            (void)rec_release(i);
        }
    }
}

static int
recs_live_count(void) {
    int n = 0;
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (recs[i].live) {
            n++;
        }
    }
    return n;
}

static int
recs_live_large_count(void) {
    int n = 0;
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (recs[i].live && recs[i].is_large) {
            n++;
        }
    }
    return n;
}

// ---------------------------------------------------------------------------
// T1 初始化与零分配
// ---------------------------------------------------------------------------

static void
test_t1_init(void) {
    size_t base_free = nr_free_pages();
    case_begin();

    // kmalloc(0) 必须返回 NULL（C1）。
    expect_text(kmalloc(0) == NULL, "kmalloc(0) != NULL");

    // kfree(NULL) 无副作用（C9）。
    kfree(NULL);
    expect_num("free pages after kfree(NULL)", nr_free_pages(), base_free);

    // 进入测试时无人分配过大对象。
    expect_num("slub_large_count at entry", slub_large_count(), 0);

    // 档表非空且单调不减（C2/C10）。
    unsigned int count = slub_class_count();
    expect_text(count >= 1, "slub_class_count() == 0");
    for (unsigned int i = 1; i < count; i++) {
        expect_text(slub_class_size(i) >= slub_class_size(i - 1),
                    "class sizes not monotonic");
    }
    // 非法下标返回 0（G2 契约）。
    expect_num("class_size(invalid idx)", slub_class_size(count), 0);

    recs_release_all();
    expect_num("free pages at end", nr_free_pages(), base_free);
    case_end("T1 init");
}

// ---------------------------------------------------------------------------
// T2 各档基本分配（覆盖 1,7,8,9,16,31,32,100,128,300,1000,2000）
// ---------------------------------------------------------------------------

static void
test_t2_per_class(void) {
    static const size_t sizes[] = {1, 7, 8, 9, 16, 31, 32,
                                   100, 128, 300, 1000, 2000};
    const size_t n = sizeof(sizes) / sizeof(sizes[0]);
    size_t base_free = nr_free_pages();
    case_begin();
    recs_clear();

    // 全部分配 + 填 pattern（登记内部做重叠/对齐检查）。
    for (size_t i = 0; i < n; i++) {
        void *p = kmalloc(sizes[i]);
        expect_text(p != NULL, "kmalloc returned NULL");
        if (p == NULL) {
            continue;
        }
        unsigned int cls = class_of(sizes[i]);
        (void)rec_register(p, sizes[i], (uint32_t)(i + 1), cls, 0);
        // 该档至少有一个已分配对象（C2/C10）。
        expect_text(slub_objects_of_class(cls) >= 1,
                    "slub_objects_of_class still 0");
    }

    // 全部写完后再统一回读（C4：相邻槽位互相覆写在这里暴露）。
    for (size_t i = 0; i < n; i++) {
        int slot = -1;
        for (int j = 0; j < SLUB_TEST_MAX_OBJS; j++) {
            if (recs[j].live && recs[j].size == sizes[i] &&
                (uintptr_t)recs[j].seed == i + 1) {
                slot = j;
                break;
            }
        }
        if (slot >= 0) {
            expect_text(pattern_mismatch(recs[slot].ptr, recs[slot].size,
                                         recs[slot].seed) < 0,
                        "pattern mismatch (overwrite)");
        }
    }

    // 全部释放：档计数与 slab 数必须归 0（I4），页回到基线（C6）。
    recs_release_all();
    for (size_t i = 0; i < n; i++) {
        unsigned int cls = class_of(sizes[i]);
        expect_num("objects after kfree", slub_objects_of_class(cls), 0);
        expect_num("slabs after kfree", slub_slabs_of_class(cls), 0);
    }
    expect_num("free pages at end", nr_free_pages(), base_free);
    case_end("T2 per-class");
}

// ---------------------------------------------------------------------------
// T3 同档批量与 slab 复用（100 个 64B 对象）
// ---------------------------------------------------------------------------

static void
test_t3_slab_reuse(void) {
    const size_t obj_size = 64;
    const size_t total = 100, half = 50;
    size_t base_free = nr_free_pages();
    case_begin();
    recs_clear();

    unsigned int cls = class_of(obj_size);
    size_t class_size = slub_class_size(cls);
    size_t objs_per_slab = PGSIZE / class_size;
    unsigned long want_slabs = (total + objs_per_slab - 1) / objs_per_slab;

    // 第一轮：100 个 64B 对象。
    for (size_t i = 0; i < total; i++) {
        void *p = kmalloc(obj_size);
        expect_text(p != NULL, "kmalloc returned NULL");
        if (p != NULL) {
            (void)rec_register(p, obj_size, (uint32_t)(i + 1), cls, 0);
        }
    }
    expect_num("objects after 100 allocs", slub_objects_of_class(cls), total);
    expect_num("slabs after 100 allocs", slub_slabs_of_class(cls), want_slabs);
    // 分配后 2 页被 slab 持有；"不回升"指部分释放后保持这一水平（C6），
    // 而不是回到 base_free——slab 仍被 cache 持有。
    size_t free_after_alloc = nr_free_pages();

    // 释放前 50 个（两页各自仍有存活对象，页不归还）。
    int released = 0;
    for (int i = 0; i < SLUB_TEST_MAX_OBJS && released < (int)half; i++) {
        if (recs[i].live && !recs[i].is_large && recs[i].cls == cls &&
            recs[i].seed <= half) {
            (void)rec_release(i);
            released++;
        }
    }
    expect_num("objects after 50 kfrees", slub_objects_of_class(cls), half);
    // C6：只释放一部分对象时页仍被 cache 持有，nr_free 不回升
    // （保持在分配后的水平，比 base_free 少 want_slabs 页）。
    expect_num("free pages after partial kfree", nr_free_pages(),
               free_after_alloc);

    // 第二轮：再分配 50 个，应完全复用空闲槽位，不新增 slab（C7）。
    for (size_t i = 0; i < half; i++) {
        void *p = kmalloc(obj_size);
        expect_text(p != NULL, "kmalloc returned NULL (reuse)");
        if (p != NULL) {
            (void)rec_register(p, obj_size, (uint32_t)(total + i + 1), cls, 0);
        }
    }
    expect_num("objects after re-alloc", slub_objects_of_class(cls), total);
    expect_num("slabs after re-alloc", slub_slabs_of_class(cls), want_slabs);

    // 第一轮幸存的 50 个对象内容不得被第二轮覆写（C4）。
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (recs[i].live && !recs[i].is_large &&
            recs[i].seed > half && recs[i].seed <= total) {
            expect_text(pattern_mismatch(recs[i].ptr, recs[i].size,
                                         recs[i].seed) < 0,
                        "survivor pattern corrupted");
        }
    }

    // 全部释放：slab 数归 0，页回基线。
    recs_release_all();
    expect_num("objects at end", slub_objects_of_class(cls), 0);
    expect_num("slabs at end", slub_slabs_of_class(cls), 0);
    expect_num("free pages at end", nr_free_pages(), base_free);
    case_end("T3 slab-reuse");
}

// ---------------------------------------------------------------------------
// T4 跨档隔离（4 个档同时持有对象，不同档不得共页）
// ---------------------------------------------------------------------------

static void
test_t4_cross_class(void) {
    size_t base_free = nr_free_pages();
    case_begin();
    recs_clear();

    unsigned int count = slub_class_count();
    // 不硬编码档值：取下标 0、count/3、2*count/3、count-1 四个档（9 档时
    // 恰为 8/64/512/2048 档）。
    unsigned int pick[4];
    pick[0] = 0;
    pick[1] = count / 3;
    pick[2] = (2 * count) / 3;
    pick[3] = count - 1;

    // 每档 3 个对象。
    for (int k = 0; k < 4; k++) {
        size_t sz = slub_class_size(pick[k]);
        for (int j = 0; j < 3; j++) {
            void *p = kmalloc(sz);
            expect_text(p != NULL, "kmalloc returned NULL");
            if (p != NULL) {
                (void)rec_register(p, sz, (uint32_t)(k * 3 + j + 1),
                                   pick[k], 0);
            }
        }
        expect_num("objects of class", slub_objects_of_class(pick[k]), 3);
    }

    // 不同档的对象不可能落在同一页：按页基址分组，同页只允许一个档。
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (!recs[i].live) {
            continue;
        }
        uintptr_t page_i = (uintptr_t)recs[i].ptr & ~(PGSIZE - 1);
        for (int j = i + 1; j < SLUB_TEST_MAX_OBJS; j++) {
            if (!recs[j].live) {
                continue;
            }
            uintptr_t page_j = (uintptr_t)recs[j].ptr & ~(PGSIZE - 1);
            if (page_i == page_j) {
                expect_text(recs[i].cls == recs[j].cls,
                            "two classes share one page");
            }
        }
    }

    // pattern 完整 + 计数一致。
    verify_shadow_counts();
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (recs[i].live) {
            expect_text(pattern_mismatch(recs[i].ptr, recs[i].size,
                                         recs[i].seed) < 0,
                        "pattern mismatch (cross-class)");
        }
    }

    recs_release_all();
    for (int k = 0; k < 4; k++) {
        expect_num("objects at end", slub_objects_of_class(pick[k]), 0);
        expect_num("slabs at end", slub_slabs_of_class(pick[k]), 0);
    }
    expect_num("free pages at end", nr_free_pages(), base_free);
    case_end("T4 cross-class");
}

// ---------------------------------------------------------------------------
// T5 对象内容不被覆写（200 个随机档对象 + 分配/释放扰动）
// ---------------------------------------------------------------------------

static void
test_t5_no_overwrite(void) {
    const size_t main_objs = 200;
    const size_t churn_rounds = 100;
    size_t base_free = nr_free_pages();
    case_begin();
    recs_clear();

    unsigned int count = slub_class_count();
    size_t max_class = slub_class_size(count - 1);

    // 200 个随机档对象：size 恰为档大小，pattern 覆盖整个槽位（R4 最严格）。
    for (size_t i = 0; i < main_objs; i++) {
        unsigned int cls = lcg_next() % count;
        size_t sz = slub_class_size(cls);
        void *p = kmalloc(sz);
        expect_text(p != NULL, "kmalloc returned NULL");
        if (p != NULL) {
            (void)rec_register(p, sz, (uint32_t)(i + 1), cls, 0);
        }
    }

    // 扰动：反复分配/立即释放其它对象，制造空闲链表与 slab 的复用压力。
    for (size_t r = 0; r < churn_rounds; r++) {
        size_t sz = (size_t)(lcg_next() % max_class) + 1;
        void *p = kmalloc(sz);
        if (p == NULL) {
            continue;  // 元数据池等耗尽属合法失败，跳过本轮扰动
        }
        unsigned int cls = class_of(sz);
        int slot = rec_register(p, sz, (uint32_t)(0x5EED0000u + r), cls, 0);
        if (slot >= 0) {
            (void)rec_release(slot);  // 释放前回读刚写入的 pattern
        } else {
            kfree(p);  // 登记失败（本身已记 FAIL）也要还回去，保住基线
        }
    }

    // 最终回读 200 个主对象的 pattern：任何一次复用错误都会在这里暴露。
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (recs[i].live) {
            expect_text(pattern_mismatch(recs[i].ptr, recs[i].size,
                                         recs[i].seed) < 0,
                        "pattern corrupted by churn");
        }
    }

    recs_release_all();
    expect_num("free pages at end", nr_free_pages(), base_free);
    case_end("T5 no-overwrite");
}

// ---------------------------------------------------------------------------
// T6 大对象直通
// ---------------------------------------------------------------------------

static void
test_t6_large_object(void) {
    size_t base_free = nr_free_pages();
    case_begin();
    recs_clear();

    unsigned int count = slub_class_count();
    size_t max_class = slub_class_size(count - 1);  // 2048
    // 覆盖：最大档+1、整页、整页+1、多页（全部 > 最大档，必走直通）。
    size_t sizes[4];
    sizes[0] = max_class + 1;
    sizes[1] = PGSIZE;
    sizes[2] = PGSIZE + 1;
    sizes[3] = 10000;

    // 再持有一批小对象，验证大对象与它们的区间也不重叠。
    size_t small_sz = slub_class_size(0);
    for (int i = 0; i < 3; i++) {
        void *p = kmalloc(small_sz);
        expect_text(p != NULL, "kmalloc small returned NULL");
        if (p != NULL) {
            (void)rec_register(p, small_sz, (uint32_t)(0xAA00u + i),
                               class_of(small_sz), 0);
        }
    }

    for (int i = 0; i < 4; i++) {
        void *p = kmalloc(sizes[i]);
        expect_text(p != NULL, "kmalloc large returned NULL");
        if (p == NULL) {
            continue;
        }
        // C5：页对齐（rec_register 内也会检查）+ 登记 + 填 pattern。
        int slot = rec_register(p, sizes[i], (uint32_t)(0xB000u + i), 0, 1);
        expect_text(slot >= 0, "register large failed");
        expect_num("large count", slub_large_count(),
                   (unsigned long)recs_live_large_count());
    }
    expect_num("large count after all", slub_large_count(), 4);

    // pattern 回读（覆盖整个 [ptr, ptr+size)）。
    for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
        if (recs[i].live) {
            expect_text(pattern_mismatch(recs[i].ptr, recs[i].size,
                                         recs[i].seed) < 0,
                        "large pattern mismatch");
        }
    }

    recs_release_all();
    expect_num("large count at end", slub_large_count(), 0);
    expect_num("free pages at end", nr_free_pages(), base_free);
    case_end("T6 large-object");
}

// ---------------------------------------------------------------------------
// T7 分配失败与零副作用（请求超过全部物理内存）
// ---------------------------------------------------------------------------

static void
test_t7_alloc_fail(void) {
    size_t base_free = nr_free_pages();
    case_begin();

    unsigned int count = slub_class_count();
    // 快照全部可观测状态。
    static size_t snap_slabs[64], snap_objs[64];
    static size_t snap_large, snap_free;
    for (unsigned int c = 0; c < count && c < 64; c++) {
        snap_slabs[c] = slub_slabs_of_class(c);
        snap_objs[c] = slub_objects_of_class(c);
    }
    snap_large = slub_large_count();
    snap_free = nr_free_pages();

    // 构造"必然失败"的请求：比第一层全部空闲页还多 1024 页。
    size_t huge = (nr_free_pages() + 1024) * PGSIZE + 1;
    void *p = kmalloc(huge);

    if (p != NULL) {
        // 第一层仍能满足（理论上不可能）→ 按规格降级为 SKIP。
        kfree(p);
        cprintf("[SLUB Test] T7 alloc-fail: SKIP "
                "(first layer still has memory)\n");
        return;
    }

    // C8：返回 NULL 时零副作用。
    for (unsigned int c = 0; c < count && c < 64; c++) {
        expect_num("slabs unchanged after fail", slub_slabs_of_class(c),
                   snap_slabs[c]);
        expect_num("objects unchanged after fail", slub_objects_of_class(c),
                   snap_objs[c]);
    }
    expect_num("large count unchanged after fail", slub_large_count(),
               snap_large);
    expect_num("free pages unchanged after fail", nr_free_pages(), snap_free);
    expect_num("free pages at end", nr_free_pages(), base_free);
    case_end("T7 alloc-fail");
}

// ---------------------------------------------------------------------------
// T8 随机压力测试（确定性，seed = 20241023）
// ---------------------------------------------------------------------------

static void
test_t8_stress(void) {
    size_t base_free = nr_free_pages();
    case_begin();
    recs_clear();

    unsigned int count = slub_class_count();
    size_t max_class = slub_class_size(count - 1);
    static const size_t fixed_sizes[5] = {1, 8, 64, 2048, 4097};

    for (size_t round = 0; round < SLUB_TEST_STRESS_ROUNDS; round++) {
        int live = recs_live_count();
        int live_large = recs_live_large_count();
        int do_alloc;
        if (live == 0) {
            do_alloc = 1;
        } else if (live >= SLUB_TEST_LIVE_CAP) {
            do_alloc = 0;
        } else {
            do_alloc = (int)(lcg_next() % 10) < 6;  // 60% 分配 / 40% 释放
        }

        if (do_alloc) {
            size_t sz;
            if (round % 16 == 0) {
                sz = fixed_sizes[(round / 16) % 5];  // 固定值穿插
            } else {
                sz = (size_t)(lcg_next() % 5000) + 1;
            }
            // 大对象表容量保护：存活大对象过多时把请求压回小对象范围。
            if (sz > max_class && live_large >= SLUB_TEST_LARGE_CAP) {
                sz = (size_t)(lcg_next() % max_class) + 1;
            }
            void *p = kmalloc(sz);
            if (p != NULL) {
                int is_large = (sz > max_class);
                unsigned int cls = is_large ? 0 : class_of(sz);
                uint32_t seed = lcg_next();
                int slot = rec_register(p, sz, seed, cls, is_large);
                if (slot < 0) {
                    kfree(p);  // 登记失败也必须还回去，保住基线
                }
                // 每次成功分配后校验影子计数（C2/C5/C10）。
                verify_shadow_counts();
            }
            // p == NULL 属于合法失败（第一层无页/池耗尽），零副作用由
            // T7 专门验证，这里不判定。
        } else {
            // 从存活记录中随机取一条释放；释放前 pattern 必须一致。
            int nth = (int)(lcg_next() % (uint32_t)live);
            int slot = -1;
            for (int i = 0; i < SLUB_TEST_MAX_OBJS; i++) {
                if (recs[i].live && nth-- == 0) {
                    slot = i;
                    break;
                }
            }
            if (slot >= 0) {
                (void)rec_release(slot);
                verify_shadow_counts();
            }
        }

        // 每 200 轮：打印 dump + 影子统计校验。
        if ((round + 1) % 200 == 0) {
            cprintf("-- stress round %lu --\n", (unsigned long)(round + 1));
            slub_dump();
            verify_shadow_counts();
        }
    }

    // 结束：全部释放后一切归零、页回基线。
    recs_release_all();
    for (unsigned int c = 0; c < count; c++) {
        expect_num("objects at end", slub_objects_of_class(c), 0);
        expect_num("slabs at end", slub_slabs_of_class(c), 0);
    }
    expect_num("large count at end", slub_large_count(), 0);
    expect_num("free pages at end", nr_free_pages(), base_free);
    case_end("T8 stress");
}

// ---------------------------------------------------------------------------
// T9 零副作用（全局）
// ---------------------------------------------------------------------------

static void
test_t9_no_side_effect(size_t entry_free) {
    case_begin();
    expect_text(pmm_manager == &best_fit_pmm_manager,
                "pmm_manager was changed");
    expect_num("free pages vs entry", nr_free_pages(), entry_free);
    expect_num("large count at exit", slub_large_count(), 0);
    slub_dump();  // 最终统计（应全为 0）
    case_end("T9 no-side-effect");
}

// ---------------------------------------------------------------------------
// 入口（G1）：由 pmm_init() 在 slub_init() 之后调用
// ---------------------------------------------------------------------------

void
slub_test(void) {
    size_t entry_free = nr_free_pages();
    fail_count = 0;

    cprintf("[SLUB Test] begin, free pages = %lu\n",
            (unsigned long)entry_free);

    test_t1_init();
    test_t2_per_class();
    test_t3_slab_reuse();
    test_t4_cross_class();
    test_t5_no_overwrite();
    test_t6_large_object();
    test_t7_alloc_fail();
    test_t8_stress();
    test_t9_no_side_effect(entry_free);

    if (fail_count == 0) {
        cprintf("[SLUB Test] ALL PASS\n");
    } else {
        cprintf("[SLUB Test] FAILED: %d case(s)\n", fail_count);
        assert(fail_count == 0);  // 停住内核，便于定位
    }
}
