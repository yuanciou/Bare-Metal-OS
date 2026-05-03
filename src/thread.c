#include "thread.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "allocator.h"

static int nr_threads = 0;
static thread* run_queue = NULL;

static void enqueue(thread** queue, thread* t) {
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

    while (next->status == THREAD_TERMINATED || next->status == THREAD_ABORTED) {
        next = next->next;
        if (next == head) {
            next = run_queue; 
            break;
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

    thread *current = run_queue->next;
    thread *prev = run_queue;
    while (current != run_queue) {
        if (current->status == THREAD_TERMINATED) {
            current->status = THREAD_ABORTED;
            prev->next = current->next;
            if (current->kernel_stack) {
                free((void*)current->kernel_stack);
            }
            free(current);
            current = prev->next;
        } else {
            prev = current;
            current = current->next;
        }
    }

    asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
}

// Ensure first schedule call works correctly
void init_thread_system(void) {
    thread* init_t = thread_create(idle);
    init_t->status = THREAD_RUNNING;
    asm volatile("mv tp, %0" : : "r"(init_t));
}