#ifndef TIMER_H
#define TIMER_H

void timer_init(const void *fdt);
void handle_timer_interrupt(void);

void add_timer(void (*callback)(void*), void* arg, int sec);
unsigned long get_time_in_seconds(void);

#endif // TIMER_H
