# MyOSv2 — Roadmap

A living map of where the OS has been and where it can go next. Each **future**
phase here is a *candidate*, described in enough detail to choose between them —
not a commitment. When you pick one, it still gets its own full cycle:
brainstorm → spec (`docs/superpowers/specs/`) → test-first plan
(`docs/superpowers/plans/`) → build (TDD, gated on `make test`) → notes
(`docs/notes/`).

Target throughout: **ARM64 (AArch64), C + assembly, QEMU `virt` board.**

---

## Done (Phases 0–11)

| # | Phase | What it gave us |
|---|-------|-----------------|
| 0–1 | Boot + UART | `_start`, stack, `.bss`, serial "hello world", `kprintf` |
| 2 | Exceptions | Vector table, `svc`, EL0↔EL1, ESR/ELR/SPSR decoding |
| 3 | Interrupts + timer | GIC, 1000 Hz timer tick, IRQ handling |
| 4 | Physical + heap memory | PMM (page allocator), `kmalloc`/`kfree` (coalescing heap) |
| 5 | Scheduler | Threads, round-robin, priorities, sleep, preemption |
| 6a/6b | MMU + address spaces | Page tables, per-process `struct addrspace`, isolation |
| 7 | VFS + ramfs + initrd | vnode/vnode_ops/fs_type, in-memory FS, embedded files |
| 8 | Processes (exec-as-spawn) | Load a program from `/bin/init`, run at EL0, fd tables |
| 9 | fork + copy-on-write | `fork()`, COW page sharing, refcounts, COW fault handler |
| 10 | Interactive shell | Keyboard input, `help`/`ls`/`cat`/`exit`, `readdir` syscall |
| 11 | ASIDs | Tagged TLB entries, flush-free context switch |

> A ramfb framebuffer + drawing layer was built as an experimental Phase 12, then
> **removed** when graphics was deferred. It's recoverable from git history if
> graphics is revived later.

---

## The north star: a capable, Unix-like OS (with networking)

The goal is to make MyOSv2 a genuinely **capable, Unix-like operating system**: a
real process model (run programs, pipe and signal them), **persistent on-disk
storage**, and a working **TCP/IP network stack with sockets**. Networking is the
capstone — the point where the OS can talk to the outside world.

**All graphics work is deferred.** The framebuffer (Phase 12) stays as-is; the
window-manager track, the display server, and `virtio-gpu` are parked at the
bottom and revisited later. Everything below builds toward a headless but
powerful systems OS.

### Critical path

```
Process model    13 exec+wait ─► 14 ELF loader + coreutils
                                       │
Memory           15 user heap (brk/malloc) ─► 16 mmap + shared memory
                                       │
IPC              17 pipes ─► 18 signals
                                       │
Storage          19 virtio transport + virtio-blk ─► 20 on-disk filesystem
                                       │
Networking       21 virtio-net ─► 22 TCP/IP stack + sockets   ◄── CAPSTONE
```

The **virtio transport** built in Phase 19 is shared: virtio-blk uses it for the
disk, virtio-net reuses it for the NIC. Storage (19→20) and networking (21→22)
are independent tracks once the transport exists — do them in either order, though
persistent storage is the simpler first win.

---

## Phase 13 — Process lifecycle: `exec` + `exit` status + `wait`

**Why:** finishes the Unix process model. `fork` (Phase 9) without `exec`/`wait`
is only half a model; the shell needs to launch and reap real programs. Also
resolves the Phase 11 ASID-recycling gap.

**Adds**
- `SYS_EXEC(path)` — replace the current process image with a program from a file
  (tear down the old user address space, build a new one, reset PC/SP).
- `SYS_EXIT(status)` — carry an exit code; mark the thread a **zombie**.
- `SYS_WAIT`/`SYS_WAITPID` — block until a child exits, collect its status, then
  **reap** it: free its kernel stack + address space (`as_destroy`) and **recycle
  its ASID**.

**Key files:** `syscall.c`, `sched.c` (zombie state, parent links, reaping),
`vm.c` (`as_destroy`, ASID free list), `proc.c` (exec), `user/sh.c`.

**Done looks like:** the shell runs an external program fork→exec→wait, prints its
exit status, returns to the prompt; processes are cleaned up (ASIDs reused).

**Depends on:** 9, 10, 11. **Size:** medium.

---

## Phase 14 — ELF loader + coreutils (`/bin`)

**Why:** today user programs are flat blobs all linked at one VA. A capable OS
loads real, separately-built binaries — and ships a handful of small utilities.

**Adds**
- A minimal **ELF64 loader**: map each `PT_LOAD` segment at its `p_vaddr` with the
  right perms (RX code / RW data), zero the `.bss` tail.
- Several **coreutils** built separately and embedded in the initrd under `/bin`
  (`echo`, `cat`, `ls`, `wc`, `true`/`false`, …); the shell runs them via exec.

**Key files:** new `elf.c`, `proc.c`/`vm.c`, `Makefile`, `user/*.c`.

**Done looks like:** `ls /bin` lists the utilities; running each works; the loader
handles a multi-segment binary.

**Depends on:** 13. **Size:** medium.

---

## Phase 15 — User-space dynamic memory (`brk`/`sbrk` + `malloc`)

**Why:** real programs allocate memory at runtime. Fixed stack/data pages aren't
enough for non-trivial utilities (and the network stack's buffers later).

**Adds**
- `SYS_BRK`/`SYS_SBRK` growing the user heap, mapping fresh pages on demand.
- A small user-space `malloc`/`free` in `ulib`.

**Key files:** `vm.c` (grow heap, demand-map), `syscall.c`, `user/ulib.c`.

**Done looks like:** a program `malloc`s/uses/frees memory; touching a fresh heap
page maps it on demand.

**Depends on:** 6b, 13. **Size:** small-medium.

---

## Phase 16 — `mmap` + shared memory

**Why:** general-purpose memory mapping — anonymous regions (generalizing the
heap) and **shared memory** between processes (a building block for IPC and, much
later, any user-space services).

**Adds**
- `SYS_MMAP`/`SYS_MUNMAP` with anonymous, demand-zeroed mappings.
- **Shared-memory objects:** `shm_create()` → a handle; multiple processes `mmap`
  the same handle and share the **same physical pages**, reusing the page-refcount
  machinery already built for COW (Phase 9).

**Key files:** `vm.c` (shared/anonymous mappings, refcounts), new `shm.c`,
`syscall.c`.

**Done looks like:** a program `mmap`s an anonymous region and uses it; two
processes `mmap` one shm handle and one sees the other's writes.

**Depends on:** 6b, 9 (refcounts), 15. **Size:** medium-large.

---

## Phase 17 — Pipes

**Why:** the first IPC primitive; enables shell pipelines like `ls | wc`, and
establishes the blocking-fd pattern sockets will reuse.

**Adds**
- `SYS_PIPE` — create a pipe, return a read fd + a write fd.
- A kernel **ring-buffer pipe object** behind the VFS `file` abstraction: `read`
  blocks until data or all writers close; `write` blocks until space.
- `SYS_DUP`/`dup2` for wiring fds. The shell parses `|` and connects a child's
  stdout to the next child's stdin.

**Key files:** new `pipe.c`, `vfs.c`, `syscall.c` (`pipe`/`dup`), `sched.c`
(block/wake), `user/sh.c`.

**Done looks like:** `ls | wc` streams output through the pipe; a blocked reader
wakes when the writer produces data.

**Depends on:** 13 (and the fd table from 8). **Size:** medium.

---

## Phase 18 — Signals

**Why:** asynchronous notification and a real **Ctrl-C** to interrupt a running
program — table-stakes for a usable shell.

**Adds**
- `SYS_KILL(pid, sig)`, a per-process pending mask, default actions (terminate),
  optional user handlers (`sigaction`) delivered on the return-to-EL0 path via a
  signal trampoline; console Ctrl-C → `SIGINT` to the foreground process.

**Key files:** new `signal.c`, `sched.c`, `syscall.c`, `vectors.S`, console.

**Done looks like:** Ctrl-C kills a long-running program and returns to the shell;
a handler-equipped program runs its handler.

**Depends on:** 13. **Size:** medium-large (touches the trap-return path).

---

## Phase 19 — virtio transport + virtio-blk (block device)

**Why:** the gateway to everything off-board. Build the generic **virtio-mmio +
virtqueue** machinery once; virtio-blk gives a real disk now, and virtio-net
(Phase 21) reuses the same transport.

**Adds**
- Generic **virtio-mmio transport**: device probe, feature negotiation, a
  **virtqueue** (descriptor / available / used rings), notify + (IRQ or polled)
  completion.
- A **virtio-blk** driver: read/write 512-byte sectors, backed by a QEMU `-drive`
  image; a generic block-device abstraction the filesystem sits on.

**Key files:** new `virtio.c` (transport + virtqueue), `virtio_blk.c`, `block.h`;
`Makefile` (`-drive`).

**Done looks like:** read and write sectors to a disk image; bytes written in one
run are readable in the next.

**Depends on:** 3 (IRQs), 6 (MMIO). **Size:** large (new driver class; transport
is reused for networking).

---

## Phase 20 — On-disk filesystem (persistence)

**Why:** turn the block device into real, persistent files mounted under the VFS —
a defining trait of a capable OS.

**Adds**
- A real filesystem as a VFS `fs_type` backed by the block device — either
  **FAT32** (interoperable with your host) or a **simple custom inode FS**.
  Format/mount/lookup/read/write/create.

**Key files:** new `fatfs.c` or `sfs.c`, VFS integration, a `mkfs` helper.

**Done looks like:** create a file from the shell, reboot QEMU, the file is still
there.

**Depends on:** 7 (VFS), 19. **Size:** large.

---

## Phase 21 — virtio-net (NIC driver)

**Why:** the hardware half of networking — get raw Ethernet frames in and out.

**Adds**
- A **virtio-net** driver on the Phase-19 transport: RX/TX virtqueues, receive
  frames into buffers, transmit frames, read the MAC address.
- A small link-layer interface the IP stack binds to.

**Key files:** new `virtio_net.c`, `netif.h`; `Makefile`
(`-netdev user,... -device virtio-net-device,...`).

**Done looks like:** the kernel receives and transmits Ethernet frames; logs
inbound frames (e.g. ARP requests from QEMU's user-net gateway).

**Depends on:** 19 (transport). **Size:** large.

---

## Phase 22 — TCP/IP stack + sockets  ⟵ CAPSTONE  ✅ DONE

**Why:** the destination — the OS speaks to the network with a real protocol stack
and a sockets API user programs can use.

**Built**
- **Ethernet + ARP** (resolve/cache/reply), **IPv4** (header checksum + next-hop
  routing), **ICMP** echo, **UDP**, **DNS** resolver, and a minimal **TCP**
  client (3-way handshake, in-order data + cumulative ACKs, simple retransmit,
  FIN close, pseudo-header checksum).
- A **BSD-style sockets API** in the fd table: `socket`/`bind`/`sendto`/
  `recvfrom` (UDP) and `socket(SOCK_STREAM)`/`connect` + `read`/`write`/`close`
  (TCP). Static guest IP (QEMU user-net 10.0.2.15); QEMU's resolver for DNS.
- The whole stack is **interrupt-driven** (the virtio-net IRQ wakes a sleeping
  waiter; no busy-polling — see `docs/notes/interrupt-driven-io.md`).
- Demos: `/bin/ping <host>`, `/bin/dnsq <host>` (UDP sockets), `/bin/http <host>`
  (fetch a page over TCP). Verified live against the real internet.

**Notes:** `docs/notes/phase-22.md`, `docs/notes/sockets-and-tcp.md`,
`docs/notes/interrupt-driven-io.md`.

**What it deliberately is NOT:** the TCP is a *client on a reliable path*, not a
full stack — see Phase 23 for the gap.

---

## Phase 23 — TCP hardening + networking completeness  (largely done)

**Why:** Phase 22's TCP is a deliberately minimal client. To be a real stack it
needs the parts that handle loss, reordering, flow/congestion, and incoming
connections. This phase is itself large — pick milestones from it.

**Done (23.1–23.9, see `docs/notes/phase-23.md`):** out-of-order reassembly;
adaptive retransmission (RFC 6298 RTO + Karn + backoff + a real retransmit
queue); flow control (advertised windows both ways); a TCP **server**
(listen/accept) + `/bin/httpd`; socket API polish (`poll`, `shutdown`); Reno
congestion control; the full RFC 793 state machine (CLOSE_WAIT/LAST_ACK,
FIN_WAIT/CLOSING/TIME_WAIT) + RST generation; segmentation + pipelining + Nagle;
and addressing (DHCP client + UDP transmit checksum).

**Still open (independent follow-ups):** `select`, non-blocking fds,
`getsockname`/`setsockopt`; delayed ACKs; IPv4 fragmentation/reassembly; ICMP
error replies; honoring TTL / forwarding; window scaling; and converting
virtio-blk to interrupt-driven completion.

### Correctness / robustness (the cut corners)
- **Out-of-order reassembly.** Today only in-order segments are accepted; a gap
  drops everything after it. Add a reassembly queue keyed on sequence number.
- **Real retransmission.** Replace the fixed 500 ms best-effort retransmit with
  **RTO estimation** (RTT samples → smoothed RTT + variance, RFC 6298),
  exponential backoff, and **Karn's algorithm** (don't sample RTT from a
  retransmitted segment). Keep a proper **retransmit queue** of unacked data
  (currently `tcp_send` only re-sends its last segment and pins the caller's
  buffer).
- **Flow control.** Honor the peer's **advertised window** (don't send past it);
  advertise our own window from real receive-buffer space and send **window
  updates**. Today we send one MSS and wait, and advertise a fixed window.
- **Congestion control.** Slow start + congestion avoidance (TCP Reno: cwnd,
  ssthresh, fast retransmit/recovery). None today.
- **Full state machine + teardown.** Proper `CLOSE_WAIT`, `TIME_WAIT` (with the
  2·MSL wait and port quarantine), simultaneous close, and **RST generation** for
  segments that match no connection. Today close is simplified.
- **Larger transfers.** Segment a `write` larger than one MSS; coalesce small
  writes (**Nagle**) and **delayed ACKs** for efficiency.

### Missing API / features
- **TCP server:** `listen`/`accept` (passive open), a listen backlog, demultiplex
  incoming SYNs to accepted sockets. (Then: a tiny in-guest HTTP server.)
- **Socket API polish:** `getsockname`/`getpeername`, `setsockopt`, `shutdown`,
  **non-blocking sockets** + `poll`/`select` (so one program can wait on several
  fds), `recv`/`send` flags.
- **Addressing:** **DHCP** client (drop the hardcoded 10.0.2.15); configurable
  IP/route.
- **IPv4 completeness:** fragmentation + reassembly; honor TTL; send **ICMP
  errors** (e.g. port-unreachable). **UDP transmit checksum** (currently 0/omitted).
- **TX path:** virtio-net transmit-completion is interrupt-driven, but
  **virtio-blk still polls** its used ring — convert it for symmetry.

**Key files:** `tcp.c` (the bulk), `socket.c` (listen/accept, poll), `netstack.c`
(fragmentation, ICMP errors, DHCP), `syscall.c` (`poll`/`select`, new socket
calls), `virtio_blk.c` (IRQ completion).

**Done looks like:** a download survives packet loss/reordering; the guest runs a
tiny TCP server an outside client can connect to; `poll`/`select` lets one
program juggle multiple sockets.

> The `http | wc` deadlock noted earlier is **fixed** — pipes now sleep/wake
> (`sched_block`/`sched_wake`) instead of yield-spinning, so a blocked reader no
> longer starves the interrupts a network-waiting peer needs.

**Depends on:** 22. **Size:** very large — treat each bullet as its own milestone.

---

## Phase 24 — The Lisp machine  ✅ DONE (24.1–24.4 complete; 24.5 graphics deferred)

**Goal:** make a Lisp the **primary userland** of MyOSv2 (Emacs / Symbolics
spirit, "Emacs-on-Unix" model). The C kernel is untouched; a Lisp image runs at
EL0 as an ordinary MMU-protected process talking to the kernel through existing
syscalls. It fork/execs external ELF programs *and* runs in-image Lisp functions
(Eshell model), eventually becomes `init`, and accepts live connections from
Doom Emacs over TCP.

- **Design spec:** `docs/superpowers/specs/2026-06-09-lisp-machine-design.md`
- **Phase notes:** `docs/notes/phase-24.md`
- **Ported from:** the standalone host interpreter at `~/Code/Sides/lm-lisp`
- **Decisions locked by the user:** conservative GC stack scanning; connect
  Emacs right after 24.1; do 24.1 → 24.5 (graphics deferred to a later phase).

### Architecture (already in place — read before continuing)

The reader/evaluator/printer/GC/primitives are ONE platform-neutral file,
**`src/lm_core.c`** (+ `src/lm.h`), with no libc and no kernel headers. It is
compiled into **two** binaries via a small platform layer:

| Build  | Platform file       | alloc     | I/O      | Role                 |
|--------|---------------------|-----------|----------|----------------------|
| kernel | `src/lm_platform.c` | `kmalloc` | UART     | drives KTEST cases   |
| user   | `user/lm.c`         | `malloc`  | syscalls | `/bin/lisp` + REPL   |

Platform hooks: `lm_alloc`/`lm_free`, `lm_sys_read`/`lm_sys_write`/`lm_open`/
`lm_close`, `lm_setjmp`/`lm_longjmp` (`src/lm_jmp.S`), `lm_abort`. The current
I/O streams are globals `lm_cur_in` (Reader*) / `lm_cur_out` (Writer*); point
them at a tty, a socket, or a capture buffer. GC is conservative: `lm_stack_base`
+ register spill via `lm_setjmp`, scanned against the live-object list (bounded by
`gc.lo/hi`). Auto-collect in `gc_alloc` is gated on `lm_stack_base != 0` (so the
KTEST build, which leaves it 0, only collects with explicit roots).

Build wiring (Makefile): `lm` is in `PROGS`; `/bin/lisp` has a **dedicated rule**
linking `user/crt0.S user/ulib.c src/lm_core.c src/lm_jmp.S user/lm.c` with
`-Isrc`. `.l` files in `user/lisp/` (`LISP_FILES`) are xxd'd into
`build/lisp_blob.c` and unpacked by the initrd to `/lib/<name>.l`.

### ✅ 24.1 — core port + serial REPL  (DONE, committed `11be899`)

`src/lm.h`, `src/lm_core.c`, `src/lm_jmp.S`, `src/lm_platform.c`, `user/lm.c`
(`/bin/lisp`), `user/lisp/bootstrap.l` (→ `/lib/bootstrap.l`). 11 KTEST cases in
`src/tests.c` (`test_lm_*`), full suite green (128 tests). Verified live under
QEMU: `(fact 6)`→720, `(mapcar … (range 5))`→`(0 1 4 9 16)`, error recovery works.
Console caveat: pasting long lines overflows the 16-byte PL011 RX FIFO (drops the
newline → reader stalls); human-speed typing is fine; the TCP path is immune.

### ✅ 24.1b — Emacs ↔ TCP REPL  (DONE)

`lisp -serve [port]` (default **7777**, not 7000 — macOS AirPlay squats on
7000 and a dead `hostfwd` kills QEMU) serves the REPL over a blocking
`accept` loop; the image persists across connections. `lm_error` now reports
through `lm_cur_out` (KTEST `lm: errors go to lm_cur_out`) so remote users see
their errors. Emacs glue in `user/lisp/lm-mode.el` (`lm-connect`, `C-c C-e`,
`C-c C-r`). `make run` forwards host:7777 → guest:7777. End-to-end check:
`python3 tools/lisp_serve_check.py` (boot → serve → eval → error-over-socket →
reconnect persistence).

### ✅ 24.2 — system primitives (DEFUN over syscalls)  (DONE)

`user/lm_sys.c` (user-only, linked into the `lm.elf` rule): `(getpid)` `(fork)`
`(exec path argv)` `(wait)`→`(pid . status)` `(exit [code])` `(kill pid sig)`
`(sleep ms)` `(open)` `(close)` `(fd-read fd n)` `(fd-write fd str)` (named
`fd-*` because the core owns `read` = parse a form) `(pipe)`→`(rfd . wfd)`
`(dup2)` `(socket 'stream|'dgram)` `(bind)` `(listen)` `(accept)` `(connect fd
host port)` `(shutdown)`. Registered via `lm_sys_register()` after `lm_boot()`.
Verified by `python3 tools/lisp_sys_check.py` (boot-and-observe over the TCP
REPL: files, pipe roundtrip, fork/exit/wait, fork/exec/wait, sockets).

### ✅ 24.3 — the shell in Lisp (`user/lisp/system.l`)  (DONE)

`(run "hello" "arg")` → fork→exec→wait→status (variadic via new core **rest
params**: bare-symbol parameter binds all args). `(| a b)` pipeline macro —
stages run as forked children joined by `pipe`+`dup2`; in-image stages compose
(`(| (princ "abcde") (run "wc"))` → 5 on the serial console / under init).
`(ls)`/`(cat)` coreutils over a new `(readdir)` primitive; `(repl)` via a new
core `eval` primitive. Shipped as `/lib/system.l`, loaded after `bootstrap.l`.
KTESTs: `lm: rest params + | symbol`, `lm: eval primitive`. Verified by
`python3 tools/lisp_shell_check.py` (serial + TCP phases).

### ✅ 24.4 — flip `init` to Lisp  (DONE)

`src/initrd.c`: `/bin/init` is now `lm_elf` (`/bin/sh` keeps the C shell as a
fallback — `(run "sh")` from Lisp, `exit` to come back). As PID 1 the serial
REPL never exits: on console EOF it reopens the reader and keeps prompting.
Verified by `python3 tools/lisp_init_check.py`, plus re-runs of all earlier
checks against the flipped boot (the harness starts the network REPL with
`(run "lisp" "-serve")`).

### Testing / workflow reminders (user's standing rules — memory)

- **TDD, test-first**, gated by `make test` + the pre-commit hook. Portable-core
  behavior is unit-tested in `src/tests.c` (`test_lm_*` via `lm_is(...)`); the
  Lisp tests must call `lm_fresh()` (pmm+kheap init + `lm_boot`) first, because
  they run before the heap tests in the registry.
- Integration (primitives, shell, TCP, init flip) is verified by **booting under
  QEMU and observing** — drive the serial console **char-by-char with ~12 ms
  gaps** (see the python snippet in the session / `phase-24.md`) to avoid the RX
  FIFO overflow; the TCP path can be driven directly from the host.
- **Heavily document** (teaching-level "why" comments). **Keep README current**
  each phase. **Prefer interrupts/blocking over polling** (the TCP server uses
  blocking `accept`, not poll-spin).

**Depends on:** 14 (ELF/exec), 17 (pipes), 18 (signals), 22 (sockets).
**Size:** large — each sub-phase is its own commit.

---

## Phase 25 — The graphical Lisp machine  ✅ DONE

The Emacs architecture, end to end: redisplay in C inside each /bin/lisp,
Lisp owns buffers/windows/faces, the kernel grows only drivers + a seat
multiplexer. **Spec:** `docs/superpowers/specs/2026-06-10-graphical-lisp-machine-design.md`
· **Notes:** `docs/notes/phase-25.md` · Plans per sub-phase in `docs/superpowers/plans/`.

- ✅ **25.1 — virtio-input**: keyboard + tablet drivers (evdev triples,
  IRQ top-half/bottom-half), blocking `input_read` syscall, `/bin/evtest`,
  QMP-driven check (`tools/input_check.py`).
- ✅ **25.2 — virtio-gpu**: 2D driver (create/backing/scanout + per-rect
  transfer/flush), `gfx_acquire` maps the fb into userland, `/bin/gfxtest`,
  screendump-verified (`tools/gfx_check.py`).
- ✅ **25.3 — rd_core**: gap buffers, window tree, faces, glyph-matrix
  layout + cell-diff damage, font painting — dual-built, 6 KTESTs red→green.
- ✅ **25.4 — Lisp integration**: `lm_gfx.c` + `frame.l`; `lisp -frame` boots
  the graphical REPL; in-OS `(screenshot)`; glyph-level screendump check
  (`tools/frame_check.py`); see `docs/images/phase-25-graphical-lisp-machine.png`.
- ✅ **25.5 — the seat**: per-seat gpu resources, input routing, Ctrl-Alt-Fn
  + `(switch-seat n)`, `(spawn-vm)`; screendump-verified (`tools/seat_check.py`).
- ✅ **25.6 — surface buffers**: shm canvases, `(surface-fill-rect)`,
  `(run-in-buffer ...)` external renderers; PTE_SHARED fork fix;
  screendump-verified (`tools/surface_check.py`).

---

## Phase 26 — Floating point + TinyGL (the teapot)  ✅ DONE

3D "just for fun", and the kernel features it smoked out.
**Notes:** Phase 26 section in `docs/notes/phase-25.md`.

- ✅ **26.1 — FPU**: `fpu_enable()` (CPACR_EL1.FPEN) before anything
  schedules; per-thread 528-byte FP/SIMD area saved/restored eagerly at
  `cpu_switch` (`src/fp.S`); the kernel itself stays `-mgeneral-regs-only`,
  user programs get floats/NEON; `/bin/fptest` proves V-register isolation
  across fork + 50 context switches.
- ✅ **26.2 — TinyGL + the Utah teapot**: TinyGL (Bellard/C-Chads, zlib
  license) vendored under `user/tinygl/` against shim headers + `tgl_rt.c`;
  `/bin/teapot` tessellates Newell's 1975 Bezier patches (quadrant data
  mirrored out, orientation-corrected normals) into a surface buffer at
  ~25fps; `(teapot)` in frame.l + the `animate` heartbeat; stream-thunk
  repaints on quiet poll ticks so slow-printing children don't drag the
  frame rate. Verified end to end by `tools/teapot_check.py`.
- ✅ **26.3 — process groups**: `pgid` in `struct thread` (fork inherits,
  fresh threads lead their own), `SYS_SETPGID`, `kill(-pgid)` signals the
  whole group — the frame's C-c now interrupts an entire job (Lisp wrapper
  AND the ping it forked), not just the wrapper. KTEST `sig: process
  groups` + the teapot check's C-c stage.

---

## Phase 27 — Deepening the Lisp machine  ✅ DONE (27.1–27.3)

Closing the gaps that kept the graphical frame tethered to the serial console.
**Notes:** `docs/notes/phase-27.md`.

- ✅ **27.1 — interactive stdin**: `stream-thunk` gains a second pipe so a
  program run from the frame reads its fd 0 from the frame KEYBOARD, not the
  serial port -- typed chars echo into the buffer and feed the child, RET
  sends a line, C-d is EOF, C-c still group-kills the job. Pure Lisp
  (`frame.l`); verified by `tools/stdin_check.py` (type into a live `wc`).
- ✅ **27.2 — line editing + history**: `stream-thunk` runs canonical (cooked)
  input -- backspace edits the buffered line, RET delivers it, C-d is EOF;
  and the REPL keeps an Up/Down history of submitted forms. Verified by
  `tools/lineedit_check.py` and `tools/history_check.py`.
- ✅ **27.3 — editable file buffers**: `(find-file)` opens a file in a split
  window; a new `(current-buffer)` primitive lets dispatch run a plain-editing
  keymap in non-REPL buffers (RET = newline, C-b/C-f + arrows move point);
  `(save-buffer)` / C-x C-s writes back via creat+fd-write. Verified by
  `tools/findfile_check.py` (edit + save round-trips through disk). Follow-ups:
  a truncate syscall (re-saving shorter files) and by-line C-p/C-n motion.
- ✅ **27.4 — scrolling**: windows follow point the Emacs way -- `layout_leaf`
  nudges each window's `top_line` (window-start) so point stays visible, so the
  REPL and tall buffers scroll instead of running off the bottom. KTEST `rd:
  scroll follows point` + `tools/scroll_check.py`.
- ✅ **27.5 — basic editing keys**: C-a/C-e, C-b/C-f, C-d, C-k, M-f/M-b, M-d
  (pure Lisp over a new `(char-at)` primitive), clamped to the prompt in the
  REPL. Fixed `repl-eval` to append at end-of-buffer (cursor motion could leave
  point mid-input). `tools/keyedit_check.py`.
- ✅ **27.6 — independent REPLs**: REPL state went per-buffer (`repl-bufs`,
  `repl-starts`) so C-x 2 / C-x 3 give the new window its OWN fresh REPL
  instead of mirroring the current buffer. `tools/split_check.py`.
- ✅ **27.7 — M-x list scrolls**: the vertico candidate window slides to follow
  the selection (`mb-top`/`mb-move`) so all matches are reachable, with an
  "i/n" position counter. `tools/mxscroll_check.py`.
- ✅ **27.8 — self-documenting help**: keymaps became DATA
  (`global`/`cx`/`help-keymap`, `key-lookup`/`run-key`) so dispatch and help
  share one source; **C-h b** lists bindings, **C-h k** shows a key's command
  and living source, **C-h m** describes the mode. `tools/helpkeys_check.py`.
- ✅ **27.9 — buffer/window management**: **C-x 1** (`delete-other-windows`,
  new `rd_delete_other`) restores a single full window; **C-x C-b**
  (`list-buffers`) over a new `(buffer-list)` primitive. KTEST `rd: delete
  other windows` + `tools/bufferlist_check.py`.
- ✅ **27.10 — C-x b switch-to-buffer**: the vertico minibuffer generalized to
  complete over any candidates (`mb-source`/`mb-label`); switch-to-buffer
  completes over buffer names and shows the pick in the window.
  `tools/bufswitch_check.py`.
- ✅ **27.11 — disk boots into the frame**: `tools/mkdisk.py` builds a fresh
  persistent `disk.img` pre-seeded with `/init.l = (run-bg "lisp" "-frame")`
  (the `$(DISK)` rule + `make fresh-disk`), so a reset disk boots straight into
  the graphical machine instead of the bare serial REPL. `tools/autostart_check.py`.

---

## Phase 28 — Emacs-style major modes  ✅ DONE

A real Emacs-style major-mode system layered over the graphical Lisp machine.
**Spec:** `docs/superpowers/specs/2026-06-15-major-modes-design.md`
· **Plan:** `docs/superpowers/plans/2026-06-15-major-modes.md`

- ✅ Pure mode model in `user/lisp/modes.l`: `*modes*` registry,
  `register-mode`, `define-derived-mode`, buffer-local variables,
  `key-lookup` / `mode-key-lookup`. Hierarchy: `special-mode` (inert root)
  → `surface-mode` + `fundamental-mode` → `text-mode` / `repl-mode` /
  `lisp-interaction-mode`.
- ✅ Mode-line field: `char mode_line[24]` in `struct rd_buffer`
  (`src/rd.h`); painted as `-- name  (mode_line) --` by `rd_core.c`;
  `(set-mode-line-name str)` primitive from `user/lm_gfx.c`. KTEST
  `rd: modeline shows mode name`.
- ✅ Mode definitions + `set-major-mode` + `eval-last-sexp` in `frame.l`;
  dispatch rewired onto `mode-key-lookup`; boot into `*scratch*`
  (lisp-interaction-mode).
- ✅ `repl-here` / `C-x r` opens a REPL (repl-mode) in the current window;
  `C-x C-f` opens files in `text-mode`; `surface-mode` wraps the teapot
  canvas. Mode-aware `frame-tick` recovery and `describe-mode` /
  `describe-bindings` follow the live keymap chain.
- [x] Emacs-style major modes (fundamental/text/repl/lisp-interaction),
      `*scratch*` default, interactive `C-x C-f`, mode line names the mode.
      Spec: docs/superpowers/specs/2026-06-15-major-modes-design.md

---

## Phase 29 — Persistent ext2 root filesystem  ✅ DONE

The OS now boots from a **real, persistent on-disk ext2 filesystem** mounted as
`/`. There is no ramfs root and no initrd at runtime; the kernel-embedded userland
(`user_blob` / `lisp_blob` / `src/initrd.c`) was removed.

- ✅ **29.1 — install userland onto disk image**: the host build runs
  `mke2fs -d build/rootfs` to populate `build/disk.img` with the full root tree:
  `/bin/*` (all programs: busybox, tcc, lisp, shell, …), `/lib/*.l` + `/lib/mycrt.o`,
  `/usr/{include,lib}` (the musl sysroot), `/hello.c` + `/hellobare.c` seed sources,
  and `/init.l`. The `/disk` mount point is retired — `/` IS the ext2 image.
- ✅ **29.2 — mount ext2 as root**: `vfs_mount_root(ext2_type())` at boot;
  the kernel halts with a clear message if the disk cannot be mounted. The musl
  sysroot moved from `/disk/usr` → `/usr`; the boot counter from `/disk/boots` →
  `/boots`; the user init file from `/disk/init.l` → `/init.l`.
- ✅ **29.3 — remove kernel-embedded userland**: `user_blob`, `lisp_blob`, and
  `src/initrd.c` deleted from the kernel build. (One tiny `sh.elf` is retained
  embedded purely as a sample ELF for VM self-tests — not runtime userland.)
- ✅ **29.4 — persistence verified**: `tools/persist_check.py` writes a file,
  reboots the same `disk.img`, reads it back, and asserts `PERSIST OK`.

`ramfs.c`/`ramfs.h` remain in the tree and are used by the KTEST harness.

---

## Phase 30 — Text properties + ANSI color  ✅ DONE

Buffers gained genuine **Emacs text properties**, and Unix/ANSI styling is
translated into them, so program output renders colored in the frame instead of
showing raw `ESC[..m` escape bytes.

- ✅ **30.1 — text-property store**: `struct rd_buffer` carries an interval list
  (`{start, end, plist}`) the mark-sweep GC traces (via `gfx_gc_mark_buffers`);
  `put-text-property` / `get-text-property` / `set-text-properties` /
  `remove-text-properties` / `text-properties-at`; intervals shift on
  insert/delete. (`#ifdef LM_BUILD`-guarded so the kernel build of `rd_core.c`
  stays Lisp-free.)
- ✅ **30.2 — propertize + insert**: `(propertize STR ...)` → a propertized
  string; `insert` stamps its plist onto the inserted range.
- ✅ **30.3 — named themeable faces**: `defface` / `set-face-attribute`; the
  `face` property value is a face name or a list merged left-to-right (Emacs
  semantics). Renderer resolves each character's face to a cell color.
- ✅ **30.4 — ANSI translation**: a themeable `ansi-*` palette and a stateful
  `ansi-color-apply` (the `ansi-color.el` analog) that strips SGR escapes and
  emits `face`-propertized pieces, carried across stream chunks; wired into the
  frame's output streamer.
- ✅ **30.5 — verified**: `tools/textprops_check.py` runs `busybox ls` in the
  frame and asserts directories render in ANSI blue with no escape litter.

---

## Phase 31 — `M-x vterm`: a Unix terminal in a buffer  ✅ DONE

Full-screen TUIs (busybox `sh`, `vi`, `less`) run **inside a frame buffer**, the
Emacs way — not on a separate seat. The frame stays pure Lisp; a small C helper
process owns libvterm and a kernel pty, and speaks a line protocol over pipes.
**Spec:** `docs/superpowers/specs/2026-06-18-vt100-terminal-design.md`

- ✅ **31.1 — kernel pseudo-terminal**: `src/pty.c` + `pty.h` — master/slave
  rings with a real **termios line discipline** (canonical line editing + VERASE,
  ECHO, raw mode, ISIG→SIGINT, OPOST/ONLCR). `SYS_OPENPT` returns a
  {master, slave} pair; `SYS_IOCTL` routes TCGETS/TCSETS*/TIOC[GS]WINSZ/PGRP to
  the fd's `struct pty`; `poll_scan` learns the pty. 6 KTESTs (cooked line on
  RET, ERASE edits, ECHO bounce, raw passthrough, ONLCR, ISIG). `src/string.c`
  (mem*) added because the larger `struct file` made gcc emit an implicit memset;
  `PIPE_SIZE` 4096→16384.
- ✅ **31.2 — libvterm helper (`/bin/vterm`)**: libvterm 0.3.3 (neovim fork)
  vendored under `user/libvterm/`; `user/musl/vterm.c` links it against musl,
  owns the pty, forks the shell on the slave, and translates between libvterm's
  screen model and a line protocol — keys in (`u`/`k`/`s`), screen out
  (`z` geometry, `p` cell-run, `c` cursor, `f` flush, `x` exit).
- ✅ **31.3 — `vterm-mode` (`user/lisp/fr-term.l`)**: pure-Lisp major mode that
  forks the helper, polls its pipe each `frame-tick`, mirrors the grid into the
  buffer (text + ANSI faces, cursor = point) and forwards keys; `C-x`/`C-g` stay
  the frame's escape (char-mode vs copy-mode via `M-x`). Registered as a major
  mode; `M-x vterm` launches it.
- ✅ **31.4 — controlling-tty / job control**: busybox ash's job-control startup
  spins until it is the tty's foreground group; the fix was `SYS_SETSID` moving
  the caller into its **own process group** (`t->pgid = t->id`), plus the helper
  doing `tcsetpgrp(0, getpid())` pre-exec. busybox `sh` now runs with line
  editing, job control and coloured `ls`; `vi` edits a file full-screen from it.
- ✅ **31.5 — per-keystroke rendering**: the helper sets `VTERM_DAMAGE_ROW`
  merge but must call `vterm_screen_flush_damage()` before reading its dirty set,
  or libvterm holds each row's cell damage until a later multi-row write evicts
  it — so typed glyphs stayed invisible until RET. One-line fix in the helper's
  `flush_damage()`. Verified by QMP screendump **before** pressing RET: typed
  text appears as you type, in the shell and inside `vi`.

Open follow-ups (none block the flow): arrows are cooked to C-p/C-n/C-b/C-f
before Lisp sees them (vi uses hjkl); terminfo isn't staged (busybox vi is fine
on `TERM=xterm`; ncurses/less want it); `string-concat` still caps at ~2048
bytes (worked around by reading small pipe chunks).

---

## Phase 32 — Claude in the image: a native AI agent that rewrites the OS  ⏳ PLANNED

Make Claude a **co-resident of the live Lisp image**: describe a feature in plain
language and the OS grows it, because the agent generates Lisp and the running
image `eval`s it — no compile, no link, no reboot. Need a calculator? It didn't
exist a second ago and now it does. The thing a Unix process can't be (it can't
`eval` itself): **the OS as the agent.** The assistant's name lives in a Lisp
variable (`*assistant-name*`, default `"emiel"`) and is overridable from inside
the image; `M-x emiel` talks to it. **Spec:**
`docs/superpowers/specs/2026-06-19-assistant-in-the-image-design.md`

Decision: a **native Lisp agent** (HTTPS client to the Anthropic Messages API +
a tool-execution loop), NOT the Node-based Claude Code binary — that needs real
threads+futex, JIT-executable mmap, libuv's epoll/eventfd/timerfd backend, a C++
runtime and BoringSSL (a multi-month port, and still not "the OS as the agent").
Safety is **hybrid-by-scope**: ephemeral generated forms (a fresh `defun`, a
buffer) auto-eval instantly; persistent ones (file writes, redefining an existing
symbol, permanent commands) pause for a one-key preview+accept, with per-change
undo. Kept features persist to `/lib/claude/` and survive reboot — the OS
accretes the features you ask for.

Milestones (each its own spec→plan→build→notes cycle, gated on `make test`):

- ✅ **32.1 — proof of life** *(no kernel work beyond a dotted-quad
  `net_resolve`)*: a Lisp HTTP/JSON client (`http.l` + `json.l`, streamed to
  avoid the 2048-byte `string-concat` cap) and `assistant-stream`, which streams
  a real Messages API reply into the `*emiel*` buffer — via a **host-side
  TLS-terminating proxy** on the user-net gateway (`10.0.2.2`), so there is zero
  crypto in the OS on day one. `M-x emiel` greets you; `*assistant-name*`
  (default `"emiel"`) names the buffer + reply label. Verified by
  `tools/assistant_check.py` against a deterministic mock SSE endpoint (hermetic,
  tokenless) **and** a live-frame screendump. Plan:
  `docs/superpowers/plans/2026-06-19-assistant-milestone-1.md` · Notes:
  `docs/notes/phase-32.md`.
- ✅ **32.2 — tools + the apply gate**: `introspect_image` (symbols + living
  source), `eval_lisp`, `function_source`, `read_file`, `write_file`, `run_bash`;
  the agentic `assistant-converse` loop (parse streamed `tool_use`, run, loop
  `tool_result`); the ephemeral/persistent classifier with per-symbol-snapshot
  undo; `/lib/claude` boot-manifest persistence (+ a `mkdir` primitive). A fresh
  defun auto-applies; redefining an existing function is gated and undoable.
  Verified by `tools/assistant_check.py` (two-turn tool_use mock) and a live
  screendump of the loop (`docs/images/phase-32-agent.png`). Notes:
  `docs/notes/phase-32.md`.
- ✅ **32.3 — OpenRouter provider**: run the agent through **OpenRouter** (your
  own key, OpenRouter bills you, Claude + any model through one endpoint) — a
  fully-supported path, no client impersonation. Adds HTTP **chunked
  transfer-decoding** to `http.l` (real endpoints stream chunked) and an
  OpenAI chat-completions request/SSE/`tool_calls` path
  (`assistant-openrouter.l`) — the only provider (the Anthropic path was removed).
  Auth is `Authorization: Bearer`; `Host:` is `openrouter.ai` while the TCP target
  stays the proxy. Verified by `tools/assistant_check.py` (chunked decode + an
  OpenAI two-turn tool_calls mock). Plan:
  `docs/superpowers/plans/2026-06-19-assistant-milestone-3-openrouter.md`. (Also
  shipped here: a **dynamic `string-concat`** — the 2048-byte cap is gone, so file
  reads / transcripts / JSON no longer truncate.)
- ⏳ **32.4 — TLS (drop the proxy)**: vendor **BearSSL** (small, pure-C, no-heap,
  bare-metal-friendly) over the existing sockets, add a `getrandom` syscall to
  seed it, embed trust anchors; point `*assistant-endpoint-host*` straight at
  `openrouter.ai:443`. A direct HTTPS turn with no proxy. (Caveats: plaintext key
  on disk, weak VM entropy — neither blocks the flow.)
- ⏳ **32.5 — polish**: `assistant-mode` niceties, a minibuffer "ask" key
  (one-line "just do X" from anywhere), richer tools (apply-patch-style edits,
  multi-file features).

Builds on: the Lisp machine (24), sockets + TCP/IP (21–22), DNS (`resolve`),
M-x vterm / busybox (31), the mode system (28), text properties + faces (30).

---

## Later / advanced (capable-OS extensions, after the capstone)

- **SMP (multicore).** Secondary-core boot (PSCI `CPU_ON`), per-CPU data,
  **spinlocks** replacing IRQ-masking critical sections, per-CPU run queues, IPIs.
  *Very large* — locking touches everything, so best once the feature set is
  stable. A strong "capable OS" eventually wants this.
- **Users & permissions.** uid/gid, file permission bits, a `root` vs user
  distinction — multi-user fundamentals.
- **More syscalls / POSIX-ish polish.** `poll`/`select`, `fcntl`, working
  directory, environment, richer `/proc`-style introspection.

---

## Deferred — graphics (parked; revisit after the OS is capable)

Your call to defer all graphics. Kept here so the plans aren't lost; none are on
the current path.

- **mmap-the-framebuffer to user space** — exposing the Phase-12 framebuffer to an
  EL0 process (a sub-feature of mmap, skipped for now).
- **Display server (user space)** — a process that owns the screen: desktop +
  mouse cursor + back-buffer compositing.
- ~~**Window protocol + terminal client** — clients render into shared buffers;
  the shell runs in a window.~~ — **done differently**: external renderers use
  shared-memory surface buffers (25.6) and the shell runs in a buffer via
  `M-x vterm` (Phase 31).
- **i3-style tiling window manager** — tiling tree, workspaces, keybindings,
  borders, status bar.
- ~~virtio-input (mouse + keyboard)~~ — **built in 25.1.**
- ~~virtio-gpu~~ — **built in 25.2** (2D + hardware cursor plane).

(The previous user-space-WM roadmap is preserved in git history if you return to
graphics later.)

---

## Notes on sequencing

- **13 is the keystone.** A real `exec`/`wait` underpins coreutils, pipes,
  signals, and a useful shell.
- **19 (virtio transport) is the big rock.** Large, but it unlocks both storage
  *and* networking — build it carefully and reuse it.
- **Storage (19→20) and networking (21→22) are independent** once the transport
  exists. Persistent storage is the simpler first capstone; networking is the
  ambitious one.
- **22 (TCP/IP) is very large** — build it layer by layer (ARP/IP/ICMP first, then
  UDP, then TCP, then sockets), each its own testable milestone.
- Every phase stands alone (spec → test-first plan → build → notes, gated on
  `make test`) and leaves the OS working.
