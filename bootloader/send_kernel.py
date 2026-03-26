import serial
import sys
import os
import struct
import threading
import time

def read_from_port(ser):
    """
        listen to UART and print the data on the screen
    """
    while True:
        try:
            # read the UART data
            reading = ser.read(1024)
            if reading:
                # output the data on the screen
                # use `.buffer.write`
                sys.stdout.buffer.write(reading)
                sys.stdout.buffer.flush()
        except Exception:
            # if Serial port is disconnected
            break

def send_kernel(ser, kernel_file):
    """
        Send the kernel binary file
    """

    # check if the kernel file exists
    if not os.path.exists(kernel_file):
        print(f"\n[-] Error: Kernel file '{kernel_file}' not found.")
        return

    with open(kernel_file, "rb") as f:
        kernel_data = f.read()

    # Pack the Header: "BOOT" (0x544F4F42) + Size
    """
        '<II' means: 2 unsigned integers (4 bytes each, 8 bytes total) in little-endian format
        0x544F4F42: is the hexadecimal representation of the ASCII string "BOOT" (magic number)
        len(kernel_data): is the size of the kernel binary in bytes (4 byte)
    """
    header = struct.pack('<II', 0x544F4F42, len(kernel_data))

    print(f"\n[*] Preparing to send {kernel_file} ({len(kernel_data)} bytes)...")
    
    print("[*] Sending header...")
    ser.write(header)
    
    # wait for bootloader to be ready
    time.sleep(0.1) 
    
    print("[*] Sending kernel data...")
    ser.write(kernel_data)
    print("[+] Transmission completed successfully!\n")

def main(tty_dev, kernel_file):
    try:
        # Open the serial port
        # QEMU PTY can work with any baudrate, board default is 115200
        ser = serial.Serial(tty_dev, 115200, timeout=0.1)
    except serial.SerialException as e:
        print(f"[-] Failed to connect to {tty_dev}: {e}")
        print("[-] Check QEMU is running and no screen process is holding.")
        sys.exit(1)

    # open a background thread to read from the serial port and print to the screen
    thread = threading.Thread(target=read_from_port, args=(ser,))
    thread.daemon = True # Daemonize thread to exit when main thread exits
    thread.start()

    print(f"[*] Connected to {tty_dev}.")
    print("[*] Press Ctrl+C to exit.\n")
    print("[*] SPECIAL COMMAND: Type 'load' and press Enter to automatically transfer the kernel!")
    print("-" * 50)

    try:
        # read the user input and send it to the serial port
        while True:
            user_input = sys.stdin.readline()
            
            # load command
            if user_input.strip() == "load":
                # send "load" command to bootloader to prepare for receiving the kernel
                ser.write(b"load\n")
                
                # wait for bootloader to be ready
                time.sleep(0.5) 
                
                # send the kernel binary file
                send_kernel(ser, kernel_file)
            else:
                # send the normal command to the serial port
                ser.write(user_input.encode('utf-8'))
                
    except KeyboardInterrupt:
        print("\n[*] Exiting...")
        ser.close()
        sys.exit(0)

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python3 term.py <tty_device> <kernel_file>")
        sys.exit(1)

    tty_device = sys.argv[1]
    kernel_bin = sys.argv[2]
    
    main(tty_device, kernel_bin)