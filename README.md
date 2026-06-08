# MyOSv2

A little operating system for **ARM64 (AArch64)**, written from scratch in C and
assembly, running on QEMU's `virt` board.

This is a **vibe-coded OS, built just for fun** — to learn how computers and
operating systems actually work, and to see how far we can get. No grand plan, no
deadlines; just building it one piece at a time and enjoying the ride.

So far it boots, talks over serial, sets up virtual memory, schedules threads,
isolates user processes, has an in-memory filesystem, and runs an interactive
shell at EL0. Where it goes next — a real process model, persistent storage, and
eventually networking — lives in **[docs/ROADMAP.md](docs/ROADMAP.md)**.

## Try it

```sh
make run     # boot it in QEMU (serial in your terminal; Ctrl-C to quit)
make test    # run the in-kernel self-test suite
```

You'll need an `aarch64-elf` cross-toolchain and `qemu-system-aarch64`.

## How it's built

Every feature goes through the same loop: a design spec, a test-first plan, then
TDD implementation gated by `make test` (a pre-commit hook blocks commits if any
test fails). Specs live in `docs/superpowers/`, notes in `docs/notes/`.

Built with a lot of help from Claude. It's a playground — expect rough edges.
