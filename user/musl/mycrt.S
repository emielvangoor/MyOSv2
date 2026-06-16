// mycrt.S -- a minimal C runtime the on-device TCC links programs against.
//
// TCC compiles a user's C on MyOSv2 but has no libc/crt of its own here. This
// gcc-assembled stub supplies the two things every freestanding C program
// needs: an ELF entry point (_start) that calls main() and turns its return
// value into the exit status, and string-output helpers (TCC's own inline
// assembler is too limited to emit `svc` reliably, so the syscalls live here,
// assembled by aarch64-linux-musl-gcc). Linux/aarch64 syscall ABI: number in
// x8, args x0..x5, `svc #0`, result in x0.
//   puts(s)        write the NUL-terminated string s to stdout (computes len)
//   print(buf,len) write exactly len bytes of buf to stdout

    .text

    .global _start
_start:
    bl   main              // x0 = main()'s return value
    mov  x8, #94            // SYS_exit_group
    svc  #0                 // exit_group(x0): the C return becomes the status

    // print(const char *buf [x0], long len [x1]) -> write(1, buf, len)
    .global print
print:
    mov  x2, x1            // len
    mov  x1, x0            // buf
    mov  x0, #1            // fd = stdout
    mov  x8, #64           // SYS_write
    svc  #0
    ret

    // puts(const char *s [x0]) -> write(1, s, strlen(s)). No hand-counting:
    // edit the string, recompile, and the whole new string prints.
    .global puts
puts:
    mov  x9, x0           // x9 = start of string
    mov  x1, x0           // x1 = walker
.Lputs_scan:
    ldrb w2, [x1]         // *x1
    cbz  w2, .Lputs_write // NUL -> done scanning
    add  x1, x1, #1
    b    .Lputs_scan
.Lputs_write:
    sub  x2, x1, x9       // len = end - start
    mov  x1, x9           // buf
    mov  x0, #1           // fd = stdout
    mov  x8, #64          // SYS_write
    svc  #0
    ret
