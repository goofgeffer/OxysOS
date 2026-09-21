<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Window Manager

**Phase**: 9, sub-tasks 9.1 and 9.2, of [`../project/PLAN.md`](../project/PLAN.md).
**The layers, the session's claim and the drawing of text are sub-task 9.5's**
and are [`SESSION.md`](SESSION.md); what is here is the manager they were added
to.
Section 2 is what a window is; Section 3 is the stack, the focus and where an
event goes, which is the whole of what a window manager decides; Section 4 is
the appearance, judged against the preference
[`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md), Section 3, wrote
down before any of this existed; Section 5 is where it runs and what the screen
is for; Section 6 is the verification; Section 7 is the demonstration a person
operates and the menu that reaches it. **Sections 10 to 12 are sub-task 9.2**:
the protocol by which a program is a client of all of the above, what the first
real client decided about the interface Section 2 had judged, the verification
of it, and its limitations.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../graphics/window.c`](../../graphics/window.c), with
[`../../kernel/include/oxys/gfx/window.h`](../../kernel/include/oxys/gfx/window.h);
the client side of sub-task 9.2 is [`../../graphics/client.c`](../../graphics/client.c),
with [`../../kernel/include/oxys/gfx/client.h`](../../kernel/include/oxys/gfx/client.h),
the six calls of [`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h)
dispatched to it by [`../../kernel/arch/x86_64/syscall/syscall.c`](../../kernel/arch/x86_64/syscall/syscall.c),
and the wrappers of [`../../libc/syscall/calls.c`](../../libc/syscall/calls.c);
asserted by [`../../kernel/test/gfx/client.c`](../../kernel/test/gfx/client.c)
running [`../../userland/window-check/main.c`](../../userland/window-check/main.c),
and demonstrated by [`../../userland/windows/main.c`](../../userland/windows/main.c).
The disc it draws its close control with is
[`../../graphics/draw.c`](../../graphics/draw.c), [`DRAWING.md`](DRAWING.md),
Section 4.1. It is asserted, and the demonstration held, by
[`../../kernel/test/gfx/windows.c`](../../kernel/test/gfx/windows.c); it is
serviced from the bootstrap processor's tick by `KernelServiceDisplay` in
[`../../kernel/kernel.c`](../../kernel/kernel.c), and the entry point decides
which of two things the screen is for. The terminal gave up the keyboard for it:
[`../../kernel/terminal/terminal.c`](../../kernel/terminal/terminal.c),
`TerminalAttachKeyboard`.

**Specifications**: none governs a window manager, and none is claimed. Three
notions are taken from the X Window System Protocol, X Version 11 Release 7.7 —
that keyboard input has one scope (Glossary, *Input focus*), that sibling
windows obscure one another in an order (Glossary, *Stacking order*), and that a
button pressed in a window binds the pointer to it until every button is
released (Chapter 11, *Input Device events*) — and they are taken as notions
and restated for one pointer and one screen. No request, event or type of that
protocol is adopted; [`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md),
Section 5, is the rule, and [`../project/REFERENCES.md`](../project/REFERENCES.md)
registers the document. The disc is J. E. Bresenham's circle algorithm of 1977,
registered likewise.

## 1. What this sub-task is, and what it is not

Phase 6 built everything that puts pixels upon a screen and needs no process:
the framebuffer, the primitives, the font, the pointer and the compositor
beneath them. [`ARCHITECTURE.md`](ARCHITECTURE.md), Section 4.1, records that
what stayed for Phase 9 was the half that needs something to run — a window
manager has nothing to manage until there are programs to own windows.

**What this adds** is the manager and nothing on the far side of it: a table of
windows upon one screen surface, the order they stack in, the one of them that
holds the keyboard, and the routing of every key and every movement of the mouse
to the window it belongs to. The programs that own windows are sub-task 9.2,
Section 10, which puts them on the far side of a protocol; until it the only
owner was the demonstration of Section 7, which was kernel code and is now a
program.

**What is decided now and will not change when a client arrives** is the shape
of the thing on this side of the protocol. A window is a rectangle of pixels the
owner draws into and a queue of events the owner drains, and the manager's whole
business is to compose the rectangles in their order and to put each event into
the right queue. A protocol carries those two things across a boundary; it does
not change what they are. The judgement `ARCHITECTURE.md`, Section 4.1, recorded
about the surface interface of 6.6 — that it was designed before any client
existed to design it against — applied to this interface too, and Section 10.1
is where both were revisited against the first client.

## 2. What a window is

A **content** surface, owned by the manager and drawn into by the owner, and a
**frame** the manager draws around it: a title band twenty-four pixels high
above the content and a one-pixel border about the whole. The owner names the
content's size and the frame's position and never draws the frame. A frame the
owner drew would be a frame every owner drew differently, and the one thing a
person recognises a window by is that they all have the same one.

The content is a tightly packed surface of the screen's pixel size, its pixels
from the heap, and `WindowSurface` hands the owner the surface itself: the owner
draws with the primitives of Phase 6 exactly as the console does, and says what
it changed with `WindowInvalidate`. Nothing reaches the screen until the manager
composes. Drawing into a window is therefore never visible as it happens, which
is the double buffering of [`COMPOSITOR.md`](COMPOSITOR.md), Section 2, applied
a second time one level up.

**Every coordinate an owner sees is the content's own.** A pointer event carries
the position relative to the content's top left, and may lie outside it — below
zero, or beyond the extent — while the pointer is bound to that window by a held
button. The owner never learns where its frame stands upon the screen unless it
asks, and the demonstration never asks.

**The table is fixed at sixteen** and a seventeenth is refused, for the reason
the compositor's layer table is fixed at four: a bound the manager is honest
about is better than an allocation that fails somewhere nothing expects it to.
The desktop of 9.5 and the utilities of 9.7 are a handful of windows; a program
that opens sixteen has a defect this bound turns into a refusal.

## 3. The stack, the focus, and where an event goes

### 3.1 The stack is an array

Sixteen identifiers, bottom first, topmost last. Raising a window removes its
entry and appends it, which moves at most fifteen words; a hit test walks the
array from the end and the first frame containing the point is the one a person
sees there. A linked list would be no shorter to write and would put the
stacking order — the one thing a person sees directly — into pointers a defect
could make circular, where an array's order is its indices and cannot be.

### 3.2 The focus

A key goes to the window that holds the focus, and to no other; where none does,
it is discarded and counted. The focus is given to a window when it is created,
when a button is pressed anywhere within its frame, and — when its holder is
destroyed — to the topmost window remaining. A transfer tells the loser before
the gainer, in that order, because an owner that draws its own state upon a
focus change would otherwise, for one composition, have two windows drawn as
focused.

The manager draws the focus too: the title band of the holder is in one colour
and every other band in another, so a person can see where the keys will go
without typing one.

### 3.3 The pointer

A movement goes to the window under the pointer, translated into that window's
content coordinates. A movement over the ground goes nowhere and is not counted
as discarded, the ground being nobody's.

**A press within a content raises the window, gives it the focus, delivers the
press, and binds the pointer to that window.** Until every button is released,
every movement and every release goes to the bound window wherever the pointer
has gone since; a further press while bound goes there too. Without the binding,
a drag begun in one window and carried over another would end in the other, and
the first would never learn that the button it saw pressed was released — which
is a window left believing a button is held, for ever. This is the rule the X
protocol states for its implicit grab, restated for one pointer.

**A press within the title band is the manager's and not the owner's.** It
raises and focuses the window, and then does one of two things: upon the close
control it puts a close event into the window's queue; anywhere else along the
band it begins a drag, and the window follows the pointer until the buttons are
released, the owner being told nothing. **The manager destroys nothing upon a
close.** What a close means is the owner's decision — an editor with an unsaved
file being the standing example of a window that must be asked and not removed —
and the demonstration's windows, having nothing to save, destroy themselves.

**A move is confined so that the band stays reachable**: its top never above the
screen, its bottom never below it, and forty-eight pixels of the frame always
upon the screen horizontally. A window dragged wholly off the screen is a window
nobody can recover without knowing its name, and nothing yet lets a person name
one.

### 3.4 The queue

Each window holds thirty-two events and drops the newest beyond that, counting
the drop, for the reason the keyboard's buffer drops the newest: the oldest is
the beginning of whatever the owner has not yet read, and a ring that overwrote
it would hand the owner a release with no press before it.

## 4. The appearance

[`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md), Section 3, wrote
down the preference before Phase 9 was reached, so that the appearance would be
designed against something written rather than something recalled, and said it
would be judged against what was built. This is the first thing built.

**The frame is flat.** A band, a border of one pixel, a title, a disc. Depth is
expressed by the stacking order and by nothing else: no line lighter above and
darker below, no shadow. The idiom that section prohibits is the cheapest to
reach with a rectangle fill and four lines, which is why the prohibition was
written down; it was not reached for.

**The close control is a disc**, ten pixels across, at the right of the band.
The section asks that the character be carried by geometry — circles, arcs —
rather than by a glyph or an ornament, and a control that is a circle where
everything else is a rectangle is exactly that. It is the one thing in the frame
that is not a rectangle, and the one thing a person is asked to find. It is drawn
by a primitive and not by an image, and the project owner's icons, when they
exist, may replace it; nothing here depends upon its being a disc.

**The palette**, supplied by the entry point in the framebuffer's encoding.
**The numbers are not here and are not in the kernel**: they are
[`../../art/palette.h`](../../art/palette.h), which the boot screen, the session
and `/bin/windows` read as well, so that the frame a window is drawn in and the
ground it stands upon are one decision rather than four.

They were written into this table and into the entry point until 2026-09-21.
When the project owner asked for a yellow scheme the entry point changed and
this table did not, and for a day it described a dark slate and a blue band that
nothing drew — which is the argument for the header stated as a thing that
actually happened.

| Use | Which colour | Why this one |
| --- | ------------ | ------------ |
| The ground | `OXYS_GROUND` — a yellow | What everything stands upon, and the same one the boot screen and the desktop use, so that starting looks like one machine rather than three pictures. |
| A window's paper | `OXYS_PAPER` — a warm white | Not pure white, which glares beside dark text at this face's weight. |
| The border | `OXYS_BORDER` — a dark brown | Darker than the ground and than the bar, so that a window has an edge upon either. |
| The band of the focus holder | `OXYS_BAR` — a lighter, more orange yellow | It differs from the ground in lightness and in hue at once, so neither a person who sees colour poorly nor a screen that renders it badly is left with two identical yellows. |
| The band of every other window | `OXYS_BAR_QUIET` — the same, paler | "Which window takes the keys" is answered by how strong a colour is rather than by which colour it is. |
| Title and disc upon the focus holder | `OXYS_INK` — a dark brown | Legibility upon the bar; black upon a saturated yellow is harsher than anything else here. |
| Title and disc upon the others | `OXYS_DIM` | Present, and not being read. |

**The title is the face of sub-task 6.4 at twice its size**, sixteen pixels
high in a band of twenty-four. There is one face and no other, and the section's
last rule — legibility first — decides the scale: at its own size the title is
eight pixels high upon a screen of eight hundred, and reads as a mark rather
than as a word. A second face is a decision for whoever draws one.

**What the section wanted that this does not have**: rounded corners, and any
asymmetry. Both are geometry the primitives can draw — the disc is the arc they
lacked — and both are left for the desktop of 9.5, which decides what a window
stands upon and therefore what its corner should meet.

## 5. Where it runs, and what the screen is for

### 5.1 The tick

The manager is serviced from the bootstrap processor's timer tick, where the
pointer already was: `KernelServiceDisplay`, called beside `TerminalService`.
Every movement the mouse driver holds is routed — every one, not the last, so
that a press and its release within one tick both arrive; every key is routed;
since sub-task 9.2 every program asleep for an event is woken where any was
routed; whatever changed is composed into the back buffer and carried to the
display with the pointer over it. A tick in which nothing moved, nothing was
pressed and nothing was drawn composes nothing and presents nothing, which is
most ticks. The windows' owners draw between ticks, from their own calls, and
what they drew is composed at the next.

It runs there rather than as a thread of its own because of what it touches. The
back buffer, the damage rectangle and the layer table are the compositor's,
reached on the ordinary path through `KernelWriteString`, whose lock masks
interrupts; the tick is an interrupt handler, which runs with them masked; and
the two therefore cannot interleave upon the one processor that reaches either
— which is the invariant [`COMPOSITOR.md`](COMPOSITOR.md), limitation 6, said a
second drawer would have to keep. A kernel thread would be pre-empted wherever
it stood, mid-composition included, and would need the lock the compositor does
not yet take below `KernelWriteString`. The cost is that the manager's whole
round runs inside an interrupt handler, which is acceptable for three windows
and is limitation 2.

### 5.2 Two things the screen can be for

Since this sub-task the default menu entry gives the screen, the keyboard and the
mouse to the window manager, and the shell of Phase 8 runs upon the serial line;
the display is held quiet, so that the shell's output — which is the serial
line's — is not drawn over the windows. Two further entries give the screen to
the shell as every entry did through Phase 8: **Shell-only**, quiet, the banner
and a prompt; and **Shell Diagnostics**, the boot log first. The three are
Section 7.2, and the entry point's decision is one condition:
`shell-only` or `diagnostics` upon the command line, or no compositor, or no
root, and the shell takes the screen and says so where the reason was an
absence.

**The terminal gives up the keyboard.** `TerminalPoll` translated every key into
bytes for the shell; with the window manager holding the keyboard, it would have
handed every keystroke to two readers, the shell upon the serial line and the
window holding the focus, and each would have acted upon half. `TerminalAttachKeyboard`
detaches it for the manager's tenure and reattaches it when the shell ends, the
serial line being read regardless.

## 6. Verification

Upon a screen composed in memory, one hundred and sixty by one hundred and
twenty, with a pitch beyond the width and a sentinel in the padding, as every
graphical assertion since sub-task 6.3. The manager composes into whatever
surface it is given, so it is given one whose every pixel can be read back, and
**the stacking order is asserted by reading the pixel where two windows
overlap** rather than by asking the manager which it believes is on top — a
manager that kept the stack right and drew it wrong would answer correctly and
show the wrong window.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The second window made stands above the first, holds the focus, and its frame is the content plus the band and the border | A new window placed beneath an existing one: asked for and not seen. A frame computed from the wrong height, so that a press in the band lands in the content |
| The first window was told it gained and then lost the focus, the second that it gained | A transfer that tells the gainer only, leaving the loser drawing itself focused |
| After composition the pixel where two windows overlap is the upper's content; a pixel the lower alone covers is the lower's; the ground is the ground; the bands are in the focused and unfocused colours; the corner is the border | The painter's order reversed, so that the lower window is drawn last and shows through the upper. A focus drawn upon the wrong window |
| The hit test in the overlap names the upper window; upon the ground, none | A hit test walking the stack from the bottom, which returns the window a person cannot see. **Observed**, Section 6.1 |
| A press in the lower window raises it, focuses it, tells the loser and the gainer, delivers the press in the content's coordinates, and binds the pointer | A raise that moves the entry but not the composition. A press delivered in screen coordinates, which an owner reads as a point outside its window |
| Raising marks the whole frame as changed, and the raised window is then drawn over the other | A raise that changed the order and marked nothing, leaving the lower window drawn on top until something else happened to change those pixels |
| While bound, a movement over the other window goes to the bound one with the bound one's coordinates, and the other receives nothing; the release goes to the bound one; after it, a movement goes to the window beneath the pointer | A binding that ends at the frame's edge, so the press's window never sees its release; a binding that never ends |
| A key goes to the focus holder and to no other | A key delivered to the window under the pointer, or to every window |
| A drag by the band moves the window by the pointer's movement and delivers nothing; both where it was and where it is are marked, and the old frame is the ground afterwards | A move marking only the new frame, which leaves the old one standing — the pointer's trail of `COMPOSITOR.md`, Section 2.3, one level up. **Observed**, Section 6.1 |
| The close control delivers one close event and nothing else, destroys nothing, and binds nothing | A manager that destroyed the window itself, taking an editor's unsaved file with it |
| A move off the top left is held at minus fourteen, zero; off the bottom right at one hundred and twelve, ninety-four | A window dragged wholly off the screen, recoverable by nobody |
| Destroying the focus holder passes the focus to the window beneath and tells it; destroying the last leaves none, and a key is then discarded and counted | A focus left naming a window that no longer exists, so that keys go into a queue nothing drains |
| The table takes sixteen and refuses a seventeenth; a content below sixteen pixels is refused; every window made is given back | A table written past its end. A self-test that left the table full, so the demonstration below could make no window and reported nothing |
| A queue holds thirty-two and drops the newest beyond, counting it | A ring overwriting its oldest, handing the owner a release with no press |
| Nothing wrote into the padding | A composition escaping the surface by the difference between pitch and width |

The disc is asserted beside it, upon a surface of its own:
[`DRAWING.md`](DRAWING.md), Section 4.1.

### 6.1 The damage applied, and what the test said

Two defects were introduced on purpose, in one build, and the run reported:

```
Window manager: asserting the stack, the focus and the routing.
  the hit test in the overlap does not name the upper window
  a move did not mark both where the window was and where it is
  a moved window's old frame still stands upon the screen
  the close control did not deliver a close event, alone
Window manager self-test FAILED.
```

The first line is the hit test walking the stack from the bottom; the second and
third are a move marking only where the window now is, the third being the
consequence a person would see; the fourth is a consequence of the first — with
the wrong window under the close control the close went elsewhere. Both were
reverted and the run passed.

**One defect was found by the test in the writing of it**, and is recorded
because it is the kind the assertion exists for: the first form of the drag
assertion pressed at a point that lay within the close control's reach, so the
"drag" was a close, and the test failed upon its own coordinates. The reach is
a square of twenty-five pixels about a disc of ten, so that a person need not
land upon the disc itself, and the test's press had to move out of it.

### 6.2 What only looking establishes

That a drag follows the hand without a trail, that a window raised over another
appears at once and whole, that the focus is where the blue is, and that the text
typed arrives in the window that holds it. The procedure is
[`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 7,
and it was followed under QEMU by driving the mouse and the keyboard through the
monitor, with the screen captured at each step, and under VirtualBox at the
keyboard.

## 7. The demonstration, and the menu

### 7.1 Three windows

Until sub-task 9.5 has a desktop to present, the default entry presents the
manager with three windows a person can operate, each showing one thing the
self-test asserts without a person. **Since sub-task 9.2 they are drawn by a
program**, `/bin/windows`, started by the entry point beside the shell and
drawing through the protocol of Section 10; at 9.1 they were kernel code.

- **Oxys** — the mark of [`../../art/logo.h`](../../art/logo.h), the disc in the
  accent and the figure in the ink, with the ground skipped so the paper shows
  through. At 9.1 this window held nine lines of instruction; it holds a picture
  because there is no face in userland yet, Section 12, limitation 4, and the
  windows say what they are by what they do. It held a ring of discs this
  project drew for itself until 2026-09-21 — a stand-in that outlived its
  reason by a day, the boot screen and the desktop having carried the owner's
  mark since the day before while the window a person actually opens carried
  the stand-in.
- **Pointer** — a disc that follows the pointer and grows while a button is
  held. Drag out of the window with the button held and the disc goes out of
  sight while the window still receives: the binding, made visible.
- **Keys** — a tile per character typed, its colour from the character, with a
  disc for a cursor; a backspace removes the last and Return clears them. It is
  made last and so holds the focus at the start; press another window and
  typing goes nowhere until it is pressed again.

Each is destroyed by the program upon its close event, and a desktop with
nothing upon it is the honest state that leaves; when the last is closed the
program's wait reports `EBADF` and it ends. The program asks the screen's size
and draws at twice the scale upon a screen at least 1024 wide: VirtualBox's 640
by 480 is where that was found necessary — twice, once for the kernel's
demonstration and once for the program's, which had first placed its windows by
a guess and stood the third of them at the screen's edge with nothing to see.

### 7.2 The menu

| Entry | Option | The screen is |
| ----- | ------ | ------------- |
| `Oxys-OS` | none | the window manager's; the shell is upon the serial line |
| `Oxys-OS (Shell-only)` | `shell-only` | the shell's, quiet: the banner, a prompt, the pointer following the mouse |
| `Oxys-OS (Shell Diagnostics)` | `diagnostics` | the shell's, after the boot log |

The second was the default entry until this sub-task and is kept, at the project
owner's direction, so that the shell can be reached with no serial line
attached; the third was named *diagnostics* and was renamed at the same
direction so that the two entries that give the shell the screen read as a pair.
The option the kernel reads did not change.

## 8. Observed state

Under QEMU at 1280 by 800, VirtualBox 7.2.0 at 640 by 480 and Bochs 3.1 at 1024
by 768, the image of this sub-task booted to the three windows with the
window manager's and the disc's self-tests passed and no verdict of `FAILED` —
save one VirtualBox boot of the two, whose serial loopback self-test reported
`A sequence did not return unaltered through the loopback` and passed upon the
rerun, a fault of that test's timing under that machine and not of this
sub-task; [`../project/TESTING-RECORD.md`](../project/TESTING-RECORD.md). Under
QEMU the mouse and the keyboard were driven through the monitor: text typed
arrived in the Keys window; the Pointer window reported the position and grew
its disc upon a press; a drag with the button held out of it reported minus two
hundred and one while the disc was out of sight; the Oxys window was dragged by
its band over the Pointer window and raised above it; its disc closed it, the
focus passed to the Pointer window, and the ground beneath it was repainted.
Under VirtualBox the keys arrived likewise. Bochs has no input and its evidence
is the log.

**Sub-task 9.2**, on 2026-09-17, the same again with the windows drawn by
`/bin/windows` at privilege level 3: under QEMU the tiles of `hello oxys`
stood in the Keys window, the disc followed the pointer and grew upon a press,
the Oxys window was dragged over the Pointer window and raised, and its disc
closed it — the program destroying it upon the close event; under VirtualBox at
640 by 480 the three windows stood scaled and apart once the program asked the
screen's size, and the keys arrived; under Bochs at 1024 by 768 the report after
the banner and the shell's prompt upon the serial line. Sixty-nine assertions in
each. One VirtualBox boot, run while Bochs was running upon the same host,
reported the shell's job-control session ending with the wrong status — a
session [`SHELL.md`](SHELL.md), Section 28, records as sensitive to the timing
of its two control bytes — and the boot rerun alone was clean;
[`../project/TESTING-RECORD.md`](../project/TESTING-RECORD.md).

## 9. Limitations of the manager

1. ~~**No client.**~~ **Closed at sub-task 9.2**, Section 10: a process owns a
   window through six calls. The queue has a producer upon the tick and a
   consumer in a system call, both upon the bootstrap processor with interrupts
   masked, and so needs no lock today; a user thread upon a second processor
   is what would make it need one, and [`CONCURRENCY.md`](CONCURRENCY.md),
   Section 10, limitation 1, lists `graphics/window.c` until it has it.
2. **The whole round runs inside the timer's handler**, Section 5.1. A window
   the size of the screen dragged across it composes and presents a million
   pixels a tick, in an interrupt handler. Three windows do not approach that;
   a desktop might, and the manager will then need a thread and the lock
   limitation 1 names.
3. **No resize and no hide** — ~~and no minimum stacking layer~~. A window is
   still the size it was made and is always shown. **The layers arrived at
   sub-task 9.5**: a root beneath every window and a panel above them all,
   each window stacking among its own layer and never outside it;
   [`SESSION.md`](SESSION.md), Section 2, and why an order alone could not
   express it.
4. **One damaged rectangle**, as the compositor's, and for its reason: two
   windows at opposite corners changed in one tick compose the screen between
   them.
5. **A window that does not intersect the changed region is not drawn, and one
   that does is drawn whole**, clipped. The primitives' clip is cheaper than any
   arithmetic here to avoid it, for three windows.
6. **The title is cut, not refused, at thirty-one characters**, and clipped
   short of the close control at whatever width the window has.
7. **The focus follows a press and nothing else.** No key moves it between
   windows, and no window can ask for it. Sub-task 9.5 added one rule and no
   more: **a root never takes it**, and the focus passed on when a window is
   destroyed skips it — [`SESSION.md`](SESSION.md), Section 2.3. A key that
   moved it, and a window that could ask, are still nobody's.
8. **Rounded corners and asymmetry are wanted and absent**, Section 4.
9. **The screen's mode is still the boot loader's.** VirtualBox gives 640 by
   480, and the demonstration scales its text to fit; nothing else does.

## 10. Sub-task 9.2: the client protocol

**Implementation**: [`../../graphics/client.c`](../../graphics/client.c) and
[`../../kernel/include/oxys/gfx/client.h`](../../kernel/include/oxys/gfx/client.h);
the calls in [`../../kernel/abi/oxys/syscall_abi.h`](../../kernel/abi/oxys/syscall_abi.h),
numbers 29 to 34, thirty-five in all; the wrappers `OxysWindowCreate`,
`OxysWindowDestroy`, `OxysWindowMove`, `OxysWindowBlit`, `OxysWindowEvent` and
`OxysWindowScreen` in [`../../libc/syscall/calls.c`](../../libc/syscall/calls.c);
`ThreadLaunch` in [`../../kernel/proc/process.c`](../../kernel/proc/process.c),
and the release of a process's windows at its ending, beside its descriptors.

### 10.1 What the first client decided

Section 1 said the shape on this side of the protocol would not change when a
client arrived, and it did not: a window is still a content and a queue. What
the first client decided was **what crosses**, and the answer is the one
`ARCHITECTURE.md`, Section 4.1, said sub-task 6.6 had left to be judged here.

**A surface does not cross.** `GraphicsSurface` describes kernel memory and
carries a clip stack; a program cannot be handed one, and a program handed the
content's pages by a shared mapping would hold memory whose lifetime is the
window's while the window's lifetime is the manager's — a mapping that must be
withdrawn when the window is destroyed, and a fault in the program when it is
withdrawn under it. What can be promised is simpler: **a rectangle of pixels the
program supplies, in one format whatever the screen's, will arrive in its
window.** So `window_blit` is a copy, validated whole before one pixel moves,
and the surface abstraction stays where it was — on this side, where the copy
is made with the same primitive the console draws with, `GraphicsPutPixel`,
upon a surface over the content. The judgement of 6.6 survives its first client
unchanged: a surface owns nothing and describes memory somebody else supplied,
and that is exactly what lets the kernel describe a client's window without the
client ever seeing the description.

**The client's pixel is `0x00RRGGBB` whatever the screen's format.** The kernel
encodes each on the way in, with the framebuffer's own `FramebufferEncode`,
which is the one function that knows the mode. A client that had to know the
format would be a client that broke when the boot loader chose another mode;
VirtualBox and QEMU already choose differently. The cost is a conversion per
pixel, which is Section 12, limitation 2.

**A queue does not cross either.** The manager keeps it; `window_event` takes
one entry across, converted field by field into `SyscallWindowEvent`, so that
the manager's event may change without the ABI moving — the rule
`CODING-STANDARDS.md` applies to a structure defined outside this project,
applied in the other direction to one defined inside it.

**A program with two windows must be able to sleep for either.** The first form
of `window_event` took one window, and the demonstration's three windows showed
at once that a program could block upon only one of them. `SYSCALL_WINDOW_ANY`
names all of the caller's, the event carries which it came from, and the scan
begins after the window last served, so that a busy window cannot starve a quiet
one.

**A program must be able to ask how big the screen is.** The first form had no
such call, the demonstration placed its windows by a guess, and VirtualBox's
640 by 480 put the third of them at the edge with forty-eight pixels showing.
`window_screen` is the sixth call.

### 10.2 Ownership

A window carries the identifier of the process that made it, set by the client
layer and attached no meaning to by the manager; the kernel's own carry zero,
which no process is. A call upon another process's window is `EBADF` — as a
call upon a descriptor the caller does not hold is, because that is what it is,
a handle that is not the caller's. **A process's windows are destroyed at its
ending**, beside its descriptors and for the reason
[`PROCESS.md`](PROCESS.md), Section 18, gives for the descriptors: a window
whose owner has ended is one nobody will draw upon or drain, and one left until
the collecting `wait` would stand upon the screen for as long as a background
job's parent took to notice.

### 10.3 The wait

`window_event` with `SYSCALL_WINDOW_WAIT` sleeps upon a channel until an event
arrives, as `read` of a pipe sleeps, and by the same discipline: the queue is
tested and the sleep entered within one masked section, so that a wake cannot
fall between the two. The wake is the tick's: after its round of routing,
`KernelServiceDisplay` wakes every sleeper where any event was routed, and each
re-tests its own queue. One channel for every window rather than one each,
because a wake is a broadcast that says only that something may have changed,
and the sleepers are few. A signal ends the wait with `EINTR`, as it ends every
sleep since 8.7. The kernel's own flow of control, which nothing can wake, is
refused with `ENOTSUP` rather than spun.

### 10.4 The demonstration is launched, not run

The entry point starts `/bin/windows` with `ThreadLaunch` — prepared as a forked
child is and admitted to the scheduler — and then runs the shell as before,
with neither waiting for the other. The program has no parent and is collected
by nobody when it ends, which is the orphan [`PROCESS.md`](PROCESS.md),
Section 19, records and which `init` of sub-task 9.3 exists to collect; until
then a launched program that ends holds one slot of the process table.

## 11. Verification of the protocol

`window-check` is run at privilege level 3 upon a manager holding a screen
composed in memory, as `signal-check` is run for the signals, and asserts for
itself what a program can; the kernel asserts around it what a program cannot.

**Why there is a kernel thread.** The program blits pixels and then waits for an
event, and the two halves that matter most — that the pixels arrived, and that
a sleeper is woken by an event — can be asserted only while the program is
asleep: after it has blitted and before it has ended. The kernel's own flow of
control cannot look then, having handed the processor to the program by
`ThreadStart` and getting it back only when the program ends. A thread the
scheduler runs when the program sleeps can, and that is the **hand**: pinned to
the bootstrap processor so that it touches the manager's tables upon the one
processor the program's calls touch them from, it yields until the program
sleeps, composes the screen and reads the pattern where the window stands,
injects a key into the window's queue, wakes the sleepers, and blocks itself for
good — the fixture thread of [`SCHEDULER.md`](SCHEDULER.md), Section 7, put to
a second use.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| The screen's bounds are the self-test's surface, and a bad address is `EFAULT` | A program placing windows by a guess, which is what the first form did |
| A window is made, its first event is the focus arriving, and an empty queue reports none | A create that returned a number naming nothing, so that every later call was `EBADF` and looked like a permission fault |
| A blit of the whole content is taken; one reaching outside it is `EINVAL`; one from an address the program may not use, or with a rectangle at one, is `EFAULT` | A blit clipped silently, so that a program's mistake painted the wrong pixels and reported success; a kernel reading a program's bad pointer at privilege level 0 |
| **The pixels are upon the screen**, read by the hand while the program sleeps: red the column, green the row, blue `0x5A`, where the content stands | A copy indexed by the wrong pitch, which shears the image by the difference each row; a conversion that swapped two channels — with `NULL` for the encoder the value must arrive unchanged |
| The focus passes between two windows of one process, read as "any" with the event naming its window, and passes back when one is destroyed; "any" with nothing queued reports none | A transfer delivered to the wrong queue; an "any" that reported the same event twice or named the wrong window |
| A destroyed window destroyed again, a number naming nothing, and the kernel's own window are each `EBADF`; a flag that does not exist is `EINVAL` | A program reaching another's window — the ownership check missing. **Observed**, Section 11.1 |
| A move is accepted; one beyond the coordinate limit is `EINVAL` | A coordinate truncated to thirty-two bits and a window placed where nothing meant it |
| The wait ends with the key the hand injected, and the program slept exactly once | A wait that returned at once with nothing, or slept for ever; a wake that reached nobody |
| The program's window, left standing on purpose, is gone at its ending — before `ProcessDestroy` — and the kernel's own survives | A window outliving its owner, upon the screen with nobody to drain it. **Observed**, Section 11.1 |
| A call from no process is `EBADF`; the program left no open file and no child; the hand ran and saw the program sleep | A test that passed because the hand never ran and the wait never happened |

### 11.1 The damage applied, and what the test said

Two defects, applied in one build and then the second alone:

```
Window clients: running window-check at privilege level 3, with a hand to read its pixels and wake it.
window-check: the client protocol, from privilege level 3.
  the kernel's own window could be destroyed by a program FAILED.
window-check: 1 assertion(s) failed.
  window-check FAILED: the status was 0x1 and not zero.
  the kernel's own window did not survive the program's ending
Window client self-test FAILED.
```

is the ownership check removed — every existing window the caller's — and

```
window-check: 0 assertion(s) failed.
  the program's ending did not destroy the window it left standing
Window client self-test FAILED.
```

is the release at the ending removed. **The second was masked by the first**
when both were applied: with ownership gone the program destroyed the kernel's
window, the count after the ending was one for the wrong reason, and the
assertion passed. That is why the second was applied alone, and why the record
says so: a negative test that is not run alone can be a negative test that
established nothing.

## 12. Limitations of the protocol

1. **No shared mapping.** Every pixel a program draws is copied, Section 10.1,
   and a program that redraws a large window at every event pays the copy each
   time; the demonstration coalesces the events it finds queued before it draws
   once. A mapping of the content into the program's address space is the
   optimisation, and it needs a lifetime the address-space layer does not yet
   express.
2. **The copy converts pixel by pixel.** `FramebufferEncode` per pixel, so that
   there is one encoder; a row-wide copy for the common case of a screen whose
   format is the client's is the day it is measured to matter.
3. **No resize, no title change, no hide.** A window is the size it was made
   and named what it was named. Section 9, limitation 3, for the manager's half.
4. ~~**No face in userland.**~~ **Answered at sub-task 9.5, the other way
   about**: `window_text` draws a run of text into a window with the system's
   own face, rather than a second face arriving in the library. The face is the
   kernel's under the kernel's licence and `libc/` is under another, so a copy
   there would be a relicensing this project may not perform; and there is
   exactly one face, which is what keeps a launcher's labels and the titles
   above them the same. [`SESSION.md`](SESSION.md), Section 4.
5. **The demonstration is an orphan**, Section 10.4, until `init` of 9.3.
6. **One process, one thread.** A second thread of one process reading the same
   window's queue would race the first upon it; there are no such threads, and
   the lock that the queue would then need is limitation 1 of Section 9.
