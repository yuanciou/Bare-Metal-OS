RISCV_GNU ?= riscv64-unknown-elf
CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy

QEMU ?= qemu-system-riscv64
TARGET = kernel

# 基礎的 CFLAGS (去除了原本 ifeq 的判斷)
CFLAGS = -mcmodel=medany -fno-pie -ffreestanding -nostdlib -g -Wall -march=rv64gc_zifencei -mabi=lp64d -Isrc -Ilib

# ----------------------------------------------------
# 根據下達的指令，自動加上對應的 CFLAGS (取代原本的 ifeq)
# ----------------------------------------------------
qemu: CFLAGS += -DQEMU
orangepi: CFLAGS += -DORANGE_PI

# ----------------------------------------------------
# 定義具體的執行目標 (Targets)
# ----------------------------------------------------

# 如果只打 make，預設執行 qemu
all: qemu

# 執行 `make qemu` 會做的事：清空 -> 編譯 -> 轉 bin -> 執行 qemu
qemu: clean build_common
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin
	$(QEMU) -M virt -m 256M -kernel $(TARGET).bin -initrd initramfs.cpio -display none -serial pty
# 	$(QEMU) -M virt -m 256M -kernel $(TARGET).bin -initrd initramfs.cpio -display none -serial stdio

# 執行 `make orangepi` 會做的事：清空 -> 編譯 -> 轉 bin -> 產生 fit 映像檔
orangepi: clean build_common
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin
	mkimage -f src/kernel.its $(TARGET).fit

# ----------------------------------------------------
# 共同的編譯規則
# ----------------------------------------------------
build_common:
	# 使用 wildcard 自動抓取當前目錄與 lib 目錄下的所有 .c 與 .S 檔
	$(CC) $(CFLAGS) -c $(wildcard *.c *.S src/*.c src/*.S lib/*.c lib/*.S)
	# gcc -c 會將所有編譯出來的 .o 檔放在「當前目錄」，所以可以直接 *.o 連結
	$(LD) -T src/link.ld -o $(TARGET).elf *.o

clean:
	rm -f $(TARGET) $(TARGET).elf $(TARGET).bin $(TARGET).fit *.o

# 宣告這些 target 不是真實的檔案名稱
.PHONY: all qemu orangepi build_common clean