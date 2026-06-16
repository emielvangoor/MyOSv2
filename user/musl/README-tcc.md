# Building `tcc.bin` — the on-device C compiler

`user/musl/tcc.bin` is a static aarch64-linux-musl build of TinyCC (TCC) that
runs **on MyOSv2** and compiles C there. It's committed as a prebuilt blob (like
`busybox.bin`); this note records exactly how to regenerate it.

## Why TCC (not GCC)
TCC compiles, assembles, and links in a single process — no `cc1`/`as`/`ld`
pipeline, no pipes/temp files, no multi-MB sysroot. It reaches "compile a hello
world on the machine" with the syscalls we already have. Full GCC is a much
larger, multi-process port; TCC is the pragmatic first compiler.

## Build steps (host: macOS + `aarch64-linux-musl-gcc`)
```sh
git clone https://github.com/TinyCC/tinycc.git      # built at commit a338258
cd tinycc
# Apply our one backend fix: TCC's arm64 inline assembler had no `svc`
# (needed for syscalls). The patch adds svc/hvc/brk encoding.
git apply /path/to/os/user/musl/tcc-arm64-svc.patch

./configure --targetos=Linux --cpu=arm64 \
    --cc=aarch64-linux-musl-gcc --ar=aarch64-linux-musl-ar --config-musl \
    --extra-cflags="-static -no-pie" \
    --extra-ldflags="-static -no-pie -Wl,-Ttext-segment=0x8000000000"

# c2str is a BUILD-time codegen helper; it must run on the host, so build that
# one with the host compiler before the (cross) tcc build, which can't run it.
cc -DC2STR conftest.c -o c2str.exe
./c2str.exe include/tccdefs.h tccdefs_.h

make tcc                                  # builds just the tcc binary
aarch64-linux-musl-strip -o /path/to/os/user/musl/tcc.bin tcc
```

## Using it on MyOSv2
TCC defaults to a *dynamic* executable; our loader is static-only, so always
build static + non-PIE, link at the user VA base, and link the runtime stub:
```
tcc -static -nostdlib -Wl,-Ttext=0x8000000000 SRC.c /lib/mycrt.o -o OUT
```
`/lib/mycrt.o` (from `user/musl/mycrt.S`) supplies `_start` and a `print`
syscall wrapper, since TCC has no libc here and its inline assembler is too
limited to emit syscalls itself. The Lisp helper `(cc "src.c" "out")` wraps
this. A full libc sysroot (for `#include <stdio.h>`/`printf`) is future work.
