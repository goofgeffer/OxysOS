<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Terminal Emulator — a Window With a Shell Beneath It

**Phase**: 9, sub-task 9.6, of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../userland/terminal/main.c`](../../userland/terminal/main.c),
[`../../libc/term/term.c`](../../libc/term/term.c),
[`../../libc/term/keys.c`](../../libc/term/keys.c),
[`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c)
(`poll`, and the refusal of `tcgroup`).
**Asserted by**: `KernelVerifyTerm`,
[`../../userland/poll-check/main.c`](../../userland/poll-check/main.c).

## 1. What this sub-task is

A person could not, until this, type at this system's own screen. The shell of
Phase 8 ran upon the serial line while the window manager held the display, and
the desktop of sub-task 9.5 offered a launcher whose only entry drew pictures.
[`SESSION.md`](SESSION.md), Section 7, limitation 8, said why the shell could
not simply be added to that launcher, and this is the sub-task that removes the
reason.

It is four things:

1. **`/bin/terminal`**, a program that owns a window and a shell and carries
   bytes between them.
2. **The grid**, in the C library: what stands where, given the bytes a program
   writes to a terminal. It is the emulator's whole display model and it reads
   and writes nothing, so the kernel's self-test drives it without a window.
3. **`poll`**, a system call, because a program that must wait upon two
   different things had no way to.
4. **A refusal**: `tcgroup` from a process whose standard input is not the
   terminal is `ENOTTY`, which is what keeps the shell in a window from taking
   the terminal away from the shell at the serial line.

## 2. The shape: a window, a shell, and two pipes

```
      keys                                      bytes
  window ──► /bin/terminal ──► pipe ──► 0 ┐
                   ▲                       ├─ /bin/sh
                   └── pipe ◄── 1, 2 ──────┘
```

The shell is **an ordinary `/bin/sh`**. It is not modified, not told it is in a
window, and not given anything a shell upon the serial line does not have: its
standard input is the read end of one pipe, its output and its diagnostics the
write end of another, and it edits its line, echoes what it is given and prints
its prompt exactly as it always did. That is the property worth having, and it
is what makes the emulator small: everything about being a shell stays in the
shell.

**The child places its own descriptors and closes every end it does not need.**
An end left open in the child is an end the pipe still counts, and a pipe with a
writer that never writes is a read that never ends — the arrangement the shell's
own pipelines have used since sub-task 8.6.

**The child puts itself in a process group of its own**, and the emulator does
not join it. Control-C is sent to that group, and a program that interrupted its
own group would interrupt itself.

**White upon black**, at the project owner's direction on 2026-09-22, and the
two colours are the program's own rather than
[`../../art/palette.h`](../../art/palette.h)'s. That is the rule the
demonstration's coral and mint already follow: a colour belongs in the shared
header when two things must agree about it, and nothing else in this system
draws a terminal. The header's paper and ink are what a frame, a boot screen and
a desktop must agree upon — a label read in a glance. A window full of text a
person reads for minutes at a time is a different problem, and it was answered
with the system's warm paper until somebody had to look at it.

**The leading is painted once and never again.** A row is drawn as glyph cells
eight pixels tall at a pitch of ten, so the two pixels between rows are never
drawn into: what shows there is whatever the window's content held when it was
made, which is the window manager's paper. That was invisible while the terminal
drew upon the same paper, and would have become white stripes across the window
the instant it drew upon black. `TerminalPaintGround` blits the ground in bands
at start — **there is no fill across the protocol**, the wall the session met
for its panel ([`SESSION.md`](SESSION.md), Section 7, limitation 4) — and
nothing afterwards uncovers it, a row redrawn writing its own cells whole.

## 3. The grid, and what it interprets

[`../../libc/include/term.h`](../../libc/include/term.h) is a fixed array of
cells, a cursor, and a mark against each row saying whether it has been written
to since it was last drawn. It acts upon **five** control characters and drops
every other:

| Byte | What it does | Why that and not something else |
| ---- | ------------ | ------------------------------- |
| `\r` | The cursor to the left margin, erasing nothing | ECMA-48's CR |
| `\n` | Down **and** to the left margin | What the kernel's console does, and therefore what every program in this system has been written against. A line feed that moved down alone would put the second line of every message under the end of the first |
| `\b` | One position left, **crossing to the end of the row above** at the left margin | The line editor of sub-task 8.1 erases by writing a space and backspacing, and a line it has wrapped could not otherwise be erased. [`CONSOLE.md`](CONSOLE.md), Section 7, is where the same crossing was got wrong once and corrected |
| `\t` | To the next multiple of eight | Nothing here writes one; a tab left to fall through would be drawn as a hole |
| `\f` | Every row blanked, the cursor to the top left, every row owed a drawing | What the kernel's console and its VGA text driver do with a form feed, and what the shell's `clear` writes. **Added on 2026-09-24**: until then the grid dropped it with every other control byte, and `clear` in a terminal window did nothing and said nothing — the one place in the system a person would type it |

**There are no escape sequences.** An `ESC` is dropped and the bytes after it
are drawn, so `\x1B[31m` appears as `[31m`. That is the honest behaviour of a
terminal that does not have them, and it costs nothing today because nothing in
this system emits them: the line editor's whole output is printable characters,
backspaces and spaces, which is what Section 6 asserts. What it would cost is
recorded as limitation 3.

**The grid says which rows have changed, and the program draws those.** Each row
drawn is a `window_text` call; a screen of twenty rows redrawn for one keystroke
would be twenty calls and twenty rows of glyphs for one character. A scroll
marks every row, and that is not laziness — a scroll moves the text of the whole
grid, and a terminal that marked only the last row would draw a screen one line
out of date and stay that way.

**Why it is in the C library.** It is the seam of [`LIBC.md`](LIBC.md), Section
9, applied a third time, after the line editor's editing and the configuration's
parsing: pure memory is asserted by the kernel's self-test, and what is left in
`/bin/terminal` is the part that cannot be asserted without a person, a window
and a shell. The same division put the key translation there — and that copy
exists at all because a program under `MIT` may not link the `LGPL` kernel's,
the wall the disc of 9.2 was redrawn behind and the font of 9.5 stands behind
still. Two copies of a table are two things that can drift, so the self-test
asserts that the library's answer **is** the kernel's, key by key.

## 4. `poll`, and why waiting upon two things needed a call

The emulator waits upon the keys its window receives and upon the bytes the
shell writes. Each alone has a call that sleeps — `window_event` with
`SYSCALL_WINDOW_WAIT`, and `read` of a pipe — and:

- sleeping in the window's call is being deaf to the shell: the output of `ls`
  would appear at the next keystroke;
- sleeping in the read is being deaf to the keyboard;
- sleeping in neither is spinning, and a window that spins is a machine that
  never halts — which [`WINDOWS.md`](WINDOWS.md), Section 10, spent a sub-task
  avoiding, and which the livelock of [`SHELL.md`](SHELL.md), Section 2.6, shows
  the cost of exactly.

So `poll` — IEEE Std 1003.1-2017's, narrowed to readability, which is the whole
of what has a caller. It takes up to eight entries, each naming a descriptor or
the constant `SYSCALL_POLL_WINDOWS`, writes `ready` against each, and returns
how many are ready; `SYSCALL_POLL_NO_WAIT` asks for the answer now rather than
for a wait, which is the shape `waitpid`'s `SYSCALL_WAIT_NO_HANG` has and for
the same reason.

**A descriptor at its end is ready, not unready.** A pipe whose last writer has
closed reads zero and does so at once. A poll that called that "not ready" would
leave the emulator asleep for ever upon the shell it started, exactly at the
moment the shell has ended and the window should close.

**The window queue is named by a constant and not by a descriptor**, and that is
the one wart. A window's events are not a file, so there is nothing to name;
making them one would mean a kind of open file that is a queue, and nothing but
this asks for it. It is written down rather than smoothed over.

**One sleep channel, not a queue per object.** A poller sleeps upon a single
channel the scheduler owns, and every source that can make something ready wakes
that channel as well as its own: a pipe written, a pipe's last writer closed, a
window event routed. A poller woken by a source it was not watching finds
nothing and sleeps again, costing one scan of its own array. The alternative is
a waiter queue upon every object, which is what a system with many pollers needs
and which this one would be carrying for a single program.
[`../../kernel/include/oxys/proc/sched.h`](../../kernel/include/oxys/proc/sched.h)
states the cost where the channel is declared.

## 5. The terminal a shell in a window does not have

The kernel's terminal is **one** terminal: the byte stream assembled from the
keyboard and the serial line, with one foreground process group. The shell
claims that group in `ShellJobsInitialise`, before its first prompt, and cannot
know whether it is the shell a person is typing at.

That was a trap. A second shell — from the launcher, before this sub-task — took
the terminal from the first, so what a person saw upon the screen was nothing at
all while the shell they were typing at stood stopped beneath them; and before
2026-09-21 it was worse than that, because the two readers livelocked the
machine ([`SHELL.md`](SHELL.md), Section 2.6). `/etc/session.conf` carried a
paragraph telling whoever came next not to add the entry back.

**`tcgroup` is now refused to a process whose standard input is not the
terminal**, with `ENOTTY`, which is the refusal IEEE Std 1003.1-2017 gives
`tcsetpgrp` for the same reason. Descriptor 0 reaches the terminal when nothing
else is placed there, so a process with a pipe at 0 — the shell in a window — is
not at the terminal and is told so. It is a refusal and not a courtesy: it holds
whatever a program does, including a program nobody has written yet.

**The shell then runs without job control, and that is a decision and not an
oversight.** With no terminal there is no foreground group to give a job, so:

- nothing is put in a group of its own — every child stays in the shell's own
  group, which is what lets the emulator interrupt a running command by
  interrupting that group;
- `fg`, `bg` and `kill %n` say plainly that this shell has no terminal, rather
  than appearing to work and doing nothing;
- the foreground wait names the shell's own group rather than the job's leader.
  **That was found by looking**: with the wait still naming a group nothing had
  been put in, every command in the first terminal window this system opened ran
  correctly, printed correctly, and was followed by `sh: a member of the job
  could not be collected: No child to collect.`

**Control-C is a signal that the emulator sends.** Nothing in the kernel turns a
control byte in a pipe into an interrupt — that is what the terminal of Phase 8
does, and this shell has no terminal — so the emulator is the line discipline
and this is the whole of it: byte 3 becomes `kill(-group, SIGINT)`. The shell
ignores `SIGINT` and whatever it is running takes the default action, which is
the behaviour a person expects.

What this costs is stated as limitation 1: a proper pseudo-terminal, with a
foreground group of its own, is what would give the window job control, and it
is a kernel object this system does not have.

## 6. Verification

Three halves, which is what the seam buys. The grid and the key translation are
ordinary C and are asserted by giving them bytes; `poll` needs a process, a pipe
and a child, and is asserted by a program at privilege level 3; and the emulator
itself is judged by operating it, which is
[`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 11.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A grid of no columns, or one beyond the array, is refused | A terminal that thought it had more columns than it has, wrapping its lines in the wrong places for ever |
| Characters stand where they are written and the cursor advances | — |
| A line exactly as wide as the grid leaves the cursor upon its own last column | A wrap at the moment of reaching the edge rather than of writing past it: a backspace would then land in the line beneath |
| A line feed is down **and** to the left margin | The second line of every message printed under the end of the first |
| A carriage return erases nothing | A prompt redrawn over its own line would blank what it meant to replace |
| A form feed blanks every row, puts the cursor at the top left, marks every row owed a drawing, and what follows it is written from the top left | `clear` in a terminal window doing nothing — **observed**, and the reason the byte is acted upon since 2026-09-24; and a grid that blanked its rows without marking them, which would leave the window showing the page it had cleared |
| A backspace moves and does not erase; a space written over a character does | The line editor's erasure, which is a space and a backspace, would delete two characters or none |
| A backspace at the left margin **crosses** to the end of the row above, and stops at the top left | A wrapped line that can never be erased — and, without the stop, a cursor off the grid. **The same defect the console had**, [`CONSOLE.md`](CONSOLE.md), Section 7 |
| A line feed upon the last row scrolls, carries the rows up, and leaves the cursor upon the last | A terminal that stopped at the foot of its window |
| A scroll marks **every** row as owed | A screen drawn one line out of date until each row happens to be written to |
| A row said to be drawn is no longer owed, and writing one row does not mark another | Every keystroke redrawing the whole screen, twenty calls for one character |
| An escape byte is dropped and a control byte is not drawn as a glyph | A screen of ink blots where a program wrote a sequence this does not have |
| A letter becomes itself; control-C becomes byte 3 whether it arrived as `c` or `C`; control with a digit alters nothing | Control-C with capitals lock sending the letter C, so that nothing could be interrupted |
| A key that produces no character contributes no bytes | A zero byte in the stream for every shift pressed |
| **The library's sequence for each cursor key is the kernel's** | The cursor keys working at a serial line and not in a window, or the reverse — two tables, edited one at a time |
| An empty pipe is not ready; a pipe holding a byte is; the end that writes is never ready to read | A terminal that slept through its shell's output, or one that spun upon a pipe it cannot read |
| **A pipe whose last writer has gone is ready** | The emulator asleep for ever upon a shell that has ended, its window never closing. **Observed** by damaging it |
| A blocking poll returns when a child writes, and writes the readiness | A poll that slept and was never woken — which hangs the boot rather than failing it |
| A poll of two entries reports the one that is ready, against the right entry | A program told the wrong thing is ready, reading the descriptor that would block |
| A poll of nothing, of too many, of an option this kernel has not, of a descriptor not held, and of the window queue by a program with no window, are each refused | A wait that nothing could ever end |
| **`tcgroup` from a child whose standard input is a pipe is `ENOTTY`**, and from a process at the terminal is not | The shell in a window taking the terminal from the shell at the keyboard: a blank screen, a stopped shell, and — before the fix of 2026-09-21 — a stopped machine |

### 6.1 The damage applied, and what the tests said

Four, each reverted:

- **The crossing backspace removed** (`\b` at the left margin doing nothing):
  `a backspace at the left margin did not cross to the row above`.
- **A pipe at its end reported unready** (`VfsPipeIsReadable` returning the held
  count alone): `a pipe whose last writer has gone was not reported ready`, and
  two assertions after it.
- **The `tcgroup` refusal removed**: `tcgroup with a pipe upon 0 was not
  refused`, and the child's status carried it back to the parent.
- **One sequence in the library's table changed** (Home as `CSI Z`): `the
  library's sequence for a cursor key is not the kernel's, so that key behaves
  differently at a window and at a serial line`, and the assertion beneath it.

## 7. Limitations

1. **There is no pseudo-terminal.** The shell is given pipes, so it has no
   terminal, no foreground group of its own and therefore no job control:
   `fg`, `bg`, control-Z and `kill %n` are not available in a window. What would
   give them is a kernel object that is a terminal — a pair of ends with a
   foreground group and a line discipline upon the input path — which is what
   every system of this lineage has and what this one should grow when something
   beyond the shell wants one.
2. **One window, one shell.** `/bin/terminal` makes a window at start and ends
   when its shell does; there is no second tab, no second window, and no way to
   start another but to press the launcher again.
3. **No escape sequences, so no full-screen program.** Nothing that moves the
   cursor about the screen — an editor of the kind `micro` will become — can be
   run in this window. The grid would need a parser for CSI and the cursor
   movements at least; the line editor already parses that grammar upon the
   input side, [`../../libc/line/line.c`](../../libc/line/line.c), and would be
   the thing to read before writing the output side.
4. ~~**The window cannot be resized, so the grid cannot be.**~~ **Closed on
   2026-09-23**: a window made full is sent its new extent
   ([`WINDOWS.md`](WINDOWS.md), Section 13), and `/bin/terminal` gives the grid
   as many whole cells as it holds with `TermResize` — each row cut or padded,
   and rows dropped from the top only so that the cursor's row survives — and
   paints the whole content, the margin past the last cell included. The
   bounds rose to 120 columns, below the 127 characters one `window_text`
   carries, and 64 rows. **What remains is that the shell is not told**: there
   is no call by which a program learns its terminal's size, so a program
   that laid out a screen would lay it out by the size it assumed. None here
   does.
5. **There is no scrollback.** What scrolls off the top is gone. The grid holds
   what is shown and nothing behind it, and a history would be a second array
   and a way to look at it.
6. **A key typed while the shell is not reading can fill the pipe.** The
   emulator writes keystrokes with a blocking write, so a shell that never read
   and a person who typed four kilobytes would stop the emulator — and with it
   the draining of the shell's output. Keystrokes are a few bytes and the pipe
   is four kilobytes, so it has not happened; what would remove it is the
   readiness half of `poll` this call does not have.
7. **The selection, the clipboard and the pointer do nothing.** A press in the
   window raises and focuses it, as it does for any window, and nothing else.
