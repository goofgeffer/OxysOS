<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Session: the Root, the Panel, the Launcher, and Who May Draw

**Phase**: 9, sub-task 9.5, of [`../project/PLAN.md`](../project/PLAN.md).
Section 1 is what this sub-task is; Section 2 is the three layers, which is the
kernel's half and the part that could not be expressed by an order alone;
Section 3 is the session itself — the root, the panel, the launcher; Section 4
is the text a program may draw and why the face is the kernel's; Section 5 is
the ownership of the display; Section 6 is the verification; Section 7 the
limitations; Section 8 is the icons, and the resolution of both pictures;
Section 9 is the background, and Section 10 the list of windows upon the panel,
both of 2026-09-23.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../userland/session/main.c`](../../userland/session/main.c)
is the session and [`../../etc/session.conf`](../../etc/session.conf) is what
its launcher offers. The kernel's half is the layers, the session claim and
`WindowDrawText` in [`../../graphics/window.c`](../../graphics/window.c), the
two calls in [`../../graphics/client.c`](../../graphics/client.c) and
[`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h).
Asserted by [`../../kernel/test/gfx/windows.c`](../../kernel/test/gfx/windows.c)
and [`../../userland/window-check/main.c`](../../userland/window-check/main.c).

**Specifications**: none governs a desktop. The notions of a root window and of
a stacking order are the X Window System Protocol's, taken as notions and
registered in [`WINDOWS.md`](WINDOWS.md); no request, event or type of it is
adopted. The appearance is judged against
[`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md), Section 3.

## 1. What this sub-task is

Since 9.2 a program can own a window; since 9.3 something starts it; since 9.4
something says what to start. What there has been no answer to is **what the
screen looks like when nothing is running**, **how a person starts anything
without a shell**, and **who is allowed to answer the first two**.

**What it adds**: three stacking layers, so that a thing may be below every
window or above every window rather than among them; a claim upon the display,
so that exactly one program may use those two layers; a call by which a program
draws text with the system's one face; and the session — the program that
claims the display, paints the root, holds the panel, and starts what a person
chooses from its launcher.

**What it does not add** is a window a person can put away: there is still no
minimise, no resize and no task list, so the panel shows a launcher and nothing
else. Section 7, limitation 2, and 9.7 is where the utilities that would fill a
panel arrive.

## 2. The three layers

```
WINDOW_LAYER_PANEL    the panel, and what it opens
WINDOW_LAYER_NORMAL   the programs' windows
WINDOW_LAYER_ROOT     the desktop itself
```

**A window stacks among its own layer and never outside it.** A raise moves a
window to the top of its layer and no further; a window is created at the top of
its layer whatever was made before it.

### 2.1 Why an order alone could not express this

Sub-task 9.1's stack was an array, bottom first, and a raise appended. Under
that rule a panel is an ordinary window that the next press upon anything puts
behind it, and a root is an ordinary window that the next raise buries the
desktop beneath. Both are things a person would call broken within one minute of
using the machine, and neither can be prevented by a session that is careful:
the session does not see the presses that raise other programs' windows, and by
the time it could re-raise the panel a frame has already been composed with the
panel hidden.

So the rule is the manager's. **The stack is kept ordered by layer at every
moment** rather than sorted when it is read: every reader — the hit test, the
composition, the focus passed to "the topmost remaining" — walks the array in
order, and each would otherwise have to know the rule separately. One insertion
knows it instead, and `WindowStackInsert` is the whole of it.

### 2.2 The root and the panel carry no frame

A title band and a close control upon a root would be a control for closing the
desktop; upon a panel they would be a band above the bar that is itself a band.
**The layer decides it** rather than a second argument to the creation, because
a decorated root and an undecorated ordinary window are both things nobody has
asked for, and neither should be reachable.

An undecorated window's frame **is** its content. Every routine that asks where
a window is asks through `WindowFrameOf`, `WindowContentOf` and
`WindowTitleBandOf`, so the two kinds differ in those three functions and
nowhere else; the band of an undecorated window is the empty rectangle, and
every test against it — the drag, the close — then fails as it should without a
second condition anywhere.

Nor is a root or a panel **confined**. The confinement of 9.1 keeps a title band
reachable, and neither has one; a root confined would be a root that could not
cover the screen, its own height being the screen's.

### 2.3 The root never takes the focus

It is made first, before anything a person would type at, and a desktop whose
keys went to the wallpaper because it was the last thing created is a desktop
that ignores its first sentence. A press upon it neither raises it — it is at
the bottom already — nor focuses it, and the focus passed on when a window is
destroyed skips it: `WindowTopmostFocusable`.

**It still receives the press.** A session is entitled to know that its desktop
was clicked, which is how the launcher closes when a person clicks away from it.

A panel does take the focus, a launcher being a thing a person may one day type
into.

## 3. The session

### 3.1 What it is, and what it is not

`/bin/session` is the one program that owns the screen. It is **not** a window
manager — the stacking, the focus and the routing are the kernel's, sub-task 9.1
— and it is **not** a supervisor: `init` starts it and starts it again if it
ends, sub-task 9.3, which is why nothing in it tries to survive its own faults.
It is started because `/etc/system.conf` names it, where that file named the
window demonstration until now; the demonstration is one of the programs the
launcher offers.

### 3.2 The root

The ground, and the mark the boot screen draws, so that what a person sees while
the machine starts and what they see when it has started are recognisably the
same system — the same slate, the same figure, the same wordmark. The hand-over
from the boot screen to the desktop is then not a flash of a different colour.

**Both draw the same table.** From 2026-09-21 the mark is the project owner's
own, [`../../art/logo.png`](../../art/logo.png), and since 2026-09-23
[`../../art/logo.h`](../../art/logo.h) holds it as a table of 192 pixels square
in which each pixel carries two coverages of four bits: how much of it the mark
covers at all, and how much of it is the ink the figure is drawn in. The kernel
includes that header and so does the session, and
[`../../art/README.md`](../../art/README.md) records why it is public domain —
neither an LGPL kernel nor an MIT program may take artwork from the other.

**Coverage and not states.** Until 2026-09-23 the table was ninety-six pixels
square and each pixel one of three states — nothing, the disc, the ink. Drawn
two screen pixels to a pixel upon every screen of 1024 or wider, its edge was a
staircase of two-pixel steps and its figure a line of blocks, and no pixel could
be *partly* the disc. A pixel of the edge is now the ground, the disc and the
ink in the proportions the artwork gives it, by `LogoMix`; the table is drawn one
to one at the scale of two, and `LogoSample` averages four of its pixels into one
at the scale of one, so the edge is smooth upon VirtualBox's 640 by 480 as upon
everything wider. `LOGO_UNITS` — the size the mark is laid out by — is still
ninety-six, so nothing about where the mark stands moved.

**The ground is mixed in, so the caller names it.** A pixel of the edge is
partly the ground, and the ground is whatever the mark is drawn upon — the
yellow of the boot screen and the desktop, the paper of the window
demonstration. Each caller passes the colour it drew beneath, and a caller that
named one colour and drew upon another would carry a fringe of the named one
about the mark. The three that draw it today each clear to the colour they name
immediately before.

**The session composes it and the kernel draws it a pixel at a time**, and the
difference is the boundary. Every blit the session makes is a system call, and a
mixed edge would make a fill for every run of one colour into thousands of them;
so the session composes the whole square into its tile, twenty-one rows at a
time, and carries each band across in one blit — ten for the mark at the scale
of two. The square about the mark is composed as the ground, which the root
already is, so the whole of it may be blitted without a pixel being wrong. The
kernel is already inside its own drawing code and has nothing to save. The
picture is the same either way.
same either way.

It replaced a ring of coloured squares the session drew for itself — squares
because a program has no circle, the primitives being the kernel's. That
limitation is gone with it: the mark is now a bitmap and needs no curve.

**Since 2026-09-23 a background, where one is named, is the whole of the
root**, and the mark and the wordmark are not drawn upon it, Section 9. The
boot screen still carries the mark; the hand-over is then a change of picture,
which is what naming a background asks for. Without one, or with one that
cannot be read, the root is what this section describes.

### 3.3 The panel and the launcher

A bar across the top, with the launcher's name at its left, and a line beneath
it — the line being what separates the panel from a window of the same colour
standing under it.

**The launcher is a second window in the panel's layer**, not an ordinary one. A
menu that were an ordinary window would stack among the programs' windows and
the first press upon one of them would bury it; in the panel's layer nothing a
program does can put anything over it, which is the whole reason the layers
exist. It is made when the launcher is pressed and destroyed when it closes,
rather than hidden, there being no hide.

Its entries are `[launch]` blocks of `/etc/session.conf`, read once at start —
the format of sub-task 9.4, and the second thing to use it after `init`.

**A file that offers nothing falls back to the shipped one**, since 2026-09-24.
With no entry the launcher did not open at all, and the terminal is the only way
to mend `/etc/session.conf` from the desktop, so a file cut short, emptied or
missing locked a person out of the one tool that repairs it — and said nothing.
It happened: a save by `micro` cut the owner's file at its first blank line,
[`LIBC.md`](LIBC.md), Section 8.3, every `[launch]` block went with the rest, and
pressing the launcher did nothing. The session now reads the shipped copy at
`/share/defaults/etc/session.conf` instead — the whole of it, the background
with the entries, so the desktop is the desktop that shipped — draws `using
defaults` upon the panel left of the clock for as long as it does, and says
upon its standard error which file to mend and the `cp` that restores it. The
shipped copy is upon the ramdisk where a persistent `/etc` does not cover it,
[`../storage/PERSIST.md`](../storage/PERSIST.md), Section 3. Where even that
offers nothing, the launcher offers the terminal alone, which is the repair.
The fall-back was first the terminal alone; the owner asked whether a person
would know what to do with it, and they would not.

**The file is read again at every opening of the launcher**, since 2026-09-24,
so that an edit is seen at the next press rather than at the next start of the
session: an entry added or removed, an icon named, a background named or
removed — the root drawn again where the background's path changed, and only
then, the image being seventy kilobytes to read and a screen to draw. The scale
is taken at start alone, the panel and every window of the session being sized
by it. **Observed** under QEMU upon a disk holding the truncated file: the full
launcher, the background and `using defaults`; `cp` of the shipped file at the
shell and a press of the launcher, and the notice was gone; the `background`
line removed with `micro` and a press, and the root was the ground and the mark.
Neither the fall-back nor the re-reading is asserted by a self-test, the session
not running during them; `config-check` asserts that the shipped copies are
there and offer a launcher.

**What it starts, it does not wait for.** A program that took a moment to draw
would otherwise stop the panel. The session reaps with `waitpid` and `WNOHANG`
when a `SIGCHLD` has woken it, because a session that never reaped would fill
the process table with the programs a person had opened and closed: `init`
adopts an orphan only when its parent ends, and the session does not end.

## 4. The text, and why the face is the kernel's

`window_text` draws a run of text into a window's content with the system face.
It is the first thing since 9.2 that crosses the protocol and is not pixels, and
the reason is a licence and a fact.

**The fact**: there is exactly one face in this system, drawn for it at
sub-task 6.4, and the window manager already draws every title with it. A system
in which each program carries its own face is a system whose text does not match
itself, and a launcher whose labels are a different face from the titles above
them looks like two systems.

**The licence**: the face is the kernel's, under `LGPL-3.0-or-later`, and
`libc/` is `MIT`. A copy of it in the library would be a relicensing this
project may not perform — [`../../LICENSING.md`](../../LICENSING.md), Section 1
— so a face in userland must be **drawn afresh**, ninety-five glyphs of it, and
that is a sub-task's work for a second face nobody wants.

So the face stays where it is and a call reaches it. The colours are the
client's `0x00RRGGBB` and are encoded here exactly as a blitted pixel is, so
nothing about the boundary changes; the glyphs are clipped by the content's own
surface, so a label longer than its window is cut at the edge rather than
wrapping or writing into the row beneath.

**[`WINDOWS.md`](WINDOWS.md), Section 12, limitation 4, is closed by this and
not by a userland face.** Sub-task 9.6's terminal emulator will draw its cells
this way.

## 5. The ownership of the display

`window_session` claims the session: the right to make a root and a panel. It is
**exclusive and first come** — a second claimant is `EPERM` while the first
holds it — and the claim is released when the claimant's process ends, since a
claim held by a process that has ended would be a screen nobody could ever take
again.

Creating a window in either guarded layer is `EPERM` to every other program. The
refusal is `EPERM` and not `EINVAL` because the argument is not wrong: **the
caller is**. It is the same distinction `power` draws at
[`INIT.md`](INIT.md), Section 4.1, and the second place in this system where a
call is refused for being made by the wrong program.

**The failure it prevents** is two programs each painting a root. Each would
paint the whole screen and each would be right to; what a person would see is
whichever composed last, and nothing anywhere would say why the desktop
flickered between two backgrounds.

**Claiming it twice is not an error**, so that a session which restarted a part
of itself need not remember whether it had claimed already.

**What this is not** is a permission system. There are no users, no credentials
and no privilege beyond `init`'s: the first program to ask becomes the session,
and a program that asked before the real session started would keep it out.
Section 7, limitation 5, and Phase 13 is where a credential could come from.

## 6. Verification

The layers, the focus rule and the text are asserted upon a screen composed in
memory, as every graphical assertion since 6.3; the claim and its refusals are
asserted by `window-check` at privilege level 3, a program being the only thing
that can be the wrong program.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The stack is ordered by layer whatever the order of creation — an ordinary window made after the panel stands beneath it | A panel that is only on top until the next window is made, which is a panel that works while nothing is running |
| A raise moves a window to the top of its own layer and no further | The panel buried by the first press upon any window. **Observed**, Section 6.1 |
| The root does not take the focus when it is made, and a press upon it neither focuses nor raises it — but is delivered | A desktop whose first keystroke goes to the wallpaper; and a session that cannot tell it was clicked, so its launcher never closes |
| A root and a panel carry no frame: the frame is the content | A close control upon the desktop, and a title band above the bar |
| A window reports the layer it was made in | A layer recorded wrongly, after which every rule above applies to the wrong window |
| The focus passed when the last window is destroyed is none, not the root | The keys going to the wallpaper the moment a person closes their last window |
| Text is drawn into a window's content and leaves ink where the glyph is; a scale of zero and a null text are refused | A call that reported success and drew nothing — a launcher of blank rows |
| A root and a panel are `EPERM` to a program that has not claimed the session; a layer that does not exist is `EINVAL` | **Any program could paint the desktop.** **Observed**, Section 6.1 |
| The session is claimed when free, and claiming it twice is not an error | A session that could not restart a part of itself |
| Text into a window the caller does not hold is `EBADF`; from an address it may not use, `EFAULT` | The kernel reading a program's bad pointer at privilege level 0 |

### 6.1 The damage applied, and what the tests said

**A raise that goes to the top of everything**, as it did before the layers:

```
Window manager: asserting the stack, the focus and the routing.
  a raise put an ordinary window over the panel
Window manager self-test FAILED.
```

**The session's authority removed**, every caller treated as the session:

```
Window clients: running window-check at privilege level 3, with a hand to read its pixels and wake it.
window-check: the client protocol, from privilege level 3.
  a root was made by a program that does not hold the session FAILED.
  a panel was made by a program that does not hold the session FAILED.
--- End of captured serial output ---
VERIFICATION FAILED: the expected banner was not observed.
```

The boot does not finish. The program made a root and a panel it had expected to
be refused and therefore never destroyed, and what came after ran upon a screen
with a root over it — which is exactly the disorder the claim exists to prevent,
arriving the moment the claim is gone.

### 6.2 Two defects the tests found in the writing

Both were in the tests rather than in the manager, and both are the same kind:
**a test that changed the state another test was reading.**

The text assertion first drew into the window whose pixels the client
self-test's hand reads while the program sleeps, and reported `the pixels the
program blitted are not upon the screen` — a failure of the test's making,
reported as a failure of the blit. It draws into a window of its own now.

Then the windows the new assertions made and destroyed passed the focus away
from that window and back, and each passing is an event: the program's wait met
one of those and returned at once with something it had caused itself, asserting
nothing about being woken. It drains its queue before it waits now, which is
what any program with more than one window must do.

### 6.3 What only looking establishes

The procedure is
[`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 11:
the desktop as it stands with nothing running, the launcher opening and closing,
a program started from it, and — the one that matters — a window dragged upward
sliding **under** the panel rather than over it.

## 7. Limitations

1. **The panel shows a launcher, a list of windows and a clock, and nothing
   else.** No indicator. ~~No clock~~ — **closed at sub-task 9.7**,
   [`UTILITIES.md`](UTILITIES.md), Section 4. ~~No list of what is
   running~~ — **closed on 2026-09-23**, Section 10: once a window could be put
   away, the list became the only way to bring it back.
2. ~~**No minimise**~~ **and no resize by hand**. Minimise and full screen
   arrived on 2026-09-23, [`WINDOWS.md`](WINDOWS.md), Section 13; a frame edge
   a person drags is still [`WINDOWS.md`](WINDOWS.md), Section 13.7,
   limitation 1.
3. ~~**The launcher is read once, at start.**~~ **Closed on 2026-09-24**: it is
   read at every opening, Section 3.3; the scale alone waits for the session to
   start again. Before, a `/etc/session.conf` edited upon
   the running machine took effect when the session is started again, which
   `init` does when it ends. Nothing yet asks to be told that a file changed —
   [`CONFIG.md`](CONFIG.md), Section 7, limitation 2.
4. **The session draws its rectangles by blitting a tile.** There is no fill
   across the protocol, so a panel the width of the screen is a buffer blitted
   in bands. It costs one small buffer and a few calls, and it is what
   [`WINDOWS.md`](WINDOWS.md), Section 12, limitation 1, already records as the
   copy a shared mapping would remove.
5. **The claim is first come and there is no credential.** Nothing distinguishes
   the program `init` started from any other program that asks first. This
   system has no users and no privilege beyond `init`'s; a credential is Phase
   13's to introduce, and until then the claim prevents an accident and not an
   intent.
6. ~~**The root's mark is squares.**~~ **Closed on 2026-09-21**: both draw the
   one bitmap of [`../../art/logo.h`](../../art/logo.h), in the one palette of
   [`../../art/palette.h`](../../art/palette.h), so the boot screen and the
   desktop cannot differ. What a program still has no way to draw is a curve of
   its own; nothing asks for one now that the mark is a picture.
7. **Nothing is drawn while a program is starting.** A person who chooses an
   entry sees the launcher close and then, a moment later, a window; there is no
   sign in between that anything is happening.
8. ~~**The launcher offers no shell.**~~ **Closed at sub-task 9.6**: it offers
   `/bin/terminal`, which gives the shell a window of its own and a pair of
   pipes where its terminal would be, and the kernel now refuses `tcgroup` to a
   process whose standard input is not the terminal — so a shell started
   anywhere can no longer take the terminal from the shell at the keyboard.
   What that shell does not have is job control, which
   [`TERMINAL.md`](TERMINAL.md), Section 7, limitation 1, owes to a
   pseudo-terminal this system does not yet have.

## 8. Icons, which are files and not a header

Since sub-task 9.6 a `[launch]` block may carry an `icon`, and the launcher
draws it beside the entry's name. It is **a path to a file**, read once when the
session starts:

```
[launch]
name = Terminal
run  = /bin/terminal
icon = /share/icons/terminal.oxi
```

**Why a file.** The mark of [`../../art/logo.h`](../../art/logo.h) is compiled
into the kernel and into this program because both draw it before there is a
filesystem and because there is exactly one of it. An icon is the opposite of
both: there is one per program, the set grows whenever somebody adds an entry to
the launcher, and nothing draws one before `/` is mounted. A picture compiled in
is a picture that needs the system rebuilt to change — and a launcher whose
entries are read from a file at start cannot have its pictures fixed at compile
time without the two disagreeing the first time somebody edits that file.

**The format** is [`../../libc/include/icon.h`](../../libc/include/icon.h): four
bytes of magic, a version, a width, a height, a reserved byte, and then one
32-bit little-endian pixel per position, row by row. A pixel is `0xTTRRGGBB`:
the colour the window protocol carries in its low three bytes, and in its top
byte how transparent the position is — zero for wholly the colour, 0xFF for
wholly whatever is behind, which is `ICON_NOTHING`, `0xFF000000`. There is no
compression and no palette: an icon is a few kilobytes, and a format a person
can read with `xxd` is a format that can be checked by looking.
[`../../art/README.md`](../../art/README.md) holds the one command that makes a
file of it from a picture somebody drew.

**Version 2, since 2026-09-23.** Version 1 allowed the top byte two values, none
and all, so a picture reduced to the slot had to make every pixel of its edge one
or the other, and was twenty-four pixels square and enlarged by two into a slot
of forty-eight. Both were the staircase a person saw. Version 2 lets the top
byte be anything between, and the terminal's icon is forty-eight pixels square —
the slot's own extent at the scale of two, drawn one to one. The number was
raised because a reader of version 1 takes a top byte of 0x80 for part of a
colour and blits it; a newer file refused is better than one drawn wrongly. A
file of version 1 is still read, since its two values mean in version 2 what
they meant in it. `ICON_EXTENT_MAXIMUM` went from thirty-two to sixty-four at
the same time, which a picture of forty-eight required.

**The transparency is resolved by the caller.** The protocol carries pixels and
has no notion of a pixel that is not there, so what an icon means by "nothing"
is "the colour behind me" — and the only thing that knows what that is, is the
program drawing it. `SessionDrawIcon` asks `OxysIconCompose` for each pixel of
the slot upon the panel's colour, composes them into its tile and carries the
result across in one blit. An icon drawn by something over a different ground
would compose it over that one instead; the file says nothing about either.

**`OxysIconCompose` fits the icon to the square it is asked for.** Each pixel of
the square is the pixels of the icon beneath it averaged, **each colour weighted
by how opaque it is**, and the rest of the pixel the paper. The weighting is not
a refinement: `ICON_NOTHING` carries black in its colour, and averaging colours
alone would bring that black into every pixel beside a transparent one — a dark
fringe about every picture reduced to the slot, looking like a fault in the
drawing and not in the arithmetic. A square larger than the icon repeats the
pixel beneath; an icon that is not square is centred in the square with the
paper about it rather than stretched. It is in the library and not the session
because it is ordinary arithmetic upon a parsed icon, and so the kernel's
self-test can assert it without a window.

**The parsing is in the C library, and the reading is beside it**, which is the
seam of [`LIBC.md`](LIBC.md), Section 9, a fifth time: the kernel's self-test
drives the parser over bytes it composes, with no filesystem and no privilege
transition, and then reads the file the ramdisk actually ships and puts it
through the same parser. That second half is `config-check`'s argument: a parser
that works and a system whose icons are what its launcher expects are different
properties, and a picture converted at the wrong size or with its transparency
flattened parses perfectly and draws a black square; one converted as version
1 was parses perfectly and draws the staircase.

**An icon that cannot be read costs the icon and not the entry.** The fault is
printed upon the standard error and the entry is offered without a picture,
which is what an entry naming no icon gets. A launcher that refused to offer a
program because its picture was missing would be a desktop a person cannot use
for a reason having nothing to do with the program.



### 8.1 Verification of the pictures

The icon's assertions are
[`../../kernel/test/libc/icon.c`](../../kernel/test/libc/icon.c) and the mark's
[`../../kernel/test/gfx/mark.c`](../../kernel/test/gfx/mark.c), both at boot and
neither needing a window. The rows below are those of 2026-09-23; the parser's
refusals, of the day before, are listed in the file itself.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| An opaque pixel drawn one to one is its own colour; a position of nothing is the paper | A launcher that tints its icons, or draws the black of `ICON_NOTHING` as a square |
| A pixel half transparent is half its colour upon black | A reader treating the top byte as none or all — the staircase version 2 exists to be rid of |
| Two white and two nothing, reduced to one upon white, are white | The black carried by `ICON_NOTHING` averaged into the edge: a dark fringe about every reduced picture |
| Two white and two black reduced to one are the grey between; enlarged, the pixel beneath is repeated; outside the square is the paper | A reduction that samples one pixel instead of averaging, or an enlargement that reads beyond the icon |
| An icon three by one is centred in a square of three | A picture stretched to fill the slot |
| Version 1 is still read, version 0 refused | A launcher whose older icons vanish when the format grows |
| The shipped icon is forty-eight square, and has a transparent, an opaque and a partly transparent pixel | The icon converted as version 1 was — at the old extent, or with its edge thresholded — which parses and draws |
| No pixel of the mark has more ink than coverage | `LogoMix`'s unsigned subtraction the wrong way round: a speck of some other colour upon the edge |
| The mark has a disc, a figure, a pixel of edge, and uncovered corners | The ground kept (a square), the figure lost, or the edge regenerated all or nothing |
| One to one the mark is its table; halved it is the average of the four beneath | A reduction that picks one of four and brings the staircase back at 640 by 480 |
| The mix at no coverage is exactly the ground, wholly covered the disc, wholly inked the ink | A mix a level off, drawing a faint box about the mark upon the boot screen |

**The damage applied.** The version-1 icon of the commit before was put back
upon the ramdisk and one byte at the corner of the table given ink without
coverage, together, in one build. The run reported the icon's extent wrong in
both directions, "no pixel of the shipped icon is partly transparent, so its
edge was converted as a staircase", "a pixel of the mark has more ink than
coverage" and "a corner of the mark is covered, so its ground was not removed",
and both self-tests FAILED; build 11 of the register is that image. Both were
reverted.

### 8.2 Limitations of the pictures

1. **An icon enlarged repeats its pixels.** Above the scale of two, and in no
   case seen yet, a step shows. ~~The mark likewise~~ — **since 2026-09-23 the
   mark is interpolated** when drawn larger than its table, `LogoInterpolate`,
   which a window made full asks for; the icons have no such need yet.
2. **One icon per program, at one extent.** A file carries one picture, so a
   slot of another size is filled by reducing or repeating it rather than by a
   picture drawn for that size.
3. **The palette of the mark is the caller's, the edge's ground too.** A caller
   that draws the mark upon a colour other than the one it passes as the
   ground gets a fringe of the one it passed. Nothing can check that but
   looking, Section 3.2.

## 9. The background, of 2026-09-23

The project owner drew a background for the desktop. It ships as a file upon
the system's own filesystem — `/share/backgrounds/background.oxim` of the
ramdisk — and `/etc/session.conf` names it:

```
[session]
background = /share/backgrounds/background.oxim
```

The session reads it once at start and covers the root with it. The source
and the one command that converts it are in
[`../../art/README.md`](../../art/README.md).

**A file, for the icons' reason**, Section 8: a person changes the desktop's
picture by editing a line, not by rebuilding the system.

**A format of its own**, [`../../libc/include/image.h`](../../libc/include/image.h),
beside the icon's. The drawing is 2048 by 1448 — three million pixels, twelve
megabytes held the icon's way, six times the ramdisk. It is flat colour, and
its pixels are twelve thousand runs of one colour: seventy kilobytes as runs.
Folding runs into the icon's format would give every icon a decoder it does not
need, and every reader of icons a length it could no longer check by
multiplying. **A run never crosses the end of a row**, so each row is judged
alone: a run allowed to cross would let one miscounted run shift every row
after it sideways, which draws — a picture sheared from that row down.

**Kept at the resolution it was drawn at, and scaled by the session.** There is
no one screen: 1280 by 800 under QEMU, 1024 by 768 under Bochs, 640 by 480 under
VirtualBox. A picture reduced for one would be enlarged for the others. The
scaler, `OxysImageScalerRow`, produces one row of the screen at a time, reading
forward through the runs, averaging every pixel of the drawing beneath a pixel
of the screen; it holds one row of the drawing and three sums per column of the
screen, never the drawing decoded.

**Covering, not stretching and not letterboxing.** The drawing is scaled by the
larger of the two ratios, so it reaches all four edges, and is cut equally from
the two sides that overhang. Stretched, the drawing's disc would be an ellipse
upon every screen of another shape; letterboxed, there would be bars of a colour
the drawing never had. What covering costs is the edges: at 4 by 3 the sides
are cut, at 16 by 10 the top and the foot.

**A background that cannot be read costs the background and not the desktop.**
The fault is said upon the standard error and the root is the ground and the
mark, which is what a session that names none draws.

The root is composed in bands of as many rows as sixty-five thousand pixels
hold and blitted a band at a time: at 1280 by 800, sixteen blits.

### 9.1 Verification of the background

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| An image composed to the format is read with its extent; one byte short, one byte over, an unknown version, a reserved byte set, a run of nothing, a pixel with a transparency and an extent of zero are each refused, and a refusal empties the image | A picture drawn from whatever followed the file in memory, or from the last image read |
| A run crossing the end of its row is refused, though the file is exactly two rows long | Six pixels decoded into a row of four — past the scaler's buffer upon an image as wide as the bound. **Observed** as a damage, Section 9.2 |
| Reduced, a pixel is the average of the pixels beneath | A scaler that samples one pixel and loses every thin line of the drawing |
| A wide image covering a square is its middle, rows in order | A stretched background, or one letterboxed, or upside down |
| Enlarged, the pixel beneath is repeated; a screen of no width, or wider than the bound, is refused | A scaler reading beyond the image, or writing beyond its sums |
| The shipped file is upon the ramdisk, parses, is the drawing's extent, scales to every row of a 1280 by 800 screen, and is more than one colour | A background converted wrongly, or empty, which parses and draws a flat field |
| `/etc/session.conf` names a background that opens | A path typed wrongly, which costs the picture and says so only upon a standard error |

The first six are [`../../kernel/test/libc/image.c`](../../kernel/test/libc/image.c);
the last is `config-check`.

### 9.2 The damage applied, and what the tests said

The parser's refusal of a run longer than what remains of its row was removed,
in the build that damaged the window manager, [`WINDOWS.md`](WINDOWS.md),
Section 13.6. The run said `a run crossing the end of its row was accepted` and
`Image self-test FAILED.`; it was reverted.

**The assertion was first written too weakly, and this is recorded.** Its first
composition was runs of three, three and two in rows of four. Without the check
a parser still refuses that file — the second row wants two more pixels and the
file has ended — so the assertion would have passed with the check gone. It was
changed, before the damage was applied, to three, three and four, which a
parser without the check accepts outright.

### 9.3 Limitations of the background

1. **One picture, chosen at start.** Changing it needs the session started
   again; nothing yet asks to be told that a file changed, limitation 3.
2. **No transparency and no palette.** A photograph, which has few runs, would
   be nearly four bytes a pixel and would not fit the ramdisk at this
   resolution; a compressed format is the remedy the day one is wanted.
3. **Enlargement repeats.** Upon a screen larger than the drawing, the scaler
   repeats its pixels. No screen here is.
4. **The edges are cut**, Section 9; nothing lets a person choose where.

## 10. The list of windows, of 2026-09-23

Once a window could be minimised — [`WINDOWS.md`](WINDOWS.md), Section 13 — it
needed a way back: a hidden window is neither drawn nor hit, so nothing upon the
screen could be pressed to show it. That way is **a button per ordinary window
upon the panel**, after the launcher's name, in the order of the windows'
numbers.

**Every window is listed, not only the minimised ones.** A list that changed its
length whenever a window was hidden would be a list a person could not learn
the places of.

**One button does both.** Pressed, it restores its window — shows it, raises it
and gives it the focus — unless that window already holds the focus, when it
minimises it. The window holding the focus is drawn upon the quiet colour the
open launcher is, and a minimised one with its title dimmed; a title is cut to
the button. A list longer than the panel is cut at the screen's edge.

**The list is asked for, and redrawn, when the root is told.** The kernel puts
`WINDOW_EVENT_WINDOWS` into the root's queue whenever the windows, their states
or the focus among them change; the session then asks with `window_list`, which
is its alone, and draws the panel. There is no polling: a desktop that asked
every second would redraw the panel every second for a list that seldom changes.

**The notice wakes the session**, since 2026-09-24, and did not before. A window
made or destroyed by a program told the roots but woke nobody, the only wakes
being a routed input event and the calls of Section 10's own; so a terminal opened
from the launcher stood unlisted until the pointer moved or the minute turned,
which is how it was found, and a mouse movement made the button appear.
`window_create` and `window_destroy` now wake every sleeper, as a process's ending
always had. Observed fixed under QEMU; not asserted by a self-test, which runs
with no session to be asleep.

**This needed the panel to stop taking the focus**, [`WINDOWS.md`](WINDOWS.md),
Section 13.3: a press upon the list that took the focus would lose, before the
session read the press, the one thing the button must know — whether its window
held the focus.

It is not asserted by a self-test of its own: it is drawing and one choice,
and the calls beneath it are asserted in [`WINDOWS.md`](WINDOWS.md),
Section 13.5. What only looking establishes is
[`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 11.

### 10.1 Limitations of the list

1. **Sixteen entries**, the window manager's capacity, and as many buttons as
   the panel's width holds.
2. **No icon upon a button.** The list does not know which program owns a
   window, and so not which icon is its.
3. **A title is cut, not scrolled or shortened with care**; two windows whose
   titles begin alike look alike.
