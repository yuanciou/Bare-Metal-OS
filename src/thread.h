#ifndef THREAD_H
#define THREAD_H

#define KERNEL_STACK_SIZE 0x8000

enum THREAD_STATUS {
    THREAD_RUNNING, THREAD_READY, THREAD_TERMINATED, THREAD_ABORTED
};

typedef struct thread {
    struct thread_context {
        unsigned long ra;
        unsigned long sp;
        unsigned long s[12];
    } context;
    int pid;
    enum THREAD_STATUS status;
    unsigned long kernel_stack;
    struct thread* next;
} thread;

void init_thread_system(void);
thread* thread_create(void (*threadfn)());
void schedule(void);
void idle(void);
void thread_exit(void);
void kill_zombies(void);
thread* get_current(void);
void switch_to(thread* prev, thread* next);

#endif