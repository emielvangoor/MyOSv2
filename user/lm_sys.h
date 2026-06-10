/* lm_sys.h -- registration hook for the Lisp machine's syscall primitives.
 * User-build only: the kernel KTEST build of the core has no syscalls to
 * wrap, so this must never be included from kernel code. */
#pragma once
void lm_sys_register(void);
void lm_gfx_register(void);   /* display primitives (user/lm_gfx.c) */
struct Writer;  /* lm.h's Writer */
Writer *lm_gfx_tick_writer(void);  /* -frame: per-tick output capture */
