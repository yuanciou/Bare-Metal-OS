#ifndef CPIO_H
#define CPIO_H

void initrd_list(const void* rd);
void initrd_cat(const void* rd, const char* filename);

#endif // CPIO_H
