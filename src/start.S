.section ".text.boot"
.global _start
_start:
    // Save hartid and fdt
    mv s0, a0
    mv s1, a1

    // Clear the .bss section (using physical addresses before MMU is on)
    la t0, __bss_start
    la t1, __bss_stop
    bge t0, t1, clear_bss_done

clear_bss:
    sd zero, 0(t0)
    addi t0, t0, 8
    blt t0, t1, clear_bss

clear_bss_done:
    // Set up a temporary stack for setup_vm
    la sp, _end

    call setup_vm

    // Jump to higher-half
    la t0, 1f
    li t1, 0xffffffc000000000
    add t0, t0, t1
    jr t0

1:
    // Now we are in the higher half!
    la sp, _end

    // Restore hartid and fdt
    mv a0, s0
    mv a1, s1
    li t0, 0xffffffc000000000
    add a1, a1, t0

    // Set stvec to handle_exception
    la t0, handle_exception
    csrw stvec, t0

    // Initialize sscratch to zero
    csrw sscratch, zero

    // Initialize tp to zero (since we use it for get_cur_thread)
    mv tp, zero

    tail start_kernel