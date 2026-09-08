# `docs/project/` — How the Work Is Conducted

These documents govern the work rather than describe the system. Nothing here
explains how the kernel functions; that is [`../design/`](../design/),
[`../devices/`](../devices/) and [`../storage/`](../storage/).

| Document | Subject |
| -------- | ------- |
| [`PLAN.md`](PLAN.md) | The thirteen-phase roadmap and the sub-task tracker: what is being built, what state each sub-task is in, and what comes next. Every other document opens by citing the phase and sub-task it belongs to, and this is where those are defined. It is the single source of truth for what is done. |
| [`STATUS.md`](STATUS.md) | The present condition of the system: what it does today, one paragraph to a phase, and the table of which environments each phase has actually been observed to work in. |
| [`HISTORY.md`](HISTORY.md) | The revision history: one row per change, pointing at the commit that made it and the design document that reasons about it. |
| [`TESTING.md`](TESTING.md) | How the kernel is verified — the `make verify` procedure, the interactive and machine-specific runs, and the dated record of every test performed with its outcome. |
| [`TOOLCHAIN.md`](TOOLCHAIN.md) | The `x86_64-elf` cross-toolchain, its construction, and the build system that uses it. |
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

## The order of work

For each sub-task: implement, verify, then document, then commit. The
documentation of a subsystem records what was observed and what was learned in
building it — the values the self-test reported, the defects found on the way —
and none of that can be written before the work is done. `PROJECT_GUIDELINES.md`,
Section 7, requires it to be in the same change; it does not permit it to precede
the change.
