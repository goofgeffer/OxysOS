<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Window Manager

**Phase**: 9, sub-task 9.1, of [`../project/PLAN.md`](../project/PLAN.md).
Section 2 is what a window is; Section 3 is the stack, the focus and where an
event goes, which is the whole of what a window manager decides; Section 4 is
the appearance, judged against the preference
[`../project/INSPIRATIONS.md`](../project/INSPIRATIONS.md), Section 3, wrote
down before any of this existed; Section 5 is where it runs and what the screen
is for; Section 6 is the verification; Section 7 is the demonstration a person
operates and the menu that reaches it.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../../graphics/window.c`](../../graphics/window.c), with
[`../../kernel/include/oxys/gfx/window.h`](../../kernel/include/oxys/gfx/window.h).
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
to the window it belongs to. The programs that will own windows are sub-task 9.2,
which puts them on the far side of a protocol; until then the only owner is the
demonstration of Section 7, which is kernel code.

**What is decided now and will not change when a client arrives** is the shape
of the thing on this side of the protocol. A window is a rectangle of pixels the
owner draws into and a queue of events the owner drains, and the manager's whole
business is to compose the rectangles in their order and to put each event into
the right queue. A protocol carries those two things across a boundary; it does
not change what they are. The judgement `ARCHITECTURE.md`, Section 4.1, recorded
about the surface interface of 6.6 — that it was designed before any client
existed to design it against — applies to this interface too, and 9.2 is where
both are revisited.

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

**The palette**, supplied by the entry point in the framebuffer's encoding and
recorded here because a palette is a decision of this phase:

| Use | Colour | Why this one |
| --- | ------ | ------------ |
| The ground | `43, 52, 64` — a dark slate | Quiet, and dark enough that a window of warm white reads as the thing upon it. |
| A window's paper | `245, 243, 238` — a warm white | Not pure white, which glares beside black text at this face's weight. |
| The border | `32, 38, 46` | Darker than the ground by a little, so that a window upon the ground has an edge and a window upon a window has one too. |
| The band of the focus holder | `79, 134, 247` — one blue | The one colour reserved for the one thing that must be distinguished at a glance, which is where the keys go. |
| The band of every other window | `217, 214, 207` — a warm grey | Between the paper and the ground; a window that is not the focus is present and not asserting itself. |
| Title and disc upon the focus holder | white | Legibility upon the blue. |
| Title and disc upon the others | `74, 74, 74` | Legibility upon the grey. |

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
the windows' owner is given its turn; whatever changed is composed into the
back buffer and carried to the display with the pointer over it. A tick in
which nothing moved, nothing was pressed and nothing was drawn composes nothing
and presents nothing, which is most ticks.

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
self-test asserts without a person:

- **Oxys** — what to do, in nine lines.
- **Pointer** — the pointer's position in the content's coordinates, and a disc
  that follows it and grows while a button is held. Drag out of the window with
  the button held and the numbers go negative while the disc goes out of sight:
  the binding, made visible.
- **Keys** — the characters typed, with a disc for a cursor. It is made last and
  so holds the focus at the start; press another window and typing goes nowhere
  until it is pressed again.

Each destroys itself upon its close event, and a desktop with nothing upon it is
the honest state that leaves. The text is the face at twice its size upon a
screen at least 1024 wide and at its own size below that, the windows scaling
with it: VirtualBox's 640 by 480 is where that was found necessary, windows
sized for 1280 by 800 standing upon one another there with nothing to be read.

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

## 9. Limitations

1. **No client.** The windows' only owner is kernel code. Sub-task 9.2 puts a
   process on the far side of a protocol, and the queue — a producer upon the
   tick and a consumer in a system call, upon possibly different processors —
   then needs a lock it does not have; [`CONCURRENCY.md`](CONCURRENCY.md),
   Section 10, limitation 1, lists `graphics/window.c` until it does.
2. **The whole round runs inside the timer's handler**, Section 5.1. A window
   the size of the screen dragged across it composes and presents a million
   pixels a tick, in an interrupt handler. Three windows do not approach that;
   a desktop might, and the manager will then need a thread and the lock
   limitation 1 names.
3. **No resize, no hide, no minimum stacking layer.** A window is the size it was
   made, is always shown, and stacks among its peers; a panel that must stay
   above every window, or a root that must stay below them all, is sub-task
   9.5's to add, and a resize is a client's to ask for at 9.2.
4. **One damaged rectangle**, as the compositor's, and for its reason: two
   windows at opposite corners changed in one tick compose the screen between
   them.
5. **A window that does not intersect the changed region is not drawn, and one
   that does is drawn whole**, clipped. The primitives' clip is cheaper than any
   arithmetic here to avoid it, for three windows.
6. **The title is cut, not refused, at thirty-one characters**, and clipped
   short of the close control at whatever width the window has.
7. **The focus follows a press and nothing else.** No key moves it between
   windows, and no window can ask for it; both are decisions for the session of
   9.5.
8. **Rounded corners and asymmetry are wanted and absent**, Section 4.
9. **The screen's mode is still the boot loader's.** VirtualBox gives 640 by
   480, and the demonstration scales its text to fit; nothing else does.
