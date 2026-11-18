#include <list.h>
#include <sync.h>
#include <proc.h>
#include <sched.h>
#include <assert.h>

void
wakeup_proc(struct proc_struct *proc) {
    assert(proc->state != PROC_ZOMBIE && proc->state != PROC_RUNNABLE);
    proc->state = PROC_RUNNABLE;
}

//LAB4 function3 2414010

// schedule - choose a runnable process and give it the CPU.
//
// The scan is round robin: it starts right after "current" in proc_list,
// skips the list head, and stops at the first process in PROC_RUNNABLE
// state, so every runnable process gets a turn.  idleproc is not linked in
// proc_list, hence it is both the place where the scan starts when the idle
// thread is running and the fallback when nobody else can run.
void
schedule(void)
{
    bool intr_flag;
    struct proc_struct *next = NULL;
    list_entry_t *le, *last;

    // selecting "next" and switching to it must be atomic: an interrupt
    // handler could call schedule() and corrupt proc_list traversal.
    local_intr_save(intr_flag);
    {
        // the reschedule request of the outgoing process is satisfied here
        current->need_resched = 0;

        last = (current == idleproc) ? &proc_list : &(current->list_link);
        le = last;
        do
        {
            if ((le = list_next(le)) != &proc_list)
            {
                next = le2proc(le, list_link);
                if (next->state == PROC_RUNNABLE)
                {
                    break;
                }
            }
        } while (le != last);

        if (next == NULL || next->state != PROC_RUNNABLE)
        {
            // nothing runnable: never panic, just let the idle thread run
            next = idleproc;
        }
        next->need_resched = 0;
        proc_run(next);
    }
    local_intr_restore(intr_flag);
}


