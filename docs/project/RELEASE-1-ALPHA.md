<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Oxys 1 Alpha

**Ordinal form**: `1-alpha`. **Tag**: `v1-alpha`. **Image**: `oxys-1-alpha.iso`.
**Cut**: 2026-09-16, at the close of Phase 8, sub-task 8.7, as
[`PLAN.md`](PLAN.md) and [`VERSIONING.md`](VERSIONING.md), Section 11.1, had
fixed since 2026-09-11, and at the project owner's direction on the day.

**Document status**: The release notes [`VERSIONING.md`](VERSIONING.md), Section
9.4, requires of every release: one document, published with the image, carrying
what kind of release this is and the judgement that decided it, what changed and
which sub-tasks closed, which editions were published, where it was tested, and
the checksum of the image. It is written once and not revised: what was true of
the image on the day it was cut is what this says, and what became true later is
[`STATUS.md`](STATUS.md)'s to say.

## 1. What kind of release this is

**A pre-release, the first of the two [`VERSIONING.md`](VERSIONING.md), Section
5.4, reserves for `Oxys 1`, and the first release of any kind under the scheme.**
It is superseded by nothing yet and will be superseded by `Oxys 1 Beta` and then
by `Oxys 1`, and by nothing else. It takes no name, no point and no edition, as
Section 5.4 has it.

**The judgement that decided it** is the one Section 11.1 recorded in advance: a
system on the way to its first release has two moments worth publishing an
image of, and this is the first — the first image that does anything when a
person types at it. Phases 1 to 8 are complete. It boots, manages memory,
services interrupts, drives its devices, mounts an EXT2 volume, runs programs
at privilege level 3, and presents a shell with line editing, pipelines,
redirection and job control. It was cut at the sub-task and not at a date, and
five days after the plan was written, because the plan fixed the moment to what
the system could do and the system came to do it.

## 2. What it does

A person who boots the image is met by a shell at `oxys$/> `. They can type a
command line and edit it with the cursor keys, Home, End, Delete and the control
characters every shell of this lineage accepts, and recall the last thirty-two
lines with the arrow keys. They can run the seventeen programs upon the root
filesystem — `ls`, `cat`, `echo`, `mkdir`, `rmdir`, `rm`, `touch`, `cp`, `mv`,
`wc`, `head`, `tail`, `grep`, `sort`, `ps`, the editor `micro`, and `sh` itself
— and the thirteen commands built into the shell: `cd`, `pwd`, `export`,
`unset`, `exit`, `true`, `false`, `help`, `clear`, `jobs`, `fg`, `bg` and
`kill`. They can set a variable and export it, redirect input and output with
every operator of IEEE Std 1003.1-2017, Section 2.7, but the here-document,
join commands with `;`, `&&`, `||` and `!`, join programs with `|`, run a
pipeline in the background with `&`, interrupt one with control-C, stop one
with control-Z and bring it back with `fg`. They can write a file with `micro`
and read it back. `help` lists everything, one command to a line.

Beneath that stands the whole of Phases 1 to 8, which [`STATUS.md`](STATUS.md)
describes one paragraph to a phase and [`PLAN.md`](PLAN.md) records sub-task by
sub-task: a Multiboot2 boot into long mode; a physical frame allocator, a
higher-half kernel, a virtual allocator and a heap, copy-on-write cloning of
address spaces; an interrupt descriptor table, the 8259A and the local and I/O
APICs, the PS/2 keyboard and mouse; the serial line, the text-mode display, ATA,
AHCI and SDHCI drivers, PCI enumeration, a block layer and a buffer cache; an
EXT2 filesystem read and written through a virtual filesystem layer, and an
initial ramdisk mounted as the root; a linear framebuffer, drawing primitives, a
bitmap font, a graphical console and a compositor; a system-call interface of
twenty-nine calls, an ELF loader, processes and threads, a round-robin
scheduler with pre-emption across two processors, spinlocks, inter-processor
interrupts and the bring-up of the application processors; a C library of
strings, a heap, buffered streams, a line editor and signals; and the shell.

## 3. What changed, and which sub-tasks closed

Every sub-task of Phases 1 to 8 is closed — 1.1 to 8.7, seventy-two in all —
and [`PLAN.md`](PLAN.md) marks each `Implemented` with the self-test that
asserts it. [`HISTORY.md`](HISTORY.md) is the index of every change that closed
one, newest first, each row pointing at its commit and its design document.
The whole of the work since the previous published image — `v0.1.0`, withdrawn
on 2026-09-09 and not part of this scheme — is Phases 6 to 8: the graphics, the
system calls, the processes and the scheduler, the C library, and the shell.

## 4. Editions

**None.** [`VERSIONING.md`](VERSIONING.md), Section 7, defines editions as build
selections of one source, and there is one selection: the image built by `make
iso` from `main`. No edition has been defined and none is published.

## 5. Where it was tested

The conditions of [`VERSIONING.md`](VERSIONING.md), Section 10, and what was done
about each:

| Condition | Done |
| --------- | ---- |
| `make verify` passes at the commit being tagged | Yes: sixty-six assertions report `passed` or `sound` and none reports a failure, [`TESTING-RECORD.md`](TESTING-RECORD.md), 2026-09-16. |
| `make clang-check` passes at that commit | Yes, every translation unit without a diagnostic under the second compiler. |
| Every document affected by the work is up to date | Yes, by the discipline of `PROJECT_GUIDELINES.md`, Section 2, and `make lint` at the commit. |
| Booted under QEMU and VirtualBox, with the runs recorded | Yes: QEMU q35 with two processors, headless and driven over the serial line; VirtualBox 7.2.0, typed at the PS/2 keyboard. Bochs 3.1 as well, to the prompt. [`TESTING-RECORD.md`](TESTING-RECORD.md), the rows of 2026-09-16. |
| Booted on physical hardware | **No.** Section 10 leaves this to the project owner's call per release, and it was not called for. The one physical machine this kernel has run upon is the laptop of [`TESTING.md`](TESTING.md), Section 5.1, where the storage drivers of Phase 4 and the filesystem of Phase 5 were reported from; nothing of Phases 6 to 8 has been seen upon a machine. This is an alpha, and it says so here rather than being silent about it. |

The environments each phase has been observed in are the table of
[`STATUS.md`](STATUS.md), Section 3.

## 6. What it does not have

Stated here so that nobody installs it and discovers the omissions.

- **It boots by BIOS alone.** The UEFI path is Phase 12; a machine with no
  compatibility support module will not boot it.
- **It has none of Phase 13's hardening.** No NX, SMEP or SMAP, no
  write-exclusive-or-execute in the kernel's mappings, no stack canaries, no
  address space layout randomisation; [`../../SECURITY.md`](../../SECURITY.md)
  is the statement of what is and is not in force, and at this release the
  answer is none of it. It is not a system to expose to anything.
- **It runs user programs upon one processor.** Every user thread is pinned to
  the bootstrap processor because the allocators, the process tables and the
  filesystem layer a system call reaches are unsynchronised;
  [`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 10,
  limitation 1, is the list. The application processors run the scheduler's
  idle thread.
- **No desktop, no networking, no cryptography.** Phases 9, 10 and 11.
- **The root is a ramdisk of two mebibytes**, rebuilt at every boot from the
  image; a file written there is gone at the next boot. A volume the machine
  carries is mounted at `/mnt` and is where a file that should last belongs.
- **The terminal has no line discipline.** `cat` reading the terminal ends at
  a control-D by its own reading, a control-C does not flush what was typed
  after it, and a line longer than the display is wide is drawn wrongly once it
  wraps upon a serial terminal. [`../design/SHELL.md`](../design/SHELL.md),
  Sections 6, 21, 24 and 29, are the limitations of the shell in full.
- **No `rename`, no `lseek` a program can reach, no clock a program can read,
  no `sleep`.** `mv` is a link and an unlink; `tail` reads its whole input.
- **An orphan is nobody's.** A job left running at `exit` is collected by no
  one until Phase 9's `init`.

## 7. The image

Built by `make iso` from the commit tagged `v1-alpha`, upon the host
[`TOOLCHAIN.md`](TOOLCHAIN.md) describes, and recorded as **build 1** of the
register [`BUILDS.md`](BUILDS.md), which resumes with this release after being
emptied on 2026-09-13 — numbering restarting at 1, as that document records
was decided. The archived image is kept under `~/oxys-builds/` as the register
keeps it, and a copy named `oxys-1-alpha.iso` beside it, as
[`VERSIONING.md`](VERSIONING.md), Section 9.2, names an image.

| Image | SHA-256 |
| ----- | ------- |
| `oxys-1-alpha.iso` | _recorded with build 1 in the commit that follows the tag_ |

The checksum is that of the archived image; an image rebuilt from the same
commit is the same kernel and the same programs and not the same bytes, the
ISO and the ramdisk carrying the timestamps of their making.
