# MyOSv2

A little operating system for **ARM64 (AArch64)**, written from scratch in C and
assembly, running on QEMU's `virt` board.

This is a **vibe-coded OS, built just for fun** — to learn how computers and
operating systems actually work, and to see how far we can get. No grand plan, no
deadlines; just building it one piece at a time and enjoying the ride.

## What it can do today

- **Boot & serial** — `_start`, stack/`.bss` setup, PL011 UART, `kprintf`.
- **Exceptions & interrupts** — vector table, syscalls (`svc`), GIC, 1000 Hz timer.
- **Memory** — physical page allocator, a coalescing kernel heap, the MMU with
  per-process page tables, ASID-tagged TLB entries (flush-free context switch).
- **Scheduler** — preemptive threads with priorities, sleep, round-robin, and a
  V6-style **sleep/wakeup** (`sched_block`/`sched_wake`) so blocking I/O sleeps
  instead of spinning.
- **Interrupt-driven I/O** — the console and NIC are driven by interrupts, not
  polling: a UART receive IRQ feeds the **tty line discipline** (Ctrl-C → SIGINT),
  `read` blocks until a key is pressed, and the virtio-net IRQ wakes the network
  stack (a blocked `ping` is woken by the reply, or aborted instantly by Ctrl-C).
- **Filesystem** — a VFS (vnode/fs_type) with an in-memory `ramfs` and an initrd.
- **Processes** — user mode at EL0, `fork` + copy-on-write, an **ELF64 loader**,
  and the full lifecycle: `exec`, `exit(status)`, `wait`/reap (with ASID + page
  recycling).
- **Userland** — an interactive shell (`/bin/init`) that runs real ELF programs
  from `/bin` (`true`, `false`, `hello`, `mtest`) via fork→exec→wait and reports
  their exit status.
- **User-space memory** — `sbrk`-grown per-process heap (demand-zeroed pages) and
  a small `malloc`/`free`; anonymous `mmap`; and **shared memory** objects two
  processes can map to communicate.
- **Pipes** — `pipe` + `dup2` with refcounted file handles, so the shell runs
  pipelines like `hello | wc`.
- **Signals** — `kill`, default actions, user handlers (with a sigreturn
  trampoline), and **Ctrl-C** → `SIGINT` to the foreground program.
- **Block device** — a **virtio-blk** disk driver on a generic virtio-mmio +
  virtqueue layer, reading and writing 512-byte sectors.
- **Persistent filesystem** — a small on-disk inode FS (SFS) mounted at `/disk`;
  files survive reboots.
- **Network interface** — a **virtio-net** driver that sends and receives raw
  Ethernet frames (verified with an ARP round-trip to QEMU's gateway).
- **TCP/IP stack** — Ethernet, **ARP** (resolve/cache/reply), **IPv4** (checksum
  + next-hop routing), **ICMP** echo, **UDP**, and a minimal **TCP** client with
  **out-of-order reassembly** (a dropped/reordered segment no longer discards the
  rest of the stream), **adaptive retransmission** (RFC 6298 RTO estimation,
  Karn's algorithm, exponential backoff), and **flow control** (honors the peer's
  advertised window; advertises its own from real receive-buffer space).
- **Sockets** — a BSD-style socket API: `socket`/`bind`/`sendto`/`recvfrom` for
  UDP datagrams, and `socket(SOCK_STREAM)`/`connect` + `read`/`write` for TCP.
  `/bin/dnsq` does a DNS lookup over UDP sockets; `/bin/http` fetches a page over
  TCP (`GET example.com` → `HTTP/1.1 200 OK`) — out to the real internet.
- **DNS + ping** — a **DNS resolver** over UDP and a user-space `ping` that takes
  a hostname: `ping https://www.google.com` strips the scheme, resolves the name,
  and ICMP-echoes the address.
- **Program arguments** — `exec` passes `argv` to programs; the shell tokenizes
  the command line, so `/bin/ping <host>` and friends get their arguments.
- **`shutdown`** — a shell command that halts the machine via PSCI (QEMU exits).

Where it goes next — a TCP *server* (listen/accept), more of the socket API,
and beyond — lives in **[docs/ROADMAP.md](docs/ROADMAP.md)**. The goal is a
capable, Unix-like OS; graphics is deferred.

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
