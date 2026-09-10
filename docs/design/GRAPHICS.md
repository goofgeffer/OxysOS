# The Graphical Work of Phase 6

**Corresponding phase**: 6, sub-tasks 6.2 to 6.6, which are the whole of the
graphical work that needs no process to exist.
[`ARCHITECTURE.md`](ARCHITECTURE.md), Section 4.1, records why they sit in
Phase 6 rather than in Phase 9, and what was given up by moving them.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 4.

**This document is an index.** It holds no design of its own. Everything it used
to hold is in the five documents below, one subject each, and Section 2 says why.

## 1. The five documents

Read them in this order if you are new to the subject: each depends upon the ones
before it, and the dependency is the reason the sub-tasks are numbered as they
are.

| Document | Subject | Sub-task | Implementation |
| -------- | ------- | -------- | -------------- |
| [`FRAMEBUFFER.md`](FRAMEBUFFER.md) | How a linear framebuffer is asked for, what is validated about the one supplied, why its pages are write-combining rather than write-back, how it is mapped, how a colour is encoded into it, and what happened to the text console that was using the screen already. | 6.2 | [`../../graphics/framebuffer.c`](../../graphics/framebuffer.c) |
| [`DRAWING.md`](DRAWING.md) | The surface, and why the primitives never name the framebuffer; clipping as the memory-safety boundary rather than a convenience; the pixel, the filled and outlined rectangle, the integer line, and the blit with the copy direction an overlap forces. | 6.3 | [`../../graphics/draw.c`](../../graphics/draw.c) |
| [`CONSOLE.md`](CONSOLE.md) | The bitmap face of ninety-five glyphs drawn for this project; the console above it, its four control characters, its scroll, and the buffer that replays what was written before a framebuffer could be mapped; and the measurement that found the console slow together with the specialisation that fixed it. | 6.4 | [`../../graphics/font.c`](../../graphics/font.c), [`../../graphics/console.c`](../../graphics/console.c) |
| [`FAULTSCREEN.md`](FAULTSCREEN.md) | The full-screen page a severe fault draws when the machine stops — one page for each fault rather than one page for all of them — what each must survive to be drawn at all, and the verification run that sub-task 6.4 closed with. | 6.4 | [`../../graphics/faultscreen.c`](../../graphics/faultscreen.c) |
| [`COMPOSITOR.md`](COMPOSITOR.md) | The pointer, which was the first thing that ever composited, and the compositor proper that put a back buffer beneath all of the above — after which **nothing reads the framebuffer**. Several of the earlier documents' limitations are discharged here. | 6.5, 6.6 | [`../../graphics/cursor.c`](../../graphics/cursor.c), [`../../graphics/compositor.c`](../../graphics/compositor.c) |

Two subjects that touch this work are documented elsewhere and always were. The
PS/2 mouse the pointer takes its movement from is
[`../devices/MOUSE.md`](../devices/MOUSE.md); the VGA text-mode display that
sub-task 6.2 displaced is [`../devices/DISPLAY.md`](../devices/DISPLAY.md).

## 2. Why this is five documents and not one

It was one document of 1,663 lines, and its own opening block had become a table
of contents: it enumerated which sections belonged to which sub-task, because
there was no other way for a reader to find the third of five subjects. A header
that has to explain how to navigate its own document is that document saying it
has become several — the same evidence
[`ARCHITECTURE.md`](ARCHITECTURE.md), Section 2.2, records for a translation unit
whose `Purpose` line had stopped describing it.

That section's rule applies here without amendment. **The division is along the
lines the faults fall upon.** A fault in the framebuffer is a display that is
garbage or a machine that is inexplicably slow; a fault in the primitives is the
wrong pixels; a fault in the font or the console is text that cannot be read; a
fault in a fault screen is the report you get when everything else has already
failed being wrong itself; a fault in the compositor is a trail, a tear, or a
screen that does not change. Five different kinds of wrongness, each found a
different way, each with its own file behind it.

**A division is not a rewrite.** Nothing was reordered, reworded or improved in
passing. Every line of the original stands in exactly one of the five, and the
check was mechanical: the five bodies rejoined differ from the original in
**heading lines alone** — the section numbers, which had to be renumbered from
1 to 10, 11 to 17, 18 to 23, 24 to 25 and 26 to 27 into five sequences each
beginning at 1. The cross-references those renumberings broke were rewritten in
the same change, and a reference that now crosses a document carries that
document's name.

Two things the division made necessary, and both are recorded where they happen
rather than only here:

- **`FAULTSCREEN.md`, Section 2, covers three subjects.** It is one verification
  run, and that run asserted the drawing optimisation, the exception disposition
  and the fault screens together. It is kept whole, because a verification run is
  evidence of what was actually done, and dividing it three ways would make it
  evidence of three things that never happened at once.
- **`FAULTSCREEN.md` acquired a limitations section.** The original had none for
  the fault screens, and [`../README.md`](../README.md) requires every document to
  end with one. Its five items are gathered from statements already in the text,
  each citing the section it came from, and nothing new is claimed.
