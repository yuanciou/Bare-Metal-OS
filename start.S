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
    tail start_kernel