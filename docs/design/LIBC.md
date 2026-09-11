<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The C Library

**Phase**: 7, sub-task 7.1, of [`../project/PLAN.md`](../project/PLAN.md).
Section 2 is the division of the system-call header, which is not part of 7.1
but was required to happen before 7.2 and is done here because this is the
change that first had a reason to touch both sides of it. Section 3 is what the
sub-task implements; Section 4 is what it deliberately does not; Section 5 is
the verification, and Section 5.1 is the negative test that found a real gap in
it. Section 7 is why a userland library is presently compiled into the kernel.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3, 4 and 6; and
[`../../LICENSING.md`](../../LICENSING.md), Section 2.1, which named the division
of Section 2 below as the first thing to be done about licensing and required it
before sub-task 7.2.

**Implementation**: [`../../libc/include/string.h`](../../libc/include/string.h)
and the four translation units beneath
[`../../libc/string/`](../../libc/string/) — `copying.c`, `comparison.c`,
`search.c` and `miscellaneous.c`, divided as ISO/IEC 9899:2011 divides Section
7.24 itself. The interface header of Section 2 is
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h);
what was left behind is
[`../../kernel/include/oxys/syscall.h`](../../kernel/include/oxys/syscall.h),
whose design is [`PRIVILEGE.md`](PRIVILEGE.md). The assertion is
[`../../kernel/test/verify_string.c`](../../kernel/test/verify_string.c).

**Specifications**: ISO/IEC 9899:2011, Section 7.24 (string handling) and
Section 4, paragraph 6 (what a freestanding implementation must provide); System
V Application Binary Interface, AMD64 supplement, Section 3.1.2 (the LP64 model)
and Section 3.2.3 (the argument registers Section 2 departs from in one place).

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
until this change one thing did: `kernel/include/oxys/syscall.h` held both halves
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
[`../../kernel/include/oxys/syscall.h`](../../kernel/include/oxys/syscall.h),
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

Three functions of Section 7.24 are absent. None is absent by oversight, and the
header says so where a reader will meet it.

| Function | Standard | Why not |
| -------- | -------- | ------- |
| `strcoll` | 7.24.4.3 | Compares according to the current locale. There is no locale in this system and no `<locale.h>` to establish one, so it would be `strcmp` under another name — an agreement with the standard that this library could not yet keep. It arrives with the locale. |
| `strxfrm` | 7.24.4.5 | The same, in the other direction: a transformation defined by a locale that does not exist. |
| `strerror` | 7.24.6.2 | Maps an integer to a message, and the integers are the failure results of `<oxys/syscall_abi.h>` as the wrappers of sub-task 7.2 will present them through `errno`. The table belongs beside the thing that sets `errno`; putting it here would fix the spelling of every error message in the system before a single call had a wrapper. |

**No non-standard function has been added either.** `strnlen`, `strdup`,
`strlcpy` and `memccpy` are each useful and each is somebody else's standard, not
ISO C's; `strdup` in any case allocates, and there is no allocator before
sub-task 7.3. Section 6, limitation 2, records the one of them that is genuinely
wanted and what deciding about it depends upon.

## 5. Verification

`KernelVerifyString`, in
[`../../kernel/test/verify_string.c`](../../kernel/test/verify_string.c).

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
   about what a ported compiler and a ported toolchain expect to link against,
   and it is better taken when sub-task 7.2 has shown what those are, than
   invented here.
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
   against `libc/include`. The single exception is
   `kernel/test/verify_string.c`, which is named by an explicit rule in the
   `Makefile` — so the exception is one line in one file, and a kernel source
   that tried to include `<string.h>` would fail to compile rather than quietly
   acquiring a dependency upon the userland.
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
