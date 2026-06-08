// proc.h -- load and run a program from a file.
#pragma once
#include <stdint.h>
struct thread;
struct trapframe;
struct addrspace;
struct thread *proc_spawn(const char *path, int priority);

// exec: replace the CURRENT process's image with the program at `path`, passing
// the NULL-terminated `argv` (or NULL for none) to the new program. On success
// rewrites *tf to enter the new program and does not conceptually return to the
// old code; returns -1 (image untouched) if the file is missing or unloadable.
int proc_exec(struct trapframe *tf, const char *path, char *const argv[]);

// Build the argv block (strings + pointer array) on `as`'s user stack and return
// the 16-byte-aligned starting stack pointer; the argument count is stored in
// *argc_out. Exposed for unit testing the stack layout.
uint64_t proc_setup_argv(struct addrspace *as, char *const argv[], int *argc_out);
