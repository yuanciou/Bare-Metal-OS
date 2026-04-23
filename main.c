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
#include "src/task.h"

void test_task_cb(void *arg) {
    uart_puts("[Task] Executing Priority ");
    uart_puts((char*)arg);
    uart_puts("\r\n");
}

void long_running_task_cb(void *arg) {
    uart_puts("[Task] Long Priority 1 Task started. Busy waiting to catch Timer Interrupt...\r\n");
    // Expand the busy wait duration significantly to guarantee catching multiple
    // 2-second timer tick intervals so we explicitly demonstrate Preemption!
    // enable interrupt temporarily for testing
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1));
    for (volatile int i = 0; i < 9000000; i++) {
        for (volatile int j = 0; j < 600; j++);
    }
    uart_puts("[Task] Long Priority 1 Task finished.\r\n");
}

struct timeout_args {
    char *message;
    int duration;
    unsigned long executed_time;
};

static void timeout_callback(void* arg) {
    struct timeout_args *targs = (struct timeout_args *)arg;
    unsigned long current_time = get_time_in_seconds();
    
    // Asynchronous output from timer interrupt, so we might need to recreate the prompt 
    // depending on the terminal layout, but here we just print what is requested.
    printf("\r\n[%lu] setTimeout: %s (Command executed at: %lu, duration: %d seconds)\r\n# ", 
           current_time, targs->message, targs->executed_time, targs->duration);
    
    // Free the duplicated string and standard argument node
    free(targs->message);
    free(targs);
}

static int my_atoi(const char *str) {
    int res = 0;
    while (*str >= '0' && *str <= '9') {
        res = res * 10 + (*str - '0');
        str++;
    }
    return res;
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
            printf("  exec [file] - execute user program in U-mode.\r\n");
            printf("  setTimeout SECONDS MESSAGE - set a timeout with a message.\r\n");
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
        } else if (strncmp(buffer, "setTimeout ", 11) == 0) {
            char *args = buffer + 11;
            while (*args == ' ') args++;
            
            if (*args < '0' || *args > '9') {
                printf("Usage: setTimeout SECONDS MESSAGE\r\n");
                continue;
            }
            int sec = my_atoi(args);
            
            // Skip the numbers
            while (*args >= '0' && *args <= '9') args++;
            // Skip spaces between sec and message
            while (*args == ' ') args++;
            
            if (*args == '\0') {
                printf("Usage: setTimeout SECONDS MESSAGE\r\n");
                continue;
            }
            
            // Dynamically allocate memory for message (non-blocking shell will overwrite buffer)
            int len = strlen(args);
            char *msg_copy = (char *)allocate((unsigned long)(len + 1));
            if (!msg_copy) {
                printf("Failed to allocate memory for setTimeout\r\n");
                continue;
            }
            strcpy(msg_copy, args);
            
            struct timeout_args *targs = (struct timeout_args *)allocate((unsigned long)sizeof(struct timeout_args));
            if (!targs) {
                free(msg_copy);
                printf("Failed to allocate memory for setTimeout arguments\r\n");
                continue;
            }
            targs->message = msg_copy;
            targs->duration = sec;
            targs->executed_time = get_time_in_seconds();
            
            add_timer(timeout_callback, targs, sec);
        } else {
            printf("Unknown command: ");
            printf(buffer);
            printf("\r\nUse help to get commands.\r\n");
        }
    }
}
int priority_set[4];

void p1_callback(){
    uart_puts("P1 start\n");
    uart_puts("P1 end\n");
}

void p3_callback(){
    uart_puts("P3 start\n");
    add_task(p1_callback, NULL, priority_set[0]);
    add_timer(NULL, NULL, 0);
    uart_puts("P3 end\n");
}

void p2_callback(){
    uart_puts("P2 start\n");
    add_task(p3_callback, NULL, priority_set[2]);
    add_timer(NULL, NULL, 0);
    uart_puts("P2 end\n");
}

void p4_callback(){
    uart_puts("P4 start\n");
    add_task(p2_callback, NULL, priority_set[1]);
    add_timer(NULL, NULL, 0);
    uart_puts("P4 end\n");
}

void test_func(){
    int from_small_to_big = 0; // set to 0 if the task with a smaller number has a higher priority
    if(from_small_to_big){
        priority_set[0] = 10;
        priority_set[1] = 20;
        priority_set[2] = 30;
        priority_set[3] = 40;
    }else{
        priority_set[0] = 40;
        priority_set[1] = 30;
        priority_set[2] = 20;
        priority_set[3] = 10;
    }

    add_task(p4_callback, NULL, priority_set[3]);
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

    // add_task(test_task_cb, "3", 3);
    
    // We add a long running Priority 1 task.
    // Right after timer initialization, a timer is ticking. When the timer interrupts
    // while this Priority 1 task is running, a Priority 10 task (Timer callback)
    // will be enqueued and seamlessly PREEMPT this one!
    // add_task(long_running_task_cb, NULL, 1);
    
    // add_task(test_task_cb, "2", 2);

    // Call run_tasks() manually once to consume these initial tasks so we can see preemption happen!
    // run_tasks();

    // 5. 確保全部基礎建設準備完畢後，最後才把 UART 切換成 Interrupt mode (Async)
    // 避免在啟動途中、甚至 timer_init 還沒正確把 CSR sstatus 等打開前，就卡在 wfi。
    extern void uart_setup_interrupts(void);
    uart_setup_interrupts();

    // We add a long running Priority 1 task.
    // Right after timer initialization, a timer is ticking. When the timer interrupts
    // while this Priority 1 task is running, a Priority 10 task (Timer callback)
    // will be enqueued and seamlessly PREEMPT this one!
    // add_task(long_running_task_cb, NULL, 1);
    
    // add_task(test_task_cb, "2", 2);
    add_timer(test_func, NULL, 0);
    // printf("ttt\r\n");
    run_shell(hartid, fdt);
}
