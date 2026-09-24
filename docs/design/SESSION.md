<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Session: Root, Panel and Launcher

**Phase**: sub-task 9.5 of [`../project/PLAN.md`](../project/PLAN.md); icons from
9.6; the background and the window list after 9.7.
**Source**: [`../../userland/session/main.c`](../../userland/session/main.c) and
[`../../etc/session.conf`](../../etc/session.conf); in the kernel the layers, the
session claim and `WindowDrawText` in [`../../graphics/window.c`](../../graphics/window.c)
and [`client.c`](../../graphics/client.c); the formats
[`../../libc/include/icon.h`](../../libc/include/icon.h) and
[`../../libc/include/image.h`](../../libc/include/image.h); the mark
[`../../art/logo.h`](../../art/logo.h) and palette [`../../art/palette.h`](../../art/palette.h).
**Specifications**: none governs a desktop. The root window and stacking order are
X11 notions, registered in [`WINDOWS.md`](WINDOWS.md). The appearance follows
[`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md).

What the screen shows when nothing is running, how a person starts things without a
shell, and who may answer both. `/bin/session` claims the display, paints the root,
holds the panel (launcher, window list, clock) and starts what a person chooses.
It is not the window manager (stacking, focus and routing are the kernel's,
[`WINDOWS.md`](WINDOWS.md)) and not a supervisor (`init` restarts it,
[`INIT.md`](INIT.md)).

## 1. Layers

```
WINDOW_LAYER_PANEL    the panel, and the launcher it opens
WINDOW_LAYER_NORMAL   programs' windows
WINDOW_LAYER_ROOT     the desktop
```

**A window stacks within its layer and never outside it**: a raise goes to the top
of its layer only, and a new window starts at the top of its layer. With one plain
order, the next press would bury the panel and the next raise would put a window
under the desktop, and a session cannot prevent it (it never sees other programs'
presses). The manager keeps the stack **ordered by layer at every moment**, in one
function, `WindowStackInsert`, so every reader (hit test, composition, "topmost
remaining") walks it in order without knowing the rule.

- **Roots and panels have no frame**: a close control on the desktop, or a band
  above the bar, is nobody's wish, so the layer decides it. `WindowFrameOf`,
  `WindowContentOf` and `WindowTitleBandOf` are the only places the two kinds
  differ; an undecorated window's band is empty, so drag and close tests fail as
  they should. Neither is confined to keep a band reachable.
- **Neither takes the focus.** A root is made first, and a desktop whose first
  keystrokes go to the wallpaper ignores the person. A press on the root neither
  raises nor focuses it **but is delivered**, which is how the launcher closes
  when a person clicks away. `WindowTopmostFocusable` skips both.

## 2. Who may draw the desktop

`window_session` claims the session: the right to make a root and a panel.
**Exclusive, first come**: a second claimant gets `EPERM`. The claim is released
when the claimant's process ends; claiming twice is not an error, so a session need
not remember. Making a root or panel without the claim is `EPERM`: the argument is
fine, **the caller is wrong** (as with `power`, [`INIT.md`](INIT.md)). Without it two
programs could each paint a root and the desktop would flicker between them for no
visible reason. It is not a permission system: there are no users, and a program
asking before the real session would keep it out.

## 3. Text

`window_text` draws a run of text in a window's content with **the system's one
face** ([`CONSOLE.md`](CONSOLE.md)), which already draws every title. A face per
program would make a system whose text does not match itself; and the face is the
kernel's under `LGPL-3.0-or-later` while `libc/` is `MIT`, so a copy in the library
would be a relicensing this project may not perform
([`../../LICENSING.md`](../../LICENSING.md), Section 1). Colours are `0x00RRGGBB`,
encoded as blitted pixels are; glyphs are clipped by the content, so long labels
are cut at the edge.

## 4. The root

Without a background: the ground colour and **the mark**, as the boot screen draws
it, so starting and started look like one system. With a `background` named in
`/etc/session.conf`: that picture covers the whole root, and the mark is not drawn.

**The mark** is the project owner's, [`../../art/logo.h`](../../art/logo.h): 192
pixels square, each with two 4-bit coverages (how much the mark covers it; how much
of that is ink). `LogoMix` mixes ground, disc and ink by those amounts; drawn one to
one at scale two, averaged four-to-one by `LogoSample` at scale one, and
interpolated (`LogoInterpolate`) when larger. `LOGO_UNITS` (96) is the layout size.
The kernel and the session include the same header; it is public domain so both
may ([`../../art/README.md`](../../art/README.md)).

- **The caller names the ground** it drew beneath, since edge pixels are partly
  ground; naming one colour and drawing on another leaves a fringe.
- **The session composes and blits in bands.** Every blit is a system call, and
  mixed edges make per-run fills impossible, so the session composes the square in
  its tile, 21 rows at a time, and blits each band; the square around the mark is
  the ground, which the root already is.

**The background** ([`../../libc/include/image.h`](../../libc/include/image.h),
`.oxim`) ships at `/share/backgrounds/background.oxim`, converted from
[`../../art/`](../../art/) by the command in its README.

- **A file**, so a person changes it by editing a line.
- **Run-length encoded**, its own format: the 2048 × 1448 drawing is flat colour,
  about 12,000 runs, 70 KiB, where raw pixels would be 12 MiB. **A run never
  crosses a row's end**, so each row is checked alone; a crossing run miscounted
  would shear every later row.
- **Kept at its drawn size and scaled by the session** (screens are 1280 × 800,
  1024 × 768, 640 × 480). `OxysImageScalerRow` produces one screen row at a time,
  averaging the drawing's pixels beneath each screen pixel, holding one drawing row
  and three sums per screen column.
- **Covering**: scaled by the larger ratio and cut equally on the overhanging sides,
  never stretched (circles would become ellipses) or letterboxed (bars of a colour
  the drawing never had).
- Composed in bands of up to 65,536 pixels (16 blits at 1280 × 800).
- **An unreadable background costs the background**: the fault goes to standard
  error and the root is the ground and the mark.

## 5. The panel

A bar across the top with a line beneath it (separating it from a window of the
same colour under it), holding, left to right: the launcher's name, the window
list, the `using defaults` notice when it applies, and the clock
([`UTILITIES.md`](UTILITIES.md)). The session has no fill across the protocol, so
the bar is a tile blitted in bands.

**The launcher** is a second window **in the panel layer**, so nothing a program
does can cover it. It is created when opened and destroyed when closed. Its entries
are the `[launch]` blocks of `/etc/session.conf` (`run`, `name`, optional `icon`;
[`CONFIG.md`](CONFIG.md)).

- **Reread at every opening**, so an edit shows at the next press: entries, icons,
  and the background (redrawn only when its path changed). The scale is read at
  start only, since everything is sized by it.
- **A file that offers nothing falls back to the shipped one**,
  `/share/defaults/etc/session.conf`, entries and background both; the panel shows
  `using defaults`, and standard error names the file to mend and the `cp` that
  restores it. Without this, a cut or missing file left a launcher that would not
  open, and no way to reach the terminal to repair it. If even the shipped copy
  offers nothing, the launcher offers the terminal alone.
- **Started programs are not waited for**; the session reaps with `waitpid` and
  `WNOHANG` on `SIGCHLD`, since `init` adopts orphans only when a parent ends.

**The window list** is a button per ordinary window, in the order of their numbers,
every window listed so places stay put. A press restores its window (show, raise,
focus), or minimises it if it already holds the focus. The focused window's button
is drawn in the quiet colour, a minimised window's title dimmed, and titles are cut
to the button. The list is fetched with `window_list` (the session's alone) and
redrawn when the root receives `WINDOW_EVENT_WINDOWS`; `window_create` and
`window_destroy` wake sleepers, so a new window is listed at once. There is no
polling.

## 6. Icons

An `icon` in a `[launch]` block is **a path to a file** read when the entries are
read, drawn beside the entry's name. The mark is compiled in because it is drawn
before any filesystem and there is one of it; icons are one per program and grow
with the launcher's entries, so they are files.

**The format** ([`../../libc/include/icon.h`](../../libc/include/icon.h), `.oxi`,
version 2): magic, version, width, height, a reserved byte, then one 32-bit
little-endian pixel per position, `0xTTRRGGBB`, where `TT` is transparency (0
opaque, 0xFF fully transparent: `ICON_NOTHING` is `0xFF000000`). Uncompressed and
readable with `xxd`. Up to `ICON_EXTENT_MAXIMUM` (64) square; the terminal's is 48,
the slot's own size at scale two, drawn one to one. Version 1 (transparency only
0 or 0xFF) is still read; a version-1 reader would take partial transparency for
colour, hence the new number.

**`OxysIconCompose`** fits the icon to the requested square over a given paper: each
square pixel is the average of the icon pixels beneath, **each colour weighted by
its opacity** (averaging colours alone would pull `ICON_NOTHING`'s black into every
edge as a dark fringe); larger squares repeat the pixel beneath; non-square icons are
centred, not stretched. It is in the library so the kernel's self-test can assert
it. **An unreadable icon costs the icon**, not the entry.

## Verification

`KernelVerifyWindows` in [`../../kernel/test/gfx/windows.c`](../../kernel/test/gfx/windows.c)
(layers, focus, text) and `window-check` at privilege level 3 (the claim):

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The stack is ordered by layer whatever the creation order. | A panel on top only until the next window is made. |
| A raise goes to the top of its own layer only. | The panel buried by the first press. |
| The root does not take the focus when made or pressed, but receives the press. | Keys to the wallpaper; a launcher that never closes. |
| Roots and panels have no frame; a window reports its layer. | A close control on the desktop; rules applied to the wrong window. |
| With the last ordinary window destroyed, the focus is none, not the root. | Keys to the wallpaper after closing the last window. |
| Text leaves ink in the content; scale zero and null text are refused. | A launcher of blank rows. |
| Root and panel are `EPERM` without the claim; an unknown layer is `EINVAL`. | **Any program painting the desktop.** |
| The claim is granted when free, and claiming twice is not an error. | A session unable to restart a part of itself. |
| Text into another's window is `EBADF`; from a bad address, `EFAULT`. | The kernel reading a bad pointer. |

`KernelVerifyIcon` ([`../../kernel/test/libc/icon.c`](../../kernel/test/libc/icon.c)),
`KernelVerifyMark` ([`../../kernel/test/gfx/mark.c`](../../kernel/test/gfx/mark.c)) and
`KernelVerifyImage` ([`../../kernel/test/libc/image.c`](../../kernel/test/libc/image.c)):

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| An opaque icon pixel is its colour; a transparent one is the paper; a half-transparent one is half its colour. | Tinted icons; black squares; transparency read as all or nothing. |
| Two white and two transparent reduce to white on white; white and black to the grey between. | A dark fringe; sampling instead of averaging. |
| Enlargement repeats; outside the square is paper; a 3 × 1 icon is centred in a 3-square. | Reading beyond the icon; stretching. |
| Version 1 is read, version 0 refused; the parser's malformed-file refusals hold. | Old icons vanishing; files drawn from garbage. |
| The shipped icon is 48 square with transparent, opaque and partly transparent pixels. | An icon converted the old way, which parses and draws a staircase. |
| No mark pixel has more ink than coverage; the mark has disc, figure, edge and uncovered corners. | Specks on the edge; a square mark; a lost figure. |
| One to one the mark is its table; halved, the average of four; the mix is exactly ground, disc or ink at the extremes. | A reduction that picks one pixel; a faint box around the mark. |
| An image composed to the format reads with its extent; short, long, unknown version, reserved byte set, empty run, transparent pixel and zero extent are refused, emptying the image. | A picture drawn from whatever followed the file, or from the last one read. |
| **A run crossing a row's end is refused**, in a file exactly the right length (runs 3, 3, 4 in rows of 4, which only this check refuses). | Rows overrunning the scaler's buffer. |
| Reduction averages; a wide image covering a square is its middle, in order; enlargement repeats; a zero-width or over-wide screen is refused. | Lost thin lines; stretching, letterboxing, inversion; out-of-bounds sums. |
| The shipped background parses, has the drawing's extent, scales to every row of 1280 × 800, and has more than one colour. | A background converted wrongly, drawing a flat field. |

`config-check` asserts that `session.conf` names a background that opens and that
the shipped defaults offer a launcher ([`CONFIG.md`](CONFIG.md)). The fallback, the
rereading and the list are observed by operating the desktop
([`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md)): the desktop
with nothing running; the launcher opening, closing and starting a program; a window
dragged up sliding **under** the panel; minimised windows restored from the list.

## Limitations

1. The panel has no indicators beyond the clock and the `using defaults` notice.
2. No resize by hand ([`WINDOWS.md`](WINDOWS.md)).
3. The scale is read only at start; nothing is told when a file changes.
4. Rectangles are drawn by blitting a tile; there is no fill across the protocol.
5. The claim is first come, with no credential.
6. Nothing is shown while a started program is starting.
7. One icon per program at one size, repeated when enlarged; the mark is
   interpolated but icons are not.
8. The background has no transparency or palette; a photograph would barely
   compress; its edges are cut with no choice of where.
9. The window list holds 16 entries, as many as the panel's width allows, with no
   icons (it does not know a window's program) and titles simply cut.
