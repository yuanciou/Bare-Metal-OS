#include "ramfs.h"
#include "vfs.h"
#include "../lib/cpio.h"
#include "../lib/string.h"
#include "../lib/list.h"
#include "config.h"
#include "mmu.h"

#define RAMFS_MAX_FILE_NAME 256

enum fsnode_type { FS_DIR, FS_FILE };

struct ramfs_inode {
    enum fsnode_type type;  // type of this node (dir or file)
    char name[RAMFS_MAX_FILE_NAME];  // name of this node (file or directory name)
    struct list_head entries; // for directories, list of vnodes
    struct list_head list;    // not use
    const char* data;         // for files, pointer to the file content in initrd
    size_t size;              // for files, size of the file content
    struct vnode* vnode;      // pointer to the vnode corresponding to this inode (for easy access when lookup in ramfs)
};

struct ramfs_dir_entry {
    struct vnode* vnode;
    struct list_head list;
};

int ramfs_open(struct vnode* file_node, struct file** target);
int ramfs_close(struct file* file);
int ramfs_read(struct file* file, void* buf, size_t len);
int ramfs_write(struct file* file, const void* buf, size_t len);
int ramfs_lookup(struct vnode* dir_node, struct vnode** target, const char* component_name);
int ramfs_create(struct vnode* dir_node, struct vnode** target, const char* component_name);
int ramfs_mkdir(struct vnode* dir_node, struct vnode** target, const char* component_name);

struct file_operations ramfs_file_ops = {
    .open = ramfs_open,
    .close = ramfs_close,
    .read = ramfs_read,
    .write = ramfs_write
};

struct vnode_operations ramfs_vnode_ops = {
    .lookup = ramfs_lookup,
    .create = ramfs_create,
    .mkdir = ramfs_mkdir
};

/**
 * @brief Helper function to create a new vnode for ramfs with the given type (dir or file) and name. 
 */
struct vnode* ramfs_create_vnode(enum fsnode_type type, const char* name) {
    struct vnode* v = allocate(sizeof(struct vnode));
    memset(v, 0, sizeof(struct vnode));
    v->mount = NULL;
    v->v_ops = &ramfs_vnode_ops;
    v->f_ops = &ramfs_file_ops;
    v->parent = NULL;

    struct ramfs_inode* inode = allocate(sizeof(struct ramfs_inode));
    memset(inode, 0, sizeof(struct ramfs_inode));
    inode->type = type;
    if (name) {
        strncpy(inode->name, name, RAMFS_MAX_FILE_NAME - 1);
        inode->name[RAMFS_MAX_FILE_NAME - 1] = '\0';
    }
    INIT_LIST_HEAD(&inode->entries);
    inode->vnode = v;

    v->internal = inode;
    return v;
}

static int hextoi(const char* s, int n) {
    int r = 0;
    while (n-- > 0) {
        r = r << 4;
        if (*s >= 'a')
            r += *s++ - 'a' + 10;
        else if (*s >= 'A')
            r += *s++ - 'A' + 10;
        else if (*s >= '0')
            r += *s++ - '0';
    }
    return r;
}

struct cpio_t {
    char magic[6];
    char ino[8];
    char mode[8];
    char uid[8];
    char gid[8];
    char nlink[8];
    char mtime[8];
    char filesize[8];
    char devmajor[8];
    char devminor[8];
    char rdevmajor[8];
    char rdevminor[8];
    char namesize[8];
    char check[8];
};

static unsigned int align_up(unsigned int n, unsigned int a) {
    return (n + a - 1) & ~(a - 1);
}

int ramfs_setup_mount(struct filesystem* fs, struct mount* mnt) {
    extern const void* kernel_fdt;
    extern unsigned long get_initrd_start(const void *fdt);
    
    // get start address of the initrd
    unsigned long initrd_start = get_initrd_start(kernel_fdt);
    if (initrd_start != 0 && initrd_start < PAGE_OFFSET) initrd_start += PAGE_OFFSET;
    
    if (initrd_start == 0) return -1;

    // create root vnode for this mount
    struct vnode* root = ramfs_create_vnode(FS_DIR, "/");
    mnt->root = root;
    mnt->fs = fs;

    const char* ptr = (const char*)initrd_start;
    while (1) {
        // same as before -> read CPIO header
        struct cpio_t* header = (struct cpio_t*)ptr;
        if (strncmp(header->magic, "070701", 6) != 0) break;

        int namesize = hextoi(header->namesize, 8);
        int filesize = hextoi(header->filesize, 8);
        int mode = hextoi(header->mode, 8);
        const char* name = ptr + sizeof(struct cpio_t);

        if (strcmp(name, "TRAILER!!!") == 0) break;

        // Skip the current directory "." entry if it exists in cpio
        if (strcmp(name, ".") == 0) {
            ptr += align_up(sizeof(struct cpio_t) + namesize, 4) + align_up(filesize, 4);
            continue;
        }

        // copy the file name in CPIO so that we can modify it (strtok will change the string)
        struct vnode* curr = root;
        char temp_name[256];
        strncpy(temp_name, name, 255);
        temp_name[255] = '\0';

        // split the file path by strtok, the next token exist or not will determine if the current token is a directory or a file
        char* saveptr;
        char* token = strtok_r(temp_name, "/", &saveptr);
        while (token != NULL) {
            char* next_token = strtok_r(NULL, "/", &saveptr);
            struct vnode* next = NULL;
            
            if (ramfs_lookup(curr, &next, token) == 0) {
                // already exist, just move to the next node
                curr = next;
            } else {
                if (next_token != NULL || (mode & 040000)) {
                    // Directory
                    next = ramfs_create_vnode(FS_DIR, token);
                    next->parent = curr;
                    struct ramfs_inode* curr_inode = curr->internal;
                    struct ramfs_dir_entry* entry = allocate(sizeof(struct ramfs_dir_entry));
                    entry->vnode = next;
                    list_add_tail(&entry->list, &curr_inode->entries);
                    curr = next;
                } else {
                    // File
                    next = ramfs_create_vnode(FS_FILE, token);
                    next->parent = curr;
                    struct ramfs_inode* next_inode = next->internal;
                    next_inode->data = ptr + align_up(sizeof(struct cpio_t) + namesize, 4);
                    next_inode->size = filesize;
                    
                    struct ramfs_inode* curr_inode = curr->internal;
                    struct ramfs_dir_entry* entry = allocate(sizeof(struct ramfs_dir_entry));
                    entry->vnode = next;
                    list_add_tail(&entry->list, &curr_inode->entries);
                    curr = next;
                }
            }
            token = next_token;
        }

        ptr += align_up(sizeof(struct cpio_t) + namesize, 4) + align_up(filesize, 4);
    }

    return 0;
}

int ramfs_open(struct vnode* file_node, struct file** target) {
    (*target)->vnode = file_node;
    (*target)->f_ops = &ramfs_file_ops;
    (*target)->f_pos = 0;
    return 0;
}

int ramfs_close(struct file* file) {
    return 0;
}

int ramfs_read(struct file* file, void* buf, size_t len) {
    struct ramfs_inode* inode = file->vnode->internal;
    if (inode->type != FS_FILE) return -1;
    
    if (file->f_pos >= inode->size) return 0;
    if (file->f_pos + len > inode->size) len = inode->size - file->f_pos;
    
    memcpy(buf, inode->data + file->f_pos, len);
    file->f_pos += len;
    return len;
}

int ramfs_write(struct file* file, const void* buf, size_t len) {
    return -1; // Read only
}

int ramfs_lookup(struct vnode* dir_node, struct vnode** target, const char* component_name) {
    struct ramfs_inode* inode = dir_node->internal;
    if (inode->type != FS_DIR) return -1;

    struct list_head* curr;
    list_for_each(curr, &inode->entries) {
        struct ramfs_dir_entry* entry = list_entry(curr, struct ramfs_dir_entry, list);
        struct ramfs_inode* entry_inode = entry->vnode->internal;
        if (strcmp(entry_inode->name, component_name) == 0) {
            *target = entry->vnode;
            return 0;
        }
    }
    return -1;
}

int ramfs_create(struct vnode* dir_node, struct vnode** target, const char* component_name) {
    return -1; // Read only
}

int ramfs_mkdir(struct vnode* dir_node, struct vnode** target, const char* component_name) {
    return -1; // Read only
}
