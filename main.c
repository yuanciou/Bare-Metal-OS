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
extern void uart_puts(const char* s);
extern void uart_hex(unsigned long h);

static void mem_alloc_demo(void) {
    /***************** Case 2 *****************/

    uart_puts("\n===== Part 1 =====\n");

    void *p1 = allocate(129);
    free(p1);

    uart_puts("\n=== Part 1 End ===\n");

    uart_puts("\n===== Part 2 =====\n");

    // Allocate all blocks at order 0, 1, 2 and 3
    int NUM_BLOCKS_AT_ORDER_0 = 3;  // Need modified
    int NUM_BLOCKS_AT_ORDER_1 = 1;
    int NUM_BLOCKS_AT_ORDER_2 = 1;
    int NUM_BLOCKS_AT_ORDER_3 = 0;

    void *ps0[NUM_BLOCKS_AT_ORDER_0];
    void *ps1[NUM_BLOCKS_AT_ORDER_1];
    void *ps2[NUM_BLOCKS_AT_ORDER_2];
    void *ps3[NUM_BLOCKS_AT_ORDER_3];
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_0; ++i) {
        ps0[i] = allocate(4096);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_1; ++i) {
        ps1[i] = allocate(8192);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_2; ++i) {
        ps2[i] = allocate(16384);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_3; ++i) {
        ps3[i] = allocate(32768);
    }

    uart_puts("\n-----------\n");

    long MAX_BLOCK_SIZE = PAGE_SIZE * (1 << BUDDY_MAX_ORDER);

    /* **DO NOT** uncomment this section */
    void *c1, *c2, *c3, *c4, *c5, *c6, *c7, *c8, *p2, *p3, *p4, *p5, *p6, *p7;

    p1 = allocate(4095);
    free(p1);                        // 4095
    p1 = allocate(4095);

    c1 = allocate(1000);
    c2 = allocate(1023);
    c3 = allocate(999);
    c4 = allocate(1010);
    free(c3);                        // 999
    c5 = allocate(989);  
    c3 = allocate(88);
    c6 = allocate(1001);
    free(c3);                        // 88
    c7 = allocate(2045);
    c8 = allocate(1);

    p2 = allocate(4096);
    free(c8);                        // 1
    p3 = allocate(16000);
    free(p1);                        // 4095
    free(c7);                        // 2045
    p4 = allocate(4097);
    p5 = allocate(MAX_BLOCK_SIZE + 1);
    p6 = allocate(MAX_BLOCK_SIZE);
    free(p2);                        // 4096
    free(p4);                        // 4097
    p7 = allocate(7197);

    free(p6);                        // MAX_BLOCK_SIZE
    free(p3);                        // 16000
    free(p7);                        // 7197
    free(c1);                        // 1000
    free(c6);                        // 1001
    free(c2);                        // 1023
    free(c5);                        // 989
    free(c4);                        // 1010


    uart_puts("\n-----------\n");

    // Free all blocks remaining
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_0; ++i) {
        free(ps0[i]);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_1; ++i) {
        free(ps1[i]);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_2; ++i) {
        free(ps2[i]);
    }
    for (int i = 0; i < NUM_BLOCKS_AT_ORDER_3; ++i) {
        free(ps3[i]);
    }

    uart_puts("\n=== Part 2 End ===\n");
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
            printf("  alloc-demo - run dynamic allocator demo case.\r\n");
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
        } else if (strcmp(buffer, "alloc-demo") == 0) {
            mem_alloc_demo();
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
    allocator_init(fdt);
    
    run_shell(hartid, fdt);
}
