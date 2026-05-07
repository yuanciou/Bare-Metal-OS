#include "exception.h"
#include "thread.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/cpio.h"
#include "allocator.h"
#include "uart.h"
#include "../lib/string.h"
#include "video.h"

// ======================================================================
//                               Syscall
// ----------------------------------------------------------------------
/*
0: long getpid()
Return the current process’s pid.

1: long uart_read(char *buf, long count)
Read count bytes into buf. Return the number of bytes read.

2: long uart_write(const char *buf, long count)
Write count bytes from buf. Return the number of bytes written.

3: int exec(const char *path)
Load and execute the program specified by path. Return 0 on success, -1 on failure.

4: long fork()
Duplicate the current process. Return the child’s pid to the parent, and 0 to the child.

5: long waitpid(long pid)
Wait for the process identified by pid to finish. Return the pid of the finished process.

6: void exit(int status)
Terminate the current process. The status can be used to indicate the exit reason, but it is not required in this lab.

7: int stop(long pid)
Terminate the process identified by pid. Return 0 on success, -1 on failure.

8: void display(unsigned int *bmp_image, unsigned int width, unsigned int height)
Display the video.

9: int usleep(unsigned int usec)
Sleep for a specified number of microseconds. Return 0 on success, -1 on failure.

10: long signal(int signum, void (*handler)())
Register a user-space handler for the given signal. The handler must run in U-mode. The return value is the previous handler for the signal, you can ignore it in this lab.

11: void sigreturn()
Restore the original user context after a signal handler returns. This syscall is called automatically via a trampoline set by the kernel. The kernel also recycles the signal stack upon completion.

12: int kill(int pid, int signum)
Send a signal to the process identified by pid. If the process has a registered handler for the signal, the handler is executed. Otherwise, the process is terminated by default. Return 0 on success, -1 on failure.
*/
// ======================================================================

extern thread* run_queue;
extern void ret_from_exception(void);

/**
 * @brief 0: Run task with the highest priority.
 */
long sys_getpid(void) {
    return get_cur_thread()->pid;
}

/**
 * @brief 1: Read count bytes into buf. 
 * @return The number of bytes read.
 */
long sys_uart_read(char *buf, long count) {
    long read_count = 0;
    
    // enable interrupt to allow UART and timer interrupts during read
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1));

    while (read_count < count) {
        int c;
        // -1: no char to read
        while ((c = uart_getc_nonblocking()) == -1) {
            // no char available -> yield the CPU
            schedule(); 
        }
        // read char into buffer
        buf[read_count++] = (char)c;
    }
    
    // disable interrupt after read and back to do_trap()
    asm volatile("csrc sstatus, %0" : : "r"(1 << 1));
    
    return read_count;
}

/**
 * @brief 2: Write count bytes from buf. 
 * @return The number of bytes written.
 */
long sys_uart_write(const char *buf, long count) {
    long write_count = 0;
    while (write_count < count) {
        uart_putc(buf[write_count++]);
    }
    return write_count;
}

/**
 * @brief 3: Load and execute the program specified by path.
 * @return 0 on success, -1 on failure.
 */
int sys_exec(const char *path, struct pt_regs *regs) {
    const char *data = 0;
    int size = 0;

    // set `data` to the start addr of the user program
    extern const void *kernel_fdt;
    extern unsigned long get_initrd_start(const void *fdt);
    
    unsigned long initrd_start = get_initrd_start(kernel_fdt);
    int ret = initrd_get_file((const void *)initrd_start, path, &data, &size);
    if (ret != 0) {
        printf("Failed to find %s\r\n", path);
        return -1;
    }
    
    thread *current = get_cur_thread();
    
    // Re-initialize user stack securely
    // Instead of changing the sp on hardware directly, 
    // we modify the `pt_regs` snapshot on kernel stack when interrupt occur
    // since after interrupt (do_trap()), the sret will restore the context from `pt_regs` and jump to the new user program.
    if (current->user_stack) {
        regs->sp = current->user_stack + USER_STACK_SIZE; // reset stack
    }
    
    // since the do_trap() will do epc += 4 to next instr,
    // but we want to jump to the start od the new user program
    // Offset by -4 to counteract the epc += 4 advancing behaviour of the trap dispatcher
    regs->epc = (unsigned long)data - 4;
    
    return 0; // Return value is irrelevant since we changed flow
}

/**
 * @brief 4: Duplicate the current process.
 * @return the child’s pid to the parent, and 0 to the child.
 */
long sys_fork(struct pt_regs *parent_regs) {
    thread* parent = get_cur_thread();
    
    thread* child = (thread*)allocate(sizeof(thread));
    if (!child) return -1;
    
    // Exact clone of task structs layout
    memcpy(child, parent, sizeof(thread));
    
    // Reset the childs pid and links
    extern int nr_threads;
    child->pid = nr_threads++;
    child->status = THREAD_READY;
    // set parent-child relationship
    child->parent = parent;

    // Signal state reset for child (clone handlers, but clear pending/stack)
    child->sigpending = 0;
    child->is_handling_signal = 0;
    child->signal_stack_page = 0;
    // handlers are already cloned by memcpy(child, parent, sizeof(thread))
    
    // Clone Kernel Stack -> addr should be different form parent
    child->kernel_stack = (unsigned long)allocate(KERNEL_STACK_SIZE);
    memcpy((void*)child->kernel_stack, (void*)parent->kernel_stack, KERNEL_STACK_SIZE);
    
    // Clone User Stack -> addr should be different form parent
    child->user_stack = (unsigned long)allocate(USER_STACK_SIZE);
    memcpy((void*)child->user_stack, (void*)parent->user_stack, USER_STACK_SIZE);
    
    // Need to fix pointers in the child's context block pointing to absolute addresses!
    // calculate the offset of kenel and user dtack between child and parent,
    // then apply the offset to the trap frame pointer (s0) and sp in the child's kernel stack.
    unsigned long kstack_offset = child->kernel_stack - parent->kernel_stack;
    unsigned long ustack_offset = child->user_stack - parent->user_stack;
    
    // Get proper handle to the cloned child regs
    struct pt_regs *child_regs = (struct pt_regs *)((unsigned long)parent_regs + kstack_offset);
    
    // Child returns 0
    child_regs->a0 = 0;
    
    // tp needs to point to the new process 
    child_regs->tp = (unsigned long)child;
    
    // Update stack pointers inside trap frame
    child_regs->sp += ustack_offset;
    child_regs->s0 += ustack_offset; // frame pointer shifted to child's user stack boundaries
    
    // We adjust ra and sp in the switch context block too
    child->context.sp = (unsigned long)child_regs;
    // set to ret_from_exception so that the child pt_regs will be loaded and back to user mode
    child->context.ra = (unsigned long)ret_from_exception; // bypass returning to fork caller in supervisor
    
    // since the child is not from do_trap() -> manually adjust the return address
    child_regs->epc += 4; 
    
    enqueue(&run_queue, child);
    
    return child->pid;
}

/**
 * @brief 5: Wait for the process identified by pid to finish.
 * @return the pid of the finished process.
 */
long sys_waitpid(long pid) {
    thread *current = get_cur_thread();
    struct thread *node = run_queue;
    
    // Search for the Target
    int found = 0;
    thread *target = NULL;
    do {
        if (node->pid == pid) {
            found = 1;
            target = node;
            break;
        }
        node = node->next;
    } while (node != run_queue);
    
    if (!found) {
        return -1;
    }
    
    // the child is finished -> no need to wait
    if (target->status == THREAD_TERMINATED || target->status == THREAD_ABORTED) {
        return pid;
    }
    
    // Block current process
    current->status = THREAD_WAITING;
    current->waiting_pid = pid;
    schedule();
    
    // the parent wake up when the child call thread_exit()
    return pid;
}

/**
 * @brief 6: Terminate the current process.
 */
void sys_exit(int status) {
    thread_exit();
}

/**
 * @brief 7: Terminate the process identified by pid.
 * @return 0 on success, -1 on failure.
 */
int sys_stop(long pid) {
    // find the target thread in run_queue
    struct thread *node = run_queue;
    thread *target = NULL;
    do {
        if (node->pid == pid) {
            target = node;
            break;
        }
        node = node->next;
    } while (node != run_queue);
    
    if (target) {
        // stop the target
        target->status = THREAD_ABORTED;
        
        // Wake up parent if it was waiting for this process
        if (target->parent != NULL && target->parent->status == THREAD_WAITING && 
           (target->parent->waiting_pid == target->pid || target->parent->waiting_pid == -1)) { // -1 means waiting for any child
            target->parent->status = THREAD_READY;
            target->parent->waiting_pid = -1;
        }
        
        // If stopping self, must yield
        if (target == get_cur_thread()) {
            schedule();
        }
        return 0;
    }
    return -1;
}

/**
 * @brief 8: Display the video.
 */
void sys_display(unsigned int *bmp_image, unsigned int width, unsigned int height) {
    video_bmp_display(bmp_image, width, height);
}

/**
 * @brief 9: Sleep for a specified number of microseconds.
 * @return 0 on success, -1 on failure.
 */
int sys_usleep(unsigned int usec) {
    return thread_sleep(usec);
}

/**
 * @brief 10: Register a user-space handler for the given signal. The handler must run in U-mode
 * @return the previous handler for the signal
 */
long sys_signal(int signum, void (*handler)()) {
    if (signum < 0 || signum >= 32) return -1;
    thread *current = get_cur_thread();
    current->signal_handler[signum] = (unsigned long)handler;
    return 0; // Return value is ignored per requirement
}

/**
 * @brief 11: Restore the original user context after a signal handler returns. 
 *            This syscall is called automatically via a trampoline set by the kernel. 
 *            The kernel also recycles the signal stack upon completion.
 */
void sys_sigreturn(struct pt_regs *regs) {
    thread *current = get_cur_thread();

    // free the signal stack since the handler is done and we have restored the original context
    if (current->signal_stack_page) {
        free((void*)current->signal_stack_page);
        current->signal_stack_page = 0;
    }

    // restore the original context saved before jumping back
    *regs = current->signal_saved_regs;
    current->is_handling_signal = 0;

    printf("[INFO SIGRETURN] Signal handler finished (PID: %d)\r\n", current->pid);
}

/**
 * @brief 12: Send a signal to the process identified by pid. 
 *            If the process has a registered handler for the signal, the handler is executed. 
 *            Otherwise, the process is terminated by default.
 * @return 0 on success, -1 on failure.
 */
int sys_kill(int pid, int signum) {
    if (signum < 0 || signum >= 32 || pid <= 0) return -1;
    
    // find target thread in run_queue
    struct thread *node = run_queue;
    thread *target = NULL;
    do {
        if (node->pid == pid) {
            target = node;
            break;
        }
        node = node->next;
    } while (node != run_queue);

    if (!target) return -1;

    if (target->signal_handler[signum]) {
        // the signal has a handler
        // -> set the pending signal bit
        target->sigpending |= (1 << signum);
    } else {
        // Default behavior: terminate
        target->status = THREAD_ABORTED;
        
        // Wake parent up if it was waiting for this process
        if (target->parent != NULL && target->parent->status == THREAD_WAITING && 
           (target->parent->waiting_pid == target->pid || target->parent->waiting_pid == -1)) {
            target->parent->status = THREAD_READY;
            target->parent->waiting_pid = -1;
        }
    }
    return 0;
}

void handle_syscall(struct pt_regs *regs) {
    unsigned long syscall_num = regs->a7;
    unsigned long arg0 = regs->a0;
    unsigned long arg1 = regs->a1;
    // unsigned long arg2 = regs->a2;
    // unsigned long arg3 = regs->a3;
    
    long ret = -1;
    
    switch (syscall_num) {
        case 0:
            ret = sys_getpid();
            break;
        case 1:
            ret = sys_uart_read((char *)arg0, arg1);
            break;
        case 2:
            ret = sys_uart_write((const char *)arg0, arg1);
            break;
        case 3:
            ret = sys_exec((const char *)arg0, regs);
            break;
        case 4:
            ret = sys_fork(regs);
            break;
        case 5:
            ret = sys_waitpid(arg0);
            break;
        case 6:
            sys_exit((int)arg0);
            break;
        case 7:
            ret = sys_stop(arg0);
            break;
        case 8:
            sys_display((unsigned int *)arg0, (unsigned int)arg1, (unsigned int)regs->a2);
            ret = 0;
            break;
        case 9:
            ret = sys_usleep((unsigned int)arg0);
            break;
        case 10:
            ret = sys_signal((int)arg0, (void (*)())arg1);
            break;
        case 11:
            sys_sigreturn(regs);
            return; // Already restored context during sigreturn, no need to adjust EPC or set return value
        case 12:
            ret = sys_kill((int)arg0, (int)arg1);
            break;
        default:
            printf("Unknown syscall number: %ld\r\n", syscall_num);
            break;
    }
    
    regs->a0 = ret;
    regs->epc += 4;
}