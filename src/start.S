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

    // Save hartid and fdt to s-registers (callee-saved)
    mv s0, a0
    mv s1, a1

    // Explicitly save to stack to address potential clobbering concerns
    addi sp, sp, -16
    sd s0, 0(sp)
    sd s1, 8(sp)

    call setup_vm

    ld s0, 0(sp)
    ld s1, 8(sp)
    addi sp, sp, 16

    // setup_vm returns PAGE_OFFSET
    // add the 1f(PA) with PAGE_OFFSET
    la t0, 1f
    add t0, t0, a0
    jr t0

.align 2
1:
    // Now we are in the higher half!
    la sp, _end

    // Drop identity mapping immediately after entering higher half
    call drop_identity_map

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