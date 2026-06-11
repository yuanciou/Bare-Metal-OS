#include "vfs.h"

#define TMPFS_MAX_FILE_NAME 32
#define TMPFS_MAX_DIR_ENTRY 16
#define TMPFS_MAX_FILE_SIZE 4096

enum fsnode_type { FS_DIR, FS_FILE }; // type of the node in tmpfs, either directory or file

struct tmpfs_vnode {
    enum fsnode_type type; // store the type of this node (dir or file)
    char name[TMPFS_MAX_FILE_NAME]; // name of this node (file or directory name)
    struct vnode* entry[TMPFS_MAX_DIR_ENTRY]; // if (type == DIR) store the children dir/file vnodes in this directory
    char* data; // if (type == FILE) store the file content in this pointer
    size_t size; // if (type == FILE) store the file size in this field
};

int tmpfs_open(struct vnode* file_node, struct file** target);
int tmpfs_close(struct file* file);
int tmpfs_read(struct file* file, void* buf, size_t len);
int tmpfs_write(struct file* file, const void* buf, size_t len);
int tmpfs_lookup(struct vnode* dir_node, struct vnode** target, const char* component_name);
int tmpfs_create(struct vnode* dir_node, struct vnode** target, const char* component_name);
int tmpfs_mkdir(struct vnode* dir_node, struct vnode** target, const char* component_name);

struct file_operations tmpfs_file_ops = {
    .open = tmpfs_open,
    .close = tmpfs_close,
    .read = tmpfs_read,
    .write = tmpfs_write
};

struct vnode_operations tmpfs_vnode_ops = {
    .lookup = tmpfs_lookup,
    .create = tmpfs_create,
    .mkdir = tmpfs_mkdir
};

/**
 * @brief Helper function to create a new vnode for tmpfs with the given type (dir or file). 
 */
struct vnode* tmpfs_create_vnode(enum fsnode_type type) {
    struct vnode* v = allocate(sizeof(struct vnode));
    memset(v, 0, sizeof(struct vnode));
    v->mount = NULL;
    v->v_ops = &tmpfs_vnode_ops;
    v->f_ops = &tmpfs_file_ops;
    v->parent = NULL;

    // that vnode's internal will point to a tmpfs_vnode struct which stores the type (dir or file), name, and content/children of this node
    struct tmpfs_vnode* internal = allocate(sizeof(struct tmpfs_vnode));
    memset(internal, 0, sizeof(struct tmpfs_vnode));
    internal->type = type;

    v->internal = internal;
    return v;
}

int tmpfs_setup_mount(struct filesystem* fs, struct mount* mnt) {
    mnt->root = tmpfs_create_vnode(FS_DIR);
    mnt->fs = fs;
    return 0;
}

int tmpfs_open(struct vnode* file_node, struct file** target) {
    (*target)->vnode = file_node;
    (*target)->f_ops = &tmpfs_file_ops;
    (*target)->f_pos = 0; // initialize file position to 0 when opening
    return 0;
}

int tmpfs_close(struct file* file) {
    // In vfs_close, the file handle itself is freed.
    // Underlying vnode should stay unless we implement unlink.
    return 0;
}

/**
 * @brief Copy data from `f_pos` of the file to `buf`
 */
int tmpfs_read(struct file* file, void* buf, size_t len) {
    struct tmpfs_vnode* inode = file->vnode->internal;
    if (inode->type != FS_FILE) return -1;
    
    // reach EOF
    if (file->f_pos >= inode->size)
        return 0;
    // the bound is the file size
    if (file->f_pos + len > inode->size)
        len = inode->size - file->f_pos;
    memcpy(buf, inode->data + file->f_pos, len);
    file->f_pos += len;
    return len;
}

/**
 * @brief Copy data from `buf` to the file at position `f_pos`
 */
int tmpfs_write(struct file* file, const void* buf, size_t len) {
    struct tmpfs_vnode* inode = file->vnode->internal;
    if (inode->type != FS_FILE) return -1;

    // allocate data buffer
    if (inode->data == NULL) {
        inode->data = allocate(TMPFS_MAX_FILE_SIZE);
    }

    // the bound is the max file size
    if (file->f_pos + len > TMPFS_MAX_FILE_SIZE)
        len = TMPFS_MAX_FILE_SIZE - file->f_pos;
    memcpy(inode->data + file->f_pos, buf, len);
    file->f_pos += len;
    if (file->f_pos > inode->size)
        inode->size = file->f_pos; // update file size if we write beyond the current file size
    return len;
}

int tmpfs_lookup(struct vnode* dir_node,
                 struct vnode** target,
                 const char* component_name) {
    struct tmpfs_vnode* dentry = dir_node->internal;
    if (dentry->type != FS_DIR) return -1;
    
    // leanrly find the target vnode with the given component_name in this directory
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (!dentry->entry[i])
            continue;
        struct tmpfs_vnode* inode = dentry->entry[i]->internal;
        if (strcmp(inode->name, component_name) == 0) {
            *target = dentry->entry[i];
            return 0;
        }
    }
    return -1;
}

int tmpfs_create(struct vnode* dir_node,
                 struct vnode** target,
                 const char* component_name) {
    struct tmpfs_vnode* dir_internal = dir_node->internal;
    if (dir_internal->type != FS_DIR) return -1;
    
    // create a file node
    struct vnode* new_vnode = tmpfs_create_vnode(FS_FILE);
    struct tmpfs_vnode* new_internal = new_vnode->internal;
    strncpy(new_internal->name, component_name, TMPFS_MAX_FILE_NAME - 1);
    new_internal->name[TMPFS_MAX_FILE_NAME - 1] = '\0';
    
    // find a new entry in the directory to put this file node
    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (dir_internal->entry[i] == NULL) {
            dir_internal->entry[i] = new_vnode;
            *target = new_vnode;
            return 0;
        }
    }
    // No space in directory
    free(new_internal);
    free(new_vnode);
    return -1;
}

int tmpfs_mkdir(struct vnode* dir_node,
                struct vnode** target,
                const char* component_name) {
    struct tmpfs_vnode* dir_internal = dir_node->internal;
    if (dir_internal->type != FS_DIR) return -1;

    struct vnode* new_vnode = tmpfs_create_vnode(FS_DIR);
    struct tmpfs_vnode* new_internal = new_vnode->internal;
    strncpy(new_internal->name, component_name, TMPFS_MAX_FILE_NAME - 1);
    new_internal->name[TMPFS_MAX_FILE_NAME - 1] = '\0';

    for (int i = 0; i < TMPFS_MAX_DIR_ENTRY; i++) {
        if (dir_internal->entry[i] == NULL) {
            dir_internal->entry[i] = new_vnode;
            *target = new_vnode;
            return 0;
        }
    }
    free(new_internal);
    free(new_vnode);
    return -1;
}
