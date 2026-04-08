#ifndef CONFIG_H
#define CONFIG_H

// The base address of the UART interface
extern unsigned long uart_base_addr;

// 1: print the buddy log; 0: disable the buddylog
#define ALLOC_ENABLE_DEMO_LOG 1

// Buddy allocator config (Basic Exercise 1)
#define BUDDY_MAX_ORDER 10
#define MAX_ALLOC_SIZE 2147483647UL // 2GB

// Dynamic allocator config (Basic Exercise 2)
#define PAGE_SHIFT 12UL
#define PAGE_SIZE (1UL << PAGE_SHIFT)

// Buddy allocator runtime layout support
#define BUDDY_MAX_POOL_SIZE (2UL * 1024UL * 1024UL * 1024UL)
#define BUDDY_TOTAL_PAGES (BUDDY_MAX_POOL_SIZE >> PAGE_SHIFT)

// ----------------------------------------------------------------------
#ifdef ORANGE_PI
// Orange Pi

// define the address to avoid bootloader overwrite itself
#define KERNEL_LOAD_ADDR 0x00200000

// Default allocable region for Basic Exercise 1 and fallback boot mode
#define BUDDY_DEFAULT_POOL_START 0x10000000UL
#define BUDDY_DEFAULT_POOL_SIZE  (16UL * 1024UL * 1024UL)

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

// Default allocable region for Basic Exercise 1 and fallback boot mode
#define BUDDY_DEFAULT_POOL_START 0x81000000UL
#define BUDDY_DEFAULT_POOL_SIZE  (16UL * 1024UL * 1024UL)

// QEMU virt UART0 (16550A) addr
#define UART_RBR  (volatile unsigned char*)(uart_base_addr + 0x0)
#define UART_THR  (volatile unsigned char*)(uart_base_addr + 0x0)
#define UART_LSR  (volatile unsigned char*)(uart_base_addr + 0x5)
#define LSR_DR    (1 << 0)
#define LSR_TDRQ  (1 << 5)

#endif

#endif // CONFIG_H