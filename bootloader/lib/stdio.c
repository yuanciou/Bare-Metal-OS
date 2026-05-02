#include "stdio.h"

extern void uart_putc(char c);
extern void uart_puts(const char* s);

static void print_uint(unsigned int num, int base, int width, char pad) {
    char buf[32];
    int i = 0;
    if (num == 0) {
        buf[i++] = '0';
    } else {
        while (num > 0) {
            int rem = num % base;
            buf[i++] = (rem < 10) ? ('0' + rem) : ('a' + rem - 10);
            num /= base;
        }
    }
    while (i < width) {
        buf[i++] = pad;
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

static void print_int(int num, int width, char pad) {
    if (num < 0) {
        uart_putc('-');
        num = -num;
        if (width > 0) width--;
    }
    print_uint((unsigned int)num, 10, width, pad);
}

static void print_long(unsigned long num, int base, int width, char pad) {
    char buf[64];
    int i = 0;
    if (num == 0) {
        buf[i++] = '0';
    } else {
        while (num > 0) {
            int rem = num % base;
            buf[i++] = (rem < 10) ? ('0' + rem) : ('a' + rem - 10);
            num /= base;
        }
    }
    while (i < width) {
        buf[i++] = pad;
    }
    while (i > 0) {
        uart_putc(buf[--i]);
    }
}

int printf(const char *fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    const char *p = fmt;

    while (*p) {
        if (*p == '%') {
            p++;
            if (*p == '%') {
                uart_putc('%');
                p++;
                continue;
            }

            int width = 0;
            char pad = ' ';
            if (*p == '0') {
                pad = '0';
                p++;
            }
            while (*p >= '0' && *p <= '9') {
                width = width * 10 + (*p - '0');
                p++;
            }
            
            int l_count = 0;
            while (*p == 'l') {
                l_count++;
                p++;
            }

            switch (*p) {
                case 'd':
                    print_int(__builtin_va_arg(args, int), width, pad);
                    break;
                case 'u':
                    if (l_count) {
                        print_long(__builtin_va_arg(args, unsigned long), 10, width, pad);
                    } else {
                        print_uint(__builtin_va_arg(args, unsigned int), 10, width, pad);
                    }
                    break;
                case 'x':
                    if (l_count) {
                        print_long(__builtin_va_arg(args, unsigned long), 16, width, pad);
                    } else {
                        print_uint(__builtin_va_arg(args, unsigned int), 16, width, pad);
                    }
                    break;
                case 's': {
                    const char *s = __builtin_va_arg(args, const char *);
                    if (!s) s = "(null)";
                    while (*s) {
                        uart_putc(*s++);
                    }
                    break;
                }
                case 'c':
                    uart_putc((char)__builtin_va_arg(args, int));
                    break;
                default:
                    uart_putc('%');
                    if (*p) uart_putc(*p);
                    break;
            }
        } else {
            if (*p == '\n') {
                // uart_putc usually handles \n -> \r\n, so we just send \n
                uart_putc('\n');
            } else {
                uart_putc(*p);
            }
        }
        if (*p) p++;
    }

    __builtin_va_end(args);
    return 0;
}