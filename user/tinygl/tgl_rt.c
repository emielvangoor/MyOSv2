// tgl_rt.c -- the freestanding runtime TinyGL leans on (string/stdlib/stdio/
// math shims over ulib + a little numerics). sqrt/fabs/floor are hardware
// instructions via builtins (the FPU phase); sin/cos are range-reduced
// Taylor series; pow (only used for specular highlights) is exp(y*ln(x)).
#include "ulib.h"

typedef unsigned long size_t;

void *memcpy(void *d, const void *s, size_t n)
{ char *a = d; const char *b = s; while (n--) { *a++ = *b++; } return d; }
void *memset(void *d, int c, size_t n)
{ char *a = d; while (n--) { *a++ = (char)c; } return d; }
void *memmove(void *d, const void *s, size_t n)
{
    char *a = d; const char *b = s;
    if (a < b) { while (n--) { *a++ = *b++; } }
    else { a += n; b += n; while (n--) { *--a = *--b; } }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *x = a, *y = b;
    while (n--) { if (*x != *y) { return *x - *y; } x++; y++; }
    return 0;
}
int strcmp(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *(unsigned char *)a - *(unsigned char *)b; }
size_t strlen(const char *s) { size_t n = 0; while (s[n]) { n++; } return n; }

void *calloc(size_t n, size_t sz)
{ void *p = malloc(n * sz); if (p) { memset(p, 0, n * sz); } return p; }
void *realloc(void *p, size_t n)
{ void *q = malloc(n); if (q && p) { memcpy(q, p, n); free(p); } return q; }
void exit(int code) { sys_exit(code); }

struct FILE { int fd; };
static struct FILE f_out = { 1 }, f_err = { 1 };
struct FILE *stdout = &f_out;
struct FILE *stderr = &f_err;
int fprintf(struct FILE *f, const char *fmt, ...)
{ (void)f; sys_write(1, fmt, (long)strlen(fmt)); sys_write(1, "\n", 1); return 0; }
int printf(const char *fmt, ...)
{ sys_write(1, fmt, (long)strlen(fmt)); return 0; }
int vfprintf(struct FILE *f, const char *fmt, __builtin_va_list ap)
{ (void)f; (void)ap; sys_write(1, fmt, (long)strlen(fmt)); sys_write(1, "\n", 1); return 0; }
void tgl_assert_fail(const char *expr)
{ sys_write(1, "tgl assert: ", 12); sys_write(1, expr, (long)strlen(expr)); sys_exit(1); }

// sin/cos: reduce into [-pi, pi], 7-term Taylor -- plenty for vertex math.
double sin(double x)
{
    const double pi = 3.14159265358979323846, tau = 6.28318530717958647692;
    while (x > pi)  { x -= tau; }
    while (x < -pi) { x += tau; }
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.0/6 + x2 * (1.0/120 + x2 * (-1.0/5040 + x2 * (1.0/362880)))));
}
double cos(double x) { return sin(x + 3.14159265358979323846 / 2.0); }

static double tgl_exp(double x)
{
    double r = 1.0, term = 1.0;
    for (int i = 1; i < 20; i++) { term *= x / i; r += term; }
    return r;
}
static double tgl_log(double x)
{
    // ln via atanh series on (x-1)/(x+1) after scaling x into [0.5, 2).
    double k = 0.0;
    while (x > 2.0)  { x *= 0.5; k += 0.6931471805599453; }
    while (x < 0.5)  { x *= 2.0; k -= 0.6931471805599453; }
    double y = (x - 1.0) / (x + 1.0), y2 = y * y, r = 0.0, t = y;
    for (int i = 1; i < 20; i += 2) { r += t / i; t *= y2; }
    return k + 2.0 * r;
}
double pow(double x, double y)
{
    if (x <= 0.0) { return 0.0; }
    return tgl_exp(y * tgl_log(x));
}
