<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `userland/` — The Programs That Run Upon This System

**Phase**: 7 of [`../docs/project/PLAN.md`](../docs/project/PLAN.md). Sub-task
7.5 placed the first material here; sub-task 7.6 added the utilities and the three
programs that assert them, and 7.7 the
ramdisk they are carried upon.
**Detailed design**: [`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section
11, which is the runtime and the link procedure every program here is built by,
and Section 12, which is the five utilities, the calls beneath them and the three
programs that assert what they cannot assert of themselves.
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
| [`echo/main.c`](echo/main.c) | Sub-task 7.6: writes its operands separated by one space and followed by one newline. `-n` is an operand and a backslash is an ordinary character, both cases being implementation-defined in IEEE Std 1003.1-2017. |
| [`cat/main.c`](cat/main.c) | Sub-task 7.6: copies each operand to the standard output. No operand is a diagnostic rather than a copy of the standard input, there being no call that reads one. |
| [`ls/main.c`](ls/main.c) | Sub-task 7.6: lists each directory operand, one entry to a line, with `-a`. It does not sort, this system having no locale and no heap sized by a directory it has not finished reading. |
| [`mkdir/main.c`](mkdir/main.c) | Sub-task 7.6: creates each operand, with `-p`. No file mode creation mask is applied, this system having none. |
| [`rm/main.c`](rm/main.c) | Sub-task 7.6: removes each operand, with `-f`. A directory is refused, there being no call that removes one. |
| [`arg-check/main.c`](arg-check/main.c) | Sub-task 7.6: compares the argument vector it was given against the vector it expects, and ends with the number of comparisons that failed. It exists because nothing in this kernel can read what a program printed. |
| [`exec-check/main.c`](exec-check/main.c) | Sub-task 7.6: becomes `arg-check` through `execve`, so that a vector crosses an address space that is destroyed. It has no assertions of its own — upon success it no longer exists, and the status the kernel collects is `arg-check`'s. |
| [`file-check/main.c`](file-check/main.c) | Sub-task 7.6: asserts the six filesystem calls by comparison — a file of known contents read byte for byte, a directory of known entries listed, and twenty refusals asserted by the **name** of the failure rather than by its sign. It was written because a negative test showed that a `read` delivering no bytes at all was reported by nothing. |

**The three `-check` programs are not utilities and are not shipped as such.**
They exist to assert what a utility cannot assert of itself, for the reason
[`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 12.4, gives: nothing
in this kernel can read what a program printed, so a program whose whole output
is text proves only that it did not fault.

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

**The link rule is generated and not written out**, once per name in
`USER_PROGRAMS`. Sub-task 7.5 wrote one rule for one program and noted that 7.6
would repeat the shape; it is not repeated, because eight copies of a link
command is eight places for a flag to be forgotten, and the way that fails is
that one program links without the archive and the defect appears as an undefined
symbol in whichever program was edited last. **A program is a directory under
this one holding `main.c`, and the directory's name is the program's**, so adding
one is adding a name to that list and a directory beside the others.

A stripped copy is then made for embedding. `build/user/<name>.elf` keeps its
debugging information and is the file a debugger is pointed at;
`build/user/<name>.embed.elf` is what the kernel image carries, and is a fifth of
the size.

## What a program here may assume

The System V Application Binary Interface, AMD64 supplement, Section 3.4.1, and
nothing beyond it: the argument count at the stack pointer, the two vectors above
it, a sixteen-byte-aligned stack, and a null frame pointer that `_start` sets.

**Since sub-task 7.6 the argument vector is real.** It was empty until then —
this kernel's `execve` refused both vectors for want of a convention about where
a program finds its strings — and the convention is now Section 3.4.1's own, with
the strings at the top of the stack and the pointers below them. A program is
bounded at `SYSCALL_ARGUMENT_COUNT_MAXIMUM` strings per vector and
`SYSCALL_ARGUMENT_BYTES_MAXIMUM` bytes for the two together, both published in
`<oxys/syscall_abi.h>` because a program that will be refused is entitled to know
what it will be refused against.

**The environment vector exists and is empty.** `envp[0]` is a null pointer, not
`envp` itself: nothing in this system sets an environment, so `getenv` remains
absent from `<stdlib.h>` and the header records why.

**A program may open, read and list a file, and may not create or write one.**
The kernel has fourteen system calls; `OxysOpen` accepts `SYSCALL_OPEN_READ` and
`SYSCALL_OPEN_DIRECTORY` and refuses every other bit, and `OxysWrite` still
reaches the two diagnostic descriptors alone. `fopen` is accordingly still absent
from `<stdio.h>`, a program reaching a file through `<syscall.h>` and a
descriptor instead. **And `stdin` is still permanently at end-of-file**: the
`read` of 7.6 reads a file and there is no call that reads a console. Each of
those is recorded at the head of the header that would otherwise declare it, and
the reasoning is [`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 12.7.

**There is no working directory**, so a relative path resolves against the root.
That is why `ls` with no operand lists `/` rather than `.`, and it is sub-task
8.3's `cd` to fix.
