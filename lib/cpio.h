#ifndef CPIO_H
#define CPIO_H

void initrd_list(const void* rd);
void initrd_cat(const void* rd, const char* filename);
int initrd_get_file(const void* rd, const char* filename, const char** data, int* size);

#endif // CPIO_H
