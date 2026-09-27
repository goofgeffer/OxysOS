<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The C Library

**Phase**: 7 of [`../project/PLAN.md`](../project/PLAN.md), with `<signal.h>`
from sub-task 8.7 and the wrappers of the calls later phases added.
**Source**: [`../../libc/`](../../libc/), whose README lists every unit; the
interface header [`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h);
the kernel's halves of `brk`, the filesystem calls and the process-entry stack
in [`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c)
and [`../../kernel/proc/process.c`](../../kernel/proc/process.c).
**Specifications**: ISO/IEC 9899:2011, Sections 4 (paragraph 6), 5.1.2.2.1, 6.5,
6.5.8, 6.3.2.3, 7.5, 7.14, 7.21, 7.22.3, 7.22.4 and 7.24; System V ABI, AMD64
supplement, Sections 3.1.2, 3.2.2, 3.2.3 and 3.4.1; IEEE Std 1003.1-2017, the
`echo`, `cat`, `ls`, `mkdir` and `rm` utilities, the Utility Syntax Guidelines,
`kill` and `pause`; Intel SDM, Volume 2B, "SYSCALL".

The library every program links: the string functions, the system-call
wrappers and `errno`, the heap, the buffered streams and formatted output, the
startup object and termination, and `<signal.h>`. The line editor is
[`SHELL.md`](SHELL.md)'s, the configuration parser [`CONFIG.md`](CONFIG.md)'s,
the terminal grid [`TERMINAL.md`](TERMINAL.md)'s, the icon and image readers
[`UTILITIES.md`](UTILITIES.md)'s and `gmtime` [`../devices/TIME.md`](../devices/TIME.md)'s.

## 1. Premises

- **The library is `MIT` and the kernel `LGPL-3.0-or-later`**, so a program that
  links the library owes nothing the kernel's licence would ask
  ([`../../LICENSING.md`](../../LICENSING.md), Section 2).
- **It is compiled twice.** Once with a program's flags into
  `build/user/liboxys.a`, which every program links (`LIBC_SOURCES` in the
  Makefile). Once into the kernel image, where boot-time self-tests call it
  directly. The second compilation exists because the self-tests reach what a
  program cannot: a heap given a region, a stream given memory for a device, an
  editor given an output function to record.
- **The kernel does not call it.** No kernel unit is compiled against
  `libc/include` except the self-tests that assert the library, each named by a
  rule in the Makefile rather than a pattern over `kernel/test/`, so a kernel
  source that included `<string.h>` fails to compile instead of quietly
  depending on the userland.
- **Policy and system are divided.** A part that decides (the allocator, the
  buffering, the conversion, the editor, the parsers) is ordinary C in one
  unit; the part that makes a system call is a few lines in a unit of its own,
  usually `system.c`. The kernel cannot execute `SYSCALL`: the `SYSRET` that
  ends its handling returns to privilege level 3 unconditionally, so a kernel
  that flushed a stream would leave its own entry path as a user program. The
  division is what lets the policy be asserted at boot and the system half by a
  program.
- **A function bearing a standard name behaves as the standard says**, or is
  absent. A standard name with other behaviour is worse than the function
  missing or a different name, and a function that can only fail is worse than
  one that does not exist.

## 2. The interface header

The call numbers, failure results, register convention, path bound, vector
bounds and the kernel's lowest address are
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h),
under `MIT`; the kernel's implementation of the interface (the model-specific
registers, the saved frame, the dispatch) stays in
`kernel/include/oxys/arch/syscall/syscall.h` under the kernel's licence and
includes it.

- **`kernel/abi/` is a separate include root**, so the library reaches exactly
  what it is entitled to with `-Ikernel/abi`. Adding `-Ikernel/include` for one
  header would put every kernel header one `#include` away.
- **It holds constants and nothing with a symbol.** A declaration there would be
  a function both sides had to agree existed; the wrappers are the library's.
- **A number once handed to a program does not change**, so calls are numbered
  in the order they were added.

## 3. The string functions

`<string.h>` is twenty functions of Section 7.24, in units divided as the
standard divides it: `copying.c` (7.24.2 and 7.24.3), `comparison.c` (7.24.4),
`search.c` (7.24.5), `miscellaneous.c` (7.24.6) and `error.c` (`strerror`,
7.24.6.2). `strcoll` and `strxfrm` are absent: they compare by a locale, and
without one they would be `strcmp` under another name. No non-standard function
(`strdup`, `strlcpy`, `strnlen`) is added under a standard's name.

- **Every byte is examined as `unsigned char`**, as the standard requires of the
  comparisons and Section 6.5, paragraph 7, permits of an object's
  representation. Plain `char` is signed here, so a comparison through it is
  right below 128 and wrong above it, and nothing faults.
- **Three behaviours that look like defects are kept exactly.** `strncpy` pads
  and does not terminate; `strncat` terminates and does not pad; `strchr` and
  `strrchr` find the terminator. An "improved" version would be a standard name
  with non-standard behaviour.
- **`strtok` keeps its position in static storage**; that is the interface, and
  `strtok_r` is someone else's standard.
- **`strerror` maps any `int`** (7.24.6.2, paragraph 2), and its messages are
  arrays rather than literals, since `-Wwrite-strings` makes a literal `const`.
- **Every function is a byte-at-a-time loop.** Nothing calls them where speed
  matters, and the measurement should come before the optimisation.

## 4. The wrappers and `errno`

**The instruction is assembly**: four routines in
[`../../libc/syscall/invoke.asm`](../../libc/syscall/invoke.asm), for calls of
none to three arguments. Each moves its arguments one register left, since the C
convention (ABI Section 3.2.3) puts the number in `RDI` and the kernel reads it
from `RAX`, consuming `RCX` before `SYSCALL` overwrites it with the return
address. It is NASM and not inline assembly because the block must hold **no
relocation**: its bytes mean the same at any address, which lets the self-test
copy them into a composed program and run the code the library ships at
privilege level 3. Calls needing more arguments pass a structure.

**The translation**, [`../../libc/syscall/result.c`](../../libc/syscall/result.c),
turns a kernel result into a value or `-1` and `errno`:

- **A result that is not negative is returned unchanged and `errno` is
  untouched**, so a program that clears `errno`, calls, and finds it set is
  never told of a failure that did not happen.
- **A result in the reserved range becomes its own name by negation.** There is
  no table: `<errno.h>` defines each number as the negated kernel result, held
  by a `_Static_assert`, so a result added on one side and not the other fails
  to compile instead of reporting the wrong cause.
- **Anything else is `ENOSYS`**, the one name that says what is known. Negating
  an arbitrary value names nothing, and negating `INT64_MIN` is undefined.
- **The numbers are reserved in two ranges**: the kernel's results from one to
  thirty-one, `EDOM`, `EILSEQ` and `ERANGE` at thirty-two and above, so a new
  kernel result never arrives wearing a mathematical name.
- **`errno` is a function call** (Section 7.5, footnote 201), so giving each
  thread its own object would change one file rather than the interface.

**The wrappers**, [`../../libc/syscall/calls.c`](../../libc/syscall/calls.c), are
one per call, each a cast, an invocation and the translation, named `Oxys...`
in `PascalCase` (`OxysWrite`, `OxysFork`, `OxysExecve`, `OxysWaitFor`,
`OxysPipe`, `OxysWindowBlit`, `OxysPower`, `OxysPause` and the rest).
They keep the project's names because several differ from POSIX's function of
the same purpose, and `OxysSbrk` is built from `OxysBrk` in the library.
`OxysExecve` passes its vectors on, since the refusal is the kernel's to make;
`OxysExit` ends in an unreachable loop that makes its `_Noreturn` true by
construction.

## 5. The heap

`malloc`, `calloc`, `realloc` and `free` (Section 7.22.3) above
[`../../libc/stdlib/heap.c`](../../libc/stdlib/heap.c). What the allocator knows
of the machine is `OxysHeapExtend` in `stdlib/system.c`; `OxysHeapAdopt` gives
it a region a caller obtained itself, which a program with a static arena needs
as much as the self-test does.

**The break beneath it** is the address one past a program's heap. `brk` moves
it and returns where it stands; `SYSCALL_BREAK_QUERY` reports it.

- **The heap begins a page above the image**, rounded and then one guard page
  further, so an overrun of the last static array faults instead of landing in
  the allocator's bookkeeping.
- **A failure is a negative result**, not the unchanged break the traditional
  call returns, which a caller must compare against what it asked for and
  usually does not.
- **A request below the heap's start is refused, not clamped**, and **a growth
  that cannot complete is undone**, so no break names pages half mapped.
- **Pages are mapped zeroed and eagerly**, there being no demand paging, and
  one process may ask for sixteen mebibytes (`PROCESS_BREAK_MAXIMUM`) so that it
  cannot consume every frame before being refused.
- **A shrink unmaps and releases the frames**; `AddressSpaceUnmapPage` returns
  the frame because only the caller knows whether a copy-on-write clone still
  refers to it.
- **`fork` copies both bounds; `execve` places the break anew** from the new
  image.

**The allocator** is first fit over a free list ordered by address, each block
carrying a thirty-two byte header. Best fit walks the whole list and leaves the
least usable remainders; a size-ordered list cannot find a block's neighbours in
memory; size classes need a workload to size them. The address order means the
walk that places a released block also finds both neighbours, and **the
successor is joined first**, so three adjacent free blocks become one.

- **`malloc(0)` returns a distinct pointer**, since null cannot be told from a
  failure.
- **A failure sets `errno`**: `ENOMEM`, or `EINVAL` when `realloc` is given
  something that is not an allocation.
- **`free` of something that is not an allocation is refused and counted.** The
  header carries an eight-byte mark (`OXYSFREE`, `OXYSLIVE`), not a flag bit, so
  a double release is refused instead of putting one block on the list twice.
- **The header does not overlay the payload**, which would read one storage
  through two types.
- **Block addresses are compared as `uintptr_t`**: relational comparison of
  pointers into different objects is undefined (6.5.8, paragraph 5), the
  conversion only implementation-defined.
- **`calloc` refuses a product that wraps**, the one security property here.
- **`realloc` grows in place** when the next block is free, and shrinks in
  place.

## 6. Streams and formatted output

`<stdio.h>` is the `FILE` of Section 7.21, the three standard streams, the byte
and block transfers, the indicators, and formatted output. The policy is
[`../../libc/stdio/stream.c`](../../libc/stdio/stream.c) and
[`format.c`](../../libc/stdio/format.c); the device is `OxysStreamWrite` and
`OxysStreamFill` in `system.c`, above `write` and `read`. A **memory stream**
gives a stream a region for a device, which is how the self-test runs the
shipped code and what a hosted library calls `fmemopen`. The census counts the
two transfers apart, so the self-test asserts that neither reached the system.

| Stream | Descriptor | Buffering | Why |
| ------ | ---------- | --------- | --- |
| `stdin` | 0 | Full | The terminal or a redirected file. |
| `stdout` | 1 | Line | Section 7.21.3, paragraph 7, allows full buffering only away from an interactive device, and the last partial line before a fault is the one worth having. |
| `stderr` | 2 | None | A diagnostic in a buffer when a program dies was not issued. |

The streams are initialised statically, so they work in a program whose first
statement is `puts`, with no constructors. `FILE` is incomplete, so no program
depends on its members. Buffers are static, `FOPEN_MAX` of `BUFSIZ`, because a
stream must work before a heap does and when the heap has failed.

- **A buffer is emptied when full, before the append**; the other order writes
  one byte past a one-byte buffer, which `setvbuf` permits.
- **A short transfer is normal**, and the policy calls again from where it
  stopped, so a message longer than the kernel's bound is not truncated.
- **A stream at its end does not ask the source again**, or a loop on `fgetc`
  would make a system call per iteration forever.
- **The error indicator is sticky** until `clearerr`.

**The conversion is one engine beneath eight names**, one destination function
each, so `printf` and `snprintf` cannot format differently. It counts what it
produced, not what was stored, which is `snprintf`'s required result and what a
caller sizing a buffer with a size of zero depends on. It implements the flags
`-`, `+`, space, `#` and `0`; width and precision as digits or `*`; the length
modifiers `hh`, `h`, `l`, `ll`, `z`, `j`, `t`; and `d i o u x X c s p %`.

- **The three paddings keep their order**: precision zeroes inside the sign,
  the `0` flag's zeroes to the width only without `-` or a precision, the field's
  spaces outside all.
- **A narrow modifier reads an `int` and converts**, the promotions having
  widened the argument.
- **The magnitude is formed without negating**, which is undefined for the
  most negative value.
- **No floating conversion**: the library is built without SSE or the x87, and
  the guidelines forbid unjustified floating point.
- **`%n` is refused**: it writes through a pointer chosen by the format string,
  which is how a string a program received becomes a write to memory.
- **An unimplemented conversion is refused and reported** as a failure, since a
  wrong count is what nobody checks.

## 7. Startup, termination and the link

**What a program finds on its stack** is the ABI's (Section 3.4.1): the argument
count at the stack pointer, the argument pointers and a null, the environment
pointers and a null, a null auxiliary entry, and the strings above, with the
stack pointer sixteen-byte aligned. `ProcessLayOutArguments` builds it
downward, strings first so the pointers name fixed addresses, padding computed
after sizing, eightbytes written a byte at a time because two stack pages need
not be adjacent frames, and through the direct map because the space being
filled is often not the active one. `execve` copies both vectors into the
kernel first, as offsets rather than pointers, bounded at sixteen strings and two
kibibytes together, before it destroys the space they stand in; a refusal comes
before that point, so a refused program keeps running.

**`crt0.asm`** clears `%rbp` so a debugger's walk stops, passes `argc`, `argv`
and `envp` to `main`, records `envp` for `getenv`, aligns the stack again (free,
and the failure it forecloses is costly to diagnose), and gives `main`'s result
to `exit`. It is assembly because C cannot read its own stack pointer and there
is no return address. It calls no constructors, nothing having one, and does not
register the ABI's `%rdx` finaliser, which is null.

**Termination**, [`../../libc/stdlib/exit.c`](../../libc/stdlib/exit.c):
`atexit` holds thirty-two registrations in `.bss`, so it cannot fail for memory.
`exit` calls them in reverse and **then** flushes the streams (7.22.4.4), since a
registered function's diagnostic goes into a buffer; a second `exit` from within
one goes to `_Exit`. `_Exit` flushes nothing, for a program that distrusts the
library's state, and `abort` ends through it. The status is not masked to eight
bits.

**The link**: the library's sources compiled with the kernel's flags less
`-mcmodel=kernel`, which would state that every symbol lies in the top two
gibibytes; archived by `ar`; linked with
[`../../libc/user.ld`](../../libc/user.ld) at four mebibytes, so the first page
is unmapped and null faults, with three `PT_LOAD` segments one per permission
and page-aligned apart. `-n` stops the file being padded for a demand-paged
loader this kernel is not. The embedded copies are stripped; the unstripped
`build/user/*.elf` keep their DWARF for a debugger. Programs are dependencies of
the kernel image, so no `make` target was added.

## 8. The filesystem calls and the descriptor table

Every path-taking call validates its arguments and calls the filesystem layer
([`../storage/VFS.md`](../storage/VFS.md)); none reimplements it.

- **Every cause the layer distinguishes reaches a program by name**, through
  `SyscallFromVfsError`, a switch with no `default` so that a new cause is a
  build failure. A program acts on `errno`: `rm -f` accepts `ENOENT`, `mkdir -p`
  accepts `EEXIST`, and collapsing the names into two would make each do the
  wrong thing with a plausible status.
- **`readdir` has three results**: 1 for an entry, 0 at the end, negative on
  failure, so a medium failure is not read as the end.
- **`read` of zero bytes is refused**, zero being the end of a file.
- **A path that is too long is `ENAMETOOLONG`**, distinguished from `EFAULT` by
  whether its first byte was readable, so the caller is told which of two things
  it can fix.

**A process holds sixteen descriptors inline**, grown in the heap as it opens
more up to `SYSCALL_DESCRIPTOR_LIMIT` (1024), a ceiling on its share; each
naming an open file of the layer's one machine-wide table. The indirection keeps
one program from naming another's file by number and bounds its share. **The
free value is not zero**, zero being a valid open file: a table cleared by
`memset` would close someone else's file at the first `close`. `fork` shares
every entry and `execve` keeps them, the layer counting holders; destruction
releases what a process still held.

## 9. `<signal.h>` and the later calls

`<signal.h>` is Section 7.14 (`sig_atomic_t`, `SIG_DFL`, `SIG_IGN`, `SIG_ERR`,
`signal`, `raise`) and POSIX's `kill`, numbered as the x86 System V convention
numbers them and asserted against the kernel's by `_Static_assert`. `signal`
installs the handler with the restorer, a `sigreturn` call at the end of
`invoke.asm` outside the block the self-test copies; it is named nowhere else,
so no handler returns through the wrong address. A handler may call anything, but
one that calls `malloc` or `printf` while the interrupted code is inside them
corrupts them; the header gives the standard's rule of setting a `volatile
sig_atomic_t`. A status is read through `SYSCALL_STATUS_KIND` and
`SYSCALL_STATUS_NUMBER`; there is no `<sys/wait.h>`.

The wrappers of later calls stand beside the others: the window calls
([`WINDOWS.md`](WINDOWS.md)), `link` and `procinfo` for `mv` and `ps`, `power`
(refused `EPERM` to any caller but `init`, the first refusal for being the wrong
program) and `pause`, which always returns `-1` with `EINTR` ([`INIT.md`](INIT.md)),
and the calls of Phase 9, each with its own document.

## 10. The utilities of Phase 7

Each is `main.c` in a directory of [`../../userland/`](../../userland/), linked
against the archive.

| Program | Does | Does not |
| ------- | ---- | -------- |
| `echo` | Writes its operands, one space apart, and a newline. | `-n` and backslashes are ordinary, both being implementation-defined and either alternative leaving something unprintable. |
| `cat` | Copies each operand, or the standard input for none or `-`, through `fputs` and `fwrite`, never as a `printf` format. | `-u`. |
| `ls` | Lists each directory, `.` for none, one entry to a line, with `-a`. | Sort: there is no locale, and sorting needs every name held. |
| `mkdir` | Creates each operand, with `-p`, mode 0777. | `-m`, or a creation mask. |
| `rm` | Removes each operand, with `-f`. | `-r` or `-i`. |

**Every operand is attempted after one fails**; **diagnostics go to standard
error**, so they never land in redirected output; **the option scan stops at the
first operand and at `--`** (Guidelines 9 and 10); **an unimplemented option is
refused**, never accepted and ignored.

**`arg-check`, `exec-check` and `file-check` assert what these cannot**, by
comparing and ending with the number of failed comparisons, because the kernel
cannot read what a program printed: a `read` that delivered no bytes let `cat`
exit with 0.

## Verification

The self-tests are [`../../kernel/test/libc/`](../../kernel/test/libc/)
`string.c`, `wrappers.c`, `heap.c`, `stdio.c`, `startup.c` and `utilities.c`,
the composed programs of [`../../kernel/test/program.c`](../../kernel/test/program.c)
(one encoder, every write bounds-checked and a refusal recorded), and the
compiled `startup-check`. The conversions were also compared with the host's
`snprintf` over sixty-six specifications: sixty-five agree and `%p` of null is
`0x0` here, `(nil)` there.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| `memcmp`, `strcmp` and `strncmp` put `0x80` above `0x01`; `memchr` finds `0x80`; `strlen` passes `0xFF`. | Comparison through signed `char`, right for all ASCII. |
| The margins either side of every destination keep the sentinel `0x5A`; zero-length calls write and read nothing. | A `<=` bound; a `do`/`while` that reads before testing. |
| `memmove` over overlaps both ways; `strncpy` pads and leaves no terminator when full; `strncat` terminates; `strchr` and `strrchr` differ on two matches and find the terminator. | A smeared copy; a "helpful" `strncpy`; `strrchr` that is `strchr`. |
| `strstr` of an empty needle, and of `"aab"` in `"aaab"`; `strtok` collapses separators and stays finished. | A search restarted at the wrong byte; a scan that restarts on a new string. |
| A success leaves `errno` alone, at zero, a length and `INT64_MAX`; each failure names itself; beyond the range and `INT64_MIN` are `ENOSYS`. | `errno` set on success; negation giving `EDOM` or zero. |
| Every `errno` has a distinct message, and any `int` maps. | A table with a hole. |
| A composed program running the shipped `invoke.asm` makes exactly the expected calls with an exact sum. | A wrong displacement landing in another routine that still returns. |
| An adopted region is one block of its size; every pointer is sixteen-aligned; three allocations keep three patterns. | A lost tail; misalignment; overlapping splits. |
| Released out of order, the heap returns to one block with the same census. | Any size error, or joining in one direction only. |
| A double release is refused once; `free(NULL)` is no event; a foreign pointer is refused. | One block on the list twice. |
| `realloc` grows in place into a free neighbour, moves with its contents past a live one, shrinks in place. | A correct and quadratic `realloc`. |
| `calloc` clears a dirtied block and refuses a wrapping product; `SIZE_MAX` is refused without asking the system. | A `calloc` that only works on fresh pages; a small block for a large request. |
| A composed program's `brk`: the break a page above the image, `EFAULT` before growth and after shrink, exactly the address asked for, writable at level 3; the kernel records no page left mapped. | A break reported but not mapped; a shrink that keeps the frames. |
| Full, line and unbuffered streams deliver when they should; a one-byte buffer keeps its sentinel; `setvbuf` refused after use. | Wrong buffering; a byte past a caller's buffer. |
| `fwrite` and `fread` count whole elements; indicators stick; `ungetc` once and clears end-of-file; `fgets` bounds, terminates and keeps a final partial line; an ended stream does not ask again. | A last line discarded; a system call per `fgetc` at the end. |
| Twenty-one conversions produce exact characters and lengths; `snprintf` truncates and reports the needed size; an unimplemented conversion is refused. | The right text with the wrong count. |
| No stream byte and no heap request reached the system during the self-test. | A self-test that resets the machine. |
| `startup-check` ends with 0 after asserting its vectors, the string functions, a wrapper's `errno`, `malloc` from the break, `printf`, and after `main` that registrations ran in reverse before the flush. | Silence taken for success; a flush before the registrations. |
| The startup image loads, is under sixty-four kibibytes, and its entry is its lowest address. | `main` placed ahead of `_start`; a padded link. |
| A process's descriptor table is emptied to the free value, not zeroed, and a process ending with a descriptor gives it back. | A fresh process closing another's file; a leaked open file. |
| `arg-check` directly and through `exec-check`; `file-check`'s bytes, entries and refusals by name; `mkdir`, `mkdir -p` and `rm` by what the volume holds, each paired with a refusal. | Arguments read after the space was destroyed; a right sign with the wrong name; a utility that did nothing and exited 0. |

## Limitations

1. `errno`, the heap and the streams are one per program and unlocked; there are
   no userland threads.
2. `strcoll`, `strxfrm`, `aligned_alloc`, the `scanf` family, the floating and
   wide conversions, `fopen` and positioning are absent, and of `<stdlib.h>` only
   the heap, termination and `getenv` exist.
3. No bounded copy that always terminates, and no `strtok_r`.
4. The string functions and the stream transfers are byte-at-a-time.
5. The heap's header is thirty-two bytes; a region is never returned to the
   system; `brk` maps eagerly.
6. `abort` does not raise SIGABRT; `sigaction`'s mask, flags and `siginfo`, and
   `alarm` and `sleep`, are absent.
7. Argument and environment vectors are bounded at sixteen strings and two
   kibibytes, and a process at 1024 descriptors.
8. Not assertable here: a growth undone part way (nothing makes `FrameAllocate`
   fail on demand), a heap page arriving unzeroed, the segments' permissions as
   mapped, the width `_start` moves, the magnitude of the most negative value, and
   the error indicator surviving a later success. Each guard stays because the
   standard promises nothing.
9. Programs link statically; there is no dynamic loader.
10. `mkdir`'s permissions are recorded and not enforced; `ls` does not sort.
