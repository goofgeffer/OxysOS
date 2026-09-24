<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Terminal Emulator

**Phase**: sub-task 9.6 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../userland/terminal/main.c`](../../userland/terminal/main.c),
[`../../libc/term/term.c`](../../libc/term/term.c),
[`../../libc/term/keys.c`](../../libc/term/keys.c), and in
[`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c)
`poll` and the `tcgroup` refusal.
**Specifications**: ECMA-48 (CR, LF, BS, HT, FF); IEEE Std 1003.1-2017, `poll()`
and `tcsetpgrp()`.

A window with an ordinary `/bin/sh` beneath it, so a person can type at the
system's own screen. It needed a character grid (in the C library), a way to wait
on two things at once (`poll`), and a rule that keeps a shell in a window from
taking the kernel's terminal.

## 1. A window, a shell and two pipes

```
      keys                                      bytes
  window ──► /bin/terminal ──► pipe ──► 0 ┐
                   ▲                       ├─ /bin/sh
                   └── pipe ◄── 1, 2 ──────┘
```

- **The shell is unmodified.** Its standard input is one pipe, its output and
  errors another; it edits, echoes and prompts as it always does. Everything about
  being a shell stays in the shell.
- **The child places its descriptors and closes every end it does not use**: an
  unused write end left open keeps a read from ever ending.
- **The child starts a process group of its own**, which the emulator does not
  join, so Control-C can be sent to it without interrupting the emulator.
- **White on black**, the program's own colours rather than
  [`../../art/palette.h`](../../art/palette.h)'s, which hold only what several
  components must agree on.
- **The ground is painted once, in bands** (`TerminalPaintGround`; there is no fill
  across the protocol). Rows are 8-pixel glyphs at a 10-pixel pitch, and the gaps
  are never drawn again; unpainted, they would show the window manager's paper as
  stripes.

When the shell ends, its output pipe reaches end-of-file, and the emulator closes
its window and exits.

## 2. The grid

[`../../libc/include/term.h`](../../libc/include/term.h): a fixed array of cells
(up to 120 columns, below the 127 characters one `window_text` carries, and 64
rows), a cursor, and a per-row mark saying the row changed since it was drawn.

| Byte | Effect | Why |
| ---- | ------ | --- |
| `\r` | Cursor to the left margin; nothing erased. | ECMA-48 CR. |
| `\n` | Down **and** to the left margin. | What the kernel's console does, and what every program here is written against. |
| `\b` | One left; at the margin, **to the end of the row above**; stops at the top left. | The line editor erases with space and backspace, and could not otherwise erase a wrapped line ([`CONSOLE.md`](CONSOLE.md)). |
| `\t` | To the next multiple of eight. | A tab otherwise draws as a hole. |
| `\f` | Every row blanked and marked; cursor to the top left. | The shell's `clear` writes it. |

Every other control byte is dropped, including `ESC`: **there are no escape
sequences**, so `\x1B[31m` shows as `[31m`. Nothing here emits them; the line
editor's output is printable characters, backspaces and spaces.

- **A line exactly the grid's width leaves the cursor on its last column**; the
  wrap happens on the next write, so a backspace does not land on the line below.
- **Only changed rows are drawn**, one `window_text` each. A scroll marks every
  row, because it moved all of them.
- **`TermResize`** fits the grid to a new window extent (the window made full or
  restored, [`WINDOWS.md`](WINDOWS.md)): rows are cut or padded, rows are dropped
  from the top only as needed to keep the cursor's row, and the whole content is
  repainted.

**Why in the C library:** pure memory can be asserted by the kernel's boot-time
self-test ([`LIBC.md`](LIBC.md)); what is left in `/bin/terminal` needs a person
and a window. The key translation (`keys.c`) is there too, a copy of the kernel's
table because an `MIT` program may not link the `LGPL` kernel's code; the self-test
asserts the two agree key by key.

**Keys to bytes**: a letter is itself; Control with a letter is its control byte,
so Control-C is byte 3 whatever the case; a key with no character contributes
nothing; cursor keys become the same sequences the kernel's terminal sends.

## 3. `poll`

The emulator waits on its window's events and on the shell's output. Sleeping in
either alone is deafness to the other; sleeping in neither is spinning, and a
spinning program is a machine that never halts. So `poll`, narrowed to
readability, which is all anything asks:

- Up to eight entries, each a descriptor or the constant `SYSCALL_POLL_WINDOWS`
  (the calling program's window events); `ready` is written against each and the
  count returned. `SYSCALL_POLL_NO_WAIT` asks without waiting.
- **A pipe at end-of-file is ready**: it reads zero at once. Calling it unready
  would leave the emulator asleep for ever on a shell that has ended.
- **The window queue is a constant, not a descriptor.** Window events are not a
  file; making them one would add a kind of open file for one caller. This is the
  one wart, stated.
- **One sleep channel.** A poller sleeps on a single channel the scheduler owns,
  and every source that can make something ready (a pipe written or its last
  writer closed, a window event routed) wakes it as well as its own channel. A
  poller woken for nothing rescans and sleeps; waiter queues per object would be
  machinery for one program ([`../../kernel/include/oxys/proc/sched.h`](../../kernel/include/oxys/proc/sched.h)).

## 4. No terminal for the shell in a window

The kernel has **one** terminal: the byte stream from the keyboard and serial line,
with one foreground process group ([`SHELL.md`](SHELL.md)). A shell claims it in
`ShellJobsInitialise`, and a second shell doing so would take it from the first,
leaving the person's shell stopped behind a blank screen.

**`tcgroup` is refused with `ENOTTY` to a process whose standard input is not the
terminal**, as POSIX refuses `tcsetpgrp`. The shell in a window has a pipe at 0, so
it is told it has no terminal, whatever it or any future program tries. It then
runs **without job control**, by decision:

- no job gets a group of its own; children stay in the shell's group, which is what
  the emulator interrupts;
- `fg`, `bg` and `kill %n` say this shell has no terminal;
- the foreground wait names the shell's own group.

**Control-C is sent by the emulator.** Nothing turns a byte in a pipe into a
signal, so the emulator is the line discipline: byte 3 becomes
`kill(-group, SIGINT)`. The shell ignores `SIGINT`, and the running command takes
the default action.

## Verification

`KernelVerifyTerm` in [`../../kernel/test/terminal/`](../../kernel/test/terminal/)
drives the grid and key translation with bytes; `poll-check`
([`../../userland/poll-check/main.c`](../../userland/poll-check/main.c)) asserts
`poll` and the refusal at privilege level 3. The emulator itself is operated by
hand ([`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md)).

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A grid of no columns, or too many, is refused. | Lines wrapped in the wrong place for ever. |
| Characters stand where written; the cursor advances; a full-width line leaves the cursor on its last column. | A premature wrap sending backspace to the next line. |
| LF goes down and to the margin; CR erases nothing. | Staircased output; a prompt blanking its own line. |
| FF blanks every row, homes the cursor, marks every row, and later text starts at the top left. | `clear` doing nothing, or leaving the old page drawn. |
| BS moves without erasing; a space erases. | The line editor deleting two characters or none. |
| BS at the margin crosses to the row above and stops at the top left. | A wrapped line that cannot be erased; a cursor off the grid. |
| LF on the last row scrolls and marks every row. | A terminal stuck at the bottom; a screen a line out of date. |
| A drawn row is no longer owed, and writing one row marks no other. | Every keystroke redrawing the screen. |
| `ESC` is dropped; no control byte draws a glyph. | Ink blots for unsupported sequences. |
| Letters are themselves; Control-C is 3 for `c` and `C`; Control with a digit changes nothing; a characterless key adds no bytes. | Control-C with Caps Lock sending `C`; zero bytes per Shift press. |
| **Each cursor key's sequence equals the kernel's.** | Keys that work at the serial line and not in a window. |
| An empty pipe is not ready, a pipe with a byte is, and a write end is never read-ready. | Sleeping through output; spinning on an unreadable pipe. |
| **A pipe whose last writer has gone is ready.** | A window that never closes. |
| A blocking poll returns when a child writes; with two entries, the ready one is marked. | A poll never woken; the wrong descriptor read. |
| Polling nothing, too many, an unknown option, an unheld descriptor, or the window queue without a window, is refused. | A wait nothing can end. |
| **`tcgroup` with a pipe at 0 is `ENOTTY`**; at the terminal it is not. | A window's shell taking the terminal from the keyboard's. |

## Limitations

1. No pseudo-terminal, so no job control in a window: no `fg`, `bg`, Control-Z or
   `kill %n`. A kernel terminal object with its own foreground group would provide
   it.
2. One window per emulator, one shell per window, no tabs.
3. No escape sequences, so no full-screen programs. The line editor already parses
   CSI on the input side ([`../../libc/line/line.c`](../../libc/line/line.c)).
4. A program cannot learn its terminal's size; nothing here lays out a screen.
5. No scrollback.
6. Keystrokes are written with a blocking write; a shell that never reads, fed four
   kilobytes of typing, would stop the emulator. `poll` has no write readiness.
7. No selection or clipboard; the pointer only raises and focuses the window.
