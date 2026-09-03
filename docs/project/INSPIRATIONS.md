# The Inspirations of Oxys-OS

**Corresponding phase**: All phases. This document is revised whenever an
inspiration bears upon a decision recorded elsewhere, and whenever a phase the
inspiration concerns is reached.

**Authority**: `PROJECT_GUIDELINES.md`, Section 2. This document exists so that
the projects named in it are recorded as what they are — sources of design and
of character — and are never mistaken for sources of code. Section 2 states the
rule that governs every one of them:

> **No External Code Copying**: All source code must be original. Reference
> implementations may be studied for understanding but must not be transcribed.
> The only permitted inclusions are standard public domain headers or minimal
> stub code explicitly required by the toolchain (e.g., linker scripts).

That rule is not weakened by anything written here. An inspiration is a reason
for a decision, not a source for an implementation. Where one of these systems
is studied, what is taken from it is the understanding of why it is arranged as
it is; the arrangement is then decided upon its merits for this project and
written from nothing.

## 1. Why this document exists

The reasoning behind the technical decisions of this project is recorded in the
documents that describe the subsystems, and the reasoning behind the order of
the work is recorded in [`PLAN.md`](PLAN.md). Neither records where the taste
came from — what a person building this project pictured when they decided what
it ought to feel like to use. That is a real influence upon the work, most of
all upon the phases that have not been reached, and a project whose stated
practice is to write down its reasons should write this one down too.

The three systems below are named because the project owner names them. They are
listed in the order of their weight, and each entry states plainly what is taken
and, where it matters, what is not.

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

## 3. SerenityOS — the retro graphical style

[SerenityOS](https://serenityos.org/) is taken as a smaller inspiration, and for
one thing: its retro graphical style. Its deliberate adoption of the visual
idiom of the graphical desktops of the nineteen-nineties — the raised and
recessed bevels, the solid colours, the plain rectilinear decorations, the
absence of ornament that is not also a control — is the direction Oxys-OS
prefers for its own appearance.

This is a preference of taste and it is stated as one. It is not a decision
already made about any particular pixel; Phase 9 has not been reached, and no
part of it has been designed. It is recorded here so that when Phase 9 is
designed the preference is already written down rather than recalled, and so
that the resulting appearance may be judged against a stated intention.

## 4. BSD — the third inspiration

The BSD family — the system that descends from the Berkeley Software
Distribution, and its present derivatives — is the third inspiration, and the
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
| The preference for a retro graphical idiom. | Any asset, font or specific design of SerenityOS. |
| The practice of a coherent system maintained as one thing. | Any BSD interface adopted without its specification being retrieved and cited. |

The line is the same in every row. What may be taken from another project is a
reason; what may not be taken is a thing. A reason is understood, restated in
this project's terms, and recorded in the document that describes the decision
it produced. A thing is written here, from nothing, by this project.

## 6. Limitations of this document

This document states intentions concerning work that has not been begun. The
graphical inspirations concern sub-tasks 6.2 to 6.6 and the whole of Phase 9,
and none of those is implemented; nothing here should be read as a description
of behaviour that exists. When they are reached, what is actually built is
recorded in its own document under [`../design/`](../design/), and this document
is revised to record how much of the intention survived contact with the work.

The characterisations of the three systems above are stated in general terms
deliberately. This project cites specifications for its assertions about
hardware; it makes no comparable claim of authority about the internals of
another project, and so it describes them only at the level at which the
influence upon this one is real.
