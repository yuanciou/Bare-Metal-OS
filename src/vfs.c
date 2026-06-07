#include "vfs.h"

#define MAX_FS   16
#define MAX_FD   16
#define PATH_MAX 255

struct mount* rootfs = NULL;
struct filesystem fs_list[MAX_FS];

void vfs_init() {
    struct filesystem* tmpfs = allocate(sizeof(struct filesystem));
    tmpfs->name = "tmpfs";
    tmpfs->setup_mount = tmpfs_setup_mount;
    register_filesystem(tmpfs);
    
    rootfs = allocate(sizeof(struct mount));
    tmpfs->setup_mount(tmpfs, rootfs);
}

int register_filesystem(struct filesystem* fs) {
    for (int i = 0; i < MAX_FS; i++) {
        if (fs_list[i].name == NULL) {
            fs_list[i].name = fs->name;
            fs_list[i].setup_mount = fs->setup_mount;
            return i;
        }
    }
    return -1;
}

int vfs_open(const char* pathname, int flags, struct file** target) {
    struct vnode* vnode;
    int res = vfs_lookup(pathname, &vnode);
    
    if (res != 0) {
        if (flags & O_CREAT) {
            // Find parent directory
            int last_slash = -1;
            for (int i = 0; pathname[i] != '\0'; i++) {
                if (pathname[i] == '/') last_slash = i;
            }
            
            char dirname[PATH_MAX];
            const char* filename;
            
            if (last_slash == -1) {
                // Relative to root or current dir (assume / for now)
                dirname[0] = '/';
                dirname[1] = '\0';
                filename = pathname;
            } else if (last_slash == 0) {
                dirname[0] = '/';
                dirname[1] = '\0';
                filename = pathname + 1;
            } else {
                strncpy(dirname, pathname, last_slash);
                dirname[last_slash] = '\0';
                filename = pathname + last_slash + 1;
            }
            
            struct vnode* dir_vnode;
            if (vfs_lookup(dirname, &dir_vnode) != 0) {
                return -1;
            }
            
            if (dir_vnode->v_ops->create(dir_vnode, &vnode, filename) != 0) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    
    *target = allocate(sizeof(struct file));
    (*target)->vnode = vnode;
    (*target)->flags = flags;
    (*target)->f_pos = 0;
    (*target)->f_ops = vnode->f_ops;
    
    return vnode->f_ops->open(vnode, target);
}

int vfs_close(struct file* file) {
    int res = file->f_ops->close(file);
    free(file);
    return res;
}

int vfs_read(struct file* file, void* buf, size_t len) {
    return file->f_ops->read(file, buf, len);
}

int vfs_write(struct file* file, const void* buf, size_t len) {
    return file->f_ops->write(file, buf, len);
}

int vfs_lookup(const char* pathname, struct vnode** target) {
    if (rootfs == NULL) return -1;
    
    if (strcmp(pathname, "/") == 0 || strlen(pathname) == 0) {
        *target = rootfs->root;
        return 0;
    }

    struct vnode* node = rootfs->root;
    char component[PATH_MAX];
    int start = 0;
    if (pathname[0] == '/') start = 1;

    int i = start;
    while (pathname[i] != '\0') {
        int j = 0;
        while (pathname[i] != '/' && pathname[i] != '\0') {
            component[j++] = pathname[i++];
        }
        component[j] = '\0';
        
        if (j > 0) {
            if (node->v_ops->lookup(node, &node, component) != 0) {
                return -1;
            }
            while (node->mount) {
                node = node->mount->root;
            }
        }
        
        if (pathname[i] == '/') i++;
    }

    *target = node;
    return 0;
}

int vfs_mkdir(const char* pathname) {
    // Basic implementation for mkdir
    // For now, just handle the same logic as O_CREAT but call mkdir
    int last_slash = -1;
    for (int i = 0; pathname[i] != '\0'; i++) {
        if (pathname[i] == '/') last_slash = i;
    }
    
    char dirname[PATH_MAX];
    const char* filename;
    
    if (last_slash == -1) {
        dirname[0] = '/';
        dirname[1] = '\0';
        filename = pathname;
    } else if (last_slash == 0) {
        dirname[0] = '/';
        dirname[1] = '\0';
        filename = pathname + 1;
    } else {
        strncpy(dirname, pathname, last_slash);
        dirname[last_slash] = '\0';
        filename = pathname + last_slash + 1;
    }
    
    struct vnode* dir_vnode;
    if (vfs_lookup(dirname, &dir_vnode) != 0) {
        return -1;
    }
    
    struct vnode* new_vnode;
    return dir_vnode->v_ops->mkdir(dir_vnode, &new_vnode, filename);
}

int vfs_mount(const char* target, const char* filesystem) {
    // Find filesystem
    struct filesystem* fs = NULL;
    for (int i = 0; i < MAX_FS; i++) {
        if (fs_list[i].name && strcmp(fs_list[i].name, filesystem) == 0) {
            fs = &fs_list[i];
            break;
        }
    }
    if (!fs) return -1;

    struct vnode* mount_point;
    if (vfs_lookup(target, &mount_point) != 0) return -1;

    struct mount* mnt = allocate(sizeof(struct mount));
    mnt->fs = fs;
    fs->setup_mount(fs, mnt);
    mount_point->mount = mnt;
    return 0;
}
