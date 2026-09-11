<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Build Register

**Document status**: Half of this document is written by hand and half by a
program, and the line between them is marked. Everything above *The record
itself* is prose and is edited as any document is. Everything between the
generated markers below is produced by
[`../../tools/builds.sh`](../../tools/builds.sh) from
[`builds.tsv`](builds.tsv), and `make lint` fails if it has been edited or has
gone stale.

**Where this sits**: [`PLAN.md`](PLAN.md) says what is being built;
[`STATUS.md`](STATUS.md) says what the system does today;
[`HISTORY.md`](HISTORY.md) says how it came to be that way;
[`TESTING-RECORD.md`](TESTING-RECORD.md) says what was tested and what happened.
This says **which image**, and it is the only one of the five that answers that.

**Phase**: none. It belongs to no phase, as
[`../../tools/README.md`](../../tools/README.md) records of everything in that
directory.

## Why a register exists at all

Every other record here is about the source. None of them can name an *image*.
By sub-task 7.2 this project had produced some hundreds of them, and not one
could be referred to: a defect seen under one emulator and not another, a boot
that worked yesterday, a kernel that grew by a hundred kibibytes between two
runs — each of those is an observation about a particular image, and an
observation that cannot name its subject is an anecdote.

So each image gets a number, and the number is what a later document cites. The
record is committed with the source, because a record kept outside the
repository is a record that exists upon one machine.

## The record is [`builds.tsv`](builds.tsv), and this document is a view of it

It was a Markdown table in this file, appended to by a program, and that lasted
three builds. A Markdown table is a view and not a record, and it could not have
three things a record must have.

**A bounded document.** Every build appended a row to the file a person opens,
so the thing a reader meets grew without limit and the prose explaining it sank
further from the top with each build. The generated section below is capped at
the twenty most recent builds and will be the same size when there are ten
thousand.

**A schema.** Every field was free text. `passed (55 assertions)`,
`x86_64-elf-gcc 13.2.0` and a comma-separated list of environments were each one
opaque string: nothing could be counted, filtered or compared, and nothing
prevented the next row from spelling any of them differently. The record now has
twelve columns, three of them drawn from fixed vocabularies, and the count of
assertions is a number rather than a phrase.

**A check.** A record nothing validates drifts, which is the defect every other
script in [`../../tools/`](../../tools/) was written after.
`tools/builds.sh check` runs under `make lint` and asserts the schema, the
consecutiveness of the numbers, the vocabularies, and — the one that matters —
that this document's generated section is what the record renders to.

### Why tab-separated and not JSON

Every tool in [`../../tools/`](../../tools/) is bash and coreutils and nothing
else. `jq` is not present upon the machine this was written on, parsing JSON with
`awk` is a way of being wrong slowly, and a new host dependency for a register of
build numbers is a poor trade. `awk -F'\t'` reads this correctly in one
expression, and so does anything else anybody will ever want to read it with.

### Why not SQLite, which is what "a database" usually means

**Because git is this project's storage, and a SQLite file is a binary.** It
would be the first binary in the repository: `git diff` would say *Binary files
differ* for the rest of the project's life, code review could not read it, and
two machines recording a build would produce a merge conflict no person could
resolve. Against a repository whose whole discipline is that everything in it is
text somebody can read, that is the wrong trade however good the query language
is.

The query language is available anyway. `tools/builds.sh sql` materialises a
throwaway database from the record, runs the statement and deletes it:

```sh
tools/builds.sh sql "SELECT compiler, COUNT(*), AVG(kernel) FROM builds GROUP BY compiler"
```

So SQL works wherever `sqlite3` is installed, nothing here requires it, and the
repository holds nothing but text either way. That is the whole of the
arrangement: **the record is the file, and SQLite is a lens.**

## The schema

| Column | What it is |
| ------ | ---------- |
| `number` | The build number, one greater than the largest already recorded — read from the record rather than from a counter kept elsewhere, a counter being a second thing that can disagree with it. It is `number` and not `#` because `#` begins a comment in this file type, and the SPDX tag at the head of the record uses one. |
| `date` | `2026-09-11T21:39Z`, in coordinated universal time so that rows from different machines sort and compare. |
| `commit` | The short hash the tree stood at. |
| `dirty` | `yes` where the working tree differed from that commit, so the image is **not** the one that commit produces. It is a column of its own and no longer a `-modified` suffix upon the hash: a suffix is a second fact encoded inside a field that already means something, and it meant that `grep 0439d05` did not find `0439d05-modified` — exactly backwards. |
| `compiler` | `gcc`, `clang`, `other` or `unknown`. Two compilers build this tree; see [`TOOLCHAIN.md`](TOOLCHAIN.md). |
| `cc_version` | Its version alone, so that versions can be compared as versions. |
| `kernel`, `iso` | Sizes in bytes, or `-` where the artefact was not there. |
| `result` | One of `passed`, `failed`, `did-not-boot`, `not-run`, `other`. Taken from the serial log by the same two conditions the `verify` target applies — the banner, and the absence of any verdict of `FAILED` — or supplied with `--result` for a run in an environment that leaves no log. |
| `assertions` | How many self-tests reported `passed` or `sound`, counted exactly as [`../../tools/check-docs.sh`](../../tools/check-docs.sh) counts them, so that two programs cannot report two numbers for one thing. |
| `environments` | Semicolon-separated tokens, each a name or `name:outcome` — `QEMU;Bochs;OVMF:did-not-boot`. The outcome is there because an image may pass in one environment and fail in another, which a single `result` column cannot say. [`TESTING.md`](TESTING.md) names five environments and a sixth must not require editing a program. |
| `note` | Why the build was made. The one field a person writes, and the only one that says anything the other eleven cannot. |

## Using it

```sh
make verify                                   # or make all, or make iso
make build-record NOTE="what this build is"   # appends a row and re-renders below

tools/builds.sh query --compiler clang
tools/builds.sh query --environment Bochs --result failed
tools/builds.sh query --since 2026-09-01 --last 5 --format md
tools/builds.sh sql "SELECT * FROM builds WHERE kernel > 2000000"
tools/builds.sh render                        # regenerate this document's view
tools/builds.sh check                         # what make lint runs
```

An image built somewhere other than `build/` is recorded from where it is, the
variable being the `Makefile`'s own:

```sh
BUILD_DIR=build-clang make build-record ENVIRONMENT=QEMU NOTE="the same source by clang"
```

For an environment whose evidence the script cannot read:

```sh
tools/builds.sh record --environment VirtualBox --result passed --assertions 55 \
    "booted headless, serial log read by hand"
```

**The script builds nothing and runs nothing.** It reads the artefacts already
there and the serial log the `verify` target leaves, which is what makes it safe
to call after any target and after a boot a person observed themselves. A script
that rebuilt in order to record would be recording something other than what was
run.

## What a row *is*, and the order of the rows

**One image, recorded once**, with the environments it had been run in by the
moment the row was written. It is not one row per run: the same image booted in a
further environment a week later gets a second row naming the same commit and the
same sizes, and the two rows together say what happened. That is the price of an
append-only record and it is the right price — a row that could be revised would
be a row whose earlier content nobody could cite.

Rows ascend by number, which is the opposite of [`HISTORY.md`](HISTORY.md) and is
deliberate. A history is read from the present backwards; a register is read by
looking a number up. Appending is also the only operation that cannot disturb a
row already written, and no row here may be disturbed.

## The record itself

Every build is in [`builds.tsv`](builds.tsv). What follows is generated from it.

<!-- BEGIN GENERATED: tools/builds.sh render -->

*This section is generated from [`builds.tsv`](builds.tsv) by
[`../../tools/builds.sh`](../../tools/builds.sh). Do not edit it: `make lint`
regenerates it and fails if what is here differs.*

## Summary

| | |
| --- | --- |
| Builds recorded | 4 |
| Verified | 4 passed, 0 not |
| Compilers | clang, gcc |
| Environments | Bochs, OVMF, QEMU, VirtualBox |
| First | 2026-09-11T21:39Z |
| Latest | 2026-09-11T22:42Z |

## The builds

| # | Date (UTC) | Commit | Compiler | Kernel | ISO | Result | Environments | Note |
| - | ---------- | ------ | -------- | ------ | --- | ------ | ------------ | ---- |
| 1 | 2026-09-11T21:39Z | `0439d05` *(modified)* | gcc 13.2.0 | 1934352 | 7063552 | passed (55 assertions) | QEMU;Bochs;VirtualBox;OVMF:did-not-boot | Sub-task 7.2: the first image carrying the C library's system-call wrappers. OVMF fails as TESTING.md Section 3 says it must until Phase 12. |
| 2 | 2026-09-11T21:39Z | `0439d05` *(modified)* | clang 18.1.3 | 1616936 | 6746112 | passed (55 assertions) | QEMU | The same source built by the second compiler, to establish that the wrappers' assembly and static assertions compile and run under both. |
| 3 | 2026-09-11T22:28Z | `3bb9b20` | gcc 13.2.0 | 1934352 | 7063552 | passed (55 assertions) | QEMU | Sub-task 7.2 as committed at 3c0490b, rebuilt clean from the committed tree. |
| 4 | 2026-09-11T22:42Z | `f5f8e6f` *(modified)* | gcc 13.2.0 | 1934352 | 7063552 | passed (55 assertions) | QEMU | The build register turned into a record with a schema: docs/project/builds.tsv, queried and rendered by tools/builds.sh. |

<!-- END GENERATED -->
