#include "thread.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "allocator.h"
#include "exception.h"
#include "timer.h"

int nr_threads = 0;
thread* run_queue = NULL;

static void thread_wakeup_callback(void* arg) {
    thread* t = (thread*)arg;
    t->status = THREAD_READY;
}

int thread_sleep(unsigned int usec) {
    thread* current = get_current();
    if (!current) return -1;

    current->status = THREAD_WAITING;
    // usec to ticks: usec * time_freq / 1000000
    if (add_timer_ticks(thread_wakeup_callback, current, (unsigned long)usec * time_freq / 1000000) != 0) {
        current->status = THREAD_READY;
        return -1;
    }
    schedule();
    return 0;
}

void enqueue(thread** queue, thread* t) {
    if (*queue == NULL) {
        *queue = t;
        t->next = t;
    } else {
        thread* tail = (*queue)->next;
        (*queue)->next = t;
        t->next = tail;
    }
}

thread* get_current(void) {
    register thread* current asm("tp");
    if (!current) return NULL;
    return current;
}

void switch_to(thread* prev, thread* next);

void schedule(void) {
    // Disable interrupts
    unsigned long saved_sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(1 << 1));

    thread* prev = get_current();
    if (prev == NULL) {
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
        return;
    }
    
    thread* next = prev->next;
    thread* head = next;
    int found = 0;

    // Search for a runnable thread
    while (1) {
        if (next->status == THREAD_READY || next->status == THREAD_RUNNING) {
            found = 1;
            break;
        }
        next = next->next;
        if (next == head) break;
    }

    // If no runnable thread found, fallback to PID 0 (idle thread) which should always be READY/RUNNING
    if (!found) {
        next = run_queue;
        while (next->pid != 0) {
            next = next->next;
            if (next == run_queue) break; // Should not happen if PID 0 exists
        }
    }

    if (prev->status == THREAD_RUNNING) {
        prev->status = THREAD_READY;
    }
    next->status = THREAD_RUNNING;

    if (prev != next) {
        switch_to(prev, next);
    }

    // Restore interrupts
    asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
}

void idle(void) {
    while (1) {
        kill_zombies();
        schedule();
    }
}

thread* thread_create(void (*threadfn)()) {
    thread* t = (thread*)allocate(sizeof(thread));
    if (!t) return NULL;
    
    char *tb = (char *)t;
    for (unsigned long i = 0; i < sizeof(thread); i++) tb[i] = 0;

    t->pid = nr_threads++;
    t->status = THREAD_READY;
    t->kernel_stack = (unsigned long)allocate(KERNEL_STACK_SIZE);
    
    t->parent = NULL;
    t->waiting_pid = -1;
    t->current_task_priority = -1;
    t->arg = NULL;

    // Setup initial context
    t->context.ra = (unsigned long)threadfn;
    t->context.sp = t->kernel_stack + KERNEL_STACK_SIZE;

    enqueue(&run_queue, t);
    return t;
}

void thread_exit(void) {
    thread* target = get_current();
    if (target) {
        target->status = THREAD_TERMINATED;
        // Wake parent up if it was waiting for this process
        if (target->parent != NULL && target->parent->status == THREAD_WAITING && 
           (target->parent->waiting_pid == target->pid || target->parent->waiting_pid == -1)) {
            target->parent->status = THREAD_READY;
            target->parent->waiting_pid = -1;
        }
    }
    schedule();
}

void kill_zombies(void) {
    unsigned long saved_sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(1 << 1));

    if (run_queue == NULL) {
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
        return;
    }

    thread *curr = run_queue;
    thread *prev = NULL;
    
    // Find the tail first because it's a circular list and we might remove the head
    thread *tail = run_queue;
    while (tail->next != run_queue) {
        tail = tail->next;
    }
    prev = tail;

    int count = nr_threads; // Safety limit to prevent infinite loops if list is corrupted
    while (count-- > 0 && run_queue != NULL) {
        if (curr->status == THREAD_TERMINATED || curr->status == THREAD_ABORTED) {
            thread *to_free = curr;
            
            if (to_free->next == to_free) {
                // Only one element in the list
                run_queue = NULL;
            } else {
                prev->next = to_free->next;
                if (to_free == run_queue) {
                    run_queue = to_free->next;
                }
            }
            
            curr = to_free->next;
            
            if (to_free->kernel_stack) {
                free((void*)to_free->kernel_stack);
            }
            if (to_free->user_stack) {
                free((void*)to_free->user_stack);
            }
            free(to_free);
            
            if (run_queue == NULL) break;
        } else {
            prev = curr;
            curr = curr->next;
        }
        
        // If we've come back to the beginning (or what is now the beginning), we're done
        if (curr == run_queue) break;
    }

    asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
}

extern void ret_from_exception(void);

thread* user_process_create(void (*user_func)()) {
    thread* t = (thread*)allocate(sizeof(thread));
    if (!t) return NULL;
    
    char *tb = (char *)t;
    for (unsigned long i = 0; i < sizeof(thread); i++) tb[i] = 0;

    t->pid = nr_threads++;
    t->status = THREAD_READY;

    // Allocate kernel stack & user stack
    t->kernel_stack = (unsigned long)allocate(KERNEL_STACK_SIZE);
    t->kernel_sp = t->kernel_stack + KERNEL_STACK_SIZE;
    
    t->user_stack = (unsigned long)allocate(USER_STACK_SIZE);
    t->user_sp = t->user_stack + USER_STACK_SIZE;

    // Set up pt_regs on the kernel stack top
    struct pt_regs *regs = (struct pt_regs *)(t->kernel_sp - sizeof(struct pt_regs));
    char *reg_ptr = (char *)regs;
    for (unsigned long i = 0; i < sizeof(struct pt_regs); i++) reg_ptr[i] = 0;

    regs->tp = (unsigned long)t;
    regs->epc = (unsigned long)user_func;
    regs->sp = t->user_sp;
    
    // Enable SPIE and set SPP to 0 (U-mode)
    regs->status |= (1 << 5);   // SPIE
    // Note: status bit 8 is SPP, setting to 0 implies returning to U-mode, which is default by 0-init

    // Enable SUM to allow accessing user memory if needed? In this basic context not strictly needed since no MMU but good practice.
    // However SUM is bit 18, so maybe regs->status |= (1 << 18);
    // The example sets sstatus bit 13 (FS) and 5 (SPIE), let's keep it simple.

    // Thread context configuration
    t->context.ra = (unsigned long)ret_from_exception;
    t->context.sp = (unsigned long)regs;

    t->parent = get_current();
    t->waiting_pid = -1;
    t->current_task_priority = -1;
    t->arg = NULL;

    enqueue(&run_queue, t);
    return t;
}

// Ensure first schedule call works correctly
void init_thread_system(void) {
    thread* init_t = thread_create(idle);
    init_t->status = THREAD_RUNNING;
    asm volatile("mv tp, %0" : : "r"(init_t));
}