<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `art/` — The Mark, and What Is Generated From It

**Phase**: 9, beside sub-task 9.5, of [`../docs/project/PLAN.md`](../docs/project/PLAN.md).
**Detailed design**: [`../docs/design/SESSION.md`](../docs/design/SESSION.md),
Section 3.2, which is where the mark is drawn and why the boot screen and the
desktop must draw the same one.
**Licence**: CC0-1.0, and that is the reason this directory exists rather than
the artwork living beside whichever program draws it —
[`../LICENSING.md`](../LICENSING.md), Section 1.

## Purpose

The mark of Oxys-OS, the bitmap generated from it, and the colours the system
draws itself in.

| File | What it is |
| ---- | ---------- |
| [`logo.png`](logo.png) | The mark as the project owner drew it: a disc with a figure upon it, on a white ground. It is the source and nothing reads it at build time. |
| [`logo.h`](logo.h) | The same, cropped to the disc, reduced to ninety-six pixels square, and classified into three states at two bits to a pixel. This is what is compiled in. |
| [`palette.h`](palette.h) | The colours this system draws itself in: the ground, the two bars, the ink, the dim, the paper, the border and the disc, with `OXYS_RGB` to pack three channels into the pixel a program's window takes. Nothing is generated; these are numbers somebody chose. |

## Why the artwork is CC0 and lives here

It is drawn upon by two things under two different licences: the kernel's boot
screen and power screen, which are `LGPL-3.0-or-later`, and the session, which
is `MIT`. Artwork that belonged to either could not be used by the other without
a relicensing this project may not perform — the same wall the font stands
behind, [`../docs/design/SESSION.md`](../docs/design/SESSION.md), Section 4 — so
the mark is placed in the public domain, where both may take it, and kept in one
directory so that there is one of it.

## Why `logo.h` is generated and not drawn

Every other picture in this system was drawn by hand into a table — the face of
sub-task 6.4, the pointer of 6.5 — because each is a few dozen bytes and a
person can see the shape in the source. This is nine thousand pixels of somebody
else's drawing, and a hand transcription of it would be a copy that drifts.

It is generated, once, by a command recorded here rather than by a rule in the
`Makefile`. A build rule would mean every build depended upon ImageMagick, which
[`../docs/project/TOOLCHAIN.md`](../docs/project/TOOLCHAIN.md) does not require
and which nothing else here needs; and the mark changes when somebody draws a
new one, not when somebody builds.

```sh
convert art/logo.png -trim +repage -resize 96x96! -depth 8 txt:- | ...
```

The full command is in the commit that added this directory. The classification
it performs is the whole of the judgement:

| The pixel | Becomes |
| --------- | ------- |
| Luminance below 100 | `LOGO_INK` — the outline and the figure |
| Nearly grey and bright: channels within 30 of each other, luminance above 190 | `LOGO_NOTHING` — the white ground, which is removed so that the mark sits upon whatever is behind it |
| Anything else | `LOGO_DISC` — the disc itself |

**Three states and not two.** A mask of one bit would make the figure and the
ground one thing, and the mark is a figure *upon* a disc: a caller draws the
disc in one colour and the figure in another, and sees neither where the ground
shows through.

**The colours are not in the file.** A caller supplies them, as every drawing in
this system does, because a pixel value means nothing without an encoding — the
kernel's is `FramebufferEncode` and a program's is the `0x00RRGGBB` the window
protocol carries.

## What reads it

[`../kernel/kernel.c`](../kernel/kernel.c), for the boot screen and the power
screen; [`../userland/session/main.c`](../userland/session/main.c), for the
desktop root; and [`../userland/windows/main.c`](../userland/windows/main.c),
for the window the demonstration opens. All three draw the same bitmap at the
same size, which is what makes the hand-over from the boot screen to the
desktop look like one machine starting rather than pictures replacing each
other.

The third was added on 2026-09-21, a day after the other two. It had a ring of
discs this project drew before there was artwork to draw, and the ring outlived
its reason: for that day the screen a person boots to carried the owner's mark
and the window a person opens carried the stand-in. A directory of artwork
makes a mark easy to share and does nothing at all about a program that never
asked for it.

`palette.h` is read by the same three. `/bin/windows` takes its paper, its ink
and its accent from here; it held its own copy of the three until the same day,
and the copy was a blue from the scheme that preceded the yellow one, which had
looked like the system for exactly as long as the system did not change. The
coral and the mint it draws beside them stay its own, because a colour belongs
here when two things must agree about it and nothing else draws those.

`/etc/desktop.conf` ships `accent = system` to name this header from a file —
[`../docs/design/CONFIG.md`](../docs/design/CONFIG.md), Section 4.2.
