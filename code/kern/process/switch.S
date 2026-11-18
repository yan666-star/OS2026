//LAB4 function3 2414010
/*
 * switch_to(struct context *from, struct context *to)
 *   a0 = from, a1 = to
 *
 * struct context layout (kern/process/proc.h), each field is 8 bytes:
 *   ra :  0    sp :  8    s0 : 16    s1 : 24    s2 : 32    s3 : 40
 *   s4 : 48    s5 : 56    s6 : 64    s7 : 72    s8 : 80    s9 : 88
 *   s10: 96    s11:104
 *
 * Only the callee-saved registers are touched here: caller-saved registers
 * (t0-t6, a0-a7) are already dead at the call site by the C calling
 * convention.  No scheduler decision, "current" or page table update is
 * done in this routine.
 */
    .text
    .globl switch_to
    .align 2
switch_to:
    /* save the context of the current thread into *from */
    sd ra,    0(a0)
    sd sp,    8(a0)
    sd s0,   16(a0)
    sd s1,   24(a0)
    sd s2,   32(a0)
    sd s3,   40(a0)
    sd s4,   48(a0)
    sd s5,   56(a0)
    sd s6,   64(a0)
    sd s7,   72(a0)
    sd s8,   80(a0)
    sd s9,   88(a0)
    sd s10,  96(a0)
    sd s11, 104(a0)

    /* restore the context of the next thread from *to */
    ld ra,    0(a1)
    ld sp,    8(a1)
    ld s0,   16(a1)
    ld s1,   24(a1)
    ld s2,   32(a1)
    ld s3,   40(a1)
    ld s4,   48(a1)
    ld s5,   56(a1)
    ld s6,   64(a1)
    ld s7,   72(a1)
    ld s8,   80(a1)
    ld s9,   88(a1)
    ld s10,  96(a1)
    ld s11, 104(a1)

    /* ra now holds the resume point of the next thread: the instruction
     * after its own switch_to call, or forkret for a brand-new thread. */
    ret
