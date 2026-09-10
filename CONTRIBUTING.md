<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Contributing to Oxys-OS

**Authority**: [`PROJECT_GUIDELINES.md`](PROJECT_GUIDELINES.md) is binding upon
every contributor, human or automated, and this document does not restate it.
What follows is the working procedure: the order things are done in, what a
change must carry before it is complete, and the standards a document or a
commit is held to.

Oxys-OS presently has one contributor. This document is written anyway, for two
reasons: the conventions here are specific enough that they cannot be inferred
from the code, and a project that writes them down only when a second person
arrives writes them down from memory.

## 1. Before anything

```sh
export PATH="$HOME/opt/cross/bin:$PATH"   # the cross-compiler is not on the default path
make toolcheck
```

`PROJECT_GUIDELINES.md`, Section 9, requires this at the start of each session:
the working directory is `~/oxys-os`, the cross-compiler is reachable,
[`docs/project/PLAN.md`](docs/project/PLAN.md) reflects the current state, and
the tree builds.

## 2. The order of work

**Implement, verify, then document, then commit** — in that order, and the order
is not arbitrary. The documentation of a subsystem records what was observed and
what was learned in building it: the values the self-test reported, the defects
found on the way, the assertion that failed first and why. None of that can be
written before the work is done. `PROJECT_GUIDELINES.md`, Section 7, requires the
documentation to be in the same change; it does not permit it to precede the
change.

## 3. What a change must carry

A change is not complete until all of these are true. This is
`PROJECT_GUIDELINES.md`, Section 2, made into a list.

| | |
| --- | --- |
| The file header block of every file touched describes what the file now is | Section 4 fixes its form: path, purpose, key definitions, specifications |
| The design document for the subject is updated | `docs/design/`, `docs/devices/` or `docs/storage/`, as [`docs/README.md`](docs/README.md) sets out |
| The owning directory's `README.md` is updated if its contents changed | Section 10 |
| [`docs/project/PLAN.md`](docs/project/PLAN.md) records the new state of the sub-task | It is the single source of truth for progress |
| [`docs/project/STATUS.md`](docs/project/STATUS.md) is updated if what the system *does* changed | — |
| [`docs/project/HISTORY.md`](docs/project/HISTORY.md) has a row for the change | About sixty words, pointing at the commit and the design document |
| A boot-time self-test asserts the new behaviour, or the absence of one is recorded | Section 6 |
| `make verify` passes | Section 7 |

**A stale document is a defect of the same kind as a bug.** That is the whole of
the reasoning: the documentation is part of the codebase and not a description of
it, so a document that no longer describes its subject is wrong in the way code
is wrong.

## 4. Before implementing a subsystem

`PROJECT_GUIDELINES.md`, Section 2, requires the authoritative specification to
be **retrieved and cited** before the code is written — not recalled. Register it
in [`docs/project/REFERENCES.md`](docs/project/REFERENCES.md) with the sections
relied upon named, and cite it where the behaviour it governs is implemented.

Where a specification is not publicly distributed, say so, and record that its
details were taken from two independent renderings and cross-verified. Two
transcriptions disagreeing is a fact worth having; one transcription being wrong
is not discoverable at all.

## 5. Testing

There is no test harness and there will be none before Phase 7, there being no
userland to run one in. The kernel therefore asserts its own properties at boot,
in the order the subsystems are initialised.

```sh
make verify        # boots the ISO headless under QEMU; fails if any self-test does
make clang-check   # compiles every unit with a second compiler, for the diagnostics
```

Three things are expected of a new self-test, and the first is the one that
matters:

**Every assertion is paired with the silent failure it exists to catch**, in the
subject's design document. This is the project's central practice and not a
documentation chore. A kernel fails silently by construction — a driver that
reads the wrong sector returns data, and data that arrived cannot be told from
data that is correct — so writing down the failure first is what produces a test
worth having. An assertion that merely confirms the code does what the code does
establishes nothing.

**Assert a relationship, not a success.** That the second block of a two-block
read is the block that follows; that a block already held is not read again; that
a dirty buffer evicted under pressure reached its device. "The operation
returned true" is the assertion that passes when everything is broken.

**Apply the damage and confirm the test catches it.** Break the thing on purpose,
run it, record the message the run produced, revert. A test never observed to
fail has not been observed to do anything. Record these in the design document's
negative-test table.

Where a machine cannot be made to produce a condition, make the decision a pure
function and compose the condition — as `ExceptionDispositionOf` is asked about a
privilege level that did not yet exist, and as the disk driver's classification
is asserted upon configuration headers composed in memory.

### 5.1 Judges this project did not write

Everything above is machinery this project wrote, asserting against fixtures this
project composed, and its characteristic failure is agreement.
[`docs/project/TESTING.md`](docs/project/TESTING.md), Section 7, sets out the
argument and lists the independent judges available. Use them: `e2fsck` found a
defect every self-test had passed, and the second compiler found another on its
first run.

## 6. Style

**Prose.** Formal, technical, objective. No emoji, no slang, no informality — in
documents, comments and commit messages alike.

This register is directed at work and not at people, and
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md), Section 2, says where the line
between the two falls. Review here is blunt by design: that document exists in
part to keep it so, a code of conduct read as forbidding plain technical
judgement being a way of putting objections out of reach.

The distinctive convention is worth stating because it cannot be guessed:
**explain a decision by the failure it prevents, not by what it does.** "The line
is clipped per pixel, because clipping at the endpoints moves the line by a pixel
and that shows as a kink where two clipped regions meet along a seam" is the
form. Read one existing design document before writing one;
[`docs/design/PROCESS.md`](docs/design/PROCESS.md) is representative.

A resolved limitation is struck through and annotated with the sub-task that
resolved it, never deleted. A fault found is recorded — including one found in
the change that introduced the assertion which caught it, that being the evidence
the assertion was worth writing.

**Code.** `PROJECT_GUIDELINES.md`, Section 4, and
[`docs/project/CODING-STANDARDS.md`](docs/project/CODING-STANDARDS.md). Types and
global functions `PascalCase`, macros `UPPER_SNAKE_CASE`, locals `snake_case`.
The diagnostic regime is `-Wall -Wextra -Werror` plus seven further warnings and
**no suppressions**; a compiler extension must be registered with its rationale,
and a structure defined by a format outside this project is decoded field by
field rather than overlaid.

**Commits.** Long-form formal prose saying what changed and why — not a bullet
list. Commit to `main`; this project does not branch. Then a second commit,
`Record the commit hash of <thing>`, filling in the `Commit` column of the
`HISTORY.md` row, which cannot be written until the first commit exists.

An amendment to `PROJECT_GUIDELINES.md` requires the project owner's explicit
decision, and the amendment and its reason must be acknowledged in the commit
that makes it. Section 7 of that document.

## 7. Licensing of contributions

[`LICENSING.md`](LICENSING.md) records which licence applies where: the kernel is
`LGPL-3.0-or-later`, the userland `MIT`, the documentation `CC0-1.0`.

By contributing you agree that your contribution is licensed under the licence
applying to the path it touches, and you affirm that you wrote it. **Transcribing
a reference implementation is prohibited** by `PROJECT_GUIDELINES.md`, Section 2,
and it is also what would make the licences above impossible to grant: a
repository carrying vendored code cannot license itself freely. Studying another
system for understanding is expressly permitted; copying from it is not.

No formal contributor agreement or sign-off is in force, there being one
contributor. Should that change, the ordinary mechanism is the Developer
Certificate of Origin with a `Signed-off-by` line, and adopting it is the project
owner's decision.

## 8. Where to look first

| You want to know | Read |
| --- | --- |
| Where the work stands and what is next | [`docs/project/PLAN.md`](docs/project/PLAN.md) |
| What the system does today | [`docs/project/STATUS.md`](docs/project/STATUS.md) |
| Why a subsystem is built as it is | its document in [`docs/`](docs/) |
| Why the phases are in this order | [`docs/design/ARCHITECTURE.md`](docs/design/ARCHITECTURE.md), Section 4 |
| How anything is tested | [`docs/project/TESTING.md`](docs/project/TESTING.md) and the three documents it heads |
| The rules that bind all of it | [`PROJECT_GUIDELINES.md`](PROJECT_GUIDELINES.md) |
| What is expected of conduct, as against of work | [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) |
| What the system defends, what it does not, and how to report a defect in either | [`SECURITY.md`](SECURITY.md) |
