<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `startup-check` — The Program That Asserts the Procedure That Built It

**Phase**: 7, sub-task 7.5, of
[`../../docs/project/PLAN.md`](../../docs/project/PLAN.md).
**Detailed design**: [`../../docs/design/LIBC.md`](../../docs/design/LIBC.md),
Section 11.5.

## Purpose

[`main.c`](main.c) is the first program in this project produced by a compiler.
Every program before it was assembled byte by byte into an array by
`kernel/test/program.c`, because there was no way to build one.

It exists to assert the thing that built it: the startup object, the linker
script, the archive, the kernel's half of the process-entry contract, and every
translation unit of the C library compiled with a program's flags rather than the
kernel's. Those flags differ, so this is a genuine second compilation of code
that had only ever been compiled one way.

## What it asserts

The initial process stack of the System V ABI, Section 3.4.1; the string
functions and the system-call wrappers, running at privilege level 3 for the
first time; `malloc` obtaining memory from the break, which joins two halves that
sub-tasks 7.3 asserted apart; `printf` reaching a descriptor, which is the half
of sub-task 7.4 the kernel cannot assert at all; and `exit` calling what `atexit`
registered **before** it flushes the streams.

The last of those is why the program is shaped as it is. It leaves a partial line
in the standard output's buffer as `main` returns, and a registered function
asserts the line is still there — which is only true if the flush has not
happened yet, and is the single moment at which that order is visible from inside
a program.

## How it reports, and why twice

It **prints**, so a person reading the serial log sees what happened and so that
every failure carries the word `FAILED`, which `make verify` greps for.

It also **ends with a status** that is the number of assertions that failed, and
[`../../kernel/test/libc/startup.c`](../../kernel/test/libc/startup.c) checks
that status against zero. The second is the one the test rests upon: the first
depends upon the very machinery under test, so a program whose `printf` did not
work would print nothing, and a test whose only evidence was output would read
silence as success.
