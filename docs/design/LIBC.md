<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The C Library

**Phase**: 7, sub-tasks 7.1 and 7.2, of [`../project/PLAN.md`](../project/PLAN.md).

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
[`../../kernel/include/oxys/syscall.h`](../../kernel/include/oxys/syscall.h),
whose design is [`PRIVILEGE.md`](PRIVILEGE.md). The wrappers of Section 8 are
[`../../libc/include/syscall.h`](../../libc/include/syscall.h),
[`../../libc/include/errno.h`](../../libc/include/errno.h),
[`../../libc/syscall/invoke.asm`](../../libc/syscall/invoke.asm),
[`../../libc/syscall/result.c`](../../libc/syscall/result.c) and
[`../../libc/syscall/calls.c`](../../libc/syscall/calls.c). The assertions are
[`../../kernel/test/verify_string.c`](../../kernel/test/verify_string.c) and
[`../../kernel/test/verify_wrappers.c`](../../kernel/test/verify_wrappers.c).

**Specifications**: ISO/IEC 9899:2011, Section 7.24 (string handling), Section
7.5 (`<errno.h>`) and Section 4, paragraph 6 (what a freestanding implementation
must provide); System V Application Binary Interface, AMD64 supplement, Section
3.1.2 (the LP64 model) and Section 3.2.3 (the argument registers Section 2
departs from in one place); Intel 64 and IA-32 Architectures Software Developer's
Manual, Volume 2B, "SYSCALL".

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
[`../../kernel/test/verify_wrappers.c`](../../kernel/test/verify_wrappers.c). It
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
