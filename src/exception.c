#include "exception.h"
#include "../lib/stdio.h"
#include "allocator.h"
#include "../lib/cpio.h"
#include "timer.h"

void do_trap(struct pt_regs* regs) {
    if (regs->cause & (1ULL << 63)) { // 最高位元為 1 代表這是一個 Interrupt (hardware do automatically)
        unsigned long cause = regs->cause & ~(1ULL << 63);
        if (cause == 5) { // 5 代表 Supervisor timer interrupt
            handle_timer_interrupt();
            return;
        }
        printf("Unknown interrupt: %ld\r\n", cause);
        return;
    }

    printf("Exception:\r\n");
    printf("  scause: 0x%lx\r\n", regs->cause);
    printf("  sepc: 0x%lx\r\n", regs->epc);
    printf("  stval: 0x%lx\r\n", regs->badaddr);
    regs->epc += 4; // to move past the ecall instruction
}

int exec(const char* filename, unsigned long initrd_start) {
    const char* data = 0;
    int size = 0;
    int ret = initrd_get_file((const void*)initrd_start, filename, &data, &size);
    if (ret != 0) {
        printf("Failed to find %s in initramfs\r\n", filename);
        return -1;
    }

    unsigned long stack_size = 4096;
    unsigned long user_sp = (unsigned long)allocate(stack_size) + stack_size;

    asm volatile("csrw sepc, %0" : : "r"((unsigned long)data));
    asm volatile("csrw sscratch, sp");
    asm volatile("mv sp, %0" : : "r"(user_sp));

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
