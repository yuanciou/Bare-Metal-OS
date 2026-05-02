#ifndef UART_H
#define UART_H

#include <stddef.h>

void uart_init(const void *fdt);
void uart_setup_interrupts(void);
void uart_putc_polling(char c);
char uart_getc_polling(void);
char uart_getc_raw_polling(void);
void uart_putc(char c);
char uart_getc(void);
char uart_getc_raw(void);
void uart_puts(const char* s);
void uart_hex(unsigned long h);
void handle_uart_interrupt(void);

#endif // UART_H