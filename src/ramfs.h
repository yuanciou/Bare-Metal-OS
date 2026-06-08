#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

int ramfs_setup_mount(struct filesystem* fs, struct mount* mnt);

#endif // RAMFS_H
