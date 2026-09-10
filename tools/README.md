<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `tools/` — Checks That Run Instead of Being Remembered

**Phase**: none. These belong to no phase; they exist because
`PROJECT_GUIDELINES.md`, Section 2, imposes a discipline that was enforced by
memory until they were written.
**Licence**: CC0-1.0, as documentation of how the work is checked rather than
part of it — [`../LICENSING.md`](../LICENSING.md), Section 1.

## Purpose

Every check here was written after a real defect that a person had missed. None
of them is a style rule, and none enforces a preference.

| Script | What it checks | The defect that motivated it |
| ------ | -------------- | ---------------------------- |
| [`spdx.sh`](spdx.sh) | Every tracked file carries the SPDX licence tag that `LICENSING.md`, Section 1, assigns to its path. | `crypto/`, `net/` and `uefi/` stood with no licence at all from the day `LICENSING.md` was written. A table is authoritative and is not machine-readable, so the mapping held only for as long as somebody remembered it. |
| [`check-docs.sh`](check-docs.sh) | Seven claims the corpus makes about itself and about the source. | `docs/design/SMP.md` was referenced by three source files before it existed; `CONCURRENCY.md` named five files as carrying a note that five of them did not carry; `STATUS.md` reported the wrong count of self-test assertions through two sub-tasks. |

## Running them

```sh
make lint          # both, and what CI runs
make spdx-check    # tags only, changes nothing
make spdx-apply    # add the tag to files that lack one
make docs-check    # the corpus only
```

Each exits non-zero on failure, so a check reads like a compiler diagnostic
rather than like a report somebody has to interpret.

Neither script builds anything or needs the cross-toolchain. They read the
repository. That is why they are a CI job of their own: a corpus that has
drifted should not be reported as a compiler failure.

## What `check-docs.sh` checks

1. **Links.** Every relative link from a Markdown file to a `.md`, `.c`, `.h`,
   `.asm`, `.ld`, `.txt`, `.sh`, `.cfg` or `.yml` file resolves.
2. **Indexes.** Every document in `docs/design/` appears in both
   `docs/design/README.md` and `docs/README.md`, and the count in the prose —
   "These sixteen documents" — is the number of them.
3. **File headers.** Every `.c`, `.h` and `.asm` file carries a `File:` line, and
   that line names the file it is in. This catches a file copied to start another
   and left carrying the original's path.
4. **Directory READMEs.** Every directory holding tracked material has one, as
   Section 10 of the guidelines requires.
5. **The unsynchronised-structure list.** Every file named in `CONCURRENCY.md`,
   Section 10, limitation 1, exists and carries a `Concurrency.` paragraph. That
   list states this about itself, and the statement was false when first made.
6. **The assertion count.** `STATUS.md`'s count of self-tests reporting `passed`
   or `sound` is the number in `build/serial.log`. Skipped where no log exists,
   because the log is the evidence and an absent one is not a failure.
7. **Stale forward references** — an advisory, not an error. A sub-task that
   `PLAN.md` marks `Implemented`, still written about in the future tense.

## Errors and advisories

An **error** is a statement that is provably false: a link to a file that is not
there, a count that does not match what was counted. It fails the run.

An **advisory** is a statement that is *probably* stale. It is printed and does
not fail the run, because some of them are correctly phrased and a checker that
cries wolf is a checker nobody reads.

**The distinction was drawn by measurement, not by taste.** The advisory check
originally also matched "until sub-task N", which reads as future tense. It
produced thirty-six hits, of which none was a defect — "there was one stack until
sub-task 6.9" is correct English about the past. That pattern was dropped. The
two that remain, "sub-task N will" and "sub-task N is what makes", produced six
hits of which all six were genuinely stale.

## What these scripts do not do

They check what can be checked. They cannot check whether a document is *true* —
whether a `Concurrency.` paragraph describes the locking that file actually
performs, whether a README describes the directory it sits in, whether a design
document's reasoning still holds. Those remain the reader's, and
`PROJECT_GUIDELINES.md`, Section 2, remains binding whether or not a script can
enforce it.

Passing `make lint` is therefore necessary and not sufficient, and treating it as
sufficient would be a worse failure than not having written it.

## Constraints upon changes here

1. **`spdx.sh` is `LICENSING.md`, Section 1, expressed as code.** The two must
   change together. `make spdx-check` is what makes a disagreement visible, and
   it can only do that while the script is the table and not an approximation of
   it.
2. **The order of the rules in `identifier_for` is load-bearing.** The first
   match wins, so the specific exceptions must be tested before the directory
   rules: `boot/grub/grub.cfg` lives under `boot/` and is CC0, not LGPL.
   Reordering them would relicense files without anything saying so.
3. **A mismatched tag is never rewritten automatically.** A file whose tag
   disagrees with the table is either a file in the wrong place or a table that
   is wrong, and neither is a thing a script should decide.
4. **A ported third-party tool is exempt** and goes in `tools/spdx-exceptions`,
   one glob to a line, rather than in the script. `PROJECT_GUIDELINES.md`,
   Section 2, requires a port to keep the licence it arrived under; adding one
   should not mean editing a program.
5. **A new check must name the defect it would have caught.** That is the bar the
   existing ones meet, and it is what keeps this directory from accumulating
   opinions.
