#include "exception.h"
#include "../lib/stdio.h"
#include "../lib/string.h"
#include "allocator.h"
#include "../lib/cpio.h"
#include "timer.h"
#include "uart.h"
#include "plic.h"
#include "task.h"
#include "thread.h"


void handle_syscall(struct pt_regs *regs);

extern char sigreturn_stub[];
extern char sigreturn_stub_end[];

void check_signals(struct pt_regs *regs) {
    thread *current = get_cur_thread();
    if (current && current->sigpending != 0 && current->is_handling_signal == 0) { // check need to handle signal and not handling other signal
        for (int i = 0; i < 32; i++) {
            if (current->sigpending & (1 << i)) { // find the first pending signal
                current->sigpending &= ~(1 << i); // clear the pending bit
                current->is_handling_signal = 1;

                // Save current context
                current->signal_saved_regs = *regs;

                // Allocate signal stack
                current->signal_stack_page = (unsigned long)allocate(4096);
                
                // Copy sigreturn trampoline to signal stack
                unsigned long stub_size = (unsigned long)sigreturn_stub_end - (unsigned long)sigreturn_stub;
                unsigned long trampoline = current->signal_stack_page + 4096 - stub_size; // cal the stack top
                memcpy((void*)trampoline, (void*)sigreturn_stub, stub_size);

                // Flush I-cache for the trampoline
                asm volatile("fence.i");

                // Redirect to handler
                regs->ra = trampoline; // set return addr to trampoline so that the sigreturn will execute when handler finish
                regs->sp = trampoline; // the stack top for the signal handler to use
                regs->epc = current->signal_handler[i];
                
                break;
            }
        }
    }
}

void do_trap(struct pt_regs* regs) {
    // Interrupt (hardware do automatically) if the MSB of scause is 1
    if (regs->cause & (1ULL << 63)) {

        // get the cause 
        unsigned long cause = regs->cause & ~(1ULL << 63);
        if (cause == 5) { // Supervisor timer interrupt
            handle_timer_interrupt();
        } else if (cause == 9) { // Supervisor external interrupt
            int irq = plic_claim();
            if (irq == 0) {
                printf("FATAL: PLIC claimed IRQ 0! Context ID mismatch!\r\n");
                while(1);
            }
            if (irq > 0) {
                extern int g_uart_irq;
                if (irq == g_uart_irq) { // UART IRQ
                    handle_uart_interrupt();
                } else {
                    printf("Unknown external interrupt: %d\r\n", irq);
                }
                // complete the interrupt hadling and write the IRQ back
                plic_complete(irq);
            }
        } else {
            printf("Unknown interrupt: %ld\r\n", cause);
        }
    } else { // software exception (ecall from S-mode)
        unsigned long cause = regs->cause;
        if (cause == 8) { // User mode ecall (syscall)
            handle_syscall(regs);
            check_signals(regs);
            return; // Syscall dispatcher manually adjusted EPC, return right after
        } else {
            printf("Exception:\r\n");
            printf("  scause: 0x%lx\r\n", regs->cause);
            printf("  sepc: 0x%lx\r\n", regs->epc);
            printf("  stval: 0x%lx\r\n", regs->badaddr);

            // Do not advance EPC for unknown crashes. Terminate the faulty process.
            thread_exit();
        }
    }

    // save the context to avoid another trap happens in run_task()
    unsigned long saved_sepc, saved_sstatus, saved_scause, saved_stval;
    asm volatile("csrr %0, sepc"   : "=r"(saved_sepc));
    asm volatile("csrr %0, sstatus": "=r"(saved_sstatus));
    asm volatile("csrr %0, scause" : "=r"(saved_scause));
    asm volatile("csrr %0, stval"  : "=r"(saved_stval));

    run_tasks(); // Will enable/disable SIE internally during tasks

    check_signals(regs);

    asm volatile("csrc sstatus, %0" : : "r"(1 << 1)); // Disable SIE locally again just in case
    asm volatile("csrw sepc, %0"   : : "r"(saved_sepc));
    asm volatile("csrw sstatus, %0": : "r"(saved_sstatus));
    asm volatile("csrw scause, %0" : : "r"(saved_scause));
    asm volatile("csrw stval, %0"  : : "r"(saved_stval));
}

int exec(const char* filename, unsigned long initrd_start) {
    const char* data = 0;
    int size = 0;
    int ret = initrd_get_file((const void*)initrd_start, filename, &data, &size);
    if (ret != 0) {
        printf("Failed to find %s in initramfs\r\n", filename);
        return -1;
    }
    // alloc a page for user stack
    // + STACK_SIZE to point to the top of the stack (since the stack grows downwards)
    unsigned long stack_size = USER_STACK_SIZE;
    unsigned long user_stack_base = (unsigned long)allocate(stack_size);
    unsigned long user_sp = user_stack_base + stack_size;

    thread* current = get_cur_thread();
    if (current) {
        current->user_stack = user_stack_base;
    }

    // set data (user program start) to sepc so that sret will jump to the user program
    asm volatile("csrw sepc, %0" : : "r"((unsigned long)data));

    // save kernel sp to sscratch so that kernel could find its sp when trap happens
    asm volatile("csrw sscratch, sp");
    asm volatile("mv sp, %0" : : "r"(user_sp)); // set sp to user stack

    // Enable SPIE but clear SPP (U-mode)
    asm volatile(
        "li t0, (1 << 8);" // SSTATUS_SPP (U-mode)
        "csrc sstatus, t0;"
        "li t0, (1 << 5);" // SSTATUS_SPIE (hardware interrupt)
        "csrs sstatus, t0;"
    );

    asm volatile("sret");
    return 0; // won't be reached
}
