//lab2 buddy system 2414010
#ifndef __KERN_MM_BUDDY_PMM_H__
#define __KERN_MM_BUDDY_PMM_H__

#include <defs.h>
#include <pmm.h>

// 伙伴系统物理内存管理器。它不要求接管内核（pmm.c 仍注册 best_fit），
// 但其中 buddy_pmm_manager 必须可被外部直接调用。
extern const struct pmm_manager buddy_pmm_manager;

// 只读调试接口：供外部单元测试观察内部状态，任何情况下都不修改状态。

// 返回支持的阶数个数，即 MAX_ORDER（合法阶为 0 .. MAX_ORDER-1）。
unsigned int buddy_max_order(void);

// 返回第 order 阶空闲链表上的空闲块个数；order 非法时返回 0。
size_t buddy_free_block_count(unsigned int order);

// 用 cprintf 打印每一阶的空闲块数量（格式自定）。
void buddy_dump(void);

#endif /* !__KERN_MM_BUDDY_PMM_H__ */
