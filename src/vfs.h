#ifndef VFS_H
#define VFS_H

#include "allocator.h"
#include "../lib/string.h"
#include "../lib/stdio.h"

#define O_CREAT 00000100

struct vnode {
    struct mount* mount; // pointer to the mount this vnode belongs to
    struct vnode_operations* v_ops; // pointer to vnode operations -> lookup, create, mkdir (directory operations)
    struct file_operations* f_ops; // pointer to file operations -> open, close, read, write, lseek64, ioctl (file operations)
    void* internal; // pointer to filesystem-specific data (e.g., tmpfs_node, devfs_node)
    struct vnode* parent; // pointer to parent directory vnode
};

// file handle (which is opened)
struct file {
    struct vnode* vnode; // pointer to the vnode this file handle refers to
    size_t f_pos;  // the position (offset) of this file handle (when r/w/lseek)
    struct file_operations* f_ops; // pointer to file operations (same as vnode's f_ops)
    int flags; // flags used when opening the file (e.g., O_CREAT)
    int ref_count; // reference count for this file handle (for proper cleanup when closed)
};

// mount filesystem point (which is mounted)
struct mount {
    struct vnode* root; // pointer to the root vnode of this mount
    struct filesystem* fs; // the filesystem type of this mount (e.g., tmpfs, devfs)
};

// filesystem type (e.g., tmpfs, devfs)
struct filesystem {
    const char* name; // name of the filesystem (e.g., "tmpfs", "devfs")
    int (*setup_mount)(struct filesystem* fs, struct mount* mount); // function pointer to setup the mount (e.g., initialize root vnode)
};

// file operations (open, close, read, write, lseek64, ioctl)
struct file_operations {
    int (*open)(struct vnode* file_node, struct file** target);
    int (*close)(struct file* file);
    int (*read)(struct file* file, void* buf, size_t len);
    int (*write)(struct file* file, const void* buf, size_t len);
    long (*lseek64)(struct file* file, long offset, int whence);
    int (*ioctl)(struct file* file, unsigned long request, void* arg);
};

// vnode operations (lookup, create, mkdir)
struct vnode_operations {
    int (*lookup)(struct vnode* dir_node, struct vnode** target,
                  const char* component_name);
    int (*create)(struct vnode* dir_node, struct vnode** target,
                  const char* component_name);
    int (*mkdir)(struct vnode* dir_node, struct vnode** target,
                 const char* component_name);
};

extern struct mount* rootfs;

void vfs_init();
int register_filesystem(struct filesystem* fs);
int vfs_open(const char* pathname, int flags, struct file** target);
int vfs_close(struct file* file);
int vfs_write(struct file* file, const void* buf, size_t len);
int vfs_read(struct file* file, void* buf, size_t len);
long vfs_lseek64(struct file* file, long offset, int whence);
int vfs_ioctl(struct file* file, unsigned long request, void* arg);
int vfs_mkdir(const char* pathname);
int vfs_mount(const char* target, const char* filesystem);
int vfs_lookup(const char* pathname, struct vnode** target);

// tmpfs
int tmpfs_setup_mount(struct filesystem* fs, struct mount* mount);

// devfs
int devfs_setup_mount(struct filesystem* fs, struct mount* mount);

#endif // VFS_H
