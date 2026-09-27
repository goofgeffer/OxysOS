<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Oxys-OS System Architecture

**Phase**: all phases of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: the whole tree; the boot's order in
[`../../kernel/kernel.c`](../../kernel/kernel.c) and
[`../../kernel/init/`](../../kernel/init/).
**Specifications**: none of its own; each subsystem's document cites its own.

How the system is divided: the premises it is built on, where each kind of code
lives and the rules that decide it, the order in which subsystems depend on one
another, the privilege model, and the diagnostic paths.

## 1. Design premises

Oxys-OS is a monolithic kernel for x86_64. A monolithic kernel is chosen because
the project's purpose is the direct exercise of hardware interfaces, which a
microkernel's message passing would obscure. Three properties are designed in
from the start rather than retrofitted:

1. **Symmetric multi-processing.** Every kernel structure is written on the
   assumption that several processors will reach it, and its file's header says
   how it is synchronised ([`CONCURRENCY.md`](CONCURRENCY.md)). Retrofitting locks
   to structures built without them is where the worst concurrency faults come
   from.
2. **Copy-on-write.** The frame allocator keeps a reference count per frame from
   the outset, so the fault resolution and the address-space cloning above it
   ([`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md)) did not require every consumer of the
   allocator to change.
3. **Boot-protocol neutrality.** Everything above `kernel/handoff/` consumes the
   neutral `BootInformation` of `<oxys/boot/bootinfo.h>` and cannot tell which
   boot protocol supplied it, so the UEFI path of Phase 12 is a new reader and
   not a change to the kernel ([`BOOT.md`](BOOT.md)).

## 2. The source tree

| Directory | Holds |
| --------- | ----- |
| `boot/` | The Multiboot2 header, the 32-bit entry, the long-mode transition, the application processors' trampoline, and the GRUB configuration. |
| `kernel/` | The kernel core: the entry point, memory management, processes and scheduling, the ELF loader, the block layer, the filesystems, the terminal and the boot handoff. |
| `kernel/arch/x86_64/` | What could not survive a change of processor: `cpu/`, `interrupt/`, `syscall/`, `smp/`, `mm/`, `proc/` (Section 2.3). |
| `kernel/include/oxys/` | The kernel's header corpus, grouped to mirror the source tree (Section 2.4). |
| `kernel/abi/oxys/` | A second include root: the system-call interface a program may rely on, licensed permissively so that the `MIT` C library can include it, and holding constants and structures, never a kernel declaration. |
| `kernel/init/` | The phases of the boot, one file each, called by `KernelMain` in the order of Section 4. |
| `kernel/test/` | The boot-time self-tests, one file per subsystem, and the fixtures they run on ([`../../kernel/test/README.md`](../../kernel/test/README.md)). |
| `drivers/` | Device drivers, one directory per device class. |
| `graphics/` | The framebuffer, the drawing primitives, the face, the console, the compositor, the pointer and the window manager. |
| `crypto/` | The entropy pool, and the generator, hash and cipher of Phase 10. |
| `libc/` | The C library linked into user programs; `libc/include/` is its header root. |
| `userland/` | The user programs: the shell, the utilities, `init`, the session and the desktop's programs. |
| `art/` | The project owner's artwork and the palette, with the commands that convert them. |
| `fonts/` | Third-party typefaces under their own licence, and the tables rendered from them. |
| `etc/` | The shipped configuration staged into `/etc` on the ramdisk. |
| `tools/` | Scripts the build and the checks run. |
| `docs/` | The documentation, indexed by [`../README.md`](../README.md). |
| `net/`, `uefi/` | Reserved for the networking of Phase 11 and the UEFI path of Phase 12. |

What each file holds is written once, in its directory's `README.md`; this
document gives the rules that decide which directory a file belongs in.

### 2.1 When a subsystem becomes a directory

A translation unit is divided when it stops being readable as one thing. The
sign is its own header: when the `Purpose` line describes a fraction of what the
file does, the file has become several. `kernel/fs/ext2/`, `kernel/fs/vfs/`,
`drivers/ata/` and `kernel/test/storage/ext2/` are such divisions.

- **Divide along the lines faults fall on**, not only the lines functions group
  by. In EXT2 a fault in `path.c` resolves a name to the wrong file, in `name.c`
  leaves a volume malformed, and in `alloc.c` leaves the bitmaps disagreeing
  with their summaries: three kinds of wrongness found in three ways.
- **A division is not a rewrite.** Nothing is reordered or improved in passing,
  so the check afterwards is mechanical: the same set of functions, and every
  non-comment line surviving but for the qualifiers the new boundaries force.
  An improvement in the same diff would hide a defect the division introduced.
- **What the parts share goes in an `internal.h` beside them**, not in the
  public corpus. The corpus is what a consumer may depend on; a subsystem's
  private seams are not that.

### 2.2 Where a file belongs

A directory's `README.md` is a claim about what is in it
(`PROJECT_GUIDELINES.md`, Section 10). A file that contradicts the claim is in
the wrong place, and the fix is to move the file, not to amend the prose to
admit an exception: an exception admitted spends the rule. The tests that
follow from this:

- **A driver programs a device.** A registry or a cache that holds no register,
  port or timing rule is not a driver: the block layer and the buffer cache are
  in `kernel/block/`, above `drivers/` and below `kernel/fs/`.
- **A boot protocol's wire format stays in `kernel/handoff/`**, with its own
  private header, because premise 3 forbids anything above the handoff to know
  it.
- **A fixture used only by the self-tests lives in `kernel/test/`**, beside the
  tests, as `volume.h` and `program.h` do.

### 2.3 The architecture boundary

`kernel/arch/x86_64/` holds what is answerable to the processor's manual rather
than to an algorithm. The two kinds of defect are found differently: a mistake
in `mm/pmm.c` is found by reasoning about the bitmap, and a mistake in
`arch/x86_64/cpu/tss.c` is found only by checking a citation against Intel's
manual. The path says which kind of file a reader has open.
[`../../kernel/arch/README.md`](../../kernel/arch/README.md) states the test and
applies it file by file.

- **`cpu/` is about a processor and `smp/` about several.** The spinlock is in
  `cpu/` because masking interrupts while it is held is owed on one core too.
- **Two subsystems span both trees.** `kernel/mm/` keeps the frame allocator,
  the arena and the heap, and `arch/x86_64/mm/` the four-level hierarchy and
  what depends on its shape; `kernel/proc/` keeps the tables and the scheduler,
  and `arch/x86_64/proc/` the context switch.
- **This is not preparation for a port**, which no phase plans. It groups the
  kernel by what a defect is answerable to, and it does not isolate the
  portable part: the portable core includes `<oxys/arch/...>` headers
  seventeen times across eight files, each listed with its reason in
  `tools/check-docs.sh`, Section 8, and graded in `kernel/arch/README.md`.

### 2.4 The header corpus and the self-tests

**`kernel/include/oxys/` mirrors the source tree**, so a header's path says what
it describes:

| Under `oxys/` | Mirrors |
| ------------- | ------- |
| `types.h`, `kernel.h` | Nothing: universal, owned by no subsystem. |
| `arch/cpu/`, `arch/interrupt/`, `arch/syscall/`, `arch/smp/`, `arch/mm/` | `kernel/arch/x86_64/` |
| `mm/`, `proc/`, `exec/`, `acpi/`, `block/`, `fs/`, `crypto/`, `terminal/`, `test/` | The kernel directory of the same name, and `crypto/` |
| `boot/` | `kernel/handoff/` |
| `dev/`, `dev/storage/` | `drivers/` |
| `gfx/` | `graphics/` |

The path is `arch/`, not `arch/x86_64/`: a consumer's `#include` says that what
it uses is processor-bound, and must not change if the processor ever did.

**`kernel/test/` is grouped by the subsystem each test asserts**, and a file in
`kernel/test/libc/` is exactly a test compiled against the C library's headers,
so one `Makefile` pattern covers them and a new one is added by placing it
there. The test functions keep their `KernelVerify` prefix, because every
subsystem's symbols share one namespace.

## 3. Where a file is described

| To find | Look in |
| ------- | ------- |
| What a file contains | Its directory's `README.md`, one line per file |
| How a subsystem works | Its document, indexed by [`README.md`](README.md), [`../devices/README.md`](../devices/README.md) and [`../storage/README.md`](../storage/README.md) |
| The detail of a function | The source file's comments |
| Which files are built | `C_SOURCES`, `ASM_SOURCES` and the user programs of the `Makefile` |

## 4. Subsystem dependency ordering

The phases of `PLAN.md` follow these dependencies, which must not be violated:

```
Phase 1  Bootstrapping
   |
   +--> Phase 2  Memory management ------+
   |         ^                           |
   |         | (page-fault delivery)     | (kernel heap)
   |         |                           v
   +--> Phase 3  Interrupts -------------+--> Phase 4  Device drivers
                                                     |
                                                     v
                                            Phase 5  EXT2 filesystem
                                                     |
                                                     v
                Phase 6  Graphics, system calls, processes, SMP
                                                     |
                                                     v
                            Phase 7  Userland and C library
                                                     |
                                                     v
                                          Phase 8  Shell
                                                     |
                          +--------------------------+--------------+
                          v                          v              v
              Phase 9  Desktop       Phase 10  Crypto, partitions   Phase 11  Networking
                          |                          |              |
                          +--------------------------+--------------+
                                                     v
                                       Phase 12  UEFI transition
                                                     |
                                                     v
                                       Phase 13  Polish and hardening
```

**Phases 2 and 3 depend on each other in one place.** Copy-on-write resolution
(2.7) cannot run until Phase 3 delivers page faults, so 2.1 to 2.6 come first,
then Phase 3, then 2.7 and 2.8.

**The graphics of 6.2 to 6.6 precede the processes they could have waited for**,
because nothing in them needs a process: a framebuffer is memory to map, and the
primitives, face and compositor are arithmetic on it. Every later phase gains a
readable console by it. What does need processes, the window manager and its
client protocol, is Phase 9. The cost is that the surface interface was designed
before a user-mode client existed, which 9.2 revisited.

**The locks (6.13) precede the application processors (6.14)**, because every
mechanism of 6.13 can be exercised on one processor, while a second processor
started without locks would corrupt the machine in ways no assertion catches.

**The boot follows the same order at run time.** `KernelMain` calls one phase
function per file of `kernel/init/`: early, memory, display, frame references,
interrupts, devices, entropy, processes, processors, storage, the userland
self-tests, and the session. Two places differ from the phase numbers, each
commented in `KernelMain`:

- **The display follows memory directly**, taking the framebuffer's range from
  the arena before anything fragments it.
- **Storage follows the processors**: the bus is enumerated once everything
  driven so far is proved, so its failures are reported through channels known
  to work.

Each phase runs its subsystems' self-tests as soon as they are established, and
none earlier, because a test run before its subsystem exists reads zeroes and
can pass.

## 5. Privilege and address-space model

The kernel occupies the upper half of the canonical 48-bit address space and is
mapped into every address space, so a system call or an interrupt needs no
change of page-table root; user processes occupy the lower half
([`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md)). A program is loaded into an address
space of its own, entered at privilege level 3 by `IRETQ`, returned to by
`SYSRET` from a system call, and ended if it faults
([`PRIVILEGE.md`](PRIVILEGE.md), [`PROCESS.md`](PROCESS.md)). The timer
pre-empts threads on a run queue ([`SCHEDULER.md`](SCHEDULER.md)).

## 6. Diagnostic policy

**Every diagnostic goes to every output path** the machine has: the COM1 serial
port, which the automated tests read ([`../project/TESTING.md`](../project/TESTING.md)),
the VGA text display, and the framebuffer console. A failure is recorded
whichever device still works.

- **`KernelWriteString` writes to all of them unconditionally** and is the only
  routine that names an output device. Deciding between paths there would put
  knowledge of the display mode into the one routine that must work before the
  mode is known.
- **The serial path reverts to polling whenever interrupts are masked**, which
  is the ordinary path of a panic: a message left in a buffer nothing drains is
  lost ([`../devices/SERIAL.md`](../devices/SERIAL.md)).
- **A quiet boot silences the screens, never the serial line**, which is the
  record the tests and a bug report rely on.
- **A fault the kernel cannot survive takes the display**: an abort, a
  non-maskable interrupt, a malformed descriptor table, or any fault the kernel
  raises within itself draws a full-screen page composed for it
  ([`FAULTSCREEN.md`](FAULTSCREEN.md)); `ExceptionDispositionOf` decides which
  ([`INTERRUPTS.md`](INTERRUPTS.md)). A fault a program raises costs that
  program alone and draws nothing, since announcing the end of the machine for
  it would be false.

## Verification

The structure is asserted by `tools/check-docs.sh`, run by `make lint`, rather
than by a self-test:

| Asserted | The silent failure it would catch |
| -------- | --------------------------------- |
| Every inclusion of an `<oxys/arch/...>` header from the portable core is listed with a reason, and every listed one still exists. | The boundary eroded by an unrecorded crossing, or an exemption left behind. |
| Every document is indexed, every relative link resolves, and every section reference names a heading that exists. | A reader sent to a file or a section that is not there. |
| Every top-level source directory that holds tracked files has a `README.md`, and the build targets `PROJECT_GUIDELINES.md` names are the `Makefile`'s. | A directory with no index; a documented target that does not exist. |

The boot order of Section 4 is asserted by `make verify`: a phase that ran
before one it depends on would fail that phase's self-tests.

## Limitations

1. The portable core is not isolated from the architecture: seventeen
   crossings are recorded, and a handful of x86 instructions (`sti; hlt`,
   `cli; hlt`) remain in `kernel/kernel.c` and `kernel/proc/`.
2. `net/` and `uefi/` are reserved and empty until Phases 11 and 12.

## Appendix A. Reference: capacity limits

The bounds a person or a program can reach, with the constant that sets each.
A **chunk** row is not a limit: the table grows by a chunk of that size at a time,
up to 256 chunks, and memory is the practical limit
([`MEMORY-LAYOUT.md`](MEMORY-LAYOUT.md), Section 14). A **fixed** row is a
static bound. An **inline** row is held without an allocation and grows past;
a **limit** row is a policy ceiling on that growth, not a size anything is
allocated at. What happens at a bound, whether a refusal, a cut or a drop, is
in the constant's header, which is the authority.

| What | Kind | Value | Constant, header |
| ---- | ---- | ----: | ---------------- |
| Processes | chunk | 64 | `PROCESS_CHUNK`, `proc/process.h` |
| Threads | chunk | 128 | `THREAD_CHUNK`, `proc/process.h` |
| Filesystem nodes held | chunk | 64 | `VFS_NODE_CHUNK`, `fs/vfs.h` |
| Open files, machine-wide | chunk | 32 | `VFS_FILE_CHUNK`, `fs/vfs.h` |
| Pipes | chunk | 8 | `VFS_PIPE_CHUNK`, `fs/pipe.h` |
| Threads in one process | fixed | 8 | `PROCESS_THREAD_MAXIMUM`, `proc/process.h` |
| Descriptors in one process, held inline | inline | 16 | `PROCESS_DESCRIPTOR_INLINE`, `proc/process.h` |
| Descriptors in one process, most | limit | 1024 | `SYSCALL_DESCRIPTOR_LIMIT`, `abi/oxys/syscall_abi.h` |
| Filesystem types | chunk | 4 | `VFS_FILESYSTEM_CHUNK`, `fs/vfs.h` |
| Mounts | chunk | 4 | `VFS_MOUNT_CHUNK`, `fs/vfs.h` |
| Blocks in the buffer cache | fixed | 64 | `BUFFER_CAPACITY`, `block/buffer.h` |
| Block devices | chunk | 8 | `BLOCK_DEVICE_CHUNK`, `block/block.h` |
| Windows | chunk | 16 | `WINDOW_CHUNK`, `gfx/window.h` |
| Events queued per window | fixed | 32 | `WINDOW_EVENT_CAPACITY`, `gfx/window.h` |
| Window title, characters | fixed | 31 | `WINDOW_TITLE_CAPACITY`, `gfx/window.h` |
| Compositor layers | fixed | 4 | `COMPOSITOR_LAYER_CAPACITY`, `gfx/compositor.h` |
| Processors | fixed | 64 | `PER_CPU_MAXIMUM`, `arch/cpu/percpu.h` |
| Terminal input queue, bytes | fixed | 1024 | `TERMINAL_QUEUE_CAPACITY`, `terminal/terminal.h` |
| Keyboard buffer, keys | fixed | 128 | `KEYBOARD_BUFFER_CAPACITY`, `dev/keyboard.h` |
| PCI functions | chunk | 64 | `PCI_FUNCTION_CHUNK`, `drivers/pci/pci.c` |
| Loadable segments in a program | fixed | 16 | `ELF_SEGMENT_MAXIMUM`, `exec/elf.h` |
| Path, bytes | fixed | 1023 | `VFS_PATH_MAXIMUM`, `fs/vfs.h` |
| Path through a system call, bytes | fixed | 255 | `SYSCALL_PATH_MAXIMUM`, `abi/oxys/syscall_abi.h` |
| Name of one component, bytes | fixed | 255 | `VFS_NAME_MAXIMUM`, `fs/vfs.h` |
| Symbolic links followed in one path | fixed | 8 | `VFS_SYMLINK_DEPTH_MAXIMUM`, `fs/vfs.h` |
| Arguments to `execve`, and environment strings, each | fixed | 16 | `SYSCALL_ARGUMENT_COUNT_MAXIMUM`, `abi/oxys/syscall_abi.h` |
| Bytes of both vectors to `execve` | fixed | 2048 | `SYSCALL_ARGUMENT_BYTES_MAXIMUM`, `abi/oxys/syscall_abi.h` |
| Entries in one `poll` | fixed | 8 | `SYSCALL_POLL_MAXIMUM`, `abi/oxys/syscall_abi.h` |
| Notifications waiting in the kernel; the oldest dropped | fixed | 8 | `SYSCALL_NOTIFICATION_QUEUE`, `abi/oxys/syscall_abi.h` |
| Text of one notification, bytes | fixed | 63 | `SYSCALL_NOTIFICATION_TEXT_MAXIMUM`, `abi/oxys/syscall_abi.h` |

Headers are under `kernel/include/oxys/` unless the path says otherwise.
