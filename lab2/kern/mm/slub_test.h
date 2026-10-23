#ifndef __KERN_MM_SLUB_TEST_H__
#define __KERN_MM_SLUB_TEST_H__

// SLUB 分配器的单元测试入口，由 pmm_init() 在 slub_init() 之后调用。
//challenge task 2414010
void slub_test(void);

#endif /* ! __KERN_MM_SLUB_TEST_H__ */
