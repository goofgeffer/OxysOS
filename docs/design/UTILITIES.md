<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Desktop Utilities: File Manager, Text Viewer, Clock, Notepad, System Info, Calculator

**Phase**: sub-task 9.7 of [`../project/PLAN.md`](../project/PLAN.md), at which
`Oxys 1 Beta` is fixed.
**Source**: [`../../userland/files/main.c`](../../userland/files/main.c),
[`../../userland/view/main.c`](../../userland/view/main.c),
[`../../userland/date/main.c`](../../userland/date/main.c),
[`../../userland/notepad/main.c`](../../userland/notepad/main.c),
[`../../userland/sysinfo/main.c`](../../userland/sysinfo/main.c),
[`../../userland/calculator/main.c`](../../userland/calculator/main.c) and
[`../../userland/calculator/engine.c`](../../userland/calculator/engine.c); the clock in
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
| Every file has the owner's file picture and every folder the folder picture, read from `/share/icons` as the window opens; where one cannot be read its rows stand without it, and a folder keeps the slash `ls -F` gives it. | A folder told from a file by nothing at all. One picture for every file, until there is a picture per kind. |
| `.` is omitted; `..` is shown except at the root. | `.` is this directory; `..` at the root is the root again. |
| A press selects; a press on the selected row opens. | A double press without a clock to time one. A press on an unselected row only selects, so the two are never confused. |
| Arrows, Page Up/Down, Home and End move the selection; Enter opens; Backspace goes up. | Every action is reachable without the pointer. |
| A directory opens in this window; a file opens in `/bin/view`, a child collected when it ends. | Two files can be read side by side. |
| A device or a pipe is reported unopenable on the status row. | A viewer handed one would read it forever. |
| It moves, copies and deletes nothing. | `mv`, `cp` and `rm` do, and are asserted doing it; a second implementation would have none of their tests. |
| A directory is listed whole, its entries grown in the heap; one the heap cannot hold is listed as far as it can, and the status says so. | A directory whose later entries are missing with nothing to say so. |
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
- **Size.** A file is read whole into the heap; where the heap refuses, the
  start is shown and the status says so, which is more use than a refusal.
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

## 4. Notepad, `/bin/notepad`

A text editor in a window, for a person at the desktop as `micro` is for a person
at the shell.

**The keys.** Printable characters are typed where the cursor stands; Enter
breaks the line; Backspace and Delete remove; Tab types four spaces. The arrows,
Home, End, Page Up and Page Down move, the column kept where a line is long
enough. Control-S saves, Control-O opens and Control-N begins a new file; the
modifier is read from the key event (`TERM_MODIFIER_CONTROL`). A file with no
name yet, and a file to open, is asked for on the bottom row, which Enter
accepts and Escape abandons. `notepad path` opens a file, or begins one of that
name where none exists.

**The text is one buffer and the cursor an offset**, and a line is found by
walking from the start whenever the window is drawn. For the 64 KiB it holds
that costs less than the drawing does, and it is one representation rather
than two kept in step. A longer file is refused at opening, not cut: saving a
file cut would lose its end.

**Saving is `micro`'s**: into a file beside the one named, which takes the name
only when whole, so a failure part way leaves the file as it was. A save posts
a notification, `Saved notes.txt.` or an error.

**Closing with changes unsaved** asks first: the first press of the close control
says so on the bottom row, and a second, with nothing typed between, discards
them.

Its icon, a pencil, is the project owner's.

## 5. System Info, `/bin/sysinfo`

A window, with the project owner's icon of a window of text, of what the machine
is and is doing:

| Row | From |
| --- | ---- |
| System | `version`, the string the banner prints. |
| Processors, memory, up for, processes | `sysinfo`: the processors online, the physical allocator's frames and those free, the interval timer's milliseconds, the processes and threads. |
| Screen | `window_screen`. |
| Date | `time`, as `date` shows it, without the seconds. |
| Entropy | `sysinfo`: the entropy pool's estimate ([`ENTROPY.md`](ENTROPY.md)). |

**`sysinfo`** is a call of its own, number 46, because the figures are the
kernel's alone and none was reachable before. It reads each counter as it
stands; they agree with each other only to within the moment it takes to read
them, which is all a window redrawn each second asks. It is open to every
process: nothing in it is another program's business in a way `ps` does not
already show.

**Redrawn every second by an alarm**, as the clock is by the minute: the alarm
ends the wait for an event with `EINTR`, so the uptime advances with no thread
and no polling.

## 6. The calculator, `/bin/calculator`

A window with a display and a grid of twenty buttons, in the project owner's
button style: the digits, `.`, `+`, `-`, `x`, `/`, `=`, `C` which clears, `+/-`
which changes the sign, `%` which divides the number shown by a hundred, and
`<-` which takes back the last digit typed. It is a desk calculator: operators
are taken left to right as they are pressed, with no precedence, so `2 + 3 x 4
=` is 20; `=` after an operator takes the number shown as its operand, so
`5 + =` is 10.

- **The keyboard is the same keys**: the digits and operators, `*` or `x`,
  Enter for `=`, Backspace for `<-`, and `c` or Escape for `C`. A press and a
  key both become one character given to `CalcKey`, so the two cannot disagree.
- **Numbers are fixed point**, a signed 64-bit count of millionths: six places
  after the point and twelve before it. Every program here is built without the
  floating-point unit, whose state the kernel does not save across a switch.
  So 0.1 + 0.2 is 0.3 exactly; a quotient is carried to six places and the last
  rounded half away from zero; a product is formed in parts so that no step
  leaves 64 bits.
- **A result past twelve digits is `Overflow`**, and a division by zero
  `Cannot divide by zero`, shown in place of the number until the next key,
  never a wrapped number shown as if it were right. A thirteenth digit before
  the point, or a seventh after it, is not taken.
- **The number is shown as typed** while it is typed, `3.` and `3.50`, and a
  result without trailing zeros. A number too wide for the display at twice the
  scale is drawn at the scale, so it is shown whole.
- **The arithmetic is apart from the window**:
  [`../../userland/calculator/engine.c`](../../userland/calculator/engine.c)
  calls nothing, and is compiled into the kernel image as the shell's grammar
  is, so `KernelVerifyCalculator` asserts the code the calculator ships.

The launcher offers it with no icon of its own: it stands as its letter until
`/share/icons/calculator.oxi` is drawn ([`SESSION.md`](SESSION.md)).

## Verification

The programs are drawing and choice and have no self-test of their own, save
the `sysinfo` call beneath System Info, which `window-check` asserts, and the
calculator's arithmetic and keys, which `KernelVerifyCalculator`
([`../../kernel/test/calculator/engine.c`](../../kernel/test/calculator/engine.c))
asserts:

| Asserted | The failure it would catch |
| -------- | -------------------------- |
| 0.1 + 0.2 is 0.3; 7 / 2 is 3.5, 1 / 3 and 2 / 3 carried to six places and rounded; products' signs and sizes, 999999 x 999999 included. | Floating-point error; a truncated or wrongly rounded place; a product that left 64 bits. |
| A sum or a product past twelve digits is `Overflow`, a division by zero refused by name. | A wrapped number shown as a result. |
| Values written with no trailing zeros and no point where there is none. | `3.500000` or `12.` on the display. |
| Keys as pressed: `12+7=`, `7/2=`, `.1+.2=`, `2+3*4=` as 20, `5+=` as 10, `3.50` as typed, backspace, the sign into a sum, percent, a thirteenth digit refused, a division by zero shown and cleared by the next key. | A calculator that disagrees with the one on a desk. |
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
| Notepad types, moves, and saves two lines with Control-S to a path asked for, which `cat` then shows as typed, and a notification says so. | A save that writes nothing, or writes the wrong bytes. |
| System Info shows the version, two processors under QEMU, memory free less than memory, and an uptime that advances by itself. | Figures read from the wrong counter; an alarm not re-armed. |
| The calculator opens from the launcher; pressing `12+7=` shows 19, and typing `7/2` and Enter shows 3.5. | Buttons or keys not reaching the arithmetic. |

## Limitations

1. The file manager copies, moves and deletes nothing.
2. No sizes, dates or kinds beyond a directory's slash: there is no `stat`
   call.
3. The viewer wraps by character, not by word, and has no search.
4. The viewer reads the file once; a change while it is shown is not shown.
5. The clock has no time zone ([`../devices/TIME.md`](../devices/TIME.md)).
6. Notepad has no undo, no search, no selection and no clipboard, and does not
   wrap long lines; it scrolls sideways instead.
7. System Info names no processor model and no disk: there is no call for
   either.
8. The calculator has no memory keys, no square root and no precedence of
   operators, and numbers are bounded at twelve digits before the point and six
   after it.
