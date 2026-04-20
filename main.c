#include "lib/string.h"
#include "lib/sbi.h"
#include "lib/cpio.h"
#include "lib/stdio.h"
#include "lib/fdt.h"
#include "config.h"
#include "allocator.h"
#include "src/exception.h"
#include "src/timer.h"
#include "src/plic.h"
#include "src/uart.h"

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
            printf("  exec [file] - execute user program in U-mode.\r\n");
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
        } else if (buffer[0] == 'e' && buffer[1] == 'x' && buffer[2] == 'e' && buffer[3] == 'c') {
            if (buffer[4] == ' ') {
                if (initrd_start) {
                    exec(buffer + 5, initrd_start);
                } else {
                    printf("No initrd found\r\n");
                }
            } else {
                printf("Usage: exec [file]\r\n");
            }
        } else {
            printf("Unknown command: ");
            printf(buffer);
            printf("\r\nUse help to get commands.\r\n");
        }
    }
}

void start_kernel(unsigned long hartid, const void *fdt) {
    // 1. 先初始化基礎 UART 與解析，確保可以呼叫 printf
    init_uart_from_fdt(fdt);
    
    // 2. Memory Allocator
    allocator_init(fdt);
    
    // 3. PLIC Init 與開啟對應的 UART IRQ Handler 機制
    plic_init(hartid, fdt);
    int uart_irq = uart_get_irq(fdt);
    printf("PLIC initialized.\r\nUART IRQ: %d\r\n", uart_irq);
    plic_enable_interrupt(uart_irq);

    // 4. Timer Init (內部會設定 sstatus.SIE 打開 Global Interrupts)
    timer_init(fdt);

    printf("Hello from Main Kernel! Initialization done.\r\n");

    // 5. 確保全部基礎建設準備完畢後，最後才把 UART 切換成 Interrupt mode (Async)
    // 避免在啟動途中、甚至 timer_init 還沒正確把 CSR sstatus 等打開前，就卡在 wfi。
    extern void uart_setup_interrupts(void);
    uart_setup_interrupts();

    run_shell(hartid, fdt);
}
