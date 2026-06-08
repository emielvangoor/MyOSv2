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

**Current limitations worth closing** (these motivate the next phases):
- `fork` exists but there's no `exec`/`wait`/`exit-status` — processes never get
  reaped, so ASIDs are never recycled (noted at the end of Phase 11).
- One embedded program (`/bin/init`); no way to load arbitrary binaries.
- No IPC (no pipes, no signals), no user-space dynamic memory, no real disk.

---

## Recommended core path

Graphics is an **independent output track** — it depends only on the MMU
(Phase 6) and PMM (Phase 4), *not* on the process work — so it can go first
without blocking anything. After it, the Unix process model builds up in order:

```
12 graphics (framebuffer)          ◄── independent; do it next, it's self-contained
       │
13 exec+wait ──► 14 ELF loader / real /bin ──► 15 pipes ──► 16 signals
       │
       └─ unlocks: shell runs real programs the Unix way (fork→exec→wait)
```

Then branch into whichever track interests you:

```
memory track:   17 user heap (brk/malloc) ──► (later) mmap / demand paging
storage track:  18 virtio-blk driver ──► 19 on-disk filesystem (persistence)
```

**Deferred for now** (your call): SMP/multicore and networking — see the bottom.

---

## Phase 12 — Graphics: high-res framebuffer (`ramfb`)

**Why now:** a brand-new device class and instantly rewarding — real pixels in a
window. It's fully **independent** of the process-model phases (depends only on
the MMU and the page allocator), so it slots in cleanly right here without
blocking anything else. If you'd rather finish the Unix process model first, it
can equally well go after Phase 16 — placement is free.

**The approach — `ramfb`.** QEMU's `-device ramfb` exposes a simple linear
framebuffer through the **fw_cfg** interface. We allocate a contiguous pixel
buffer (e.g. 1280×720 or 1920×1080 × 4 bytes ≈ 2–8 MB via `pmm_alloc_pages`),
then hand QEMU its physical address + width/height/stride/format with one DMA
write to the `etc/ramfb` fw_cfg file. After that it's a raw `XRGB8888` array we
write colors into — **arbitrary resolution, genuinely high-res**, with no PCI and
no virtqueues. (A richer `virtio-gpu` path — dynamic resolution, a cursor — is a
later upgrade once the virtio transport from Phase 18 exists; see Deferred.)

**Adds**
- A tiny **fw_cfg driver** (`fwcfg.c`): MMIO at `0x09020000`, select a file by
  name from the fw_cfg directory, read it, and DMA-write to it.
- A **framebuffer driver** (`fb.c`): allocate the buffer, register it via
  `etc/ramfb` (big-endian `RAMFBCfg`: address, fourcc `XR24`, flags, width,
  height, stride), expose `fb_info()` (base/width/height/pitch).
- **Drawing primitives** (`draw.c`): `put_pixel`, `fill_rect`, `clear`, plus an
  8×8/8×16 bitmap **font** for `draw_char`/`draw_text`. Stretch: blit an embedded
  image, and/or a framebuffer **console** (mirror `kprintf`/the shell to screen).
- **Run setup:** swap `-display none` → `-display cocoa` (your Mac) and add
  `-device ramfb`, keeping `-serial stdio` for the shell. Kept easy to close.

**Key files:** new `fwcfg.c`/`.h`, `fb.c`/`.h`, `draw.c`/`.h`, `font8x16.h`;
`mmu.c` (map the framebuffer region cacheable/normal); `Makefile` (QEMU flags);
`kmain.c` (init + a demo frame); `tests.c` (framebuffer-geometry tests).

**Done looks like:** a QEMU window opens at your chosen resolution showing drawn
shapes and text (e.g. "MyOSv2" rendered from the bitmap font), while the shell
keeps running on the serial console.

**Depends on:** MMU/MMIO (6), PMM (4). **Size:** medium.

**Testing note:** pixels on screen aren't unit-testable, so tests pin the
*machinery* — framebuffer geometry (pitch = width × bpp), `put_pixel` writing the
right offset in a mock buffer, `fill_rect` bounds, font glyph lookup — and the
visible frame is the manual confirmation (same pattern as the shell).

---

## Phase 13 — Process lifecycle: `exec` + `exit` status + `wait`

**Why:** finishes the Unix process model. `fork` (Phase 9) without
`exec`/`wait` is only half a model. This resolves the Phase 11 ASID-recycling
limitation.

**Adds**
- `SYS_EXEC(path)` — replace the *current* process's image with a program loaded
  from a file (tear down the old user address space, build a new one, reset the
  user PC/SP). Returns only on failure.
- `SYS_EXIT(status)` — carry an exit code; mark the thread a **zombie** (exited
  but not yet reaped) instead of vanishing.
- `SYS_WAIT`/`SYS_WAITPID` — block until a child exits, collect its status, then
  **reap** it: free its kernel stack, free its address space (`as_destroy`:
  decref every page, free page-table pages), and **free + recycle its ASID**.

**Key files:** `syscall.c` (exec/wait/exit), `sched.c` (zombie state, parent
links, reaping), `vm.c` (`as_destroy`, ASID free list), `proc.c` (exec load
path), `user/sh.c` (run external commands), a second tiny user program.

**Done looks like:** at the shell you type a program name → it `fork`s, the child
`exec`s the binary, the shell `wait`s and prints the exit status, then returns to
the prompt. Processes are properly cleaned up; ASIDs get reused.

**Depends on:** 9, 10, 11. **Size:** medium.

---

## Phase 14 — ELF loader + real `/bin` programs

**Why:** today user programs are flat binaries all linked at `USER_CODE_VA`. To
ship several real programs and load them properly, parse ELF and place each
segment at its own virtual address.

**Adds**
- A minimal **ELF64 loader**: read the program headers, map each `PT_LOAD`
  segment at its `p_vaddr` with the right permissions (RX for code, RW for data),
  zero the `.bss` tail.
- Several small user programs (`echo`, `cat`, `ls`, `true`/`false`) built as
  separate binaries and embedded into the initrd under `/bin`.

**Key files:** new `elf.c`, `proc.c`/`vm.c` (map segments), `Makefile` (build
multiple user binaries into the initrd), `user/*.c`.

**Done looks like:** `ls /bin` shows several programs; running each from the
shell works; the loader handles a multi-segment binary.

**Depends on:** 13. **Size:** medium.

---

## Phase 15 — Pipes

**Why:** the first IPC primitive; enables shell pipelines like `ls | cat`.

**Adds**
- `SYS_PIPE` — create a pipe, return a read fd + a write fd.
- A kernel **ring-buffer pipe object** behind the VFS `file` abstraction: `read`
  blocks until data or all writers closed; `write` blocks until space.
- `SYS_DUP`/`dup2` for wiring fds. Shell parses `|` and connects a child's stdout
  to the next child's stdin.

**Key files:** new `pipe.c`, `vfs.c` (pipe file ops), `syscall.c`
(`pipe`/`dup`), `sched.c` (block/wake on pipe state), `user/sh.c` (pipeline
parsing).

**Done looks like:** `ls | cat` at the shell streams output through the pipe;
a blocked reader wakes when the writer produces data.

**Depends on:** 13 (and the fd table from 8). **Size:** medium.

---

## Phase 16 — Signals

**Why:** asynchronous notification, and a real **Ctrl-C** to interrupt a running
program.

**Adds**
- `SYS_KILL(pid, sig)`, a per-process **pending-signal mask**, default actions
  (terminate), and optional **user handlers** (`sigaction`) delivered on the
  return-to-EL0 path via a signal trampoline.
- Console Ctrl-C → `SIGINT` to the foreground process.

**Key files:** new `signal.c`, `sched.c` (pending signals, deliver on `eret`),
`syscall.c` (`kill`/`sigaction`), `vectors.S` (handler trampoline), console/UART
(Ctrl-C detection).

**Done looks like:** start a long-running program, press Ctrl-C, it's killed and
you're back at the shell; a program with a `SIGINT` handler runs its handler.

**Depends on:** 13. **Size:** medium-large (touches the trap-return path).

---

## Phase 17 — User-space dynamic memory (`brk`/`sbrk` + `malloc`)

**Why:** let user programs allocate memory at runtime instead of only fixed
stack/data pages.

**Adds**
- `SYS_BRK`/`SYS_SBRK` growing the user heap region, mapping fresh pages on
  demand (a controlled version of demand paging).
- A tiny user-space `malloc`/`free` in `ulib`.

**Key files:** `vm.c` (grow the user heap, demand-map pages), `syscall.c`
(`brk`/`sbrk`), `user/ulib.c` (`malloc`/`free`).

**Done looks like:** a user program `malloc`s, writes, and frees memory; touching
a fresh heap page maps it on demand.

**Depends on:** 6b. **Size:** small-medium. *(Natural follow-on: `mmap` + true
demand paging / lazy zero-fill.)*

---

## Phase 18 — virtio-blk block device driver

**Why:** the first real device beyond the UART/timer, and the gateway to
persistent storage.

**Adds**
- virtio-mmio transport + **virtqueue** setup, then a **virtio-blk** driver that
  reads/writes 512-byte sectors, backed by a QEMU `-drive` disk image.
- A generic block-device abstraction the filesystem layer can sit on.

**Key files:** new `virtio.c`, `virtio_blk.c`, `block.h`; `Makefile` QEMU flags
(`-drive`).

**Done looks like:** read and write sectors to a disk image; bytes written in one
run are readable in the next.

**Depends on:** MMU/MMIO (6), interrupts (3). **Size:** large (new driver class).
Also lays the **virtio transport** groundwork that a later `virtio-gpu` graphics
upgrade would reuse.

---

## Phase 19 — On-disk filesystem (persistence)

**Why:** turn the block device into real, persistent files mounted under the VFS.

**Adds**
- A real filesystem as a VFS `fs_type` backed by the block device — either
  **FAT32** (interoperable with your host) or a **simple custom inode FS**.
  Format/mount/lookup/read/write/create.

**Key files:** new `fatfs.c` or `sfs.c`, VFS integration, a `mkfs` helper.

**Done looks like:** create a file from the shell, reboot QEMU, the file is still
there.

**Depends on:** VFS (7), block driver (18). **Size:** large.

---

## Deferred — not now (revisit later)

You've parked these for now; kept here so the ideas aren't lost.

- **SMP (multicore).** Secondary-core boot (PSCI `CPU_ON`), per-CPU data,
  **spinlocks** replacing IRQ-masking critical sections, per-CPU run queues, IPIs.
  *Very large* — forces revisiting locking across the whole kernel, so it's most
  valuable once the feature set is stable.
- **Networking.** virtio-net driver + Ethernet/ARP/IP/UDP (maybe minimal TCP) and
  a socket-style API. *Very large* stretch goal. Depends on the virtio transport
  from Phase 18.
- **`virtio-gpu` (richer graphics).** An upgrade to Phase 12's framebuffer:
  dynamic resolution, multiple displays, a hardware cursor, 2D acceleration — over
  virtqueues. Worth it only after the virtio transport (Phase 18) exists; until
  then `ramfb` already gives high-res pixels.

---

## Notes on sequencing

- **12 (graphics) is free-floating.** It depends only on the MMU and PMM, so it
  can go next (recommended — it's self-contained and fun) or any time later.
- **13 is the keystone of the process track.** Almost everything social between
  processes (pipes, signals, a useful shell) gets easier once
  `exec`/`wait`/`exit` exist.
- **14 pairs tightly with 13** — exec is most convincing with real `/bin`
  programs to exec into.
- **Memory (17) and storage (18→19) are independent tracks** — do either order.
