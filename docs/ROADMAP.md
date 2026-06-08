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

## Phase 22 — TCP/IP stack + sockets  ⟵ CAPSTONE

**Why:** the destination — the OS speaks to the network with a real protocol stack
and a sockets API user programs can use.

**Adds**
- **Ethernet + ARP** (resolve IPs to MACs), **IPv4** (routing to QEMU's
  gateway), **ICMP** (ping), **UDP**, and **TCP** (handshake, sequencing,
  retransmit, basic windowing).
- A **sockets API**: `socket`/`bind`/`connect`/`listen`/`accept`/`send`/`recv`/
  `close`, integrated with the fd table (so `read`/`write`/`poll` work on
  sockets); DHCP or a static IP for addressing.
- A demo client (e.g. fetch a page over TCP) and/or a tiny server.

**Key files:** new `net/` (`eth.c`, `arp.c`, `ip.c`, `icmp.c`, `udp.c`, `tcp.c`,
`socket.c`), `syscall.c` (socket calls), fd-table integration.

**Done looks like:** `ping` from the guest reaches QEMU's gateway and replies; a
user program opens a TCP connection and exchanges data with a host service.

**Depends on:** 17 (blocking-fd pattern), 21. **Size:** very large (build it
incrementally: ARP/IP/ICMP → UDP → TCP → sockets).

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
- **Window protocol + terminal client** — clients render into shared buffers; the
  shell runs in a window.
- **i3-style tiling window manager** — tiling tree, workspaces, keybindings,
  borders, status bar.
- **virtio-input (mouse + keyboard)** — needed for a cursor/GUI; reuses the
  Phase-19 virtio transport.
- **virtio-gpu** — a richer display than ramfb (dynamic resolution, hardware
  cursor, 2D accel).

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
