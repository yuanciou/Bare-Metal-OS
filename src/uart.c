#include "config.h"
#include "uart.h"

unsigned long uart_base_addr = 0;
int uart_async = 0;

#define RINGBUF_SIZE 2048

typedef struct {
    char buffer[RINGBUF_SIZE];
    volatile int head;
    volatile int tail;
} ringbuf_t;

static ringbuf_t rx_ring = { .head = 0, .tail = 0 };
static ringbuf_t tx_ring = { .head = 0, .tail = 0 };

static int ringbuf_push(ringbuf_t *ring, char c) {
    int next = (ring->head + 1) % RINGBUF_SIZE;
    if (next != ring->tail) {
        ring->buffer[ring->head] = c;
        ring->head = next;
        return 1;
    }
    return 0; // Full
}

static int ringbuf_pop(ringbuf_t *ring, char *c) {
    if (ring->head != ring->tail) {
        *c = ring->buffer[ring->tail];
        ring->tail = (ring->tail + 1) % RINGBUF_SIZE;
        return 1;
    }
    return 0; // Empty
}

static int ringbuf_empty(ringbuf_t *ring) {
    return ring->head == ring->tail;
}

#define IER_RDI   (1 << 0)
#define IER_THRI  (1 << 1)
#define MCR_OUT2  (1 << 3)

static int irq_enabled(void) {
    unsigned long sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    return (sstatus & (1 << 1)) != 0; // SSTATUS_SIE
}

void uart_setup_interrupts() {
    if (uart_base_addr == 0) return;
    *UART_IER |= IER_RDI;
    *UART_MCR |= MCR_OUT2;
    uart_async = 1;
}

static void uart_try_tx(void) {
    char c;
    while ((*UART_LSR & LSR_TDRQ) != 0) {
        if (!ringbuf_pop(&tx_ring, &c)) {
            *UART_IER &= (unsigned char)~IER_THRI;
            break;
        }
        *UART_THR = c;
    }
}

void handle_uart_interrupt(void) {
    while ((*UART_LSR & LSR_DR) != 0) {
        char c = (char)*UART_RBR;
        ringbuf_push(&rx_ring, c);
    }
    uart_try_tx();
}

char uart_getc_polling() {
    while ((*UART_LSR & LSR_DR) == 0)
        ;
    char c = (char)*UART_RBR;
    return c == '\r' ? '\n' : c;
}

char uart_getc_raw_polling() {
    while ((*UART_LSR & LSR_DR) == 0)
        ;
    return (char)*UART_RBR;
}

void uart_putc_polling(char c) {
    if (c == '\n')
        uart_putc_polling('\r');

    while ((*UART_LSR & LSR_TDRQ) == 0)
        ;
    *UART_THR = c;
}

char uart_getc() {
    if (uart_async && irq_enabled()) {
        char c;
        // uart_puts("async_getc()");
        while (!ringbuf_pop(&rx_ring, &c)) {
            // wait for interrupt
            asm volatile("wfi");
        }
        return c == '\r' ? '\n' : c;
    } else {
        while ((*UART_LSR & LSR_DR) == 0)
            ;
        char c = (char)*UART_RBR;
        return c == '\r' ? '\n' : c;
    }
}

char uart_getc_raw() {
    if (uart_async && irq_enabled()) {
        char c;
        while (!ringbuf_pop(&rx_ring, &c)) {
            asm volatile("wfi");
        }
        return c;
    } else {
        while ((*UART_LSR & LSR_DR) == 0)
            ;
        return (char)*UART_RBR;
    }
}

void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');

    if (uart_async && irq_enabled()) {
        // uart_puts("async_putc()\r\n");
        while (!ringbuf_push(&tx_ring, c)) {
            // buffer full, enable tx irq to drain it safely if possible
            *UART_IER |= IER_THRI;
            asm volatile("wfi");
        }
        *UART_IER |= IER_THRI;
        uart_try_tx(); // attempt to start transfer if idle
    } else {
        while ((*UART_LSR & LSR_TDRQ) == 0)
            ;
        *UART_THR = c;
    }
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