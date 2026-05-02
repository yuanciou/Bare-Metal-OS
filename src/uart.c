#include "config.h"
#include "uart.h"

unsigned long uart_base_addr = 0;
int uart_async = 0;

// =============================================================================
//                         UART Ring Buffer Operations
// =============================================================================
#define RINGBUF_SIZE 2048

typedef struct {
    char buffer[RINGBUF_SIZE];
    volatile int head; // write head -> the next empty position to write
    volatile int tail; // read tail -> the next position to read (oldest data)
} ringbuf_t;

// rx (receive) ring buffer for getc
static ringbuf_t rx_ring = { .head = 0, .tail = 0 };

// tx (transmit) ring buffer for putc
static ringbuf_t tx_ring = { .head = 0, .tail = 0 };

static int ringbuf_push(ringbuf_t *ring, char c) {
    int next = (ring->head + 1) % RINGBUF_SIZE;
    if (next != ring->tail) {
        ring->buffer[ring->head] = c;
        ring->head = next;
        return 1; // success
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

// =============================================================================
//                         Async UART Intterrupt Handling
// =============================================================================
/**
 * @brief Check if the UART interrupt is enabled
          use `sstatus` and the `1 << 1` (SIE (Supervisor Interrupt Enable) bit) 
          to determine if interrupts are enabled in the current execution context.
 */
static int irq_enabled(void) {
    unsigned long sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(sstatus));
    return (sstatus & (1 << 1)) != 0; // SSTATUS_SIE
}

void uart_setup_interrupts() {
    if (uart_base_addr == 0) return;
    *UART_IER |= (1 << 0); // `IER_RDI` bit (Receiver Data Interrupt Enable) to enable receive interrupts
    *UART_MCR |= (1 << 3); // `MCR_OUT2` bit (Output 2) to enable modem control to send interrupts to CPU
    uart_async = 1; // enable async mode
}

/**
 * @brief Try to transmit data from the tx ring buffer to the UART hardware. 
          !! Turn off the THRI bit if the buffer is empty to prevent continuous interrupts.
 */
static void uart_try_tx(void) {
    char c;
    while ((*UART_LSR & LSR_TDRQ) != 0) { // same check as polling putc
        // the hardware is empty (ready to accept a new byte)
        if (!ringbuf_pop(&tx_ring, &c)) { // get a byte from the tx tring buffer
            // !! turn off the THRI (Transmit Holding Register Empty Interrupt) bit
            // since if the buffer is empty and we keep it on.
            // the hardware will keep triggering the interrupt because it's always empty,
            *UART_IER &= (unsigned char)~(1 << 1); // IER_THRI -> 0
            break;
        }
        *UART_THR = c;
    }
}

/**
 * @brief Handle UART interrupts
          Keep reading the UART_IIR (Interrupt Identification Register) until there are no pending interrupts.
          Reading the IIR can effectively clear certain stuck interrupt signals (e.g., THR Empty state).
 */
void handle_uart_interrupt(void) {
    while (1) {
        // read out the IIR
        unsigned int iir = *UART_IIR;
        if ((iir & 1) == 1) { // no pending interrupt when bit 0 is `1`
            break;
        }

        // The Receiver Data Interrupt (RDI) is triggered when there is data available in the UART's receive buffer.
        // push the received data into the rx ring buffer until it's empty
        if ((*UART_LSR & LSR_DR) != 0) {
            while ((*UART_LSR & LSR_DR) != 0) {
                char c = (char)*UART_RBR;
                ringbuf_push(&rx_ring, c);
            }
        }

        // Try to transmit data from the TX ring buffer
        uart_try_tx();
    }
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
    if (uart_async && irq_enabled()) { // check if async mode is enabled
        char c;

        // try to pop a character from the rx ring buffer
        while (!ringbuf_pop(&rx_ring, &c)) {
            // wait for interrupt if the buffer is empty
            asm volatile("wfi");
        }
        return c == '\r' ? '\n' : c;
    } else {
        return uart_getc_polling();
    }
}

char uart_getc_raw() {
    if (uart_async && irq_enabled()) { // check if async mode is enabled
        char c;
        while (!ringbuf_pop(&rx_ring, &c)) {
            asm volatile("wfi");
        }
        return c;
    } else {
        return uart_getc_polling();
    }
}

void uart_putc(char c) {
    if (c == '\n')
        uart_putc('\r');

    if (uart_async && irq_enabled()) { // check if async mode is enabled
        while (!ringbuf_push(&tx_ring, c)) {
            // buffer full, enable tx irq to drain it safely if possible
            *UART_IER |= (1 << 1); // enable the THRI bit to let the interrupt handler drain the buffer
            asm volatile("wfi"); // wait for interrupt (ring buffer has space after draining) 
        }
        *UART_IER |= (1 << 1); // enable the THRI bit to let the interrupt handler drain the buffer
        uart_try_tx(); // attempt to start transfer if idle
    } else {
        uart_putc_polling(c);
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