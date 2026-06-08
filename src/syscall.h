#ifndef SYSCALL_H
#define SYSCALL_H

#include "exception.h"

// System call implementations
long sys_getpid(void);
long sys_uart_read(char *buf, long count);
long sys_uart_write(const char *buf, long count);
int sys_exec(const char *path, struct pt_regs *regs);
long sys_fork(struct pt_regs *parent_regs);
long sys_waitpid(long pid);
void sys_exit(int status);
int sys_stop(long pid);
void sys_display(unsigned int *bmp_image, unsigned int width, unsigned int height);
int sys_usleep(unsigned int usec);
long sys_signal(int signum, void (*handler)());
void sys_sigreturn(struct pt_regs *regs);
int sys_kill(int pid, int signum);
void *sys_mmap(void *addr, unsigned long length, int prot, int flags);

// VFS System calls
int sys_open(const char *pathname, int flags);
int sys_close(int fd);
long sys_read(int fd, void *buf, unsigned long count);
long sys_write(int fd, const void *buf, unsigned long count);
long sys_lseek64(int fd, long offset, int whence);
int sys_ioctl(int fd, unsigned long request, void *arg);
int sys_mkdir(const char *pathname, unsigned mode);
int sys_mount(const char *src, const char *target, const char *filesystem, unsigned long flags, const void *data);
int sys_chdir(const char *path);

// Handle the syscall dispatched from exception
void handle_syscall(struct pt_regs *regs);

#endif // SYSCALL_H