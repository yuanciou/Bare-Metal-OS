.section ".text.boot"
.global _start
_start:         /* 0x20000000 define in liker.ld */
    j _relocate /* 4 byte jump instr */

/* since `ld` needs to align to 8 bytes */
.align 8

/* `.dword` -> allocate 8 bytes and save the value */
_link_addr:
    /* the value will be 0x0000000020000000 in `_link_addr` flag (addr) */
    .dword _start
_link_end:
    .dword _end

_relocate:
    /* Get current runtime address of _link_addr */
    /* since the program may run at other physical addr */
    la t0, _link_addr

    /* Load the link time address of _start from _link_addr */
    /* i.e, the value save in `_link_addr` flag (addr)*/
    ld t1, 0(t0)

    /* t0 -> _link_addr (runtime) */
    /* t1 -> _start (the addr want to run at) */
    /* t2 = Runtime _start */
    /* t4 = Src (_start runtime) */
    /* t5 = Dst (_start link) */
    /* t6 = Dst End (_end link) */

    /* cal the runtime addr of _start */
    /* -8 byte is because the ((j _relocate) + (.align 8)) */
    addi t2, t0, -8  /* t2 = Runtime _start */
    
    beq t2, t1, init_bss /* If Runtime == Link, done */
    
    /* Relocate */
    /* Cal the Moving Size */
    ld t3, 8(t0)      /* t3 = Link _end */ /* 8 is because .dword preserve 8 bytes */
    sub t3, t3, t1    /* t3 = Size (_end - _start) */
    
    /* Copy */
    mv t4, t2         /* t4 = Src (_start runtime) */
    mv t5, t1         /* t5 = Dst (_start link) */
    add t6, t5, t3    /* t6 = Dst End (_end link) */
    
1:
    bge t5, t6, 2f    /* _start link >= _end link, done */ /* f: find flag forward */
    lw t3, 0(t4)      /* load word (4 bytes) from _start runtime */
    sw t3, 0(t5)      /* store word to _start link */
    addi t4, t4, 4
    addi t5, t5, 4
    j 1b              /* b: find flag backward */
    
2:
    /* Ensure changes to instruction memory are visible */
    /* fence.i -> clear the I-cache, 
       so that the CPU will fetch the instr from memory instead of the cache */
    fence.i
    
    /* Jump to init_bss at linker address */
    /* Since derectly jump may stay in the old runtime addr */
    /* t1 holds Linker _start Address */
    /* t2 holds Runtime _start Address */
    
    la t0, init_bss      /* t0 = Runtime init_bss Address */
    sub t0, t0, t2       /* t0 = Offset from _start */
    add t0, t0, t1       /* t0 = Linker init_bss Address */
    jr t0                /* Jump to init_bss at linker address (expected) */

init_bss:
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