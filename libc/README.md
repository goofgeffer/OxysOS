<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `libc/` — The C Library

**Phase**: 7 of [`../docs/project/PLAN.md`](../docs/project/PLAN.md). Sub-task
7.1 placed the first material here; sub-tasks 7.2 to 7.5 add the system-call
wrappers, the heap, the buffered input and output, and the runtime startup
object.
**Detailed design**: [`../docs/design/LIBC.md`](../docs/design/LIBC.md).

## Purpose

This directory holds the C library that user programs are linked against. It is
`MIT`-licensed, unlike the kernel above which it stands, so that a program
linking it is under no obligation the kernel's licence would impose;
[`../LICENSING.md`](../LICENSING.md) is the map and Section 2 of it states where
the boundary between the two falls.

**Nothing here is part of the kernel**, and the kernel is compiled without this
directory's include root in reach so that nothing here can become part of it by
accident. The one exception is named explicitly by a rule in the
[`../Makefile`](../Makefile): the boot-time self-test that asserts this code.
[`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 7, explains why that
self-test is presently the only thing that runs any of this, and what sub-task
7.5 changes about it.

## Contents

| Path | Description |
| ---- | ----------- |
| [`include/string.h`](include/string.h) | The declarations of ISO/IEC 9899:2011, Section 7.24, as this library implements them — and, at its head, which three of that section's functions are absent and what each is waiting for. |
| [`string/copying.c`](string/copying.c) | 7.24.2 and 7.24.3: `memcpy`, `memmove`, `strcpy`, `strncpy`, `strcat`, `strncat`. |
| [`string/comparison.c`](string/comparison.c) | 7.24.4: `memcmp`, `strcmp`, `strncmp`. |
| [`string/search.c`](string/search.c) | 7.24.5: `memchr`, `strchr`, `strcspn`, `strpbrk`, `strrchr`, `strspn`, `strstr`, `strtok`. |
| [`string/miscellaneous.c`](string/miscellaneous.c) | 7.24.6: `memset`, `strlen`. |

The four units divide Section 7.24 as the standard divides it, rather than by
size or by taste, so that a reader looking for the whole of that section finds it
arranged as that section is — and can see at a glance which of it is missing.

## What this directory is not

It is not where the system-call interface is defined. That is
[`../kernel/abi/`](../kernel/abi/), which is a separate include root under the
same permissive licence and which this library includes; the wrappers of sub-task
7.2 will be declared here and defined here, but the numbers they pass are the
kernel's. [`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 2, records
why the two are apart.

## Specifications implemented

- **ISO/IEC 9899:2011**, Section 7.24 (string handling), and Section 4,
  paragraph 6, which fixes what a freestanding implementation may assume — the
  four headers `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` and `<float.h>` among
  them, and the reason nothing here needs a hosted library to compile.
- **System V Application Binary Interface**, AMD64 supplement, Section 3.1.2:
  the LP64 model, in which `size_t` is 64 bits.

Each file cites the subsection of 7.24 that defines each function it holds. The
corpus is enumerated in
[`../docs/project/REFERENCES.md`](../docs/project/REFERENCES.md).

## How it is asserted

[`../kernel/test/verify_string.c`](../kernel/test/verify_string.c), run by
`make verify` at every boot.
[`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 5, holds the table
pairing each assertion with the silent failure it exists to catch, and Section
5.1 records the negative test that found a defect in the test itself.
