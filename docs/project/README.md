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
| [`VERSIONING.md`](VERSIONING.md) | What a release gets called and why: the ordinal, the optional point beneath it, the name that stands for an ordinal, and the edition that is a variety of a release rather than a successor to it — together with the record of releases, which is empty. |
| [`TESTING.md`](TESTING.md) | How the kernel is verified and where: what `make verify` asserts, interactive execution under QEMU, the three further environments — OVMF, VirtualBox and the physical machine — debugging with GDB, and the judges this project did not write. |
| [`TESTING-SYSTEM.md`](TESTING-SYSTEM.md) | What the test of each non-graphical subsystem establishes: the devices, the storage stack, the privilege apparatus and the concurrency primitives, each with its negative test. |
| [`TESTING-GRAPHICS.md`](TESTING-GRAPHICS.md) | The same for sub-tasks 6.2 to 6.6, kept apart because most of what matters there cannot be asserted by the kernel at all and has to be looked at. |
| [`TESTING-RECORD.md`](TESTING-RECORD.md) | The dated record of every test performed, with its outcome. |
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

## The order of work

For each sub-task: implement, verify, then document, then commit. The
documentation of a subsystem records what was observed and what was learned in
building it — the values the self-test reported, the defects found on the way —
and none of that can be written before the work is done. `PROJECT_GUIDELINES.md`,
Section 7, requires it to be in the same change; it does not permit it to precede
the change.
