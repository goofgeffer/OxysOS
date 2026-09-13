<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The C Library

**Phase**: 7, sub-tasks 7.1, 7.2, 7.3 and 7.4, of [`../project/PLAN.md`](../project/PLAN.md).

**Sub-task 7.1** is Sections 2 to 7. Section 2 is the division of the system-call
header, which is not part of 7.1 but was required to happen before 7.2 and is
done here because this is the change that first had a reason to touch both sides
of it. Section 3 is what the sub-task implements; Section 4 is what it
deliberately does not; Section 5 is the verification, and Section 5.1 is the
negative test that found a real gap in it. Section 7 is why a userland library is
presently compiled into the kernel.

**Sub-task 7.2** is Section 8: the system-call wrappers, the `errno` they set,
and `strerror`. Section 8.7 is the negative test that found the first version of
its assertion worthless, and is the reason that assertion is shaped as it is.

**Sub-task 7.3** is Section 9: the heap, the `brk` system call beneath it, and
the division between the two — Section 9.1 — that is what makes either of them
assertable. Section 9.4 holds the two tables of assertions and Section 9.7 the
fourteen negative tests, of which one found a limitation and one found code that
did nothing.

**Sub-task 7.4** is Section 10: the buffered stream, the three standard streams,
and the formatted conversion above them. Section 10.2 is the division — the same
one 7.3 made, applied a second time — and Section 10.2.1 is the counter that had
to be split before the self-test could assert anything at all. Section 10.5 is
the verification, including the sixty-six conversions checked against an
implementation this project did not write, and Section 10.8 the twenty negative
tests, of which four found gaps in the assertions and two found something no
assertion here can defend.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3, 4 and 6; and
[`../../LICENSING.md`](../../LICENSING.md), Section 2.1, which named the division
of Section 2 below as the first thing to be done about licensing and required it
before sub-task 7.2.

**Implementation**: [`../../libc/include/string.h`](../../libc/include/string.h)
and the five translation units beneath
[`../../libc/string/`](../../libc/string/) — `copying.c`, `comparison.c`,
`search.c`, `miscellaneous.c` and `error.c`, the first four divided as ISO/IEC
9899:2011 divides Section 7.24 itself. The interface header of Section 2 is
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h);
what was left behind is
[`../../kernel/include/oxys/arch/syscall/syscall.h`](../../kernel/include/oxys/arch/syscall/syscall.h),
whose design is [`PRIVILEGE.md`](PRIVILEGE.md). The wrappers of Section 8 are
[`../../libc/include/syscall.h`](../../libc/include/syscall.h),
[`../../libc/include/errno.h`](../../libc/include/errno.h),
[`../../libc/syscall/invoke.asm`](../../libc/syscall/invoke.asm),
[`../../libc/syscall/result.c`](../../libc/syscall/result.c) and
[`../../libc/syscall/calls.c`](../../libc/syscall/calls.c). The heap of Section 9
is [`../../libc/include/stdlib.h`](../../libc/include/stdlib.h),
[`../../libc/include/heap.h`](../../libc/include/heap.h),
[`../../libc/stdlib/heap.c`](../../libc/stdlib/heap.c) and
[`../../libc/stdlib/system.c`](../../libc/stdlib/system.c), with the kernel's
half in `SyscallDoBrk` and `ProcessSetBreak`. The assertions are
[`../../kernel/test/libc/string.c`](../../kernel/test/libc/string.c),
[`../../kernel/test/libc/wrappers.c`](../../kernel/test/libc/wrappers.c) and
[`../../kernel/test/libc/heap.c`](../../kernel/test/libc/heap.c), the last
two composing their programs with
[`../../kernel/test/program.c`](../../kernel/test/program.c).
The streams of Section 10 are
[`../../libc/include/stdio.h`](../../libc/include/stdio.h),
[`../../libc/include/stream.h`](../../libc/include/stream.h),
[`../../libc/stdio/internal.h`](../../libc/stdio/internal.h),
[`../../libc/stdio/stream.c`](../../libc/stdio/stream.c),
[`../../libc/stdio/format.c`](../../libc/stdio/format.c) and
[`../../libc/stdio/system.c`](../../libc/stdio/system.c), asserted by
[`../../kernel/test/libc/stdio.c`](../../kernel/test/libc/stdio.c) — which needs
no composed program, every stream it opens having a region of memory for a
device.

**Specifications**: ISO/IEC 9899:2011, Section 7.24 (string handling), Section
7.5 (`<errno.h>`), Section 7.21 (input and output), Section 7.16 (variable
arguments), Section 7.22.3 (the memory management functions), Section
6.2.8 (fundamental alignment) and Section 4, paragraph 6 (what a freestanding
implementation must provide); System V Application Binary Interface, AMD64
supplement, Section 3.1.2 (the LP64 model) and Section 3.2.3 (the argument
registers Section 2 departs from in one place); Intel 64 and IA-32 Architectures
Software Developer's Manual, Volume 2B, "SYSCALL".

## 1. What this sub-task is, and what it is not

Phase 6 ended with a program that can be loaded, entered at privilege level 3,
and made to call the kernel — written in machine code, by hand, twenty-nine bytes
of it. Phase 7 is the runtime that makes writing such a program a matter of
writing C. This sub-task is its floor.

**What it adds**: the nineteen functions of ISO/IEC 9899:2011, Section 7.24,
that this library implements; the header that declares them; and the assertion
that they behave as the standard says rather than as they appear to.

**What it does not add**: any way for a program to be built. There is no
`crt0`, no static-linking procedure, no archive and no user-mode compilation —
those are sub-task 7.5. The functions here are compiled for the kernel's own
image and run by the kernel's own self-test, which Section 7 explains at length
because it is the odd thing about this sub-task and not an incidental one.

**Nothing here calls the kernel.** These are the functions a program may use
before it has a heap, a descriptor, or an address space it did not start with,
which is exactly why they come first: every other sub-task of this phase depends
upon them and none of them depends upon anything.

## 2. The division of the system-call header

### 2.1 Why it had to happen before the wrappers

The kernel is `LGPL-3.0-or-later`; `libc/` and `userland/` are `MIT`. Those two
licences coexist without difficulty as long as nothing crosses between them, and
until this change one thing did: `kernel/include/oxys/arch/syscall/syscall.h` held both halves
of the system-call interface in one file.

It held **the interface a program is entitled to** — the call numbers, the
failure results, the register convention, the bound upon a path, the address at
which the kernel's half of the address space begins. And it held **the kernel's
implementation of that interface** — `IA32_STAR`, `IA32_LSTAR`, `IA32_FMASK`,
the flag mask, the saved register frame, the dispatch and the validation of a
caller's arguments.

A C library under a permissive licence cannot cleanly include such a file, and
the position [`../../LICENSING.md`](../../LICENSING.md), Section 2, takes — that
*calling* this kernel makes no program a derivative work of it — does not dispose
of the difficulty. That paragraph is about the calls. This is about the headers,
and it is the distinction Section 2.1 of that document was written to record.

### 2.2 What was done

The interface is now
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h),
under `MIT`, and holds exactly the constants above and the commentary that came
with them. The implementation stays in
[`../../kernel/include/oxys/arch/syscall/syscall.h`](../../kernel/include/oxys/arch/syscall/syscall.h),
under the kernel's licence, and includes the other.

**Nothing was changed in the move.** Every constant has the same name, the same
value and the same surrounding commentary; the kernel header includes the new one
so that every consumer of it sees what it saw before; and no source file outside
the two headers was edited at all.

`kernel/abi/` is a second include root rather than a subdirectory of
`kernel/include/`, and the reason is the one the difficulty was about. A library
that added `-Ikernel/include` to reach one permissive header would have every
header of the kernel in reach, and would be one `#include` away from the thing
this division exists to prevent. It adds `-Ikernel/abi` and can reach exactly
what it is entitled to.

### 2.3 Why the interface may hold no declaration

`kernel/abi/` holds constants and a stated convention and **nothing that has a
symbol**. A function declared there would be a function the kernel and every
program had to agree existed, which is an obligation neither of them asked for;
the whole value of an interface header is that it commits neither side to
anything but a number. The wrappers of sub-task 7.2 are declared by the C
library, in `libc/include/`, and are the library's.

## 3. What is implemented

The nineteen functions of Section 7.24 that do not require a locale or an
`errno`, in four translation units divided as the standard divides its own
subsections — so that a reader looking for the whole of 7.24 finds it arranged
as 7.24 is, and can see at a glance what is missing.

| Unit | Standard | Functions |
| ---- | -------- | --------- |
| [`copying.c`](../../libc/string/copying.c) | 7.24.2, 7.24.3 | `memcpy`, `memmove`, `strcpy`, `strncpy`, `strcat`, `strncat` |
| [`comparison.c`](../../libc/string/comparison.c) | 7.24.4 | `memcmp`, `strcmp`, `strncmp` |
| [`search.c`](../../libc/string/search.c) | 7.24.5 | `memchr`, `strchr`, `strcspn`, `strpbrk`, `strrchr`, `strspn`, `strstr`, `strtok` |
| [`miscellaneous.c`](../../libc/string/miscellaneous.c) | 7.24.6 | `memset`, `strlen` |
| [`error.c`](../../libc/string/error.c) | 7.24.6.2 | `strerror` — **added at sub-task 7.2**, not at 7.1, for the reason Section 4 gave when it was absent. It is listed here so that the whole of Section 7.24 can be seen in one table. |

### 3.1 The one property that decides whether they are correct

**Every byte is examined through `unsigned char`.** The standard requires it of
the comparing and searching functions; ISO/IEC 9899:2011, Section 6.5, paragraph
7, is why the copying functions do it too, `unsigned char` being the only type an
object's representation may be inspected as.

The requirement is easy to satisfy and easy to lose, and losing it is invisible.
An implementation that compared plain `char` would agree with a correct one for
every byte below 128 and disagree for every byte above it, because plain `char`
is signed upon x86_64 with this toolchain. Every string of letters would sort
correctly. The first UTF-8 sequence, hash, or binary buffer would sort backwards,
and nothing would fault. That is why the assertions of Section 5 are made upon
`0x80` and `0xFF` and not upon letters.

### 3.2 Three behaviours of the standard that look like defects

Each is implemented exactly as specified, and each is asserted, because an
implementation that "improved" upon it would be a function with a standard name
and non-standard behaviour — which is worse than either the standard function or
a differently named one.

- **`strncpy` pads and does not terminate.** A source shorter than `n` leaves the
  remainder of the destination filled with null bytes; a source of `n` bytes or
  more leaves no terminator at all. It is the wrong function for almost every use
  it is reached for, and Section 6, limitation 2, records what ought to stand
  beside it.
- **`strncat` terminates and does not pad**, which is the opposite of `strncpy`
  in both respects. `n` bounds the bytes taken from the source; the destination
  receives at most `n + 1`.
- **`strchr` and `strrchr` find the terminator.** `strchr(s, 0)` is a pointer to
  the end of `s`, the terminator being part of the string they search. A loop
  that tests for the terminator before comparing — which is the natural way to
  write one — returns null instead.

### 3.3 `strtok` keeps state, and that is the interface

`strtok` holds its position between calls in an object of static storage
duration, so it is the one function in this header that two threads may not call
at once and that a caller may not interleave two scans with. This is a property
of the interface the standard defines and not of this implementation: `strtok_r`
and `strtok_s` exist elsewhere precisely because of it. Neither is invented here
under a standard name. Section 6, limitation 3.

## 4. What is not implemented, and why

**Two** functions of Section 7.24 are absent — three were, when this section was
written at sub-task 7.1, and the third is now Section 8.5. Neither of the
remaining two is absent by oversight, and the header says so where a reader will
meet it.

| Function | Standard | Why not |
| -------- | -------- | ------- |
| `strcoll` | 7.24.4.3 | Compares according to the current locale. There is no locale in this system and no `<locale.h>` to establish one, so it would be `strcmp` under another name — an agreement with the standard that this library could not yet keep. It arrives with the locale. |
| `strxfrm` | 7.24.4.5 | The same, in the other direction: a transformation defined by a locale that does not exist. |
| ~~`strerror`~~ | 7.24.6.2 | **Implemented at sub-task 7.2**, which is what this row said would happen: the integers it maps are the failure results of `<oxys/syscall_abi.h>` as the wrappers present them through `errno`, and the table belongs beside the thing that sets `errno`. Section 8.5. |

**No non-standard function has been added either.** `strnlen`, `strdup`,
`strlcpy` and `memccpy` are each useful and each is somebody else's standard, not
ISO C's; `strdup` in any case allocates, and there is no allocator before
sub-task 7.3. Section 6, limitation 2, records the one of them that is genuinely
wanted and what deciding about it depends upon.

## 5. Verification

`KernelVerifyString`, in
[`../../kernel/test/libc/string.c`](../../kernel/test/libc/string.c).

**Every function here has a correct implementation and several plausible wrong
ones**, and the wrong ones give right answers for the inputs anybody tests with.
Three failures recur across the whole group, and the table below is organised
around them: the signed byte, the byte just past the end, and the empty case.

Every destination is a region inside a buffer filled with the sentinel `0x5A`,
and the margin either side of it is asserted to still hold the sentinel
afterwards. The sentinel is neither `0x00` nor `0xFF` because both are values
these functions legitimately write — a terminator and a `memset` fill — and a
sentinel a correct function may produce is not a sentinel.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| **`memcmp`, `strcmp` and `strncmp` report `0x80` greater than `0x01`** | The comparison performed through plain `char`. Correct for every ASCII string in the system and wrong for every byte above 127. Asserted three times because three functions can fall into it independently. |
| **`memchr` finds `0x80`; `strlen` does not stop at `0xFF`** | The same defect in the searching functions, where it shows as a byte that cannot be found and a length that is short — so every copy made from that length silently truncates. |
| `memcmp` of zero bytes reports equality; `strncmp` of zero bytes reports equality; `memchr` of zero bytes finds nothing | A loop written as `do`/`while`, which reads one byte before testing the count. It gives the right answer for every positive length. |
| **The margin either side of every destination still holds the sentinel** | A loop bounded by `<=` where it should be `<`. In a test whose buffers are adjacent zeroes, a stray zero written past the end is invisible. |
| `memcpy` and `memset` of zero bytes write nothing at all | The same, at the one length where it is certain. |
| **`memmove` over an overlap, in both directions, against eight distinguishable bytes** | A copy made in the wrong direction, which smears the first byte across the whole overlap. It produces a plausible-looking buffer, not a fault. |
| `memmove` of an object onto itself leaves it unchanged | The degenerate case the direction test is written around. |
| **`strncpy` pads to its bound, and leaves no terminator when the source fills it** | An implementation that terminated "helpfully". It would pass every other assertion in the file and would truncate one byte of every maximal copy in the system, for ever, under a standard name. |
| `strncat` terminates, and takes at most `n` bytes from its source | The assumption that the two bounded functions bound the same thing. |
| `strcat` appends at the terminator, and appending an empty string changes nothing | An append that starts at the beginning of the destination. |
| **`strchr` and `strrchr` disagree upon a subject holding `x` twice** | A `strrchr` that returns the first occurrence, which is `strchr` under another name and passes every assertion whose subject holds one occurrence. |
| **`strchr(s, 0)` and `strrchr(s, 0)` return a pointer to the terminator** | The loop that tests for the terminator before comparing. |
| `strspn` against an empty set is 0; `strcspn` against an empty set is the whole length | A set-membership test that treats the set's own terminator as a member. It makes one of the two functions return zero always, and the other is what says which. |
| **`strstr` of an empty needle returns the haystack — including when the haystack is empty** | See Section 5.1. The second half of that assertion is the only one of the two that can fail. |
| `strstr` finds `"aab"` in `"aaab"` | A search that restarts at the byte where the comparison failed rather than at the byte after the one it began at. |
| `strtok` skips leading separators, collapses runs of them, and yields no token for a trailing one | The expectation that `strtok` splits fields. It does not, and a caller who believes otherwise gets a token count that is right for the strings they tried. |
| **A finished `strtok` scan stays finished** | A position left pointing at the subject's terminator. It gives the same answer by accident, and a wrong one the moment the caller begins a new scan of a different string. |

### 5.1 The negative tests, and the one that found something

`PROJECT_GUIDELINES.md`, Section 2, and the practice of
[`../project/TESTING-SYSTEM.md`](../project/TESTING-SYSTEM.md) require each
assertion to be confirmed by a defect deliberately inserted. Five were inserted
and removed. Four behaved as intended:

| Defect inserted | What the run said |
| --------------- | ----------------- |
| `memcmp` comparing through plain `char` | `memcmp compared bytes as signed rather than unsigned FAILED.` and `memcmp is not antisymmetric upon a high byte FAILED.` |
| `memcpy` bounded by `<=` | `memcpy wrote outside the range it was given FAILED.` and `memcpy of zero bytes wrote something FAILED.` |
| `strncpy` made to terminate what it fills | `strncpy terminated a destination it filled FAILED.` and `strncpy wrote beyond its bound FAILED.` |
| `memmove` copying forwards in both directions | `memmove upwards over an overlap smeared a byte FAILED.` |
| `strrchr` keeping the first match instead of the last | `strrchr did not find the last occurrence FAILED.` |

**The fifth found a gap, and it is the reason this section exists.** The guard at
the head of `strstr` — which returns the haystack when the needle is empty — was
deleted, and **every assertion still passed**.

The guard had been written with a comment claiming it was what made an empty
needle work at all, and that was wrong. The search loop already produces the
right answer for an empty needle against a haystack that is not empty: the inner
comparison never runs, the needle's terminator is reached at offset zero, and the
first position matches. The guard is load-bearing in exactly one case — **an
empty needle in an empty haystack**, where the outer loop performs no iteration
and the function falls out to null — and the self-test had asserted the empty
needle only against a subject that was not empty.

So the test asserted a property that could not fail, beside a line of code whose
comment described a job it was not doing. The assertion now covers the empty
haystack, and deleting the guard fails it:
`strstr of an empty needle in an empty haystack did not return the haystack FAILED.`
The comment in `search.c` was corrected in the same change.

This is the ordinary yield of the negative-test discipline and is recorded at
this length because the defect it found was in the *test*, which is the class of
defect a passing run cannot report.

### 5.2 The second compiler

`make clang-check` compiles every unit here as it does the rest of the tree, and
it had one thing to say: `memset(region, 0xFF, 0)` in the self-test reads, to
clang's `-Wmemset-transposed-args`, as a caller who meant `memset(region, 0,
0xFF)`. That is a fair diagnostic and the commoner mistake by far. The remedy is
the one clang documents — parenthesise the length, which states that the zero is
deliberate — and not a suppression; no warning is disabled for this sub-task,
in the Makefile or anywhere else.

## 6. Limitations

1. **Every function is a byte-at-a-time loop.** No word-at-a-time copy, no
   alignment handling, no table-driven set membership, and a `strstr` that is the
   product of the two lengths in the worst case. This is deliberate and is the
   right trade today: correctness is asserted, nothing in the system calls these
   functions in a loop that matters, and there is no workload to measure an
   improvement against. **A ported compiler is the workload that will change
   that**, and it is named in [`../project/PLAN.md`](../project/PLAN.md), Section
   B, as the thing Phase 7's library must be sized for. The measurement should
   come before the optimisation.
2. **There is no bounded copy that is also safe.** `strncpy` is the standard's,
   and Section 3.2 says why it is not repaired. What is wanted beside it is a
   function that truncates and always terminates — `strlcpy` is one spelling of
   it, `strcpy_s` another, and neither is ISO C's. Which to adopt is a decision
   about what a ported compiler and a ported toolchain expect to link against.
   Sub-task 7.2 has now been written and **did not settle it**: nothing in the
   wrappers copies a string at all, the one bounded copy in the system being the
   kernel's own `SyscallCopyUserString`. The decision therefore moves to the
   first sub-task that links a program — 7.5 — or to the port itself, and this
   limitation stands as written.
3. **`strtok` is not re-entrant and cannot be made so.** Section 3.3. A
   `strtok_r` belongs beside it and is not added in this sub-task for the same
   reason as limitation 2.
4. **Nothing here is asserted upon a machine other than QEMU**, and nothing here
   has ever run at privilege level 3. Both follow from Section 7: the only
   executor is the kernel's boot-time self-test. The first run of this code in a
   user program is sub-task 7.5, and it is a genuine second verification rather
   than a formality — the compilation flags differ.
5. **There is no `<string.h>` guarantee that these are the only definitions.**
   The kernel is compiled without `libc/include` in reach precisely so that it
   cannot come to depend upon them, but nothing mechanically prevents a future
   Makefile edit from adding the root. The isolation is a rule expressed in one
   place — the C library's own compilation rule — and named in this document so
   that it is a rule somebody can find.

## 7. How this is built, and why a userland library is in the kernel image

**`make verify` is the only thing in this project that can execute anything.**
There is no test harness and there will be none until the userland this phase
builds can host one, which is the same statement
[`../../kernel/test/README.md`](../../kernel/test/README.md) has made since
Phase 2 — and this sub-task is the first one for which the subject of the test is
not the kernel.

These functions are freestanding in the strict sense: they call nothing, allocate
nothing, and depend upon nothing but the C language. Anything that can execute C
can run them. So the four translation units are compiled into the kernel image
and `KernelVerifyString` calls them, and that is the whole of the arrangement.

Three things make it honest rather than expedient:

1. **The kernel does not call them.** No kernel translation unit is compiled
   against `libc/include`. The exceptions are the self-tests that assert this
   library — `kernel/test/libc/string.c` at sub-task 7.1,
   `kernel/test/libc/wrappers.c` at 7.2 and `kernel/test/libc/heap.c` at 7.3
   — each named by an explicit rule in the `Makefile`, so the exception is three
   named lines rather than a rule about a directory, and a kernel source that
   tried to include `<string.h>` would fail to compile rather than quietly
   acquiring a dependency upon the userland. **A pattern over `kernel/test/`
   would have been shorter and is deliberately not written**: it would put every
   future self-test in reach of the userland's headers whether or not it
   asserted the userland.
2. **The image already carries code that is there to be asserted.** Every file
   in `kernel/test/` is in the image for the same reason, and has been since
   Phase 2.
3. **The licences permit it.** `MIT` code may be combined into a work distributed
   under `LGPL-3.0-or-later` provided its notice is retained, which the
   per-file `SPDX` tags do. [`../../LICENSING.md`](../../LICENSING.md), Section
   1, records the combination explicitly, because the rule that decides a
   directory there is *what is linked* — and this is the one case where a
   permissively licensed file is linked into the kernel image without being the
   kernel's.

**Sub-task 7.5 is where this stops being the only path.** The same sources are
then compiled a second time, with the flags a user program requires — which are
not the kernel's: `-mcmodel=kernel` places every symbol in the topmost two
gibibytes of the address space, and a program does not live there. Until then,
`LIBC_SOURCES` in the `Makefile` is the list that second compilation will name,
which is why it is a list of its own rather than merged into `C_SOURCES`.

---

## 8. Sub-task 7.2: the system-call wrappers

**Phase**: 7, sub-task 7.2, of [`../project/PLAN.md`](../project/PLAN.md).

**What it adds**: a function for each of the seven calls
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h)
numbers; the `SYSCALL` instruction beneath them; the `errno` of ISO/IEC
9899:2011, Section 7.5, which is the only thing in this library that any of them
writes; and `strerror`, which Section 4 said would arrive with them and has.

**What it does not add**: any way for a program to be built, still. There is no
`crt0`, no archive and no user-mode compilation — those remain sub-task 7.5. The
wrappers are compiled for the kernel's image exactly as the string functions are,
and Section 7 governs them unchanged.

### 8.1 The instruction is a translation unit of assembly

Everything else here is C above four routines in
[`../../libc/syscall/invoke.asm`](../../libc/syscall/invoke.asm), one for each
number of arguments a call of this kernel takes — none, one, two and three. Each
moves the caller's arguments one register to the left, executes `SYSCALL`, and
returns what the kernel put in `RAX`.

The shift is the whole of what they do, and it exists because two conventions
disagree by one place. The System V ABI, Section 3.2.3, passes a C function's
first four integer arguments in `RDI`, `RSI`, `RDX` and `RCX`; the kernel reads a
call's number from `RAX` and its arguments from `RDI`, `RSI` and `RDX`. So
`OxysSyscallInvoke3(number, a, b, c)` arrives with the number in `RDI` and must
leave with it in `RAX`, and every argument moves down one. The order of the moves
is not free: `RCX` must be consumed before `SYSCALL` executes, the instruction
putting the return address there (Intel SDM, Volume 2B).

**Why assembly rather than inline assembly**, when the kernel uses inline
assembly in twenty-one translation units. Two reasons, and the second is the
larger.

1. `PROJECT_GUIDELINES.md`, Section 8, prohibits a GCC-specific extension that
   has not been justified. A NASM translation unit is not an extension of the C
   language at all, and Section 3 of the guidelines already names NASM as this
   project's language for architecture-specific routines. A system-call
   invocation is one.
2. **It must contain no relocation**, and inline assembly cannot promise that.
   These four routines hold no memory operand, no relative displacement and no
   absolute address, so the bytes the assembler emits mean the same thing at
   every address. That is what allows the self-test of Section 8.4 to copy them
   out of the kernel image into a program's own address space and execute them at
   privilege level 3 — asserting the code this library ships rather than a
   reconstruction of it. A compiler given the same instructions would have been
   free to add a prologue, a frame and a reference to a kernel address, and the
   copy would then have been impossible.

**There is no invocation for four, five or six arguments.** The convention
reserves `R10`, `R8` and `R9` for them, no call of this kernel uses one, and a
wrapper nothing calls is a wrapper nothing asserts. They arrive with the first
call that needs them, by which time there will be something to assert them
against. `R10` in particular deserves an assertion when it appears and cannot
have one now: it is the one register where this kernel's convention departs from
the C one, which makes it the one most likely to be got wrong.

### 8.2 The translation, and the `errno` it sets

A call returns a length, a count, an identifier — or a negative failure result.
[`../../libc/syscall/result.c`](../../libc/syscall/result.c) turns that into the
convention a C program expects: `-1` and an `errno`, or the value unchanged.

Three properties, and each is asserted:

- **A result that is not negative is returned exactly, and `errno` is not
  touched.** Section 7.5, paragraph 3, would permit a library to set `errno` upon
  a call that succeeded. This one promises not to, and `<syscall.h>` says so,
  because a program that clears `errno`, calls a wrapper that succeeds and then
  finds `errno` set has been told of a failure that did not happen.
- **A negative result within the reserved range becomes its own name**, by
  negation. There is no table: [`../../libc/include/errno.h`](../../libc/include/errno.h)
  defines each number *as* the negation of the kernel's result, and asserts the
  correspondence with `_Static_assert`. A table would be a second list that must
  agree with a first, and the way that fails is that somebody adds a failure
  result and forgets the other half — whereupon a program reports the wrong cause
  and nothing faults. Renumber a result in the kernel's interface now and this
  library fails to compile.
- **A negative result outside that range becomes `ENOSYS`.** Nothing this kernel
  returns is outside it, so this branch guards against a kernel that has changed
  and a library that has not. It must not simply negate: an arbitrary value would
  put a number into a program's `errno` that names nothing, and `INT64_MIN` has
  no positive counterpart at all — negating it is the undefined behaviour
  `PROJECT_GUIDELINES.md`, Section 8, forbids. `ENOSYS` is the answer because it
  is the one failure that says exactly what is known in that case: the system did
  not perform the call, and the library cannot say why.

**The numbers are reserved in two ranges and the reservation is load-bearing.**
One to thirty-one belong to the derivation from the kernel's results; `EDOM`,
`EILSEQ` and `ERANGE` — which Section 7.5, paragraph 2, requires to exist — stand
at thirty-two and above. Had `EDOM` been eight, the eighth failure result this
kernel ever acquires would have arrived in a program's `errno` wearing the name
of a mathematical domain error.

**`errno` is a function call and not a variable**, which Section 7.5, footnote
201, exists to permit: "the macro `errno` need not be the identifier of an
object. It might expand to a modifiable lvalue resulting from a function call".
The standard requires `errno` to have thread local storage duration; this system
has no userland threads and the object is therefore one object. A library that
exported a plain `extern int errno` would have published the wrong thing —
callers would resolve the object at link time, and giving each thread its own
would become a change to the interface rather than to the implementation. As
written, that change touches this one file. Section 8.6, limitation 1.

### 8.3 The seven wrappers

[`../../libc/syscall/calls.c`](../../libc/syscall/calls.c). Each is a cast of its
arguments, an invocation and the translation above.

| Wrapper | Call | What it returns |
| ------- | ---- | --------------- |
| `OxysWrite(descriptor, buffer, length)` | `write` | Bytes written, which may be fewer than asked: the kernel bounds a single transfer. `EBADF` for a descriptor other than 1 or 2 — there are no files yet — and `EFAULT` for a range the program may not read. |
| `OxysTicks()` | `ticks` | The interval timer's count. It cannot fail. |
| `OxysVersion(buffer, capacity)` | `version` | Bytes copied, excluding the terminator the kernel always writes. `EINVAL` for a capacity of zero. |
| `OxysFork()` | `fork` | The child's identifier to the parent and zero to the child; `ENOMEM` where the frames, tables or slot could not be had. |
| `OxysExecve(path, argv, envp)` | `execve` | Does not return upon success. `EINVAL` unless both vectors are null; `ENOENT` for a file that is not there or will not load. |
| `OxysExit(status)` | `exit` | Does not return, and is declared `_Noreturn`. |
| `OxysWait(status)` | `wait` | The identifier of a child collected; `ECHILD` where there is none, `EFAULT` where the status has nowhere to go. |

**The names are this project's and not POSIX's**, and that is a decision rather
than an omission. Five of the seven have a POSIX name that means very nearly this
and none of the five means exactly it: this `execve` refuses an argument vector,
this `wait` takes no process identifier and no options, this `write` reaches two
diagnostic descriptors and no file. A function bearing a standard name and
behaving otherwise is worse than either the standard function or a differently
named one — which is the judgement Section 4 already records about `strlcpy` and
`strdup`, applied in the other direction. The POSIX spellings arrive when the
semantics do; meanwhile these comply with `PROJECT_GUIDELINES.md`, Section 4,
under which a global function is `PascalCase`.

**Two places a reader would guess wrong**, and both are commented where they
stand:

- **`OxysExecve` passes its vectors on rather than dropping them.** The refusal
  is the kernel's to make and not the library's to conceal; a wrapper that
  quietly passed null in their place would turn a refusal into a program running
  with no arguments and no way to discover why.
- **`OxysExit` ends in an infinite loop that is never executed.** The call does
  not return, so the loop is unreachable — but a `_Noreturn` function a compiler
  can see falling off its end is a diagnostic in both compilers this project uses,
  and the loop is what makes the declaration true by construction rather than by
  the kernel keeping a promise no compiler can check.

### 8.4 Verification

`KernelVerifyWrappers`, in
[`../../kernel/test/libc/wrappers.c`](../../kernel/test/libc/wrappers.c). It
is in two halves because the subject is.

**`SYSCALL` cannot be executed by this kernel.** The instruction itself works at
any privilege level, but the `SYSRET` that ends the kernel's handling of it
returns to privilege level 3 unconditionally — so a kernel that called `OxysWrite`
would enter its own entry path and leave it as a user program, upon a stack and
in an address space that are not a user program's. There is no arrangement in
which it survives. `PRIVILEGE.md`, Section 9.4, records the same thing from the
other side: since sub-task 6.7 the only executor of `SYSCALL` in this system is a
program.

So the translation, which is on this side of the instruction, is asserted by
calling it; and the invocation, which is not, is asserted by **copying the bytes
this library ships into a program composed for the purpose** and running them.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| **A successful result leaves `errno` as it was** — asserted at zero, at a length, and at `INT64_MAX` | A translation that set `errno` unconditionally. Every program that checks `errno` after a call that worked is then told of a failure that did not happen, and the program is right to believe it. |
| `INT64_MAX` is returned unchanged | A sign test performed by casting to a narrower type. It agrees with a correct one for every result this kernel actually returns. |
| **Each of the seven failure results names itself**, from a previous `errno` that no result maps to | A translation that returns `-1` and leaves `errno` alone, which is the commonest way to write this wrong; and a derivation that is off for one entry, which a single assertion would not distinguish from one that is right. |
| **A negative result beyond the reserved range becomes `ENOSYS`** | A translation that simply negates. It would put 32 into `errno` here — which is `EDOM`, so a program would be told that a system call had reported a mathematical domain error. |
| **`INT64_MIN` becomes `ENOSYS`** | The same, at the one value where negating is undefined behaviour. The commonest outcome on this architecture is the value negated to itself, cast to `int` as zero — and `errno` set to zero is the one thing Section 7.5 says a library function never does. The assertion that no translation leaves `errno` at zero is what catches it. |
| **Every number `<errno.h>` defines has a message, and no two of them share one** | A table with an entry omitted. Every number still answers something and every one of them answers wrongly; a check that each message was non-empty would pass. Distinctness is what does not. |
| `strerror` answers for `-1`, for an unassigned number, and for `INT32_MAX` and `INT32_MIN` | A table-driven implementation walking off its own ends. Section 7.24.6.2 requires *any* value of type `int` to be mapped. |
| `strerror(errno)` after a failed translation describes that failure | A library whose `strerror` and whose `errno` disagree. Every assertion above would still pass. |
| **The library's invocation block fits where the program expects it, and the composed driver does not run into it** | `invoke.asm` growing. The block would then be executed as whatever the driver's last bytes happened to be. |
| **A program at privilege level 3 makes exactly seven calls** | An invocation whose call displacement was wrong. It would land in the middle of another routine — which within this block is still a valid instruction sequence and still returns — so the count of calls that reached the dispatcher is what distinguishes that from seven correct ones. |
| **The sum of what every call returned is exactly what the kernel computes it must be** | Any argument lost or misplaced by the invocation. See Section 8.7, which is where this assertion came from. |
| **The tick count the program read lies between what this processor observed either side of the run** | An invocation that returned something other than the kernel's result. The count is carried in the low digits of the status, beneath a scale no boot reaches, so that its unavoidable imprecision cannot absorb an error in the exact sum above it. |
| The program ended, its process is marked ended, and the boot continued | The whole path. A program that faulted instead ends with a negative vector and fails the sum. |

The program itself is the system's name fetched by one call and written by
another, so the log carries a line no part of the kernel composed:

```
  A program at privilege level 3 reports, through the library's own invocation: Oxys-OS unreleased
```

### 8.5 `strerror`

[`../../libc/string/error.c`](../../libc/string/error.c). Section 4 said this
function belonged beside the thing that sets `errno`, and this is that change.

Two properties are worth stating because the standard's wording invites the
opposite of each. **It maps any value of type `int`** — 7.24.6.2, paragraph 2 —
so an unrecognised number is a case to answer and not a case to refuse. And
**the messages are arrays rather than string literals**, because this project
compiles with `-Wwrite-strings`, under which a literal has type `const char[]`
and returning one from a function declared `char *` is a diagnostic. The choice
was between casting the qualifier away at every return — a lie told eleven times
— and giving each message an array of its own, which is what the standard's own
wording contemplates: it speaks of "the array pointed to" and of a program that
must not modify it.

### 8.6 Limitations

1. **`errno` is one object and the standard requires one per thread.** Section
   7.5, paragraph 2, is explicit about thread local storage duration. There are
   no userland threads to give one to and no thread-local storage block for a
   `_Thread_local` object to live in. The function form is what makes this a
   change to one file when threads arrive; until then this library is conforming
   only for a program with one thread, which is every program there is.
2. **The typed wrappers are not asserted, only the layers above and below
   them.** `OxysSyscallResult` is asserted by calling it and `invoke.asm` by
   running its own bytes at privilege level 3 — but `OxysWrite` passing its
   `length` where the kernel reads a length is checked by nothing. It cannot be
   until a program is linked against this library, because the wrappers are
   compiled `-mcmodel=kernel` and hold a reference to `errno` at a kernel
   address, which is exactly the property that makes the invocation copyable and
   them not. **Sub-task 7.5 closes this**, and it is the largest single thing
   outstanding about this sub-task.
3. **There is no invocation of four, five or six arguments**, and therefore no
   assertion upon `R10` — the one register where this kernel's convention departs
   from the C one. Section 8.1.
4. **Nothing here has run in a user program**, in the sense of having been linked
   into one: the bytes of `invoke.asm` have executed at privilege level 3, but as
   a block copied by a self-test, driven by a hand-assembled caller. The first
   genuine link is sub-task 7.5, and limitation 4 of Section 6 applies to this
   sub-task word for word.
5. **`errno` is never set by anything but a system call.** That is true today and
   is a property of what exists rather than a decision: there is no allocator, no
   formatted conversion and no mathematical library to set it. The three numbers
   ISO C requires are defined and nothing writes them.

### 8.7 The negative tests, and the one that found something

`PROJECT_GUIDELINES.md`, Section 2, and the practice of
[`../project/TESTING-SYSTEM.md`](../project/TESTING-SYSTEM.md) require each
assertion to be confirmed by a defect deliberately inserted. Seven were inserted
and removed.

| Defect inserted | What the run said |
| --------------- | ----------------- |
| `OxysSyscallResult` negating without the range check | `a failure beyond the reserved range was not refused FAILED.`, `the least representable result was not refused FAILED.` and `a translation left errno at zero FAILED.` — the third being the `INT64_MIN` case arriving exactly as Section 8.2 predicts it would. |
| `OxysSyscallResult` setting `errno` upon success | `a successful call altered errno FAILED.` and the two assertions beside it. |
| `strerror`'s table with one entry removed | `strerror returned an empty message FAILED.` |
| `invoke.asm`'s two-argument routine losing its shift | `the program's calls did not return what they had to return FAILED.` |
| `invoke.asm`'s three-argument routine losing its shift | The same. |
| `invoke.asm`'s no-argument routine not placing the number in `RAX` | The same, and `the tick count the program read follows the run FAILED.` |
| A failure result renumbered in `<oxys/syscall_abi.h>` | `error: static assertion failed: "EBADF does not name SYSCALL_EBADF."` — a compile-time failure, which is the class of report about a two-sided agreement that cannot be missed. |

**The fourth and fifth of those passed the first time they were tried, and that
is why this section exists.**

The assertion, as first written, was that the program end with a status composed
of the result of one call that had to fail. `invoke.asm`'s three-argument routine
was then altered to drop `mov rdx, rcx` — losing the third argument of every
three-argument call, which is precisely the defect the whole copy-the-bytes
arrangement exists to catch — and **every assertion passed**.

It passed for a reason worth recording, because the reason is general. The lost
argument was a *length*, and the kernel bounds a length rather than refusing an
implausible one: a length of 0x402000 became 4096, the range was readable because
the program's data page is a whole page, and the write emitted the same string it
would have emitted anyway. The only trace was a newline that did not appear in
the log — and nothing was asserting the log.

So the status is now the **sum of what every call returned**, and every call's
result is in it. The sum is exact and the kernel computes it from the same
version string the kernel's own call copies; the tick count, which cannot be
exact, is carried beneath a scale of a million so that its imprecision cannot
absorb an error in the sum. A second `version` call was added with a capacity
*smaller* than the string, because a capacity larger than the string is not a
capacity the result depends upon — which is how the two-argument defect had
passed as well.

**Two of the seven defects were invisible to the first version of this test, and
both were defects in the one file the test was written for.** That is the ordinary
yield of this discipline, and it is recorded at length for the same reason
Section 5.1 is: a passing run cannot report a test that does not test.

---

## 9. Sub-task 7.3: the heap, and the break beneath it

**Phase**: 7, sub-task 7.3, of [`../project/PLAN.md`](../project/PLAN.md).

**What it adds**: the four memory management functions of ISO/IEC 9899:2011,
Section 7.22.3 — `malloc`, `calloc`, `realloc` and `free` — above an allocator of
this project's own; the `brk` system call by which a program asks this kernel for
memory, which is the eighth call the kernel implements and the first one added
since Phase 6; and the two wrappers that reach it.

**What it does not add**: any way for a program to be built, still. There is no
`crt0`, no archive and no user-mode compilation; those remain sub-task 7.5, and
Section 7 governs how this is compiled and asserted exactly as it governs 7.1 and
7.2. Nothing in this system allocates yet — the heap exists for the things that
will, beginning with the formatted output of sub-task 7.4.

**Implementation**: [`../../libc/include/stdlib.h`](../../libc/include/stdlib.h),
[`../../libc/include/heap.h`](../../libc/include/heap.h),
[`../../libc/stdlib/heap.c`](../../libc/stdlib/heap.c) and
[`../../libc/stdlib/system.c`](../../libc/stdlib/system.c) on the library's side;
`SYSCALL_BRK` in
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h),
`SyscallDoBrk` in [`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c) and
`ProcessSetBreak` in [`../../kernel/proc/process.c`](../../kernel/proc/process.c)
on the kernel's. The assertion is
[`../../kernel/test/libc/heap.c`](../../kernel/test/libc/heap.c), which
composes its program with
[`../../kernel/test/program.c`](../../kernel/test/program.c).

**Specifications**: ISO/IEC 9899:2011, Section 7.22.3 (the memory management
functions), Section 6.2.8 (fundamental alignment), Section 6.5, paragraph 6
(the effective type of storage with no declared type), Section 6.5.8, paragraph
5 (where relational comparison of pointers is defined) and Section 6.3.2.3,
paragraph 5 (the conversion between a pointer and an integer).

### 9.1 The division, and why the sub-task has one

A heap is two things that fail differently.

**The policy** — first fit, splitting, coalescing, the arithmetic of `realloc` —
is ordinary C. It calls nothing that can fail for a reason outside the C
language, it runs anywhere C runs, and it is wrong in ways an assertion can see:
a block one alignment too small, a join that only ever works in one direction, a
`realloc` that copies when it needed only to grow.

**The source** — where the memory comes from — is a system call, and **this
kernel cannot execute one**. Section 8.4 records the obstacle in full: `SYSCALL`
executes at any privilege level, but the `SYSRET` that ends the kernel's handling
of it returns to privilege level 3 unconditionally, so a kernel that called
`OxysSbrk` would leave its own entry path as a user program upon a stack that is
not a user program's.

So the two are named apart. [`../../libc/include/heap.h`](../../libc/include/heap.h)
declares `OxysHeapExtend`, which is the whole of what the allocator knows about
the machine beneath it, and `OxysHeapAdopt`, by which a caller gives the heap a
region it obtained itself. The policy is asserted by giving it a region and
exercising it; the system call is asserted from the other side, by a program at
privilege level 3. What joins them is
[`../../libc/stdlib/system.c`](../../libc/stdlib/system.c), which is six lines.

**`OxysHeapAdopt` is not a test hook.** A program with a statically reserved
arena, or one running before a break exists, has the same need and no other way
to meet it. The self-test is merely its first caller, and the design would carry
the function whether or not there were a test.

### 9.2 The kernel's half: `brk`

A program's **break** is the address one past the last byte of the region it may
use for a heap. `SYSCALL_BRK` moves it and returns where it stands afterwards;
`SYSCALL_BREAK_QUERY`, which is zero, reports it without moving it — and is how
a program discovers its own heap, the address being derived from the image the
loader placed and a program having no view of its own program headers.

**Where the heap begins.** Immediately above the program's image, rounded up to a
page and then advanced by one more. That extra page is a guard and is not
decoration: the image's highest address is the end of a program's `.bss`, so a
program that walks off the end of its last static array would otherwise walk into
the first byte of its own heap, where it would find memory that is mapped,
writable and holding an allocator's bookkeeping. One unmapped page turns that
into a fault at the instruction that caused it.

**Three departures from the traditional call**, each answering a way the
traditional one is misused.

1. **It returns the new break, and a failure is negative.** Kernels of this
   lineage return the break as it stands whether or not the request succeeded,
   which obliges every caller to compare the result against what it asked for and
   to make a second call to discover what it has. The comparison is easy to omit,
   the result being a plausible address either way — and a caller that omits it
   believes it owns memory that was never mapped. Every other call of this kernel
   reports a failure as a negative result and this one does too.
2. **A request below where the heap begins is refused rather than clamped.** A
   program that asked for such a break computed an address wrongly, and a kernel
   that moved the request to the nearest legal value would leave it believing the
   arithmetic was sound.
3. **A growth that cannot be completed is undone.** `brk` maps a frame for every
   page the region gains, and a machine may run out part way. A break left
   between the two is a heap whose first pages are mapped and whose last are not,
   which the program discovers at whichever byte it happens to touch first — far
   from the request that failed. The pages mapped by a failed attempt are
   therefore withdrawn again and the break is left where it was.

**What it maps.** A zeroed, writable, user-accessible frame per page, eagerly.
Zeroed for the reason the user stack is zeroed: a frame arrives holding whatever
its last owner left in it, and an allocator is the one caller most likely to hand
the bytes straight on without writing them first. Eagerly because there is no
demand paging in this kernel; Section 9.6, limitation 3.

**What bounds it.** `PROCESS_BREAK_MAXIMUM`, sixteen mebibytes. The bound is upon
what one process may ask for and not upon what the machine has, and it exists
because the mapping is eager: a program asking for its whole address space would
otherwise consume every frame in the machine before the request was refused, and
the refusal would arrive with nothing left to report it with.

**Shrinking withdraws the mapping and releases the frame.** That required a
primitive this kernel did not have — `AddressSpaceUnmapPage`, above
`PagingUnmapPageIn` — which is the first operation here that takes a mapping away
from a live address space. It returns the frame rather than releasing it,
because whether the caller holds the last reference to it is something only the
caller knows: a frame shared by a copy-on-write clone has more than one referrer,
and a function that decided for its caller would free a frame another address
space is still translating through. The intermediate paging structures are left
standing, a heap that shrank being a heap that will grow.

**`fork` and `execve`.** A child inherits both bounds of its parent's break, the
pages between them having been cloned like any other. Copying only where the heap
begins and letting the child start with an empty one would leave those pages
mapped and unaccounted: the child would grow its break over memory it already
had, and the growth would map a fresh frame over a page the program was still
using. `execve` places the break anew from the image that replaced the old one,
for the converse reason — a break carried across names an address derived from a
program that no longer exists, and the new image being smaller, that address may
lie within the new program's own `.bss`.

### 9.3 The library's half: the allocator

The heap is a set of blocks. Every block carries a thirty-two byte header giving
its whole size and a mark saying whether it is free; the free ones are linked
into a single list ordered by address. A request is met by the first block large
enough, split where the remainder would itself be a block. A release marks the
block free, inserts it at its place in the order, and joins it to whichever of
its two neighbours lies against it. When nothing fits, the heap asks
`OxysHeapExtend` for a region and makes a block of it.

**First fit over an address-ordered list**, and not one of the three obvious
alternatives:

- **Not best fit.** It costs a walk of the whole list instead of a walk to the
  first fit, and has been held since Knuth to fragment no less for the trouble:
  the remainder it leaves is by construction the smallest possible, which is to
  say the least likely ever to be usable again.
- **Not a size-ordered list.** It makes a fit cheap and coalescing dear: joining
  a released block to its neighbour requires knowing which block lies against it
  *in memory*, which a list ordered by size cannot answer without boundary tags
  beneath every block or a walk of the whole list.
- **Not segregated free lists by size class.** That is what a heap under real
  load wants, and this heap is under no load at all — nothing in this system
  allocates yet. Choosing it now would mean writing several hundred lines against
  an allocation profile that has never been measured, which is the judgement
  Section 6, limitation 1, records about the string functions. The workload that
  will justify measuring is a ported compiler, and this is one translation unit
  to replace behind an interface four functions wide.

The address ordering is what makes the release cheap where it matters: the walk
that finds a released block's place in the list is the same walk that finds both
of its neighbours, so coalescing costs nothing beyond the insertion it was going
to perform anyway.

**The order of the two joins is load-bearing.** The successor is absorbed first,
so that a predecessor which also abuts absorbs a block that has already grown and
three adjacent free blocks become one. The other order leaves the middle block
merged backward and the successor stranded — a heap that fragments under exactly
the pattern a heap meets most, a run of allocations released in the order they
were made.

**Four decisions about what the standard leaves open**:

- **`malloc(0)` returns a distinct pointer, not a null one.** Section 7.22.3,
  paragraph 1, makes the choice implementation-defined. The alternative is
  unusable: a null pointer returned for a zero-sized request cannot be told apart
  from a failure, and every correct program checks for null. Two such requests
  return different pointers, which that paragraph does require — every allocation
  must yield a pointer disjoint from any other object.
- **A failure sets `errno`.** ISO C does not require it of `malloc` and Section
  7.5, paragraph 3, permits it. It is done because every other way this library
  reports a failure sets `errno`, and a caller should not have to know which
  functions are the exception. `ENOMEM` for a request that cannot be met;
  `EINVAL` where `realloc` is given something that is not an allocation, the
  machine not having run out of anything.
- **`free` of something that is not an allocation is defined as a refusal.**
  Section 7.22.3.3, paragraph 2, makes it undefined behaviour, and this library
  defines it: the header is examined for the mark the allocator wrote, and one
  that does not carry it is left alone and counted. The marks are eight-byte
  words and not a flag bit — `OXYSFREE` and `OXYSLIVE` in ASCII — because every
  value of a flag byte is a valid answer and a header written over by an overrun
  would say whatever the overrun left. That is what makes a double release a
  refusal instead of a block upon the free list twice, after which two later
  requests are met with the same memory and the failure appears in whichever of
  the two callers writes second.
- **The header is thirty-two bytes and does not overlay the payload.** The usual
  practice is to carry the free-list link in the first bytes of the payload,
  which halves the overhead. It is not done here because it makes the payload of
  an allocated block and the link of a free one the same storage read through two
  types, and `PROJECT_GUIDELINES.md`, Section 8, binds this project not to rely
  upon behaviour it cannot point at a paragraph for. The cost is limitation 1
  below rather than a silence.

**Every comparison of two block addresses is performed upon `uintptr_t`.** ISO/IEC
9899:2011, Section 6.5.8, paragraph 5, defines the relational operators only for
pointers into the same array object, and the blocks of a heap are by construction
not in one array — they lie in regions the system supplied at unrelated times.
`first < second` upon two block pointers is therefore exactly the undefined
behaviour Section 8 of the guidelines forbids, however obviously it works. The
conversion to `uintptr_t` is *implementation-defined* by Section 6.3.2.3,
paragraph 5, which is a different thing, and is defined by this implementation as
the address.

### 9.4 Verification

`KernelVerifyHeap`, in
[`../../kernel/test/libc/heap.c`](../../kernel/test/libc/heap.c), in two
halves because the subject is.

#### 9.4.1 The policy, asserted by calling it

The allocator is given a sixty-four kibibyte arena by `OxysHeapAdopt` and
exercised against it. What runs is the code the library ships, not a
reconstruction of it.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| A null region, and a region too small for one block, are refused and are not counted | An adopted region that becomes a block smaller than its own header, after which every walk of the free list reads past the end of the region. |
| An adopted region becomes exactly one free block of exactly its own size | A region whose head or tail is silently lost, which no later assertion about *changes* to the heap would notice. |
| **Two requests of zero bytes return different pointers**, and both are not null | An allocator answering every zero-sized request with one shared address, which satisfies every other assertion here and violates 7.22.3, paragraph 1. |
| Every pointer returned is a multiple of sixteen | An allocator whose header is not a whole number of alignments. Every payload after the first is then misaligned, and on this architecture nothing faults — the failure is a `long double` that is merely slow, until something is compiled that assumes otherwise. |
| **Three allocations keep three distinct patterns** written into them | A split that left the second block overlapping the tail of the first. The addresses differ and the storage does not, so comparing addresses would pass. |
| **Everything released leaves the heap as it began** — one block, one free block, the same available bytes, the same largest request | Any defect in the size arithmetic at all. A block one alignment too small or too large produces a heap that never returns to one block, however plausible each individual allocation looked. This is the only assertion here that a defect in splitting or joining cannot hide from. |
| The release order for that assertion is not the allocation order | A coalescence that only ever works backward. Releasing in the order allocated gives every block a free predecessor and no free successor, so one join alone would pass. The middle block is released last, with a free neighbour upon each side. |
| `free(NULL)` is neither a release nor a refusal | An allocator reporting an event that did not happen — 7.22.3.3, paragraph 2, makes it no action. |
| A pointer that is not an allocation is refused | Storage the allocator does not own being linked into the free list and handed to the next caller. |
| **A block released twice is released once and refused once** | The defect the marks exist for, and the one whose symptom appears furthest from its cause. |
| **Growing where the next block is free does not move the allocation** | An allocator that is correct and quadratic: every program that reads something of unknown length then copies everything it has read at every step. Nothing but this assertion distinguishes the two. |
| Growing past an allocated neighbour moves the block **and carries its contents** | A move that allocates and frees and forgets the copy — 7.22.3.5, paragraph 2. |
| Shrinking does not move the block and gives the difference back | A `realloc` that answers every shrink with a fresh block and a copy. |
| `realloc` of something that is not an allocation returns null with `EINVAL` | A refusal reported as `ENOMEM`, which sends a caller looking for memory pressure that is not there. |
| **`calloc` clears a block that was dirtied and released**, not a fresh one | A `calloc` that never clears anything. Every page this kernel maps arrives zeroed, so it would pass upon a fresh heap and fail upon every heap a real program has. |
| **A count and size whose product wraps is refused** | The one security property here. A wrapped product gives a small block for a large request; the caller writes the elements it asked for and the write runs off the end of a block the allocator believes is smaller than it is. Nothing can detect it afterwards. |
| A request of `SIZE_MAX` is refused by the arithmetic and **without asking the system for anything** | An allocator that discovers an impossible size by trying to obtain it. |
| The census balances — headers plus handed out plus free is exactly what was adopted | Any of the three being maintained by a path that forgot to. |
| **The heap never asked the system for memory** | A change to this test that made it exhaust the arena. `OxysHeapExtend` executes `SYSCALL`, which this kernel cannot survive; the assertion is what states the constraint rather than the comment at the head of the file. |

#### 9.4.2 The break, asserted by a program at privilege level 3

A program is composed by hand — [`../../kernel/test/program.c`](../../kernel/test/program.c)
emits the instructions — and the C library's own invocation block is copied into
it, exactly as Section 8.4 arranges for the wrappers. It makes ten calls and ends
with a status that is the sum of what nine of them returned. The sum is exact and
the kernel computes what it must be from the same version string its own call
copies.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| The break is reported, and a loaded program has one | A `brk` that answered zero, which is an address no program can use and which every program would take for a heap at the bottom of its address space. |
| The heap begins a page above the image and upon a page boundary | A heap placed at the image's end, where an overrun of the program's last static object lands in the allocator's own bookkeeping. |
| **`version` into the break fails with `EFAULT` before the growth** | A `brk` that reported an address it had not mapped. The program would then be told it owns memory that faults at the first byte it touches. |
| **The growth returns exactly the address asked for** — the difference from where the break began is summed | A kernel returning the *old* break, which is what the traditional call does, and which is a plausible address the program cannot tell from success. |
| `version` into the page succeeds, which means the page is mapped and **writable at privilege level 3** | A page mapped without the user bit, or without the writable bit. Either faults for the program and neither is visible to a kernel that only reads its own record of what it mapped. |
| `write` reads the same bytes back, and they appear in the log | A page mapped writable and not readable — and, in passing, the one line in this boot log that came out of memory a program asked the kernel for. |
| **`version` into the break fails with `EFAULT` again after the shrink** | A shrink that moved a number and left the mapping. The program keeps memory it gave back, and the frames are lost until the process ends. |
| The break returns to where it began, asked twice — once as the shrink's result and once by a fresh query | A shrink that reported the address asked for without recording it. |
| Exactly ten calls reached the dispatcher | An invocation whose call displacement was wrong. It would land in the middle of another routine, which within this block is still a valid instruction sequence and still returns. |
| **The kernel recorded one growth, one shrink, and no page left mapped by either** | The same events from the kernel's side, independent of what the program reported. The last is the assertion that a shrink releases frames rather than merely forgetting about them. |

### 9.5 The composer, and why two tests share one

[`../../kernel/test/program.c`](../../kernel/test/program.c) is new at this
sub-task and is not part of it. Sub-task 7.2 assembled a program by hand and this
one needed a second; two copies of an instruction encoder is two places for a
byte to be wrong, and the second copy would have been wrong in a way the first
one's assertions could not see. The encoders are now one translation unit and
each self-test holds only the program it means to run.

It gained one thing neither copy had: **every write goes through a bounds check,
and a refusal is recorded rather than reported at the call site.** A composer
that ran off the end of its array would write into whatever the linker placed
after it, and the failure would appear in an unrelated subsystem long after the
self-test that caused it had reported success. Both tests now assert that nothing
was refused.

### 9.6 Limitations

1. **The header is thirty-two bytes per allocation**, where sixteen would do. A
   sixteen-byte request therefore costs forty-eight. Section 9.3 records the
   reason — the smaller header requires the payload of an allocated block and the
   free-list link of a released one to be the same storage read through two
   types. The workload that would justify revisiting it is the same one that
   would justify segregated free lists, and neither exists yet.
2. **`aligned_alloc` is not implemented.** Section 7.22.3.1 requires it to honour
   any alignment the implementation supports, and every extended alignment is
   stricter than the sixteen bytes every block already has. Honouring one means
   returning a pointer that is not at a fixed displacement from its own header,
   which is the invariant `free` finds a block by and the one thing everything
   else here depends upon. It arrives when something asks for it.
3. **`brk` maps eagerly and there is no demand paging.** A program that moves its
   break by a mebibyte gets a mebibyte of frames whether it touches them or not.
   This is why `PROCESS_BREAK_MAXIMUM` exists, and it is the reason a heap here
   costs what it asks for rather than what it uses.
4. **A region is never given back to the system.** The break is a single boundary
   and a heap may hold regions that are not the topmost, so `free` returns memory
   to the free list and never to the kernel. A program's heap therefore only
   grows. The usual answer is to release the topmost region when it becomes
   wholly free, and it is not written because nothing has yet allocated enough
   for it to matter.
5. **The undo path of a failed growth is not asserted.** The negative test of
   Section 9.7 removed it and every assertion passed: no test here exhausts the
   frame allocator, which is the only thing that makes a growth fail part way.
   Asserting it needs a way to make `FrameAllocate` fail on demand, which this
   kernel has not got.
6. **A heap page arriving unzeroed is not asserted either.** The kernel zeroes
   every page `brk` maps, and the composed program has no way to read a byte and
   compare it — it has no comparison instruction and no branch. What *is*
   asserted is the allocator's own clearing, which is the half a program can be
   harmed by twice over.
7. **There is no locking.** There are no userland threads to contend with — the
   same fact Section 8.6, limitation 1, records of `errno` — and a lock taken
   against nothing is a lock nothing asserts. Every entry point of the allocator
   needs one the day threads exist.
8. **The typed wrappers `OxysBrk` and `OxysSbrk` are not asserted**, and neither
   is `OxysHeapExtend`. They are compiled into this image and cannot be called
   from it, exactly as limitation 2 of Section 8.6 records of the seven wrappers
   before them. **Sub-task 7.5 closes this**, and it is the largest single thing
   outstanding about this sub-task: it is where the two halves above are joined
   and run together for the first time.
9. **The whole of `<stdlib.h>` but Section 7.22.3 is absent** — the string
   conversions, the pseudo-random sequence, the communication with the
   environment, the searching and sorting, the integer arithmetic and the
   multibyte conversions. Each arrives with the sub-task that needs it, and none
   of them is needed by a heap.

### 9.7 The negative tests, and the two that found something

`PROJECT_GUIDELINES.md`, Section 2, and the practice of
[`../project/TESTING-SYSTEM.md`](../project/TESTING-SYSTEM.md) require each
assertion to be confirmed by a defect deliberately inserted. Fourteen were
inserted and removed.

| Defect inserted | What the run said |
| --------------- | ----------------- |
| `calloc` not clearing the block | `a cleared allocation was not all bits zero FAILED.` |
| `calloc` not checking the product | `a count and size whose product wraps was met FAILED.`, and four more — the refused request having been met, the heap never returned to one block. |
| `free` not checking the mark | `a pointer that is not an allocation was not refused FAILED.`, `a pointer that is not an allocation was released FAILED.`, **and then the boot did not complete**: the free list had a region's foreign storage upon it and the next walk left the heap. |
| A shrink not withdrawing its pages | `the program's calls did not return what they had to return FAILED.` and `a page mapped by a growth survived the shrink that gave it back FAILED.` — the program's own view and the kernel's, disagreeing with the same defect. |
| `HeapInsert` without the forward join | `a heap with nothing allocated is not the heap it started as FAILED.` and three more. |
| `HeapSplit` leaving a remainder one alignment short | `releasing everything did not give back every byte FAILED.`, and the heap did not return to one block. |
| `HeapAbsorbNext` never finding the next block | `growing into a free neighbour moved the allocation FAILED.` |
| `brk` returning the old break rather than the new one | `the program's calls did not return what they had to return FAILED.` |
| The heap placed without its guard page | `the heap begins without a guard page below it FAILED.` |
| `HeapTake` not marking the block as handed out | Ten assertions, across four of the six phases. |
| `OxysHeapAdopt` not checking the usable size | `a region smaller than one block was adopted FAILED.`, `a refused region was counted FAILED.`, and the boot did not complete. |
| `SYSCALL_BRK` in the dispatch table and not in the switch | `the program's calls did not return what they had to return FAILED.` and the two counters — which is the case the `default` arm of that switch exists for, reported as `ENOSYS` rather than as the number the caller passed. |
| A failed growth not being undone | **Nothing was reported.** Limitation 5. |
| `OxysHeapAdopt` not checking `bytes` at its head | **Nothing was reported**, and the check was removed rather than kept. See below. |

**The thirteenth found a limitation and the fourteenth found redundant code.**

The fourteenth is the one worth recording. `OxysHeapAdopt` checked twice that a
region was large enough: once against `bytes` at its head, and once against the
usable size after the head and tail below an alignment had been taken off.
Deleting the first changed nothing any assertion could see — and it could not,
because the second rejects every region the first does and more besides: a region
of a hundred bytes beginning sixty bytes before an alignment has forty usable
ones, which the first check passes and the second does not. The first was
therefore not a guard but a statement of the same intent in a place where it is
not yet knowable, and it was deleted. **The check that survived is the one asked
after the adjustment**, and the comment there now says why.

The thirteenth is limitation 5 above, and is the ordinary yield of this
discipline: an assertion that does not exist cannot be made to fail, and the only
way to find out which ones those are is to try.

---

## 10. Sub-task 7.4: the buffered stream, and the conversion above it

**Phase**: 7, sub-task 7.4, of [`../project/PLAN.md`](../project/PLAN.md).

**What it adds**: the `FILE` of ISO/IEC 9899:2011, Section 7.21, and the three
standard streams; the buffering that decides when a stream's bytes leave it and
when more arrive; the byte and block transfers of Sections 7.21.7 and 7.21.8; the
indicators of Section 7.21.10; and the formatted output of Section 7.21.6 — one
conversion engine beneath eight standard names.

**What it does not add**: any way for a program to be built, for the last time.
That is sub-task 7.5, which follows immediately and for a reason recorded in
Section 10.5.2: this is the first sub-task whose shipped code the kernel is
*unable* to assert even in part, and the program 7.5 builds is what closes it.

### 10.1 What of Section 7.21 is here, and what is not

The head of [`../../libc/include/stdio.h`](../../libc/include/stdio.h) states
each absence beside the thing it is waiting for, and this section does not
restate the list. The shape of it is worth stating: **everything absent is absent
because this kernel has eight system calls and not one of them opens, closes,
positions or reads a file.**

| Absent | Waiting for |
| ------ | ----------- |
| `fopen`, `freopen`, `fclose`, `remove`, `rename`, `tmpfile`, `tmpnam` | A system call that opens a file by name. Phase 8, the shell being the first thing that needs one. |
| `fseek`, `ftell`, `fgetpos`, `fsetpos`, `rewind`, `fpos_t`, `SEEK_*` | The same. Nothing that exists here is positionable. |
| The `scanf` family | A source of characters; see Section 10.3.4. |
| Every floating conversion, in both directions | Nothing. `PROJECT_GUIDELINES.md`, Section 8, prohibits floating-point arithmetic without a justification, and this library is compiled `-mno-sse` and `-mno-80387` besides. Section 10.4.2. |
| The wide-character conversions | A locale and a multibyte encoding, which this system has not got — the same reason `strcoll` and `strxfrm` are absent from `<string.h>`. |
| `%n` | Nothing. It is refused deliberately; Section 10.4.3. |

**A function that can only fail is worse than one that does not exist.** That is
the rule this table is an application of, and it is the same judgement Section 4
records about `strdup`: a `fopen` here would compile, link, and tell a program at
run time that the library had lied about what it offers.

### 10.2 The division, and why this sub-task has one too

The division is the one sub-task 7.3 made and
[`../../libc/include/heap.h`](../../libc/include/heap.h) argued for, applied a
second time, and [`../../libc/include/stream.h`](../../libc/include/stream.h) is
where it is stated.

**The policy** — when a buffer is emptied, what a partial transfer means, how a
pushback interacts with an end-of-file indicator, what a conversion specification
produces — is ordinary C. It runs anywhere and it is wrong in ways a test can
see.

**The transfers** are `OxysStreamWrite` and `OxysStreamFill`, and one of them is
a system call. This kernel cannot execute one: the `SYSRET` that ends its
handling of a `SYSCALL` returns to privilege level 3 unconditionally, so a kernel
that flushed a stream would leave its own entry path as a user program upon a
stack that is not a user program's. Section 8.4 records the same obstacle for the
wrappers and Section 9.1 for the break.

So the seam is a pair of named functions in a translation unit of its own —
[`../../libc/stdio/system.c`](../../libc/stdio/system.c), which is eleven lines
of code — and the policy above it is
[`../../libc/stdio/stream.c`](../../libc/stdio/stream.c) and
[`../../libc/stdio/format.c`](../../libc/stdio/format.c), which touch a device
only through it.

**The memory stream is what lets the policy be exercised.** `OxysStreamOpenMemoryWrite`
and `OxysStreamOpenMemoryRead` give a stream a region of memory for a device, so
the kernel's own self-test runs the whole of the buffering and the whole of the
conversion against **the code this library ships** rather than a reconstruction
of it. Neither is a test hook, for the reason `OxysHeapAdopt` is not one: a
program composing a string by the formatted conversion has the same need and no
other way to meet it — it is what a hosted implementation spells `fmemopen` — and
the self-test is merely its first caller.

#### 10.2.1 The two seams are counted apart, and the first version did not

`OxysStreamCensus` counts calls to `OxysStreamWrite` and calls to
`OxysStreamFill` in two fields. The first version of that structure had one field
for both, named `transfers`, and the self-test asserted it was zero — which is
the assertion that keeps the test from resetting the machine.

**It failed on the first run, and the failure was the assertion's and not the
code's.** Only one of the two seams executes `SYSCALL`. `OxysStreamWrite` does;
`OxysStreamFill` does not, there being no call that reads for it to make, so it
is ordinary C that returns zero. The self-test reads the standard input — because
asserting that `stdin` reports an *end* rather than an *error* is worth doing and
is safe — and the single counter therefore recorded a transfer that had reached
nothing. The choice was to loosen the assertion or to split the counter, and a
loosened assertion would have stopped reporting the thing it exists for.

### 10.3 The stream

#### 10.3.1 What each of the three standard streams is, and why

| Stream | Descriptor | Buffering | Why |
| ------ | ---------- | --------- | --- |
| `stdin` | 0 | Fully buffered | Nothing reads it. Its policy is real and asserted; its source reports end-of-file. |
| `stdout` | 1 | **Line** buffered | Section 7.21.3, paragraph 7, permits full buffering only where the stream does not refer to an interactive device, and the thing at the far end here is a person reading a console. A fully buffered `stdout` loses the last partial line whenever a program faults, and the last partial line before a fault is the one worth having. |
| `stderr` | 2 | **Un**buffered | Paragraph 7 requires it not to be fully buffered. A diagnostic still in a buffer when the program dies is a diagnostic that was not issued. |

They are initialised statically and not by anything called before `main`. This
library has no constructor mechanism and sub-task 7.5's startup object
deliberately does not acquire one for this: a stream that is correct because the
loader zeroed `.bss` and the initialisers filled in the rest is a stream that
works in a program whose first statement is `puts`.

#### 10.3.2 `FILE` is an incomplete type

Section 7.21.1, paragraph 2, requires it to be an object type, and every
operation in Section 7.21 takes a `FILE *`. Leaving it incomplete is what makes
"a program never depends upon the members" a guarantee rather than an
expectation, and the consequence — that a program cannot declare a `FILE` of its
own — is one no conforming program minds.

#### 10.3.3 The three decisions the buffering makes

1. **A buffer is emptied when it is already full, not after the byte that fills
   it.** Written the other way round the buffer is emptied one byte late, and the
   difference is invisible until a caller gives a stream a buffer of one byte —
   whereupon the append writes past its end. `setvbuf` accepts a size of one, so
   that is a reachable state and not a hypothetical one. Section 10.8, negative
   test 2, is the record of this being got wrong deliberately and of the
   assertion that could not see it.
2. **A short transfer is the system's normal behaviour and not a failure**, so
   the loop that calls again from where the last one stopped belongs to the
   policy and not to the seam. The kernel bounds a single write; a library that
   treated the first short result as the whole answer would truncate every
   message longer than that bound, silently, because the count `fwrite` returned
   would be the truncated one and almost nothing checks it.
3. **A stream at its end does not ask its source again.** The end-of-file
   indicator is consulted before the source is. A stream that asked again would
   make one system call per call after the end — for a program looping upon
   `fgetc`, one per iteration for ever — and nothing about the characters it
   delivered would differ. Negative test 17 is the record of that, and of the
   assertion it caused to be written.

#### 10.3.4 Why there is an input side at all

The sub-task is buffered input *and* output, and **the buffering is the part
worth getting right**: the pushback, the two indicators that stick until they are
cleared, a partial read that is not an error, a byte read back after being pushed
back, a partial line at end-of-file that must not be discarded. All of that is
policy above a source, exactly as the heap is policy above a source, and it is
asserted the same way — against a memory stream, which supplies characters
without a system call.

What is absent is only the shipped source. `OxysStreamFill` reports end-of-file,
and `stdin` is therefore a stream permanently at it. **It reports an end and not
an error**, and the distinction is the whole of why it is written as a function
that returns zero rather than as a function that does not exist: an error would
make every program reading `stdin` report a fault that did not occur. The day
this kernel acquires a call that reads, the change is the body of one function of
six lines and nothing else in the library.

### 10.4 The conversion

#### 10.4.1 One engine, eight names

Every function in [`../../libc/stdio/format.c`](../../libc/stdio/format.c) is the
same engine with a different destination, which is one function pointer and one
context. The alternative — a conversion loop for streams and a second one for
arrays — is how a library comes to format `%#o` correctly in `printf` and
incorrectly in `snprintf`, and the defect is invisible because nobody tests both.

**The engine counts what it produced and not what was stored.** That is
`snprintf`'s return value under Section 7.21.6.5, paragraph 2, and it is also
`fprintf`'s, a stream storing everything it is given or failing. Counting stored
characters would make `snprintf` return the truncated length, and a caller sizing
an array by calling with a size of zero would be told it needs nothing.

What is implemented is the flags `-`, `+`, space, `#` and `0` of paragraph 6; the
field width and precision of paragraphs 4 and 5, each as a digit string or as an
asterisk; the length modifiers `hh`, `h`, `l`, `ll`, `z`, `j` and `t` of
paragraph 7; and the conversions `d`, `i`, `o`, `u`, `x`, `X`, `c`, `s`, `p` and
`%%` of paragraph 8.

Two details are worth stating because they are the ones a hand-written formatter
gets wrong:

- **The three paddings are three different things and their order is fixed.** The
  precision pads with zeroes inside the sign and the prefix; the `0` flag pads
  with zeroes inside them too, but only to the field width, only when the result
  is not left-justified and only when no precision was given; and the field pads
  with spaces outside everything. A formatter that conflates the second and the
  third prints `0-042` for `%05d` of −42.
- **A narrow length modifier is a conversion back from `int` and not a different
  `va_arg` type.** The default argument promotions have already widened a `char`
  or a `short`, so `hh` reads an `int` and converts; a formatter that reads
  `va_arg(arguments, char)` has undefined behaviour and happens to work.

#### 10.4.2 No floating-point conversion, and none possible

`PROJECT_GUIDELINES.md`, Section 8, prohibits floating-point arithmetic that has
not been justified, and this library is compiled `-mno-sse`, `-mno-sse2`,
`-mno-mmx` and `-mno-80387`. A conversion that formed a `double` would not
assemble. The eight floating conversions are therefore refused where they are
recognised.

#### 10.4.3 `%n` is refused deliberately

It is the one conversion that writes through a pointer taken from the argument
list under the direction of the format string, which is the mechanism by which a
format string a program did not compose becomes a write to an address somebody
else chose. Section 7.21.6.1, paragraph 8, requires it and this library refuses
it.

What that costs is that a conforming program using `%n` does not work here. What
it buys is that a program which passes a string it received to `printf` cannot be
made to write memory by it — and a program that passes a received string to
`printf` is a defect this library cannot prevent and can decline to arm.

### 10.5 Verification

#### 10.5.1 What the kernel asserts

[`../../kernel/test/libc/stdio.c`](../../kernel/test/libc/stdio.c), run as
`KernelVerifyStdio` from `KernelMain`. Every stream it opens has a region of
memory for a device, so the whole of it runs inside this kernel.

| Property asserted | The silent failure it exists to catch |
| ----------------- | ------------------------------------- |
| A fully buffered stream delivers nothing before it is flushed, and a newline changes nothing | Line buffering applied where full buffering was asked for: every write reaches the device, which is correct output and the wrong cost. |
| A line buffered stream delivers upon the newline and not before it | The newline case forgotten: a program's diagnostics arrive in blocks and the last partial line is lost at a fault. |
| An unbuffered stream delivers each byte as it is written | `stderr` buffered: a diagnostic still in a buffer when the program dies. |
| A buffer of one byte delivers each byte as the next arrives, **and the byte beyond the buffer is untouched** | The buffer emptied after the append rather than before it, which writes one byte past a caller's array. The bytes delivered are the same bytes in the same order; the sentinel is the only trace. |
| `setvbuf` is refused after the stream has been used, and for a mode that is not one of the three | A buffer replaced while it holds data, which discards that data with no report. |
| `fwrite` returns whole elements, and an element whose bytes went out only in part is not one | Section 7.21.8.2, paragraph 2, got wrong in the direction that tells a caller its data went out. |
| `fwrite` of a zero size or a zero count writes nothing | A loop written without the guard, which writes one element. |
| A stream counts as delivered only what its device took | A partial delivery reported as a whole one, which makes `OxysStreamDelivered` — the thing a caller measures a region by — read past what was written. |
| The error indicator is sticky and only `clearerr` clears it | A program that checks once after a sequence of writes told everything was well because the last write happened to succeed. |
| `fgetc` delivers every byte in order, then end-of-file, and sets the indicator | An off-by-one at either end of the buffer. |
| A stream at its end does not ask its source again | One system call per call after the end, invisible in the characters delivered. |
| `ungetc` accepts one pushback, refuses a second, delivers it next, clears end-of-file, and refuses `EOF` | Section 7.21.7.10, paragraph 2, forgotten: a caller that pushes a character back after reading `EOF` cannot read it. |
| `fgets` keeps the newline, terminates, stops at the bound, and returns a partial line at end-of-file | The last line of every source that does not end with a newline, discarded. |
| `fgets` with a count of one terminates the array and writes nothing past it | The bound a loop written with `<=` writes past — and the array returned, no end having been met. |
| `fread` returns whole elements and sets end-of-file | The mirror of the `fwrite` case. |
| A source of no bytes is a stream immediately at its end | The case every loop over a stream gets wrong first. |
| Twenty-one conversions produce exactly the expected characters **and report exactly their length** | A formatter that produces the right characters and returns the wrong count, which is wrong in exactly the way `snprintf`'s measuring idiom depends upon. |
| `snprintf` truncates within the size given, terminates, and returns what it would have needed | A caller sizing an array by a call with a size of zero, told it needs nothing. |
| An unimplemented conversion is refused and reported | Section 10.4.3. |
| The three standard streams have descriptors 0, 1 and 2, may not be closed, and `stdin` reports an end rather than an error | A library that let `stdout` be closed would have to answer what `printf` does afterwards. |
| The pool is back to three streams, and **no byte reached the system** | The second is what keeps this test from resetting the machine, and it is an assertion rather than a comment. |

**The conversions were also checked against a second implementation.** The two
translation units were compiled by the host's compiler, against the host's
`snprintf`, and sixty-six conversion specifications were formatted by both and
compared byte for byte. Sixty-five agreed exactly, including the truncation and
the size-of-zero cases; the one that differed is `%p` of a null pointer, where
this library prints `0x0` deliberately and the reference prints `(nil)`. That is
the same kind of corroboration the `mke2fs` comparison of
[`../project/TESTING.md`](../project/TESTING.md) is, and it is worth as much: it
is the only judge in this project that does not share this project's
understanding of the standard.

#### 10.5.2 What the kernel cannot assert, and what closes it

`OxysStreamWrite` is a system call and is not asserted here. It is asserted by
the program sub-task 7.5 builds and runs at privilege level 3, whose output
arrives upon the serial channel by way of `printf` — so the gap between the two
halves of this sub-task is **one sub-task wide and not one phase wide**, which is
why 7.5 follows immediately rather than 7.6.

### 10.6 Why an unimplemented conversion is a reported failure

Section 7.21.6.1, paragraph 9, makes an undefined conversion specification
undefined behaviour, so any answer conforms. There are two available answers.

The first is to produce something and carry on, which is what a hosted
implementation does with a conversion it does not know. The count returned is
then wrong, and it is wrong in a way nothing checks — the return value of
`printf` being the least-examined result in C.

The second is to refuse and say so, which is what this library does. A caller
that checks the result is told; a caller that does not is no worse off than
under the first answer. And for `%n` in particular the caller is entitled to
know: it asked for a write, and the write did not happen.

### 10.7 Limitations

1. **Every transfer above the buffer is byte-at-a-time.** `fread`, `fwrite`,
   `fgets` and `fputs` are each a loop over the single-character operation rather
   than a block copy into the buffer. It is the same judgement Section 6,
   limitation 1, records about the string functions: a block copy is faster and
   has four more ways to be wrong — the buffer boundary, the pushback, the
   line-buffering decision and the partial transfer — and there is no workload
   here to measure the difference against. A ported compiler is that workload.
2. **Nothing here is asserted upon a machine other than QEMU**, and — until
   sub-task 7.5 — nothing here has run at privilege level 3. Section 6,
   limitation 4, unchanged.
3. **The magnitude of the most negative representable value cannot be asserted
   wrong.** `FormatMagnitude` avoids forming `-value`, which is undefined
   behaviour for `INTMAX_MIN` and is the single input every hand-written
   formatter gets wrong. Negative test 9 replaced it with the unsafe form and
   **nothing was reported**: upon this architecture the negation produces the
   same bits, so the output is identical. The guard stays, because the standard
   promises nothing and a different compiler or optimisation level need not
   agree; but no assertion in this project can defend it.
4. **The error indicator's stickiness is asserted only across further failures.**
   A write stream here fails by exhausting its region, and a region once
   exhausted admits no successful write — so "the indicator survives a later
   *successful* write" has no reachable case. Negative test 19 set the indicator
   false at the head of `fputc` and nothing was reported. It becomes reachable
   when a device exists that can refuse one transfer and accept the next, which
   is a real file.
5. **A stream has no lock, and every one of them is shared state.** There are no
   userland threads, so nothing is wrong today. The day a program has two, every
   stream needs one and the census needs atomic access; the paragraph at the head
   of `stream.c` is where that change begins.
6. **The buffers are static and cost eight kibibytes of a program's `.bss`.**
   `FOPEN_MAX` entries of `BUFSIZ` each, whether or not a stream is ever opened.
   The alternative is to obtain a buffer from the heap, which cannot be done: a
   stream must be usable before a heap has been grown, and the first thing a
   program does with a heap that failed is try to report it.

### 10.8 The negative tests, and the four that found something

Twenty defects were introduced deliberately, one at a time, each built and run.

| The defect introduced | What was reported |
| --------------------- | ----------------- |
| The line-buffering flush upon a newline removed | `a line buffered stream did not deliver upon the newline FAILED.` |
| The buffer emptied *after* the byte that fills it | **Nothing was reported.** See below. |
| A memory stream that fills not reporting the bytes it could not take | Five assertions, across three phases of the test. |
| `fwrite` counting an element whose bytes went out only in part | `fwrite counted an element whose bytes went out only in part FAILED.` |
| `ungetc` not clearing the end-of-file indicator | `ungetc did not clear the end-of-file indicator FAILED.` |
| `ungetc` accepting a second pushback | That, and `the character pushed back was not the next one read FAILED.` |
| `fgets` discarding the newline it must keep | `fgets did not stop after the newline, or discarded it FAILED.` |
| `fgets` returning a null pointer whenever nothing was read | The two assertions upon a count of one — which is the case that condition is about. |
| `FormatMagnitude` forming `-value` | **Nothing was reported.** Limitation 3. |
| A zero value with a precision of zero still producing a digit | `a precision was applied wrongly to an integer FAILED.` |
| The `0` flag not ignored where a precision was given | `the zero flag was not ignored where a precision was given FAILED.` |
| The alternative form prefixing a zero hexadecimal value | `the hexadecimal conversion or its prefix is wrong FAILED.` |
| `snprintf` reporting what it stored | Both of the assertions that exist for that. |
| `setvbuf` accepting a call made after the stream had been used | `setvbuf accepted a call made after the stream had been used FAILED.` |
| A standard stream that may be closed | Six assertions, including the census's. |
| An unimplemented conversion produced rather than refused | All three refusal assertions. |
| The end-of-file indicator consulted after the source rather than before | **Nothing was reported.** See below. |
| The sign flags allowed through an unsigned conversion | **Nothing was reported.** See below. |
| The error indicator cleared by a later successful write | **Nothing was reported.** Limitation 4. |
| A partial delivery counted as a whole one | **Nothing was reported.** See below. |

**Four of the five silent ones were gaps in the assertions and were closed.**

- **The buffer emptied after the append** delivers the same bytes in the same
  order and overruns the caller's array by one. The one-byte-buffer assertion was
  upon what arrived at the device and could not see it. The buffer given to
  `setvbuf` is now an array of two bytes with a sentinel in the second, and the
  assertion is upon the sentinel.
- **The end-of-file indicator consulted late** changes nothing a caller can
  observe except how often the source is asked. The test now records the census
  before and after three reads past the end and asserts the source was not asked.
- **The sign flags let through an unsigned conversion** was not covered at all:
  every assertion upon `+` and space used a signed conversion. There is now one
  upon `%+u`, `% u` and `%+x`.
- **A partial delivery counted as a whole one** leaves `OxysStreamDelivered`
  reporting more than the region holds, which is what a caller measures a region
  by. There is now an assertion upon that count after a delivery the device
  refused.

The fifth and the ninth are limitations 4 and 3, and are the ordinary yield of
this discipline: an assertion that does not exist cannot be made to fail, and the
only way to find out which ones those are is to try.
