<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `userland/` — The Programs That Run Upon This System

**Phase**: 7 of [`../docs/project/PLAN.md`](../docs/project/PLAN.md). Sub-task
7.5 placed the first material here; sub-task 7.6 adds the utilities and 7.7 the
ramdisk they are carried upon.
**Detailed design**: [`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section
11, which is the runtime and the link procedure every program here is built by.
**Licence**: `MIT`, as [`../LICENSING.md`](../LICENSING.md), Section 1, assigns
to this path — the same licence as `libc/`, and for the same reason: a program
that links the C library should be under no obligation the kernel's licence would
impose.

## Purpose

This directory holds programs, and a program here is the first thing in this
project that is neither the kernel nor a part of the kernel's image by necessity.
Everything under `kernel/`, `drivers/` and `graphics/` runs at privilege level 0
and is linked into one image; everything here is compiled separately, linked
against the C library's archive, and loaded by the kernel as an ELF file.

**The distinction is the point of the directory and not a filing convenience.** A
program here may call nothing but `libc/include/` and the system calls beneath
it. It has no access to the kernel's headers, no way to reach a kernel symbol,
and no means of failing in a way that costs the machine rather than itself —
which is what makes the boundary worth having somewhere a person can see.

## Contents

| Path | Description |
| ---- | ----------- |
| [`startup-check/main.c`](startup-check/main.c) | Sub-task 7.5: the first program in this project built from source rather than composed byte by byte, and the thing that asserts the runtime startup object and the link procedure. It makes its own assertions and ends with the number that failed; `kernel/test/libc/startup.c` runs it and checks that number. |

## How a program here is built

There is no `make` target for it, and that is deliberate. The programs are
embedded in the kernel image — the self-test that runs one reads it from there —
so they are a dependency of the image exactly as `build/trampoline.bin` is, and
`make all` builds them. A phony target would also have obliged an amendment to
[`../PROJECT_GUIDELINES.md`](../PROJECT_GUIDELINES.md), Section 3, whose list of
targets is checked against the `Makefile` by `tools/check-docs.sh`.

The four parts of the build are in the `Makefile` under "The user-mode build of
sub-task 7.5":

1. **`USER_CFLAGS`** — the kernel's diagnostic regime without its code model, and
   with the C library's include root. Every warning flag is kept: a program built
   here is held to the standard the kernel is held to.
2. **`build/user/liboxys.a`** — the same `LIBC_SOURCES` compiled a second time
   with those flags, collected by `ar rcs`.
3. **`build/user/crt0.o`** — [`../libc/crt/crt0.asm`](../libc/crt/crt0.asm), the
   first instructions of every program.
4. **The link** — `ld -n -T libc/user.ld`, with the archive named *after* the
   program's own objects, a linker resolving an archive's members against the
   references it has already seen.

A stripped copy is then made for embedding. `build/user/<name>.elf` keeps its
debugging information and is the file a debugger is pointed at;
`build/user/<name>.embed.elf` is what the kernel image carries, and is a fifth of
the size.

## What a program here may assume

The System V Application Binary Interface, AMD64 supplement, Section 3.4.1, and
nothing beyond it: the argument count at the stack pointer, the two vectors above
it, a sixteen-byte-aligned stack, and a null frame pointer that `_start` sets.
Both vectors are presently empty — this kernel's `execve` accepts neither, there
being no convention yet fixed for where a program finds its strings — so `argc`
is zero and `argv[0]` is a null pointer.

**There is no environment, no file to open, and nothing to read.** The kernel has
eight system calls; `getenv` is absent from `<stdlib.h>` for that reason, the
file operations are absent from `<stdio.h>` for that reason, and `stdin` is
permanently at end-of-file for that reason. Each of those is recorded at the head
of the header that would otherwise declare it.
