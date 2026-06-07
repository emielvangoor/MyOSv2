# Auto-connect to QEMU's GDB stub and stop at kmain.
# Use together with `make debug` (Terminal A) then `make gdb` (Terminal B).
target remote :1234
break kmain
continue
