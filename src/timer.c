#include "timer.h"
#include "../lib/sbi.h"
#include "../lib/stdio.h"
#include "../lib/fdt.h"
#include "../lib/endian.h"
#include "../lib/list.h"
#include "allocator.h"
#include "config.h"
#include "task.h"
#include <stdint.h>

#include "uart.h"

// Core Timer frequency
static unsigned long time_freq = 10000000;

struct timer_node {
    unsigned long expire_time;
    void (*callback)(void*);
    void *arg;
    struct list_head list;
};

// 使用未初始化的 struct list_head，讓它落在 bss 區段
static struct list_head timer_list;

static inline unsigned long rdtime(void) {
    unsigned long time;
    asm volatile("rdtime %0" : "=r"(time));
    return time;
}

unsigned long get_time_in_seconds(void) {
    return rdtime() / time_freq;
}

#ifdef ENABLE_PERIODIC_TIMER
static void periodic_timer_callback(void* arg) {
    // 印出開機後經過的秒數
    printf("[Timer] %lu seconds passed since boot.\r\n", get_time_in_seconds());

    // 重新註冊下一個 2 秒的計時器 (Reprogram the timer for the next 2 seconds)
    add_timer(periodic_timer_callback, NULL, 10);
}
#endif

void timer_init(const void *fdt) {
    // 必須在執行期初始化串列節點，才不會因為 Linker 編譯位址 (0x200000) 
    // 與真實載入位址 (0x80200000) 不符，導致存取到錯亂的絕對位址。
    INIT_LIST_HEAD(&timer_list);

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

    sbi_set_timer(-1ULL); // 設定為無限遠未來，直到第一個計時器註冊

#ifdef ENABLE_PERIODIC_TIMER
    // 啟動第一顆 2 秒的計時器
    add_timer(periodic_timer_callback, NULL, 10);
#endif

    // 啟用 Supervisor Timer Interrupt Enable (STIE, bit 5)
    asm volatile("csrs sie, %0" : : "r"(1 << 5));

    // 啟用 Supervisor Interrupt Enable (SIE, bit 1) in sstatus
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1));
}

void add_timer(void (*callback)(void*), void* arg, int sec) {
    unsigned long current_time = rdtime();
    unsigned long expire_time = current_time + (unsigned long)sec * time_freq;
    
    struct timer_node *new_node = (struct timer_node *)allocate(sizeof(struct timer_node));
    if (!new_node) return;
    new_node->expire_time = expire_time;
    new_node->callback = callback;
    new_node->arg = arg;
    INIT_LIST_HEAD(&new_node->list);
    
    // 把 new_node 按照時間順序插入 timer_list 
    // 越早到期的 (數字越小) 排在前面
    struct list_head *pos;
    for (pos = timer_list.next; pos != &timer_list; pos = pos->next) {
        struct timer_node *node = list_entry(pos, struct timer_node, list);
        if (new_node->expire_time < node->expire_time) {
            break;
        }
    }
    // 將 new_node 插入到 pos 的前面
    list_add_tail(&new_node->list, pos);
    
    // 若這個計時器是最前面那顆（代表它最早到期），更新硬體定時器
    if (timer_list.next == &new_node->list) {
        sbi_set_timer(expire_time);
    }
}

void handle_timer_interrupt(void) {
    unsigned long current_time;
    uart_puts("[Timer] Handling Timer Interrupt!\r\n");
    
    while (!list_empty(&timer_list)) {
        current_time = rdtime();
        // 取得最早的計時器
        struct timer_node *head_node = list_entry(timer_list.next, struct timer_node, list);
        
        // 如果到了 (或超過) 它指定的觸發時間
        if (head_node->expire_time <= current_time) {
            // 從串列上拔下來
            list_del(&head_node->list);
            
            // 為了實現 Decoupled Interrupt Handlers
            // 這裡不再直接在 interrupt handler 內執行 callback (這原本是同步、blocking)，
            // 而是將它註冊為高優先權的 Task，交給 run_tasks 在開啟 SIE 的環境下才非同步執行。
            if (head_node->callback) {
                // 給定一個極高優先權 (例如 10)，確保 Timer Callback 回去馬上能被搶佔執行
                uart_puts("[Timer] Timeout completed! Added to Task Queue!\r\n");
                add_task(head_node->callback, head_node->arg, 10);
            }
            
            // 回收記憶體
            free(head_node);
        } else {
            // 這個串列有嚴格按照時間排序，如果頭都還沒到期，後面的更不用檢查了
            break;
        }
    }

    // 將硬體計時器設到下一個鬧鐘，或者關閉它（設為無窮遠）
    if (!list_empty(&timer_list)) {
        struct timer_node *next_node = list_entry(timer_list.next, struct timer_node, list);
        sbi_set_timer(next_node->expire_time);
    } else {
        sbi_set_timer(-1ULL);
    }
}
