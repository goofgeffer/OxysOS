<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Settings Application and the Notifications

**Phase**: sub-task 9.8 of [`../project/PLAN.md`](../project/PLAN.md), which
closes Phase 9, and the notifications.
**Source**: [`../../userland/settings/main.c`](../../userland/settings/main.c);
[`../../libc/config/edit.c`](../../libc/config/edit.c);
the notifications in [`../../graphics/client.c`](../../graphics/client.c) and
[`../../userland/session/main.c`](../../userland/session/main.c);
[`../../userland/notify/main.c`](../../userland/notify/main.c).
**Specifications**: none. The format being edited is this system's own,
[`CONFIG.md`](CONFIG.md).

## 1. The settings application, `/bin/settings`

A window of buttons, reached from the launcher, that edits the choices a person
makes of their desktop:

| Choice | File and key |
| ------ | ------------ |
| The background: each file of `/share/backgrounds`, or none | `/etc/session.conf`, `[session]` `background` |
| The size of the desktop: automatic, small or large | `/etc/session.conf`, `[session]` `scale` |
| Which launcher entries are pinned beside the launcher | `/etc/session.conf`, each `[launch]` block's `pin` |
| Which launcher entries stand on the desktop | `/etc/session.conf`, each `[launch]` block's `desktop` |

**Nothing is written until Save.** A press changes what is shown as chosen, and
Save writes it. Were every press an edit of `/etc`, a person trying the
backgrounds one after another would keep whichever they tried last. Revert reads
the files again.

**Save edits the text and not the settings.** The shipped files are mostly
comments explaining each key. Written back from the parsed settings, a file
would keep the settings and lose all of that, so the first Save would leave
`/etc` unable to explain itself. `OxysConfigEdit` changes the one line that
changes and nothing else ([`CONFIG.md`](CONFIG.md)). A choice turned off
(`pin`, `desktop`, or no background) removes its line.

**Save writes as `micro` does:** into a file beside the one edited, which takes
the name only once it is whole. A failure part way leaves the file as it was.

**After saving**, it posts `Settings saved.` with `SYSCALL_NOTIFY_RECONFIGURE`.
The session reads its configuration again before showing that notice, so the
new background and pins are on the screen when the notice appears. The size is
the exception: the session makes its panel and clock that size as it starts, so
a new size is used when the desktop next starts, and a second notice says so.

**What it does not edit.** `/etc/system.conf` lists the services `init`
starts, and a service removed by a stray press is a machine that does not start
its desktop; `micro` edits it, with every line in view. Launcher entries are
not added or removed here: an entry is a program's path and a name, which are
typed rather than chosen.

**Each button is as wide as its label measures**, by `window_text`'s
`SYSCALL_WINDOW_TEXT_MEASURE`, with a margin of four units either side, and a
press is matched against that width in pixels. The labels are proportional, so
a width counted in grid columns would leave a short label in a long button and
cut a long one at its edge. A choice is marked `>Rose<`, and every button of a
row is as wide as its marked label whether chosen or not: a button that widened
when chosen would push its neighbours along, and the next press would land on
a button the person had not aimed at.

**Every background and launcher entry has a button**, however many there are:
the lists, the buttons and the text Save edits are grown in the heap. Background
buttons wrap to a new row at the window's right margin, and the window is as
tall as its rows, never shorter than before. Each launcher entry has two
toggles, one in the list of pins and one in the list of what stands on the
desktop; the two lists share the screen's rows, each taking at most half, and
where a list has not the rows for every entry it goes on in further columns and
the window widens, so the second list's heading and Save and Revert are never
cut from its foot. A window cannot be resized by its
program and labels are measured through a window, so the layout is counted in a
first window and the window made again at the counted height before anything is
drawn; Revert does the same when the file has gained or lost entries. A list
with a bound would leave the entries past it with no button and Save leaving
their lines alone, with nothing to say why.

A background path of the person's own, which no button names, is kept until
another is chosen and shown as `Now:` beneath the buttons. An accent the four
buttons do not name is likewise left alone.

## 2. The notifications

Small windows at the bottom right of the screen, above the panel. Each has a
symbol at the left, in a coloured disc, and up to three lines of text at the
right. Each stays for five seconds, or until pressed.

| Kind | Symbol | Colour | For |
| ---- | ------ | ------ | --- |
| `SYSCALL_NOTIFY_INFORMATION` | `i` | blue | Something worth knowing. |
| `SYSCALL_NOTIFY_SUCCESS` | a tick | green | Something asked for was done. |
| `SYSCALL_NOTIFY_WARNING` | `i`, the information symbol | blue | Something missing was stood in for. |
| `SYSCALL_NOTIFY_ERROR` | `!` | red | Something failed. |

### 2.1 The path

Any program posts one with `notify`
([`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h)):
a kind, a flag, and at most 63 characters of text. The kernel keeps the last
eight in a ring and sends the session's root one `SYSCALL_WINDOW_EVENT_NOTIFY`.
The session takes them with `notification`, which is refused (`EPERM`) to any
other process.

**The kernel holds them, not the session**, because the notices worth having
are mostly about something missing, and the commonest moment for that is the
start, before the session exists. The ring keeps them, and the session takes
them all as it starts.

**The oldest is dropped when the ring is full**, where a window's event queue
drops the newest. A notice lives for seconds; eight waiting means the session
is not reading, and when it reads, the latest is what a person wants to see.

**The session posts its own the same way**, rather than drawing them directly,
so that there is one path and it is the one every program uses.

### 2.2 What posts them

| Posted by | When | Kind |
| --------- | ---- | ---- |
| The session | The background cannot be read (the shipped one is shown, or none) | warning |
| The session | The start icon cannot be read | warning |
| The session | A launcher entry's icon cannot be read; once per icon, not at every opening | warning |
| The session | `session.conf` offers nothing, and the shipped one is used | warning |
| The session's child | A program the launcher started could not be executed | error |
| `init` | `system.conf` has lines that could not be read | warning |
| `init` | A service could not be started, or kept ending and was stopped | error |
| `settings` | Saved, and the size needing a new start; or the save failed | success, information, error |
| `notify` | Whatever a script or a person says | any |

### 2.3 On the screen

At most three stand at once, the newest lowest, and the oldest is dismissed
early to make room: a column up the screen would cover the windows a person is
working in. Each is a panel-layer window, so nothing a program does covers it,
and it touches neither edge the window manager reserves rows for.

**The symbols are the project owner's**: `notify-information`,
`notify-success` and `notify-error` of `/share/icons`, read once as the session
starts and averaged down to the symbol's square. Warnings take the information
symbol; the kind still decides nothing else. **Where a
symbol cannot be read, the session draws one**: a disc of the kind's colour and a
white glyph of strokes, each pixel sampled four times to soften its edges. So the
notice saying an icon is missing never goes without its own symbol.

**The timing shares the one alarm a process has with the clock.** At each pass
of its loop the session cancels the alarm, which returns what remained of it;
what it asked for less what remained is exactly the time that passed. The
clock and every notice are aged by that, and the alarm is asked for at the
soonest of them. The interval timer's ticks are not used: the ABI does not say
how long a tick is, and the alarm counts milliseconds. The time passed is
settled before a new notice joins, or the notice would be aged by time that
passed before it existed, and one arriving late in a long wait would vanish at
once.

## 3. `notify`

`notify [-i|-s|-w|-e] text...` posts one from the shell or a script. A text
longer than 63 characters is refused and said, not cut, so a person who typed
it knows it was not shown as typed.

## Verification

| Asserted | The failure it would catch |
| -------- | -------------------------- |
| `window-check`: any program may post; a program not holding the session is refused `notification` with `EPERM`; a kind, a flag or a text beyond the bound is `EINVAL`; the session takes what was posted, kind, text and sender as posted; an empty ring is 0. | Another program reading the session's notices; a notice silently cut or mangled. |
| `KernelVerifyConfig`: five edits compared byte for byte with the text each should give (a value changed with its indentation kept, the second block of a list, a key new to its block, a key removed, a block made); the refusals leave the text unchanged; an edited text parses to what was edited. | A comment lost or another line touched by Save, which would still parse; a half-made edit. |

What only looking establishes, and was judged by eye: the four symbols
and the stacking; each notice gone five seconds after it appeared; the settings
window's layout; and Save changing the background and the pins while the window
stood, with `Settings saved.` shown, and `/etc/session.conf` read back from the
shell with no file left
beside them.

## Limitations

1. **The size needs a new start** of the desktop, as Section 1 says.
2. **Launcher entries are not added or removed**, and `system.conf` is not
   edited, here.
3. **The settings window is sized once for its layout.** It is not laid out again
   when made full, and a screen too narrow for the columns of pins has the
   last of them cut, which it says on its standard error.
4. **Notices are not kept.** One missed is gone; there is no list of past ones.
