#include "task.h"
#include "allocator.h"
#include "../lib/stdio.h"
#include <stddef.h>

struct task_node {
    task_callback_t callback;
    void *arg;
    int priority;
    struct task_node *next;
};

static struct task_node *task_queue_head = NULL;
static int current_task_priority = -1;

void add_task(task_callback_t callback, void *arg, int priority) {
    // Disable interrupts during enqueue to prevent concurrent queue mutations
    // Since this function might be called from both normal code and interrupt handlers
    unsigned long saved_sstatus;
    asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
    asm volatile("csrc sstatus, %0" : : "r"(1 << 1)); // Disable SIE

    // Create a new task node
    struct task_node *new_node = (struct task_node *)allocate(sizeof(struct task_node));
    if (new_node) {
        new_node->callback = callback;
        new_node->arg = arg;
        new_node->priority = priority;
        new_node->next = NULL;

        // Insert sorted by priority (higher numerical priority means higher priority)
        if (!task_queue_head || task_queue_head->priority < priority) {
            new_node->next = task_queue_head;
            task_queue_head = new_node;
        } else {
            struct task_node *curr = task_queue_head;
            while (curr->next && curr->next->priority >= priority) {
                curr = curr->next;
            }
            new_node->next = curr->next;
            curr->next = new_node;
        }
    }

    // Restore previous interrupt state
    asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
}

void run_tasks(void) {
    while (1) {
        // Disable interrupts to safely check the queue
        unsigned long saved_sstatus;
        asm volatile("csrr %0, sstatus" : "=r"(saved_sstatus));
        asm volatile("csrc sstatus, %0" : : "r"(1 << 1));

        if (!task_queue_head) { // task queue is empty
            asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
            break;
        }

        if (task_queue_head->priority <= current_task_priority) { // No higher priority task to run
            asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
            break;
        }

        struct task_node *task = task_queue_head;
        task_queue_head = task->next;

        int saved_priority = current_task_priority;
        current_task_priority = task->priority;

        // Re-enable interrupts to allow preemption during task execution
        asm volatile("csrs sstatus, %0" : : "r"(1 << 1));

        // printf("[Task] Start executing task with priority %d\r\n", task->priority);

        task->callback(task->arg);

        // Disable interrupts to clean up
        asm volatile("csrc sstatus, %0" : : "r"(1 << 1));

        current_task_priority = saved_priority;
        free(task);

        // Restore original state
        asm volatile("csrw sstatus, %0" : : "r"(saved_sstatus));
    }
}
