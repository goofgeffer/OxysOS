# The PS/2 Mouse and the Pointer

**Corresponding phase**: Phase 6, sub-task 6.5.

**Specifications**: IBM Personal System/2 auxiliary device; the 8042 controller
command set; the PS/2 device command set and the standard movement packet; the
wheel extension of the same.

**Implementation**: [`../../drivers/ps2/ps2.c`](../../drivers/ps2/ps2.c),
[`../../drivers/mouse/mouse.c`](../../drivers/mouse/mouse.c) and
[`../../graphics/cursor.c`](../../graphics/cursor.c), declared by
`<oxys/ps2.h>`, `<oxys/mouse.h>` and `<oxys/cursor.h>`.

---

## 1. Three things, and the lines between them

This sub-task adds one device and produces three modules. The division is the
substance of the design, so it is stated before anything else.

| Module | Owns | Knows nothing of |
| ------ | ---- | ---------------- |
| `drivers/ps2/ps2.c` | The 8042 controller: its ports, its self-test, its two device ports, and the one configuration byte governing both. | What is attached to either port. |
| `drivers/mouse/mouse.c` | The mouse: its command sequence, its packet, the framing of the stream, and the pointer position accumulated from the movements. | The display, and how a pointer is drawn. |
| `graphics/cursor.c` | The pointer's picture: its shape, the pixels it stands upon, and the restoring of them. | The mouse, and where the position came from. |

Each line is drawn where it is for a reason recorded below: Section 2 for the
controller, Section 6 for the position, and Section 7 for the picture.

## 2. The controller stopped being the keyboard's

Until this sub-task the 8042 was driven by `drivers/keyboard/keyboard.c`, and
that was right while there was one device upon it. The 8042 and the keyboard are
reached through the same pair of ports, and separating them would have been a
distinction with no reader.

A second device makes the distinction load-bearing, and the reason is one byte.
The controller's configuration byte is not the keyboard's and is not the
mouse's:

| Bit | Meaning when set | Whose |
| --- | ---------------- | ----- |
| 0 | The first port may raise IR1 | The keyboard's |
| 1 | The second port may raise IR12 | The mouse's |
| 4 | The first port's clock is stopped | The controller's |
| 5 | The second port's clock is stopped | The controller's |
| 6 | Scan code set 2 is translated into set 1 | The controller's, on the keyboard's behalf |

It is read, modified and written **whole**. Two drivers each keeping their own
idea of it would each write back the other's bits as they last saw them: the
mouse driver, enabling bit 1, would restore bit 6 to whatever it had been when
the mouse driver first looked. The keyboard would then deliver scan code set 2
while decoding it as set 1 — which is not a failure to work but a failure to be
right, the two sets overlapping without agreeing, so that most keys still produce
plausible characters and some produce the wrong ones.

The controller therefore has one owner. It is initialised once, before either
driver, and each driver asks it for the port it wants.

### 2.1 The translation went with it

Bit 6 is set by `Ps2Initialise` and not by the keyboard driver, and that is where
it belongs: **it is the controller that translates**. A keyboard sends set 2
whatever this kernel does, and set 1 is what appears at the data port only while
that bit stands. The keyboard driver decodes set 1 because the controller was
established to present it, and now says so rather than assuming it.

### 2.2 The second port's existence is discovered

No register reports how many device ports a controller has, so the question is
put indirectly. The second port is enabled, and the configuration byte read
back: a controller that has one has started its clock, so bit 5 is found clear. A
controller with one port ignores the command and the bit stands as it was.

The test is performed **before** either port's own test, because testing a port
that does not exist is how a machine with one port comes to report a failure it
did not have.

## 3. The movement packet

A packet is three bytes, or four where the device has a wheel.

| Byte | Bits | Meaning |
| ---- | ---- | ------- |
| 0 | 0, 1, 2 | Left, right and middle buttons, set while held |
| 0 | 3 | **Always set** |
| 0 | 4, 5 | The signs of the horizontal and vertical movements |
| 0 | 6, 7 | Overflow of the horizontal and vertical movements |
| 1 | all | The magnitude of the horizontal movement |
| 2 | all | The magnitude of the vertical movement |
| 3 | 0 to 3 | The wheel movement, two's complement, where a wheel was found |

Three properties of this layout each have a way of being got wrong that produces
working, wrong behaviour rather than an error.

### 3.1 The movement is nine bits, not eight

The magnitude is a whole byte and its sign is a bit of another byte, so the
quantity is **nine** bits:

| Sent | Means |
| ---- | ----- |
| magnitude `0xFF`, sign set | −1 |
| magnitude `0x01`, sign clear | +1 |
| magnitude `0x00`, sign set | **−256** |

The obvious implementation — cast the magnitude to a signed char and ignore the
sign bit — gets −1 right by accident and turns −256 into **0**. The movement is
lost silently, and only for the largest movements a hand can make within one
report period, which is to say only when somebody moves the mouse quickly. The
driver subtracts 256 where the sign bit stands, and the self-test asserts the
−256 case specifically.

### 3.2 The vertical sense is inverted

A mouse measures vertical movement **positive upward**. A display measures it
positive downward. The negation is performed in `MouseCompletePacket`, at the one
place that knows what the device meant, so that every consumer above receives the
screen's sense.

A driver that forwarded the device's sign produces a pointer that moves correctly
horizontally and backwards vertically — which presents as a broken mouse rather
than a broken driver, and is diagnosed as one.

### 3.3 An overflow means the magnitude is not the movement

Bits 6 and 7 are the device saying the movement exceeded what nine bits express,
so the magnitude it sent is the low bits of a larger number. Using it would jump
the pointer somewhere arbitrary. The movement is discarded and counted; the
buttons in the same byte are unaffected and are kept.

## 4. Framing, which is the fault worth the most care

The device sends a stream of bytes with nothing in it to say where a packet
begins.

A driver that has lost its place **does not stop working**. It reads the second
byte of one packet as the first byte of the next, and thereafter reports button
states taken from a movement magnitude and movements taken from a button byte,
indefinitely, with no error anywhere. Every value it produces is a value the
device could have sent.

Bit 3 of the first byte is set in every packet. Refusing any byte that lacks it
while a packet is being awaited is therefore a test a mis-framed stream fails and
a correct one cannot, and it recovers by itself: bytes are discarded singly until
one arrives that could begin a packet. The discards are counted, so a fault the
driver corrects is still visible to whoever reads the report.

`MouseFlush` abandons a partly received packet along with the buffered events,
for the same reason. Retaining the fragment would mean the next byte to arrive
was appended to a packet whose remaining bytes had been discarded — the very
mis-framing the always-set bit exists to prevent, introduced by the routine whose
purpose is to restore a known state.

## 5. Both devices deliver through one byte

The controller holds **one** byte for the processor at a time, and both devices
deliver through it. Status bit 5 says which port the byte standing there came
from, and it must be read **before** the data port, since reading the data clears
the bit along with the buffer.

The two handlers are woken by different lines, IR1 and IR12, but the byte waiting
when a handler runs is not necessarily that handler's: a movement packet arriving
while a keystroke is being serviced is routinely presented to whichever handler
runs next. A handler that discarded such a byte would lose one byte of a
three-byte packet — and by Section 4 that is not one lost movement but a decoder
out of step with every packet after it.

Each handler therefore reads the byte, sees which port it came from, and hands it
to the other driver where it is not its own. The read cannot be undone, so
handing it across is the only alternative to losing it.

## 6. The position is kept by the driver

A mouse reports movement, not position. The position is the running sum of the
movements, and something must keep it.

It is kept in the driver, next to the decoder, for the same reason the keyboard
driver keeps the modifier state rather than handing every consumer the make and
break codes: **a sum is only correct if exactly one thing performs it**. Were the
position kept by a consumer, a consumer that missed an event — because the buffer
overflowed, or because it was not reading at that moment — would not lose one
movement but would be permanently displaced by it, with nothing to correct it
against for the rest of the machine's life.

This is also why an event carries the position and not only the movement, and why
the echo loop reads the position from the driver rather than from the last event
it drained.

The bounds are **not** the driver's to know, a mouse having no idea what it is
pointing at. They are supplied through `MouseSetBounds` by whoever knows the
display. Until they are, they are one pixel by one pixel and the position is
therefore the origin: a driver that defaulted to some plausible display size
would be reporting a position it had invented, and a caller could not tell that
from one it had measured.

## 7. The pointer, and the pixels beneath it

*Written at sub-task 6.5 and superseded by 6.6, which is recorded in Section 8.2
and in [`../design/COMPOSITOR.md`](../design/COMPOSITOR.md), Section 2. It is kept
because the difficulty it describes is why the compositor was written.*

A pointer is not drawn once. It is drawn, then it moves, and what was beneath it
must reappear — and until sub-task 6.6 there was one surface and no back buffer
to redraw the display from. The pixels beneath were therefore read before it was
drawn and written back before it was drawn elsewhere.

The shape is two bitmaps rather than one, because a pointer needs three states
and one bitmap offers two: transparent, outline, or interior. That is what makes
it visible upon any background — a white arrow vanishes upon white and a black
one upon black, and an arrow that is white within a black outline vanishes upon
neither. It was drawn for this project, as the face of sub-task 6.4 was, and for
the same reason: `PROJECT_GUIDELINES.md`, Section 2.

### 7.1 The save-under is only correct while nothing else draws

If the console prints a line while the pointer is shown, one of two things
happens. The console writes over the pointer, and the pointer's next move
restores stale pixels over the text; or the pointer moves first, restoring pixels
the console has since legitimately overwritten. Both leave debris that **no
assertion could detect**, because every pixel involved holds a value something
meant to write.

`CursorConceal` and `CursorReveal` are how the rest of the kernel says it is
about to draw, and `KernelWriteString` is where the saying is done — it is
already the one routine permitted to name an output device, so it is the one
place every console write passes through.

They are a **counted pair** and not a flag because the situations nest: a panic
raised from within a write would otherwise reveal the pointer in the middle of
the write that concealed it, and the save-under would then record a half-finished
line as the display beneath.

The fault screens of sub-task 6.4 hide the pointer outright rather than
concealing it. Not because it would spoil the page, which is about to fill the
screen anyway, but because nothing will restore it: the machine has stopped, and
the last thing drawn upon the display should be the fault screen and nothing
else.

### 7.2 The saved coordinates, and why they are not the position

`CursorMoveTo` advances the position **before** it repairs the old location, so
the store's own record of where its pixels came from is what the repair uses.
Written the other way round the two would agree at every restore, and a repair
that used the position instead would be indistinguishable from a correct one —
until some later caller moved the pointer and repaired afterwards, at which point
it would paint stale pixels over the new position and leave an arrow standing at
the old one for ever. The negative test of Section 8.2 is exactly that damage.

### 7.3 What this costs, and what replaces it

Reading is the expensive half: the framebuffer is mapped write-combining, and
reads from write-combining memory are uncached — the same fact that made the
console's scroll the costly operation of `CONSOLE.md`, Section 6. It is 216
pixels, paid once per **movement** and not once per packet: the echo loop drains
the event buffer and moves the pointer once, and `CursorMoveTo` returns
immediately where the position has not changed, which is what a stationary mouse
still sending a hundred button packets a second requires.

**Sub-task 6.6 removed all of it.** With a back buffer the pointer is composited
as the changed region is carried to the display, so no pixels need saving, and
the save-under turned out to be what made a pointer possible one sub-task early
rather than a mechanism the kernel kept. Sections 7, 7.1 and 7.2 above describe
the arrangement as it stood between sub-tasks 6.5 and 6.6 and are left standing:
what they say about *why* a pointer needs more than a bitmap is still true, and
the cost they describe is what a reader should understand the compositor to have
removed. What replaced them is `COMPOSITOR.md`, Section 2.4.

One thing carried over unchanged and is worth naming, because the compositor did
not make it unnecessary: `CursorMoveTo` still returns immediately where the
position has not changed. A stationary mouse sends a hundred packets a second and
each would otherwise mark two rectangles as changed, so the display would be
carried out a hundred times a second for a pointer standing still.

## 8. Verification

Neither self-test needs a mouse, and that is the point of both.

### 8.1 The decoder, driven directly

`MouseProcessByte` is exposed for the same reason `KeyboardProcessScancode` is:
the decoding of a packet is not a property of the 8042, and a byte arriving by
any route decodes identically. `KernelVerifyMouse` composes packets and asserts:

| Assertion | The silent failure it catches |
| --------- | ----------------------------- |
| A plain packet yields its movement, position and buttons | A decoder that reads the bytes in the wrong order |
| Vertical movement is inverted | A pointer that moves backwards vertically, diagnosed as a broken mouse |
| Magnitude `0xFF` with the sign bit is −1 | — |
| Magnitude `0x00` with the sign bit is −256 | Eight-bit sign extension: the largest movements silently lost |
| The position stops at each of the four edges | An off-by-one that puts the hot spot one pixel outside the display, where the clip declines to draw it and the pointer appears to stick short of the edge |
| A button transition is reported once and named | A consumer obliged to keep the previous state, and to get it right |
| An overflowed movement is discarded, its buttons kept | The pointer jumping to an arbitrary position under fast movement |
| A byte lacking bit 3 is refused and counted, and the stream recovers | Section 4: indefinite plausible nonsense |
| A flush abandons a partly received packet | The same mis-framing, introduced by the routine meant to prevent it |
| A full buffer discards the newest and counts it | Events overwritten in place, appearing as movements nobody made |

What cannot be asserted that way — that a device answered, that the line was
claimed, that the controller agrees the port is usable — is checked against the
driver's own report and skipped where no mouse was found. A machine may genuinely
have none, and the driver is required to discover that without blocking;
reaching the assertion at all is evidence that it did.

### 8.2 The pointer, and what sub-task 6.6 changed about asserting it

Until sub-task 6.6 the pointer drew itself upon a surface and kept the pixels
beneath it, so what there was to assert was the drawing and the restoration: a
trail left behind, a background overwritten, a concealment that failed to nest.
The surface was composed in memory with a pitch exceeding its width, so that a
draw computing an address from the width was caught writing into the padding.

None of that exists now. The pointer is rendered once and composited, so what is
left to assert here is the **shape** and the **rendering**, and the compositing
is asserted where it lives — `COMPOSITOR.md`, Section 2.5.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Every interior pixel is also opaque | The two bitmaps are separate data and nothing else makes them agree. An interior pixel that were not opaque is a hole in the shape drawn in the interior colour, invisible in the picture comments because each is written beside its own row |
| The hot spot is part of the shape | A pointer whose tip is transparent points at a pixel it does not draw, and cannot be aimed |
| The shape covers neither everything nor nothing | Either is what a mask read with the wrong bit order produces |
| The rendered surface is the size of the shape | — |
| Coverage follows the opacity bitmap exactly | A mask taken from the wrong bitmap gives a pointer that is a solid rectangle |
| Each covered pixel is the colour its interior bit chooses | An interior test inverted gives a pointer drawn inside out, which upon a black background looks almost right |
| Showing twice leaves it shown; a move goes where it was sent | — |
| A move to where the pointer already stands is **not** counted | Section 7.3: the display carried out a hundred times a second for a hand at rest |

The rendering is worth its own assertions because it is a conversion between two
representations of the same thing, and those are where a shape silently loses a
column or gains one.

### 8.3 The negative tests

Each was applied, `make verify` run, and the change reverted.

| Damage | Result |
| ------ | ------ |
| Forward the device's vertical sign rather than inverting it | `A movement was decoded with the wrong sense or magnitude.` |
| Sign-extend the magnitude as eight bits | `A movement of minus 256 was decoded as zero.` |
| Restore the saved pixels at the pointer's position rather than where they came from | `The pointer left a trail where it had been.`, and three further failures |
| Save what is beneath the pointer after drawing it rather than before | `Hiding the pointer did not restore what was beneath it.`, and three further failures |

The third is the one worth noting. Applied to the code as first written it
**passed**, because `CursorMoveTo` repaired the old location before advancing the
position, so the saved coordinates and the position agreed at every restore and
the damage could not show. The order was changed — position first, repair second
— which makes the saved coordinates load-bearing and the test meaningful. See
Section 7.2.

The last two applied to the save-under, which sub-task 6.6 removed, and they are
recorded here as what was done rather than as a procedure to repeat: there is
nothing left to damage in that way. The equivalent fault now lives in the
compositor — a layer that marks only where it has arrived and not where it was —
and is provoked and recorded in `../project/TESTING-GRAPHICS.md`, Section 6.

## 9. Observed state

Under QEMU 1280 by 800, with no mouse moved:

```
8042 controller: present, first port usable, second port usable.
8042 controller: configuration 0x40, translation on, commands 11, timeouts 0, bytes drained 0.
PS/2 mouse: present with a wheel, four-byte packets, line unmasked.
PS/2 mouse: bytes 316, packets 78, mis-framed 2, overflowed 1, discarded 4.
PS/2 mouse: pointer at 640, 400 within 1280 by 800, buttons 0x0.
Pointer: hidden at 640, 400; 12 by 18, composited, moves 0.
```

Every figure but the position is the self-test's own work, and is named here so
that a reader is not left to wonder why a machine nobody touched has decoded
seventy-eight packets, mis-framed two bytes and discarded four events. The
configuration byte reads `0x40` because the report is printed before either
driver enables its port's interrupt; the two interrupt bits are set afterwards,
and the interrupt controller's own report shows IR1 and IR12 claimed and
unmasked.

### 9.1 A caveat for headless capture

A keystroke injected through the QEMU monitor **before the controller has been
initialised** makes `Ps2Initialise` fail, and the machine then reports no
controller, no keyboard and no mouse. The byte lands in the controller's output
buffer after the single drain at the start of the sequence and is read as the
answer to one of the commands that follow.

This is an artefact of injecting a keystroke into an emulated controller whose
ports are disabled; on a real machine a disabled port is not scanned. It matters
only because it is an easy way to spend an hour diagnosing a driver that is
working: when capturing a screenshot, either let GRUB's timeout elapse or send
the keystroke and wait for the boot to finish before drawing conclusions from the
report.

## 10. Limitations

1. **The fourth and fifth buttons are not decoded.** The driver interrogates for
   the wheel alone, so a five-button device has not been put into the mode where
   the upper nibble of the fourth byte means those buttons. Reading them would be
   reading bits whose meaning was never established.
2. **The wheel movement is carried in events and used by nothing.** There is
   nothing yet to scroll.
3. **Acceleration is not applied.** Scaling is set linear at the device, so a
   movement is the distance the hand moved. Any acceleration is a policy for
   whoever draws the pointer, and belongs with the window system of Phase 9.
4. **A USB mouse is invisible.** Machines that present one through the firmware's
   PS/2 emulation work; those that do not will report no mouse. The same is true
   of the keyboard and has been since sub-task 3.7.
5. ~~**The save-under is a single-surface expedient.**~~ Removed by sub-task 6.6,
   which replaced the whole arrangement rather than improving it. The pointer is
   a compositor layer: `CursorConceal`, `CursorReveal`, the concealment count and
   the store of saved pixels are gone, and `KernelWriteString` no longer knows a
   pointer exists. See Section 8.2 and
   [`../design/COMPOSITOR.md`](../design/COMPOSITOR.md), Section 2.
