<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The System Configuration

**Phase**: sub-task 9.4 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../libc/include/config.h`](../../libc/include/config.h) (format and
interface), [`../../libc/config/config.c`](../../libc/config/config.c) (parser),
[`../../libc/config/system.c`](../../libc/config/system.c) (the one place a file is
read); the files in [`../../etc/`](../../etc/); the readers
[`../../userland/init/main.c`](../../userland/init/main.c),
[`../../userland/windows/main.c`](../../userland/windows/main.c) and
[`../../userland/session/main.c`](../../userland/session/main.c).
**Specifications**: none governs a configuration format; the familiar shape (a
comment, a bracketed section, `key = value`) is taken as a shape, not from any
implementation. ISO/IEC 9899:2011, Section 7.24, for the strings.

The format of `/etc`, the parser in the C library, and what each file means. The
files persist when a disk labelled `oxys-etc` is attached
([`../storage/PERSIST.md`](../storage/PERSIST.md)); read-only shipped copies are at
`/share/defaults/etc`.

## 1. The format

```
# A comment; so is the rest of any line after an unquoted hash.

[section]
key   = value
other = "a value # with a hash in it"

[section]           # the same name again: a list
key   = another
```

`[name]` opens a section; `key = value` sets a key in the last section opened; a
blank line is nothing. A value runs to the end of the line, trimmed, or is a quoted
string. **A repeated section is a list**: the third `[service]` is the third
element.

- **A line at a time, not a balanced structure.** In a nested format one missing
  brace makes the file unreadable. This file is read by the first user process at
  boot, where nobody can yet ask what went wrong. Here a bad line costs that line:
  it is recorded with its number and reason, the parse continues, and the reader
  decides whether it can go on. Faults are kept and printed, because a setting
  silently ignored costs someone an evening.
- **Sections, not dotted keys.** A list of services with dotted keys needs
  `service.1.run`: a person numbering what a machine can count. A repeated section
  can also be appended to without rewriting the rest, which a settings application
  will want.

| Rule | The failure it prevents |
| ---- | ----------------------- |
| A value is trimmed, and a comment after it is not part of it. | `run = /bin/windows   # the desktop` naming a program with a comment in its path. |
| Quotes exist, needed only to keep a `#` in a value. | Noise on every line, or values silently cut at a hash. |
| Sections, keys and truth values ignore case. | `[Service]` quietly meaning something else. |
| A key given twice in one section is a fault; the first stands. | Two disagreeing lines, one silently winning. |
| A number that is not entirely a number, or a truth that is not one, gives the reader's fallback. | `scale = 2x` obeyed as 2; `restart = mabye` read as false. |
| Anything beyond a bound is refused and recorded, never truncated. | A path cut to a different path. |

## 2. The parser

`config.c` touches only memory: it parses a buffer someone else read, allocates
nothing, and calls only the string functions. `system.c` opens and reads the file.
This split lets the kernel's boot-time self-test, which cannot make system calls,
assert the parser, while a program asserts the file path ([`LIBC.md`](LIBC.md) uses
the same seam throughout).

**The store is fixed and supplied by the caller**, a static `OxysConfig` of a few
kilobytes. A parser that called `malloc` could fail for a second reason at the
moment the machine can least report it. The bounds are 64 settings, 8 recorded
faults (more are counted) and 4 KiB of text.

## 3. The files

`/etc` holds what a person may change without a rebuild. The files are
[`../../etc/`](../../etc/) in the repository, staged onto the ramdisk by the
`Makefile` (and also to `/share/defaults/etc`), so the file edited on the machine
and the file in the source are the same, and git shows changes to it.

| File | Read by | Contents |
| ---- | ------- | -------- |
| `system.conf` | `/bin/init`, at start | `[system]` `banner`; one `[service]` per supervised program. |
| `desktop.conf` | `/bin/windows`, at start | `[desktop]` `scale`, `accent`. |
| `session.conf` | `/bin/session`, at start and at every opening of the launcher | `[session]` `scale`, `background`; one `[launch]` per launcher entry. |

**`[service]`** ([`INIT.md`](INIT.md)):

| Key | Meaning | If absent |
| --- | ------- | --------- |
| `run` | The program's path. | The block is refused and reported. |
| `name` | What `init` calls it in reports. | The path. |
| `restart` | `always`, or `never` for a program meant to end. | `always`. |
| `needs` | `display`: start only when the window manager has the screen. | Started on every boot entry. |

`needs = display` keeps the desktop off the entries that give the shell the
screen, where it would be refused, exit, and restart for ever. A service ending
five times in a row is given up on, and `init` says so. If `system.conf` cannot be
read at all, `init` reports it and starts its one built-in service rather than
leaving a bare screen.

**`[desktop]`**: `scale` is 1–4, or `0` to choose from the screen (shipped); others
are refused. `accent` is `system` (shipped), meaning the accent in
[`../../art/palette.h`](../../art/palette.h), or three numbers `r, g, b`. A word is
needed because every triple, including `0, 0, 0`, is a colour someone may want. A
file holding its own copy of the system's colour would be right until the system
changed it. The accent is the content's; the frame's colours belong to the window
manager in the kernel ([`WINDOWS.md`](WINDOWS.md)). Every key has a default, so a
missing or faulty file leaves the desktop as it was, with faults printed to
standard error (the serial line).

**`[session]` and `[launch]`** are [`SESSION.md`](SESSION.md): the scale, the
background picture, and each launcher entry's `run`, `name` and `icon`.

## Verification

`KernelVerifyConfig` in [`../../kernel/test/libc/config.c`](../../kernel/test/libc/config.c)
asserts the parser on text in memory, then runs `config-check`
([`../../userland/config-check/main.c`](../../userland/config-check/main.c)) at
privilege level 3 for the file half.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| Faultless text reports no fault, and every value is as written. | A parser that works only on its own example. |
| Values are trimmed; a trailing comment is excluded; a hash inside quotes is kept. | Programs named with comments; values cut at a hash. |
| A key with no value is the empty value, not absent. | "Set to nothing" indistinguishable from "not set". |
| Repeated sections are separate list elements, found regardless of case; none beyond the last. | Two services read as one: the wrong program started. |
| Negative numbers read; partial numbers, absent keys and non-truths give the fallback; `YES` is true. | `2x` as 2; `mabye` as false. |
| Each fault (not `key = value`, a setting before any section, unclosed bracket or quote, empty key, duplicate key) is recorded against its own line. | A person sent to the wrong line; a duplicate silently winning. |
| **The parse continues past a fault**, reading what follows. | One bad line costing the file. |
| An over-long value is refused, not cut; the store refuses beyond its capacity; faults beyond those kept are counted. | A different path; a store overrun; a file that looks nearly right. |
| A file the program wrote reads back as written; a missing file leaves the configuration empty. | Success reported on nothing; a previous file's settings left standing. |
| **The shipped files carry the keys the programs read**, read from `/share/defaults/etc` so that a person's own `/etc` cannot fail the test: a service running `/bin/session` with `needs = display`; `desktop.conf`'s `scale` and `accent`; `session.conf`'s `scale`, at least one `[launch]` with `run`, and a readable background; the shipped copies at `/share/defaults/etc`, readable and offering a launcher. | A key renamed in a program and not its file: a bare screen with every other test passing. |

That settings are **obeyed** is checked by eye: changing `accent` to three numbers
changes the colour of the desktop's discs and not the frames; `accent = system`
draws the palette's colour ([`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md)).

## Limitations

1. The kernel reads no configuration; its colours are constants, shared with
   programs at build time through `art/palette.h`. A kernel whose boot depended on
   editable text would fail on a typo.
2. No includes, overrides or per-user files. Files are read at start, except
   `session.conf`, which the launcher rereads when opened; a program wanting new
   settings is restarted.
3. Without an `oxys-etc` disk, `/etc` is the ramdisk's and edits are lost at boot.
4. Values are strings, numbers and truths; no lists within a value (the accent's
   three numbers are split by the desktop), durations, sizes or paths.
5. A fault gives its line number, not its column or text.
6. The parser's bounds (Section 2); beyond them, the excess is refused and
   reported.
