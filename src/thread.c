#include "thread.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "allocator.h"
#include "exception.h"
#include "timer.h"

// ============================================
//                    Utils
// ============================================

int nr_threads = 0;     // number of thread (use for PID assignment)
thread* run_queue = NULL;

/**
 * @brief Enqueue a thread into the run queue (circular linked list)
 */
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

/**
 * @brief Get the current running thread by reading the tp register.
 *        `register` -> used to hint the compiler to keep the variable in a register (tp) for fast access (instead of stack on memory).  
 */
thread* get_cur_thread(void) {
    register thread* current asm("tp");
    if (!current) return NULL;
    return current;
}

// ============================================
//              Sleep and Wakeup
// ============================================
/**
 * @brief Set the wakeuped thread's status to READY so that it can be scheduled again.
 */
static void thread_wakeup_callback(void* arg) {
    thread* t = (thread*)arg;
    t->status = THREAD_READY;
}

/**
 * @brief Sleep -> change the thead status to WAITING and add a timer to wake it up after the specified duration.
 */
int thread_sleep(unsigned int usec) {
    thread* current = get_cur_thread();
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

// ============================================
//                 Scheduler
// ============================================
void switch_to(thread* prev, thread* next);

/**
 * @brief Schedule the thread to next runnabel thread.
 *        - run_queue > 1 -> switch_to next runnable thread.
 *        - run_queue = 1 -> keep current thread.
 *        - run_queue = 0 -> back to idle thread
 */
void schedule(void) {
    // Disable interrupts
    // since if we enable interrupt in schedule, the timer interrupt might trigger and call schedule again before we finish the current scheduling.
    // then the race condition may happen
    unsigned long saved_sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus)); // store whether interrupts were enabled before we disabled them
    asm volatile("csrc sstatus, %0" : : "r"(1 << 1));

    // lode the current thread
    thread* prev = get_cur_thread();
    if (prev == NULL) {
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
        return;
    }
    
    // find the next runnable thread in the run queue
    thread* next = prev->next;
    thread* head = next;
    int found = 0;

    // Search for a runnable thread
    while (1) {
        if (next->status == THREAD_READY || next->status == THREAD_RUNNING) {
            // Found a thread to run (next runnabele or only 1 runnable thread which is itself)
            found = 1;
            break;
        }
        next = next->next;

        // no runnable thread found after traversing the whole queue, break and fallback to idle thread (PID 0)
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

    // make the status of the previous (current) thread to READY
    // and change the status of the next thread to RUNNING (if it's not already)
    if (prev->status == THREAD_RUNNING) {
        prev->status = THREAD_READY;
    }
    next->status = THREAD_RUNNING;

    // if the candidate thread is not as same as the current -> switch to it
    if (prev != next) {
        switch_to(prev, next);
    }

    // Restore interrupts status
    asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
}

// ============================================
//       Zombie Killer and Idle Thread
// ============================================
// The zombie exit since when the thread exit(), it can't free() itself since it still run on it. 
void kill_zombies(void) {
    // Disable interrupts since we will modify the run queue
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
        if (curr->status == THREAD_TERMINATED || curr->status == THREAD_ABORTED) { // the zombie thread
            thread *to_free = curr;
            
            if (to_free->next == to_free) {
                // Only one element in the list
                run_queue = NULL;
            } else {
                prev->next = to_free->next;
                if (to_free == run_queue) { // if remove the queue head
                    run_queue = to_free->next;
                }
            }
            
            curr = to_free->next;
            
            // free the kernel stack, user stack and the thread struct itself
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

    // restore the previous interrupt state
    asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
}

/**
 * @brief The idle thread should keep killing zombies and yield the CPU to other threads.
 */
void idle(void) {
    while (1) {
        kill_zombies();
        schedule();
    }
}

// ============================================
//      Thread Creater, Wrapper and Exit
// ============================================
/**
 * @brief The wrapper function for the thread, which will be set as the initial return address (ra) of the thread's context.
 *        It will enable interrupts, call the thread's entry function, and then call thread_exit
 */
void kernel_thread_wrapper() {
    // Enable interrupts (SIE = 1), since the schedule will disabel interrupts
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1));

    // Execute the entry function
    thread *current = get_cur_thread();
    if (current && current->entry_func) {
        current->entry_func();  // Call the thread's entry function
    }

    // Exit the thread if the entry function returns
    thread_exit();
}

/**
 * @brief The thread creation. Initialize the thread struct, and enqueue it to the run queue.
 */
thread* thread_create(void (*threadfn)()) {
    // allocate memory for the thread struct
    thread* t = (thread*)allocate(sizeof(thread));
    if (!t) return NULL;
    
    // initialize the allocated field to 0
    char *tb = (char *)t;
    for (unsigned long i = 0; i < sizeof(thread); i++) tb[i] = 0;

    t->pid = nr_threads++;
    t->status = THREAD_READY;
    t->kernel_stack = (unsigned long)allocate(KERNEL_STACK_SIZE);
    
    t->parent = NULL;
    t->waiting_pid = -1;
    t->current_task_priority = -1;
    t->arg = NULL;
    t->entry_func = threadfn;

    // Initialize signal fields
    for (int i = 0; i < 32; i++) t->signal_handler[i] = 0;
    t->sigpending = 0;
    t->is_handling_signal = 0;
    t->signal_stack_page = 0;

    // Setup initial context to use the wrapper
    t->context.ra = (unsigned long)kernel_thread_wrapper;
    t->context.sp = t->kernel_stack + KERNEL_STACK_SIZE;  // since the stack grows downwards

    enqueue(&run_queue, t);
    return t;
}

/**
 * @brief Mark the current thread to zombie, wake up the parent, and yield the CPU.
 */
void thread_exit(void) {
    thread* target = get_cur_thread();
    if (target) {
        target->status = THREAD_TERMINATED; // mark itself as zombie
        // Wake parent up if it was waiting for this process
        if (target->parent != NULL && target->parent->status == THREAD_WAITING && 
           (target->parent->waiting_pid == target->pid || target->parent->waiting_pid == -1)) { //waiting_pid == -1 means waiting for any child
            target->parent->status = THREAD_READY;
            target->parent->waiting_pid = -1;
        }
    }
    // yield the CPU
    schedule();
}

// ============================================
//                System Init
// ============================================
/**
 * @brief The thread system initialization function.
 *        Create the idle thread (PID 0) and set it as the current running thread on `tp`.  
 */
void init_thread_system(void) {
    thread* init_t = thread_create(idle);
    init_t->status = THREAD_RUNNING;
    asm volatile("mv tp, %0" : : "r"(init_t));
}