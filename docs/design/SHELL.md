<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Shell

**Phase**: 8 of [`../project/PLAN.md`](../project/PLAN.md). This document is
Phase 8's, as [`LIBC.md`](LIBC.md) is Phase 7's: one section per sub-task, in
order, each recording what that sub-task built and why, and each revised as the
design is. Sub-task 8.1 is the whole of it so far.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every control
sequence named below carries a citation, and the two documents cited are
registered in [`../project/REFERENCES.md`](../project/REFERENCES.md).

**Implementation of sub-task 8.1**: the terminal is
[`../../kernel/terminal/terminal.c`](../../kernel/terminal/terminal.c) behind
[`../../kernel/include/oxys/terminal/terminal.h`](../../kernel/include/oxys/terminal/terminal.h),
reached by `SyscallDoRead` in
[`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c);
the line editor is [`../../libc/line/line.c`](../../libc/line/line.c) and
[`../../libc/line/system.c`](../../libc/line/system.c) behind
[`../../libc/include/line.h`](../../libc/include/line.h); the shell is
[`../../userland/sh/main.c`](../../userland/sh/main.c), started by
`KernelRunShell` in [`../../kernel/kernel.c`](../../kernel/kernel.c); and it is
asserted by [`../../kernel/test/terminal/terminal.c`](../../kernel/test/terminal/terminal.c),
[`../../kernel/test/libc/line.c`](../../kernel/test/libc/line.c) and
[`../../userland/line-check/main.c`](../../userland/line-check/main.c).

## 1. Sub-task 8.1: what it is, and what it is not

The plan's line for it is "line editing with history", and that is what a
person sees: a prompt, a line that can be edited with the cursor keys, Home,
End, Delete, Backspace and the control characters every shell of this lineage
accepts, and a history the arrow keys walk. What the line names is a program —
`/bin/sh`, upon the initial ramdisk of sub-task 7.7, started by the kernel when
the boot finishes — and what the plan's line does not name is the thing that had
to exist before any of it could: **a way for a program to read what a person
types.**

Until this sub-task there was none. `stdin` was a stream permanently at
end-of-file, `read` reached files alone, and the only consumer of the keyboard
was the kernel's own echo loop. So the sub-task is three things, in the order
they depend upon each other:

1. **The terminal** — one byte stream, assembled in the kernel from the keyboard
   and the serial line, that `read` of descriptor 0 delivers. Section 2.
2. **The line editor** — the code in the C library that turns those bytes into a
   line, redraws the display as it goes, and keeps the history. Section 3.
3. **The shell** — the program that prompts, reads a line through the editor,
   and, there being no tokeniser until 8.2, hands the line back. Section 4.

What it is not: a shell. Nothing is parsed, expanded, or run. The shell of 8.1
answers every line with a statement that it cannot run it, and it is shipped in
that state — rather than held back until 8.4, when something could run — because
it is the first thing upon this system that a person can type at and be answered
by, and a property like that is worth having on the day it exists.

## 2. The terminal

### 2.1 Raw, and why

The terminal delivers every byte as it arrives, echoes nothing, and assembles
nothing. A program that reads it gets the keystrokes. This is what a Unix
terminal in *raw* (non-canonical) mode does, and it is the only arrangement
under which a line editor can work at all: an editor that moves the cursor needs
to see the arrow key when it is pressed, not after a newline.

The alternative, *canonical* mode — the kernel assembling lines, handling
backspace, echoing, and delivering a line whole — is what every Unix has as its
default, and it is the arrangement under which `cat` with no operand and a
program that calls `fgets` behave as a person expects. It is deliberately not
built here, and the reason is the one this project gives for everything it
defers: nothing yet wants it, and a line discipline nobody calls is a line
discipline nothing asserts. Section 6 records what it would cost. What `fgets`
upon `stdin` does today is deliver the raw bytes, control sequences and all,
which is correct and is not what a person would call friendly.

### 2.2 One stream, and the translation that makes it one

Two devices produce input: the PS/2 keyboard, whose driver produces *key events*
— a scancode, the character the key would produce under the modifiers in force,
and whether it was pressed or released — and the serial adapter, whose driver
produces *bytes*. A program cannot be asked to read two things, so the terminal
merges them, and the merge has to decide what a cursor key becomes.

**It becomes what a terminal would send.** ECMA-48, Section 5.4, defines a
control sequence as CSI (ESC `[`) followed by parameter bytes and a final byte,
and Sections 8.3.18 to 8.3.22 assign the final bytes D, B, C and A to the four
cursor movements CUB, CUD, CUF and CUU. Every terminal emulator sends those for
the arrow keys, and xterm's *Control Sequences* document records the rest of
what the keys this editor cares about send: Home and End as CSI H and CSI F (or
SS3 H and SS3 F in application mode, or CSI 1 ~ and CSI 4 ~ upon a VT220), and
Delete as CSI 3 ~. So the keyboard's up-arrow event becomes the three bytes
ESC `[` A, and a program reading the terminal sees exactly what it would see if
the person were at the far end of a serial line.

The other choice — a private code of this kernel's own for each key — was
refused because a serial terminal sends what it sends. The editor would have had
to understand the terminal's dialect anyway, so a private dialect would have
been a second one for it to parse and a convention only this kernel's programs
knew. One dialect, and the terminal's, means one parser and one behaviour.

**The control key is collapsed here too.** The keyboard driver reports control
and `a` as the character `a` with the control flag set — which is the right
record for the window system of Phase 9, which will want the key — and the
terminal turns it into byte 1, which is what a terminal sends. The driver stays
a decoder of scan code set 1 and nothing more.

**What becomes nothing.** A release. A key with no character and no assigned
sequence — a function key, a modifier. And an extended key that shares an
ordinary key's code, such as the keypad's Enter, which the keyboard driver
already refuses to look up in the ordinary table. The alternative, delivering
*some* byte for every key, would put bytes the editor did not ask for into a
line the person did not see.

### 2.3 Where the bytes wait, and how a program waits for them

The terminal keeps a queue of a kibibyte, filled by *polling* the two drivers'
own buffers when a reader asks and not by their interrupt handlers. Each
driver's buffer is a single-producer, single-consumer ring that is correct
without a lock; a queue written by two interrupt handlers and read by a system
call would be three parties and a lock, for no gain — the only thing that could
observe a byte arriving later than it might is the reader, and the reader is
what does the polling.

**A `read` of descriptor 0 waits.** It halts the processor, interrupts enabled,
until at least one byte is queued, then delivers what is queued up to the length
asked for — never nothing, so that a program can tell the terminal from a file
at its end by the one property that distinguishes them. The wait is `sti; hlt`
in a loop, the idiom the kernel's echo loop records and for the same reason: the
enable takes effect after the halt has been entered, so a keystroke cannot fall
between the two and leave the processor halted with nothing to wake it.

That a system call halts the processor is worth stating plainly, because it is
only right under a condition that holds today and will not always. Every user
program runs upon the bootstrap processor, upon that processor's own flow of
control — `ThreadStart` does not return until the program ends — and there is
one program at a time. So the processor that halts has nothing else to do, and
the interrupt that wakes it is the one the program was waiting for. When there
are two programs, a `read` that halts the processor stops both; the right shape
then is a wait queue the scheduler can block a thread upon, which
[`SCHEDULER.md`](SCHEDULER.md), Section 9, limitation 8, already records as
absent. Section 6 counts it.

**The queue is bounded and the bound discards the newest.** For the reason the
keyboard driver gives: the first bytes typed are the beginning of a line, and a
queue that dropped from the front would silently rewrite text a program had not
yet read. The discard is counted and the report shows it.

### 2.4 `stdin`, and the one function that changed

[`LIBC.md`](LIBC.md), Section 10.2, divided the stream machinery into a policy
and two transfers, and said of the second — `OxysStreamFill`, which returned
end-of-file because there was no call that reads — that "the day a read call
exists, one function of six lines changes and every program above it keeps
working." That day was this sub-task, and that is what happened: the body of
`OxysStreamFill` is now a call of `OxysRead`, and nothing above it changed.

What did change beside it is where `stdin` may be *read*. The kernel's stdio
self-test read it, to assert that it reported an end rather than an error, and
so did `startup-check`; both now would execute `SYSCALL` — the first fatally,
the second waiting for a person in the middle of `make verify` — and both stop.
The end-of-file discipline they asserted is asserted upon a memory stream
instead, which is the same policy above a different source, and the census the
stdio test ends by examining now requires both transfer counters to be zero
rather than one.

## 3. The line editor

### 3.1 What the keys do

The whole table is in [`../../libc/include/line.h`](../../libc/include/line.h),
at `LineFeed`, and is not repeated here. In summary: printable bytes insert at
the cursor; CR and LF complete the line — both, because a serial terminal sends
the first and the keyboard driver the second, and a program should not know
which it is attached to; BS and DEL erase before the cursor — both, because
IEEE Std 1003.1-2017 leaves the erase character to the terminal and the two
families of terminal chose differently; the cursor keys, Home, End and Delete
act in every form the references of Section 2.2 record; and the control
characters of the Emacs binding that every shell of this lineage accepts —
A, E, B, F, P, N, U, K, D — do what they do everywhere.

Control-D upon an empty line is the end of input. Upon a line that is not empty
it deletes the character under the cursor, as it does everywhere, so that a
person who presses it by habit in the middle of a line does not lose the shell.

Everything else is ignored and counted — a tab, a function key's sequence, an
escape the editor does not know — and *ignored* means the line and the display
are untouched, which the self-test asserts.

### 3.2 The redraw discipline: one assumption of the display

Everything the editor draws, it draws with printable characters, spaces and the
backspace character, and the only thing it assumes of the display is that a
backspace moves the cursor one position left without erasing. That is what a
serial terminal does, what this kernel's text-mode display does, and what its
framebuffer console does. None of the three is asked to interpret a control
sequence, so the editor draws correctly upon a display that interprets none —
which all three of this kernel's are.

So an insertion in the middle of a line writes the new character and every
character after it, then backspaces over the ones after it. A deletion writes
every character after the gap, then a space to blank the position the line no
longer reaches, then backspaces over all of that. A recall from the history
backspaces to the start, writes the recalled line over the old one, blanks
whatever the old one extended beyond it, and backspaces over the blanks. Moving
right is *writing the character under the cursor*, which every display advances
past — there is no other way to move right with a backspace alone, and it
redraws what is already there.

The price is that the editor cannot move the cursor *up*, so a line longer than
the display is wide is drawn wrongly once it wraps: the backspaces reach the
left margin and stop, upon a serial terminal, while the text continued on the
row below. Section 6 records it. It is a smaller limitation than depending upon
the display's dialect would have been, and it is the limitation a later console
can lift by interpreting CUU without anything in the editor changing.

### 3.3 The history

Thirty-two lines, a ring, the thirty-third displacing the oldest. A line is
remembered by a separate call and not upon completion, because whether a line
is worth remembering is the caller's decision — a shell that asked for a
password would not want it recalled by an arrow key — and the shell remembers
every non-empty line. An empty line is never remembered, and neither is a line
equal to the newest entry, so that pressing return upon a blank prompt or
running the same command twice does not fill the history with that.

Recall keeps the *draft*: the line being typed when the person first pressed
up is saved, and stepping down past the newest entry restores it. A recalled
line that is edited and entered becomes a new entry and the original stands.

The editor is some seventeen kibibytes and is declared with static storage
duration by every program that uses one, for the reason the header gives: a
prompt must work before a heap exists, and a shell whose line editor could fail
for want of memory would be a shell that could not report the failure.

### 3.4 The division, made a third time

[`LIBC.md`](LIBC.md), Sections 9.1 and 10.2, divided the heap and the streams
into a *policy* that runs anywhere and a *transfer* that executes `SYSCALL`,
so that the policy could be asserted by the kernel and the transfer by a
program. The editor is divided the same way and for the same reason: the
editing, the parsing and the history take an *output function* and never name
a descriptor, and `LineRead` — the one function that does — is a translation
unit of its own above `OxysRead` and `OxysWrite`.

**This is the first thing in this project built so that what a program prints
can be asserted.** [`LIBC.md`](LIBC.md), Section 12.7, limitation 1, records
that nothing in the kernel can read what a program wrote to the diagnostic
path, so a program whose whole output is text proves only that it did not
fault. The editor's output goes through a function it is given, and the
kernel's self-test gives it one that appends to an array; so the bytes an
editing key produces — the character echoed, the tail redrawn, the backspaces
that return the cursor — are compared byte for byte. Section 5 records the one
defect class this catches that nothing else could: a redraw that leaves the
line right and the screen wrong.

## 4. The shell at sub-task 8.1

It prompts with `oxys$ `, reads a line through `LineRead`, remembers it if it
is not empty, and prints `sh: no tokeniser yet, so nothing runs:` followed by
the line. Control-D upon an empty line ends it, and there is no `exit` word:
that is a built-in of sub-task 8.3, and a word recognised specially here would
be the beginning of a parser written in the wrong file.

**The kernel starts it** when the boot finishes, where there is a root to read
it from and a keyboard or a serial adapter to type at. It is started again when
it ends at the end of its input, because the alternative is a machine that
halts the first time somebody presses control-D; it is *not* started again when
it ends any other way, because a shell that failed at once would be started at
once, for ever, and the log would be that. Where there is no root — a boot
whose ramdisk was not found — the echo loop of Phase 3 remains, as the
demonstration of the interrupt path it always was.

`/bin/sh` is upon the ramdisk beside the five utilities, and it is embedded in
the kernel image as they are, so that the ramdisk self-test compares it byte
for byte against the copy that was built. [`../storage/INITRD.md`](../storage/INITRD.md),
Section 2.

## 5. Verification

Three self-tests, and each catches something the other two cannot.

### 5.1 The terminal, from scancodes

`kernel/test/terminal/terminal.c` drives the keyboard decoder with scancodes,
as the keyboard's own self-test does, and reads the bytes the terminal delivers.

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| A key that produces a character becomes that byte alone; a release becomes nothing. | A terminal that delivered a byte per release would double every keystroke. |
| Enter is LF and Backspace is BS. | The editor completes upon LF and erases upon BS; a driver table edit that changed either would break both silently. |
| Control with a letter is the control character, with a digit is the digit, and with shift as well is still the control character. | A translation that applied the mask to every byte would turn control-1 into byte 17; one that applied it only to lower case would make control-shift-A a capital A. |
| A letter typed after control is released is a letter. | A translation that kept the modifier would turn every later keystroke into a control character. |
| The seven extended keys become the seven sequences, in order, and are counted as seven. | A sequence with the wrong final byte moves the cursor the wrong way; the count shows a key that was silently dropped. |
| A function key, and an extended key sharing an ordinary key's code, become nothing. | A byte the editor did not ask for, in a line the person did not see. |
| Injected bytes are delivered in order and in the counts asked for; a short read leaves the rest. | A ring whose read index was advanced by the capacity asked for rather than the count delivered would lose bytes upon every short read. |
| The queue fills to its capacity and stops, the excess is counted, and the first capacity's worth is what survives. | A ring without a bound would overwrite bytes a program had not read; one that dropped the oldest would rewrite the beginning of the line. |
| A flush empties the queue and the devices beneath it. | A keystroke from the boot, read back as the answer to a prompt. |

### 5.2 The editor, captured

`kernel/test/libc/line.c` drives the library's own editing code with the bytes a
terminal sends and captures what it writes.

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| Insertion at the end writes the character and nothing else; insertion in the middle writes the tail and backspaces over it, and the line and the cursor agree with the display. | The one this test exists for: a redraw that leaves the line right and the screen wrong. The negative test of Section 5.4 removed the backspaces and `line-check` still passed. |
| Backspace and DEL erase before the cursor, redraw the tail, blank the vacated position and return; at the start of the line they do nothing, write nothing, and are counted. | An erase at the start that consumed the prompt, or one that left the last character standing upon the display. |
| CSI 3 ~ and control-D upon a non-empty line delete under the cursor; control-D upon an empty line ends the input and writes a line feed. | A control-D that ended the shell in the middle of a line. |
| Home and End in every form the references record, and control-A, E, B, F; right at the end moves nothing and writes nothing. | A form one terminal sends and the editor does not accept, which looks like a key that does nothing. |
| Control-K blanks the removed text; control-U redraws the kept text over the removed and blanks the rest. | A kill that removed text from the line and left it upon the screen. |
| An unknown escape, a malformed sequence, an unassigned final byte and a tab are each ignored once, and none touches the line or the display. | A parser that inserted the bytes of a sequence it did not know as text. |
| CR and LF each complete the line and write exactly one line feed; the line survives completion for the caller. | A shell that completed upon the serial line's return and not the keyboard's, or the reverse. |
| The five hundred and twelfth character is discarded, counted, and not written. | A line that overran its array, or one whose display showed a character the line did not hold. |
| An empty line is not remembered; a repeat of the newest is not; two lines are held in order; a position beyond the count is null. | A history filled with blank lines. |
| Up keeps the draft and recalls the newest; up again the older; up past the oldest is ignored; down returns; down past the newest restores the draft with the cursor at its end; the redraw of a recall blanks the excess of a longer line. | A draft lost by looking at the history; a recalled shorter line with the tail of the longer one still upon the screen. |
| A recalled line edited and entered is a new entry and the original stands; control-P and N are the arrows. | A history whose entries changed when they were recalled. |
| Thirty-three entries leave thirty-two, the oldest displaced, the order kept, and a walk up through them reaches the oldest; a remembered line is copied and not referenced. | A ring that turned in the wrong place, or an entry that changed when the next line was edited. |

### 5.3 The program, through descriptor 0

The same test then places a session of seventy-one bytes upon the terminal's
queue — every family of key used at least once, so that a sequence delivered
one byte to a call is seen to survive the parser's state between calls — and
runs `line-check` at privilege level 3. The program reads the session through
`LineRead`, which is `OxysRead` of descriptor 0 one byte at a time, compares the
five lines the session edits into, compares the four lines the history should
then hold, and ends with the number that differed. The kernel then asserts that
exactly the session was consumed: a reader that took bytes ahead of need, or
left some behind, leaves the count wrong in one direction or the other.

**What nothing asserts**: that the `read` *waits*. The session is placed before
the program starts, so the wait is never entered under `make verify`. It is
exercised by every interactive session — the rows of
[`../project/TESTING-RECORD.md`](../project/TESTING-RECORD.md) for this
sub-task were made by typing at the shell under QEMU over the serial line and
under VirtualBox at the PS/2 keyboard — and it is the one property of this
sub-task that a person sees and no assertion does.

### 5.4 The negative tests, and the one that found the point

Three defects were inserted deliberately, each observed to be caught, and each
reverted.

| Defect inserted | Caught by | What it showed |
| --------------- | --------- | -------------- |
| The right-arrow key translated to the left-arrow's sequence. | The terminal test's assertion upon the seven sequences in order. | — |
| An insertion that redraws the tail and does not backspace over it. | The editor test's captured output alone. **`line-check` passed** — the line was right and only the screen was wrong. | That the captured output is not a nicety: it is the only assertion in this project that can see the class of defect a person would see first. |
| The history recording a repeated line twice. | Eight assertions of the editor test, and `line-check`'s count of the history. | — |

## 6. Limitations

1. **The terminal is raw and there is no canonical mode.** `fgets` upon `stdin`
   delivers keystrokes, control sequences and all, unechoed; `cat` with no
   operand still reports that it cannot copy the standard input, which is now a
   false statement about the call and a true one about the result a person
   would get. A line discipline in front of the terminal — echo, erase, kill,
   and delivery by the line — is what would close it, and `tcgetattr` and
   `tcsetattr` of IEEE Std 1003.1-2017, Section 11, are the interface a program
   would switch it with. Nothing yet wants it; the shell wants raw.
2. **A `read` of the terminal halts the processor.** Correct while one program
   runs at a time upon the bootstrap processor's own flow of control, and wrong
   the moment there are two; Section 2.3. It needs the wait queue
   [`SCHEDULER.md`](SCHEDULER.md), Section 9, limitation 8, records as absent,
   and it is the first thing in this project that would use one.
3. **A line longer than the display is wide is drawn wrongly once it wraps.**
   Section 3.2. The line itself is right; the display of it is not, upon a
   serial terminal, and is right upon this kernel's own two consoles only
   because their backspace crosses a row boundary. It needs the editor to move
   the cursor up, which needs a display that interprets CUU, which none of the
   three do.
4. **Nothing interrupts a program.** Control-C is byte 3 and is ignored by the
   editor; a program that does not return to the prompt cannot be stopped from
   the keyboard. That is sub-task 8.7's terminal signal delivery, and there is
   nothing to interrupt before 8.4 runs a program.
5. **The two devices are merged without an order between them.** The keyboard
   is drained before the serial line at each poll, so bytes from one device are
   in the order they were typed and bytes from the two are not. Two people
   typing at once upon two devices have no expectation the machine could meet.
6. **The serial path is exercised by a person and not by an assertion.** The
   serial self-test cannot make a character arrive and neither can the
   terminal's; the four lines of `TerminalPoll` that drain the receive buffer
   are seen to work in every interactive session over the serial line and
   asserted by nothing.
7. **No tab completion, no search, and no editing of a previous line in place**
   — the history is a list the arrow keys walk. Each is a feature and not a
   defect; none is wanted before there are commands to complete.
8. **None of this is synchronised.** The terminal's queue is touched by the
   one flow of control that reads it, which is the property that makes it
   correct without a lock, and [`CONCURRENCY.md`](CONCURRENCY.md), Section 10,
   limitation 1, counts it with the rest.

## 7. What a person sees

Under QEMU with `-serial stdio`, or under VirtualBox at the keyboard, after the
boot log:

```
The Oxys-OS shell, sub-task 8.1: a prompt and a line editor. Arrow keys edit and recall;
control-D upon an empty line ends the shell.
oxys$ echo hello
sh: no tokeniser yet, so nothing runs: echo hello
oxys$
```

Typing `wrold`, pressing Home, Right, Delete, Right, `r`, End and Return
produces `sh: no tokeniser yet, so nothing runs: world`; pressing Up recalls
it. Control-D upon an empty line prints `sh: end of input.`, the kernel reports
that the shell ended at the end of its input, and starts it again.
