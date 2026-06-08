// crt0.S -- the program entry. Placed at offset 0 (the load/run address).
.section .text._start
.global _start
_start:
    bl      umain          // run the program
    mov     x0, #0
    mov     x8, #4         // SYS_EXIT
    svc     #0
1:  b       1b             // never reached
