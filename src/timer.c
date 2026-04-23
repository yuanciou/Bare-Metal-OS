#include "timer.h"
#include "../lib/sbi.h"
#include "../lib/stdio.h"
#include "../lib/fdt.h"
#include "../lib/list.h"
#include "allocator.h"
#include "config.h"
#include "task.h"
#include "uart.h"
#include <stdint.h>

unsigned long time_freq = TIMER_FREQ_DEFAULT;

struct timer_node {
    unsigned long expire_time; // the 'absolute' expire time in timer ticks
    void (*callback)(void*);   // the callback function
    void *arg;                 // the argument (parameter) to pass to the callback function 
    struct list_head list;     // the list head to link timer nodes
};

static struct list_head timer_list;

/**
 * @brief Read the time CSR using the `rdtime` instruction
 *
 * @return the current time in timer ticks (not seconds, but the raw timer value)
 */
static inline unsigned long rdtime(void) {
    unsigned long time;
    asm volatile("rdtime %0" : "=r"(time));
    return time;
}

/**
 * @brief Convert timer ticks to readable seconds
 */
unsigned long get_time_in_seconds(void) {
    return rdtime() / time_freq;
}

/**
 * @brief The call back function for periodic timer
 */
#ifdef ENABLE_PERIODIC_TIMER
static void periodic_timer_callback(void* arg) {
    // printf("[Timer] %lu seconds passed since boot.\r\n", get_time_in_seconds());

    // Reprogram the timer for the next 2 seconds
    add_timer(periodic_timer_callback, NULL, 10);
}
#endif

/**
 * @brief Init the timer
 */
void timer_init(const void *fdt) {
    // init the timer list head in runtime to get on the right addr
    INIT_LIST_HEAD(&timer_list);

    time_freq = fdt_get_timebase_frequency(fdt, time_freq);
    // printf("Timer frequency: %lu Hz\r\n", time_freq);

    // use sbi to set the timer in a far future until we add some real timers
    sbi_set_timer(-1ULL); 

#ifdef ENABLE_PERIODIC_TIMER
    // add the first prriodic timer
    add_timer(periodic_timer_callback, NULL, 10);
#endif

    // turn on Supervisor Timer Interrupt Enable (STIE, bit 5)
    asm volatile("csrs sie, %0" : : "r"(1 << 5));

    // turn on Supervisor Interrupt Enable (SIE, bit 1) in sstatus to allow receiving timer interrupts
    asm volatile("csrs sstatus, %0" : : "r"(1 << 1));
}

/**
 * @brief Add a timer to the timer list
 */
void add_timer(void (*callback)(void*), void* arg, int sec) {
    // change the relative sec to absolute expire time cycle
    unsigned long current_time = rdtime();
    unsigned long expire_time = current_time + (unsigned long)sec * time_freq;
    
    // allocate a space for timer_node
    struct timer_node *new_node = (struct timer_node *)allocate(sizeof(struct timer_node));
    if (!new_node) return;
    new_node->expire_time = expire_time;
    new_node->callback = callback;
    new_node->arg = arg;
    INIT_LIST_HEAD(&new_node->list); // make the listhead point to itself first
    
    // add the new timer node into the timer listm
    // the sorting is based on the expire_time (expire sonner goes in front)
    struct list_head *pos;
    for (pos = timer_list.next; pos != &timer_list; pos = pos->next) {
        struct timer_node *node = list_entry(pos, struct timer_node, list);
        if (new_node->expire_time < node->expire_time) {
            break;
        }
    }
    // add the new node before pos
    list_add_tail(&new_node->list, pos);
    
    // if the new timer is the earliest one
    // -> update the hardware timer to trigger at the new timer's expire time
    if (timer_list.next == &new_node->list) {
        sbi_set_timer(expire_time);
    }
}

/**
 * @brief Handle the timer interrupt and add the callback function to task queue.
          Also update the hardware timer.
 */
void handle_timer_interrupt(void) {
    unsigned long current_time;
    // uart_puts("[Timer] Handling Timer Interrupt!\r\n");
    
    while (!list_empty(&timer_list)) {
        current_time = rdtime();
        // get the earliest timer node (the head of the list)
        struct timer_node *head_node = list_entry(timer_list.next, struct timer_node, list);
        
        // if the head node is expired (current_time >= expire_time) -> handle it
        if (head_node->expire_time <= current_time) {
            list_del(&head_node->list);
            
            // add the callback funtion to the task queue with a hign priority
            if (head_node->callback) {
                // uart_puts("[Timer] Timeout completed! Added to Task Queue!\r\n");
                add_task(head_node->callback, head_node->arg, 10);
            }
            
            // free the timer node after handling it
            free(head_node);
        } else {
            // since the list is sorted -> no expired timer
            break;
        }
    }

    // add the next timer to the hardwre timer of turn off the timer
    if (!list_empty(&timer_list)) {
        struct timer_node *next_node = list_entry(timer_list.next, struct timer_node, list);
        sbi_set_timer(next_node->expire_time);
    } else {
        sbi_set_timer(-1ULL);
    }
}
