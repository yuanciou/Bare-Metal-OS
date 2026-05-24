#ifndef THREAD_H
#define THREAD_H

#include "exception.h"

#define KERNEL_STACK_SIZE 0x2000
#define USER_STACK_SIZE 0x2000

enum THREAD_STATUS {
    THREAD_RUNNING,     // currently running
    THREAD_READY,       // ready to run in the run queue
    THREAD_WAITING,     // waiting for an event (e.g., sleep, waitpid). Not in the run queue.
    THREAD_TERMINATED,  // [Zombie] the thread has finished execution (or call exit()) but not yet cleaned up. Not in the run queue.
    THREAD_ABORTED,     // [Zombie] error or killed by signal. Not in the run queue.
};

typedef struct thread {
    struct thread_context {
        unsigned long ra;       // return address
        unsigned long sp;       // stack pointer
        unsigned long s[12];    // callee-saved registers s0-s11
    } context;
    int pid;                    // process ID
    enum THREAD_STATUS status;  // thread status
    unsigned long kernel_sp;    // kernel sp (since interrupt need kernel stack)
    unsigned long user_sp;      // user sp
    unsigned long kernel_stack; // the base addr of the kernel stack (for freeing when terminated)
    unsigned long user_stack;   // the base addr of the user stack (for freeing when terminated)
    struct thread* next;        // for the linked list in the run queue
    struct thread* parent;      // for fork (when child dead -> wake up parent)
    int waiting_pid;            // let parent know which child it's waiting for
    int current_task_priority;  // for run_task() to determine the thread priority
    char* arg;                  // the argument to pass to the thread function
    void (*entry_func)();       // the entry function of the thread
    unsigned long* pgd;         // page global directory (physical address stored in satp)

    // POSIX Signal fields
    unsigned long signal_handler[32];   // the signal handler 
    unsigned int sigpending;            // record the pending signal
    int is_handling_signal;
    struct pt_regs signal_saved_regs;   // saved register when handling interrupt
    unsigned long signal_stack_page;
} thread;

void init_thread_system(void);
thread* thread_create(void (*threadfn)());
void schedule(void);
void idle(void);
void thread_exit(void);
void kill_zombies(void);
thread* get_cur_thread(void);
void switch_to(thread* prev, thread* next);
void enqueue(thread** queue, thread* t);
int thread_sleep(unsigned int usec);

extern thread* run_queue;
extern int nr_threads;

#endif