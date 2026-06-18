#!/bin/sh
# Fast KTEST inner loop: incremental TEST_EXIT build + semihosting run.
# (Same flags as `make test`, minus the surrounding clean, for a quick RED/GREEN
# cycle. Run `make test` for the authoritative clean-build gate before commit.)
set -e
make --no-print-directory EXTRA_CFLAGS=-DTEST_EXIT build/kernel.elf build/disk.img >/tmp/ktest_build.log 2>&1 || { tail -25 /tmp/ktest_build.log; exit 1; }
timeout 90 qemu-system-aarch64 -machine virt -cpu cortex-a72 -m 256M -display none \
  -chardev stdio,id=ch0,signal=off -serial chardev:ch0 \
  -device virtio-keyboard-device -device virtio-tablet-device -device virtio-gpu-device \
  -kernel build/kernel.elf \
  -global virtio-mmio.force-legacy=false \
  -drive file=build/disk.img,if=none,format=raw,id=hd0 -device virtio-blk-device,drive=hd0 \
  -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
  -semihosting 2>&1 | grep -E "FAIL|self-tests:|dup3|pipe2|nanosleep|unlink|fchmod|utimens|ftrunc|readlink|faccess|sendfile" || true
