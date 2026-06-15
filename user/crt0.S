// crt0.S -- the program entry. Placed at offset 0 (the load/run address).
.section .text._start
.global _start
_start:
    bl      umain          // run the program; its return value is left in x0
    mov     x8, #94        // SYS_EXIT_GROUP (Linux), x0 = umain's return status
    svc     #0
1:  b       1b             // never reached
