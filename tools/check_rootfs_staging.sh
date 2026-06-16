#!/usr/bin/env bash
# Verify the disk-image staging dir holds a complete root filesystem.
set -euo pipefail
cd "$(dirname "$0")/.."
make --no-print-directory build/disk.img >/dev/null
R=build/rootfs
fail=0
for f in \
  bin/init bin/lisp bin/sh bin/busybox bin/tcc bin/teapot \
  lib/bootstrap.l lib/system.l lib/frame.l lib/mycrt.o \
  hello.c hellobare.c \
  usr/include/stdio.h usr/lib/libc.a usr/lib/libtcc1.a \
  init.l test/small.txt test/big.bin
do
  if [ ! -e "$R/$f" ]; then echo "MISSING: $R/$f"; fail=1; fi
done
if [ "$fail" = 0 ]; then echo "rootfs staging OK"; else echo "rootfs staging FAILED"; exit 1; fi
