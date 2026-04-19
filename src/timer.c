#include "timer.h"
#include "../lib/sbi.h"
#include "../lib/stdio.h"
#include "../lib/fdt.h"
#include "../lib/endian.h"
#include <stdint.h>

// Core Timer frequency
static unsigned long time_freq = 10000000;

static inline unsigned long rdtime(void) {
    unsigned long time;
    asm volatile("rdtime %0" : "=r"(time));
    return time;
}

void timer_init(const void *fdt) {
    if (fdt) {
        int len;
        // The timebase-frequency property is usually located under /cpus
        int cpus_offset = fdt_path_offset(fdt, "/cpus");
        if (cpus_offset >= 0) {
            const uint32_t *prop = fdt_getprop(fdt, cpus_offset, "timebase-frequency", &len);
            if (prop && len >= 4) {
                // Device tree values are big-endian
                time_freq = bswap32(*prop);
                printf("Found timebase-frequency from FDT: %lu Hz\r\n", time_freq);
            }
        }
    }

    unsigned long current_time = rdtime();
    unsigned long target_time = current_time + 2 * time_freq;

    sbi_set_timer(target_time);

    // 啟用 Supervisor Timer Interrupt Enable (STIE, bit 5)
    asm volatile("csrs sie, %0" : : "r"(1 << 5));

    // 啟用 Supervisor Interrupt Enable (SIE, bit 1) in sstatus
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1));
}

void handle_timer_interrupt(void) {
    unsigned long current_time = rdtime();
    printf("Core timer interrupt! %ld seconds after booting.\r\n", current_time / time_freq);

    // 重新設定下一次是 2 秒後
    unsigned long target_time = current_time + 2 * time_freq;
    sbi_set_timer(target_time);
}
