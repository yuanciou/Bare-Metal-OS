#include "exception.h"
#include "../lib/stdio.h"
#include "allocator.h"
#include "../lib/cpio.h"
#include "timer.h"
#include "uart.h"
#include "plic.h"
#include "task.h"
#include "thread.h"


void handle_syscall(struct pt_regs *regs);

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

    thread* current = get_current();
    if (current) {
        current->user_stack = user_stack_base;
    }

    // Offset by -4 to counteract the epc += 4 advancing behaviour of the shell thread calling ecall? 
    // Wait, the shell thread is currently executing exec directly from C in kernel mode!
    // But if exec is called from user mode (sys_exec), it is handled via syscall.c.
    // If it's called natively from the shell_thread via shell command "exec [file]", 
    // the sret directly jumps to data. So data does not need - 4 here because we are not in a trap loop!
    asm volatile("csrw sepc, %0" : : "r"((unsigned long)data));

    // save kernel sp to sscratch so that kernel could find its sp when trap happens
    asm volatile("csrw sscratch, sp");
    asm volatile("mv sp, %0" : : "r"(user_sp)); // set sp to user stack

    // Enable SPIE but clear SPP (U-mode)
    asm volatile(
        "li t0, (1 << 8);" // SSTATUS_SPP
        "csrc sstatus, t0;"
        "li t0, (1 << 5);" // SSTATUS_SPIE
        "csrs sstatus, t0;"
    );

    asm volatile("sret");
    return 0; // won't be reached
}
