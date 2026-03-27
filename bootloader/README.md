# Abstract
This floder contain the **BOOTLOADER** of the kernel.
The bootloader is also such a kernel, but it has the `load` command.
When we write the bootloader on the board, we can use the `load` command to load the new kernel on the board.

# Setup
- On Orange Pi
  Use `ls /dev/` find the `ttyUSB*` device (now we find `/dev/ttyUSB0`), then run:
  ```shell
  make orangepi
  ```
  Then, write the `kernel.fit` to the board.
  - To load the new kernel through bootloader
    ```
    cd ..
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
├── src/
│   ├── config.h      // the macro diefine of different platform
│   ├── link.ld       // define the self relocated addr
│   ├── kernel.its
│   ├── start.S       // handle the bootloader self relocation
│   └── uart.c
├── initramfs.cpio
│── main.c              // add the load command
├── Makefile
├── README.md
└── x1_orangepi-rv2.dtb
```

# Tips
- The `main.c`, `config.h`, `link.ld`, `start.S`, `Makefile` is different form the new kernel. When there is a new feature
need to add on the bootloader, it should be carefully modify.
- The library is depand on the `../lib` folder