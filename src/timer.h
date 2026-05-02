#ifndef TIMER_H
#define TIMER_H

#define TIMER_FREQ_DEFAULT 10000000UL
extern unsigned long time_freq;

void timer_init(const void *fdt);
void handle_timer_interrupt(void);

void add_timer(void (*callback)(void*), void* arg, int sec);
unsigned long get_time_in_seconds(void);

#endif // TIMER_H
