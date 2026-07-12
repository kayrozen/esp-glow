#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  for (size_t i = 0; i < n; i++) d[i] = s[i];
  return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  if (d == s || n == 0) return dst;
  if (d < s) {
    for (size_t i = 0; i < n; i++) d[i] = s[i];
  } else {
    for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
  }
  return dst;
}

void *memset(void *p, int c, size_t n) {
  unsigned char *d = (unsigned char *)p;
  for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
  return p;
}

int memcmp(const void *a, const void *b, size_t n) {
  const unsigned char *x = (const unsigned char *)a;
  const unsigned char *y = (const unsigned char *)b;
  for (size_t i = 0; i < n; i++) {
    if (x[i] != y[i]) return (int)x[i] - (int)y[i];
  }
  return 0;
}

size_t strlen(const char *s) {
  size_t n = 0;
  while (s[n] != '\0') n++;
  return n;
}

int strcmp(const char *a, const char *b) {
  while (*a != '\0' && *a == *b) { a++; b++; }
  return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (a[i] != b[i] || a[i] == '\0') return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
  }
  return 0;
}

char *strcpy(char *dst, const char *src) {
  char *d = dst;
  while ((*d++ = *src++) != '\0') { }
  return dst;
}

char *strchr(const char *s, int c) {
  while (*s != '\0') {
    if (*s == (char)c) return (char *)s;
    s++;
  }
  return (c == '\0') ? (char *)s : (void *)0;
}

size_t strspn(const char *s, const char *accept) {
  size_t n = 0;
  while (s[n] != '\0') {
    const char *a = accept;
    int found = 0;
    while (*a != '\0') { if (*a == s[n]) { found = 1; break; } a++; }
    if (!found) break;
    n++;
  }
  return n;
}

size_t strcspn(const char *s, const char *reject) {
  size_t n = 0;
  while (s[n] != '\0') {
    const char *r = reject;
    int found = 0;
    while (*r != '\0') { if (*r == s[n]) { found = 1; break; } r++; }
    if (found) break;
    n++;
  }
  return n;
}
