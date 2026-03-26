# Basic Exercise 1 - UART Bootloader
- On Orange Pi
  Use `ls /dev/` find the `ttyUSB*` device (now we find `/dev/ttyUSB0`), then run:
  ```shell
  make orangepi
  sudo python3 send_kernel.py /dev/ttyUSB0 kernel.bin
  ```

- On QEMU
  ```shell
  make qemu
  ```
  the output should contain:
  ```
  char device redirected to /dev/pts/1 (label serial0)
  ```
  then open a new terminal and run:
  ```shell
  python3 send_kernel.py /dev/pts/1 kernel.bin
  ```
  !! Note that the `dev/pts/1` should be the same number

# Basic Exercise 2 - Devicetree
- To generate qemu.dtb
  ```shell
  qemu-system-riscv64 -nographic -machine virt,dumpdtb=qemu.dtb -m 256M -bios default
  ```

# Basic Exercise 3 - Initial Ramdisk
- Generate CPIO file
  ```shell
  mkdir rootfs
  cd rootfs/
  # put the file contain in initail ramdisk into the floder
  find . | cpio -o -H newc > ../initramfs.cpio
  cd ..
  ```

# File Structure
```
.
├── bootloader/             // the bootloader code
│   ├── lib/
│   ├── initramfs.cpio
│   ├── kernel.its
│   ├── link.ld             // the self relocate addr
│   ├── main.c              // add the load command
│   ├── Makefile
│   ├── README.md
│   ├── send_kernel.py
│   ├── start.S             // handle bootloader self relocation
│   ├── uart.c
│   └── x1_orangepi-rv2.dtb
├── lib/
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
├── initramfs.cpio
├── kernel.its
├── link.ld
├── main.c
├── Makefile
├── README.md
├── send_kernel.py          // the python script to send kernel over UART
├── start.S
├── uart.c
├── x1_orangepi-rv2.dtb
└── x1_orangepi-rv2.dts
```