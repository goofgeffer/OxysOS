<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `art/` — The Mark, and What Is Generated From It

**Phase**: 9, beside sub-task 9.5, of [`../docs/project/PLAN.md`](../docs/project/PLAN.md).
**Detailed design**: [`../docs/design/SESSION.md`](../docs/design/SESSION.md)
which is where the mark is drawn and why the boot screen and the
desktop must draw the same one.
**Licence**: CC0-1.0, and that is the reason this directory exists rather than
the artwork living beside whichever program draws it —
[`../LICENSING.md`](../LICENSING.md), Section 1.

## Purpose
The mark of Oxys-OS, the bitmap generated from it, the colours the system
draws itself in, the icons its launcher draws, and the background the desktop
is covered with. The icons and the background are files upon the system's own
filesystem — `/share/icons` and `/share/backgrounds` of the ramdisk — and are
the things here that are not compiled in.
the one thing here that is not compiled in.

| File | What it is |
| ---- | ---------- |
| [`logo.png`](logo.png) | The mark as the project owner drew it: a disc with a figure upon it, on a white ground. It is the source and nothing reads it at build time. |
| [`logo.h`](logo.h) | The same, cropped to the disc and reduced to 192 pixels square, each pixel carrying how much of it the mark covers and how much of it is ink, four bits of each. This is what is compiled in. |
| [`palette.h`](palette.h) | The colours this system draws itself in: the ground, the two bars, the ink, the dim, the paper, the border and the disc, with `OXYS_RGB` to pack three channels into the pixel a program's window takes. Nothing is generated; these are numbers somebody chose. |

## Why the artwork is CC0 and lives here

It is drawn upon by two things under two different licences: the kernel's boot
screen and power screen, which are `LGPL-3.0-or-later`, and the session, which
is `MIT`. Artwork that belonged to either could not be used by the other without
a relicensing this project may not perform — the same wall the font stands
behind, [`../docs/design/SESSION.md`](../docs/design/SESSION.md) — so
the mark is placed in the public domain, where both may take it, and kept in one
directory so that there is one of it.

## Why `logo.h` is generated and not drawn

Every other picture in this system was drawn by hand into a table — the face of
sub-task 6.4, the pointer of 6.5 — because each is a few dozen bytes and a
person can see the shape in the source. This is thirty-seven thousand pixels of
somebody else's drawing, and a hand transcription of it would be a copy that
drifts.

It is generated, once, by a command recorded here rather than by a rule in the
`Makefile`. A build rule would mean every build depended upon ImageMagick, which
[`../docs/project/TOOLCHAIN.md`](../docs/project/TOOLCHAIN.md) does not require
and which nothing else here needs; and the mark changes when somebody draws a
new one, not when somebody builds. It is written for ImageMagick 6, whose
`-extent` takes no expression, which is why the side is measured first.

```sh
S=$(convert art/logo.png -trim -format '%[fx:max(w,h)]' info:)
convert art/logo.png -trim +repage -gravity center -background white \
        -extent ${S}x${S} -colorspace Gray square.png
convert square.png -threshold 74.5% -negate -filter box -resize 192x192! -depth 8 gray:covered
convert square.png -threshold 39.2% -negate -filter box -resize 192x192! -depth 8 gray:inked
paste -d' ' <(od -An -v -tu1 -w1 covered) <(od -An -v -tu1 -w1 inked) |
  awk '{ c = int(($1 * 15) / 255 + 0.5); i = int(($2 * 15) / 255 + 0.5);
         if (i > c) i = c;
         printf "%s0x%X%X,", (n % 12 == 0) ? "    " : " ", c, i;
         if (++n % 12 == 0) printf "\n" }'
```

The output is the body of `LogoCoverage`, and replaces the one in `logo.h`
between its opening brace and its closing one. What it does is the whole of the
judgement:

| Step | Why |
| ---- | --- |
| Trimmed, then padded to a square of white | The artwork is a disc a little wider than it is tall once trimmed. Stretched to a square, as the table of three states was, it would be an ellipse by half a percent; padded, it is the disc as drawn. |
| Covered where the luminance is below 190 of 255 (74.5%) | The disc is a tan near 164 and the ground is white; everything that is not the ground is the mark. |
| Ink where the luminance is below 100 of 255 (39.2%) | The outline and the figure. Everything that is ink is also covered, since 100 is below 190. |
| Each of the two decided at the artwork's full resolution, and only then reduced with a box filter | A pixel of the table is then the *fraction* of the artwork beneath it that is covered, and that is ink. Reduced first and decided afterwards, a pixel on the edge is still all or nothing, and the edge is the staircase this table replaced. |
| Four bits of each, the ink clamped to the coverage | Sixteen levels are more than a person tells apart upon an edge one pixel wide; and `LogoMix` subtracts the ink from the coverage, so the clamp is what keeps a rounding the wrong way from being a speck of some other colour. |

**Coverage and not states.** The table of 2026-09-20 to 2026-09-23 was
ninety-six pixels square with three states — nothing, disc, ink — and was drawn
two screen pixels to a pixel upon every screen of 1024 or wider, which is every
screen but VirtualBox's. Its edge was a staircase of two-pixel steps, and
nothing about the three states could say "half of this pixel is the disc". A
table of coverage can; and it is drawn one to one at the scale of two and
averaged by `LogoSample` at the scale of one, so the mark is smooth upon both.
The size a caller lays the mark out by, `LOGO_UNITS`, is still ninety-six, so
nothing about the screens the mark is drawn upon moved.

**The colours are not in the file.** A caller supplies them, as every drawing in
this system does, because a pixel value means nothing without an encoding — the
kernel's is `FramebufferEncode` and a program's is the `0x00RRGGBB` the window
protocol carries. The caller supplies the colour of the ground too, because a
pixel of the edge is partly the ground; which means a caller must draw the mark
upon the colour it says the ground is, or the edge carries a fringe of the
colour it named instead.

[`../kernel/test/gfx/mark.c`](../kernel/test/gfx/mark.c) asserts every byte of
the table has no more ink than coverage, that the corners are not covered, that
there is a pixel of the edge, and that the sampling and the mixing are exact at
the points that can be computed by hand.

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

`/bin/terminal` of sub-task 9.6 takes neither, and that is the same rule seen
from the other side: it draws white upon black, because a window of text read
for minutes at a time is not the problem a label upon a panel is, and no second
thing has to agree with it about either colour.

`/etc/desktop.conf` ships `accent = system` to name this header from a file —
[`../docs/design/CONFIG.md`](../docs/design/CONFIG.md).

## `icons/` — the pictures the launcher draws, which are files

| File | What it is |
| ---- | ---------- |
| [`icons/terminal.png`](icons/terminal.png) | The terminal's icon as the project owner drew it. It is the source and nothing reads it at build time. |
| [`icons/terminal.oxi`](icons/terminal.oxi) | The same, upon nothing rather than white, reduced to forty-eight pixels square with the transparency of every pixel kept, and written in version 2 of the format [`../libc/include/icon.h`](../libc/include/icon.h) sets out. This is what the ramdisk carries and what the session reads. |

**They are files and not a header**, which is the whole difference between an
icon and the mark above. There is one mark and it is drawn before there is a
filesystem; there is one icon per program, the set grows whenever somebody adds
an entry to `/etc/session.conf`, and nothing draws one before `/` is mounted. A
picture compiled in would need the system rebuilt to change, and a launcher
whose entries are read from a file cannot have its pictures fixed at compile
time without the two disagreeing the first time somebody edits it.
[`../docs/design/SESSION.md`](../docs/design/SESSION.md).

The conversion is one command, recorded here rather than made a rule of the
`Makefile` — for the reason `logo.h`'s is: a build rule would put ImageMagick in
the path of every build, and an icon changes when somebody draws one, not when
somebody builds. `E` is the extent; forty-eight is the launcher's slot of
twenty-four units at the scale of two.

```sh
E=48
convert art/icons/terminal.png -alpha set -fuzz 10% -fill none \
        -draw 'color 0,0 floodfill' -trim +repage cut.png
S=$(convert cut.png -format '%[fx:max(w,h)]' info:)
convert cut.png -background none -gravity center -extent ${S}x${S} \
        -filter box -resize ${E}x${E} -depth 8 rgba:raw
{ printf 'OXIC\002'; printf "\\$(printf %03o $E)\\$(printf %03o $E)\\000"; } > head
xxd -p -c 4 raw | awk '{ printf "%s%s%s%02x", substr($0,5,2), substr($0,3,2),
        substr($0,1,2), 255 - strtonum("0x" substr($0,7,2)) }' | xxd -r -p > pixels
cat head pixels > art/icons/terminal.oxi
```

Four things in that are the whole of the judgement.

**The ground is made transparent at the artwork's full resolution**, by a flood
from a corner, because the drawing arrived opaque — a picture upon white, not a
picture upon nothing. `-background none` alone says what lies *outside* the
picture and nothing about the white inside it, so at this extent the rounded
corners of the frame would be specks of white upon the panel. The self-test of
[`../kernel/test/libc/icon.c`](../kernel/test/libc/icon.c) refuses a shipped
icon with no transparent pixel for exactly that failure, taken to the whole
square.

**Padded to a square, centred**, so that a picture of any shape becomes the
square the launcher's slot was sized for, padded rather than stretched.

**Reduced after the ground is gone, with a box filter**, so that a pixel of the
edge carries the fraction of the drawing beneath it as its opacity. The
conversion this replaced reduced to twenty-four and then made every pixel below
half alpha nothing and every other opaque, because version 1 of the format had
no answer between; the edge was a staircase, and the session enlarged it by two.
Version 2 carries the fraction, and the session mixes it with the panel —
`OxysIconCompose`, [`../docs/design/SESSION.md`](../docs/design/SESSION.md).

**Transparency and not opacity** in the top byte, `255 - alpha`, so that a pixel
wholly opaque has a top byte of zero and is the `0x00RRGGBB` the window protocol
carries, and a pixel wholly transparent is `ICON_NOTHING`, as each was in
version 1.

The header's five significant bytes are `OXIC`, the version 2, the width 0x30,
the height 0x30 and a reserved zero; the pixels are little-endian, which is why
the `awk` writes blue, green, red and then the transparency.

## `backgrounds/` — the picture the desktop is covered with, which is a file

| File | What it is |
| ---- | ---------- |
| [`backgrounds/background.png`](backgrounds/background.png) | The background as the project owner drew it, 2048 by 1448. It is the source and nothing reads it at build time. |
| [`backgrounds/background.oxim`](backgrounds/background.oxim) | The same, at the same resolution, in the run-length format [`../libc/include/image.h`](../libc/include/image.h) sets out: seventy kilobytes. This is what the ramdisk carries at `/share/backgrounds/background.oxim`, what `/etc/session.conf` names, and what the session reads. |

**A file, for the icons' reason and one more.** The session reads it once at
start and a person changes it by editing one line of `/etc/session.conf`; and a
background compiled in would be the size of the drawing in the session's own
image, where a file costs the ramdisk and nothing else.

**At the resolution it was drawn at, and scaled by the session.** The picture
is not reduced to a screen size here, because there is no one screen size —
1280 by 800 under QEMU, 1024 by 768 under Bochs, 640 by 480 under VirtualBox —
and a picture reduced for one is enlarged for the others. The session's scaler
covers whatever screen it has, averaging as it reduces. What that costs is
nothing, for a drawing of flat colour: its three million pixels are twelve
thousand runs.

**Covering, not stretching.** The drawing is wider than 4 by 3 and narrower than
16 by 10, so upon every screen above it is cut at two edges — the sides at 4 by
3, the top and the foot at 16 by 10 — and is never distorted. A drawing whose
important part is near an edge loses it upon some screen; draw with a margin.

The conversion is one command, for the reason every conversion here is:

```sh
in=art/backgrounds/background.png; out=art/backgrounds/background.oxim
W=$(identify -format '%w' "$in"); H=$(identify -format '%h' "$in")
{ printf 'OXIM\001\000\000\000'
  printf "\\$(printf %03o $((W & 255)))\\$(printf %03o $((W >> 8)))"
  printf "\\$(printf %03o $((H & 255)))\\$(printf %03o $((H >> 8)))"; } > "$out"
convert "$in" -background white -alpha remove -depth 8 rgb:- | xxd -p -c 3 |
  awk -v W=$W '
    function flush() { printf "%02x%02x%s%s%s00", n % 256, int(n / 256),
                         substr(p,5,2), substr(p,3,2), substr(p,1,2); }
    { if (n > 0 && ($0 != p || x == 0 || n == 65535)) { flush(); n = 0 }
      if (n == 0) p = $0; n++; x = (x + 1) % W }
    END { if (n > 0) flush() }' | xxd -r -p >> "$out"
```

Three things in that are the judgement. **A run is ended at the end of every
row** — the `x == 0` — because the format refuses a run that crosses one, and a
converter that let one cross would make a file every reader rejects rather than
a picture sheared by one. **A run is ended at 65535**, the largest count the
format's sixteen bits hold. And **transparency is flattened upon white**: a
background has nothing behind it, the format has no transparency, and the
drawing arrived opaque in any case.

The drawing arrives in this repository by being copied from the project
owner's own `use_this.png` at its root, which git ignores so that the copy
handed over is never committed beside the one kept here.
