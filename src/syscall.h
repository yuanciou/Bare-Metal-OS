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
long sys_signal(int signum, void (*handler)());
void sys_sigreturn(struct pt_regs *regs);
int sys_kill(int pid, int signum);

// Handle the syscall dispatched from exception
void handle_syscall(struct pt_regs *regs);

#endif // SYSCALL_H