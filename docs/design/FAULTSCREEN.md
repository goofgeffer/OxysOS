<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Fault Screens

**Phase**: sub-task 6.4 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../graphics/faultscreen.c`](../../graphics/faultscreen.c),
[`../../kernel/include/oxys/gfx/faultscreen.h`](../../kernel/include/oxys/gfx/faultscreen.h);
the disposition in `ExceptionDispositionOf`,
[`../../kernel/arch/x86_64/interrupt/exceptions.c`](../../kernel/arch/x86_64/interrupt/exceptions.c).
**Specifications**: Intel SDM, Volume 3A, Chapter 6 and Table 6-1; Sections 6.13
and 6.15; Figures 6-6 and 6-9 (the two error-code forms).

The full-screen page drawn when the kernel stops. Every fatal fault is also
reported on the serial port and the text display, but in a graphics mode the text
display is hidden and many machines (VirtualBox among them) have no serial line;
without this page a person would see the boot log stop and nothing else.

## 1. When a screen is drawn

**Only for a fault the kernel cannot survive.** `ExceptionDispositionOf` decides
([`INTERRUPTS.md`](INTERRUPTS.md)); a program's mistake ends the program and
draws nothing. A screen claiming the system stopped, shown for a fault that should
have ended one program, would be a false account given to the person least able
to check it. What reaches this file:

- an **abort** (double fault, machine check), from which the architecture allows no
  resumption;
- a **non-maskable interrupt**: hardware reporting a condition;
- a **malformed descriptor table** (`#TS`, `#NP`), which is the kernel's structure;
- **any fault raised inside the kernel**;
- a **panic**, raised by the kernel's own check.

The titles say `KERNEL PAGE FAULT`, not `PAGE FAULT`, because by then that is what
it is.

## 2. One screen per fault

Each fault asks a different question: a page fault names an address; a
general-protection fault names a selector or the instruction; an invalid opcode
asks what the bytes are; a double fault says **the earlier fault is the one to
find**, and its screen says so. One register dump for all would not tell the
reader which question to ask. So each screen has its own title and colour, a
sentence on what the processor reports, a sentence on what to examine first, and
the evidence relevant to that fault:

| Vector | Screen | Evidence |
| --- | --- | --- |
| 2 NMI | Non-maskable interrupt | Control registers |
| 6 `#UD` | Bad instruction in kernel | Instruction bytes, stack |
| 8 `#DF` | Double fault | Stack, control registers |
| 10 `#TS` | Malformed task state segment | Selector |
| 11 `#NP` | Descriptor not present | Selector |
| 12 `#SS` | Kernel stack fault | Selector, stack |
| 13 `#GP` | Kernel protection fault | Selector, instruction bytes |
| 14 `#PF` | Kernel page fault | Faulting address and cause, control registers |
| 18 `#MC` | Machine check | Control registers |
| — | Kernel panic | The message naming the failed check |

Any other fatal vector (a divide by zero inside the kernel, say) gets the general
`UNEXPECTED KERNEL FAULT` screen, naming the exception and carrying the general
evidence: for a kernel bug, the instruction, the stack and the registers are what
matter.

**`#AC` has no screen**: an alignment check needs privilege level 3, `CR0.AM` and
`RFLAGS.AC` together (SDM 6.15), so the kernel cannot raise one.

## 3. Drawing inside a failed machine

A fault while drawing is a double fault; another while handling that resets the
machine with nothing written. So the screen:

- **allocates nothing** (the heap may be corrupt);
- **reads memory only after `PagingTranslate` says it is mapped**; an unmapped
  stack word is reported as absent, which is often the diagnosis itself;
- **draws once**: a second call means a fault while drawing the first, which must
  not be overwritten;
- **draws nothing without a framebuffer**, where the text display already shows the
  report;
- **suspends the console** (`ConsoleSuspend`) first. `KernelPanic` writes to the
  diagnostic path, which includes the console on the same framebuffer; with its
  cursor at the bottom, each newline would scroll the fault screen up. The text
  display and serial port still receive everything. There is no resumption.

No lock is taken: a handler waiting on a lock held by the processor that faulted
would turn a reported fault into a silent hang.

## Verification

`KernelVerifyFaultScreen` in [`../../kernel/test/gfx/faultscreen.c`](../../kernel/test/gfx/faultscreen.c),
with the fast drawing paths in `KernelVerifyGraphics` and `KernelVerifyConsole`.
Nothing is drawn: drawing would mark a screen as shown and refuse the display to a
real fault later in the boot.

**The disposition**, a pure function of vector and code selector, so the
privilege-3 half can be asked directly:

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A privilege-3 selector is outside the kernel; a privilege-0 one is not. | Every fault blamed on the wrong side. |
| `#BP` and `#OF` resume. | `INT3` halting the machine. |
| NMI, `#DF`, `#MC`, `#TS`, `#NP` are fatal at both levels. | An abort or corrupt table treated as one program's problem. |
| `#DE`, `#BR`, `#UD`, `#SS`, `#GP`, `#PF`, `#AC` **end the program** at level 3 and are **fatal** at level 0. | The machine halted for a program's mistake; a kernel fault blamed on a program that does not exist. |
| No vector is more lenient inside the kernel than outside; every vector has a disposition. | A backwards rule; a vector unclassified. |
| `#AC` is raisable only outside the kernel, `#PF` not. | The screen rules below misapplied. |

**The table** (whether a page reads well is judged by eye,
[`../project/TESTING-GRAPHICS.md`](../project/TESTING-GRAPHICS.md)):

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| Every entry has a title, an account and a direction. | A blank where the point of the page should be. |
| No two entries share a title, a colour, or a vector. | A copied row; indistinguishable faults; an unreachable entry. |
| Every always-fatal vector has its own entry. | A deleted entry silently falling back to the general screen. |
| No entry exists for a never-fatal vector, or one raised only outside the kernel. | A screen nobody could see, implying a program's fault stops the machine. |
| Every character is one the font draws. | Replacement boxes in the text, e.g. from a UTF-8 dash. |
| A panic has its own entry. | A panic shown a processor exception's screen. |
| Every title fits a 640-pixel display at the scale used there. | Titles off the edge on VirtualBox only. |
| Evidence flags name only panels that exist; an index past the end yields nothing; no screen has been drawn yet. | A screen with no evidence; reading past the table; a display already taken. |

**The fast paths** ([`CONSOLE.md`](CONSOLE.md)):

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| An aligned four-byte surface is word-addressable; an odd base and pitch, or three-byte pixels, is not. | A fast path that never runs; misaligned rows; spilled pixels. |
| **Word and byte paths draw identical pixels** on surfaces differing only in alignment. | One path wrong, invisible to tests using the other. |
| A pattern block reproduces its bits MSB-first, writes paper as well as ink, and trims (not shifts) to a clip; an empty or outside block writes nothing. | Mirrored text; stale cells; shifted glyphs. |
| For **every glyph**, `FontDrawGlyph` and `FontDrawGlyphOpaque` light the same ink. | A character wrong only on the path the console uses. |

## Limitations

1. Ten screens; other fatal vectors share the general one.
2. A screen can be seen only by causing its fault; the double fault, machine check
   and malformed TSS cannot be caused safely, so their screens have been seen only
   when they were written.
3. The screen keeps the display; it is for a machine that has stopped.
4. Two processors faulting at once may interleave two screens, accepted as the
   lesser failure ([`CONCURRENCY.md`](CONCURRENCY.md)).
