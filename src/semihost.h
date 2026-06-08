// semihost.h -- talk to the host (QEMU) via ARM semihosting.
#pragma once

// Terminate QEMU with the given process exit code (used by `make test`).
void qemu_exit(int code);
