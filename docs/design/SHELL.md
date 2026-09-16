<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Shell

**Phase**: 8 of [`../project/PLAN.md`](../project/PLAN.md). This document is
Phase 8's, as [`LIBC.md`](LIBC.md) is Phase 7's: one section per sub-task, in
order, each recording what that sub-task built and why, and each revised as the
design is. Sub-tasks 8.1 (Sections 1 to 7), 8.2 (Sections 8 to 10) and 8.3
(Sections 11 to 15) are here so far.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every control
sequence and every rule of the grammar named below carries a citation, and the
documents cited are registered in
[`../project/REFERENCES.md`](../project/REFERENCES.md).

**Implementation of sub-task 8.1** (8.2's is at Section 8): the terminal is
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

## 4. The shell at sub-task 8.1 (superseded by Section 8.5)

At 8.1 it prompted with `oxys$ `, read a line through `LineRead`, remembered
it if it was not empty, and printed `sh: no tokeniser yet, so nothing runs:`
followed by the line. Since 8.2 it parses the line and describes the structure;
Section 8.5. Control-D upon an empty line ends it, and there is no `exit` word:
that is a built-in of sub-task 8.3, and a word recognised specially here would
be the beginning of a parser written in the wrong file.

**The kernel starts it** when the boot finishes, where there is a root to read
it from and a keyboard or a serial adapter to type at. It is started again when
it ends by its own choice — control-D, or since 8.3 `exit [n]`, whatever the
number — because the alternative is a machine that halts the first time
somebody presses control-D; it is *not* started again when it ends by a fault,
because a shell that faulted at once would be started at once, for ever, and
the log would be that. (Until 8.3 any non-zero status stopped the restart,
which `exit 9` showed to be wrong: a status the shell chose is an ending.)
Where there is no root — a boot
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

## 8. Sub-task 8.2: the tokeniser and the parser

**Implementation**: [`../../userland/sh/shell.h`](../../userland/sh/shell.h),
[`../../userland/sh/lexer.c`](../../userland/sh/lexer.c) and
[`../../userland/sh/parser.c`](../../userland/sh/parser.c), used by
[`../../userland/sh/main.c`](../../userland/sh/main.c); asserted by
[`../../kernel/test/shell/parser.c`](../../kernel/test/shell/parser.c).

### 8.1 What of the grammar is here

IEEE Std 1003.1-2017, Section 2.10, gives the grammar of the shell language,
and this sub-task implements the part of it that sub-tasks 8.3 to 8.7 act
upon: a `list` of `and_or`s separated by `;` and `&`, each an `and_or` of
`pipeline`s joined by `&&` and `||`, each pipeline a `!`-negatable sequence of
`simple_command`s joined by `|`, and each simple command its words and its
redirections — every operator of Section 2.7 with its `io_number`, the
here-document excepted. The tokeniser is Section 2.3's ten rules, the quoting
is Section 2.2's three forms, and every one is cited at the code that
implements it.

**What is outside the subset is refused by name, and that is a design decision
and not a gap.** A compound command — `if`, `while`, `for`, `case`, `{ }`,
`( )` — a function definition, a here-document and `;;` are each recognised as
what they are and answered `not implemented by this shell near `if'`. The
alternative, a parser that did not know the word `if`, would have parsed
`if true` as a simple command naming a program called `if`, and the failure
would have looked like a missing program. The reserved words are recognised
only in command position, as Section 2.10.2, rule 1, requires, so `echo if`
is the word `if`.

**Nothing is expanded.** Section 2.3, rule 5, has the tokeniser recognise the
extent of a `$` or backquote expansion; no expansion is performed at this
sub-task, so a `$` is an ordinary character of a word. The expansions of
Section 2.6 arrive with the environment they expand from, and Section 10
counts what the tokeniser will then have to learn.

### 8.2 The tokens keep their quotes

Section 2.6 orders the expansions before quote removal, and the expansions
must know which characters were quoted: `"$HOME"` expands and `'$HOME'` does
not. A tokeniser that removed the quotes as it went would have been simpler,
and would have had to be rewritten one sub-task later, with its assertions.
So a token is the characters of the line, quotes and all, and `ShellUnquote`
— Section 2.6.7 — is applied last, by whoever wants the string: the shell's
description at this sub-task, and the argument vector at 8.4.

### 8.3 The structure, and why it is flat

The parser builds a `ShellList`: pipelines in order, each carrying the
*condition* under which it follows the one before (`&&`, `||`, or none) and
the *separator* that follows it (`;`, `&`, or none), each pipeline its
commands, and each command its words and redirections in the order written. A
tree — a list of and_ors of pipelines — would have said the same thing in more
structure than a shell that runs left to right needs; what 8.4 needs to know
of a pipeline is what came before it and what follows it, and both are one
field.

**Nothing here allocates**, for the reason the line editor does not: a shell
that could not parse a line for want of memory could not report the failure.
Every bound is a number in `shell.h` — sixteen words to a command, which is
the argument vector's own bound; eight redirections; eight commands to a
pipeline; sixteen pipelines to a line; a hundred and twenty-eight tokens — and
a line that exceeds one is refused with a status naming which.

### 8.4 A line that continues

A quote left open, a `|`, a `&&` or `||` with nothing after it, or a
redirection with no target, is not a syntax error: it is a command that is not
finished, and Section 2.10's `linebreak` after those operators is where the
grammar says so. The tokeniser and the parser both report `INCOMPLETE`, and
the shell prompts with `> ` — PS2 — and appends the next line with the newline
between, which is a character inside a quote and a blank outside one. The
whole is then parsed again from the start. A parser that resumed from the
middle would have been a second parser to assert.

Each line is remembered in the history on its own, as typed. A command of
several lines recalled as one would not fit the editor's line, and a recalled
continuation is a thing a person may want.

### 8.5 The shell at sub-task 8.2

It reads a command as above, and — there being nothing yet that runs one —
describes what it understood: one line per pipeline, each word in brackets
with its quotes removed, each redirection as its descriptor and operator and
target, the condition before and the separator after. A refused line is
answered upon the standard error with the status and the token it stopped at:

```
oxys$ echo hello world >out 2>&1 | wc -l ;
sh: parsed 1 pipeline(s); nothing runs until sub-task 8.4:
  1: [echo] [hello] [world] 1>[out] 2>&[1] | [wc] [-l] ;
oxys$ ls ; ; wc
sh: syntax error: unexpected token near `;'.
```

### 8.6 The shell's grammar is compiled into the kernel

`lexer.c` and `parser.c` reach no system call, so they are compiled into the
kernel image as the C library is, under `SHELL_SOURCES` in the `Makefile`, and
`kernel/test/shell/parser.c` asserts them against fifty lines with known
answers — the code the shell ships and not a reconstruction. It is named apart
from `LIBC_SOURCES` because a program's sources in the kernel image is a
stranger arrangement than a library's and should be visible as one. The
`Makefile`'s program rule was generalised in the same change: a program is
every `.c` file in its directory, where it had been `main.c` alone.

## 9. Verification of sub-task 8.2

### 9.1 The tokeniser

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| Words separated by spaces and tabs are three tokens of the right text, at the right columns, and the END token follows. | A column off by one names the wrong token in a diagnostic. |
| Every operator, longest first: `a>>b` is three tokens, `> >` is two, and the twenty kinds are recognised in one line. | `>>` read as two `>` is an output redirection that truncates where it should append — a file emptied. The negative test of Section 9.4 made exactly that change. |
| Digits immediately before `<` or `>` are an `io_number`; `2 >` is the word 2. | `2>err` sending the standard output to `err`. |
| A quoted blank or operator character delimits nothing, and the quotes are kept upon the word. | `'a b'` as two words; a quote removed before an expansion could see it. |
| A `#` at the start of a token begins a comment; within a word it is a character. | A comment run as a command; `a#b` cut in half. |
| An unterminated quote, in either form, and a trailing backslash, are INCOMPLETE. | A quote silently closed at the end of the line. |
| An empty line and a line of blanks are one END token. | A blank line reported as a syntax error. |
| More tokens than the bound are refused, not overrun. | A token written past the end of the array. |

### 9.2 Quote removal

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| An unquoted word is unchanged; single quotes are removed and their contents kept literal, backslash included. | `'\n'` becoming a newline. |
| Within double quotes a backslash escapes exactly `$`, `` ` ``, `"`, `\` and newline, and nothing else. | `"\d"` becoming `d`, or `"\$"` staying `\$`. |
| An unquoted backslash preserves the character after it; adjacent quoted and unquoted parts join into one word; `''` is the empty word; an escaped newline is removed. | `f\ g` as two words; `a'b'` as two. |
| A word that does not fit is refused. | A string written past its buffer. |

### 9.3 The parser

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| A simple command's words are in order, the first the name, with no condition, separator or negation. | An argument vector in the wrong order. |
| A pipeline of three is one pipeline of three commands; `<in` on the first and `>out` on the last carry descriptors 0 and 1 and their targets, and neither becomes a word. | A redirection's target run as a command. |
| Every redirection operator, with and without an `io_number`, records its meaning and its descriptor, and the words either side of them stay in order. | `4>&1` acting upon descriptor 1; `<&3` recorded as an input file named 3. |
| A list's separators, conditions and negation are as written, a trailing `;` ends it, and `!` is not a word of the command it negates. | A program called `!` sought upon the ramdisk. |
| A blank line and a comment are EMPTY; `ls |`, `a &&` and `a >` are INCOMPLETE; a pipeline continued across a newline parses as one, and a quote continued across one keeps the newline. | A continuation reported as a syntax error, or a newline lost from a quoted string. |
| A leading `|`, a doubled `;` and a redirection whose target is an operator are UNEXPECTED, at the token's position. **The negative test of Section 9.4 made an empty command legal and these two caught it.** | An empty pipeline run as nothing, silently. |
| `if true`, `cat <<EOF`, `(ls)` and `1<<-y` are UNSUPPORTED; `echo if` is not. | A program called `if` sought upon the ramdisk. |
| Seventeen words, nine redirections, nine commands, seventeen pipelines and a four-digit `io_number` are each refused by name. | A word written past the end of a command's array. |

### 9.4 The shell, upon a session

The shell itself is then run at privilege level 3 upon a session of eight
lines — a command with every kind of redirection and a pipe, a quote continued
across a line, a pipe continued across a line, a doubled separator, a compound
command, a comment, and control-D — and asserted to consume exactly the
session and to end with zero. What it printed is read by a person; Section 8.5
shows it.

**Two negative tests**, each caught and reverted: `>>` tokenised as two `>`,
caught by the operator assertion and by three upon the redirections; and an
empty simple command made legal, caught by the two UNEXPECTED assertions —
while the shell, run upon the same session, still ended with zero, because it
printed an empty pipeline and nothing asserts what it prints.

## 10. Limitations of sub-task 8.2

1. **No expansion.** `$HOME`, `$1`, `$?`, `~`, `` `…` ``, `$(…)`, `$((…))`,
   field splitting and pathname expansion are all absent; a `$` is a character
   of a word. The tokeniser will have to recognise the extent of an expansion
   (Section 2.3, rule 5) when one exists, and that is a change to `lexer.c`
   and to its assertions.
2. **No assignment words.** `NAME=value cmd` is the word `NAME=value` and a
   command named by it. Section 2.9.1's assignment prefix arrives with the
   variables 8.3's `export` sets.
3. **No compound command, function or here-document**, each refused by name;
   Section 8.1. A subshell `( )` likewise.
4. **The bounds are small**, and sixteen words to a command is the smallest of
   them: it is the argument vector's own bound and a command with more would be
   refused by the kernel in any case. A larger vector is the change
   [`LIBC.md`](LIBC.md), Section 12.7, limitation 9, describes.
5. **Nothing runs.** The structure is built and described, and 8.3 and 8.4 are
   what act upon it.

## 11. Sub-task 8.3: the working directory

**Implementation**: `working_directory` in
[`../../kernel/include/oxys/proc/process.h`](../../kernel/include/oxys/proc/process.h),
set at creation and copied by `ProcessFork` in
[`../../kernel/proc/process.c`](../../kernel/proc/process.c); `SyscallCopyUserPath`,
`SyscallCanonicalisePath`, `SyscallDoChangeDirectory` and
`SyscallDoGetWorkingDirectory` in
[`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c);
`SYSCALL_CHDIR` and `SYSCALL_GETCWD` in
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h);
`OxysChangeDirectory` and `OxysGetWorkingDirectory` in
[`../../libc/syscall/calls.c`](../../libc/syscall/calls.c); asserted by
[`../../userland/dir-check/main.c`](../../userland/dir-check/main.c), run by
[`../../kernel/test/proc/directory.c`](../../kernel/test/proc/directory.c).

`cd` is the first thing in the plan's line for 8.3, and it presumes something
this kernel did not have: [`LIBC.md`](LIBC.md), Section 12.7, limitation 5,
records that every path a program named was absolute in effect, a relative one
resolving against the root. So the sub-task begins in the kernel.

**It is a path in the process control block, and every call resolves against
it in one place.** Each process holds its working directory as an absolute path
of at most `SYSCALL_PATH_MAXIMUM` characters, `/` at creation, copied by `fork`
and kept by `execve` — as IEEE Std 1003.1-2017 has both. A relative path given
to *any* call is joined to it by `SyscallCopyUserPath`, the one function every
path-taking call copies its argument through, so that `open`, `mkdir`,
`unlink`, `execve` and `chdir` itself cannot resolve a relative path
differently from one another or forget to. The bound is upon the joined path:
a relative path that fits by itself and not once joined is `ENAMETOOLONG`,
the same refusal an absolute path of that length receives.

**`chdir` stores the canonical form and establishes it is a directory first.**
The path is reduced lexically — no `.`, no `..`, no repeated or trailing
separator, `..` at the root staying at the root as Section 4.13 has it — and
`VfsStat` is asked what it names before it is stored: a working directory
that named a file would make every later relative path fail with `ENOTDIR`
at some later call, which is the wrong place for the refusal. The reduction is
lexical, which is what every shell's `pwd -L` reports, and a symbolic link in
the path is not followed; Section 15 counts it. `getcwd` copies the path out,
and a buffer too small is `ENAMETOOLONG` from the kernel — which has no
`ERANGE` among its results — and `ERANGE` from the C library's wrapper, which
is the one wrapper that translates a result rather than passing it through,
because the standard names `ERANGE` for exactly this.

**A path and not a held node**, which is the cheaper of the two shapes and has
a consequence recorded rather than hidden: a directory removed or renamed
beneath a process leaves it with a working directory that names nothing, and
its next relative path fails with `ENOENT` rather than resolving from where it
was. Holding the node would need the reference count upon an open file that
[`../storage/VFS.md`](../storage/VFS.md) records the filesystem layer as not
having.

## 12. Variables, assignments and expansion

**Implementation**: [`../../userland/sh/variables.c`](../../userland/sh/variables.c)
and [`../../userland/sh/expand.c`](../../userland/sh/expand.c), the assignment
prefix in [`../../userland/sh/parser.c`](../../userland/sh/parser.c); both
units compiled into the kernel image under `SHELL_SOURCES` and asserted by
[`../../kernel/test/shell/parser.c`](../../kernel/test/shell/parser.c).

`export` is the third thing in the plan's line, and an `export` with nothing
to read a variable back is a word that does nothing observable. So the
sub-task carries the smallest expansion that makes `export` mean something:
`$NAME`, `${NAME}` and `$?`, and the assignment word `NAME=value` of Section
2.10.2, rule 7, which the parser now records before a command's name and
treats as a word after it.

**The variables are a fixed table** — sixty-four names of sixty-four
characters and values of two hundred and fifty-five — each marked exported or
not, for the reason every other store in this shell is fixed: a shell that
could not set a variable for want of memory could not say so. A name is
Section 3.235's; `export NAME` of a name not yet set creates an empty exported
variable, which is the simplest record of the standard's "exported once
assigned".

**Expansion and quote removal are one pass, and the reason is a rule of the
standard.** Section 2.6 orders them as two steps, but the characters an
expansion produces are never quoting characters; done as two passes, a value
holding a quote would be scanned by quote removal as if it had been typed. One
pass copies a value straight to the output, past the quote scan, and respects
the three quotings on the way: `$` within single quotes is a character, within
double quotes it expands, and a backslash escapes it in both the places
Section 2.2 says it does. The expansion takes a *lookup function* rather than
the table above, so that the kernel's self-test asserts it against a table of
its own and so that `?` — which is no variable — is the shell's to answer.

**The tokeniser did not change**, which is what Section 8.2 bought: the tokens
kept their quotes, and the expansion reads them where they stand.

## 13. The built-ins

**Implementation**: [`../../userland/sh/builtins.c`](../../userland/sh/builtins.c),
the one unit of the shell's that reaches a system call and is therefore not
compiled into the kernel image; run from `ShellRunCommand` in
[`../../userland/sh/main.c`](../../userland/sh/main.c).

A built-in is a command the shell must run itself because a child could not
do it on the shell's behalf: a directory changed in a child is changed for the
child, and so is a variable set there. The four of the plan's line are the
four this sub-task has.

| Built-in | What it does | What it refuses |
| -------- | ------------ | --------------- |
| `cd [dir]` | `chdir`, then `OLDPWD` and `PWD` set and exported; no operand is `HOME`, `-` is `OLDPWD` and prints the new directory. | `-L` and `-P`, because the kernel keeps `-L`'s answer and has no way to give `-P`'s; `CDPATH` is not searched. |
| `pwd` | `getcwd`, and a newline. | Any operand or option. |
| `export [-p] [name[=value]...]` | Sets and marks; with no operand, or `-p`, writes `export NAME=value` for each exported variable. | A word that is not a name. |
| `exit [n]` | Ends the shell with `n`, or with `$?`. It sets a flag the loop looks at rather than ending the process, so that what was printed is flushed. | An operand that is not a number, or more than one. |

**Special and regular, Section 2.14.** `exit` and `export` are special
built-ins, so an assignment before one persists in the shell; `cd` and `pwd`
are regular, so an assignment before one does not, and the shell does not
apply it at all — there being no environment to apply it to until 8.4.

**Everything else is answered with 127.** A command that is not a built-in is
described, as 8.2 described everything, and given the status Section 2.8.2
assigns a command that could not be found — which is the truth of it: nothing
can find a program until 8.4. `&&`, `||` and `!` are honoured upon that status
and upon the built-ins' own, and `$?` reports it, so the built-ins can be
composed and their statuses seen; a redirection upon a built-in is named and
not performed until 8.5, and a pipeline of more than one command is refused
until 8.6.

## 14. Verification of sub-task 8.3

### 14.1 The working directory, by `dir-check`

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| A program begins at the root; the kernel asserts the field, the program asserts `getcwd`. | A process inheriting whatever its slot's last occupant left. |
| `chdir /bin` moves it, `getcwd` reports it, and `open echo` — a call that is neither — resolves against it. | A working directory that `chdir` and `getcwd` agreed about and `open` ignored. |
| `..`, `.`, `bin/../bin/./`, `../..` above the root and `//bin//` all reach the canonical path. | `pwd` printing `/bin/../bin/.`; `..` at the root leaving the root. |
| `chdir` into a name that does not exist is `ENOENT` and into a file is `ENOTDIR`, and neither moves it. | The negative test below: a `chdir` into a file that succeeded, and every later relative path failing somewhere else. |
| `getcwd` into a buffer too small is `ERANGE`. | A path truncated into a buffer and reported as complete. |
| A relative path that fits alone and not once joined is `ENAMETOOLONG`. | A path silently truncated at the bound. |
| A child of `fork` inherits it, and the child's `chdir` leaves the parent's alone. | The negative test below: a child that began at the root. |

### 14.2 Assignments, variables and expansion, in the kernel

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| Assignment words before the name are recorded whole and after it are words; assignments alone are a command without a name; nine are refused by name. | `C=3` after a command's name taken as an assignment and lost from the argument vector. |
| What is and is not a name: `_a1=` is, and `1a=`, `a-b=` and `=x` are not. | A program named `a-b=x` run as an assignment. |
| `$HOME`, `${HOME}x`, `$HOMEx` taking the longest name, `$?`, an unset variable as nothing, an empty one as nothing, a lone `$`, `$1` and an unclosed `${` as literal. | `$HOMEx` expanding `$HOME` and appending `x`. |
| `$HOME` expands within double quotes and not within single, and an escaped `$` expands in neither. | The negative test below. |
| A value holding quotes comes through as it is, unquoted and within double quotes. | A value of `it's` closing a quote the person opened. |
| The table: set, set again, unset is null, export marks, exporting an unset name creates an empty exported variable, a bad name and a value beyond the bound are refused, and the count is right. | A variable set twice held twice. |

### 14.3 The shell, upon a session whose evidence is its status

A second session runs the shell at privilege level 3 after the root is
mounted — `X=5`, `export Y=7`, `cd /bin && H=3`, `cd /nope || F=1`,
`cd /bin/echo && G=2`, `cd .. ; pwd`, `export`, `exit $F$G$H$Y` — and the
shell is asserted to end with **137**, which it does only if the assignment,
the export, the expansion, `cd` succeeding into a directory and failing into
nothing and into a file, and `&&` and `||` acting upon those statuses all
worked. `pwd` and `export` print, which a person reads; the status is what is
asserted, and it is the same device `startup-check` and `arg-check` use: a
number the kernel checks independently of anything printed.

### 14.4 The negative tests

Three defects inserted, each caught, each reverted. `chdir` made to accept a
file: caught by `dir-check` twice, and by the session — which ended with 213
because `cd /bin/echo && G=2` had set `G`. `fork` made to copy only the first
character of the working directory: caught by `dir-check` twice. A `$` made to
expand within single quotes: caught by the expansion's assertion.

## 15. Limitations of sub-task 8.3

1. **The working directory is a path**, reduced lexically: a symbolic link in
   it is not followed, and a directory removed beneath a process leaves it
   with a name that resolves to nothing. Section 11.
2. **The expansion is `$NAME`, `${NAME}` and `$?` and nothing else.** No
   positional parameter — `$1` is literal, there being no way to give a script
   arguments — no `${NAME:-word}` and its kin, no tilde, no command
   substitution, no arithmetic, no field splitting and no pathname expansion.
   Each arrives when something wants it; field splitting in particular changes
   what a word *is* and is the first thing to decide before a script is ever
   read.
3. **No environment reaches a program yet.** The exported variables are marked
   and listed, and 8.4's `execve` is what will carry them; until then `export`
   is a promise the shell keeps to itself.
4. **`cd` sets `PWD` and `OLDPWD` but `HOME` is nobody's to set.** No login
   sets it, so `cd` with no operand fails until somebody exports one.
5. **A regular built-in's assignment prefix is discarded** rather than applied
   to an environment the built-in would see, there being no such environment
   until 8.4.
6. **`exit` in a `&&` chain ends the shell where the standard would too**, but
   `&` is still recorded and not honoured, and a pipeline of two built-ins is
   refused rather than run in a subshell.
