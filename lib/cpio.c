#include "cpio.h"
#include "string.h"
#include <stddef.h>
#include "stdio.h"
#include "align.h"

extern void uart_putc(char c);

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

/**
 * @brief Convert a hexadecimal string to integer
 *
 * @param s hexadecimal string
 * @param n length of the string
 * @return integer value
 */
static int hextoi(const char* s, int n) {
    int r = 0;
    while (n-- > 0) {
        r = r << 4;
        if (*s >= 'A')
            r += *s++ - 'A' + 10;
        else if (*s >= '0')
            r += *s++ - '0';
    }
    return r;
}

void initrd_list(const void* rd) { // *rd -> the start position of the initramfs
    // since void* cannot do pointer arithmetic (+-)
    // change to char* since char is 1 byte (+N = +(N * sizeof(char)) bytes)
    const char* ptr = (const char*)rd;

    // keep reading cpio headers
    while (1) {
        struct cpio_t* header = (struct cpio_t*)ptr; // read current addr as a cpio header

        // cpio `newc` format has 070701 magic number
        if (strncmp(header->magic, "070701", 6) != 0) {
            printf("Invalid magic\r\n");
            break;
        }

        // since the size in cpio header is save as a hex string
        // -> use hextoi to convert it to integer
        int namesize = hextoi(header->namesize, 8);
        int filesize = hextoi(header->filesize, 8);

        // the name field is right after the cpio header (end with '\0')
        const char* name = ptr + sizeof(struct cpio_t);
        
        if (strcmp(name, "TRAILER!!!") == 0) {
            break;
        }
        
        printf("%d \t %s\r\n", filesize, name);
        
        // since (cpio header + name size) and (file size) are both aligned to 4 bytes
        ptr += align_up_int((int)sizeof(struct cpio_t) + namesize, 4) + align_up_int(filesize, 4);
    }
}

void initrd_cat(const void* rd, const char* filename) {
    // same as the above function
    const char* ptr = (const char*)rd;
    int found = 0; // flag to indicate if the file is found
    while (1) {
        struct cpio_t* header = (struct cpio_t*)ptr;
        if (strncmp(header->magic, "070701", 6) != 0) {
            printf("Invalid magic\r\n");
            break;
        }
        int namesize = hextoi(header->namesize, 8);
        int filesize = hextoi(header->filesize, 8);
        const char* name = ptr + sizeof(struct cpio_t);
        
        if (strcmp(name, "TRAILER!!!") == 0) {
            break;
        }
        
        // change part
        if (strcmp(name, filename) == 0) {
            // skip align4(cpio header + namesize) to get the data part
            const char* data = ptr + align_up_int((int)sizeof(struct cpio_t) + namesize, 4);

            // directly putchar the data 
            for (int i = 0; i < filesize; ++i) {
                if (data[i] == '\n') {
                    uart_putc('\r');
                }
                uart_putc(data[i]);
            }
            printf("\r\n");
            found = 1;
            break;
        }
        
        // if strcmp to filename wrong
        ptr += align_up_int((int)sizeof(struct cpio_t) + namesize, 4) + align_up_int(filesize, 4);
    }
    
    if (!found) {
        printf("File %s not found\r\n", filename);
    }
}
