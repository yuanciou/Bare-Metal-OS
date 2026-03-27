#ifndef CONFIG_H
#define CONFIG_H

// The base address of the UART interface
extern unsigned long uart_base_addr;

#ifdef ORANGE_PI
// Orange Pi

// define the address to avoid bootloader overwrite itself
#define KERNEL_LOAD_ADDR 0x00200000

// Orange Pi UART
#define UART_RBR  (volatile unsigned int*)(uart_base_addr + 0x0)
#define UART_THR  (volatile unsigned int*)(uart_base_addr + 0x0)
#define UART_LSR  (volatile unsigned int*)(uart_base_addr + 0x14)
#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

// ----------------------------------------------------------------------
#else
// QEMU virt

// define the address to avoid bootloader overwrite itself
#define KERNEL_LOAD_ADDR 0x80200000

// QEMU virt UART0 (16550A) addr
#define UART_RBR  (volatile unsigned char*)(uart_base_addr + 0x0)
#define UART_THR  (volatile unsigned char*)(uart_base_addr + 0x0)
#define UART_LSR  (volatile unsigned char*)(uart_base_addr + 0x5)
#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

#endif

#endif // CONFIG_H