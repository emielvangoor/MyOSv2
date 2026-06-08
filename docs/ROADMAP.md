# MyOSv2 — Roadmap

A living map of where the OS has been and where it can go next. Each **future**
phase here is a *candidate*, described in enough detail to choose between them —
not a commitment. When you pick one, it still gets its own full cycle:
brainstorm → spec (`docs/superpowers/specs/`) → test-first plan
(`docs/superpowers/plans/`) → build (TDD, gated on `make test`) → notes
(`docs/notes/`).

Target throughout: **ARM64 (AArch64), C + assembly, QEMU `virt` board.**

---

## Done (Phases 0–12)

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
| 12 | Graphics (ramfb) | 1280×720 framebuffer, draw primitives, bitmap font |

---

## The north star: a user-space i3-style tiling window manager

The headline goal is an **i3-like tiling window manager that runs as a user-space
process** — windows tiled automatically into a tree of splits, keyboard-driven,
with a mouse cursor for focus and border-resize, and the shell's terminal living
inside a tile.

"User space" is the demanding part. The kernel owns the framebuffer and the input
devices; the window manager is an **ordinary EL0 program** that asks the kernel
for those resources and composites everything itself. That only works once a
handful of foundations exist — which is why most of this roadmap is
**prerequisites**, built bottom-up, before the WM itself appears.

### Why the prerequisites are non-negotiable

A user-space compositor must be able to:
- **run as its own program** alongside its clients → process lifecycle (exec/wait),
  real binaries (ELF loader);
- **allocate memory freely** for its window tree and buffers → a user-space heap;
- **see the framebuffer from EL0** and **share pixel buffers with clients** →
  `mmap` + shared memory (the linchpin);
- **talk to clients** (create window, resize, deliver input, commit a frame) →
  an IPC channel;
- **receive mouse + keyboard in user space** → virtio-input + an event device.

Only then does the window system itself (display server → protocol + terminal
client → tiling WM) make sense.

### Critical path

```
Foundations          13 exec+wait ─► 14 ELF loader/ real bin ─► 15 user heap
                                                                     │
Share + talk         16 mmap + shared memory ─► 17 IPC channel ─► 18 signals
                          (framebuffer + client buffers)   (window protocol)  (Ctrl-C / close)
                                                                     │
Input                19 virtio transport + virtio-input ─► 20 input → user space
                                                                     │
Window system        21 display-server core (user space)  ◄── mmap fb + cursor
                          │
                     22 window protocol + terminal client  ◄── shell in a window
                          │
                     23 i3-style tiling WM  ◄── THE GOAL: tiles, workspaces, keybinds
```

Storage (virtio-blk, on-disk FS), networking, virtio-gpu, and SMP are **not on
this path** — they're parked in *Deferred* at the bottom.

---

## Phase 13 — Process lifecycle: `exec` + `exit` status + `wait`

**Why (for the WM):** the WM and every client are separate processes; the WM must
launch them and notice when they exit (to close their windows). This also finishes
the Unix process model and resolves the Phase 11 ASID-recycling gap.

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
exit status, and processes are cleaned up (ASIDs reused).

**Depends on:** 9, 10, 11. **Size:** medium.

---

## Phase 14 — ELF loader + real `/bin` programs

**Why (for the WM):** the WM, the terminal, and other clients are distinct
binaries loaded at their own addresses. Flat blobs all linked at one VA won't do.

**Adds**
- A minimal **ELF64 loader**: map each `PT_LOAD` segment at its `p_vaddr` with the
  right perms (RX code / RW data), zero the `.bss` tail.
- Several small programs built separately and embedded in the initrd under `/bin`.

**Key files:** new `elf.c`, `proc.c`/`vm.c`, `Makefile`, `user/*.c`.

**Done looks like:** `ls /bin` shows several programs; each runs; multi-segment
binaries load correctly.

**Depends on:** 13. **Size:** medium.

---

## Phase 15 — User-space dynamic memory (`brk`/`sbrk` + `malloc`)

**Why (for the WM):** a window manager needs a heap — for its tiling tree, window
records, the event queue, per-client state. Fixed stack/data pages aren't enough.

**Adds**
- `SYS_BRK`/`SYS_SBRK` growing the user heap, mapping fresh pages on demand.
- A small user-space `malloc`/`free` in `ulib`.

**Key files:** `vm.c` (grow heap, demand-map), `syscall.c`, `user/ulib.c`.

**Done looks like:** a program `malloc`s/uses/frees memory; touching a fresh heap
page maps it on demand.

**Depends on:** 6b, 13. **Size:** small-medium.

---

## Phase 16 — `mmap` + shared memory  ⟵ the linchpin

**Why (for the WM):** this is what makes a *user-space* compositor possible at
all. The WM must (a) see the **framebuffer** from EL0, and (b) share **pixel
buffers** with clients so a client can render a window and the WM can read it
without copying through the kernel.

**Adds**
- `SYS_MMAP` with anonymous mappings (demand-zeroed pages) — generalizes the
  Phase 15 heap.
- **Framebuffer mapping:** map the ramfb framebuffer's physical pages into a user
  address space (a device mapping), so a user process can draw straight to the
  screen.
- **Shared-memory objects:** `shm_create()` → a handle; multiple processes
  `mmap` the same handle and share the **same physical pages**. This reuses the
  page-refcount machinery already built for COW (Phase 9) — shared pages are just
  refcounted pages mapped into more than one address space.

**Key files:** `vm.c` (shared mappings, device mapping, refcounts), new `shm.c`,
`syscall.c` (`mmap`/`shm_create`), `fb.c` (expose the framebuffer for mapping).

**Done looks like:** a user program `mmap`s the framebuffer and draws a rectangle
that appears on screen; two processes `mmap` one shm handle and one sees the
other's writes.

**Depends on:** 6b, 9 (refcounts), 12 (framebuffer), 15. **Size:** large.

---

## Phase 17 — IPC channel (message ports)

**Why (for the WM):** the window protocol — *create surface, configure size,
deliver input, commit a frame* — needs a real bidirectional, message-framed
channel between the WM and each client, with the ability to pass a **shm handle**
along with a message (so a client can hand the WM its buffer).

**Adds**
- A **port/channel** abstraction: connect to a named server port; send/receive
  discrete messages (datagram/SEQPACKET-style, not a raw byte stream); attach a
  shared-memory handle to a message.
- Blocking `recv` that wakes on arrival; non-blocking `poll`-style check so the
  WM can service input *and* client messages in one loop.
- (Pipes — `ls | cat` — fall out of the same buffering work and can be included.)

**Key files:** new `ipc.c` (ports, message queues), `syscall.c`
(`connect`/`send`/`recv`/`poll`), `sched.c` (block/wake), fd-table integration.

**Done looks like:** two processes exchange messages over a named port; a message
carries an shm handle the receiver then maps and reads.

**Depends on:** 13, 16. **Size:** large.

---

## Phase 18 — Signals

**Why (for the WM):** a terminal needs **Ctrl-C** to interrupt the foreground
program, and the WM wants to ask a client to close. Asynchronous notification.

**Adds**
- `SYS_KILL(pid, sig)`, a per-process pending mask, default actions, optional user
  handlers delivered on the return-to-EL0 path; `SIGINT` from the console.

**Key files:** new `signal.c`, `sched.c`, `syscall.c`, `vectors.S`.

**Done looks like:** Ctrl-C kills a long-running program and returns to the shell;
a handler-equipped program runs its handler.

**Depends on:** 13. **Size:** medium-large (touches the trap-return path).
*(Useful companion; can trail the memory/IPC work.)*

---

## Phase 19 — virtio transport + virtio-input (mouse + keyboard)

**Why (for the WM):** a window manager is nothing without a pointer and a
keyboard. The `virt` board has no PS/2, so input comes from **virtio-input**,
which requires building the generic **virtio-mmio + virtqueue** machinery first.

**Adds**
- Generic **virtio-mmio transport**: device probe, feature negotiation, a
  **virtqueue** (descriptor / available / used rings), notify + (IRQ or polled)
  completion.
- A **virtio-input** driver bound twice: a **tablet** (absolute X/Y + buttons —
  absolute coords skip pointer-acceleration math) and a **keyboard** (keycodes).
- Decode the event stream into `(x, y, buttons)` and key up/down events.

**Key files:** new `virtio.c` (transport + virtqueue), `virtio_input.c`, GIC
wiring; `Makefile` (`-device virtio-tablet-device,-keyboard-device`).

**Done looks like:** moving the host mouse / pressing keys over the QEMU window
logs live pointer coordinates, button state, and keycodes in the kernel.

**Depends on:** 3 (IRQs), 6 (MMIO). **Size:** large (the transport is the bulk;
reused later for gpu/blk/net).

---

## Phase 20 — Input delivery to user space

**Why (for the WM):** the WM (not the kernel) must interpret input — focus
follows the pointer, keybindings drive tiling. So raw events flow to a user
process.

**Adds**
- An **input event device** (e.g. read from an `/dev/input` fd, or a dedicated
  `recv`): the focused/owning process reads a stream of `{type, code, value}`
  events. Kernel just queues; user space gives them meaning.
- Non-blocking/poll integration so the WM's single loop can wait on input *and*
  IPC together.

**Key files:** `virtio_input.c` (enqueue to a user-readable queue), new
`evdev.c`, `syscall.c`/`ipc.c` (poll), VFS device node.

**Done looks like:** a small user program reads pointer + key events and prints
them — proving input reaches EL0.

**Depends on:** 17 (poll), 19. **Size:** medium.

---

## Phase 21 — Display-server core (user space)

**Why (for the WM):** the first genuinely user-space graphics — a program that
**owns the screen**. It establishes the compositor loop everything else plugs
into.

**Adds**
- A user program that `mmap`s the framebuffer (Phase 16), reads input (Phase 20),
  and runs a **compositor loop** ~30–60×/s: draw a desktop background, a **mouse
  cursor** sprite at the pointer, into an off-screen **back-buffer**, then copy to
  the framebuffer (no flicker).
- A user-space copy of the draw primitives + font (ported from Phase 12), or a
  shared `libdraw`.

**Key files:** new `user/wm/` (compositor main, back-buffer, cursor, libdraw),
`user/user.ld`/Makefile for the new binary.

**Done looks like:** a user-space program shows a desktop with a mouse cursor you
can move — drawn entirely from EL0.

**Depends on:** 16, 20. **Size:** large.

---

## Phase 22 — Window protocol + terminal client

**Why (for the WM):** turn the compositor into a real window system — clients
create windows, render into shared buffers, and receive input. The first client
is a **terminal** running the shell, so "the shell lives in a window."

**Adds**
- A minimal **window protocol** over the IPC channel (Phase 17): `connect`,
  `create_surface`, `set_size`/`configure`, `attach(shm buffer)` + `commit`,
  and inbound `pointer`/`key`/`focus`/`close` events. The compositor composites
  each committed client buffer into its window rectangle.
- A **terminal emulator** client: a character-cell grid sized to its window,
  putchar/newline/backspace/scroll rendered with the font, drawing into its shm
  buffer; it runs the Phase-10 shell with stdio bound to the terminal (writes →
  cells, key events → shell stdin).

**Key files:** new `user/libwin/` (protocol client lib), `user/term/` (terminal +
shell glue), compositor-side protocol handling in `user/wm/`.

**Done looks like:** a window appears containing a working shell — type a command,
see its output rendered in the window; closing the shell closes the window.

**Depends on:** 13, 17, 21. **Size:** large.

---

## Phase 23 — i3-style tiling window manager  ⟵ THE GOAL

**Why:** the destination — automatic tiling, keyboard-driven, multiple windows.

**Adds**
- A **tiling tree** of containers (horizontal/vertical splits); new windows tile
  into the focused container; closing one re-flows the layout.
- **Workspaces** (switch with `Mod+number`), **focus movement**
  (`Mod+h/j/k/l`), **split direction** (`Mod+v`/`Mod+s`), **launch a terminal**
  (`Mod+Enter`), **move window**, **fullscreen**, optional **floating** windows.
- Window **borders** with focus highlight and a simple **status bar** (workspace
  list + clock). Mouse for focus-follows-pointer and dragging a tile **border to
  resize**.

**Key files:** `user/wm/` (layout tree, workspaces, keybindings, bar), building on
the compositor + protocol from 21–22.

**Done looks like:** launch several terminals with `Mod+Enter`; they tile
automatically; move focus and re-split with the keyboard; switch workspaces;
resize a split by dragging its border — an i3-feeling WM, all in user space.

**Depends on:** 22. **Size:** large.

---

## Deferred — parallel / not required for the WM

Kept so the ideas aren't lost; none are on the window-manager critical path.

- **virtio-blk + on-disk filesystem (persistence).** A real disk via the
  Phase-19 virtio transport, then a FAT32 / custom FS under the VFS so files
  survive reboot. *Large.* Independent track.
- **`virtio-gpu`.** A richer display than ramfb (dynamic resolution, multiple
  outputs, hardware cursor, 2D accel) over virtqueues. The WM works fine on ramfb;
  this is a later upgrade once virtio-gpu is worth it.
- **Networking.** virtio-net + Ethernet/ARP/IP/UDP (+ maybe minimal TCP) and a
  socket API. *Very large* stretch.
- **SMP (multicore).** Secondary-core boot, spinlocks, per-CPU run queues, IPIs.
  *Very large* — best once the feature set is stable, since locking touches
  everything.

---

## Notes on sequencing

- **16 (mmap + shared memory) is the true linchpin.** A user-space compositor is
  impossible without mapping the framebuffer to EL0 and sharing client buffers.
  Build it carefully — it reuses the Phase-9 page refcounts.
- **19 (virtio transport) is the other big rock.** It's large but pays off
  repeatedly (input now; disk/net/gpu later).
- **21→23 are where it visibly becomes a window manager.** Each is large, but each
  is a demo you can *see*: cursor on a desktop → a shell in a window → tiled
  workspaces.
- **Signals (18) and pipes (in 17) are flexible in order** — useful companions,
  not strict gates. Slot them where Ctrl-C / pipelines start to matter.
- The path is long but every phase stands alone (spec → test-first plan → build →
  notes, gated on `make test`) and leaves the OS working.
