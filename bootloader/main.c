#include "lib/string.h"
#include "lib/cpio.h"
#include "lib/stdio.h"
#include "lib/fdt.h"
#include "config.h"

extern char uart_getc(void);
extern char uart_getc_raw(void);
extern void uart_putc(char c);
extern void uart_hex(unsigned long h);

void load_kernel(unsigned long hartid, const void *fdt) {
    printf("Waiting for kernel...\r\n");

    unsigned long magic = 0;
    unsigned long size = 0;

    // check the magic number
    // wait for 0x544F4F42 (magic number)
    while (1) {
        // since its little endian -> shift and combine to get the number
        char c = uart_getc_raw();
        magic = c;
        magic |= ((unsigned long)(unsigned char)uart_getc_raw() << 8);
        magic |= ((unsigned long)(unsigned char)uart_getc_raw() << 16);
        magic |= ((unsigned long)(unsigned char)uart_getc_raw() << 24);

        if (magic == 0x544F4F42) {
            break;
        }
    }

    /*
        As the pack part in `send_kernel.py`,
        the kernel size is sent as 4 byte (little endian) right after the magic number
    */
    size = (unsigned char)uart_getc_raw();
    size |= ((unsigned long)(unsigned char)uart_getc_raw() << 8);
    size |= ((unsigned long)(unsigned char)uart_getc_raw() << 16);
    size |= ((unsigned long)(unsigned char)uart_getc_raw() << 24);

    printf("Kernel size: ");
    uart_hex(size);
    printf("\r\nLoading...\r\n");

    // write the kenel to the memory define before
    // write the kernel byte by byte, since the kernel format is unknown
    char *target = (char *)KERNEL_LOAD_ADDR;
    for (unsigned long i = 0; i < size; i++) {
        target[i] = uart_getc_raw();
    }

    printf("Kernel loaded. Jumping to it...\r\n");

    // Flush the instruction cache to ensure the new kernel code is visible
    asm volatile("fence.i");

    // pass hartid and fdt to the new kernel
    void (*new_kernel_addr)(unsigned long, const void *) = (void (*)(unsigned long, const void *))KERNEL_LOAD_ADDR;

    // jump to it
    new_kernel_addr(hartid, fdt);
}

void run_shell(unsigned long hartid, const void *fdt) {
    char buffer[256];
    int idx = 0;

    while (1) {
        printf("\r\n# ");
        idx = 0;
        // handle the user input [command] until press Enter
        while (1) {
            char c = uart_getc();
            if (c == '\n' || c == '\r') {
                printf("\r\n");
                buffer[idx] = '\0';
                break;
            } else if (idx < sizeof(buffer) - 1) {
                buffer[idx++] = c;
                uart_putc(c);
            }
        }

        if (idx == 0) continue;

        if (strcmp(buffer, "help") == 0) {
            printf("Avalible commands:\r\n");
            printf("  help - show all commands.\r\n");
            printf("  hello - print Hello world.\r\n");
            printf("  load - load kernel over UART.\r\n");
        } else if (strcmp(buffer, "hello") == 0) {
            printf("Hello world.\r\n");
        } else if (strcmp(buffer, "load") == 0) {
            load_kernel(hartid, fdt);
        } else {
            printf("Unknown command: ");
            printf(buffer);
            printf("\r\nUse help to get commands.\r\n");
        }
    }
}

void start_kernel(unsigned long hartid, const void *fdt) {
    init_uart_from_fdt(fdt);

    printf("Start Bootloader...:\r\n");
    run_shell(hartid, fdt);
}
