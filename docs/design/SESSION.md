<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Session: the Root, the Panel, the Launcher, and Who May Draw

**Phase**: 9, sub-task 9.5, of [`../project/PLAN.md`](../project/PLAN.md).
Section 1 is what this sub-task is; Section 2 is the three layers, which is the
kernel's half and the part that could not be expressed by an order alone;
Section 3 is the session itself — the root, the panel, the launcher; Section 4
is the text a program may draw and why the face is the kernel's; Section 5 is
the ownership of the display; Section 6 is the verification; Section 7 the
limitations.

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

**Both draw the same bitmap.** From 2026-09-21 the mark is the project owner's
own, [`../../art/logo.png`](../../art/logo.png), reduced to ninety-six pixels
square and classified into three states at two bits to a pixel by
[`../../art/logo.h`](../../art/logo.h): nothing, the disc, and the ink the
figure is drawn in. The kernel includes that header and so does the session, and
[`../../art/README.md`](../../art/README.md) records why it is public domain —
neither an LGPL kernel nor an MIT program may take artwork from the other.

Three states and not a mask of one bit, for the reason the pointer of sub-task
6.5 has a coverage byte: the mark is a figure **upon** a disc, and where the
bitmap says nothing the ground behind shows through, so the same file sits upon
the yellow of the boot screen and the yellow of the desktop without a colour
reserved to mean "absent".

**The session draws it a run at a time and the kernel a pixel at a time**, and
the difference is the boundary. Every fill the session makes is a system call,
so a row is walked and each run of one state becomes one call; the kernel is
already inside its own drawing code and has nothing to save. The picture is the
same either way.

It replaced a ring of coloured squares the session drew for itself — squares
because a program has no circle, the primitives being the kernel's. That
limitation is gone with it: the mark is now a bitmap and needs no curve.

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

1. **The panel shows a launcher and nothing else.** No clock, no list of what is
   running, no indicator: a clock is sub-task 9.7's, and a task list needs a
   window to be put away, which is limitation 2.
2. **No minimise, no resize, no hide.** A window is the size it was made and is
   always shown. The manager grew layers here and nothing else;
   [`WINDOWS.md`](WINDOWS.md), Section 9, limitation 3, keeps the rest.
3. **The launcher is read once, at start.** A `/etc/session.conf` edited upon
   the running machine takes effect when the session is started again, which
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
