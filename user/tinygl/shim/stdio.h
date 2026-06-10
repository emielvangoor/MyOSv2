// Error-path printing only: TinyGL fprintf()s on misuse. Format args are
// dropped; the message text is what matters.
#pragma once
typedef struct FILE FILE;
extern FILE *stderr;
extern FILE *stdout;
int fprintf(FILE *f, const char *fmt, ...);
int printf(const char *fmt, ...);
typedef __builtin_va_list va_list;
int vfprintf(FILE *f, const char *fmt, va_list ap);
