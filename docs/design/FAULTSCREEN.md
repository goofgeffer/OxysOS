<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Fault Screens

**Corresponding phase**: 6, sub-task 6.4 — the full-screen page a severe fault
draws when the machine stops, one page for each fault rather than one page for
all of them.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2 and 4.

**Implemented by**:
[`../../graphics/faultscreen.c`](../../graphics/faultscreen.c),
[`../../kernel/include/oxys/faultscreen.h`](../../kernel/include/oxys/faultscreen.h).
The disposition each screen reports is decided by `ExceptionDispositionOf` in
[`../../kernel/arch/x86_64/interrupt/exceptions.c`](../../kernel/arch/x86_64/interrupt/exceptions.c), whose design is
[`INTERRUPTS.md`](INTERRUPTS.md), Section 8.1.

**Asserted by**: `KernelVerifyFaultScreen` in
[`../../kernel/test/verify_faultscreen.c`](../../kernel/test/verify_faultscreen.c).

**Specifications**: Intel 64 and IA-32 Architectures Software Developer's
Manual, Volume 3A, Chapter 6 and Table 6-1, Sections 6.13 and 6.15 with Figures
6-6 and 6-9: the exceptions each screen accounts for, and the two forms of error
code they decode.

**Where this sits**: the fourth of the five documents the graphical work of
sub-tasks 6.2 to 6.6 is divided into. [`GRAPHICS.md`](GRAPHICS.md) is the index
and records why they are five.

**Section 2 covers three subjects and not one.** It is the verification run that
sub-task 6.4 closed with, and that run asserted the drawing optimisation of
[`CONSOLE.md`](CONSOLE.md), Section 6, the disposition of
[`INTERRUPTS.md`](INTERRUPTS.md), Section 8.1, and the screens below, in one
pass. It is kept whole and kept here rather than divided three ways, because
three of its five subsections are about these screens and because a verification
run is evidence of what was actually done — dividing it would make it evidence of
three things that never happened together.

---

## 1. The fault screens

A framebuffer took the operator's view away in sub-task 6.2 and [`CONSOLE.md`](CONSOLE.md), Section 2, gave
it back for the boot log. It did not give it back for the thing a person most
needs to see, which is what happened when the machine stopped.

Every fatal fault has always been reported in full, upon the serial port and upon
the text display. In a graphics mode the text display is not shown, and a serial
adapter is not something most machines have — VirtualBox does not. Such a person
saw the boot log stop, and nothing else.

### 1.1 What draws one, and what does not

**A fault screen is drawn for a fault the kernel cannot survive, and for no
other.** That decision is not made here; it is made by `ExceptionDispositionOf`,
and `docs/design/INTERRUPTS.md`, Section 8.1, is where it is set out.

The distinction was got wrong first and the correction matters. Every exception
was treated as fatal, so a divide by zero — the plainest mistake a program can
make, and one that must cost that program and nothing else — would have halted
the machine and drawn a full-screen page announcing it. A screen that says the
system has stopped, shown for a fault that ought to have ended one program, is
not a cosmetic error: it is a false account of what happened, given to the person
least able to check it.

What reaches this file is therefore only:

- an **abort** — a double fault or a machine check — which the architecture
  permits no resumption from, whatever raised it;
- a **non-maskable interrupt**, which is hardware announcing a condition rather
  than a program erring;
- a **malformed descriptor table**, `#TS` or `#NP`, whose faulty structure is the
  kernel's own however it was reached;
- **any fault the kernel raised within itself**, where there is no program to
  blame and nothing smaller than the machine to abandon;
- a **panic** the kernel raised by its own check.

A divide by zero, an invalid opcode, an unresolved page fault or an alignment
check raised by a program draws nothing here at all. From Phase 7 the program
ends and the machine carries on.

The titles say so. They are `KERNEL PAGE FAULT` and `KERNEL PROTECTION FAULT`,
not `PAGE FAULT` and `GENERAL PROTECTION FAULT`, because by the time one is on
the screen that is what it is.

### 1.2 Why there is more than one screen

Because the faults are not one thing.

A page fault names an address and asks what was meant to be mapped there. A
general-protection fault names a selector, or names nothing at all and is then
about the instruction. An invalid opcode asks what the bytes at the instruction
pointer are. A double fault says that the processor could not deliver something
else, and that **the earlier fault is the one worth finding** — the screen for it
says so in those words, because a reader looking at a double fault's registers is
looking at the wrong fault.

One screen carrying one register dump would present all of that identically and
would tell the reader nothing about which question to ask. So each severe fault
has its own title, its own colour, a sentence saying what the processor is
reporting, a sentence saying what to examine first, and **the evidence that bears
upon that fault and not upon the others**.

| Vector | Screen | Evidence it carries |
| --- | --- | --- |
| 2 NMI | Non-maskable interrupt | The control registers |
| 6 `#UD` | Bad instruction in kernel | The instruction bytes, and the stack |
| 8 `#DF` | Double fault | The stack, and the control registers |
| 10 `#TS` | Malformed task state segment | The selector |
| 11 `#NP` | Descriptor not present | The selector |
| 12 `#SS` | Kernel stack fault | The selector, and the stack |
| 13 `#GP` | Kernel protection fault | The selector, and the instruction bytes |
| 14 `#PF` | Kernel page fault | The faulting address and its cause, and the control registers |
| 18 `#MC` | Machine check | The control registers |
| — | Kernel panic | The message naming the check that failed |

A vector outside that table which is nevertheless fatal receives the general
screen, `UNEXPECTED KERNEL FAULT`. It names the exception from the dispatcher's
own mnemonics, carries the general evidence, and says plainly that this kernel
has no account written for it — adding that most exceptions reaching it are
ordinary mistakes of a program, so arriving there means the kernel made one. That
is what a divide by zero within the kernel gets, and it is the right treatment: a
kernel that divides by zero has a bug, and the useful facts are the instruction,
the stack and the registers, not a lecture about arithmetic.

**`#AC` has no screen and must not have one.** Intel SDM, Volume 3A, Section
6.15: an alignment check requires privilege level 3, `CR0.AM` and `RFLAGS.AC`
together, so kernel code cannot raise one however it is written. A screen for it
would be a page nobody could ever see and a claim that the kernel treats a
program's mistake as the end of the machine. Section 2.3 refuses one.

The panic is separate from all of them deliberately. An exception is the machine
saying something went wrong; a panic is this kernel saying it has found the world
in a state it does not know how to continue from, and the message names the check
that failed rather than any register.

### 1.3 What it must survive

Every routine runs inside a fault handler, upon a machine that has already gone
wrong, and the one thing it must not do is go wrong itself: a fault raised while
drawing a fault screen is a double fault, and one raised while handling that
resets the machine with nothing written anywhere.

**It allocates nothing.** The heap is a thing the fault may have corrupted.

**It reads nothing without asking the paging hierarchy first.** The instruction
bytes and the stack words both go through `PagingTranslate`, and a word that does
not translate is reported as absent rather than fetched. That report is often the
most useful thing on the page: "the stack pointer names memory that is not
mapped" *is* the diagnosis.

**It draws once.** A second call means a fault occurred while the first screen
was being drawn, and overwriting the first would destroy the only account of the
original failure.

**It draws nothing where there is no framebuffer**, that being the case in which
the display driver is already showing the report.

### 1.4 The screen changes hands

The console is suspended when a fault screen begins, and this was not foreseen —
it was found by looking at a screen that had been drawn correctly and displayed
wrongly.

`KernelPanic` follows every fatal exception, and it writes to the diagnostic
path, and the diagnostic path includes the console, and the console is upon this
framebuffer. Its cursor stood at the foot of a screen full of boot log, so each
newline of `KERNEL PANIC: ...` **scrolled the whole framebuffer up by eight
pixels**. Three newlines carried the banner off the top of the display and
shifted the entire layout by three character rows.

`ConsoleSuspend` ends it: the console stops drawing and stops recording, and the
display driver and the serial port go on receiving everything. There is no
resumption, a machine that has drawn a fault screen being one that is halting.

### 1.5 The two demonstrations, which prove different things

`fault-screen=<vector>` composes a trap frame and draws that vector's page. It
proves **the page**: that its text fits the display, that its panels lay out one
beneath another, that its colour and title are its own. It proves nothing about
the processor, and the frame is filled with values no machine would produce —
repeated nibbles, and an obviously artificial address — so that a photograph of
it cannot be filed as evidence of a fault that occurred.

`fault-raise` writes to an unmapped address, which raises a genuine page fault.
That proves **the wiring**: handler, report, screen, end to end. A page fault is
used because it is the one severe fault that can be raised deliberately without
endangering the machine — a double fault is raised by destroying the stack, and a
machine check cannot be asked for at all.

The two are kept apart because they answer different questions and because
confusing them would let a broken handler pass a test of the drawing.

## 2. Verification of the optimisation, the disposition and the fault screens

### 2.1 The word path

A fast path is the most dangerous kind of code to leave unasserted: it runs only
when its own precondition holds, so a fault in it is invisible upon every surface
that does not meet the condition — and the surface the self-tests use and the
surface a person looks at are not the same surface.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| A four-byte surface on a word boundary with a word-multiple pitch **is** marked word-addressable | The fast path never runs, and the measurement above was of nothing. |
| A surface whose base and pitch are both odd is **not** | Every row would be written misaligned. |
| A three-byte pixel is **not**, whatever its alignment | A write would spill into the pixel beside it. |
| **The word path and the byte path draw identical pixels** | One of them is wrong and the tests see only the other. Two surfaces differing in nothing but alignment are drawn upon and compared pixel for pixel. |
| A pattern block reproduces its own bits, most significant leftmost | The bit order or the row order is wrong; every character would be mirrored. |
| A pattern block writes the **paper** as well as the ink | A console cell would keep the character drawn there before it. |
| A clipped block draws the bits that survive, not the bits from the start of the pattern | It was shifted to the clip rather than trimmed by it. |
| A block outside the clip, of no rows, or with no pattern writes nothing | The cheapest rejections are broken. |

### 2.2 The two glyph routines agree

For **every glyph in the face**, `FontDrawGlyph` and `FontDrawGlyphOpaque` must
light the same ink pixels. The console changed from one to the other, so no other
test here uses the path every character of the boot log actually goes through; a
difference between them would be a difference nothing else could see. The whole
face is checked rather than a sample, the fault being of exactly the kind that
afflicts one character and no other.

### 2.3 The disposition

The disposition is asserted first, because the screens depend upon it and because
it cannot be exercised any other way: half of it concerns faults raised at
privilege level 3, and there is no code outside the kernel to raise one until
sub-task 6.10. `ExceptionDispositionOf` is a pure function of a vector and a code
segment selector, so it can be asked the question for a privilege level that does
not yet exist.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| A privilege level 3 selector is recognised as outside the kernel, and a privilege level 0 one is not | Every kernel fault would be blamed upon a program, or every program's fault upon the kernel. |
| `#BP` and `#OF` resume | A trap would halt the machine, and `INT3` would cease to be usable as a marker. |
| NMI, `#DF`, `#MC`, `#TS`, `#NP` are fatal at **both** privilege levels | An abort or a corrupt descriptor table would be treated as one program's problem, leaving the machine running on a structure known to be wrong. |
| `#DE`, `#BR`, `#UD`, `#SS`, `#GP`, `#PF` and `#AC` **terminate** the program at privilege level 3 | **This is the assertion this section exists for.** Its failure is the machine halting for a mistake that should have cost one program — the fault this kernel actually had. |
| The same seven are **fatal** at privilege level 0 | A fault the kernel raised within itself would be blamed upon a program that does not exist. |
| No vector is treated more leniently within the kernel than outside it | The kernel would survive something a program would not, which is backwards. |
| Every vector has one of the three dispositions | A vector falls through the classification entirely. |
| `#AC` is recognised as raisable only outside the kernel, and `#PF` is not | The rule below would forbid a screen that is needed, or permit one that can never be drawn. |

### 2.4 The fault screen table

This asserts the table and not the drawing, for the reason Section 8.1 gives
about the display generally: whether a page reads well is not something a kernel
can determine about itself. What is asserted is everything a person reading one
screen would not notice.

| Assertion | What its failure would mean |
| --------- | --------------------------- |
| Every entry has a title, an account and a direction | A screen draws a blank space where the one thing the page was for should be. |
| **No two entries share a title** | A copied row with the vector changed and the identity not. The reader cannot tell the faults apart, which is the whole purpose of having more than one screen. |
| **No two entries share a colour** | The same, at a glance rather than on reading. |
| No two entries claim the same vector | One of them is unreachable. |
| Every vector that is fatal **whatever raised it** has an entry of its own | A deleted entry falls back to the general screen, which still names the vector and so does not look broken — it is merely less useful than it was, silently. |
| **No screen exists for a fault that is never fatal** | A page nobody could ever see, and a claim that the kernel treats as the end of the machine something that costs one program. |
| **No screen exists for a fault the processor raises only outside the kernel** | The same, argued from the architecture rather than from the disposition: `#AC` needs all of privilege level 3, `CR0.AM` and `RFLAGS.AC`, so it can never be the kernel's. |
| Every character of every sentence is one the font can draw | Text that renders as replacement boxes. This caught a real fault: an em dash reached a string literal, which in UTF-8 is three bytes none of which the face covers, and the sentence rendered with three boxes in the middle of it — visible only to somebody looking at the page, at the worst possible moment. |
| There is an entry for a panic the kernel raises itself | It would be shown a screen written for a processor exception that did not occur. |
| Every title fits a **640-pixel** display at the scale used there | The title runs off the edge upon VirtualBox and not upon QEMU, so whichever machine the person judging the screens did not use is where it is broken. |
| The evidence flags name only panels that exist | A screen carries no evidence and looks exactly like one meant to carry none. |
| No screen has been drawn when the self-test runs | A real fault later in the boot would find the display taken and draw nothing — invisible precisely when it matters. |
| An index past the end yields nothing | The table is read past its own end. |

Nothing here draws, and that is deliberate: drawing would set the flag recording
a screen as shown, and a real fault later in the same boot would then be refused
the display by the test meant to protect it.

### 2.5 The negative tests

**The word path.** The alignment conditions were removed from `whole_words`, so
that every four-byte surface claimed the fast path. The run reported `a surface
whose base and pitch are both odd was marked as addressable by words, so every
row would be written misaligned` and ended `Graphics self-test FAILED.`

**The pattern block.** It was made to skip its clear bits, as the transparent
glyph does. Three assertions fired in the graphics self-test and a fourth in the
console self-test, the last naming the code point at which the two glyph routines
first disagreed — `the two glyph routines disagree, at code 0x20`.

**A duplicated screen.** Two entries were given the same title and colour, as a
copied row would be. The run reported `two fault screens share a title, at vectors
0xA and 0xB` and `two fault screens share a colour, at vectors 0xA and 0xB`.

**A deleted screen.** The machine-check entry was removed. The run reported `a
fault that threatens the kernel has no screen of its own, at vector 0x12` — the
fault this catches being precisely the one that would otherwise look like nothing
at all.

**Every fault made fatal**, which is what this kernel did before the disposition
existed. The privilege-level test was removed from `ExceptionDispositionOf`. The
run named all seven vectors in turn — `a program's own fault would halt the
machine rather than the program, at vector 0x0`, and the same for `0x5`, `0x6`,
`0xC`, `0xD`, `0xE` and `0x11`. Vector 0 is the divide by zero, which is the
example the fault was reported with.

**A screen for a fault that can never be the kernel's.** The alignment check was
given an entry again. The run reported `a screen exists for a fault the processor
raises only outside the kernel, at vector 0x11`. Worth recording that the weaker
form of this rule — asking merely whether the vector is ever fatal — did **not**
catch it, `#AC` being nominally fatal from a kernel selector; the architectural
fact had to be stated before the assertion had any force.

**An undrawable character.** An em dash was put back into one screen's text. The
run reported `a fault screen's text holds a character the font cannot draw, at
vector 0x8, code 0xE2` — the first byte of its UTF-8 encoding.

Every edit was reverted.

---


## 3. Limitations

These are stated within the sections above and are gathered here because
[`../README.md`](../README.md) requires every document to end with what has not
been done. Nothing new is claimed; each item cites the section it comes from.

1. **Ten screens, and the rest of the vectors share none.** A vector with no
   screen of its own is one the kernel does not consider fatal to itself, and it
   is ended as a program's fault instead. Section 1.2 gives the rule and Section
   2.4 the two assertions that hold the table to it.
2. **Most severe faults cannot be demonstrated.** Section 1.5: a page fault is
   the one that can be raised deliberately without endangering the machine, so
   `fault-raise` raises that and nothing else. A double fault is raised by
   destroying the stack and a machine check cannot be asked for at all, which is
   why the other screens are shown from a composed frame — filled with values no
   machine would produce, so that a photograph of one cannot be mistaken for a
   real report.
3. **The screen takes the display and does not give it back.** Section 1.4: the
   console is suspended and the fault screen owns the framebuffer from that point
   onward. That is correct for a machine that has stopped and is not a mechanism
   for anything that has to carry on afterwards.
4. **The picture a person judges is judged by a person.** Sections 2.4 and 2.5
   assert the table, the titles, the colours and the drawability of every
   character; that the resulting page is *legible* is established by looking at
   it, and the captures are recorded in
   [`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md), Section 5.
5. **Two processors faulting at once would interleave two screens.** No lock is
   taken here, and none ever will be: the spinlock of sub-task 6.13 exists, and a
   fault handler that waited upon a lock held by the processor that faulted would
   replace a reported fault with a stopped machine. Two interleaved screens is
   the lesser failure and is accepted. This is the same limitation the diagnostic
   path carries and is recorded with it in
   [`CONCURRENCY.md`](CONCURRENCY.md), Section 10, limitation 1.
