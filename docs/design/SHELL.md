<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Shell

**Phase**: 8 of [`../project/PLAN.md`](../project/PLAN.md), and the utilities
added beside it.
**Source**: the terminal, [`../../kernel/terminal/terminal.c`](../../kernel/terminal/terminal.c);
the line editor, [`../../libc/line/`](../../libc/line/) behind
[`../../libc/include/line.h`](../../libc/include/line.h); the shell,
[`../../userland/sh/`](../../userland/sh/); the utilities under
[`../../userland/`](../../userland/).
**Specifications**: IEEE Std 1003.1-2017, Shell Command Language (Sections 2.2,
2.3, 2.6, 2.7, 2.8.2, 2.9, 2.10, 2.14) and General Terminal Interface (Sections
11.1.4 and 11.1.9); ECMA-48, Sections 5.4 and 8.3; xterm, *Control Sequences*.

`/bin/sh` and what it stands on: the terminal a program reads keystrokes from,
the line editor, the grammar and the expansions, the built-ins, the running of
programs with their redirections and pipelines, job control, and the small
utilities that make a pipe worth having. The shell runs on the serial line upon
the desktop's entry, on the screen upon the shell-only entries, and in a window
through `/bin/terminal` ([`TERMINAL.md`](TERMINAL.md)).

## 1. The terminal

One byte stream, assembled in the kernel from the PS/2 keyboard and the serial
line, which a `read` of descriptor 0 delivers when nothing is redirected there.

- **Raw, not canonical.** Every byte is delivered as it arrives, nothing is
  echoed and nothing is assembled into lines, because a line editor must see the
  arrow key when it is pressed. Canonical mode is not built: nothing has asked
  for it.
- **A key becomes what a terminal would send**: an arrow the ECMA-48 sequence
  CSI A to D (Sections 8.3.18 to 8.3.22), Home and End CSI H and CSI F, Delete
  CSI 3 ~, and control with a letter the control character. A serial terminal
  sends those anyway, so the editor parses one dialect and a program cannot tell
  which device it is reading.
- **What has no byte becomes nothing**: a release, a function key, a modifier.
  A byte the editor did not ask for would land in a line the person did not see.
- **The queue is a kibibyte, filled by polling** the two drivers' lock-free rings
  when a reader asks or the tick runs, rather than by their interrupt handlers,
  which would make three writers and need a lock. At the bound the newest byte is
  discarded and counted, since dropping the oldest would rewrite the start of a
  line.
- **A read waits until at least one byte is queued** and never returns nothing,
  so a program can tell the terminal from a file at its end. It yields to the run
  queue and halts only when nothing else can run, because the tick that polls the
  devices lands on a halted processor.
- **One reader.** A reader that is not in the foreground group stops waiting and
  is stopped by SIGTTIN (Section 11.1.4), retrying when continued, or is refused
  `EIO` if it ignores SIGTTIN. Two readers waiting at once would each keep the
  other runnable, the processor would never halt, the tick would never poll, and
  the machine would stop.
- **Control-C and control-Z are signals**, SIGINT and SIGTSTP to the foreground
  group (Section 11.1.9), acted on when they reach the head of the queue, so they
  keep their order with the bytes before them; the tick services the queue every
  ten milliseconds, reaching a program that never reads.
- **The keyboard can be detached** while the window manager has it, so a key
  reaches the focused window and not also the shell on the serial line
  ([`WINDOWS.md`](WINDOWS.md)).

## 2. The line editor

`LineRead` edits one line from descriptor 0: printable bytes insert at the
cursor, CR or LF completes, BS or DEL erases, the cursor keys, Home, End and
Delete act in every form Section 1 names, and the Emacs control keys A, E, B, F,
P, N, U, K and D do what they do everywhere. Control-D ends input on an empty
line and deletes under the cursor otherwise. Anything else is ignored and
counted, leaving the line and the display untouched. The full table is
`LineFeed` in `line.h`.

- **It draws with printable characters, spaces and backspaces only**, assuming
  of the display only that a backspace moves left without erasing, so it works
  on a serial terminal and both consoles, none of which interprets a cursor
  sequence. An insertion writes the tail and backspaces over it; a deletion
  blanks the vacated cell; moving right rewrites the character under the cursor.
- **The history is thirty-two lines in a ring**, remembered by an explicit call
  because whether a line is worth keeping is the caller's decision. Empty lines
  and repeats of the newest are not kept. Walking up keeps the draft, and walking
  down past the newest restores it; editing a recalled line makes a new entry.
- **Its storage is static**, about seventeen kibibytes, so a prompt works before
  any heap exists and cannot fail for memory.
- **Editing and I/O are divided**: the editing, parsing and history write through
  an output function they are given, and only `LineRead` names a descriptor. The
  kernel's self-test gives it a function that records, so what each key draws is
  compared byte for byte, which is the only way to catch a redraw that leaves the
  line right and the screen wrong. `LineEdit` starts from a given text, which is
  how `micro` changes a line.

## 3. The grammar

The shell implements the subset of Section 2.10 that it acts on: a list of
and-ors separated by `;` and `&`, and-ors of pipelines joined by `&&` and `||`,
pipelines optionally negated by `!`, and simple commands of assignments, words
and redirections, every operator of Section 2.7 with its `io_number` but the
here-document. Tokenising is Section 2.3's rules and quoting Section 2.2's three
forms, each cited at its code.

- **What is outside the subset is refused by name**: `if`, `while`, `for`,
  `case`, `{ }`, `( )`, functions, `<<` and `;;` are answered `not implemented
  by this shell near 'if'`. A parser that did not know `if` would run a program
  called `if`, and the failure would look like a missing program. Reserved words
  are recognised only in command position, so `echo if` is a word.
- **Tokens keep their quotes** until the last step, because expansion must know
  what was quoted: `"$HOME"` expands and `'$HOME'` does not.
- **The parse is flat**: a list of pipelines, each with the condition before it
  and the separator after it, which is all a left-to-right shell needs.
- **Nothing allocates.** Every bound is in `shell.h` (eight redirections; eight
  commands to a pipeline; sixteen pipelines to a line; 128 tokens), and a line
  beyond one is refused naming which. A command has as many words as its line
  has tokens, drawn from one pool for the line, and so as many as `execve`
  accepts.
- **An unfinished line continues**: an open quote, a trailing `|`, `&&`, `||`
  or a redirection without a target prompts `> ` and the whole is parsed again
  with the next line joined, rather than resuming, which would be a second parser
  to assert.
- **The grammar is compiled into the kernel too**: the tokeniser, parser and
  expansion make no system call, so `SHELL_SOURCES` builds them into the kernel
  image and its self-test asserts the code the shell ships.

## 4. Variables and expansion

Variables are a fixed table of sixty-four names, each exported or not. The
expansions are `$NAME`, `${NAME}` and `$?`; an assignment word `NAME=value`
before a command's name is recorded as one (Section 2.10.2, rule 7) and after it
is a word.

- **Expansion and quote removal are one pass**, because an expanded value's
  characters are never quoting characters: done as two passes, a value holding a
  quote would be read as if typed. Single quotes keep `$` literal, double quotes
  expand, and a backslash escapes as Section 2.2 says.
- **Expansion takes a lookup function**, so the kernel's self-test asserts it
  against a table of its own and `$?` is the shell's to answer.

## 5. The working directory

The working directory is a canonical absolute path in the process control block,
`/` at creation, inherited by `fork` and kept by `execve`. `SyscallCopyUserPath`
joins every relative path to it, so every path-taking call resolves relative
paths the same way; a path that fits alone but not joined is `ENAMETOOLONG`.

- **`chdir` stores the lexically reduced path**, with no `.`, `..` or repeated
  separator and `..` at the root staying there, and first checks that it names a
  directory, so the refusal comes at `chdir` and not at some later call.
- **`getcwd` into a buffer too small** is `ENAMETOOLONG` from the kernel and
  `ERANGE` from the C library, the standard's name for it.
- **It is a path, not a held node**, the cheaper shape: a directory removed
  beneath a process leaves it naming nothing.

## 6. The built-ins

A built-in runs in the shell because a child could not change the shell's
directory, variables or jobs on its behalf.

| Built-in | Does |
| -------- | ---- |
| `cd [dir \| -]` | `chdir`, then sets and exports `PWD` and `OLDPWD`; no operand is `$HOME`, `-` is `$OLDPWD`. `CDPATH` and `-P` are not supported. |
| `pwd` | `getcwd`. |
| `export [-p] [name[=value]...]` | Sets and marks; with no operand lists the exported variables. |
| `unset name...` | Removes. |
| `exit [n]` | Ends the shell with `n` or `$?`, after flushing. |
| `true`, `false` | Status 0 and 1. |
| `help` | One line per command, built-ins then `/bin`. |
| `clear` | Writes a form feed. |
| `jobs`, `fg [%n]`, `bg [%n]`, `kill [-SIG] pid\|%n...` | Job control, Section 10. |

`exit` and `export` are special built-ins (Section 2.14), so an assignment before
one persists; before a regular built-in it is discarded.

**`clear` is a byte**, the form feed: the text display and the console clear
the screen on it, and the diagnostic path turns it into `ESC [ 2 J ESC [ H` for
the serial line alone, since a serial terminal ignores a form feed. A byte
crosses a pipe or a file, which a call would not.

**The prompt** is `oxys$`, the working directory, and `> `, asked of the kernel
at every prompt so it is the truth rather than the shell's record of it. The
continuation prompt is `> ` alone.

## 7. Running programs

A command that is not a built-in is a program. A name with a slash is a path;
otherwise each directory of `PATH` is tried in turn, `/bin` if `PATH` is unset
(Section 2.9.1.1).

- **The search is done in the child by `execve`**, because this kernel has no
  call that asks whether a file exists short of opening it: `ENOENT` tries the
  next directory, any other refusal is 126 and ends the search, nothing found is
  127 (Section 2.8.2).
- **The environment** is one `NAME=value` per exported variable, built before the
  fork and laid on the new stack by `execve`; `getenv` reads it. The kernel's
  vector bound of 128 strings applies.
- **A program is given** its arguments with quotes removed and expansions made,
  `argv[0]` as typed, the environment, the working directory, and every
  descriptor the shell holds, with 0, 1 and 2 the terminal and the diagnostic
  path unless redirected.
- **The status** is the program's code, or the encoded signal that ended it
  ([`PROCESS.md`](PROCESS.md)).

## 8. Redirection

Redirections are applied **in the child, between `fork` and `execve`, in the
order written** (Section 2.7), so `>out 2>&1` sends both to the file and
`2>&1 >out` does not. A failure ends the child with 1 before any program runs
on the wrong descriptors.

| Operator | The child |
| -------- | --------- |
| `[n]<word` | Opens for reading, placed at `n` (0). |
| `[n]>word`, `[n]>\|word` | Opens for writing, created or truncated, at `n` (1); with no `noclobber` the two are one. |
| `[n]>>word` | Opens for appending, created. |
| `[n]<>word` | Opens for reading and writing, created. |
| `[n]<&m`, `[n]>&m` | `dup2(m, n)`; `-` closes `n`. |

- **An open file counts its holders**, one per descriptor, so `dup2` and a
  child's inheritance share one file and one position, and the file is released
  at the last close ([`../storage/VFS.md`](../storage/VFS.md)).
- **A number below 3 that holds no file names the kernel's own path**, the
  terminal or the diagnostic path. It may be given to another of the three, as
  `2>&1` does, but not above them, since the table holds files and those paths
  are not files. A redirected 2 that is closed is the diagnostic path again.

## 9. Pipelines

`ShellRunPipeline` makes one child per command with a pipe between each pair
(Section 2.9.2), and the pipeline's status is the last command's, inverted by
`!`.

- **The pipe is placed before the command's own redirections**, so
  `a 2>&1 | b` sends diagnostics down the pipe.
- **Every pipe end is closed in the shell as soon as its child exists.** A
  reader sees the end of the file only when the last write end closes, and one
  left open in the shell would keep every reader waiting.
- **A built-in in a pipeline runs in the child**, the standard's subshell, so
  `help | wc -l` counts; `cd /bin | true` moves the child and not the shell.
- **The shell collects every child** in the order they end. A pipeline of one
  command runs in the shell itself, where a built-in must run.

**The pipe** is a page-sized queue between two open files with no node beneath
them ([`../storage/VFS.md`](../storage/VFS.md)):

- An empty pipe's reader sleeps until a write or the last writer's close, which
  reads as zero.
- A full pipe's writer sleeps until there is room for the whole of what remains,
  where that fits in a page, so no write is split or interleaved.
- A write with no reader is `EPIPE` and SIGPIPE.
- Readers and writers are counted as open files, not descriptors, so a child's
  inherited end does not close the pipe under its parent.
- A caller that cannot sleep, the kernel's own flow of control, is refused as
  busy rather than waiting for itself.

## 10. Job control

**Every pipeline, and every program run alone, is a job**: a process group led
by its first child. Parent and child both set the group, and for a foreground job
both give it the terminal (`tcgroup`, the standard's `tcsetpgrp`), because
neither knows which runs first after the fork.

- **The shell waits for a foreground job** with `waitpid` on its group and
  `WUNTRACED`, until every member has ended or every live one has stopped, then
  takes the terminal back. A stopped job's status is 128 plus SIGTSTP and it
  stays in the table. The table is doubled in the heap when every slot is taken;
  `n` is a job's slot and one, so slots are never compacted, or `%2` would come
  to name another job.
- **A background job (`&`) is announced** as `[n] pid`, and what became of it is
  reported before the next prompt.
- **`fg` and `bg` continue a job** in the foreground or background; **`kill`**
  sends a signal by name or number, SIGTERM by default, to a process or a whole
  job, and continues a stopped job it signals so the signal is acted on now.
- **The shell ignores SIGINT and SIGTSTP**, so control-C at its prompt does not
  end it, and each child restores the defaults before it becomes a program,
  since an ignore survives `execve` and a program that inherited it could not be
  interrupted.
- **A shell with no terminal**, as in a window, learns so from `tcgroup`
  answering `ENOTTY`, and then has no job control rather than a broken one:
  children stay in the shell's group, which the terminal emulator interrupts, and
  `fg`, `bg` and `kill %n` say there is no terminal.

## 11. The utilities of the shell

| Program | Does | Within |
| ------- | ---- | ------ |
| `micro FILE` | A line editor: `p` print, `a` append, `i N` insert, `d N` delete, `e N` edit line N with the arrow keys, `w` write, `q`, `q!`, `wq`, `h`. | A screen editor needs cursor sequences the display does not interpret. The file is written beside and renamed into place, so a failed write leaves it as it was. |
| `wc [-c\|-l\|-w]` | Counts lines, words and bytes; reads the standard input to its end. | |
| `head`, `tail` | `-n N` or `-N`; `tail` buffers its whole input, there being no `lseek`. | |
| `grep [-n\|-c\|-v\|-i]` | A fixed string, not a regular expression. | |
| `sort [-r]` | By byte. | |
| `mv` | `link` then `unlink`; `EXDEV` across volumes. | |
| `ps` | One line per process, by `procinfo`. | |
| `cat`, `cp`, `touch`, `rmdir`, `echo`, `ls`, `mkdir`, `rm` | As their names say. `cat` of the terminal ends at a control-D, which it treats as the end itself. | |

## Verification

The self-tests are [`../../kernel/test/terminal/terminal.c`](../../kernel/test/terminal/terminal.c),
[`../../kernel/test/libc/line.c`](../../kernel/test/libc/line.c),
[`../../kernel/test/shell/parser.c`](../../kernel/test/shell/parser.c) (the
grammar, expansion and six sessions of the shell at privilege level 3) and
[`../../kernel/test/proc/directory.c`](../../kernel/test/proc/directory.c), with
the programs `line-check`, `dir-check`, `env-check`, `file-check` and
`signal-check`.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A key becomes its byte and a release nothing; Enter is LF and Backspace BS; control applies to letters only and does not stick; the seven extended keys become their sequences; the queue keeps the first bytes at its bound; a flush empties it. | Doubled keystrokes; a cursor that moves the wrong way; the start of a line overwritten. |
| Each editing key draws exactly the bytes it should: insertion, erasure, kill, deletion, the cursor keys in every form, recall with the excess of a longer line blanked; unknown sequences change nothing; the 512th character is refused. | A redraw that leaves the line right and the screen wrong. |
| The history keeps order, skips blanks and repeats, displaces the oldest at 33, and keeps the draft. | A history that changes when recalled. |
| A session read through descriptor 0 edits five lines correctly and consumes exactly its bytes. | A reader that reads ahead or leaves bytes behind. |
| Tokens and their columns; every operator longest first; `io_number` only when adjacent; quotes kept; comments; incomplete lines; the bounds refused, not overrun. | `>>` read as two `>`, emptying a file. |
| Quote removal: single quotes literal, backslash within double quotes only before `$` `` ` `` `"` `\` and newline, adjacent parts joined. | `'\n'` becoming a newline. |
| The parse of lists, pipelines and redirections; continuations; unexpected tokens at their position; compound commands refused by name; two commands of twenty words read back whole and apart. | An empty pipeline run silently; a program called `if`; one command's words running into the next's in the line's pool. |
| Expansion: the longest name, `$?`, unset as empty, quoting respected, values with quotes passed through; assignments before and after the name. | `$HOMEx` expanding `$HOME`; a value closing a quote the person opened. |
| `dir-check`: the root at start; `chdir` and `getcwd` agree and `open` follows; paths canonicalised; `ENOENT`, `ENOTDIR`, `ERANGE` and `ENAMETOOLONG`; a child inherits and does not affect the parent. | A working directory `open` ignores; a child that starts at the root. |
| `env-check` receives its arguments and exported variables only; exit statuses of a program, a failure and a name not found; a parent's stack survives a child's `execve`. | Quotes reaching a program; an unexported variable leaking. |
| `file-check`: writes advance, append and truncate, shared positions through `dup2`, a closed 2 reverting, inheritance; pipes carry twelve kibibytes through a page intact, end at the last writer, refuse the wrong direction, and are `EPIPE` with no reader. | A duplicate closed from under another; a pipe that loses or duplicates bytes, or never ends. |
| Sessions compose statuses only if every step worked: assignments and `cd` (137); programs and the environment (168); every redirection operator, read back from the files; pipelines and `!` (10); job control by control-C, control-Z, `jobs`, `bg`, `fg` and `kill %1` (34). | Any step wrong, which changes the composed number. |

## Limitations

1. The terminal is raw: `fgets` on it delivers keystrokes, control sequences and
   all, and a control-C does not flush what was typed after it.
2. A line longer than the display wraps wrongly, the editor being unable to move
   the cursor up.
3. The keyboard and the serial line are merged without an order between them;
   the serial path is exercised by use, not asserted.
4. No compound commands, functions, subshells or here-documents; no positional
   parameters, `${NAME:-word}`, tilde, command substitution, arithmetic, field
   splitting or pathname expansion.
5. A redirection on a built-in outside a pipeline is named and not performed.
6. The environment and argument vectors are bounded at 128 strings, and a
   process at 1024 descriptors.
7. No SIGTTOU, `wait` built-in, `%%` or `%string` job names, `disown` or
   `suspend`; control-C at the prompt does not clear the line.
8. The working directory is a lexical path; symbolic links in it are not
   followed.
9. `grep` has no regular expressions, `sort` no `-n`, `-u` or `-k`, `tail` no
   `-f`, `ps` no options, and `mv` does not move a directory.
