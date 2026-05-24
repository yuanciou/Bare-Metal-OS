OUTPUT_ARCH(riscv)
ENTRY(_start)
SECTIONS
{
    . = 0xffffffc000000000;
    . += 0x80200000;
    _start = .;
    .text : { *(.text.boot) *(.text) }
    . = ALIGN(0x1000);
    _text_end = .;
    
    .rodata : { *(.rodata) }
    . = ALIGN(0x1000);
    _rodata_end = .;
    
    .data : { *(.data) }
    . = ALIGN(0x1000);
    _data_end = .;
    
    .bss : {
        __bss_start = .;
        *(.sbss) *(.bss)
        __bss_stop = .;
    }
    . = ALIGN(16);
    . = . + 0x4000;
    _end = .;
}