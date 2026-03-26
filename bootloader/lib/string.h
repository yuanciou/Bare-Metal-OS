#ifndef STRING_H
#define STRING_H

#include <stddef.h>

int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
int strlen(const char *s);
char *strcpy(char *dest, const char *src);
void *memset(void *s, int c, size_t n);

#endif // STRING_H
