<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Inspirations of Oxys-OS

**Corresponding phase**: All phases. This document is revised whenever an
inspiration bears upon a decision recorded elsewhere, and whenever a phase the
inspiration concerns is reached.

**Authority**: `PROJECT_GUIDELINES.md`, Section 2. This document exists so that
the projects named in it are recorded as what they are — sources of design and
of character — and are never mistaken for sources of code. Section 2 states the
rule that governs every one of them:

> **Original Kernel and Userland**: All source code under `kernel/`, `boot/`,
> `drivers/`, `graphics/`, `libc/`, `net/`, `crypto/`, `uefi/` and `userland/`
> must be original. Reference implementations may be studied for understanding
> but must not be transcribed. The only permitted inclusions there are standard
> public domain headers or minimal stub code explicitly required by the
> toolchain (e.g., linker scripts).

That rule is not weakened by anything written here, and it is not weakened by
the permission that stands beside it either. Section 2 also allows a
third-party **tool** to be ported and depended upon — that is how Oxys-OS
becomes self-hosting — but a port is a whole program brought across intact,
under its own licence, in a directory of its own. It is the opposite of what
this document guards against, which is a line of somebody else's
implementation appearing inside this project's own, unattributed, because a
system was admired.

An inspiration is a reason for a decision, not a source for an implementation.
Where one of these systems is studied, what is taken from it is the
understanding of why it is arranged as it is; the arrangement is then decided
upon its merits for this project and written from nothing.

## 1. Why this document exists

The reasoning behind the technical decisions of this project is recorded in the
documents that describe the subsystems, and the reasoning behind the order of
the work is recorded in [`PLAN.md`](PLAN.md). Neither records where the taste
came from — what a person building this project pictured when they decided what
it ought to feel like to use. That is a real influence upon the work, most of
all upon the phases that have not been reached, and a project whose stated
practice is to write down its reasons should write this one down too.

The two systems below are named because the project owner names them, and are
listed in the order of their weight; each entry states plainly what is taken
and, where it matters, what is not. Section 3 stands between them and names no
system: the appearance this project intends is nobody else's, and it is recorded
here because it is a matter of taste, and taste is what this document holds.

## 2. ToaruOS — the principal inspiration

[ToaruOS](https://toaruos.org/) is a hobby operating system written from
scratch, with its own kernel, its own userland and its own graphical
environment, and it is the principal inspiration of this project. Two things are
taken from it.

**Its architecture.** ToaruOS demonstrates the shape this project is attempting:
a monolithic kernel written from nothing, carrying its own drivers, its own
filesystem support and its own userland, arriving at a complete graphical system
without adopting an existing kernel or an existing desktop beneath it. That the
shape is achievable by a small project, and that the whole of it may be
original, is the demonstration this project takes from it. The particular
decisions of this project's architecture are its own and are argued in
[`../design/ARCHITECTURE.md`](../design/ARCHITECTURE.md); what ToaruOS
contributes is the confidence that a system of that scope is a reasonable
objective rather than an unreasonable one.

**Its style of graphical environment.** A compositing window system serving
client processes through a protocol of its own, with a stacking window manager
above it, is the style of graphical environment Oxys-OS intends. The sub-tasks
of Phases 6 and 9 in [`PLAN.md`](PLAN.md) — the compositing surface abstraction
of sub-task 6.6, and the stacking window manager and client protocol of
sub-tasks 9.1 and 9.2 — describe that style, and it is the style this
inspiration concerns.

**What is not taken.** The theme of ToaruOS is not taken. Its visual identity —
its palette, its decorations, its iconography, its naming, its presentation — is
its own and belongs to it. Oxys-OS is to look like itself. Where the appearance
of this project is decided, it is decided in Phase 9 and recorded then; it is
not decided by reference to how ToaruOS looks. The distinction this section
draws is between the *structure* of a graphical environment, which is taken, and
the *appearance* of one, which is not.

## 3. The appearance intended

The systems named above and below are named for what they demonstrate and for
how they are conducted. This section names no system, because the appearance of
Oxys-OS is taken from none: it is a preference of the project owner's, stated
here so that Phase 9 is designed against something written down rather than
something recalled.

The direction is contemporary: **playful minimalism** — modernist, and playfully
geometric. What that commits the work to, stated as qualities rather than as
pixels:

- **Flat surfaces.** Depth is expressed, where it is expressed at all, by
  spacing, by scale and by the stacking order itself, not by simulated bevels,
  gradients or shadows imitating a physical control.
- **A modernist restraint.** Nothing is drawn that is not a control, a boundary
  or a label. Ornament is not added to fill space; space is left.
- **Geometry as the character.** The playfulness is carried by shape and
  proportion — circles, arcs, rounded rectangles, deliberate asymmetry — rather
  than by decoration applied on top of an otherwise plain form. The result
  should read as light without reading as unserious.
- **Colour used sparingly and deliberately.** A small palette, mostly quiet,
  with colour reserved for the few things that must be distinguished at a
  glance. A palette is a decision of Phase 9 and is not made here. **It was
  made on 2026-09-21**, at the project owner's direction: a yellow ground, a
  lighter and more orange yellow for the panel and for the band of the window
  holding the focus, a paler one for every other band, and a dark brown ink.
  [`../../art/palette.h`](../../art/palette.h) is the whole of it — one header
  the kernel and the session both read, so that a colour cannot be decided
  twice — and [`../design/SESSION.md`](../design/SESSION.md) is
  where it is drawn.
- **Legibility first.** Where a playful choice and a legible one disagree, the
  legible one is taken. A desktop that is pleasant to look at and hard to read
  has failed at the thing it exists to do.

**A retro-styled desktop is not wanted**, now or at any later point: not the
visual idiom of the nineteen-nineties desktops, not raised and recessed bevels,
and not a retro theme offered beside a contemporary one. The prohibition is
written down rather than left implied because that idiom is where a system
written from nothing drifts if nobody stops it. It is the cheapest appearance to
reach with the primitives Phase 6 already has — a rectangle fill, two lines of a
lighter colour and two of a darker, and a control looks raised — so it arrives
by default, and an appearance arrived at by default is indistinguishable,
afterwards, from one that was chosen. This section is what makes the difference
visible.

This remains a preference of taste and is stated as one, and the colours and the
mark it is now carried out in are the project owner's own —
[`../../art/README.md`](../../art/README.md). **Sub-task 9.1 is the first thing
built against it**, on 2026-09-17, and
[`../design/WINDOWS.md`](../design/WINDOWS.md) is where the window's
frame and the palette are judged against this section: a flat band, a one-pixel
border, one blue reserved for the focus, and a disc for the one control — the
geometry was reached, the bevel was not reached for, and rounded corners and any
asymmetry wait for the desktop of 9.5, which decides what a corner meets. Every
later part of Phase 9 is recorded likewise in its own document under
[`../design/`](../design/), and this section is judged against each.

## 4. BSD — the second inspiration

The BSD family — the system that descends from the Berkeley Software
Distribution, and its present derivatives — is the second inspiration, and the
one that bears least upon appearance and most upon conduct.

What is admired there is the coherence of a system developed as a whole: kernel,
userland and documentation maintained together, as one thing with one manner,
rather than assembled from parts that were each designed in ignorance of the
others. Several practices this project already follows are of that character,
and are the better for having a precedent:

- A complete system in one repository, its kernel and its userland versioned
  together, which is how this project is arranged and how [`PLAN.md`](PLAN.md)
  orders its phases.
- Documentation treated as part of the system rather than as commentary upon it.
  `PROJECT_GUIDELINES.md`, Section 7, requires the documentation affected by a
  change to be brought up to date within the same change, and
  [`README.md`](README.md) records why: a description written before the work is
  a description of what was intended.
- A preference for the plain and the explicit over the clever, which
  [`CODING-STANDARDS.md`](CODING-STANDARDS.md) states as a rule and Section 7.1
  of that document applies to the decoding of on-disk structures.

No BSD interface is adopted by name and no BSD source is consulted for the
purpose of reproduction. Where this project implements something a BSD also
implements, the specification is retrieved and cited as
`PROJECT_GUIDELINES.md`, Section 2, requires, and
[`REFERENCES.md`](REFERENCES.md) records it.

## 5. The distinction this document maintains

| Taken | Not taken |
| ----- | --------- |
| The demonstration that a wholly original system of this scope is achievable. | Any part of the source of any such system. |
| The style of graphical environment: compositing, a window manager, a client protocol. | The theme, palette, iconography or visual identity of ToaruOS. |
| Nothing. The appearance is this project's own, and Section 3 states the preference it is designed against. | The retro graphical idiom, and any asset, font or specific design of another system. |
| The practice of a coherent system maintained as one thing. | Any BSD interface adopted without its specification being retrieved and cited. |

The line is the same in every row. What may be taken from another project is a
reason; what may not be taken is a thing. A reason is understood, restated in
this project's terms, and recorded in the document that describes the decision
it produced. A thing is written here, from nothing, by this project.

## 6. Limitations of this document

This document states intentions, and most of the work they concern has not been
begun. The graphical inspirations concern sub-tasks 6.2 to 6.6, which are
built, and the whole of Phase 9, of which sub-task 9.1 alone is; nothing here
should be read as a description of behaviour that exists — that is
[`../design/WINDOWS.md`](../design/WINDOWS.md) for 9.1, and Section 3 above now
records how much of the intention survived contact with it. As each later part
is reached, what is actually built is recorded in its own document under
[`../design/`](../design/), and this document is revised likewise.

The characterisations of the two systems above are stated in general terms
deliberately. This project cites specifications for its assertions about
hardware; it makes no comparable claim of authority about the internals of
another project, and so it describes them only at the level at which the
influence upon this one is real.
