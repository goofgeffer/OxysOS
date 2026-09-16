<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Shell

**Phase**: 8 of [`../project/PLAN.md`](../project/PLAN.md). This document is
Phase 8's, as [`LIBC.md`](LIBC.md) is Phase 7's: one section per sub-task, in
order, each recording what that sub-task built and why, and each revised as the
design is. Sub-tasks 8.1 (Sections 1 to 7), 8.2 (Sections 8 to 10), 8.3
(Sections 11 to 15), 8.4 (Sections 16 to 18), 8.5 (Sections 19 to 21) and 8.6
(Sections 22 to 24) are here so far; Section 25 is `micro`, the line editor
added beside 8.6, Section 26 is `clear`, and Section 27 the prompt that names the
working directory.

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
[`SCHEDULER.md`](SCHEDULER.md), Section 10, limitation 8, already records as
absent. Section 6 counts it, and Section 22.3 records what 8.6 did about it.

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
2. ~~**A `read` of the terminal halts the processor.**~~ **Amended at sub-task
   8.6**, Section 22.3: a read of the terminal yields to whatever the run queue
   holds and halts only when it holds nothing, so a pipeline's children run
   while the shell — or `cat` at the head of a pipeline — waits for a key. It
   does not sleep upon the wait queue of [`SCHEDULER.md`](SCHEDULER.md),
   Section 9, because the bytes arrive through an interrupt handler and nothing
   yet wakes a thread from one; 8.7's signals are what will.
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

## 16. Sub-task 8.4: external program execution

**Implementation**: [`../../userland/sh/run.c`](../../userland/sh/run.c),
called from `ShellRunCommand` in [`../../userland/sh/main.c`](../../userland/sh/main.c);
`getenv` in [`../../libc/stdlib/environment.c`](../../libc/stdlib/environment.c),
its vector recorded by [`../../libc/crt/crt0.asm`](../../libc/crt/crt0.asm);
asserted by [`../../userland/env-check/main.c`](../../userland/env-check/main.c)
and the third session of [`../../kernel/test/shell/parser.c`](../../kernel/test/shell/parser.c),
and by the assertion `dir-check` gained; and the correction to
[`../../kernel/arch/x86_64/syscall/syscall_entry.asm`](../../kernel/arch/x86_64/syscall/syscall_entry.asm)
the sub-task found necessary, Section 16.3.

### 16.1 What runs, and how it is found

A command that is not a built-in is a program, since this sub-task. Section
2.9.1.1 of IEEE Std 1003.1-2017 gives the search: a name holding a slash is
the pathname; one without is sought in each directory of `PATH` in turn, and
`PATH` unset is `/bin`, the one directory this system's programs stand in.
The shell forks, and **the search happens in the child by executing each
candidate**: this kernel has no call that asks whether a file exists short of
opening it, and a program found by `open` and then not executable would be a
second call's failure to report. `execve` is tried upon each candidate, and
`ENOENT` means the next directory; any other refusal is a program that is
there and cannot run, which is Section 2.8.2's 126 and the end of the search.
Nothing found at all is 127. The parent waits, and the status is the
program's — or, for a program ended by a fault, 128 plus the vector, this
system having no signals until 8.7 and the vector standing in for one.

### 16.2 The environment

The exported variables become the program's environment: one `NAME=value`
string each, in a vector the shell builds before it forks, carried by the
`execve` of 7.6 and laid upon the new program's stack after its arguments as
the System V ABI, Section 3.4.1, has it. `_start` records where the vector
stands, and `getenv` — present in `<stdlib.h>` from this sub-task, for the
first program that could have used it — searches it. A variable assigned and
not exported does not reach the program, which is what `export` was for.

The bound is the kernel's: sixteen strings to a vector and two kibibytes for
both, so a shell that exported more than sixteen variables would be refused
by `execve` with `EINVAL` and the program reported as not runnable.
[`LIBC.md`](LIBC.md), Section 12.7, limitation 9, records what enlarging it
costs; Section 18 counts it here.

### 16.3 The defect this sub-task found in the system-call entry path

The first program the shell ran printed its line and the shell then faulted at
an address made of the bytes of its own stack. `dir-check`, given a fork
whose child became `echo`, reproduced it: after `wait` the parent resumed
with the child's stack pointer.

The entry path of sub-task 6.7 saved the caller's stack pointer in the
per-processor block and restored it from there at `SYSRET`, though it had
also pushed it into the frame. The block is per processor and the frame is
per thread, and nothing distinguished the two while one thread at a time was
inside a system call upon a processor. But `wait` runs a child *within* the
parent's call, and a child that executes `SYSCALL` writes its own stack
pointer over the parent's in the block. A child that merely exited from the
cloned stack had the same pointer as its parent — which is why five sub-tasks
of `fork` and `wait` never saw it — and the first child to become another
program, whose stack is new, did. The stack pointer is now restored from the
frame, by `POP RSP`, and the block's copy is used only for the push at entry.
[`PRIVILEGE.md`](PRIVILEGE.md), Section 6, records the correction beside the
path it corrects.

## 17. What a program is given

| Given | From | Since |
| ----- | ---- | ----- |
| `argv`, quotes removed and expansions made, `argv[0]` the name as typed | The shell's expansion of the command's words | 8.4 |
| `envp`, one `NAME=value` per exported variable | `ShellBuildEnvironment` | 8.4 |
| The working directory | Inherited across `fork` | 8.3 |
| Descriptors 0, 1 and 2 | The kernel's own, the terminal and the diagnostic path | 8.1 |
| An open file of the shell's | Nothing: a child inherits no descriptor | — until 8.5 |

## 18. Verification of sub-task 8.4, and its limitations

`env-check` is written by the self-test to `/verify/env-check` — a check
program is not shipped in `/bin`, and a program written onto the root at run
time being found and run is itself the assertion — and the shell is run upon
a session: `export MARK=abcd`, `HIDDEN=1`, `/verify/env-check alpha "b c" &&
A=1`, `/bin/cat /nope; C=$?`, `nothing; N=$?`, `echo hi && E=2`,
`exit $A$C$N$E`. The status is 111272 in the eight bits a status has — 168 —
only if `env-check` found its three arguments and its exported variable and
not the hidden one, `cat` ended with 1, a name not found ended with 127, and
`echo` was found upon the default `PATH`. `dir-check` gained the assertion of
Section 16.3: a parent's stack survives a child's `execve`.

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| `argc` is three, `argv[0]` the pathname typed, `argv[1]` `alpha`, `argv[2]` `b c` as one word. | Quotes reaching the program, or a quoted operand split in two. |
| `MARK` reaches the environment, `HIDDEN` does not, `PWD` does, and `getenv` does not match a prefix. | An unexported variable leaking; `getenv("MAR")` answering for `MARK`. |
| `cat /nope` is 1, `nothing` is 127, `echo` upon `PATH` is 0. | A program's status lost, or a not-found reported as a failure of a program. |
| The parent's stack pattern survives a child's `execve`. | The defect of Section 16.3, which appeared as a fault at a return address made of the pattern. |

**Two negative tests**, each caught and reverted: the environment vector built
empty, caught by `env-check` and by the session's status; and the entry path's
correction reverted, caught by `dir-check` and by the shell faulting.

Limitations: **redirections are named and not performed** (8.5); **a pipeline
of two commands is refused** (8.6); **`&` is recorded and not honoured** and
**nothing interrupts a program** (8.7); **the environment is bounded at
sixteen variables** by the kernel's vector bound; **a program is run upon the
shell's own flow of control** — `wait` runs the child synchronously, as
[`PROCESS.md`](PROCESS.md), Section 13.2, records — so nothing runs beside
the shell until the scheduler carries a user thread of its own — which it does
since 8.6, Section 22.3.

## 19. Sub-task 8.5: input and output redirection

**Implementation**: `ShellApplyRedirections` in
[`../../userland/sh/run.c`](../../userland/sh/run.c); in the kernel, the
holder count of [`../../kernel/fs/vfs/file.c`](../../kernel/fs/vfs/file.c),
`ProcessPlaceDescriptor` and the inheritance in
[`../../kernel/proc/process.c`](../../kernel/proc/process.c), and `open` with
its four new flags, `write` to a file, `read` from a redirected 0, `dup2` and
`rmdir` in [`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c);
asserted by [`../../userland/file-check/main.c`](../../userland/file-check/main.c)
and the fourth session of [`../../kernel/test/shell/parser.c`](../../kernel/test/shell/parser.c).

### 19.1 What a redirection is, and where it is done

A redirection makes a program's 0, 1 or 2 name a file, before the program
runs; the program itself neither knows nor cares. It is done **in the child,
between `fork` and `execve`**, in the order written — IEEE Std 1003.1-2017,
Section 2.7 — so that `>out 2>&1` sends both to the file and `2>&1 >out`
does not, which is the difference between the two that every shell of this
lineage has and every person who has typed the wrong one has learned. Each
operator opens or duplicates and then places by `dup2`; a failure ends the
child with 1 before any program runs upon the wrong descriptors.

| Operator | What the child does |
| -------- | ------------------- |
| `[n]<word` | `open` for reading, placed at `n` (0). |
| `[n]>word`, `[n]>\|word` | `open` for writing, created or truncated, placed at `n` (1). `noclobber` does not exist, so the two are one. |
| `[n]>>word` | `open` for writing, created, appending. |
| `[n]<>word` | `open` for reading and writing, created. |
| `[n]<&m`, `[n]>&m` | `dup2(m, n)`; `-` closes `n`. |

The expansion of the target word is the shell's ordinary one — `>$F` works
— and quote removal follows it.

### 19.2 What the kernel had to grow

[`LIBC.md`](LIBC.md), Section 12.7, held two limitations for this sub-task
since 7.6, and it closes both.

**A call that creates or writes a file.** `open` accepts WRITE, CREATE,
TRUNCATE and APPEND beside READ and DIRECTORY, refusing any other bit and any
of the three change flags without WRITE; it takes a mode, recorded and not
enforced, as `mkdir`'s has been. `write` reaches the open file a descriptor
names, and the diagnostic path only where 1 or 2 names none. `read` of 0
reaches a file placed there and the terminal otherwise.

**A descriptor a child inherits — and its cause, a count of holders.** An
open file of the filesystem layer now counts how many descriptors name it:
one at `open`, one more per `VfsHold`, one fewer per `VfsClose`, released by
the last. That single field is what `dup2` and inheritance both are: two
numbers, or two processes, holding one open file and one position. So a child
of `fork` inherits every descriptor, as POSIX has it, and `execve` keeps them
— it closed them from 7.6 to 8.4, the safe half of the rule while nothing
could mean to keep one; the redirection is what means to, and the table is
the same process's. [`PROCESS.md`](PROCESS.md), Section 14, and
[`../storage/VFS.md`](../storage/VFS.md), limitation 2.

**`dup2`, with one rule of this kernel's own.** A number below
`SYSCALL_DESCRIPTOR_FIRST` that holds no file names the kernel's own path —
the terminal for 0, the diagnostic path for 1 and 2 — and that path may be
given to another of the three (`2>&1` with nothing else redirected) but not to
a number above them, the table holding files and the kernel's paths not being
files. A program that closes a redirected 2 finds it the diagnostic path
again. `file-check` asserts each of these.

**`rmdir`**, the call `rm` could not stand in for since 7.6, exposed now that a
directory made from the prompt is a thing a person wants gone.

### 19.3 More commands

`help`, `true`, `false` and `unset` join the built-ins — `help` because a
person at a prompt with no manual has nothing else to ask — and `touch`, `cp`
and `rmdir` join `/bin`, the first two being the first utilities that write a
file and the third the first that removes a directory. `cat` with no operand,
or `-`, copies the standard input at last: a file the shell redirected ends,
and the terminal, which does not, ends at a control-D, which `cat` treats as
the end because no line discipline is there to do it for every program.
The shell prints no greeting: the prompt is the whole of what a person sees,
and `help` is for the rest.

## 20. Verification of sub-task 8.5

### 20.1 `file-check`, extended

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| Two writes to a created file are one file of six bytes; a read of a write-only descriptor is `EINVAL`. | A position that did not advance, overwriting the first write with the second. |
| Append goes to the end; a truncating open empties the file. | An append at the start; a truncate that did nothing. |
| Two numbers of one file share one position; closing the original leaves the duplicate; a closed duplicate is `EBADF`. | The negative test below: a close by one number releasing the file from under the other. |
| A file placed at 2 receives what is written to 2, and 2 reverts to the diagnostic path when it is closed. | A redirected 2 writing to the log, or a closed 2 refusing the next diagnostic. |
| The diagnostic path cannot be duplicated above the three; an empty number cannot be duplicated. | A table slot holding a number that names no file. |
| A child writes through the inherited descriptor and the parent's write follows it. | A child that inherited nothing, or a copy of the file rather than a share of it. |

### 20.2 The shell, upon a session the files answer for

A fourth session uses every operator from the prompt — `>`, `>>`, `<` with
`>`, `2>`, `>` with `2>&1`, `>|` — and the three utilities, and the kernel then
reads each file back and compares it: `/verify/out` holds `written` and
`more`, `/verify/copy` what `cat </verify/out` copied, `/verify/err` and
`/verify/both` `cat`'s own diagnostic, `/verify/t` nothing, `/verify/c2` the
copy `cp` made; and `rmdir` removed what `mkdir` made, which `exit $D` and
`VfsStat` both say.

**Two negative tests**, each caught and reverted: `VfsClose` releasing a file
at the first close regardless of holders, caught by `file-check` six times
and by the redirection session; and the redirections applied in reverse order,
caught by `/verify/both` holding nothing.

## 21. Limitations of sub-task 8.5

1. **A redirection upon a built-in is named and not performed.** Applying one
   in the shell and restoring it needs the terminal to be duplicable above the
   three, which Section 19.2's rule forbids; `export >file` is the case a
   person will meet. It arrives when the kernel's paths become files.
2. **No here-document, and `<<` is still refused by name.**
3. **The environment and the vector are still bounded at sixteen strings**,
   and the descriptor table at sixteen numbers.
4. **`cat` upon the terminal ends at a control-D by its own reading**, there
   being no line discipline; every other program reading the terminal reads
   keystrokes without end until 8.7.
5. ~~**Pipelines and `&`** are 8.6's and 8.7's, as before.~~ Pipelines arrived
   at 8.6, Section 22; `&` is 8.7's still.

## 22. Sub-task 8.6: pipelines

**Implementation**: `ShellRunPipeline` and `ShellExecuteProgram` in
[`../../userland/sh/run.c`](../../userland/sh/run.c), `ShellRunStage` and the
`in_child` mode of `ShellRunCommand` in
[`../../userland/sh/main.c`](../../userland/sh/main.c); in the kernel, the pipe
of [`../../kernel/fs/vfs/pipe.c`](../../kernel/fs/vfs/pipe.c) behind
[`../../kernel/include/oxys/fs/pipe.h`](../../kernel/include/oxys/fs/pipe.h),
the `pipe` call in
[`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c),
and — the larger half — a child of `fork` that runs beside its parent,
[`PROCESS.md`](PROCESS.md), Section 17, upon the wait channel of
[`SCHEDULER.md`](SCHEDULER.md), Section 9. `OxysPipe` in
[`../../libc/syscall/calls.c`](../../libc/syscall/calls.c) and `EPIPE` in
[`../../libc/include/errno.h`](../../libc/include/errno.h); `wc` in
[`../../userland/wc/main.c`](../../userland/wc/main.c). Asserted by
[`../../userland/file-check/main.c`](../../userland/file-check/main.c),
`KernelVerifyVfsPipes` in
[`../../kernel/test/storage/vfs.c`](../../kernel/test/storage/vfs.c), and the
fifth session of
[`../../kernel/test/shell/parser.c`](../../kernel/test/shell/parser.c).

### 22.1 What a pipeline is, and where its children are made

IEEE Std 1003.1-2017, Section 2.9.2: "the standard output of *command1* shall be
connected to the standard input of *command2*", each command in a subshell
environment, the shell waiting for the last and the status the last command's
— or, after `!`, its inverse. The parser of 8.2 has held a pipeline as a list of
commands since it was written, and until this sub-task the shell answered any
list longer than one with a statement that it could not run it.

`ShellRunPipeline` makes one child per command, each before the next, with a
pipe between each pair. In the child the read end of the pipe before it is
placed at 0 and the write end of the pipe after it at 1, by `dup2`, **before the
command's own redirections** — so that `a 2>&1 | b` sends `a`'s diagnostics
down the pipe, the standard's order — and every end the child does not need is
closed there. In the shell, every end is closed the moment the children that
need it exist: the write end after the child that writes it is made, the read
end after the child that reads it is. That order is the whole of what makes a
pipeline end. A pipe's reader sees the end of the file only when the *last*
write end is closed, and a write end left open in the shell — which holds it
only to hand to a child — would keep every reader waiting for a writer that had
already gone. Section 23 records the negative test that showed it.

**A built-in in a pipeline runs in the child**, which is what "a subshell
environment" means and what makes `help | wc -l` count something. It is also
the surprise every shell of this lineage offers — `cd /bin | true` moves the
child and not the shell — and it is kept rather than avoided because the
alternative, running the built-in in the shell with its output somehow
captured, is a pipe the shell would have to read itself while the next command
wrote it. The child runs the command by the same `ShellRunCommand` the shell
does, told it is in a child: a program is become by `execve` rather than forked
for, and a built-in's status is the child's, with `exit` flushing whatever it
printed into the pipe before the write end goes with the process.

The shell then waits for every child, collecting them in whatever order they
end, and the status kept is the last command's, matched by number; 126 where the
last command was never made. A pipeline of one command is not brought here at
all: it runs in the shell itself, where a built-in must run to have any effect.

### 22.2 The pipe

A bounded queue of bytes between two open files, one that reads it and one
that writes it, upon which a reader sleeps while it is empty and a writer
sleeps while it is full. It is an open file of the filesystem layer with no
node beneath it, for the reason [`../storage/VFS.md`](../storage/VFS.md),
Section 11.3, gives: everything a descriptor does was built at 8.5 upon the
open file, and a pipe end that was not one would have needed all of it twice.

| Rule | What it prevents |
| ---- | ---------------- |
| A read of an empty pipe sleeps until a write, or until the last writer closes, upon which it reports zero. | A reader that saw the end before the writer had finished — `wc` counting half a file. |
| A write to a full pipe sleeps until a reader makes room, and waits for room for the *whole* of what remains where that fits the buffer. | Bytes dropped, or two writers' bytes interleaved; every write a program can make is a page or less, and the buffer is a page, so every write is one piece. |
| A write to a pipe held open for reading by nobody is `EPIPE`. | A writer told its bytes went somewhere; the signal the standard sends beside it is 8.7's. |
| Readers and writers are counted as open files, not descriptors. | A child's inherited write end closing the pipe from under its parent — one open file with two holders is one writer. |
| One channel for both, woken by every write, read and close. | A close that woke the reader and not the writer, or the reverse; each sleeper re-tests and sleeps again if it was not the one meant. |
| A caller that cannot sleep is refused as busy. | The kernel's own flow of control, reading an empty pipe in a self-test, waiting for ever for a writer that is itself. |

### 22.3 What the kernel had to become

A pipeline is two programs alive at once, and this kernel had never had two:
a child ran upon its parent's flow of control, inside the parent's `wait`. So
the sub-task's larger half is not the pipe but the scheduler beneath it, and
four documents hold it. A child of `fork` is admitted to the run queue at the
fork and runs when its parent sleeps or is pre-empted at privilege level 3;
`wait` sleeps upon the parent's process and is woken when a child ends; the
thread to return to is a field of the started thread rather than one pointer
per processor, which a sleeping shell made wrong; and the counted
interrupt-disable travels with the thread across a switch, which the first
sleeper resumed from the idle thread made necessary.
[`PROCESS.md`](PROCESS.md), Section 17; [`SCHEDULER.md`](SCHEDULER.md),
Section 9; [`CONCURRENCY.md`](CONCURRENCY.md), Section 4.1.

**A user thread is pre-empted at privilege level 3 and nowhere else.** The
kernel beneath a system call is not written to be entered by two threads, and
a user thread therefore keeps the processor inside the kernel until it gives it
up — asleep in `wait`, upon a pipe, or at the terminal — which is where it
holds nothing. That is what keeps the list of unsynchronised structures in
`CONCURRENCY.md` a list about a second processor and not about this sub-task.

**The terminal's reader yields rather than sleeps.** A `read` of the terminal
halted the processor until an interrupt, which stopped every program with it;
it now gives the processor to whatever the run queue holds and halts only when
the queue is empty, so `cat` reading the terminal at the head of a pipeline
feeds the command after it. It does not sleep upon the wait channel, because
the bytes arrive through an interrupt handler and nothing yet wakes a thread
from one — 8.7 pays that cost, when a signal must interrupt a read.
Section 6, limitation 2.

### 22.4 `wc`, and the other things that arrived

`wc` joins `/bin` — IEEE Std 1003.1-2017's, counting newlines, words and bytes
in that order, `-c`, `-l` and `-w` selecting one, a `total` line for more than
one operand, and the standard input for none — because it is what a person puts
at the end of a pipeline to learn how much came through it. Unlike `cat` it
reads the standard input to its end and not to a control-D: it is what a pipe
feeds, and a pipe ends when its writer does.

`help` is a list now, one command to a line — the name, its arguments, a comma
and one sentence — the built-ins first and then the programs of `/bin`, at the
project owner's direction on 2026-09-16: a person who typed `help` wanted to
find a command, not to read about the shell, and the paragraph that described
the operators was removed. The first session of the self-test, which the shell
had answered by refusing its pipelines, now runs them, and was amended so that
it names no file: a session run at every boot must leave nothing upon the root.

## 23. Verification of sub-task 8.6

### 23.1 `file-check`, extended

| Property asserted | The silent failure it catches |
| ----------------- | ----------------------------- |
| `pipe` gives two new numbers above the three; three bytes in are three bytes out. | A pipe made of one open file, or bytes delivered to the end that wrote them. |
| A read of the write end and a write to the read end are both `EINVAL`. | An end that did both, which a program would discover as its own output. |
| Bytes written before the close are read after it, and the read after those is zero. | The end of the file arriving early, or never. |
| A write with no reader is `EPIPE`. | A writer told its bytes went somewhere. |
| Twelve kibibytes from a child cross a four-kibibyte pipe intact and in order, and the child ends with zero. | A writer that did not sleep when the pipe was full; a reader that did not sleep when it was empty; a close that woke nobody, which this program would report by never ending. |

### 23.2 The kernel's own assertions

`KernelVerifyVfsPipes` asserts the pipe from the kernel's flow of control, which
cannot sleep: the table accounting, the ordered bytes, each end's one direction,
the refusal of a seek, the busy refusal of a read that would block, the second
holder that keeps the pipe open through the first close, and the broken pipe.
[`../storage/VFS.md`](../storage/VFS.md), Section 11.3, has the table.

### 23.3 The shell, upon a session the files and the status answer for

A fifth session: `echo one two three | wc -w`, `cat | cat | cat` of a file the
session made, `help | wc -l`, `cat /verify/nonexistent 2>&1 | wc -l`, and
`cat /bin/sh | wc -c` — the whole of the shell, some thirty-three kibibytes,
eight times the pipe's buffer — each into a file the kernel reads back; and
`false | true`, `true | false` and `! true | false`, whose statuses compose
`exit $T$F$N` into 010, which is 10, only if the last command's status is the
pipeline's and `!` inverts it. The size of `/bin/sh` is asked of the layer and
not written into the test, the shell upon the ramdisk being a build product.
The session leaves no pipe behind, and at least one byte crossed one.

**Two negative tests**, each caught and reverted. The pipe's writer was made to
take what fitted and claim the rest was written, waiting for a byte's room
rather than for room for all of it; `file-check` did not catch it — its chunks
are the buffer's own size, so nothing was ever partly written — and
`/verify/p5` did, holding a count short of the file. And the shell was made to
keep a pipe's write end open after making the child that writes it; the first
pipeline of the first session never ended, and the verification timed out with
the prompt still waiting for `wc`.

**One defect this sub-task met in its own change**, recorded in
[`PROCESS.md`](PROCESS.md), Section 17.2: a child admitted at the fork without
its kernel stack prepared, which faulted in the switch at a stack pointer with
no stack beneath it. The first program to fork after the change found it.

## 24. Limitations of sub-task 8.6

1. **No `SIGPIPE`.** A writer whose reader has gone is told `EPIPE` and nothing
   more; a program that ignores the result — `cat` does not — runs on. The
   signal arrives with 8.7.
2. **`&` is still recorded and not honoured**, and nothing interrupts a program;
   both are 8.7's. A pipeline that reads the terminal and never ends can be
   ended only by the terminal's control-D reaching a `cat`.
3. **Eight pipes**, each of a page, drawn from a fixed table; a ninth is refused
   as `EMFILE`. A pipeline of nine commands is therefore refused at its eighth
   pipe, which is one more than `SHELL_COMMAND_MAXIMUM` allows anyway.
4. **A wake walks the thread table**, and the terminal's reader polls rather
   than sleeps; [`SCHEDULER.md`](SCHEDULER.md), Section 10, limitations 8 and 9.
5. **A built-in in a pipeline affects the child alone**, which is the
   standard's subshell and a surprise all the same; and a redirection upon a
   built-in outside a pipeline is still named and not performed, Section 21,
   limitation 1.
6. **The here-document, the environment's bound and the terminal's line
   discipline** are as Section 21 left them.

## 25. `micro`, the line editor

**Implementation**: [`../../userland/micro/main.c`](../../userland/micro/main.c);
`LinePreset` in [`../../libc/line/line.c`](../../libc/line/line.c) and
`LineEdit` in [`../../libc/line/system.c`](../../libc/line/system.c). Added on
2026-09-16 at the project owner's request, beside sub-task 8.6 and belonging
to no sub-task: the smallest editor that can edit.

Until it, a file could be written — `echo >f`, `>>f`, `cat >f`, `cp` — and not
changed: no editor, and no `lseek` a program can reach. `micro FILE` loads the
file whole, prints it with line numbers, and takes commands at a `micro> `
prompt: `p` prints, `a` appends lines typed until a line holding only `.`,
`i N` inserts them before line N, `d N` deletes line N, `e N` hands line N back
to be changed, `w` writes the file whole over what it was, `q` quits and is
refused while there are unsaved changes, `q!` discards them, `wq` writes and
quits, `h` lists these. The letters and the manner are `ed`'s and nothing else
of `ed` is here — no address, no regular expression, no `s`.

**It is a line editor because the display can be nothing else yet.** Neither
the text-mode display nor the framebuffer console interprets a cursor-positioning
sequence, and the line editor beneath the shell draws with printable characters,
spaces and backspaces alone so that it draws the same upon a serial line
(Section 3). A screen editor needs the display to move the cursor up and clear a
line, and a program to learn the screen's size, and an editor that assumed either
would scroll the screen into nonsense. A line editor asks nothing of the display
the shell does not already ask.

**`e` is what makes it an editor.** The C library's `LineRead` began every line
empty; `LineEdit` begins it as a given text, drawn after the prompt with the
cursor at its end, so that a person changes the line with the arrow keys, Home,
End and Delete rather than retyping it — `LinePreset` beneath it is the
replacement the history's recall already performed, offered to a caller. Control-D
upon the line abandons the edit, an edit abandoned not being an edit made.

**The file is held whole and written whole**, and nothing touches it between the
load and `w`: a person who quits with `q!` has the file exactly as it was, and a
`w` that fails leaves a file that is at worst truncated — the one loss, recorded
here rather than hidden, that a rename would close when there is one
([`../storage/VFS.md`](../storage/VFS.md), limitation 5).

Observed on 2026-09-16 under QEMU, driven over the serial line: a new file
begun, three lines appended, `wrold` corrected to `world` by Home, Right, Delete,
Right, `r`, End, Return; a line deleted and one inserted before the first; `q`
refused with changes unsaved; `wq`; `cat` showing the three lines; the file
reopened and shown; `cat hello.txt | wc` reporting `3 3 18`.
[`../project/TESTING-RECORD.md`](../project/TESTING-RECORD.md).

Limitations: **1,024 lines and 511 bytes to a line**, a longer line cut when
loaded and said to be; **no search, no replace, no undo**; **a `w` that fails
half way leaves the file short**, until there is a rename; **the history the
arrow keys walk is shared** between commands and typed lines.

## 26. `clear`, and the form feed

**Implementation**: the built-in in
[`../../userland/sh/builtins.c`](../../userland/sh/builtins.c); the form feed in
`VgaPutCharacter` of [`../../drivers/vga/vga.c`](../../drivers/vga/vga.c) and
`ConsoleClear` of [`../../graphics/console.c`](../../graphics/console.c); and
`KernelSerialWriteTranslated` in [`../../kernel/kernel.c`](../../kernel/kernel.c).
Added on 2026-09-16 at the project owner's request.

`clear` writes one byte, the form feed (FF, 0x0C), and flushes it. ANSI X3.4
makes a form feed a new page; upon a screen a new page is the screen cleared and
the cursor at its top, and that is what the text-mode display and the console now
do upon it, the erase limit reset with the cursor so that a backspace after a
clear stops where the new page begins. A terminal upon the serial line does not
treat a form feed so — most print nothing, a few print a glyph — so the
diagnostic path, which is the one place that writes to all three, turns the byte
into ECMA-48's `ED 2` and `CUP`, `ESC [ 2 J ESC [ H`, for the serial line alone.
The translation is there and not in the serial driver, which carries bytes and
gives them no meaning, nor in the display drivers, which do not know a terminal
is listening.

It is a byte and not a system call because a program that wants the screen
cleared should be able to say so by writing, as it says everything else, and
because a byte crosses a pipe and a file the way a call cannot. It is not the
ECMA-48 sequence itself on the program's side, because nothing upon the display
side parses sequences yet, and a parser written for one sequence would be the
beginning of the terminal Section 25 says is not there.

Observed on 2026-09-16: over the serial line under QEMU, `clear` produced exactly
`ESC [ 2 J ESC [ H` between the prompts; typed at VirtualBox's keyboard, the
screen went black and the next prompt stood at the top left.
[`../project/TESTING-RECORD.md`](../project/TESTING-RECORD.md).

## 27. The prompt names the working directory

Since 2026-09-16, at the project owner's request, PS1 is `oxys$`, the working
directory and `> ` — `oxys$/> ` at the root and `oxys$/bin> ` within it —
rather than the fixed `oxys$ ` of Section 4. A person who has changed
directory and then typed `ls` should not have to remember which of two
directories the listing is of. The directory is asked of the kernel at every
prompt, by `getcwd`, rather than read from `PWD`, so that the prompt is the
truth and not the shell's record of it; the shell keeps `PWD` for programs, as
Section 13 has it. The continuation prompt is `> ` still, and a person reading
a session must tell the two apart by what precedes the arrow, which is nothing
for a continuation. [`../../userland/sh/main.c`](../../userland/sh/main.c),
`ShellPrompt`.

Anything that waits for the prompt — the driver of
[`../project/TESTING.md`](../project/TESTING.md), Section 2.1, was one — now
waits for `> ` at the end of the output rather than for `oxys$ `.
