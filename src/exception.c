#include "exception.h"
#include "../lib/stdio.h"
#include "allocator.h"
#include "../lib/cpio.h"
#include "timer.h"
#include "uart.h"
#include "plic.h"
#include "task.h"


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
        printf("Exception:\r\n");
        printf("  scause: 0x%lx\r\n", regs->cause);
        printf("  sepc: 0x%lx\r\n", regs->epc);
        printf("  stval: 0x%lx\r\n", regs->badaddr);
        regs->epc += 4; // to move past the ecall instruction
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
    unsigned long stack_size = 4096;
    unsigned long user_sp = (unsigned long)allocate(stack_size) + stack_size;

    // write the user program entry point to sepc
    /*
        - %0 in the asm volatile means the first input operand, which is "r"((unsigned long)data) in this case.
        - "r" tells the compiler to put the value of ((unsigned long)data) into a general-purpose register,
            so that csrw could read ((unsigned long)data) from the register
    */
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
