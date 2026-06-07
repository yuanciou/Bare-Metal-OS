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

static void get_parent_and_child(const char* pathname, char* dirname, char* childname) {
    int len = strlen(pathname);
    char temp[PATH_MAX];
    strncpy(temp, pathname, PATH_MAX - 1);
    temp[PATH_MAX - 1] = '\0';

    // Strip trailing slashes
    while (len > 1 && temp[len - 1] == '/') {
        temp[len - 1] = '\0';
        len--;
    }

    int last_slash = -1;
    for (int i = 0; temp[i] != '\0'; i++) {
        if (temp[i] == '/') last_slash = i;
    }

    if (last_slash == -1) {
        // Relative path without slash (treat as in / for now)
        dirname[0] = '/';
        dirname[1] = '\0';
        strcpy(childname, temp);
    } else if (last_slash == 0) {
        // Path like /file
        dirname[0] = '/';
        dirname[1] = '\0';
        strcpy(childname, temp + 1);
    } else {
        // Path like /dir/file
        strncpy(dirname, temp, last_slash);
        dirname[last_slash] = '\0';
        strcpy(childname, temp + last_slash + 1);
    }
}

int vfs_open(const char* pathname, int flags, struct file** target) {
    struct vnode* vnode;
    int res = vfs_lookup(pathname, &vnode);
    
    if (res != 0) {
        if (flags & O_CREAT) {
            char dirname[PATH_MAX];
            char filename[PATH_MAX];
            get_parent_and_child(pathname, dirname, filename);
            
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
    
    // Handle root path
    if (strcmp(pathname, "/") == 0 || strlen(pathname) == 0) {
        *target = rootfs->root;
        return 0;
    }

    struct vnode* node = rootfs->root;
    char component[PATH_MAX];
    int i = 0;
    if (pathname[0] == '/') i = 1;

    while (pathname[i] != '\0') {
        int j = 0;
        // Skip consecutive slashes
        while (pathname[i] == '/') i++;
        if (pathname[i] == '\0') break;

        while (pathname[i] != '/' && pathname[i] != '\0') {
            component[j++] = pathname[i++];
        }
        component[j] = '\0';
        
        if (j > 0) {
            if (node->v_ops->lookup(node, &node, component) != 0) {
                return -1;
            }
            // Cross mount point
            while (node->mount) {
                node = node->mount->root;
            }
        }
    }

    *target = node;
    return 0;
}

int vfs_mkdir(const char* pathname) {
    char dirname[PATH_MAX];
    char filename[PATH_MAX];
    get_parent_and_child(pathname, dirname, filename);
    
    struct vnode* dir_vnode;
    if (vfs_lookup(dirname, &dir_vnode) != 0) {
        return -1;
    }
    
    struct vnode* new_vnode;
    return dir_vnode->v_ops->mkdir(dir_vnode, &new_vnode, filename);
}

int vfs_mount(const char* target, const char* filesystem) {
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
