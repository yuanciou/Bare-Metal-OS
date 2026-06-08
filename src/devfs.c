#include "vfs.h"
#include "uart.h"
#include "allocator.h"
#include "thread.h"
#include "video.h"
#include "../lib/string.h"

int devfs_open(struct vnode* file_node, struct file** target);
int devfs_close(struct file* file);
int devfs_read(struct file* file, void* buf, size_t len);
int devfs_write(struct file* file, const void* buf, size_t len);
long devfs_lseek64(struct file* file, long offset, int whence);
int devfs_ioctl(struct file* file, unsigned long request, void* arg);
int devfs_lookup(struct vnode* dir_node, struct vnode** target, const char* component_name);

struct file_operations devfs_file_ops = {
    .open = devfs_open,
    .close = devfs_close,
    .read = devfs_read,
    .write = devfs_write,
    .lseek64 = devfs_lseek64,
    .ioctl = devfs_ioctl
};

struct vnode_operations devfs_vnode_ops = {
    .lookup = devfs_lookup,
    .create = NULL,
    .mkdir = NULL
};

struct vnode* devfs_create_vnode(const char* name) {
    struct vnode* v = (struct vnode*)allocate(sizeof(struct vnode));
    v->mount = NULL;
    v->v_ops = &devfs_vnode_ops;
    v->f_ops = &devfs_file_ops;
    v->internal = (void*)name;
    v->parent = NULL;
    return v;
}

int devfs_setup_mount(struct filesystem* fs, struct mount* mnt) {
    mnt->root = devfs_create_vnode("root");
    mnt->fs = fs;
    return 0;
}

static struct vnode* uart_vnode = NULL;
static struct vnode* fb_vnode = NULL;

int devfs_lookup(struct vnode* dir_node, struct vnode** target, const char* component_name) {
    if (strcmp(component_name, "uart") == 0) {
        if (!uart_vnode) uart_vnode = devfs_create_vnode("uart");
        *target = uart_vnode;
        return 0;
    }
    if (strcmp(component_name, "fb") == 0) {
        if (!fb_vnode) fb_vnode = devfs_create_vnode("fb");
        *target = fb_vnode;
        return 0;
    }
    return -1;
}

int devfs_open(struct vnode* file_node, struct file** target) {
    (*target)->vnode = file_node;
    (*target)->f_ops = &devfs_file_ops;
    (*target)->f_pos = 0;
    return 0;
}

int devfs_close(struct file* file) {
    return 0;
}

int devfs_read(struct file* file, void* buf, size_t len) {
    if (len == 0) return 0;
    char* b = (char*)buf;
    
    // Wait for at least one character
    int c;
    while ((c = uart_getc_nonblocking()) == -1) {
        schedule();
    }
    b[0] = (char)c;
    size_t count = 1;
    
    // Non-blocking read for the rest
    while (count < len) {
        c = uart_getc_nonblocking();
        if (c == -1) break;
        b[count++] = (char)c;
    }
    
    return (int)count;
}

int devfs_write(struct file* file, const void* buf, size_t len) {
    if (file->vnode == fb_vnode) {
        void* fb_base = video_get_base();
        memcpy((char*)fb_base + file->f_pos, buf, len);
        video_flush((char*)fb_base + file->f_pos, len);
        file->f_pos += len;
        return (int)len;
    }

    const char* b = (const char*)buf;
    for (size_t i = 0; i < len; i++) {
        uart_putc(b[i]);
    }
    return (int)len;
}

long devfs_lseek64(struct file* file, long offset, int whence) {
    if (whence == 0) { // SEEK_SET
        file->f_pos = offset;
        return (long)file->f_pos;
    }
    return -1;
}

int devfs_ioctl(struct file* file, unsigned long request, void* arg) {
    if (file->vnode == fb_vnode && request == FB_IOCTL_GET_INFO) {
        video_get_info((struct framebuffer_info*)arg);
        return 0;
    }
    return -1;
}
