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
[`builds.sh`](builds.sh) is the one that is more than a check — it keeps a record
as well as validating it — and is described beneath the table.

| Script | What it checks | The defect that motivated it |
| ------ | -------------- | ---------------------------- |
| [`spdx.sh`](spdx.sh) | Every tracked file carries the SPDX licence tag that `LICENSING.md`, Section 1, assigns to its path. | `crypto/`, `net/` and `uefi/` stood with no licence at all from the day `LICENSING.md` was written. A table is authoritative and is not machine-readable, so the mapping held only for as long as somebody remembered it. |
| [`check-docs.sh`](check-docs.sh) | Nine claims the corpus makes about itself and about the source, the ninth being the build register of [`builds.sh`](builds.sh). | `docs/design/SMP.md` was referenced by three source files before it existed; `CONCURRENCY.md` named five files as carrying a note that five of them did not carry; `STATUS.md` reported the wrong count of self-test assertions through two sub-tasks; and `PROJECT_GUIDELINES.md`, Section 3, went several phases describing a set of build targets that was no longer the set the `Makefile` had. |

## `builds.sh` — the register, and the one script here that writes a record

[`builds.sh`](builds.sh) keeps the build register: `record` appends a build,
`query` selects from it, `render` regenerates the human view, `check` validates
all of it, and `sql` gives SQL where `sqlite3` happens to be installed.
`make build-record` invokes the first, and `make lint` the fourth by way of
`check-docs.sh`.

It is here rather than anywhere else because it belongs to no phase, as
everything in this directory does, and because it is the same kind of thing: a
discipline that was somebody's memory. The defect it answers is that **every
other record in this repository is about the source**. `PLAN.md` says what is
being built, `HISTORY.md` how it came to be, `TESTING-RECORD.md` what was run —
and none of them can name an *image*. By sub-task 7.2 this project had produced
some hundreds, of which not one could be referred to.

**The record is [`../docs/project/builds.tsv`](../docs/project/builds.tsv) and
the document is a view of it**, because a Markdown table is a view and cannot be
a record: it grows without bound in the file a person opens, every field in it is
free text so nothing can be counted or compared, and nothing validates it. The
record has twelve columns and three fixed vocabularies; the document carries the
twenty most recent builds and a summary, generated between markers; and `check`
fails `make lint` if the two disagree.
[`../docs/project/BUILDS.md`](../docs/project/BUILDS.md) sets out why the record
is tab-separated rather than JSON, and why SQLite is a lens over it rather than
the thing committed to git.

It builds nothing and runs nothing. It reads the artefacts that are already
there, which is what makes it safe to call after any target and after a boot a
person observed themselves in an environment that leaves no log.
`BUILD_DIR` selects where it reads from, as it does for the `Makefile`.

## Running them

```sh
make lint          # both checks, and what CI runs
make spdx-check    # tags only, changes nothing
make spdx-apply    # add the tag to files that lack one
make docs-check    # the corpus, the build register included
make build-record NOTE="…"   # not a check: one row in the register

tools/builds.sh query --compiler clang --result failed
tools/builds.sh sql "SELECT compiler, COUNT(*) FROM builds GROUP BY compiler"
```

Each of the checks exits non-zero on failure, so it reads like a compiler
diagnostic rather than like a report somebody has to interpret.

No script here builds anything or needs the cross-toolchain. They read the
repository. That is why the checks are a CI job of their own: a corpus that
has drifted should not be reported as a compiler failure. Nothing here needs
anything beyond bash and coreutils; `builds.sh sql` is the single exception, it
is the only subcommand that touches `sqlite3`, and it says so plainly when it is
absent rather than failing obscurely.

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
7. **Build targets.** The set of targets in the `Makefile`'s `.PHONY` line and
   the set named in `PROJECT_GUIDELINES.md`, Section 3, are the same set. It is
   checked both ways: a target the guidelines do not describe, and a target the
   guidelines tell you to run that does not exist. The second is the worse of
   the two.
8. **Stale forward references** — an advisory, not an error. A sub-task that
   `PLAN.md` marks `Implemented`, still written about in the future tense.
9. **The build register**, by calling `builds.sh check`: the schema of
   `docs/project/builds.tsv`, that its numbers are consecutive, that its
   constrained fields are drawn from their vocabularies, and that
   `docs/project/BUILDS.md`'s generated section is what the record renders to.
   The last is the one that matters — a generated document nothing regenerates
   stops being true the first time somebody edits it by hand.

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
