<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `kernel/abi/` — The Interface a Program Is Entitled To

**Phase**: 7, sub-task 7.1, of
[`../../docs/project/PLAN.md`](../../docs/project/PLAN.md), which is where the
division this directory exists for was made. The interface itself dates from
sub-tasks 6.7 and 6.11.
**Detailed design**: [`../../docs/design/LIBC.md`](../../docs/design/LIBC.md),
Section 2; and [`../../LICENSING.md`](../../LICENSING.md), Section 2.1, which
required the division and named it the first thing to be done about licensing.

## Purpose

This directory is a **second include root**, holding the part of this kernel's
interface that a program is entitled to know — and nothing else. It is licensed
`MIT` rather than `LGPL-3.0-or-later`, so that it may be included by anything:
this kernel, the C library in [`../../libc/`](../../libc/), a program written by
somebody who has never seen the rest of this repository, and a C library that is
not this one.

**It is a root of its own and not a subdirectory of
[`../include/`](../include/), and that is the whole point.** A library that
added `-Ikernel/include` in order to reach one permissive header would have every
header of the kernel within reach, and would be one `#include` away from the
thing the division exists to prevent. It adds `-Ikernel/abi` and can reach
exactly what it is entitled to.

## Contents

| Path | Description |
| ---- | ----------- |
| [`oxys/syscall_abi.h`](oxys/syscall_abi.h) | The system-call interface: the register convention and its one departure from the System V AMD64 convention, the seven call numbers, the eight results a call may fail with, the bound upon a path, and the address at which the kernel's half of the address space begins. |

## The rule this directory is under

**Nothing here may acquire a declaration.** A function declared in this
directory would be a function the kernel and every program had to agree existed,
which is an obligation neither of them asked for; the value of an interface
header is that it defines constants and a convention and commits neither side to
a symbol. The wrappers of sub-task 7.2 are declared by the C library and are the
C library's.

**A number here is a number that has been handed out.** The call numbers were
already fixed before this directory existed — sub-task 6.11 appended its four
rather than interleaving them for exactly this reason — and the obligation binds
harder now that this file is the thing programs are compiled against rather than
a header internal to the kernel.

## What the kernel kept

[`../include/oxys/syscall.h`](../include/oxys/syscall.h): the three
model-specific registers that configure the mechanism, the flags cleared upon
entry, the frame the entry path saves, the dispatch, and the validation of a
caller's arguments. It includes the header above, so a consumer of it sees what
it always saw. Its design is
[`../../docs/design/PRIVILEGE.md`](../../docs/design/PRIVILEGE.md).

## Specifications implemented

- **Intel 64 and IA-32 Architectures Software Developer's Manual**, Volume 2B,
  `SYSCALL` and `SYSRET`: why the fourth argument is in `R10` and not in `RCX`.
- **System V Application Binary Interface**, AMD64 supplement, Section 3.2.3: the
  six integer argument registers, from which the convention here departs in
  exactly one place.
