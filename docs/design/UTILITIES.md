<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Desktop Utilities: File Manager, Text Viewer, Clock

**Phase**: sub-task 9.7 of [`../project/PLAN.md`](../project/PLAN.md), at which
`Oxys 1 Beta` is fixed.
**Source**: [`../../userland/files/main.c`](../../userland/files/main.c),
[`../../userland/view/main.c`](../../userland/view/main.c),
[`../../userland/date/main.c`](../../userland/date/main.c); the clock in
[`../../userland/session/main.c`](../../userland/session/main.c).
**Specifications**: ISO/IEC 9899:2011, Section 7.27 (`<time.h>`); IEEE Std
1003.1-2017, `alarm()`, `date`, `fork()`. The appearance is judged against
[`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md).

The three programs without which the desktop is not usable: a file manager, a
text viewer and a clock. Each is an ordinary program at privilege level 3 upon
the client protocol of [`WINDOWS.md`](WINDOWS.md). The date and the alarm
beneath the clock are [`../devices/TIME.md`](../devices/TIME.md).

## 1. The file manager, `/bin/files`

A window listing one directory, starting at `/`: the path on the top row, the
entries below, a status row at the foot. The launcher's `Files` entry starts it.

| Rule | Reason |
| ---- | ------ |
| Directories first, then by name as bytes. | The order `ls` gives, so that knowing one is knowing the other. |
| A directory is marked with a slash, as `ls -F` marks it; nothing else is marked. | The system has no picture per file type. |
| `.` is omitted; `..` is shown except at the root. | `.` is this directory; `..` at the root is the root again. |
| A press selects; a press on the selected row opens. | A double press without a clock to time one. A press on an unselected row only selects, so the two are never confused. |
| Arrows, Page Up/Down, Home and End move the selection; Enter opens; Backspace goes up. | Every action is reachable without the pointer. |
| A directory opens in this window; a file opens in `/bin/view`, a child collected when it ends. | Two files can be read side by side. |
| A device or a pipe is reported unopenable on the status row. | A viewer handed one would read it forever. |
| It moves, copies and deletes nothing. | `mv`, `cp` and `rm` do, and are asserted doing it; a second implementation would have none of their tests. |
| A directory is listed to 256 entries, and the status says so beyond that. | The list is a fixed array. |
| A path longer than `SYSCALL_PATH_MAXIMUM` is refused, not cut. | A path silently shortened names another file. |

## 2. The text viewer, `/bin/view`

`view FILE` shows the file's text in a window titled with its name, with a
status row saying which rows of how many are shown. It is started by the file
manager or at the shell. It views; `micro` edits.

- **Characters.** Printable ASCII as itself; a tab to the next multiple of eight
  columns; a line feed ends the row; a carriage return is nothing, so that a
  file from another system shows its lines rather than a column of full stops;
  any other byte is a full stop.
- **Wrapping.** Rows are wrapped at the window's edge, because the viewer has no
  horizontal scroll. When the window's extent changes (made full or restored,
  [`WINDOWS.md`](WINDOWS.md)) the text is wrapped again and the byte at the top
  stays at the top, so the reader is still looking at what they were reading.
  The row table holds the file's length plus two entries: a file of line feeds
  is a row per byte and one after the last.
- **Keys.** Arrows, Page Up/Down, Home, End, and the space bar, which pages down.
- **Size.** A file is read to 256 KiB; beyond that the start is shown and the
  status says so, which is more use than a refusal.
- **Long rows.** A row wider than 120 characters is drawn in pieces, because
  `window_text` carries at most 127 characters and refuses a longer string.

## 3. The clock

**In a box of its own at the top right** of the screen, a fifth of its width, as
`HH:MM`. Seconds are not shown: a clock
redrawn every second is a blit every second for a digit nobody reads.

**Woken by an alarm.** The session asks for `SIGALRM` at the start of the next
minute, counted from the seconds `time` returns. The signal ends the session's
wait for window events with `EINTR`; the loop draws the clock and asks for the
next alarm. A clock drawn on every event would stand still on an idle desktop;
one polled each second would keep an idle program awake.

**Accuracy.** Under QEMU the clock turns two to three seconds after the host's
minute: the alarm is counted by an emulated timer that runs slow, and the
kernel's seconds are whole. `time` reads the real-time clock at every call, so
the error does not grow with uptime ([`../devices/TIME.md`](../devices/TIME.md)).

**`/bin/date`** prints the same with seconds, `YYYY-MM-DD hh:mm:ss`: ISO 8601's
extended form rather than the POSIX default, which names a time zone this
system does not have.

## Verification

The three programs are drawing and choice, and have no self-test of their own.
What they stand on is asserted in [`../devices/TIME.md`](../devices/TIME.md):
the clock's decoding, the date arithmetic, `gmtime`, and the alarm at privilege
level 3. The checks a person performs are in
[`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md).

| Property checked by looking | The failure it would catch |
| --------------------------- | -------------------------- |
| `Files` lists `/` with directories first and the entry count on the status. | A wrong order or a lost entry. |
| Two presses on a directory list it; on a file, open a viewer titled with its name. | Selection and opening confused. |
| Making a viewer full rewraps the text with the top row unchanged. | A resize that loses the reader's place. |
| The panel's clock matches the host's minute and turns by itself. | An alarm that is not re-armed. |

## Limitations

1. The file manager copies, moves and deletes nothing.
2. No sizes, dates or kinds beyond a directory's slash: there is no `stat`
   call.
3. The viewer wraps by character, not by word, and has no search.
4. The viewer reads the file once; a change while it is shown is not shown.
5. The clock has no time zone ([`../devices/TIME.md`](../devices/TIME.md)).
