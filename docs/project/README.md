# `docs/project/` — How the Work Is Conducted

These documents govern the work rather than describe the system. Nothing here
explains how the kernel functions; that is [`../design/`](../design/),
[`../devices/`](../devices/) and [`../storage/`](../storage/).

| Document | Subject |
| -------- | ------- |
| [`PLAN.md`](PLAN.md) | The thirteen-phase roadmap, the sub-task tracker and the revision history. Every other document opens by citing the phase and sub-task it belongs to, and this is where those are defined. It is the single source of truth for what is done. |
| [`TESTING.md`](TESTING.md) | How the kernel is verified — the `make verify` procedure, the interactive and machine-specific runs, and the dated record of every test performed with its outcome. |
| [`TOOLCHAIN.md`](TOOLCHAIN.md) | The `x86_64-elf` cross-toolchain, its construction, and the build system that uses it. |
| [`CODING-STANDARDS.md`](CODING-STANDARDS.md) | Style, naming, file headers, the `-Wall -Wextra -Werror` regime, and the register of compiler extensions relied upon with the justification of each. |
| [`REFERENCES.md`](REFERENCES.md) | Every specification the project relies upon, with the sections relied upon named and, where a specification is not publicly distributed, a note of how its details were cross-verified. |

## The order of work

For each sub-task: implement, verify, then document, then commit. The
documentation of a subsystem records what was observed and what was learned in
building it — the values the self-test reported, the defects found on the way —
and none of that can be written before the work is done. `PROJECT_GUIDELINES.md`,
Section 7, requires it to be in the same change; it does not permit it to precede
the change.
