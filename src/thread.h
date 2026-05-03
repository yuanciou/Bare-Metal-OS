#ifndef THREAD_H
#define THREAD_H

#define KERNEL_STACK_SIZE 0x2000
#define USER_STACK_SIZE 0x2000

enum THREAD_STATUS {
    THREAD_RUNNING, THREAD_READY, THREAD_TERMINATED, THREAD_ABORTED, THREAD_WAITING
};

typedef struct thread {
    struct thread_context {
        unsigned long ra;
        unsigned long sp;
        unsigned long s[12];
    } context;
    int pid;
    enum THREAD_STATUS status;
    unsigned long kernel_sp;
    unsigned long user_sp;
    unsigned long kernel_stack;
    unsigned long user_stack;
    struct thread* next;
    struct thread* parent;
    int waiting_pid;
    char* arg;
} thread;

void init_thread_system(void);
thread* thread_create(void (*threadfn)());
thread* user_process_create(void (*user_func)());
void schedule(void);
void idle(void);
void thread_exit(void);
void kill_zombies(void);
thread* get_current(void);
void switch_to(thread* prev, thread* next);
void enqueue(thread** queue, thread* t);

extern thread* run_queue;
extern int nr_threads;

#endif