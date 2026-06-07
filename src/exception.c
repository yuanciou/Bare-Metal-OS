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
#include "mmu.h"


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

                // Allocate signal stack physical page
                void *sig_phys = allocate(4096);
                memset(sig_phys, 0, 4096);
                current->signal_stack_page = (unsigned long)sig_phys; // store PA + PAGE_OFFSET
                
                // Find a free VMA region for the signal stack and record it
                unsigned long sig_vaddr = find_free_vma_region(current, 4096);
                current->signal_stack_vaddr = sig_vaddr;
                add_vma(current, sig_vaddr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS);

                // Map the signal stack page to user space at the dynamic address
                map_pages_at(current->pgd, sig_vaddr, (unsigned long)sig_phys - PAGE_OFFSET, 4096, PTE_V | PTE_U | PTE_R | PTE_W | PTE_X | PTE_A | PTE_D);

                // Copy sigreturn trampoline to signal stack
                unsigned long stub_size = (unsigned long)sigreturn_stub_end - (unsigned long)sigreturn_stub;
                unsigned long trampoline_kern = current->signal_stack_page + 4096 - stub_size;
                memcpy((void*)trampoline_kern, (void*)sigreturn_stub, stub_size);

                // Flush I-cache and TLB
                asm volatile("fence.i");
                asm volatile("sfence.vma %0, zero" : : "r"(sig_vaddr));

                unsigned long trampoline_user = sig_vaddr + 4096 - stub_size;

                // Redirect to handler
                regs->ra = trampoline_user; 
                regs->sp = trampoline_user; 
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
        } else if (cause == 12 || cause == 13 || cause == 15) { // Page Fault
            // 12 Instruction Page Fault, 13 Load Page Fault, 15 Store Page Fault
            unsigned long badaddr = regs->badaddr;
            thread *current = get_cur_thread();
            vm_area *vma = current->vmas;
            int handled = 0;
            while (vma) {
                if (badaddr >= vma->start && badaddr < vma->end) {
                    // Check if it is a CoW event: Store fault on a read-only page in a writable VMA
                    if (cause == 15 && (vma->prot & PROT_WRITE)) { // VMA-writable store fault could be CoW
                        unsigned long *pte = pagewalk(current->pgd, badaddr, 0);
                        if (pte && (*pte & PTE_V) && !(*pte & PTE_W)) {
                            // Permission fault (CoW)
                            printf("[Permission fault]: %lx\n", badaddr);
                            unsigned long old_pa = (*pte >> 10) << 12; // old physical address
                            void *old_page = (void *)(old_pa + PAGE_OFFSET);
                            
                            if (get_page_ref(old_page) > 1) {
                                void *new_page = allocate(PAGE_SIZE);
                                if (new_page) {
                                    memcpy(new_page, old_page, PAGE_SIZE);
                                    // Update PTE (0x3FF 10-bit flags)
                                    *pte = (((unsigned long)new_page - PAGE_OFFSET) >> 12 << 10) | (*pte & 0x3FF) | PTE_W;
                                    free(old_page); // Decrement cow_ref_count
                                }
                            } else {
                                // Only one owner, just make it writable
                                *pte |= PTE_W;
                            }
                            // Flush TLB
                            asm volatile("sfence.vma %0, zero" : : "r"(badaddr));
                            handled = 1;
                            break;
                        }
                    }

                    // Check permissions for normal translation fault
                    int permission_ok = 1;
                    if (cause == 12 && !(vma->prot & PROT_EXEC)) permission_ok = 0;
                    if (cause == 13 && !(vma->prot & PROT_READ)) permission_ok = 0;
                    if (cause == 15 && !(vma->prot & PROT_WRITE)) permission_ok = 0;

                    if (!permission_ok) {
                        break; // Fall through to unhandled -> segfault
                    }

                    // Log the translation fault as required
                    printf("[Translation fault]: 0x%lx\r\n", badaddr);

                    void *phys_page = allocate(PAGE_SIZE);
                    if (phys_page) {
                        memset(phys_page, 0, PAGE_SIZE);
                        
                        // If it's backed by data (e.g. code), copy it
                        if (vma->file_data) {
                            unsigned long vma_offset = (badaddr & ~(PAGE_SIZE - 1)) - vma->start; // align down to 4KB page boundary
                            if (vma_offset < vma->file_size) {
                                unsigned long copy_size = vma->file_size - vma_offset;
                                if (copy_size > PAGE_SIZE) copy_size = PAGE_SIZE;
                                memcpy(phys_page, vma->file_data + vma_offset, copy_size);
                            }
                        }

                        unsigned long pte_prot = PTE_V | PTE_U | PTE_A | PTE_D;
                        if (vma->prot & PROT_READ) pte_prot |= PTE_R;
                        if (vma->prot & PROT_WRITE) pte_prot |= (PTE_W | PTE_R); // R=1 if W=1
                        if (vma->prot & PROT_EXEC) pte_prot |= PTE_X;
                        
                        map_pages_at(current->pgd, badaddr & ~(PAGE_SIZE - 1), (unsigned long)phys_page - PAGE_OFFSET, PAGE_SIZE, pte_prot);
                        // Flush TLB for the new mapping
                        asm volatile("sfence.vma %0, zero" : : "r"(badaddr));
                        handled = 1;
                    }
                    break;
                }
                vma = vma->next;
            }
            if (!handled) {
                printf("[Segmentation fault]: Kill Process\n");
                thread_exit();
            }
            return;
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

    thread* current = get_cur_thread();
    if (current) {
        // Clear old VMAs
        vm_area *vma = current->vmas;
        while (vma) {
            vm_area *next = vma->next;
            free(vma);
            vma = next;
        }
        current->vmas = NULL;
    }

    // Allocate new PGD for the user process
    unsigned long* new_pgd = (unsigned long*)allocate(PAGE_SIZE);
    memset(new_pgd, 0, PAGE_SIZE);
    
    // Copy kernel mappings from global kernel_pgd
    extern unsigned long* kernel_pgd;
    for (int i = 256; i < 512; i++) {
        new_pgd[i] = kernel_pgd[i];
    }

    // Update thread struct
    if (current) {
        current->pgd = new_pgd;
    }

    // Map user code at virtual address 0x0
    unsigned long code_pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    extern vm_area* add_vma(thread *t, unsigned long start, unsigned long length, unsigned long prot, unsigned long flags);
    vm_area *code_vma = add_vma(current, 0, code_pages * PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS);
    if (code_vma) {
        code_vma->file_data = data;
        code_vma->file_size = (unsigned long)size;
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

    // set sepc to 0 so that sret will jump to the start of the user program at VA 0x0
    asm volatile("csrw sepc, %0" : : "r"(0UL));

    // save kernel sp to sscratch so that kernel could find its sp when trap happens
    asm volatile("csrw sscratch, sp");
    asm volatile("mv sp, %0" : : "r"(stack_top)); // set sp to user stack virtual address

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
