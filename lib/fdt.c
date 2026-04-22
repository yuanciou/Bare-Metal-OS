#include <stdint.h>
#include <stddef.h>
#include "endian.h"
#include "string.h"
#include "fdt.h"
#include "align.h"

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
            struct_ptr = align_up_ptr(struct_ptr + name_len + 1, 4);
            
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
            struct_ptr = align_up_ptr(struct_ptr + len, 4);
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
    struct_ptr = align_up_ptr(struct_ptr + strlen(node_name) + 1, 4);

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
            struct_ptr = align_up_ptr(struct_ptr + len, 4);
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
    if (!reg_prop || len < 4) return;

    // Determine address cells from root or assume 2
    int root_offset = fdt_path_offset(fdt, "/");
    int ac_len;
    const uint32_t* ac_prop = fdt_getprop(fdt, root_offset, "#address-cells", &ac_len);
    int addr_cells = ac_prop ? bswap32(*ac_prop) : 2;

    // Read base address safely to avoid unaligned access faults
    const uint32_t *reg_words = (const uint32_t *)reg_prop;
    unsigned long base = 0;
    if (addr_cells == 2 && len >= 8) {
        base = ((unsigned long)bswap32(reg_words[0]) << 32) | bswap32(reg_words[1]);
    } else {
        base = bswap32(reg_words[0]);
    }
    uart_base_addr = base;
}

unsigned long fdt_totalsize(const void *fdt) {
    const struct fdt_header *header = (const struct fdt_header *)fdt;

    if (!fdt) {
        return 0;
    }
    if (bswap32(header->magic) != 0xd00dfeed) {
        return 0;
    }

    return (unsigned long)bswap32(header->totalsize);
}

static unsigned long parse_addr_prop(const void *prop, int len) {
    if (!prop) {
        return 0;
    }
    if (len == 4) {  // QEMU
        return (unsigned long)bswap32(*(const uint32_t *)prop);
    }
    if (len == 8) {  // Orange Pi
        return (unsigned long)(((unsigned long)bswap32(*(const uint32_t *)prop) << 32) | bswap32(*((const uint32_t *)prop + 1)));
    }

    return 0;
}

/**
 * @brief Parse the addr property in /chosen node.
 */
static unsigned long get_chosen_addr_prop(const void *fdt,
                                          const char *name,
                                          const char *fallback_name) {
    int chosen;
    int len = 0;
    const void *prop;

    if (!fdt) {
        return 0;
    }

    chosen = fdt_path_offset(fdt, "/chosen");
    if (chosen < 0) {
        return 0;
    }

    prop = fdt_getprop(fdt, chosen, name, &len);
    if (!prop && fallback_name) {
        prop = fdt_getprop(fdt, chosen, fallback_name, &len);
    }

    return parse_addr_prop(prop, len);
}

/**
 * @brief Get the start addr of initial ramdisk in /chosen node.
 */
unsigned long get_initrd_start(const void *fdt) {
    return get_chosen_addr_prop(fdt, "linux,initrd-start", "initrd-start");
}

unsigned long get_initrd_end(const void *fdt) {
    return get_chosen_addr_prop(fdt, "linux,initrd-end", "initrd-end");
}

/**
 * @brief Parse the type define as `prop = <start size>` in the FDT>
    Example: reg = <0x00 0xc0800000 0x00 0x40000>;
    Example: alloc-ranges = <0x00 0x40000000 0x00 0x30000000>;

 *
 * @param start the start address to be returned
 * @param size the size to be returned
 */
static int parse_start_and_size(const void *reg_prop,
                                 int len,
                                 unsigned long *start,
                                 unsigned long *size) {
    if (!reg_prop || len <= 0) {
        return -1;
    }

    if (len >= 16) { // 64-bit -> Orange Pi
        *start = (unsigned long)(((unsigned long)bswap32(*(const uint32_t *)reg_prop) << 32) | bswap32(*((const uint32_t *)reg_prop + 1)));
        *size = (unsigned long)((((unsigned long)bswap32(*((const uint32_t *)reg_prop + 2))) << 32) | bswap32(*((const uint32_t *)reg_prop + 3)));
        return 0;
    }

    if (len >= 8) { // 32-bit -> QEMU
        *start = (unsigned long)bswap32(*(const uint32_t *)reg_prop);
        *size = (unsigned long)bswap32(*((const uint32_t *)reg_prop + 1));
        return 0;
    }

    return -1;
}

/**
 * @brief Parse the type define as `alloc-ranges` and `size` in the FDT.
    Example: 
        linux,cma {
                compatible = "shared-dma-pool";
                alloc-ranges = <0x00 0x40000000 0x00 0x30000000>;
                size = <0x00 0x18000000>;
                alignment = <0x00 0x100000>;
                linux,cma-default;
                reusable;
            };
 *
 * @param alloc_ranges_prop the pointer to the `alloc-ranges` property value
 * @param alloc_ranges_len the length of the `alloc-ranges` property value
 * @param size_prop the pointer to the `size` property value
 * @param size_len the length of the `size` property value
 * @param start the start addr to be returned
 * @param size the size to be returned
 */
static int parse_alloc_ranges_with_size(const void *alloc_ranges_prop,
                                        int alloc_ranges_len,
                                        const void *size_prop,
                                        int size_len,
                                        unsigned long *start,
                                        unsigned long *size) {
    unsigned long range_size = 0;   // the size defined in alloc-ranges, which is the upper bound of the reserved region
    unsigned long requested_size = 0;   // the size defined in size property, which is the requested reserved size

    // parse the alloc-ranges to get the start addr and the range size
    if (parse_start_and_size(alloc_ranges_prop,
                              alloc_ranges_len,
                              start,
                              &range_size) != 0) {
        return -1;
    }

    // parse the size property to get the requested reserved size
    if (!size_prop || size_len <= 0) {
        return -1;
    }

    if (size_len >= 8) {
        requested_size = (unsigned long)(((unsigned long)bswap32(*(const uint32_t *)size_prop) << 32) | bswap32(*((const uint32_t *)size_prop + 1)));
    } else if (size_len >= 4) {
        requested_size = (unsigned long)bswap32(*(const uint32_t *)size_prop);
    } else {
        return -1;
    }

    if (requested_size == 0) {
        return -1;
    }

    // if request > range -> use range size
    if (range_size != 0 && requested_size > range_size) {
        requested_size = range_size;
    }

    *size = requested_size;
    return 0;
}

/**
 * @brief Parse the reserved memory regions in `reserved-memory` node
 *
 * @param fdt the pointer to the FDT blob
 * @param index the index of the reserved region to be parsed (0-based) -> check not return the same one
 * @param start the pointer to store the start address of the reserved region
 * @param size the pointer to store the size of the reserved region
 */
int fdt_get_reserved_memory_region(const void *fdt,
                                   int index,
                                   unsigned long *start,
                                   unsigned long *size) {
    int reserved_offset;
    const char *struct_ptr;
    int depth = 1;
    int current = 0;
    int len = 0;
    const void *reg_prop;
    const void *size_prop;
    const void *alloc_ranges_prop;
    int size_len = 0;
    int alloc_ranges_len = 0;

    if (!fdt || index < 0 || !start || !size) {
        return -1;
    }

    // find the `reserved-memory` node
    reserved_offset = fdt_path_offset(fdt, "/reserved-memory");
    if (reserved_offset < 0) {
        return -1;
    }

    // check if the reserved-memory node itself has the reg property
    reg_prop = fdt_getprop(fdt, reserved_offset, "reg", &len);
    if (parse_start_and_size(reg_prop, len, start, size) == 0) { // success
        if (current == index) {
            return 0;
        }
        current++;
    }

    // in the reserved-memory node
    struct_ptr = (const char *)fdt + reserved_offset;
    if (bswap32(*(const uint32_t *)struct_ptr) != FDT_BEGIN_NODE) {
        return -1;
    }

    // move through the token and the node name to reach the first child node
    struct_ptr += 4; // skip the FDT_BEGIN_NODE token
    struct_ptr = align_up_ptr(struct_ptr + strlen(struct_ptr) + 1, 4); // skip the node name and align to 4 bytes

    while (depth > 0) { // when depth == 0 -> leave the reserved-memory node
        const char *token_ptr = struct_ptr;
        uint32_t token = bswap32(*(const uint32_t *)token_ptr);

        struct_ptr += 4; // the token is 4 bytes

        if (token == FDT_BEGIN_NODE) {
            // move througn the node name and depth++ to enter the child node
            int child_offset = (int)(token_ptr - (const char *)fdt);
            const char *child_name = struct_ptr;

            struct_ptr = align_up_ptr(struct_ptr + strlen(child_name) + 1, 4);
            depth++;

            if (depth == 2) { // the child node of reserved-memory node, which defines a reserved region
                // the `reg = <start size>` type
                reg_prop = fdt_getprop(fdt, child_offset, "reg", &len);
                if (parse_start_and_size(reg_prop, len, start, size) == 0) {
                    if (current == index) {
                        return 0;
                    }
                    current++;
                    continue;
                }

                // the `alloc-ranges` and `size` type
                alloc_ranges_prop = fdt_getprop(fdt,
                                                child_offset,
                                                "alloc-ranges",
                                                &alloc_ranges_len);
                size_prop = fdt_getprop(fdt, child_offset, "size", &size_len);
                if (parse_alloc_ranges_with_size(alloc_ranges_prop,
                                                alloc_ranges_len,
                                                size_prop,
                                                size_len,
                                                start,
                                                size) == 0) {
                    if (current == index) {
                        return 0;
                    }
                    current++;
                }
            }
        } else if (token == FDT_END_NODE) {
            // depth-- to leave the current node
            depth--;
        } else if (token == FDT_PROP) {
            // skip the property (len + nameoff + value) to reach the next token
            uint32_t prop_len = bswap32(((const uint32_t *)struct_ptr)[0]);

            struct_ptr += 8; // skip the len and nameoff
            struct_ptr = align_up_ptr(struct_ptr + prop_len, 4); // skip the property value and align to 4 bytes
        } else if (token == FDT_NOP) {
            continue;
        } else if (token == FDT_END) {
            break;
        }
    }

    return -1;
}

/**
 * @brief Find the offset of `phandle = <interrupt-patent value>`
 */
int fdt_get_node_by_phandle(const void* fdt, uint32_t phandle) {
    const struct fdt_header* header = (const struct fdt_header*)fdt;
    if (bswap32(header->magic) != 0xd00dfeed) return -1;
    
    uint32_t off_dt_struct = bswap32(header->off_dt_struct);
    const char* struct_ptr = (const char*)fdt + off_dt_struct;
    const char* strings_block = (const char*)fdt + bswap32(header->off_dt_strings);

    int current_node_offset = struct_ptr - (const char*)fdt;

    while (1) {
        uint32_t token = bswap32(*(const uint32_t*)struct_ptr);
        int token_offset = struct_ptr - (const char*)fdt;
        struct_ptr += 4;

        if (token == FDT_BEGIN_NODE) {
            current_node_offset = token_offset;
            const char* node_name = struct_ptr;
            struct_ptr = align_up_ptr(struct_ptr + strlen(node_name) + 1, 4);
        } else if (token == FDT_END_NODE) {
        } else if (token == FDT_PROP) {
            uint32_t len = bswap32(((const uint32_t*)struct_ptr)[0]);
            uint32_t nameoff = bswap32(((const uint32_t*)struct_ptr)[1]);
            const char* prop_name = strings_block + nameoff;
            struct_ptr += 8;

            if (strcmp(prop_name, "phandle") == 0 || strcmp(prop_name, "linux,phandle") == 0) {
                if (len >= 4) {
                    uint32_t val = bswap32(*(const uint32_t*)struct_ptr);
                    if (val == phandle) {
                        return current_node_offset;
                    }
                }
            }
            struct_ptr = align_up_ptr(struct_ptr + len, 4);
        } else if (token == FDT_NOP) {
        } else if (token == FDT_END) {
            break;
        }
    }
    return -1;
}

/**
 * @brief Find the PLIC base addr
 */
unsigned long fdt_get_plic_base(const void* fdt) {
    int uart_offset = fdt_path_offset(fdt, "/chosen");
    if (uart_offset >= 0) {
        int len;
        const void *prop = fdt_getprop(fdt, uart_offset, "stdout-path", &len);
        if (prop) {
            char path[256];
            int i = 0;
            while(i < len && ((const char*)prop)[i] && ((const char*)prop)[i] != ':') {
                path[i] = ((const char*)prop)[i];
                i++;
            }
            path[i] = '\0';
            
            // Re-find actual UART node or alias
            uart_offset = fdt_path_offset(fdt, path);
            if (uart_offset < 0) {
                int aliases_offset = fdt_path_offset(fdt, "/aliases");
                if (aliases_offset >= 0) {
                    const void *alias_prop = fdt_getprop(fdt, aliases_offset, path, &len);
                    if (alias_prop) {
                        uart_offset = fdt_path_offset(fdt, (const char*)alias_prop);
                    }
                }
            }
        }
    }
    
    if (uart_offset < 0) return 0;

    int len;
    const uint32_t *iparent = fdt_getprop(fdt, uart_offset, "interrupt-parent", &len);
    if (!iparent) return 0;

    unsigned long plic_base = 0;
    uint32_t plic_phandle = bswap32(*iparent);
    int plic_offset = fdt_get_node_by_phandle(fdt, plic_phandle);

    if (plic_offset >= 0) {
        const uint32_t *reg = fdt_getprop(fdt, plic_offset, "reg", &len);
        if (reg) {
            int root_offset = fdt_path_offset(fdt, "/");
            const uint32_t* ac_prop = fdt_getprop(fdt, root_offset, "#address-cells", &len);
            int addr_cells = ac_prop ? bswap32(*ac_prop) : 2;

            if (addr_cells == 2) {
                plic_base = ((uint64_t)bswap32(reg[0]) << 32) | bswap32(reg[1]);
            } else {
                plic_base = bswap32(reg[0]);
            }
        }
    }
    return plic_base;
}

/**
 * @brief Find the UART IRQ
 */
int g_uart_irq = 10;
int uart_get_irq(const void* fdt) {
    if (!fdt) return 10;
    int uart_offset = fdt_path_offset(fdt, "/chosen");
    if (uart_offset >= 0) {
        int len;
        const void *prop = fdt_getprop(fdt, uart_offset, "stdout-path", &len);
        if (prop) {
            char path[256];
            int i = 0;
            while(i < len && ((const char*)prop)[i] && ((const char*)prop)[i] != ':') {
                path[i] = ((const char*)prop)[i];
                i++;
            }
            path[i] = '\0';
            
            uart_offset = fdt_path_offset(fdt, path);
            if (uart_offset < 0) {
                int aliases_offset = fdt_path_offset(fdt, "/aliases");
                if (aliases_offset >= 0) {
                    const void *alias_prop = fdt_getprop(fdt, aliases_offset, path, &len);
                    if (alias_prop) {
                        uart_offset = fdt_path_offset(fdt, (const char*)alias_prop);
                    }
                }
            }
        }
    }
    if (uart_offset >= 0) {
        int len;
        const unsigned int *irq_prop = fdt_getprop(fdt, uart_offset, "interrupts", &len);
        if (irq_prop && len >= 4) {
            unsigned int irq = bswap32(*irq_prop);
            g_uart_irq = (int)irq;
            return irq;
        }
    }

    // Default QEMU virt UART IRQ
    g_uart_irq = 10;
    return 10;
}
