.section ".text.boot"
.global _start
_start:
    // Clear the .bss section
    la t0, __bss_start
    la t1, __bss_stop
    bge t0, t1, clear_bss_done

clear_bss:
    sd zero, 0(t0)
    addi t0, t0, 8
    blt t0, t1, clear_bss

clear_bss_done:
    // Set up the stack pointer
    la sp, _end

    // Set stvec to handle_exception
    la t0, handle_exception
    csrw stvec, t0

    // Initialize sscratch to zero so that our trap handler knows we are starting in S-mode
    csrw sscratch, zero

    tail start_kernel