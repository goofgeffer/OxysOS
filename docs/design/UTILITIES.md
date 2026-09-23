<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Utilities of the Desktop: a File Manager, a Text Viewer, and a Clock

**Phase**: 9, sub-task 9.7, of [`../project/PLAN.md`](../project/PLAN.md) — the
sub-task at which `Oxys 1 Beta` is fixed, [`../project/VERSIONING.md`](../project/VERSIONING.md),
Section 11.1.

Section 1 is what this sub-task is; Section 2 the file manager; Section 3 the
text viewer; Section 4 the clock upon the panel and `/bin/date`; Section 5 the
verification; Section 6 the limitations.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../userland/files/main.c`](../../userland/files/main.c),
[`../../userland/view/main.c`](../../userland/view/main.c) and
[`../../userland/date/main.c`](../../userland/date/main.c); the clock in
[`../../userland/session/main.c`](../../userland/session/main.c). Beneath them,
the real-time clock and the `time` and `alarm` calls of
[`../devices/TIME.md`](../devices/TIME.md), Section 10, and `<time.h>` of
[`../../libc/include/time.h`](../../libc/include/time.h).

**Specifications**: ISO/IEC 9899:2011, Section 7.27, for `<time.h>`; IEEE Std
1003.1-2017, `alarm()`, `date` and `fork()`; the Motorola MC146818A data sheet
beneath the clock, cited in [`../devices/TIME.md`](../devices/TIME.md). No
specification governs a file manager or a viewer; the appearance is judged
against [`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md), Section 3.

## 1. What this sub-task is

[`../project/VERSIONING.md`](../project/VERSIONING.md) fixes `Oxys 1 Beta` at
the point the desktop has "the utilities the desktop is not usable without",
and [`../project/PLAN.md`](../project/PLAN.md) names them: a file manager, a text
viewer and a clock. Before this a person at the desktop could open a terminal
and nothing else of use; they could not see what files the machine held without
typing `ls`, read one without `cat` scrolling it past, or tell the time at all —
the kernel did not know it.

The three are ordinary programs at privilege level 3 upon the client protocol,
as the terminal is. Two things beneath them are new: **the date**, read from
the real-time clock, and **the alarm**, by which a program asleep is woken at a
time rather than by an event. [`../devices/TIME.md`](../devices/TIME.md),
Section 10, holds both.

## 2. The file manager, `/bin/files`

The launcher's `Files` entry. A window listing one directory, beginning at `/`:
the path upon the top row, the entries below it, a status upon the foot.

**Directories first, then by name as bytes** — the order `ls` gives within each,
so that a person who knows one knows the other. A directory is marked with the
slash `ls -F` would give it and nothing else is marked, a picture per type being
a picture this system does not have. `.` is left out, being this directory;
`..` is kept everywhere but at the root, where it would be the root again.

**A press selects; a press upon the row already selected opens.** That is a
double press without a clock to time one by — and the two are never confused,
because a press upon an unselected row only ever selects. The keys do the same:
the arrows, the page keys, Home and End move the selection, Enter opens and
Backspace goes up. **A directory opens in this window; a file opens in a window
of its own**, `/bin/view` started as a child and collected when it ends, so that
a person may read two files beside each other. Anything else — a device, a pipe
— is said to be unopenable upon the status row rather than handed to a viewer
that would read it forever.

**It moves, copies and deletes nothing.** `mv`, `cp` and `rm` do that at the
shell and are asserted there; a file manager that also did it would be a second
implementation of each with none of their tests, upon a ramdisk whose contents
are lost at every reboot in any case.

A directory of more than 256 entries is listed to 256 and the status says so; a
path that would pass SYSCALL_PATH_MAXIMUM is refused rather than cut, a path
silently shortened naming another file.

## 3. The text viewer, `/bin/view`

`view file` — started by the file manager, or at the shell. The file's text in
a window titled with its name, a status row upon the foot saying which rows of
how many are shown.

**What it shows.** Printable ASCII as itself; a tab to the next multiple of
eight columns; a line feed ending a row; **a carriage return as nothing**, so
that a file written upon another system shows its lines and not a column of
full stops down its right edge; every other byte as a full stop. It is a viewer
and not an editor — `micro` edits, at the shell.

**It wraps, and wraps again when the window changes.** A line cut at the edge
is a line whose end a person cannot see without a horizontal scroll, which this
does not have. A window made full is sent its new extent
([`WINDOWS.md`](WINDOWS.md), Section 13) and the text is wrapped again to it,
**with the byte at the top kept at the top**: a person who made the window full
to read more is still looking at what they were reading. The row table is as
long as the file plus two, because a file of nothing but line feeds is a row
per byte and one after the last.

**The keys** are the arrows, the page keys, Home, End, and the space bar, which
pages down as it does in every pager of this lineage. A file is read to 256 KiB;
a larger one is shown to that much and says so upon the status, the start of a
large file being more use than a refusal.

A row wider than 120 characters is drawn in pieces, because `window_text`
carries at most 127 and a row it refused would not be drawn at all.

## 4. The clock

**Upon the panel**, at its right: the hours and minutes, `HH:MM`, as the
machine's clock holds them. The list of windows of 2026-09-23 stops short of
it. **Seconds are not shown**: a panel redrawn each second is a blit each second
for a digit nobody reads.

**It is woken by an alarm.** The session asks for SIGALRM at the start of the
next minute, counted from the seconds `time` returns; the signal ends its wait
upon the window events with EINTR, and the loop draws the clock, which asks for
the next. A clock drawn upon every event would stand still upon an idle desktop;
one drawn upon a poll each second would keep a program awake that has nothing
to do.

**It turns within a few seconds of the minute.** Watched against the build host
under QEMU after `time` came to read the clock at every call, it turned two to
three seconds after the host's minute, at each of two minutes watched — the
alarm is counted by an emulated timer that runs slow, and the kernel's seconds
are whole. [`../devices/TIME.md`](../devices/TIME.md), Section 10.1, holds the
first form, which fell further behind the longer the machine ran.

**`/bin/date`** prints the same at the shell, with the seconds: `YYYY-MM-DD
hh:mm:ss`, ISO 8601's extended form rather than the POSIX default, which names a
time zone this system does not have and would have to invent.

## 5. Verification

The three programs are drawing and choice and are not asserted by a self-test
of their own; what is beneath them is, in
[`../devices/TIME.md`](../devices/TIME.md), Section 10.3 — the clock's
decoding, the arithmetic, `gmtime`, and the alarm at privilege level 3. What
only looking establishes is
[`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 11,
items 10 and 11.

**Observed**, under QEMU at 1280 by 800 on 2026-09-23, the pointer driven
through the monitor: the launcher's `Files` opened a window listing `bin/`,
`etc/`, `lost+found/`, `mnt/` and `share/` with `5 entries` upon the status;
`etc/` pressed twice listed `/etc`; `system.conf` pressed twice opened a viewer
titled `system.conf` reading `1-23 of 57`, and the status of the file manager
said `opened /etc/system.conf`; Page Down and then the full-screen control gave
the same file wrapped to the whole width, `17-43 of 43`, the row that had been
at the top still there. The panel's clock read the host's minute, and turned
over by itself at each minute watched.

## 6. Limitations

1. **The file manager changes nothing**, Section 2.
2. **No sizes, dates or kinds beyond a directory's slash.** There is no `stat`
   call; the size of a file could be had only by reading it.
3. **The viewer wraps by character and not by word**, and has no search.
4. **The viewer reads the file once.** A file changed while it is shown is not
   shown changed.
5. **The clock shows the clock's time**, with no zone — [`../devices/TIME.md`](../devices/TIME.md),
   Section 10.5, limitation 1.
6. **No icons** for `Files`; the launcher shows its name alone until one is
   drawn.
