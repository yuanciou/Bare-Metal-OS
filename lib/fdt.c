#include <stdint.h>
#include <stddef.h>
#include "string.h"
#include "fdt.h"

extern unsigned long uart_base_addr;

#define FDT_BEGIN_NODE 0x00000001
#define FDT_END_NODE   0x00000002
#define FDT_PROP       0x00000003
#define FDT_NOP        0x00000004
#define FDT_END        0x00000009

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

static inline uint32_t bswap32(uint32_t x) {
    return ((x & 0xff000000) >> 24) |
           ((x & 0x00ff0000) >> 8) |
           ((x & 0x0000ff00) << 8) |
           ((x & 0x000000ff) << 24);
}

static inline uint64_t bswap64(uint64_t x) {
    return ((uint64_t)bswap32(x & 0xFFFFFFFF) << 32) | bswap32(x >> 32);
}

static inline const void* align_up(const void* ptr, size_t align) {
    return (const void*)(((uintptr_t)ptr + align - 1) & ~(align - 1));
}

static inline int match_path(const char* current, const char* path) {
    while (*path) {   // compare the current path with the target path char by char
        if (*current == *path) {
            current++;
            path++;
        } else if (*current == '@' && *path == '/') {
            // if the target path wants to go down a level (/) but the current node has @unit-address
            // -> ignore all characters between @ and the next node (/)
            while (*current && *current != '/') current++;
        } else {
            return 0;
        }
    }
    // when the target path is end
    // -> if current node is also end ('\0') or is has @unit-address, it's a match
    return (*current == '\0' || *current == '@');
}

int fdt_path_offset(const void* fdt, const char* path) {
    const struct fdt_header* header = (const struct fdt_header*)fdt;
    
    // check the magic number to ensure the .dtb is valid
    // bswap32 () to convert the big-endian to little-endian
    if (bswap32(header->magic) != 0xd00dfeed) {
        return -1;
    }

    // get the begin addr of the structure block
    uint32_t off_dt_struct = bswap32(header->off_dt_struct);
    const char* struct_ptr = (const char*)fdt + off_dt_struct;
    
    char current_path[1024] = "";  // save the current path
    int path_len = 0;

    while (1) {
        uint32_t token = bswap32(*(const uint32_t*)struct_ptr);
        int token_offset = struct_ptr - (const char*)fdt;
        
        // since the token is 4 bytes
        // move the pointer 4 bytes to read the data (or next token) after the token
        struct_ptr += 4;

        if (token == FDT_BEGIN_NODE) {
            // if the token is FDT_BEGIN_NODE
            // -> push the node name to the current path

            // The node name is stored right after the FDT_BEGIN_NODE token
            // and is end with '\0' (C string)
            const char* node_name = struct_ptr;
            int name_len = strlen(node_name);

            // move the pointer to name_len + 1 ('\0')
            // and align it to 4 bytes since the spec
            struct_ptr = align_up(struct_ptr + name_len + 1, 4);
            
            // add the node name to current path
            // then call match_path to check if it match the target path
            if (path_len == 0) {
                strcpy(current_path, "/");
                path_len = 1;
            } else {
                if (path_len > 1) {
                    current_path[path_len] = '/';
                    path_len++;
                }
                strcpy(current_path + path_len, node_name);
                path_len += name_len;
            }

            if (match_path(current_path, path)) {
                return token_offset;
            }
        } else if (token == FDT_END_NODE) {
            // if the token is FDT_END_NODE
            // -> pop the last node from the current path
            if (path_len <= 1) {
                path_len = 0;
                current_path[0] = '\0';
            } else {
                while (path_len > 0 && current_path[path_len - 1] != '/') {
                    path_len--;
                }
                if (path_len > 1) {
                    path_len--; // Remove trailing slash
                }
                current_path[path_len] = '\0';
            }
        } else if (token == FDT_PROP) {
            // since the FDT_PROP is followed by len (4 bytes) and name_off (4 bytes)
            // jump to the next token by adding (4 + 4) bytes + len -> align to 4 bytes
            uint32_t len = bswap32(((const uint32_t*)struct_ptr)[0]);
            struct_ptr += (4 + 4);
            struct_ptr = align_up(struct_ptr + len, 4);
        } else if (token == FDT_NOP) {
            // Do nothing
            // since we made pointer + 4 at the begining
        } else if (token == FDT_END) {
            // node end
            break;
        }
    }

    return -1;
}

const void* fdt_getprop(const void* fdt,
                        int nodeoffset,
                        const char* name,
                        int* lenp) {
    const struct fdt_header* header = (const struct fdt_header*)fdt;
    const char* struct_ptr = (const char*)fdt + nodeoffset;
    const char* strings_block = (const char*)fdt + bswap32(header->off_dt_strings);

    // directly jump to the node offset, check if valid
    uint32_t token = bswap32(*(const uint32_t*)struct_ptr);
    if (token != FDT_BEGIN_NODE) {
        return NULL;
    }
    struct_ptr += 4;
    
    // get node name same as `fdt_path_offset`
    const char* node_name = struct_ptr;
    struct_ptr = align_up(struct_ptr + strlen(node_name) + 1, 4);

    while (1) {
        token = bswap32(*(const uint32_t*)struct_ptr);
        struct_ptr += 4;

        if (token == FDT_PROP) {
            // read the len (data length) and nameoff (the offset of the property name in the strings block)
            uint32_t len = bswap32(((const uint32_t*)struct_ptr)[0]);
            uint32_t nameoff = bswap32(((const uint32_t*)struct_ptr)[1]);
            struct_ptr += 8;

            // use strings_block + nameoff to get the property name
            const char* prop_name = strings_block + nameoff;
            if (strcmp(prop_name, name) == 0) {
                // if (lenp != NULL) store the len as *lenp value
                if (lenp) *lenp = len;
                
                // the property value is stored right after the nameoff, and is len bytes long
                // so we can directly return the pointer to the property value
                return struct_ptr;
            }
            struct_ptr = align_up(struct_ptr + len, 4);
        } else if (token == FDT_NOP) {
            // Do nothing
        } else {
            // FDT_BEGIN_NODE or FDT_END_NODE means we've left the properties area
            break;
        }
    }

    return NULL;
}

void init_uart_from_fdt(const void *fdt) {
    // set a default first just in case
    uart_base_addr = 0x10000000UL; 

    // get stdout-path from /chosen
    int chosen_offset = fdt_path_offset(fdt, "/chosen");
    if (chosen_offset < 0) return;

    // get the property value and length of "stdout-path" in /chosen node
    int len;
    const void *prop = fdt_getprop(fdt, chosen_offset, "stdout-path", &len);
    if (!prop) return;

    // copy the path to avoid modifying the read-only FDT string
    char path[256];
    strcpy(path, (const char *)prop);

    // change ':' to '\0' to get the clean path
    // (e.g. "serial0:115200n8" -> "serial0")
    char *ptr = path;
    while (*ptr) {
        if (*ptr == ':') {
            *ptr = '\0';
            break;
        }
        ptr++;
    }

    // if the path doesn't start with '/' -> alias
    const char *real_path = path;
    if (path[0] != '/') {
        int aliases_offset = fdt_path_offset(fdt, "/aliases");
        if (aliases_offset >= 0) {
            const void *alias_prop = fdt_getprop(fdt, aliases_offset, path, &len);
            if (alias_prop) {
                real_path = (const char *)alias_prop;
            } else {
                return; // Alias not found
            }
        } else {
            return; // No /aliases node
        }
    }

    // get the target UART node offset
    int uart_offset = fdt_path_offset(fdt, real_path);
    if (uart_offset < 0) return;

    // get the "reg" property
    const void *reg_prop = fdt_getprop(fdt, uart_offset, "reg", &len);
    if (!reg_prop) return;

    // big-endian to little-endian
    unsigned long base = bswap64(*(const unsigned long *)reg_prop);
    uart_base_addr = base;
}

unsigned long get_initrd_start(const void *fdt) {
    unsigned long initrd_start = 0;
    if (fdt) {
        int chosen = fdt_path_offset(fdt, "/chosen");
        if (chosen >= 0) {
            int len = 0;
            const void* prop = fdt_getprop(fdt, chosen, "linux,initrd-start", &len);
            
            // Try fallback name if standard one is missing
            if (!prop) {
                prop = fdt_getprop(fdt, chosen, "initrd-start", &len);
            }

            if (prop) {
                if (len == 4) { // QEMU
                    initrd_start = bswap32(*(const uint32_t *)prop);
                } else if (len == 8) { // Orange Pi
                    initrd_start = bswap64(*(const uint64_t *)prop);
                }
            }
        }
    }
    return initrd_start;
}
