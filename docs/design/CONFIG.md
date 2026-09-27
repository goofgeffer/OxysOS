<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The System Configuration

**Phase**: sub-task 9.4 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../libc/include/config.h`](../../libc/include/config.h) (format and
interface), [`../../libc/config/config.c`](../../libc/config/config.c) (parser),
[`../../libc/config/system.c`](../../libc/config/system.c) (the one place a file is
read); the files in [`../../etc/`](../../etc/); the readers
[`../../userland/init/main.c`](../../userland/init/main.c),
[`../../userland/settings/main.c`](../../userland/settings/main.c) and
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
  can also be appended to without rewriting the rest, which the settings application
  does.

| Rule | The failure it prevents |
| ---- | ----------------------- |
| A value is trimmed, and a comment after it is not part of it. | `run = /bin/session   # the desktop` naming a program with a comment in its path. |
| Quotes exist, needed only to keep a `#` in a value. | Noise on every line, or values silently cut at a hash. |
| Sections, keys and truth values ignore case. | `[Service]` quietly meaning something else. |
| A key given twice in one section is a fault; the first stands. | Two disagreeing lines, one silently winning. |
| A number that is not entirely a number, or a truth that is not one, gives the reader's fallback. | `scale = 2x` obeyed as 2; `restart = mabye` read as false. |
| Anything beyond a bound is refused and recorded, never truncated. | A path cut to a different path. |

## 2. The parser

`config.c` touches only memory: it parses a buffer someone else read, and calls
only the string functions and, where the caller supplies one, a grower.
`system.c` opens and reads the file. This split lets the kernel's boot-time
self-test, which cannot make system calls, assert the parser, while a program
asserts the file path ([`LIBC.md`](LIBC.md) uses the same seam throughout).

**The store starts fixed, and grows only where the reader lets it.** A static
`OxysConfig` holds 64 settings within it, so a file of the size a person writes
is parsed with no allocation, and `init` reading its services at boot cannot
fail for want of memory. Settings past those go to the heap through the
`grow` member, which `OxysConfigRead` supplies and a bare `OxysConfigParse`
does not: the kernel's self-test calls the parser directly, and an allocation
there would reach `brk`, which the kernel cannot execute. The text is read the
same way, into a 4 KiB buffer and then into the heap. Where there is no grower,
or the heap refuses, the line past the store is refused and recorded, never
dropped. Faults are kept to 8 (more are counted), since they are a report and
not the settings; a key, a section name and a value have the lengths in
`config.h`, which a line past is refused.

**Editing** is `edit.c`'s `OxysConfigEdit`, for the settings
application: one key of one block set or removed within the text of a file, every
other line left as written. It edits the text and not the parsed settings because
the shipped files are mostly comments, which a file written back from its settings
would lose. A changed line keeps its indentation; a new key goes after its block's
last setting, so a trailing comment stays with what it describes; a block is made
only as the next of its kind. A value that would not read back as itself (a
newline, a `#`, space at an end) is refused, and a refused edit changes nothing.
[`SETTINGS.md`](SETTINGS.md).

## 3. The files

`/etc` holds what a person may change without a rebuild. The files are
[`../../etc/`](../../etc/) in the repository, staged onto the ramdisk by the
`Makefile` (and also to `/share/defaults/etc`), so the file edited on the machine
and the file in the source are the same, and git shows changes to it.

| File | Read by | Contents |
| ---- | ------- | -------- |
| `system.conf` | `/bin/init`, at start | `[system]` `banner`; one `[service]` per supervised program. |
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

**Every key has a default**, so a missing or faulty file leaves the desktop as
it was, with the faults on the serial line; and a colour is named by a word
where the system has one, so that it stays right when the system's colour
changes.

**`[session]` and `[launch]`** are [`SESSION.md`](SESSION.md): the scale, the
background picture, and each launcher entry's `run`, `name`, `icon`, `pin`
(whether it also stands upon the bar beside the launcher) and `desktop` (whether
it also stands upon the desktop).

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
| An over-long value is refused, not cut; without a grower the store refuses beyond its capacity, and with one the settings past it are taken and read back; faults beyond those kept are counted. | A different path; a store overrun; a setting past the store lost or read from the wrong place; a file that looks nearly right. |
| A file the program wrote reads back as written; a missing file leaves the configuration empty. | Success reported on nothing; a previous file's settings left standing. |
| **The shipped files carry the keys the programs read**, read from `/share/defaults/etc` so that a person's own `/etc` cannot fail the test: a service running `/bin/session` with `needs = display`; `session.conf`'s `scale`, at least one `[launch]` with `run`, and a readable background; the shipped copies at `/share/defaults/etc`, readable and offering a launcher. | A key renamed in a program and not its file: a bare screen with every other test passing. |

That settings are **obeyed** is checked by eye: changing the background changes the
desktop and not the frames
([`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 10).

## Limitations

1. The kernel reads no configuration; its colours are constants, shared with
   programs at build time through `art/palette.h`. A kernel whose boot depended on
   editable text would fail on a typo.
2. No includes, overrides or per-user files. Files are read at start, except
   `session.conf`, which the launcher rereads when opened; a program wanting new
   settings is restarted.
3. Without an `oxys-etc` disk, `/etc` is the ramdisk's and edits are lost at boot.
4. Values are strings, numbers and truths; no lists within a value (a reader
   splits one itself), durations, sizes or paths.
5. A fault gives its line number, not its column or text.
6. Without a grower the parser holds 64 settings, and faults are kept to 8 and
   token lengths bounded (Section 2); beyond them, the excess is refused and
   reported.
