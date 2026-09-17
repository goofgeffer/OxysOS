<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `libc/` — The C Library

**Phase**: 7 of [`../docs/project/PLAN.md`](../docs/project/PLAN.md). Sub-task
7.1 placed the first material here, sub-task 7.2 the system-call wrappers,
sub-task 7.3 the heap and sub-task 7.4 the buffered streams and the formatted
conversion, and sub-task 7.5 the runtime startup object, the termination
functions and the link a program is built by. Sub-task 7.6 added the six
filesystem wrappers and the thirteen failure names they report, and is where
`errno` stopped being seven numbers.
**Detailed design**: [`../docs/design/LIBC.md`](../docs/design/LIBC.md), Sections
2 to 7 for sub-task 7.1, Section 8 for sub-task 7.2, Section 9 for sub-task 7.3,
Section 10 for sub-task 7.4, Section 11 for sub-task 7.5 and Section 12 for
sub-task 7.6.

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
| [`include/syscall.h`](include/syscall.h) | The thirty-five system-call wrappers — the six of sub-task 9.2 the window calls, `../docs/design/LIBC.md`, Section 14 —, `OxysSbrk` beside them, the raw invocation beneath them, and the translation of a kernel result into a result and an `errno`. |
| [`syscall/invoke.asm`](syscall/invoke.asm) | The `SYSCALL` instruction itself, one routine per number of arguments; since 8.7 the signal restorer as well, outside the range the self-test copies. It holds no relocation, which is what lets the self-test copy its bytes into a program and run them at privilege level 3. |
| [`syscall/result.c`](syscall/result.c) | The translation, and the `errno` object — the only thing in this library that writes `errno`. |
| [`syscall/calls.c`](syscall/calls.c) | The thirty-five wrappers: the arguments named rather than numbered. `OxysSbrk` is built here from `OxysBrk` rather than given a call of its own, the kernel having no need to know that a caller thinks in increments. |
| [`include/stdlib.h`](include/stdlib.h) | ISO/IEC 9899:2011, Section 7.22.3: `malloc`, `calloc`, `realloc` and `free` — and, at its head, which of `<stdlib.h>` is absent, why `aligned_alloc` is among it, and why these four keep their standard names when every other global function here is `PascalCase`. |
| [`include/heap.h`](include/heap.h) | The seam: `OxysHeapExtend`, which is the whole of what the allocator knows about the machine beneath it, `OxysHeapAdopt`, by which a caller gives the heap a region it obtained itself, and the census a caller may take. |
| [`stdlib/heap.c`](stdlib/heap.c) | The allocator: first fit over an address-ordered free list of boundary-marked blocks, splitting and joining. The policy, and nothing about where memory comes from. |
| [`stdlib/system.c`](stdlib/system.c) | Where memory comes from: `OxysHeapExtend`, six lines, above `OxysSbrk`. It is a translation unit of its own so that the policy may be asserted without it. |
| [`include/stdio.h`](include/stdio.h) | ISO/IEC 9899:2011, Section 7.21, as this library implements it — and, at its head, which of that section is absent and what each absence is waiting for. `FILE` is an incomplete type, so no program can depend upon what is inside one. |
| [`include/stream.h`](include/stream.h) | The seam: `OxysStreamWrite` and `OxysStreamFill`, which are the whole of what a stream knows about the machine beneath it; the memory stream, by which the buffering is exercised without one; and the census, whose two transfer counters are apart because only one of the seams executes `SYSCALL`. |
| [`stdio/stream.c`](stdio/stream.c) | The policy: the `FILE` object, the three standard streams, the decision of when a buffer is emptied or filled, and every transfer of Sections 7.21.7 and 7.21.8 above it. Nothing here knows where a byte goes. |
| [`stdio/format.c`](stdio/format.c) | The conversion: one engine that reads a format string and produces characters, and the eight standard names above it, which differ only in where the characters go. A conversion it does not implement is refused and the refusal is reported. |
| [`stdio/internal.h`](stdio/internal.h) | The two counters the conversion keeps in the census the stream owns. It is in `stdio/` and not `include/` so that the include root a program is compiled against does not carry it. |
| [`stdio/system.c`](stdio/system.c) | Where a stream's bytes go and, since sub-task 8.1, where they come from: eleven lines above `OxysWrite`, and a fill above `OxysRead` that was a function reporting end-of-file until there was a call that reads — the one function that changed when there was. A translation unit of its own so that the policy may be asserted without it. |
| [`crt/crt0.asm`](crt/crt0.asm) | The first instructions of every program this system runs: the argument count, the two vectors, the call of `main` and the `exit` that takes what it returned. Assembly because a C function cannot read its own stack pointer and because there is nowhere to return to. |
| [`stdlib/exit.c`](stdlib/exit.c) | ISO/IEC 9899:2011, Section 7.22.4: `atexit`, `exit`, `_Exit` and `abort`. Thirty-two registrations in `.bss`, called in reverse, and the streams flushed **after** them — which is the order the standard fixes and the reason it fixes it. |
| [`stdlib/environment.c`](stdlib/environment.c) | 7.22.4.6: `getenv`, since sub-task 8.4, and the environment vector `crt0` records for it to search — a pointer into the program's own initial stack, where the kernel laid the strings the shell exported. |
| [`user.ld`](user.ld) | The linker script every program is linked with: four mebibytes, three page-separated segments one per permission, and the discard list that keeps a fourth segment from appearing for a build identifier nothing reads. |
| [`include/line.h`](include/line.h) | The line editor of sub-task 8.1, which ISO/IEC 9899:2011 has no such thing as, and which is here for the reason `<syscall.h>` and `<heap.h>` are: it is this system's library. The editor, the table of what every byte does, the history of thirty-two lines, and the seam — an output function, so that the editing never names a descriptor. |
| [`line/line.c`](line/line.c) | The editing, the parsing of the terminal's control sequences (ECMA-48, Section 5.4) and the history, above an output function; since 2026-09-16 `LinePreset`, which makes the line a given text to be edited rather than typed, for the editor `micro`. It assumes one thing of a display: that a backspace moves the cursor left without erasing. |
| [`line/system.c`](line/system.c) | `LineRead`, and `LineEdit` which begins the line as a given text: the prompt written to descriptor 1, the bytes read from descriptor 0 one at a time, the editor's output carried to descriptor 1 between them. A translation unit of its own so that the editing may be asserted without it, and the first thing in this library that reads the standard input. |
| [`include/signal.h`](include/signal.h) | ISO/IEC 9899:2011, Section 7.14, and IEEE Std 1003.1-2017's `kill`: the signals, `SIG_DFL`, `SIG_IGN`, `SIG_ERR`, `signal`, `raise` and `kill`, of sub-task 8.7. Each number is asserted against the kernel's. |
| [`signal/signal.c`](signal/signal.c) | The three functions, and the one place the restorer — the routine a handler returns through, at the end of `syscall/invoke.asm` — is named. |

The five `string/` units divide Section 7.24 as the standard divides it, rather
than by size or by taste, so that a reader looking for the whole of that section
finds it arranged as that section is — and can see at a glance which of it is
missing.

The two `stdlib/` units divide the heap **by what can fail**: the policy calls
nothing that can fail for a reason outside the C language, and the source is a
system call. That division is the whole of why either half can be asserted at
all, and [`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 9.1, is the
argument for it.

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
- **ISO/IEC 9899:2011**, Section 7.22.3 (the memory management functions): the
  alignment every pointer returned satisfies, the unspecified order and
  contiguity of successive allocations, the implementation-defined answer to a
  request of zero bytes, and the requirement that each allocation be disjoint
  from every other object. With Section 6.2.8, paragraph 2, which fixes what a
  fundamental alignment is, and Section 6.5.8, paragraph 5, which is why every
  comparison of two block addresses in `stdlib/heap.c` is made upon `uintptr_t`.
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
- **ECMA-48**, 5th edition, Section 5.4: the grammar of a control sequence —
  CSI, parameter bytes, intermediate bytes, a final byte — which `line/line.c`
  parses whatever the final byte turns out to be; and Sections 8.3.18 to
  8.3.22, the four cursor movements the arrow keys send. **XTerm Control
  Sequences** (Dickey): the forms in which Home, End, Delete and the cursor
  keys arrive, every one of which is accepted because which form arrives is the
  terminal's choice and not the program's.

Each file cites the subsection of 7.24, or of 7.5, that defines each function it
holds. The corpus is enumerated in
[`../docs/project/REFERENCES.md`](../docs/project/REFERENCES.md).

## How it is asserted

[`../kernel/test/libc/string.c`](../kernel/test/libc/string.c),
[`../kernel/test/libc/wrappers.c`](../kernel/test/libc/wrappers.c) and
[`../kernel/test/libc/heap.c`](../kernel/test/libc/heap.c), all run by
`make verify` at every boot.
[`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 5, holds the table
pairing each of the first test's assertions with the silent failure it exists to
catch, and Section 5.1 records the negative test that found a defect in the test
itself; Section 8.4 holds the same table for the second, and Section 8.7 the
negative test that found *its* first version worthless; Section 9.4 holds the two
tables for the third, and Section 9.7 the fourteen negative tests of which one
found a limitation and one found code that did nothing.

**The second and third tests are each in two halves because the kernel cannot
call what they assert.** `SYSCALL` executes at any privilege level, but the
`SYSRET` that ends the kernel's handling of it returns to privilege level 3
unconditionally, so the invocation is asserted by copying the bytes of
`syscall/invoke.asm` into a program composed for the purpose and running them
where they belong. The third divides the same way for a reason of its own: its
subject is a policy that runs anywhere above a system call that does not, and
`include/heap.h` names the seam between them so that each may be asserted where
it can be.

**The line editor of sub-task 8.1 is divided the same way, and it is the first
thing here whose output is asserted.** `include/line.h` names the seam — an
output function — so [`../kernel/test/libc/line.c`](../kernel/test/libc/line.c)
drives the editing with the bytes a terminal sends and compares, byte for byte,
what it wrote; then places a session upon the kernel's terminal and runs
`userland/line-check`, which reads it through `LineRead` at privilege level 3.
[`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 12.7, limitation 1,
records why nothing else here can be asserted that way;
[`../docs/design/SHELL.md`](../docs/design/SHELL.md), Section 5, holds the
tables and the negative test that showed the point of it.
