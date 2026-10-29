#include "vfs.h"
#include "thread.h"
#include "ramfs.h"

#define MAX_FS   16
#define MAX_FD   16
#define PATH_MAX 255

struct mount* rootfs = NULL;
struct filesystem fs_list[MAX_FS];

/**
 * @brief Initialization of VFS: register filesystems, setup rootfs and mount other filesystems.
 */
void vfs_init() {
    struct filesystem* tmpfs = allocate(sizeof(struct filesystem));
    memset(tmpfs, 0, sizeof(struct filesystem));
    tmpfs->name = "tmpfs";
    tmpfs->setup_mount = tmpfs_setup_mount;
    register_filesystem(tmpfs);
    
    struct filesystem* ramfs = allocate(sizeof(struct filesystem));
    memset(ramfs, 0, sizeof(struct filesystem));
    ramfs->name = "ramfs";
    ramfs->setup_mount = ramfs_setup_mount;
    register_filesystem(ramfs);

    struct filesystem* devfs = allocate(sizeof(struct filesystem));
    memset(devfs, 0, sizeof(struct filesystem));
    devfs->name = "devfs";
    devfs->setup_mount = devfs_setup_mount;
    register_filesystem(devfs);

    // setup rootfs (/) with filesystem tmpfs
    rootfs = allocate(sizeof(struct mount));
    memset(rootfs, 0, sizeof(struct mount));
    tmpfs->setup_mount(tmpfs, rootfs);
    rootfs->root->parent = rootfs->root; // Root's parent is itself

    // Create /ramfs and mount ramfs
    vfs_mkdir("/ramfs");
    vfs_mount("/ramfs", "ramfs");

    // Create /dev and mount devfs
    vfs_mkdir("/dev");
    vfs_mount("/dev", "devfs");
}

/**
 * @brief Register the filesystem to the fs_list and assign it an ID (index in fs_list).
 */
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

/**
 * @brief Helper function to parse the pathname and get the parent directory path and the child name.
 *        ex: /dir1/dir2/file -> parent: /dir1/dir2, child: file
 */
static void get_parent_and_child(const char* pathname, char* dirname, char* childname) {
    int len = strlen(pathname);
    char temp[PATH_MAX];
    strncpy(temp, pathname, PATH_MAX - 1);
    temp[PATH_MAX - 1] = '\0';

    // Strip trailing slashes (ex: /dir/file/// -> /dir/file)
    while (len > 1 && temp[len - 1] == '/') {
        temp[len - 1] = '\0';
        len--;
    }

    // find the last slash (the separator between parent and child)
    int last_slash = -1;
    for (int i = 0; temp[i] != '\0'; i++) {
        if (temp[i] == '/') last_slash = i;
    }

    if (last_slash == -1) {
        // ex: file (in current directory)
        dirname[0] = '.'; // set parent to current directory
        dirname[1] = '\0';
        strcpy(childname, temp); // child is the whole name
    } else if (last_slash == 0) {
        // Path like /file
        dirname[0] = '/'; // set parent to root directory+
        dirname[1] = '\0';
        strcpy(childname, temp + 1); // child is the part after the first slash
    } else {
        // Path like /dir/file
        strncpy(dirname, temp, last_slash); // parent is the whole part before the last slash
        dirname[last_slash] = '\0';
        strcpy(childname, temp + last_slash + 1); // child is the part after the last slash
    }
}

/**
 * @brief Find the vnode of the given pathname and return it in target. If not found, return -1.
 */
int vfs_open(const char* pathname, int flags, struct file** target) {
    // First try to lookup the vnode of the pathname
    struct vnode* vnode;
    int res = vfs_lookup(pathname, &vnode);
    
    // If lookup fails and O_CREAT flag is set, we try to create the file
    if (res != 0) {
        if (flags & O_CREAT) {
            char dirname[PATH_MAX];
            char filename[PATH_MAX];
            get_parent_and_child(pathname, dirname, filename);
            
            struct vnode* dir_vnode;
            if (vfs_lookup(dirname, &dir_vnode) != 0) {
                return -1;
            }
            
            // create the file in the parent directory
            if (dir_vnode->v_ops->create(dir_vnode, &vnode, filename) != 0) {
                return -1;
            }
            vnode->parent = dir_vnode;
        } else {
            return -1;
        }
    }
    
    // initialize the file handle for this vnode
    *target = allocate(sizeof(struct file));
    memset(*target, 0, sizeof(struct file));
    (*target)->vnode = vnode;
    (*target)->flags = flags;
    (*target)->f_pos = 0;
    (*target)->f_ops = vnode->f_ops;
    (*target)->ref_count = 1;
    
    return vnode->f_ops->open(vnode, target);
}

int vfs_close(struct file* file) {
    file->ref_count--;
    if (file->ref_count > 0) return 0;
    
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

long vfs_lseek64(struct file* file, long offset, int whence) {
    if (file->f_ops->lseek64) {
        return file->f_ops->lseek64(file, offset, whence);
    }
    return -1;
}

int vfs_ioctl(struct file* file, unsigned long request, void* arg) {
    if (file->f_ops->ioctl) {
        return file->f_ops->ioctl(file, request, arg);
    }
    return -1;
}

/**
 * @brief Lookup the vnode of the given pathname and return it in target.
 */
int vfs_lookup(const char* pathname, struct vnode** target) {
    if (rootfs == NULL || pathname == NULL) return -1;
    
    struct vnode* node;
    int i = 0;
    
    // Handle Leading Slashes
    if (pathname[0] == '/') {
        // Start from root if path starts with '/'
        node = rootfs->root;
        while (pathname[i] == '/') i++; // handle multiple leading slashes (ex: ///dir/file)
    } else {
        // Start from current working directory if path is relative
        // If no current working directory, start from root
        thread* cur = get_cur_thread();
        node = (cur && cur->cwd) ? cur->cwd : rootfs->root;
    }

    char component[PATH_MAX];
    while (pathname[i] != '\0') {
        // Downward cross mount point (change to the root of the mounted filesystem)
        while (node->mount) {
            node = node->mount->root;
        }

        // Parse next component (find the internal name between slashes)
        int j = 0;
        while (pathname[i] != '/' && pathname[i] != '\0') {
            component[j++] = pathname[i++];
        }
        component[j] = '\0';
        
        if (strcmp(component, ".") == 0) {
            // current directory, do nothing
        } else if (strcmp(component, "..") == 0) {
            // parent directory, move up to parent if exists (if no parent, stay at current node)
            if (node->parent) {
                node = node->parent;
            }
        } else if (j > 0) {
            struct vnode* next_node;
            // lookup the next component in the current node's children
            if (node->v_ops->lookup(node, &next_node, component) != 0) {
                return -1;
            }

            // Set parent pointer for the next node if not set (some filesystems may not set it during lookup)
            if (next_node->parent == NULL && next_node != rootfs->root) {
                next_node->parent = node;
            }
            node = next_node;
        }

        // Skip consecutive slashes (ex: /dir//file -> /dir/file)
        while (pathname[i] == '/') i++;
    }

    // Final downward cross
    // If the final node is a mount point, we should return the root of the mounted filesystem
    while (node && node->mount) {
        node = node->mount->root;
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
    int res = dir_vnode->v_ops->mkdir(dir_vnode, &new_vnode, filename);
    if (res == 0) {
        new_vnode->parent = dir_vnode;
    }
    return res;
}

int vfs_mount(const char* target, const char* filesystem) {
    // check if the filesystem is registered
    struct filesystem* fs = NULL;
    for (int i = 0; i < MAX_FS; i++) {
        if (fs_list[i].name && strcmp(fs_list[i].name, filesystem) == 0) {
            fs = &fs_list[i];
            break;
        }
    }
    if (!fs) return -1;

    // find the target vnode to mount on
    struct vnode* mount_point;
    if (vfs_lookup(target, &mount_point) != 0) return -1;

    // create a new mount and setup the root vnode of the mounted filesystem
    struct mount* mnt = allocate(sizeof(struct mount));
    memset(mnt, 0, sizeof(struct mount));
    mnt->fs = fs;
    fs->setup_mount(fs, mnt); // the filesystem will initialize the root vnode of this mount in setup_mount
    
    // Set the parent pointer of the root vnode of the mounted filesystem to the parent of the mount point
    if (mount_point == rootfs->root) {
        mnt->root->parent = mnt->root;
    } else {
        mnt->root->parent = mount_point->parent;
    }
    
    // Set the mount pointer of the mount point vnode to this new mount
    mount_point->mount = mnt;
    return 0;
}
