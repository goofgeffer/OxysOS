<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `docs/project/` — How the Work Is Conducted

These documents govern the work rather than describe the system. Nothing here
explains how the kernel functions; that is [`../design/`](../design/),
[`../devices/`](../devices/) and [`../storage/`](../storage/).

| Document | Subject |
| -------- | ------- |
| [`PLAN.md`](PLAN.md) | The thirteen-phase roadmap and the sub-task tracker: what is being built, what state each sub-task is in, and what comes next. Every other document opens by citing the phase and sub-task it belongs to, and this is where those are defined. It is the single source of truth for what is done. |
| [`STATUS.md`](STATUS.md) | The present condition of the system: what it does today, one paragraph to a phase, and the table of which environments each phase has actually been observed to work in. |
| [`HISTORY.md`](HISTORY.md) | The revision history: one row per change, pointing at the commit that made it and the design document that reasons about it. |
| [`VERSIONING.md`](VERSIONING.md) | What a release gets called and why: the ordinal, the optional point beneath it, the alpha and beta that precede the first ordinal alone, the name that stands for an ordinal, and the edition that is a variety of a release rather than a successor to it — together with the record of releases — one, `Oxys 1 Alpha`, since 2026-09-16 — and the plan of the three that are intended. |
| [`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md) | The release notes of `Oxys 1 Alpha`, cut on 2026-09-16 at the close of Phase 8: what kind of release it is and the judgement that decided it, what it does and what it does not have, where it was tested and where it was not, and the checksum of the image. One document per release, as `VERSIONING.md`, Section 9.4, requires; written once and not revised. |
| [`TESTING.md`](TESTING.md) | How the kernel is verified and where: what `make verify` asserts, interactive execution under QEMU, the four further environments — OVMF, VirtualBox, Bochs and the physical machine — debugging with GDB, and the judges this project did not write. |
| [`TESTING-SYSTEM.md`](TESTING-SYSTEM.md) | What the test of each non-graphical subsystem establishes: the devices, the storage stack, the privilege apparatus and the concurrency primitives, each with its negative test. |
| [`TESTING-GRAPHICS.md`](TESTING-GRAPHICS.md) | The same for sub-tasks 6.2 to 6.6, kept apart because most of what matters there cannot be asserted by the kernel at all and has to be looked at. |
| [`TESTING-RECORD.md`](TESTING-RECORD.md) | The dated record of every test performed, with its outcome. |
| [`BUILDS.md`](BUILDS.md) | The numbered register of every image produced: the commit it came from, the compiler that built it, its size, what the verification said and where it was run. It was emptied at the project owner's direction on 2026-09-13 and resumed at `Oxys 1 Alpha` on 2026-09-16 with build 1, the alpha's image; the document itself says what was removed and why the numbering restarted. Written by [`../../tools/builds.sh`](../../tools/builds.sh) from [`builds.tsv`](builds.tsv), which is the record; the document is a generated view of it and `make lint` fails if the two disagree. |
| [`TOOLCHAIN.md`](TOOLCHAIN.md) | The `x86_64-elf` cross-toolchain, its construction, the build system that uses it, and the GitHub workflow that runs the verification upon a machine nobody here has configured. |
| [`CODING-STANDARDS.md`](CODING-STANDARDS.md) | Style, naming, file headers, the `-Wall -Wextra -Werror` regime, and the register of compiler extensions relied upon with the justification of each. |
| [`REFERENCES.md`](REFERENCES.md) | Every specification the project relies upon, with the sections relied upon named and, where a specification is not publicly distributed, a note of how its details were cross-verified. |
| [`INSPIRATIONS.md`](INSPIRATIONS.md) | The systems this project takes its character from — ToaruOS principally, BSD besides — what is taken from each, what is not, the appearance Phase 9 is to be designed against, and the rule that separates an inspiration from a source of code. |

## The three that answer three questions

The first three documents above are kept apart because they answer three
different questions, and a document that answers two of them at once is scanned
for neither. Each question has one place:

- **Where are we, and what comes next?** — `PLAN.md`.
- **What does it do today, and where has it been seen to work?** — `STATUS.md`.
- **How did it come to be that way?** — `HISTORY.md`.

There is deliberately no separate roadmap document. `PLAN.md` *is* the roadmap,
and a second one would be a second place for the phase list to drift.

`VERSIONING.md` answers a fourth question that none of the three can — **what
does a released thing get called?** — and it is apart from them for the same
reason they are apart from each other. A commit and a release are published to
different audiences, and the release record is not a subset of `HISTORY.md`: most
commits are in no release, and a release is a decision rather than a change.

Its Section 11.1 also answers **when** — three releases, each fixed to a sub-task
of `PLAN.md` rather than to a date. That sits there and not in `PLAN.md` because
it is a statement about releases that happens to be indexed by sub-task, and
`PLAN.md` carries a marker at each of the three rather than a second copy of the
plan.

`BUILDS.md` answers a fifth, added at sub-task 7.2 — **which image was that?**
The three above are all about the source and none of them can name an artefact,
so an observation about one image and not another had nothing to attach itself
to. Its record, [`builds.tsv`](builds.tsv), is the one machine-readable file in
this directory, and the document itself is half prose and half generated from it.

The two are adjacent and answer different questions, which `VERSIONING.md`,
Section 5.5, is the boundary of: **a release is named there, and an image that is
not released is numbered here.** An internal debug build has a row in
`builds.tsv` and no ordinal, no tag and no release notes; it acquires all three
the moment it is handed to somebody, at which point it is a release and not a
build.

## The order of work

For each sub-task: implement, verify, then document, then commit. The
documentation of a subsystem records what was observed and what was learned in
building it — the values the self-test reported, the defects found on the way —
and none of that can be written before the work is done. `PROJECT_GUIDELINES.md`,
Section 7, requires it to be in the same change; it does not permit it to precede
the change.
