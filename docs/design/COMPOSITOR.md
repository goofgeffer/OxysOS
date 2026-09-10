# The Pointer and the Compositor

**Corresponding phase**: 6, sub-tasks 6.5 and 6.6. Section 1 is the pointer, the
first thing that ever composited; Section 2 is the compositor proper, which is
where several of the earlier documents' limitations are discharged and after
which **nothing reads the framebuffer**.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 4.

**Implemented by**: [`../../graphics/cursor.c`](../../graphics/cursor.c),
[`../../graphics/compositor.c`](../../graphics/compositor.c),
[`../../kernel/include/oxys/cursor.h`](../../kernel/include/oxys/cursor.h),
[`../../kernel/include/oxys/compositor.h`](../../kernel/include/oxys/compositor.h).

**Asserted by**: `KernelVerifyCursor` in
[`../../kernel/test/verify_mouse.c`](../../kernel/test/verify_mouse.c), and
`KernelVerifyCompositing` and `KernelVerifyCompositor` in
[`../../kernel/test/verify_compositor.c`](../../kernel/test/verify_compositor.c).

**Where this sits**: the last of the five documents the graphical work of
sub-tasks 6.2 to 6.6 is divided into. [`GRAPHICS.md`](GRAPHICS.md) is the index
and records why they are five. The pointer's device — the PS/2 mouse it takes its
movement from — is [`../devices/MOUSE.md`](../devices/MOUSE.md), which is a
different subject and was always a separate document.

---

## 1. The pointer of sub-task 6.5

The device that moves the pointer is documented in
[`../devices/MOUSE.md`](../devices/MOUSE.md), which also carries the shape and
every assertion made upon it — and, as history, the save-under that sub-task 6.6
removed. What belongs here is what the pointer says about the drawing this
document describes.

*This section was written at sub-task 6.5, when there was one surface and no back
buffer. Section 2 is what became of it, and the parts superseded there are
marked where they stand rather than deleted: the argument is why the compositor
was necessary.*

### 1.1 It is the first thing that composites

Everything drawn until now has been drawn once and left. The console writes a
glyph; the fault screen fills a page; neither has any obligation to what was
underneath, because nothing was. A pointer is the first object that must be
**removable** — drawn over whatever is there, and then taken away leaving that
whatever intact.

With one surface and no back buffer there is exactly one way to do it: read the
pixels back before drawing and write them again afterwards. That is what
`graphics/cursor.c` does, and it is the first use in this kernel of
`GraphicsPixelAt` outside a self-test.

[`FRAMEBUFFER.md`](FRAMEBUFFER.md), Section 6, declared that routine to exist "for the self-test", and noted that
nothing in a drawing path reads the surface it draws upon. That is no longer
true, and the reason it stopped being true is worth keeping: a compositor reads.
The remark stands as written for Sections 6 to 25, and this is where it ends.

### 1.2 What the surface abstraction bought

The pointer draws through `GraphicsPutPixel` and `GraphicsPixelAt` and names no
framebuffer. Two consequences follow directly, and both were the argument for
the abstraction in [`DRAWING.md`](DRAWING.md), Section 1:

- The pointer is asserted upon a surface composed in memory, pixel by pixel,
  upon a machine with no display — including the assertion that it writes nothing
  into the row padding, which a framebuffer could not be asked.
- Sub-task 6.6 substitutes a back buffer for the framebuffer and the pointer
  needs no change to be composited into it instead.

### 1.3 The cost, and why it is bounded

Reading is the expensive half. The framebuffer is mapped write-combining and
reads from write-combining memory are uncached — the same fact that made the
scroll the costly operation in [`CONSOLE.md`](CONSOLE.md), Section 6.2. The pointer is 12 by 18, so a move
costs 216 uncached reads and up to 216 writes.

Three things keep that from mattering:

1. **A move is per movement, not per packet.** The echo loop drains the mouse's
   event buffer and calls `CursorMoveTo` once. A hundred packets a second
   describe a path the eye cannot follow, and drawing the intermediate positions
   would pay the cost a hundred times to show nothing.
2. **A move to where the pointer already is costs nothing.** A stationary mouse
   still sends button packets at the full rate; without the early return the
   pointer would be erased and redrawn a hundred times a second for as long as
   nobody moved it.
3. **The pointer is shown only once the boot log is finished.** Each of those
   several hundred lines would otherwise conceal and reveal it — reading the
   pixels back each time — for a pointer nobody is yet moving.

### 1.4 The screen now has three owners, and the rule is unchanged

[`CONSOLE.md`](CONSOLE.md), Section 2.5, established that the screen has one owner at a time: the console
holds it, the drawing figures take it, and the fault screens take it for good.
The pointer does not take it. It is a fourth party that draws **over** whoever
holds it and must get out of the way when they draw.

`CursorConceal` and `CursorReveal` are that arrangement, and
`KernelWriteString` is where it is applied, being already the one routine
permitted to name an output device — the same fan-out point [`CONSOLE.md`](CONSOLE.md), Section 2.1,
established for the same kind of reason. The pair is counted rather than a flag
because a panic raised from within a write nests inside it.

The fault screens do not conceal the pointer; they hide it. Concealment implies
a reveal, and there will be none: the machine has stopped, and the last thing
drawn upon the display should be the fault screen alone.

### 1.5 What 6.6 removed

*Written before sub-task 6.6 and left as it was written, the answer following.*

> The save-under is a single-surface expedient and is expected to disappear. With
> a back buffer the display is redrawn each frame and the pointer is composited
> last, so nothing needs saving, nothing needs concealing, and
> `KernelWriteString` stops knowing that a pointer exists.
>
> What survives into 6.6 is the shape, the two-mask encoding of it, and the
> position — which belongs to the mouse driver and never belonged here. That is
> the test of whether this division was drawn in the right place, and it will be
> answered one sub-task from now rather than argued here.

**It was answered exactly so.** `CursorConceal`, `CursorReveal`, the concealment
count, the save-under store and its coordinates are gone; `KernelWriteString` no
longer names the pointer; and what remains in `graphics/cursor.c` is the two
bitmaps, the conversion of them into a surface and a mask, and a position it
reflects from the mouse driver. The division was drawn in the right place, and
the prediction is left standing above so that a reader may check the claim rather
than take it. See Section 2.4.

## 2. The compositor of sub-task 6.6

Four sub-tasks deferred something here, and all four deferred the same thing.

| Deferred by | What was deferred | Section |
| ----------- | ----------------- | ------- |
| 6.3 | A clip **stack**; a caller nesting regions saved and restored the clip by hand | 17.1 |
| 6.3 | **Blending**; every colour was opaque and a pixel was written, not combined | 17.2 |
| 6.4 | A **dirty-region** scheme; a scroll redrew the whole screen | 20.5 |
| 6.4 | **Double buffering**; drawing was visible as it happened | 20.6 |
| 6.4 | A **text cursor** the console could remove again | 20.3 |
| 6.5 | The **save-under**, and the concealment that made it safe | 26.5 |
| 6.4 | The framebuffer **read-back** of a scroll, which is the expensive half | 23.3 |

They are one thing because they have one cause: **the kernel drew directly upon
the framebuffer**, so anything that had to appear over something else had to
remember what was beneath it, and anything that wanted to read what was there had
to read it back through a mapping in which reads are uncached.

A back buffer removes the cause rather than the symptoms. The display is composed
in ordinary memory and carried to the adapter; what is beneath a thing is simply
still there.

### 2.1 What was substituted for what

`ConsoleInitialise` asked `GraphicsSurfaceFromFramebuffer` for a surface. It now
asks `CompositorSurface`. That is the whole of the change to the console's
drawing, and it is one line, because **a surface owns nothing**: it describes
memory somebody else supplied, and every primitive, every glyph and the scroll
itself go wherever it points without knowing which they got.

That was the argument for the abstraction in [`DRAWING.md`](DRAWING.md), Section 1, made four sub-tasks
before there was anything to spend it on. This is the sub-task that collects.

Where there is no compositor — no framebuffer, or an arena that cannot supply the
pages — the console takes the framebuffer as it did before and behaves exactly as
it did before. A machine with no display is not a machine with a broken display.

### 2.2 The layers are composited at presentation, not into the buffer

A layer drawn *into* the back buffer would have to be undone before the next
frame, which is the save-under again under a different name.

Compositing during the copy costs nothing extra. The pixels of the changed region
are being written to the framebuffer anyway; a pixel a layer covers is simply
composed differently on its way out. The back buffer is never disturbed, so what
a layer covered needs no restoring — it was never overwritten.

**Nothing reads the framebuffer.** The colour beneath a layer comes from the back
buffer, which is ordinary write-back memory. The framebuffer is written and never
read, which is the whole of the gain over [`CONSOLE.md`](CONSOLE.md), Section 6.2's arrangement, where a
scroll read four megabytes back through a mapping in which reads are uncached.

A layer is a surface, a position, and an optional **mask of one coverage byte per
pixel**. The mask is what makes a shape of arbitrary outline possible without a
colour reserved to mean "absent" — a reserved colour is a colour the shape may
not contain, and the pointer contains black and white, which are the two any
caller would reach for. The coverage is a byte and not a bit so that a shape with
a soft edge needs no change here, only a table with values between.

### 2.3 The damage, and why it is one rectangle

Regions accumulate as the rectangle enclosing them, not as a list.

Two cells at opposite corners of the screen therefore present the whole screen,
which is this scheme's cost. What it buys is that **no amount of drawing can
exhaust it**. A list needs a bound, and a compositor that had run out of entries
would have to present everything anyway — so the fragmenting scheme's worst case
is this scheme's ordinary one, and this one has no bookkeeping.

A presentation empties the damage. A presentation with nothing damaged writes
nothing and is not an error: it is what the echo loop does between keystrokes.

**A movement must mark both places.** A layer that marked only where it had
arrived would leave its previous appearance standing until something else
happened to change those pixels — which upon a pointer crossing a static screen
is a trail, and is precisely the fault the save-under was written to prevent.
Getting this wrong reintroduces the bug the sub-task exists to remove.

### 2.4 What this took out of the pointer

The pointer of sub-task 6.5 kept the 216 pixels it covered and put them back
before each move. That store was correct only while nothing else drew, so the
rest of the kernel had to declare that it was about to: `KernelWriteString`
concealed a pointer it had no business knowing existed, the concealment had to
nest, and the fault screens had to hide what they could not reveal.

None of it remains. `CursorConceal`, `CursorReveal`, the concealment count, the
save-under store and its coordinates are all gone, and `KernelWriteString` has
stopped knowing that a pointer exists. The pointer is rendered **once** into a
surface of its own with a coverage mask beside it, and a movement is one call
that marks two rectangles.

Section 1.5 predicted that what would survive was "the shape, the two-mask
encoding of it, and the position — which belongs to the mouse driver and never
belonged here", and said the prediction was the test of whether the division had
been drawn in the right place. It survived exactly that and nothing else.

### 2.5 Verification

Two groups, and the division is deliberate. The clip stack and the blending are
asserted **against surfaces composed in memory**, as every primitive since
sub-task 6.3 has been, so the whole of that holds upon a machine with no display.
The compositor itself cannot be: it owns one back buffer and one framebuffer and
there is no second of either to compose.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A push **intersects** the clip in force and does not replace it | A push that could widen the clip, so that a caller nesting a panel within a region it was handed draws outside that region |
| A pop restores what the matching push saved, and a pop with nothing pushed is refused | An underflow taking a rectangle from beyond the array, after which every drawing is clipped to whatever that held |
| The stack is bounded and a push beyond it is refused | An overflow overwriting the entry beneath, so that a pop restores the wrong region |
| A reset abandons the saved clips | A later pop narrowing the surface to a region long since finished with |
| Full coverage is exactly an opaque write; no coverage writes nothing | Everything drawn before this sub-task depends upon the first being unchanged; the second is the case where a read of the destination must not happen |
| The channels are combined **apart** | A packed pixel interpolated whole carries out of one channel into the next: red over blue produces a green neither contains |
| A masked surface arrives as the shape the mask describes | The mask indexed by the destination's pitch rather than the source's width, which shears the shape by the difference each row |
| Damage accumulates as the rectangle enclosing both regions | A compositor keeping only the latest leaves the earlier standing upon the display for ever |
| Damage outside the buffer is discarded | A region widened to cover pixels that do not exist, and a presentation that walks off the end of the buffer |
| A presentation empties the damage, and one with nothing damaged carries nothing | Every presentation writing the whole screen, which is the dirty-region scheme not working at all |
| The layer table refuses more than its capacity, and a layer with no surface | A layer written past the end of the table |

**The self-test gives back every layer it takes.** The first form of it did not,
and the consequence was a machine that booted with no pointer and reported no
failure: the table was exhausted by the test, `CursorInitialise` could not get a
layer, and every routine in the pointer then correctly did nothing. A self-test
that consumes a bounded resource must return it, or it is testing a machine
nobody else will ever run.

### 2.6 What only looking establishes

That the pointer leaves no trail, that text beneath it is intact after it has
passed, and that a fault screen is not carried away by the next presentation.
The procedure is in [`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 6.

The last of those is worth stating as a rule rather than a case. **A fault screen
draws straight upon the framebuffer**, the machine having stopped and the back
buffer holding a boot log that is no longer what should be on the screen — so
`FaultScreenBegin` suspends the compositor, permanently. Without that the next
`KernelWriteString`, and the panic path makes several, would carry the back
buffer over the top of the page just drawn. That is the fault of [`FAULTSCREEN.md`](FAULTSCREEN.md), Section 1's
console scrolling the screen out from under a fault screen, arriving again by a
different route, and it is the reason there is no resumption: everything that
suspends the display has stopped the machine.

### 2.7 Limitations

1. **One damaged rectangle.** Section 2.3. Two changes at opposite corners
   present everything between them.
2. **No text cursor yet.** [`CONSOLE.md`](CONSOLE.md), Section 3, limitation 3, named this sub-task as the one with the
   compositing needed to remove a text cursor again, and the compositing now
   exists — a cursor is a layer, and a layer is removed by hiding it. Nothing
   yet has a use for one: the echo loop does not need it, and a shell is Phase 8.
3. **The presentation is synchronous.** It happens where it is asked for and
   nothing waits for the adapter's vertical blank, so a large presentation can be
   seen to arrive. There is nothing to synchronise against without an interrupt
   from the adapter, which the Multiboot2 framebuffer does not offer.
4. **Layers are composited pixel by pixel.** The back buffer is copied by words
   and the layers are not, because a layer has a mask and a mask is per pixel.
   The pointer is 216 pixels, so this has nothing yet to buy.
5. **The back buffer is the size of the display and is never resized.** The mode
   is fixed by the boot loader and this kernel never changes it.
6. **The ordinary path is locked; drawing in general is not.** Since sub-task
   6.14 there is more than one processor, and every presentation on the ordinary
   path is reached through `KernelWriteString`, which holds the spinlock sub-task
   6.13 built. Its critical section is a whole call, and a call is exactly a
   presentation — which is the granularity this limitation always said was
   required: a processor drawing between the reading of the damage rectangle and
   its clearing would have its work discarded, the region that recorded it having
   been cleared by a presentation that never copied it.

   **What is not covered is a caller that draws through the primitives directly
   and presents afterwards.** There is one such caller today, the fault screen,
   and it is unprotected by decision — [`FAULTSCREEN.md`](FAULTSCREEN.md) says
   why. Sub-task 6.15 is what will produce others, and it is what must extend the
   lock to them.
