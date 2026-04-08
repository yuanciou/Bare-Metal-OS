# Abstract
The project is aim to wirte the bare metal os on orange pi.

# Setup
## On Orange Pi
- Load the kernel on the orange pi through the bootloader which is already on the board.
- For more infomation, see the `README.md` file in `bootloader/`
Use `ls /dev/` find the `ttyUSB*` device (now we find `/dev/ttyUSB0`), then run:
```shell
make orangepi
sudo python3 send_kernel.py /dev/ttyUSB0 kernel.bin
```

## On QEMU
```shell
make qemu
```

# File Structure
```
.
├── bootloader/             // the bootloader code
├── lib/
│   ├── align.c             // alignment 
│   ├── align.h
│   ├── endian.c            // bswap
│   ├── endian.h
│   ├── buddy.c             // memory buddy system
│   ├── buddy.h
│   ├── cpio.c              // the cpio parser for initramfs
│   ├── cpio.h
│   ├── fdt.c               // the fdt parser for devicetree  
│   ├── fdt.h
│   ├── sbi.c
│   ├── sbi.h
│   ├── stdio.c             // printf
│   ├── stdio.h
│   ├── string.c
│   └── string.h
├── src/
│   ├── config.h            // the macro define of different platform
│   ├── kernel.its
│   ├── link.ld
│   ├── start.S
│   ├── allocator.c         // memory allocator (chunk pool, startup allocation)
│   ├── allocator.h
│   └── uart.c
├── initramfs.cpio
├── main.c
├── Makefile
├── README.md
├── send_kernel.py          // the python script to send kernel over UART
├── qemu.dtb
└── x1_orangepi-rv2.dtb
```

# Feature
- Hello world
  - uart
  - sbi ecall
- Booting
  - bootloader
  - devicetree parsing
  - initial ramdisk
  - bootloader self relocation
- Memory allocator
  - startup allocation (find addr for frame array)
  - reserved memory
  - buddy system
  - dynamic memory allocator (chunk < page size)