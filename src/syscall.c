#include "exception.h"
#include "thread.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/cpio.h"
#include "allocator.h"
#include "uart.h"
#include "../lib/string.h"
#include "video.h"

extern thread* run_queue;
extern void ret_from_exception(void);

long sys_getpid(void) {
    return get_current()->pid;
}

void sys_display(unsigned int *bmp_image, unsigned int width, unsigned int height) {
    video_bmp_display(bmp_image, width, height);
}

int sys_usleep(unsigned int usec) {
    return thread_sleep(usec);
}

// long sys_uart_read(char *buf, long count) {
//     long read_count = 0;
//     while (read_count < count) {
//         char c = uart_getc();
//         buf[read_count++] = c;
//     }
//     return read_count;
// }
long sys_uart_read(char *buf, long count) {
    long read_count = 0;
    
    // 進入 syscall 時開啟中斷，允許 Timer 和 UART 觸發
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1));

    while (read_count < count) {
        int c;
        // 嘗試非阻塞讀取。如果拿到 -1 代表沒字元可以讀。
        while ((c = uart_getc_nonblocking()) == -1) {
            // 沒字元就讓出 CPU，讓影片播放器等其他 Thread 繼續跑
            schedule(); 
        }
        // 拿到了合法的字元，存進 buffer
        buf[read_count++] = (char)c;
    }
    
    // 離開 syscall 前，把中斷關閉，恢復原本 trap handler 預期的狀態
    asm volatile("csrc sstatus, %0" : : "r"(1 << 1));
    
    return read_count;
}

long sys_uart_write(const char *buf, long count) {
    long write_count = 0;
    while (write_count < count) {
        uart_putc(buf[write_count++]);
    }
    return write_count;
}
// long sys_uart_write(const char *buf, long count) {
//     long write_count = 0;
    
//     // 進入 syscall 時開啟中斷。
//     // 這是必須的！因為如果沒有開啟中斷，UART 硬體傳完字元後無法觸發中斷來清空 Buffer，
//     // 你的 tx_ring 就會永遠是滿的，導致死結！
//     asm volatile("csrs sstatus, %0" : : "r"(1 << 1));

//     while (write_count < count) {
//         char c = buf[write_count];
        
//         // 處理換行字元 \n -> \r\n
//         if (c == '\n') {
//             while (uart_putc_nonblocking('\r') == 0) {
//                 schedule(); // Buffer 滿了，讓出 CPU 給 Video Player 跑
//             }
//         }
        
//         // 傳送實際的字元
//         while (uart_putc_nonblocking(c) == 0) {
//             schedule(); // Buffer 滿了，讓出 CPU
//         }
        
//         write_count++;
//     }
    
//     // 離開 syscall 前，把中斷關閉，恢復原本 trap handler 預期的狀態
//     asm volatile("csrc sstatus, %0" : : "r"(1 << 1));
    
//     return write_count;
// }

int sys_exec(const char *path, struct pt_regs *regs) {
    const char *data = 0;
    int size = 0;
    
    // We retrieve the initrd start mapped earlier if not supplied, we can access via external scope if required.
    // For now we get it from FDT, but let's assume get_initrd_start() handles this or we define a generic fallback.
    // Wait, the example exec gets the entry point directly.
    extern const void *kernel_fdt;
    extern unsigned long get_initrd_start(const void *fdt);
    
    unsigned long initrd_start = get_initrd_start(kernel_fdt);
    int ret = initrd_get_file((const void *)initrd_start, path, &data, &size);
    if (ret != 0) {
        printf("Failed to find %s\r\n", path);
        return -1;
    }
    
    thread *current = get_current();
    
    // Re-initialize user stack securely
    if (current->user_stack) {
        regs->sp = current->user_stack + USER_STACK_SIZE; // reset stack
    }
    
    // Offset by -4 to counteract the epc += 4 advancing behaviour of the trap dispatcher!
    regs->epc = (unsigned long)data - 4;
    
    return 0; // Return value is irrelevant since we changed flow
}

long sys_fork(struct pt_regs *parent_regs) {
    thread* parent = get_current();
    
    thread* child = (thread*)allocate(sizeof(thread));
    if (!child) return -1;
    
    // Exact clone of task structs layout
    memcpy(child, parent, sizeof(thread));
    
    // Reset pid and links
    extern int nr_threads;
    child->pid = nr_threads++;
    child->status = THREAD_READY;
    child->parent = parent;
    
    // Clone Kernel Stack
    child->kernel_stack = (unsigned long)allocate(KERNEL_STACK_SIZE);
    memcpy((void*)child->kernel_stack, (void*)parent->kernel_stack, KERNEL_STACK_SIZE);
    
    // Clone User Stack
    child->user_stack = (unsigned long)allocate(USER_STACK_SIZE);
    memcpy((void*)child->user_stack, (void*)parent->user_stack, USER_STACK_SIZE);
    
    // Need to fix pointers in the child's context block pointing to absolute addresses!
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
    child->context.ra = (unsigned long)ret_from_exception; // bypass returning to fork caller in supervisor
    
    // advance parent pc here as they are returning exactly from the trap dispatcher
    // child starts right after the ecall!
    child_regs->epc += 4; 
    
    enqueue(&run_queue, child);
    
    return child->pid;
}

long sys_waitpid(long pid) {
    thread *current = get_current();
    struct thread *node = run_queue;
    
    // Is it existing?
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
    
    if (target->status == THREAD_TERMINATED || target->status == THREAD_ABORTED) {
        return pid;
    }
    
    // Block current process
    current->status = THREAD_WAITING;
    current->waiting_pid = pid;
    schedule();
    
    return pid;
}

void sys_exit(int status) {
    thread_exit();
}

int sys_stop(long pid) {
    struct thread *node = run_queue;
    do {
        if (node->pid == pid) {
            node->status = THREAD_ABORTED;
            return 0;
        }
        node = node->next;
    } while (node != run_queue);
    return -1;
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
        default:
            printf("Unknown syscall number: %ld\r\n", syscall_num);
            break;
    }
    
    regs->a0 = ret;
    regs->epc += 4;
}