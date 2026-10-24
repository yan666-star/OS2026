//lab2 buddy system 2414010
#ifndef __KERN_MM_BUDDY_TEST_H__
#define __KERN_MM_BUDDY_TEST_H__

// Buddy System 分配器的单元测试入口，由 pmm_init() 在 check_alloc_page() 之后调用。
void buddy_test(void);

#endif /* ! __KERN_MM_BUDDY_TEST_H__ */
