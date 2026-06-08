// proc.h -- load and run a program from a file.
#pragma once
struct thread;
struct thread *proc_spawn(const char *path, int priority);
