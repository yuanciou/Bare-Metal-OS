[![Review Assignment Due Date](https://classroom.github.com/assets/deadline-readme-button-22041afd0340ce965d47ae6ef1cefeee28c7c493a6346c4f15d667ab976d596c.svg)](https://classroom.github.com/a/-oo4r1dh)

# UART Bootloader
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

# File Structure
```
.
├── lib
│   ├── cpio.c
│   ├── cpio.h
│   ├── fdt.c
│   ├── fdt.h
│   ├── sbi.c
│   ├── sbi.h
│   ├── stdio.c
│   ├── stdio.h
│   ├── string.c
│   └── string.h
├── initramfs.cpio
├── kernel.its
├── link.ld             // the self relocate addr
│── main.c              // add the load command
├── Makefile
├── README.md
├── send_kernel.py
├── start.S             // handle bootloader self relocation
├── uart.c
└── x1_orangepi-rv2.dtb
```