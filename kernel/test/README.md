# `kernel/test/` — The Boot-Time Self-Tests

**Phase**: introduced in Phase 6, sub-task 6.1, as a reorganisation of tests
written from Phase 2 onward. This directory grows with every subsequent phase.
**Detailed design**: [`../../docs/project/TESTING.md`](../../docs/project/TESTING.md);
each subsystem's own design document holds the table pairing its assertions with
the failure each would catch.

## Purpose

This directory holds the tests the kernel runs upon itself at boot, and the
fixture they are conducted upon.

**There is no test harness, and there will be none before Phase 7**, there being
no userland to run one in. So the tests are not a separate program: they are part
of the kernel image, executed by `KernelMain` in the order the subsystems are
initialised, because a test cannot run before the thing it asserts exists. That
sequence in `KernelMain` is the subsystem dependency ordering of
`docs/design/ARCHITECTURE.md`, Section 4, made executable.

Every test reports its own verdict and returns. **None halts the machine, and
none may.** A kernel that stopped at the first failed assertion would report one
failure where it might have reported nine, and the run that matters most is the
one where several things are broken at once. `make verify` reads the verdicts out
of the serial log and fails if any of them says `FAILED`;
`docs/project/TESTING.md`, Section 1, records what that target asserts and why
both of its assertions are necessary.

## Why these are here and not in `kernel.c`

Until sub-task 6.1 they were in `kernel.c`, which had grown to 9,050 lines of
which the entry point was the last 250. Every sub-task of every phase edited that
one file, and `KernelMain` — the thing a reader opens `kernel.c` to find — stood
at line 8,797.

The tests are now one file per subsystem, declared by
[`../include/oxys/verify.h`](../include/oxys/verify.h), which is the only thing
`kernel.c` needs to know about them. `kernel.c` is 708 lines and is again what its
own header block says it is.

Nothing was rewritten in the move: the assertions, their order and their wording
are as they were, and the serial output after the change differs from the output
before it only in the size of the kernel image and in two counters that jitter
between runs.

## Contents

| Path | Description |
| ---- | ----------- |
| `volume.c` | The fixture. Two block devices backed by arrays in `.bss`, and a complete EXT2 volume composed within them byte by byte, so that every storage and filesystem assertion holds upon a machine with no disk. The second volume is a copy of the first with the owner of one file altered, so that an assertion can state which volume a path reached. |
| `verify_memory.c` | Phase 2: the physical frame allocator, the paging hierarchy, the virtual address allocator and the heap, per-frame reference counting, the resolution of a copy-on-write fault, and the cloning of an address space. |
| `verify_interrupts.c` | Phase 3: the descriptor table and its gates, the 256 stubs and the uniform trap frame they construct, the dispatcher's routing, and the exception handlers. |
| `verify_privilege.c` | Phase 6, sub-task 6.1: the user-mode descriptors and their ordering, the task state segment, the interrupt stack table exercised rather than inspected, and the `SYSCALL` configuration exercised by executing it. |
| `verify_devices.c` | Phases 3 and 4: the 8259A controllers, the interval timer, the PS/2 keyboard, the 16550 serial adapter, the VGA display, and PCI enumeration. |
| `verify_storage.c` | Phase 4: the ATA driver, the generic block layer, and the buffer cache. |
| `verify_ext2.c` | Phase 5: the EXT2 format, in full, against the composed volume; and `KernelReportVolumes`, which reports upon whatever volume the machine actually carries. |
| `verify_vfs.c` | Sub-task 5.8: the virtual filesystem layer, its mounts and its node identity; and `KernelVfsProbeVolume`, which exercises a real volume through the layer. |

## The two headers

[`../include/oxys/verify.h`](../include/oxys/verify.h) declares the entry points
`KernelMain` calls, together with the two things `kernel.c` supplies to the tests:
the parsed boot information, and whether the boot loader's command line names a
given option.

[`../include/oxys/testvolume.h`](../include/oxys/testvolume.h) declares the
fixture — the two block devices, the composed volume's geometry, and the routines
that address a field of it directly. Three of the test files include it: the
storage tests, which need a device; and the EXT2 and virtual filesystem tests,
which need a volume.

## The distinction between a test and a probe

Two routines here are not self-tests and are named so that they cannot be
mistaken for one: `KernelReportVolumes` and `KernelVfsProbeVolume`. They examine
whatever volume the machine actually carries and **assert nothing**, there being
nothing to assert about a disk this kernel did not write.

Their value is the one thing a composed fixture cannot supply. The composed
volume shares this kernel's understanding of the format, so a misreading of the
specification would be composed into it and then asserted against itself. The
probes produce a volume that a tool outside this kernel — `e2fsck`, `debugfs`,
`dumpe2fs` — can judge, and that is how the defect in the recorded deletion time
described in `docs/storage/VFS.md`, Section 11.1, was found: every assertion in
this directory passed, and the volume was nevertheless wrong.

The probes that write are selected by the boot loader's command line and never
run by default. A kernel that wrote to a stranger's disk merely by having been
booted would impose a real cost for nothing. The GRUB entries that set those
options are in [`../../boot/grub/grub.cfg`](../../boot/grub/grub.cfg).

## Specifications implemented

Each test cites the specification of the subsystem it asserts, in its own file
header. The corpus is enumerated in
[`../../docs/project/REFERENCES.md`](../../docs/project/REFERENCES.md).

## Present limitations

1. **Every test runs at every boot.** There is no means of selecting one, and no
   need of one yet; the whole corpus costs a fraction of a second. When it ceases
   to, selection belongs on the boot loader's command line beside the write
   probes.
2. **A test cannot assert a refusal that panics.** Several routines in the kernel
   treat an impossible argument as unrecoverable, and no means of surviving a
   panic exists before the test harness of Phase 7. Where that is so, the test
   asserts the admitting direction and says that it does.
3. **Nothing here is safe against concurrent execution.** The fixture is a pair
   of static arrays and the tests write to them. From sub-task 6.8 the corpus
   must run on one processor, or be given a fixture per processor.
4. **The fixture is one volume geometry**: 1024-byte blocks, one block group, 32
   inodes. A volume with several groups is exercised only by the probes against
   real images, which `docs/project/TESTING.md` records at both block sizes this
   kernel accepts.
