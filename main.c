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
#include "src/thread.h"

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

unsigned long kernel_hartid;
const void *kernel_fdt;

static volatile int completed_foo = 0;

void run_shell(unsigned long hartid, const void *fdt);

void shell_thread(void) {
    printf("\r\nStarting Shell (PID: %d)...\r\n", get_current()->pid);
    run_shell(kernel_hartid, kernel_fdt);
    thread_exit();
}

void user_program_thread_wrapper(void) {
    thread *cur = get_current();
    char *filename = NULL;
    if (cur) filename = cur->arg;

    unsigned long initrd_start = get_initrd_start(kernel_fdt);

    if (!filename) {
        printf("No program specified for exec wrapper\r\n");
        thread_exit();
    }

    exec(filename, initrd_start);

    // If exec returns, free the filename and exit the thread
    free(filename);
    thread_exit();
}

void foo(void) {
    for (int i = 0; i < 5; i++) {
        printf("Thread id: %d %d\r\n", get_current()->pid, i);
        for (int j = 0; j < 100000000; j++);
        schedule();
    }
    
    // Count how many foo instances have finished
    completed_foo++;
    if (completed_foo == 3) {
        // Once the last foo test completes, start the shell thread
        thread_create(shell_thread);
    }
    thread_exit();
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
                    // Copy program name to heap so each thread has its own copy
                    char *prog = (char *)allocate((unsigned long)(strlen(buffer + 5) + 1));
                    if (!prog) {
                        printf("Failed to allocate memory for program name\r\n");
                        continue;
                    }
                    strcpy(prog, buffer + 5);

                    thread *t = thread_create(user_program_thread_wrapper);
                    if (!t) {
                        free(prog);
                        printf("Failed to create thread for program %s\r\n", prog);
                        continue;
                    }
                    t->arg = prog;
                    printf("Launched %s as PID: %d\r\n", prog, t->pid);
                    // Non-blocking: shell continues immediately
                    while (t->status != THREAD_TERMINATED && t->status != THREAD_ABORTED) {
                        schedule(); // 把 CPU 讓給剛建立的 user_thread
                    }
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

void start_kernel(unsigned long hartid, const void *fdt) {
    // init uart base to enable printf
    init_uart_from_fdt(fdt);
    
    // Memory Allocator
    allocator_init(fdt);
    
    // PLIC Init open UART IRQ Handler
    plic_init(hartid, fdt);
    int uart_irq = uart_get_irq(fdt);
    plic_enable_interrupt(uart_irq);

    // Timer Init (sstatus.SIE open Global Interrupts)
    timer_init(fdt);
    printf("Hello from Main Kernel! Initialization done.\r\n");

    // enable the UART interrupt when we check the above is inti and open
    uart_setup_interrupts();

    kernel_hartid = hartid;
    kernel_fdt = fdt;

    // Initialize thread mechanism and start testing
    init_thread_system(); // Creates idle thread as PID 0

    // (Deferred Shell Thread Creation)
    // We let the foo threads run first so shell doesn't block the CPU.
    // The last foo thread will create the shell thread.

    // To test User Mode, uncomment the thread_create below or let the custom shell launch fork_test using exec
    // thread_create(fork_test_thread);

    // Create foo threads for testing interleaving (PIDs 1, 2, 3)
    for (int i = 0; i < 3; i++) {
        thread_create(foo); 
    }

    printf("Starting idle thread (PID: 0)... Tests will run, then shell will start.\r\n");
    idle();
}
