#ifdef ORANGE_PI
// Orange Pi
unsigned long uart_base_addr = 0;
#define UART_RBR  (volatile unsigned int*)(uart_base_addr + 0x0)
#define UART_THR  (volatile unsigned int*)(uart_base_addr + 0x0)
#define UART_LSR  (volatile unsigned int*)(uart_base_addr + 0x14)
#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)
#else
// QEMU virt UART0 (16550A) addr
unsigned long uart_base_addr = 0;
#define UART_RBR  (volatile unsigned char*)(uart_base_addr + 0x0)
#define UART_THR  (volatile unsigned char*)(uart_base_addr + 0x0)
#define UART_LSR  (volatile unsigned char*)(uart_base_addr + 0x5)
#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)
#endif

char uart_getc() {
    while ((*UART_LSR & LSR_DR) == 0)
        ;
    char c = (char)*UART_RBR;
    return c == '\r' ? '\n' : c;
}

char uart_getc_raw() {
    while ((*UART_LSR & LSR_DR) == 0)
        ;

    // Directly return the char without converting
    return (char)*UART_RBR;
}

void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');

    while ((*UART_LSR & LSR_TDRQ) == 0)
        ;
    *UART_THR = c;
}

void uart_puts(const char* s) {
    while (*s)
        uart_putc(*s++);
}

void uart_hex(unsigned long h) {
    uart_puts("0x");
    unsigned long n;
    for (int c = 60; c >= 0; c -= 4) {
        n = (h >> c) & 0xf;
        n += n > 9 ? 0x57 : '0';
        uart_putc(n);
    }
}
