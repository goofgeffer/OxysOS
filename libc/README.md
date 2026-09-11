<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `libc/` — The C Library

**Phase**: 7 of [`../docs/project/PLAN.md`](../docs/project/PLAN.md). Sub-task
7.1 placed the first material here and sub-task 7.2 the system-call wrappers;
sub-tasks 7.3 to 7.5 add the heap, the buffered input and output, and the runtime
startup object.
**Detailed design**: [`../docs/design/LIBC.md`](../docs/design/LIBC.md), Sections
2 to 7 for sub-task 7.1 and Section 8 for sub-task 7.2.

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
| [`string/error.c`](string/error.c) | 7.24.6.2: `strerror`. Added at sub-task 7.2 rather than 7.1, the numbers it maps being the ones the wrappers set. |
| [`include/errno.h`](include/errno.h) | ISO/IEC 9899:2011, Section 7.5: `errno`, and the numbers a library function may set it to. Each of the kernel's failure results negated, with a `_Static_assert` holding the two halves together. |
| [`include/syscall.h`](include/syscall.h) | The seven system-call wrappers, the raw invocation beneath them, and the translation of a kernel result into a result and an `errno`. |
| [`syscall/invoke.asm`](syscall/invoke.asm) | The `SYSCALL` instruction itself, one routine per number of arguments. It holds no relocation, which is what lets the self-test copy its bytes into a program and run them at privilege level 3. |
| [`syscall/result.c`](syscall/result.c) | The translation, and the `errno` object — the only thing in this library that writes `errno`. |
| [`syscall/calls.c`](syscall/calls.c) | The seven wrappers: the arguments named rather than numbered. |

The five `string/` units divide Section 7.24 as the standard divides it, rather
than by size or by taste, so that a reader looking for the whole of that section
finds it arranged as that section is — and can see at a glance which of it is
missing.

## What this directory is not

It is not where the system-call interface is defined. That is
[`../kernel/abi/`](../kernel/abi/), which is a separate include root under the
same permissive licence and which this library includes. The wrappers of sub-task
7.2 are declared and defined here, but the numbers they pass are the kernel's,
and `kernel/abi/` holds constants and a convention and never a symbol.
[`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 2, records why the
two are apart.

## Specifications implemented

- **ISO/IEC 9899:2011**, Section 7.24 (string handling), and Section 4,
  paragraph 6, which fixes what a freestanding implementation may assume — the
  four headers `<stdint.h>`, `<stddef.h>`, `<stdbool.h>` and `<float.h>` among
  them, and the reason nothing here needs a hosted library to compile.
- **ISO/IEC 9899:2011**, Section 7.5 (`<errno.h>`): the three macros the standard
  requires, that `errno` expands to a modifiable lvalue of thread local storage
  duration, and that no library function sets it to zero. Footnote 201 — the
  macro need not be the identifier of an object — is why `errno` here is a
  function call.
- **System V Application Binary Interface**, AMD64 supplement, Section 3.1.2:
  the LP64 model, in which `size_t` is 64 bits; and Section 3.2.3, the six
  integer argument registers, from which `syscall/invoke.asm` shifts by one.
- **Intel 64 and IA-32 Architectures Software Developer's Manual**, Volume 2B,
  "SYSCALL": the instruction destroys `RCX` and `R11`, which fixes the order of
  the moves in `syscall/invoke.asm`.

Each file cites the subsection of 7.24, or of 7.5, that defines each function it
holds. The corpus is enumerated in
[`../docs/project/REFERENCES.md`](../docs/project/REFERENCES.md).

## How it is asserted

[`../kernel/test/verify_string.c`](../kernel/test/verify_string.c) and
[`../kernel/test/verify_wrappers.c`](../kernel/test/verify_wrappers.c), both run
by `make verify` at every boot.
[`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 5, holds the table
pairing each of the first test's assertions with the silent failure it exists to
catch, and Section 5.1 records the negative test that found a defect in the test
itself; Section 8.4 holds the same table for the second, and Section 8.7 the
negative test that found *its* first version worthless.

**The second test is in two halves because the kernel cannot call what it
asserts.** `SYSCALL` executes at any privilege level, but the `SYSRET` that ends
the kernel's handling of it returns to privilege level 3 unconditionally, so the
invocation is asserted by copying the bytes of `syscall/invoke.asm` into a
program composed for the purpose and running them where they belong.
