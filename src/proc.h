// proc.h -- load and run a program from a file.
#pragma once
struct thread;
struct trapframe;
struct thread *proc_spawn(const char *path, int priority);

// exec: replace the CURRENT process's image with the program at `path`.
// On success rewrites *tf to enter the new program and does not conceptually
// return to the old code; returns -1 (image untouched) if the file is missing
// or unloadable.
int proc_exec(struct trapframe *tf, const char *path);
