#include "lib/string.h"
#include "lib/sbi.h"
#include "lib/cpio.h"
#include "lib/stdio.h"
#include "lib/fdt.h"
#include "config.h"
#include "allocator.h"

extern char uart_getc(void);
extern char uart_getc_raw(void);
extern void uart_putc(char c);
extern void uart_hex(unsigned long h);

static void buddy_demo_case(void) {
    void *p1;
    void *p2;
    void *p3;

    printf("[Demo] buddy alloc/free start\r\n");
    p1 = allocate(4000);
    p2 = allocate(8000);
    p3 = allocate(4000);

    allocator_dump_pages();

    free(p1);
    free(p2);
    free(p3);

    allocator_dump_pages();
    printf("[Demo] buddy alloc/free end\r\n");
}

static void dynamic_demo_case(void) {
    void *ptr1;
    void *ptr2;
    void *ptr3;
    void *ptr4;
    void *big;
    void *too_big;

    printf("[Demo] dynamic allocator start\r\n");

    ptr1 = allocate(16);
    ptr2 = allocate(32);
    ptr3 = allocate(64);
    ptr4 = allocate(128);

    free(ptr1);
    free(ptr2);
    free(ptr3);
    free(ptr4);

    big = allocate(8000);
    free(big);

    too_big = allocate(BUDDY_MAX_ALLOC_SIZE + 1);
    if (!too_big) {
        printf("Allocation failed as expected for size > MAX_ALLOC_SIZE\r\n");
    }

    printf("[Demo] dynamic allocator end\r\n");
}

void run_shell(unsigned long hartid, const void *fdt) {
    char buffer[256];
    int idx = 0;

    // Get initrd address from FDT
    unsigned long initrd_start = get_initrd_start(fdt);

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
            printf("  info - print system info.\r\n");
            printf("  ls - list files in initramfs.\r\n");
            printf("  cat [file] - print file content in initramfs.\r\n");
            printf("  buddy - run buddy allocator demo case.\r\n");
            printf("  alloc - run dynamic allocator demo case.\r\n");
        } else if (strcmp(buffer, "hello") == 0) {
            printf("Hello world.\r\n");
        } else if (strcmp(buffer, "info") == 0) {
            printf("System information:\r\n");
            printf("  OpenSBI specification version: ");
            uart_hex(sbi_get_spec_version());
            printf("\r\n");
            printf("  Implementation ID: ");
            uart_hex(sbi_get_impl_id());
            printf("\r\n");
            printf("  Implementation version: ");
            uart_hex(sbi_get_impl_version());
            printf("\r\n");
        } else if (strcmp(buffer, "ls") == 0) {
            if (initrd_start) {
                initrd_list((const void*)initrd_start);
            } else {
                printf("No initrd found\r\n");
            }
        } else if (buffer[0] == 'c' && buffer[1] == 'a' && buffer[2] == 't') {
            if (buffer[3] == ' ') {
                if (initrd_start) {
                    initrd_cat((const void*)initrd_start, buffer + 4);
                } else {
                    printf("No initrd found\r\n");
                }
            } else {
                printf("Usage: cat [file]\r\n");
            }
        } else if (strcmp(buffer, "buddy") == 0) {
            buddy_demo_case();
        } else if (strcmp(buffer, "alloc") == 0) {
            dynamic_demo_case();
        } else {
            printf("Unknown command: ");
            printf(buffer);
            printf("\r\nUse help to get commands.\r\n");
        }
    }
}

void start_kernel(unsigned long hartid, const void *fdt) {
    init_uart_from_fdt(fdt);
    printf("Hello from Main Kernel!\r\n");
    allocator_init();
    
    run_shell(hartid, fdt);
}
