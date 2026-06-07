// Kernel entry from boot.S. For now, just spin so we can prove the build +
// QEMU pipeline works before adding any device code.
void kmain(void)
{
    for (;;) {
        __asm__ volatile("wfe");
    }
}
