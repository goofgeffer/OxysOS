<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The System Configuration, its Format, and the `/etc` Hierarchy

**Phase**: 9, sub-task 9.4, of [`../project/PLAN.md`](../project/PLAN.md).
Section 1 is what this sub-task is; Section 2 is the format and every decision
in it; Section 3 is the `/etc` hierarchy and what each file it holds says;
Section 4 is what `init` and the desktop do with what they read; Section 5 is
the parser, and the seam that lets half of it be asserted before there is a
program; Section 6 is the verification; Section 7 the limitations.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../libc/include/config.h`](../../libc/include/config.h)
is the format and the interface;
[`../../libc/config/config.c`](../../libc/config/config.c) is the parser and
[`../../libc/config/system.c`](../../libc/config/system.c) the one place it
touches a file. The hierarchy is [`../../etc/`](../../etc/) in the repository and
`/etc` upon the initial ramdisk, which the `Makefile` stages there. Its readers
are [`../../userland/init/main.c`](../../userland/init/main.c) and
[`../../userland/windows/main.c`](../../userland/windows/main.c). Asserted by
[`../../kernel/test/libc/config.c`](../../kernel/test/libc/config.c), which runs
[`../../userland/config-check/main.c`](../../userland/config-check/main.c).

**Specifications**: none governs a configuration format, and none is claimed.
The shape below — a comment, a bracketed section, `key = value` — is the one
every system of this kind has had since the nineteen-eighties, and it is taken
as a shape and not from any implementation. ISO/IEC 9899:2011, Section 7.24, is
the string handling it is built upon.

## 1. What this sub-task is

Sub-task 9.3 left `init` knowing what to start because it was **written into
`init`**, and recorded that as the first of its limitations. A system whose list
of services is a constant in a program is a system that must be rebuilt to
change what it runs, and the desktop of 9.5, the utilities of 9.7 and the
settings application of 9.8 each need something to read and — for the last —
something to write.

**What this adds**: a format, a parser in the C library, an `/etc` holding two
files, and two programs that read them. **What it does not add** is a way to
edit them upon the running machine beyond `micro`; sub-task 9.8 is the
application that does, and it is why Section 2 cares that a repeated section can
be appended to without rewriting the file around it.

## 2. The format

```
# A comment, and so is the rest of any line after an unquoted hash.

[section]
key   = value
other = "a value # with a hash in it"

[section]           # the same name again: a list
key   = another
```

A line at a time. `[name]` opens a section; `key = value` sets a key within the
section opened last; a blank line is nothing. A value runs to the end of the
line with the space either side removed, or is a quoted string. **A section
repeated is a list**, and the third `[service]` block's keys are the third
element.

### 2.1 Why a line at a time, and not a format with brackets around the whole

A nested format — JSON, or anything with a shape that must balance — has **one
point of failure**: a brace that is missing makes the *file* unreadable rather
than the line. This file is read by the first user process, at boot, upon a
machine where nobody can yet ask what went wrong, and a format in which one bad
character costs everything is a format in which one bad character costs the
machine.

Here a line that cannot be read costs that line. It is recorded with its number
and its reason, the parse continues, and the caller decides whether it can
proceed without it. Every fault is retrievable and `init` prints them, because
**a setting silently ignored is a setting somebody will spend an evening looking
for** — which is the whole reason the faults are kept rather than counted.

### 2.2 Why sections rather than dotted keys

`service.run` needs no sections, but a *list* of services would then need
`service.1.run`, and a person hand-writing a file would be numbering things a
machine can count. A repeated section is the simplest list a line-oriented
format has. It is also the one shape a settings application can **append to**
without rewriting what is already there, which 9.8 will want and which a dotted
scheme with numbers in it would make into a renumbering.

### 2.3 The decisions in the small

| Decision | The failure it prevents |
| -------- | ----------------------- |
| A value is trimmed of surrounding space, and a comment after it is not part of it | `run = /bin/windows   # the desktop` otherwise names a program whose path has a comment in it, and the service never starts |
| Quoting exists, and is needed only where a `#` is wanted in a value | Requiring quotes everywhere makes every line noisier for the one line in a hundred that needs one; having no quotes at all cuts a value at a hash with nothing said |
| Sections, keys and truths compare without regard to case | `[Service]` meaning something other than `[service]` is a distinction nobody expects a configuration to make, and one nothing would report |
| A key given twice in one section is a fault, and the first stands | Two lines that disagree, of which one silently wins. A person who meant a list wrote one section twice; a person who meant an override wrote a mistake |
| A number that is not entirely a number gives the fallback | `scale = 2x` read as 2 is obeying something nobody wrote; and a zero returned instead would be a value somebody might have meant |
| A truth that is not one gives the fallback | The same, for `restart = mabye` |
| Everything beyond a bound is refused and recorded, never truncated | A path cut to thirty-one characters is a path that names a different file, and nothing says so |

## 3. The `/etc` hierarchy

`/etc` holds the configuration a program reads at start. It is two files, and
the rule for what belongs there is: **what a person may change without a
rebuild, and what more than a moment's work would be needed to discover the
default of.**

| File | Read by | What it says |
| ---- | ------- | ------------ |
| `/etc/system.conf` | `/bin/init` | `[system]`, a banner it prints; and a `[service]` block for each program `init` starts, with `run`, `name`, `restart` and `needs`. |
| `/etc/desktop.conf` | `/bin/windows` | `[desktop]`, the scale its windows are drawn at and the accent its contents are drawn with — `system` for the palette's. |
| `/etc/session.conf` | `/bin/session` | `[session]`, the scale the desktop is drawn at; and a `[launch]` block for each entry of the launcher, with `run`, `name` and — since sub-task 9.6 — `icon`, a path to a picture the launcher draws beside the name. |

They are **files in the repository**, at [`../../etc/`](../../etc/), staged onto
the initial ramdisk by the `Makefile` rather than written by a recipe: the thing
a person edits upon the running machine and the thing they edit in the source
are then the same file, and a change to one is a change git can show.

`/etc` is writable, the ramdisk being this kernel's own —
[`../storage/INITRD.md`](../storage/INITRD.md), Section 6 — so `micro` can edit
either upon the running machine, and a settings application will be able to.
What it cannot yet do is survive a reboot, the ramdisk being memory; Section 7,
limitation 3.

## 4. What is done with what is read

### 4.1 `init` and its services

Each `[service]` block is a program `init` starts and supervises:

| Key | Means | Absent |
| --- | ----- | ------ |
| `run` | The program's path. | The block is refused and said so; there is nothing to start. |
| `name` | What `init` calls it when it reports. | The path is used. |
| `restart` | `always`, or `never` for a program that is meant to end. | `always`. |
| `needs` | `display`, for a service that wants the window manager to have the screen. | Nothing; it is started upon every entry. |

**`needs = display` is what keeps the desktop off the entries that give the
shell the screen.** Without it, `/bin/windows` would be started there, refused
by `window_screen` with `ENOTSUP`, exit, and be started again — which is a
restart loop upon exactly the entries a person boots when something is wrong.

**A service that keeps ending is given up on**, after five consecutive endings,
and `init` says so. That is a bound upon consecutive failures and not a rate,
there being no clock a program may read
([`LIBC.md`](LIBC.md), Section 13) — which is what
[`INIT.md`](INIT.md), Section 7, limitation 3, recorded as owed. It is enough
for the failure it exists to stop: a service whose program is missing, or which
faults upon its first instruction, would otherwise be restarted for the
machine's whole life. Observed, with the path made wrong on purpose:

```
init: desktop ended 5 times in a row, last with status 0x7f; it is not started again.
```

**A configuration that cannot be read at all is not a machine with no
supervisor.** `init` says so and falls back to the one service it would
otherwise have had written into it. The alternative is a machine that boots to a
bare screen because somebody mistyped a path, and the person best placed to
notice is the one who then cannot start anything.

### 4.2 The desktop and its appearance

`/bin/windows` reads `scale` and `accent`. Every key has a default already in
place, so a file that is absent, unreadable, or wrong in a line **leaves the
desktop looking as it did** rather than not appearing — the faults are printed
upon the standard error, which is the serial line, and the drawing goes on.

`scale = 0`, which is what the file ships with, means the program chooses from
the screen it is given, which is what it did before there was a file. A scale
outside one to four is refused rather than obeyed: a window drawn at sixteen
times the face fits upon no screen this system has, and a desktop nobody can see
is worse than one that ignored a setting.

**The accent is the content's and not the frame's.** The frame's colours are the
window manager's, [`WINDOWS.md`](WINDOWS.md), Section 4, and the kernel does not
read this file — a kernel that parsed text a person may edit would be a kernel
whose boot depends upon it. The file says so where a person will read it.

`accent = system`, which is what the file ships with since 2026-09-21, is the
accent [`../../art/palette.h`](../../art/palette.h) carries — the colour the
boot screen, the window frames and the session already draw, and so the one key
in this file whose value is decided somewhere else. Three numbers instead still
override it, and a value that is neither is still reported and the default left
standing.

**A colour has no spare number to mean "the one the system chose" the way
`scale = 0` does**: `0, 0, 0` is black, and black is a colour somebody may
actually want. The sentinel is therefore a word. The file carried the three
numbers themselves until that date, and by then they were a blue left over from
the scheme that preceded the yellow one — a file holding its own copy of a
colour the system has already decided is a file that is right until the day the
system changes its mind and wrong from then on, with nothing saying so. That is
Section 6.1's failure reached from the other end: there the file and the program
disagreed about the name of a key, here they would have disagreed about the
value of one.

## 5. The parser, and the seam

[`../../libc/config/config.c`](../../libc/config/config.c) touches nothing but
memory: the text it parses is a buffer somebody else obtained, it allocates
nothing, and it calls nothing but the string functions of sub-task 7.1.
[`../../libc/config/system.c`](../../libc/config/system.c) is the one place a
file is opened and read.

**This is the arrangement of [`LIBC.md`](LIBC.md), Sections 9 to 11, a fourth
time**, and for the same reason: what is ordinary C can be asserted by the
kernel's boot-time self-test, which cannot execute a system call, and what
reaches a descriptor can be asserted only by a program, which can. The line
editor of sub-task 8.1 was the third, and its header records the same seam.

**The store is fixed and the caller supplies it.** `OxysConfig` is some
kilobytes and is meant to be a static object. A parser that called `malloc`
would be one that can fail for a second reason at the moment the machine is
least able to report it; the bounds are large enough for a configuration a
person wrote and small enough to be honest about, and everything beyond them is
a recorded fault.

## 6. Verification

Two halves, and the division is the seam of Section 5.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A text with no fault reports none, and every value is what was written | A parser that works upon the file it was written against and nothing else |
| The space either side of a value is removed; a comment after a value is not part of it | `run = /bin/windows # the desktop` naming a program that does not exist |
| A key with no value is the empty value, not an absent key | A caller that cannot tell "set to nothing" from "not set" |
| A hash within a quoted value is kept | A path or a message cut at its hash, silently |
| A repeated section is a list, its blocks kept apart by occurrence; a section is found whatever its case; a block beyond the last is not answered | Two services read as one, or the second's keys found in the first — a machine that starts the wrong program |
| A negative number is read; a value that is not entirely a number gives the fallback, as does an absent key; `YES` is a truth and a non-truth gives the fallback | `scale = 2x` obeyed as 2; `restart = mabye` read as false and a service never restarted |
| Each fault — a line that is not `key = value`, a setting before any section, an unclosed bracket, an empty key, an unclosed quote, a key given twice — is recorded **against the line it stands upon** | A fault reported against the wrong line, which sends a person to the wrong place; and a second key of one name silently replacing the first |
| **The parse carries on past a fault**, and what follows a bad line is still read | The whole point of the format lost: one bad line costing the file, and the machine. **Observed**, Section 6.1 |
| A value beyond the bound is refused and not kept cut to the bound | A path truncated to a different path |
| The store refuses entries beyond its capacity, and faults beyond what it keeps are counted | A store written past its end; a file wrong everywhere reporting only the first few faults with no sign there were more |
| **A file this program wrote is read back as written**; a file that is not there leaves the configuration empty | A read that reported success upon nothing, or one that left the previous file's settings standing — a program then acting upon a configuration it did not read |
| **The files `/etc` ships carry the keys the programs read**: a service naming `/bin/windows` with `needs = display`, and the desktop's `scale` and `accent` | A key renamed in a program and not in the file — a machine that boots to a bare screen with every other test still passing. **Observed**, Section 6.1 |

### 6.1 The damage applied, and what the tests said

**The parse made to stop at the first fault** — one `break` added:

```
Configuration: asserting the format upon text in memory, then running config-check.
  a bad line stopped the parse; what followed it was lost
  the faults beyond the store's bound were not counted
Configuration self-test FAILED.
```

**The shipped configuration made to name a program that is not there** —
`/bin/windows` changed to `/bin/desktop` in `/etc/system.conf`:

```
config-check: the configuration read from a file, from privilege level 3.
  /etc/system.conf does not name /bin/windows as a service FAILED.
config-check: 1 assertion(s) failed.
Configuration self-test FAILED.
```

The second is the one worth having. Nothing in the parser was wrong and nothing
in `init` was wrong; the file and the program simply disagreed, which is the
failure that a system with a configuration acquires the day it acquires one, and
it is invisible to every test that does not read the shipped file.

### 6.2 What only looking establishes

That the configuration is **actually obeyed**. The desktop's accent was changed
in `/etc/desktop.conf` from `79, 134, 247` to `230, 120, 40`, the image rebuilt,
and the desktop's discs drew orange where they had drawn blue — while the
window frame's band stayed blue, the frame being the window manager's and not
the program's, exactly as the file says. The procedure is
[`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 10.

**The sentinel was established the same way, and in both directions**, on
2026-09-21: with `accent = system` the discs drew the tan of the mark, which is
the colour [`../../art/palette.h`](../../art/palette.h) carries; with
`accent = 20, 200, 20` written in its place and the image rebuilt they drew
green. A sentinel observed only one way is a sentinel that may simply be the
default standing because the file is not being read at all, and that looks
identical from the outside.

## 7. Limitations

1. **The kernel reads none of it.** The window manager's palette, the boot
   screen and every other thing the kernel draws are constants in the kernel. A
   kernel that parsed text a person may edit would be a kernel whose boot
   depends upon that text being right; what belongs in a file is what a program
   reads.
   Since 2026-09-21 the desktop draws its accent from those same constants,
   [`../../art/palette.h`](../../art/palette.h) being compiled into the kernel
   and into the program alike: that is agreement reached at build time rather
   than by reading, it costs a rebuild to change where a file costs a restart,
   and it leaves the kernel depending upon no text a person may edit.
2. **There is no include, no override and no per-user file.** One file per
   subject, read once at start. A program that wants its configuration again
   must be started again, nothing yet asking to be told that a file changed.
3. **Nothing survives a reboot.** `/etc` is upon the initial ramdisk, which is
   memory: a file edited upon the running machine is gone at the next boot. What
   would keep it is a writable volume mounted at boot, which is
   [`../storage/VFS.md`](../storage/VFS.md)'s to arrange and which nothing yet
   asks for.
4. **The values are strings, numbers and truths.** There is no list within a
   value — the desktop's accent is three numbers parsed by the desktop itself,
   not by the parser — and no duration, no size with a suffix, and no path type.
   Each of those is a conversion the parser could grow the day two programs want
   the same one.
5. **A fault is reported and not located within its line.** The line number is
   kept; the column is not, and neither is the text of the line, so a person is
   sent to the line and reads it themselves.
6. **The bounds are the parser's, not the file's**: sixty-four settings, eight
   faults kept, four kilobytes of text. A configuration beyond any of them is
   refused in the part that exceeds it and says so, which is honest but is not
   the same as reading it.
