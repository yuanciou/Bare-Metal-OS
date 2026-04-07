#ifndef LIST_H
#define LIST_H

#include <stddef.h>

// Doubly Circular Linked List
struct list_head {
    struct list_head *next;
    struct list_head *prev;
};

/* 
Two Types of the INIT Approach 
--------------------------------------------------
1. Directly generate a LIST_HEAN
```
LIST_HEAD(ready_queue);
// -> the compiler will generate it as
// struct list_head ready_queue = { &(ready_queue), &(ready_queue) };
```
--------------------------------------------------
2. Initailize a list head in a struct
```
struct my_struct {
    int data;
    struct list_head list;
};

struct my_struct obj = {
    .data = 10,
    .list = LIST_HEAD_INIT(obj.list)
};
```
*/
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) \
    struct list_head name = LIST_HEAD_INIT(name)


static inline void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

static inline void __list_add(struct list_head *new_node,
                              struct list_head *prev,
                              struct list_head *next) {
    next->prev = new_node;
    new_node->next = next;
    new_node->prev = prev;
    prev->next = new_node;
}

static inline void list_add(struct list_head *new_node, struct list_head *head) {
    // add the new node between `head` and `head->next(first node)`
    __list_add(new_node, head, head->next);
}

static inline void list_add_tail(struct list_head *new_node, struct list_head *head) {
    // add the new node between `head->prev(last node)` and `head`
    __list_add(new_node, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
    // del a node
    next->prev = prev;
    prev->next = next;
}

static inline void list_del(struct list_head *entry) {
    __list_del(entry->prev, entry->next);
    // point the deleted node to itself to avoid dangling pointer
    entry->next = entry;
    entry->prev = entry;
}

static inline int list_empty(const struct list_head *head) {
    // 1: empty; 0: not empty
    return head->next == head;
}

/*
In the list implementation, we can only get the addr of the `list_head`,
but we usually want to get the addr of the struct which contains the `list_head`.

Example:
```
struct task {                     // assume the addr of a task struct is 0x1000
    int id;                       // 4 bytes (addr 1000~1003)
    struct list_head run_list;    // addr at 1004
};
```
1. `container_of`
   - `ptr` is the address of `run_list`, which is 0x1004.
   - offsetof(type, member) is the offset of `run_list` in `struct task`, which is 4.
    - `type` is `struct task`.
    - `member` is `run_list`.
2. `list_entry` -> a more readable wrapper of `container_of`
3. `list_first_entry` -> get the first entry of the list (the first node), instead of the list head.
*/

// offsetof -> get the offset of the member in the struct (type)
// ptr -> the addr of the member in a struct (list_head)
// (char *) -> convert the pointer to char* for byte-level arithmetic
// (type *) -> convert the pointer back to the original struct type
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - offsetof(type, member)))

#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

#define list_first_entry(ptr, type, member) \
    list_entry((ptr)->next, type, member)

#endif // LIST_H