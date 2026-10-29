#include "string.h"

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

int strncmp(const char *s1, const char *s2, size_t n) {
    while (n && *s1 && (*s1 == *s2)) {
        ++s1;
        ++s2;
        --n;
    }
    if (n == 0) {
        return 0;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strcpy(char *dest, const char *src) {
    char *d = dest;
    while ((*d++ = *src++));
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n) {
    char *d = dest;
    while (n && (*d++ = *src++)) {
        n--;
    }
    while (n--) {
        *d++ = '\0';
    }
    return dest;
}

void *memset(void *s, int c, size_t n) {
    unsigned char *p = s;
    while (n--) {
        *p++ = (unsigned char)c;
    }
    return s;
}

void *memcpy(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

char *strtok_r(char *str, const char *delim, char **saveptr) {
    char *token;
    if (str == NULL) {
        str = *saveptr;
    }
    if (str == NULL || *str == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    // Skip leading delimiters
    while (*str != '\0') {
        const char *d = delim;
        while (*d != '\0' && *d != *str) d++;
        if (*d == '\0') break;
        str++;
    }

    if (*str == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    token = str;
    // Find end of token
    while (*str != '\0') {
        const char *d = delim;
        while (*d != '\0' && *d != *str) d++;
        if (*d != '\0') break;
        str++;
    }

    if (*str != '\0') {
        *str = '\0';
        *saveptr = str + 1;
    } else {
        *saveptr = NULL;
    }
    return token;
}

static char *__strtok_saveptr;
char *strtok(char *str, const char *delim) {
    return strtok_r(str, delim, &__strtok_saveptr);
}
