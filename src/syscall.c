#include "exception.h"
#include "thread.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "../lib/cpio.h"
#include "allocator.h"
#include "uart.h"
#include "../lib/string.h"
#include "video.h"
#include "mmu.h"

// ======================================================================
//                               Syscall Helpers
// ----------------------------------------------------------------------
static int is_vma_overlap(thread *t, unsigned long start, unsigned long end) {
    vm_area *curr = t->vmas;
    while (curr) {
        if (!(end <= curr->start || start >= curr->end)) {
            return 1;
        }
        curr = curr->next;
    }
    return 0;
}

vm_area* add_vma(thread *t, unsigned long start, unsigned long length, unsigned long prot, unsigned long flags) {
    vm_area *new_vma = (vm_area *)allocate(sizeof(vm_area));
    new_vma->start = start;
    new_vma->end = start + length;
    new_vma->prot = prot;
    new_vma->flags = flags;
    new_vma->file_data = NULL;
    new_vma->file_size = 0;
    new_vma->next = NULL;

    // Insert sorted by start address
    if (!t->vmas || start < t->vmas->start) {
        new_vma->next = t->vmas;
        t->vmas = new_vma;
    } else {
        vm_area *curr = t->vmas;
        while (curr->next && curr->next->start < start) {
            curr = curr->next;
        }
        new_vma->next = curr->next;
        curr->next = new_vma;
    }
    return new_vma;
}

void remove_vma(thread *t, unsigned long start) {
    vm_area **curr = &t->vmas;
    while (*curr) {
        if ((*curr)->start == start) {
            vm_area *to_free = *curr;
            *curr = (*curr)->next;
            free(to_free);
            return;
        }
        curr = &((*curr)->next);
    }
}

unsigned long find_free_vma_region(thread *t, unsigned long length) {
    unsigned long addr = 0x10000000; // Start searching from 256MB
    while (1) {
        int overlap = 0;
        vm_area *curr = t->vmas;
        while (curr) {
            if (!(addr + length <= curr->start || addr >= curr->end)) {
                addr = curr->end;
                overlap = 1;
                break;
            }
            curr = curr->next;
        }
        if (!overlap) break;
    }
    return addr;
}

static vm_area* copy_vma_list(vm_area* vmas) {
    if (!vmas) return NULL;
    vm_area* head = (vm_area*)allocate(sizeof(vm_area));
    if (!head) return NULL;
    memcpy(head, vmas, sizeof(vm_area));
    head->next = copy_vma_list(vmas->next);
    return head;
}

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

13: void* mmap(void *addr, unsigned long length, int prot, int flags)
Create memory regions for a user process.
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
        
        // Enable SUM to access user buffer and disable interrupts
        unsigned long saved_sstatus;
        asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
        unsigned long new_sstatus = (saved_sstatus | (1UL << 18)) & ~(1UL << 1);
        asm volatile("csrw sstatus, %0" : : "r"(new_sstatus));
        
        // read char into buffer
        buf[read_count++] = (char)c;
        
        // Restore sstatus
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
    }
    
    // disable interrupt
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
        // Enable SUM to access user buffer and disable interrupts
        unsigned long saved_sstatus;
        asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
        unsigned long new_sstatus = (saved_sstatus | (1UL << 18)) & ~(1UL << 1);
        asm volatile("csrw sstatus, %0" : : "r"(new_sstatus));
        
        char c = buf[write_count++];
        
        // Restore sstatus
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
        
        uart_putc(c);
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

    // Enable SUM to access path from user space and disable interrupts
    unsigned long saved_sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
    unsigned long new_sstatus = (saved_sstatus | (1UL << 18)) & ~(1UL << 1);
    asm volatile("csrw sstatus, %0" : : "r"(new_sstatus));

    // set `data` to the start addr of the user program
    extern const void *kernel_fdt;
    extern unsigned long get_initrd_start(const void *fdt);
    
    unsigned long initrd_start = get_initrd_start(kernel_fdt);
    if (initrd_start && initrd_start < PAGE_OFFSET) initrd_start += PAGE_OFFSET;

    int ret = initrd_get_file((const void *)initrd_start, path, &data, &size);

    // Restore sstatus
    asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));

    if (ret != 0) {
        printf("Failed to find %s\r\n", path);
        return -1;
    }
    
    thread *current = get_cur_thread();
    
    // Clear old VMAs
    vm_area *vma = current->vmas;
    while (vma) {
        vm_area *next = vma->next;
        free(vma);
        vma = next;
    }
    current->vmas = NULL;

    // Allocate new PGD for the user process
    unsigned long* old_pgd = current->pgd;
    unsigned long* new_pgd = (unsigned long*)allocate(PAGE_SIZE);
    memset(new_pgd, 0, PAGE_SIZE);
    
    // Copy kernel mappings from global kernel_pgd
    for (int i = 256; i < 512; i++) {
        new_pgd[i] = kernel_pgd[i];
    }

    // Update thread struct
    current->pgd = new_pgd;

    // Map user code at virtual address 0x0
    unsigned long code_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    vm_area *code_vma = add_vma(current, 0, code_pages * PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS);
    if (code_vma) {
        code_vma->file_data = data;
        code_vma->file_size = size;
    }
    // Pre-mapping removed for Demand Paging (Advanced Exercise 2)

    // Map user stack at virtual address 0x003f_ffff_f000
    unsigned long stack_top = 0x4000000000UL;
    unsigned long stack_base = stack_top - USER_STACK_SIZE;
    add_vma(current, stack_base, USER_STACK_SIZE, PROT_READ | PROT_WRITE, MAP_ANONYMOUS);
    // Pre-mapping removed for Demand Paging (Advanced Exercise 2)
    
    // Switch to new page table immediately
    unsigned long satp_val = MAKE_SATP((unsigned long)new_pgd - PAGE_OFFSET);
    asm volatile(
        "csrw satp, %0\n"
        "sfence.vma zero, zero\n"
        : : "r"(satp_val) : "memory"
    );

    // Free old PGD and its user mappings if it was a private one
    if (old_pgd && old_pgd != kernel_pgd) {
        free_pgd(old_pgd);
    }

    // Initialize user registers in the trap frame
    regs->epc = 0 - 4; // Will be 0 after handle_syscall does epc += 4
    regs->sp = stack_top;
    
    return 0;
}

/**
 * @brief 4: Duplicate the current process.
 * @return the child’s pid to the parent, and 0 to the child.
 */
long sys_fork(struct pt_regs *parent_regs) {
    thread* parent = get_cur_thread();
    
    thread* child = (thread*)allocate(sizeof(thread));
    if (!child) return -1;
    
    // Clone task struct
    memcpy(child, parent, sizeof(thread));
    
    child->pid = nr_threads++;
    child->status = THREAD_READY;
    child->parent = parent;

    // Reset signal state for child
    child->sigpending = 0;
    child->is_handling_signal = 0;
    child->signal_stack_page = 0;
    child->signal_stack_vaddr = 0;
    
    // Clone Kernel Stack
    child->kernel_stack = (unsigned long)allocate(KERNEL_STACK_SIZE);
    if (!child->kernel_stack) {
        free(child);
        return -1;
    }
    memcpy((void*)child->kernel_stack, (void*)parent->kernel_stack, KERNEL_STACK_SIZE);
    
    // Clone Address Space
    if (parent->pgd && parent->pgd != kernel_pgd) {
        child->pgd = copy_pgd(parent->pgd);
        if (!child->pgd) {
            free((void*)child->kernel_stack);
            free(child);
            return -1;
        }
    } else {
        child->pgd = kernel_pgd;
    }
    
    // Clone VMAs
    child->vmas = copy_vma_list(parent->vmas);

    // Calculate register offsets for kernel stack
    unsigned long kstack_offset = child->kernel_stack - parent->kernel_stack;
    struct pt_regs *child_regs = (struct pt_regs *)((unsigned long)parent_regs + kstack_offset);
    
    // Child returns 0
    child_regs->a0 = 0;
    // tp points to child thread struct
    child_regs->tp = (unsigned long)child;
    
    // Child's sp and epc are already correct virtual addresses!
    // We just need to ensure child starts at the right place.
    child_regs->epc += 4; // Since parent's epc will be incremented in handle_syscall, we do it here for child.

    // Setup switch context
    child->context.sp = (unsigned long)child_regs;
    child->context.ra = (unsigned long)ret_from_exception;
    
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
    if (pid <= 0) return -1; // Protect PID 0 (idle) and invalid PIDs

    unsigned long saved_sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(1 << 1)); // Disable interrupts

    struct thread *node = run_queue;
    thread *target = NULL;
    if (node) {
        do {
            if (node->pid == pid) {
                target = node;
                break;
            }
            node = node->next;
        } while (node != run_queue);
    }
    
    if (target && target->status != THREAD_TERMINATED && target->status != THREAD_ABORTED) {
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
            asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
            schedule();
            return 0; // Won't be reached
        }
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
        return 0;
    }
    asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
    return -1;
}

/**
 * @brief 8: Display the video.
 */
void sys_display(unsigned int *bmp_image, unsigned int width, unsigned int height) {
    // Enable SUM (Supervisor User Memory) to access user memory in supervisor mode
    // and disable interrupts to prevent context switches while SUM is set.
    unsigned long saved_sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
    unsigned long new_sstatus = (saved_sstatus | (1UL << 18)) & ~(1UL << 1);
    asm volatile("csrw sstatus, %0" : : "r"(new_sstatus));

    video_bmp_display(bmp_image, width, height);

    // Restore sstatus
    asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
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
        
        // Unmap the user page
        unsigned long vaddr = current->signal_stack_vaddr;
        unsigned long *pte = pagewalk(current->pgd, vaddr, 0);
        if (pte) {
            *pte = 0;
            asm volatile("sfence.vma %0, zero" : : "r"(vaddr));
        }
        
        // Remove the VMA record so the address becomes free for mmap
        remove_vma(current, vaddr);
        current->signal_stack_vaddr = 0;
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
    
    unsigned long saved_sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(1 << 1)); // Disable interrupts

    // find target thread in run_queue
    struct thread *node = run_queue;
    thread *target = NULL;
    if (node) {
        do {
            if (node->pid == pid) {
                target = node;
                break;
            }
            node = node->next;
        } while (node != run_queue);
    }

    if (!target || target->status == THREAD_TERMINATED || target->status == THREAD_ABORTED) {
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
        return -1;
    }

    if (target->signal_handler[signum]) {
        // the signal has a handler
        // -> set the pending signal bit
        target->sigpending |= (1 << signum);
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
    } else {
        // Default behavior: terminate
        target->status = THREAD_ABORTED;
        
        // Wake parent up if it was waiting for this process
        if (target->parent != NULL && target->parent->status == THREAD_WAITING && 
           (target->parent->waiting_pid == target->pid || target->parent->waiting_pid == -1)) {
            target->parent->status = THREAD_READY;
            target->parent->waiting_pid = -1;
        }

        if (target == get_cur_thread()) {
            asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
            schedule();
            return 0; // Won't be reached
        }
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
    }
    return 0;
}

/**
 * @brief 13: Create memory regions for a user process.
 * @return the starting address of the mapping on success, -1 on failure.
 */
void *sys_mmap(void *addr, unsigned long length, int prot, int flags) {
    thread *current = get_cur_thread();

    // 1. Round up length to page size
    length = (length + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (length == 0) return (void *)-1;

    unsigned long start = (unsigned long)addr;

    // 2. Handle addr hint
    // If it is NULL, or not page-aligned, or overlaps with existing ones, the kernel chooses.
    if (start == 0 || (start & (PAGE_SIZE - 1)) != 0 || is_vma_overlap(current, start, start + length)) {
        start = find_free_vma_region(current, length);
    }

    // 3. Add VMA
    add_vma(current, start, length, (unsigned long)prot, (unsigned long)flags);
    printf("[MMAP] PID: %d, start: 0x%lx, length: 0x%lx, prot: 0x%x, flags: 0x%x\r\n", 
           current->pid, start, length, prot, flags);

    // 4. Handle MAP_POPULATE
    // If user specifies MAP_POPULATE, kernel should map physical pages immediately.
    // For anonymous pages: allocate and map.
    if (flags & MAP_POPULATE) {
        unsigned long pte_prot = PTE_V | PTE_U | PTE_A | PTE_D;
        if (prot & PROT_READ) pte_prot |= PTE_R;
        if (prot & PROT_WRITE) pte_prot |= (PTE_W | PTE_R); // R=1 if W=1
        if (prot & PROT_EXEC) pte_prot |= PTE_X;

        for (unsigned long a = start; a < start + length; a += PAGE_SIZE) {
            void *phys_page = allocate(PAGE_SIZE);
            if (!phys_page) return (void *)-1;
            memset(phys_page, 0, PAGE_SIZE);
            map_pages_at(current->pgd, a, (unsigned long)phys_page - PAGE_OFFSET, PAGE_SIZE, pte_prot);
        }
    }

    return (void *)start;
}

void handle_syscall(struct pt_regs *regs) {
    unsigned long syscall_num = regs->a7;
    unsigned long arg0 = regs->a0;
    unsigned long arg1 = regs->a1;
    unsigned long arg2 = regs->a2;
    unsigned long arg3 = regs->a3;

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
        case 13:
            ret = (long)sys_mmap((void *)arg0, arg1, (int)arg2, (int)arg3);
            break;
        default:
            printf("Unknown syscall number: %ld\r\n", syscall_num);
            break;
    }

    regs->a0 = ret;
    regs->epc += 4;
}