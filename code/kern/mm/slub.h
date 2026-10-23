#ifndef __KERN_MM_SLUB_H__
#define __KERN_MM_SLUB_H__

#include <defs.h>

/*
 * SLUB 风格"任意大小内存单元"分配器（Challenge）
 * challenge task 2414010
 * 两层架构：
 *   第一层 = 已有的物理页分配器（kern/mm/pmm.c，alloc_pages/free_pages），
 *            SLUB 只通过 alloc_pages()/free_pages() 申请/归还整页，绝不绕过；
 *   第二层 = 本模块：把整页按对象大小分档（size class）切成等长对象，
 *            用内嵌空闲链表（空闲对象头 8 字节存 next 指针）做 O(1) 摘挂链。
 *
 * 档次表（2 的幂，8..2048 字节，共 9 档；每 slab 恰好 1 页 = 4096 字节）：
 *   idx :  0   1   2   3    4    5    6    7     8
 *   size:  8  16  32  64  128  256  512  1024  2048
 *   objs: 512 256 128  64   32   16    8    4     2
 * 请求 size 向上取整到覆盖它的最小档（1→8，100→128，2000→2048）；
 * 超过 SLUB_MAX_OBJ_SIZE 的请求走"大对象直通"：
 *   alloc_pages(ceil(size/PGSIZE))，返回页对齐地址，不做对象切分。
 * 用户可见大小 = 档大小 >= 请求 size。
 */

// 对象档次：最小/最大档大小与档次个数
#define SLUB_MIN_OBJ_SIZE   8u
#define SLUB_MAX_OBJ_SIZE   2048u
#define SLUB_CLASS_COUNT    9u

// 元数据容量上限（耗尽时 kmalloc 返回 NULL，不越界写）
#define SLUB_MAX_SLABS      256u    // slab 元数据池项数
#define SLUB_MAX_LARGE      128u    // 大对象登记表项数

// ---- G1: 对外接口 ----
void slub_init(void);               // 初始化所有 cache 与元数据池（不向第一层要页）
void *kmalloc(size_t size);         // 第二层：任意大小对象分配
void kfree(void *obj);              // 第二层：对象释放（NULL 直接返回）

// ---- G2: 只读统计/调试接口（任何情况下都不修改状态；idx 非法返回 0）----
unsigned int slub_class_count(void);            // 档次个数
size_t slub_class_size(unsigned int idx);       // 第 idx 档的对象大小（字节）
size_t slub_slabs_of_class(unsigned int idx);   // 该档当前持有的 slab 数（=页数）
size_t slub_objects_of_class(unsigned int idx); // 该档当前已分配对象数
size_t slub_large_count(void);                  // 当前存活的大对象个数
void slub_dump(void);                           // cprintf 打印各档统计汇总

#endif /* !__KERN_MM_SLUB_H__ */
