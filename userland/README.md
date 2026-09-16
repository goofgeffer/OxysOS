<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `userland/` — The Programs That Run Upon This System

**Phase**: 7 and 8 of [`../docs/project/PLAN.md`](../docs/project/PLAN.md). Sub-task
7.5 placed the first material here; sub-task 7.6 added the utilities and the three
programs that assert them; and sub-task 7.7 put five of them somewhere a person
could find, in `/bin` upon the initial ramdisk the kernel mounts as its root; and
sub-task 8.1 added the shell, which the kernel starts when the boot finishes, and
the program that asserts its line editor; 8.5 added `touch`, `cp` and `rmdir`, and 8.6
`wc` and, at the project owner's request, the editor `micro`, so that `/bin` holds
eleven. See
[`../docs/storage/INITRD.md`](../docs/storage/INITRD.md), and Section 2 of it for
why the six `-check` programs are **not** carried there: each exists to make a
machine-readable statement about a system call, so each is embedded in the kernel
image beside the self-test that runs it, and a system that shipped them in `/bin`
would be shipping its own test harness to somebody who asked for a shell.
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
| [`cat/main.c`](cat/main.c) | Sub-task 7.6: copies each operand to the standard output — and, since 8.5, the standard input for no operand or `-`: a file the shell redirected ends, and the terminal ends at a control-D, `cat` treating it as the end because no line discipline does. |
| [`ls/main.c`](ls/main.c) | Sub-task 7.6: lists each directory operand — or, with none, the working directory, since sub-task 8.3 gave it one — one entry to a line, with `-a`. It does not sort, this system having no locale and no heap sized by a directory it has not finished reading. |
| [`mkdir/main.c`](mkdir/main.c) | Sub-task 7.6: creates each operand, with `-p`. No file mode creation mask is applied, this system having none. |
| [`rm/main.c`](rm/main.c) | Sub-task 7.6: removes each operand, with `-f`. A directory is refused, there being no call that removes one. |
| [`touch/main.c`](touch/main.c) | Sub-task 8.5: creates each operand that does not exist — the first utility here to make a file. No timestamps, this system keeping no clock. |
| [`cp/main.c`](cp/main.c) | Sub-task 8.5: copies one file to another, created or truncated — the first utility here to write what it read. One source and one target. |
| [`rmdir/main.c`](rmdir/main.c) | Sub-task 8.5: removes each empty directory operand, by the call `rm` could not stand in for since 7.6. |
| [`micro/main.c`](micro/main.c) | Added on 2026-09-16 at the project owner's request, beside sub-task 8.6: the smallest editor that can edit. `micro FILE` loads the file, prints it numbered, and takes `p`, `a`, `i N`, `e N`, `d N`, `w`, `q`, `q!` and `wq`; `e` hands the line back through the C library's line editor to be changed with the arrow keys rather than retyped. A line editor and not a screen editor, because the display interprets no cursor-positioning sequence; the file is held whole and written whole, there being no `lseek`. |
| [`wc/main.c`](wc/main.c) | Sub-task 8.6: counts the newlines, words and bytes of each operand, or of the standard input for none, with `-c`, `-l` and `-w` and a `total` line — the first utility whose reason to exist is the pipeline, being what a person puts at the end of one. It reads the standard input to its end and not to a control-D, a pipe ending when its writer does. |
| [`arg-check/main.c`](arg-check/main.c) | Sub-task 7.6: compares the argument vector it was given against the vector it expects, and ends with the number of comparisons that failed. It exists because nothing in this kernel can read what a program printed. |
| [`exec-check/main.c`](exec-check/main.c) | Sub-task 7.6: becomes `arg-check` through `execve`, so that a vector crosses an address space that is destroyed. It has no assertions of its own — upon success it no longer exists, and the status the kernel collects is `arg-check`'s. |
| [`file-check/main.c`](file-check/main.c) | Sub-task 7.6, extended at 8.5 and 8.6: asserts the six filesystem calls by comparison — a file of known contents read byte for byte, a directory of known entries listed, and twenty refusals asserted by the **name** of the failure rather than by its sign. It was written because a negative test showed that a `read` delivering no bytes at all was reported by nothing. Since 8.6 it asserts the pipe as well: the ends, the end of the file, `EPIPE`, and twelve kibibytes crossing from a child. |
| [`sh/main.c`](sh/main.c) | Sub-task 8.1, extended at 8.2, 8.3, 8.4, 8.5 and 8.6: the shell, as far as a line editor, a parser, nine built-ins, `fork` and `execve`, redirections and pipelines take it. It prompts with `oxys$/> ` — the working directory between the two, since 2026-09-16 — reads a command through the C library's editor — continuing it upon a `> ` prompt where a quote or an operator is left open — parses it, expands `$NAME` and `$?`, applies assignments, runs `cd`, `pwd`, `export` and `exit`, honours `&&`, `||` and `!`, and runs every other command as a program sought upon `PATH`, with the 127 of a command not found where there is none; performs a program's redirections in the child; and runs a pipeline as one child per command with a pipe between each pair, the status the last command's. A refused line is answered with the status and the token it stopped at. `exit` or control-D ends it. |
| [`sh/shell.h`](sh/shell.h) | Sub-task 8.2: the token, the command structure the parser builds — pipelines of simple commands with their redirections, conditions and separators — the bounds upon both, and why the tokens keep their quotes. |
| [`sh/lexer.c`](sh/lexer.c) | Sub-task 8.2: the tokeniser of IEEE Std 1003.1-2017, Section 2.3, the quoting of Section 2.2, and the quote removal of Section 2.6.7 applied last. Compiled into the kernel image as well, where the self-test asserts it. |
| [`sh/parser.c`](sh/parser.c) | Sub-task 8.2: the subset of Section 2.10's grammar this shell implements, and the remainder refused by name. Compiled into the kernel image as well. |
| [`sh/expand.c`](sh/expand.c) | Sub-task 8.3: parameter expansion — `$NAME`, `${NAME}`, `$?` — and quote removal in one pass, so that a value's characters are never quoting characters. Takes a lookup function, so the kernel asserts it against a table of its own. Compiled into the kernel image as well. |
| [`sh/variables.c`](sh/variables.c) | Sub-task 8.3: the variable table, fixed at sixty-four, each marked exported or not; what a name is; what an assignment word is. Compiled into the kernel image as well. |
| [`sh/builtins.c`](sh/builtins.c) | Sub-task 8.3: `cd`, `pwd`, `export` and `exit`, and since 8.5 `help`, `true`, `false` and `unset` — `help` a list of every command since 8.6, one to a line — and `clear`, one form feed, added on 2026-09-16 — the one unit of the shell's that reaches a system call, and therefore the one not compiled into the kernel image. |
| [`sh/run.c`](sh/run.c) | Sub-task 8.4: the search upon `PATH`, the environment built from the exported variables, the `fork`, the `execve` tried upon each candidate in the child, and the `wait` that turns what the child ended with into a status; since 8.5, the redirections performed in the child between the two, in the order written; since 8.6, the pipeline — one child per command, a pipe between each pair, every end closed where it is not needed, every child collected. Reaches system calls, so not compiled into the kernel image. |
| [`line-check/main.c`](line-check/main.c) | Sub-task 8.1: reads an editing session the kernel's self-test placed upon the terminal, through the same `LineRead` the shell uses, and ends with the number of lines that were not what the session should have edited into. It asserts the half of the sub-task the kernel cannot: the `read` of descriptor 0 and the editor's output reaching descriptor 1, both of which execute `SYSCALL`. |
| [`dir-check/main.c`](dir-check/main.c) | Sub-task 8.3: asserts the working directory from the only place it can be asserted — a program — and ends with the number of assertions that failed: that a process begins at the root, that `chdir` moves it and `getcwd` reports it canonically, that a relative path is resolved against it by `open`, that a child of `fork` inherits it, and that each refusal is the named one. |
| [`env-check/main.c`](env-check/main.c) | Sub-task 8.4: asserts what the shell gives a program it runs — the argument vector as typed with its quotes removed, the exported variables in the environment and the assigned ones absent — and ends with the number that failed. The self-test writes it to `/verify/env-check` for the session and removes it, a check program not being shipped in `/bin`. |

**The `-check` programs are not utilities and are not shipped as such.**
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
this one, every `.c` file in it is the program's, and the directory's name is the
program's**, so adding one is adding a name to that list and a directory beside
the others. It was `main.c` alone until sub-task 8.2, whose shell was three
translation units and is seven at 8.4; four of them — the tokeniser, the parser,
the expansion and the variables — are also compiled
into the kernel image under `SHELL_SOURCES`, where the self-test asserts the
grammar without running the shell — the arrangement the C library has, and the
`Makefile` records why a program's sources in the kernel image are named apart.

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

**The environment vector is real since sub-task 8.4.** A program the shell runs
finds the variables the shell exported, as `NAME=value` strings, and `getenv`
of `<stdlib.h>` searches them; `_start` records where they stand. A program
the kernel starts — the shell itself, and the check programs — still finds
`envp[0]` a null pointer, nothing in the kernel setting an environment.

**A program may open, read, list, create, write, truncate and append a file,
and remove a directory** — the last five since sub-task 8.5. The kernel had
fourteen system calls at 7.6 and has eighteen since 8.5; `OxysOpen` accepts
READ, WRITE, CREATE, TRUNCATE, APPEND and DIRECTORY and refuses every other
bit, `OxysWrite` reaches a file or the diagnostic path, `OxysDuplicate` is
`dup2`, and a child of `fork` inherits every descriptor. `fopen` is still
absent from `<stdio.h>`, a program reaching a file through `<syscall.h>` and a
descriptor instead. **Since sub-task 8.1 `stdin` is the terminal** — a `read`
of descriptor 0 waits until something has been typed and delivers it raw,
unechoed and unedited, which is what the shell's line editor wants and what
`fgets` upon `stdin` does not; [`../docs/design/SHELL.md`](../docs/design/SHELL.md),
Section 2.1, records why there is no canonical mode — **or, since 8.5, the
file the shell redirected it from.** The rest is recorded at the head of the
header that would otherwise declare it, and the reasoning is
[`../docs/design/LIBC.md`](../docs/design/LIBC.md), Section 12.7.
**Since sub-task 8.3 there is a working directory**: every process holds one,
`OxysChangeDirectory` moves it, `OxysGetWorkingDirectory` reports it, and the
kernel resolves every relative path against it — so `ls` with no operand lists
`.`. Until then a relative path resolved against the root, which is why `ls`
listed `/`. The kernel has **sixteen** system calls since that sub-task.
