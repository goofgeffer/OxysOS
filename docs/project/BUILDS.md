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
This says **which image**, and it is the only one of the five that answers that —
though it presently answers it about nothing, recording having been suspended
until `Oxys 1 Alpha`.

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

A Markdown table is a view and cannot be a record. It lacks three things a record
must have.

**A bounded document.** A table in this file would put every build into the file
a person opens, so the thing a reader meets would grow without limit and the
prose explaining it would sink further from the top with each build. The
generated section below is capped at the twenty most recent builds and will be
the same size when there are ten thousand.

**A schema.** In a table every field is free text. `passed (55 assertions)`,
`x86_64-elf-gcc 13.2.0` and a comma-separated list of environments are each one
opaque string: nothing can be counted, filtered or compared, and nothing prevents
the next row from spelling any of them differently. The record has thirteen
columns, three of them drawn from fixed vocabularies, and the count of assertions
is a number rather than a phrase.

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
| `sha256` | The hash of the image, where the image was kept, and `-` where it was not. It is the hash of the **uncompressed** ISO and not of the compressed file in the archive, so that it names the artefact rather than this project's storage of it: were the compression ever changed the hash would still identify the same image, and a hash of the container would silently not. |
| `note` | Why the build was made. The one field a person writes, and the only one that says anything the other twelve cannot. |

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
tools/builds.sh check --deep                  # and re-hash every archived image
```

To keep the image as well as the row, and to keep one belonging to a build that
was recorded before anybody thought to:

```sh
make build-record ARCHIVE=1 NOTE="what this build is"
tools/builds.sh archive 9 build/oxys.iso
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

## What is kept, and what became of the first eight builds

*Written before the register was cleared, and left as it stands: the section below
is about what happens to images that are not archived, which is a property of the
arrangement and not of any row. See "The register was cleared" below.*

A row describes an image. **It is not the image**, and for the first ten builds
nothing kept the image at all: `build/` is ignored by git and `make clean`
removes it, so an ISO's life was typically the hour between being recorded and
the next clean rebuild.

The consequence was found by looking, on 2026-09-12, and it is worth stating
plainly because the register had been reporting it as a success the whole time:

| Builds | State |
| ------ | ----- |
| 1, 2, 4, 5, 6, 8 | **Unrecoverable.** Recorded `dirty=yes`, so the tree they were compiled from was never committed, and their images are gone. Nothing can reconstruct them. |
| 3, 7 | Image gone, but `dirty=no` — rebuildable from the commit, given the same toolchain. |
| 9, 10 | Archived — the only images that survived that far. They were deleted with the rest on 2026-09-13; see below. |

Six of the first eight builds, lost inside two days. Build 1 is the one worth
regretting: it is the first image this project ever numbered, it was exercised in
four environments — more than anything since — and its `assertions` field reads
55 where every build after it reads 56, so it is not an older copy of something
that still exists but a different artefact entirely.

### The two ways a build survives, and why both columns are needed

**`dirty`** answers *can this be rebuilt?* A build recorded against a clean tree
can be reproduced from its commit whether or not anyone kept the ISO.

**`sha256`** answers *does the image still exist?* An image kept is an image that
needs no toolchain, no commit and no rebuild to boot.

Neither substitutes for the other. A `dirty=yes` build with no archived image is
gone; a `dirty=yes` build that *was* archived survives as bytes although its
source state never will; and a clean build with no image survives as a recipe.
Only the two columns together say which.

**And the recipe does not recover everything.** Measured on 2026-09-13, by
building the same clean tree twice:

| Artefact | Reproducible? |
| -------- | ------------- |
| `build/oxys.elf` | **Yes, byte for byte.** The compile and the link are deterministic, so a clean build's kernel can be recovered exactly from its commit. |
| `build/oxys.iso` | **No.** `grub-mkrescue` writes a timestamp and a volume identifier of its own, so two runs over identical input produce different bytes. |

So `dirty=no` guarantees the *kernel* can be brought back and never the *image*.
An ISO that is not archived is gone in the strict sense even when everything
needed to build an equivalent one survives — and an equivalent one is not the
artefact that was booted, which is the distinction a build register exists to
keep. That is the argument for archiving a clean build's image and not only a
dirty one's.

### Where the images are, and why not here

`OXYS_BUILD_ARCHIVE`, which defaults to `~/oxys-builds/`. Outside the working
tree, and [`../../tools/builds.sh`](../../tools/builds.sh) refuses an archive
root inside one rather than merely recommending against it.

The arithmetic is why. An ISO is about 7 MB and compresses to about 2 MB; at one
to two builds a day that is roughly a gigabyte a year. This repository's entire
history is 28 MB. Git keeps a full copy of every binary it has ever seen, for
ever, and history cannot be rewritten once pushed — so the mistake would be
permanent and would be paid for by everybody who ever clones the project. A
check is used rather than a convention because a convention is what somebody
overrides at the moment it matters.

### Archiving is not the default

`make build-record ARCHIVE=1` keeps the image; plain `make build-record` does
not. Most builds here are made, verified and discarded within the hour, and
keeping every one would fill a disk with images nobody will ask for. Which builds
are worth keeping is a judgement, so it is a flag rather than a policy.

**And keeping is the weaker of the two protections.** The pre-release builds of
other systems that survive today mostly survive because they were *handed out* —
conference discs, beta programmes, subscriber downloads — and not because anyone
curated them; the vendors' own archives have holes. Many copies in many hands
beats one copy in one directory, and one directory is what this is. The builds of
this project most likely to exist in ten years are therefore the released ones
named in [`VERSIONING.md`](VERSIONING.md), Section 11.1, because those are the
ones that will leave this machine.


## The register was cleared, and no build was recorded until the alpha

**On 2026-09-13 the project owner directed that every row be removed and every
archived image deleted.** Twelve rows and three ISOs went; `~/oxys-builds/` is
empty, and [`builds.tsv`](builds.tsv) holds its licence tag, its schema and
nothing else. **Recording did not resume until `Oxys 1 Alpha`**, which
[`PLAN.md`](PLAN.md) fixed at sub-task 8.7 and which was cut on 2026-09-16: build 1
of the register below is the alpha's image, [`RELEASE-1-ALPHA.md`](RELEASE-1-ALPHA.md).

**The machinery is untouched, and that was the point.** `make build-record` works
exactly as it did and
[`../../tools/builds.sh`](../../tools/builds.sh) is byte for byte the script it
was. A register that could not be emptied without dismantling the thing that
keeps it would be a register whose tooling had become the record, and the whole
argument of this document is that the two are separate: the record is a file, the
tooling reads and writes it, and removing every line from the file should leave
the tooling exactly as it was. Clearing it is the first thing that ever tested
that claim, and the claim held.

### Numbering restarts at 1, and what that costs

**The next build recorded is 1**, by the owner's decision. A build number is
therefore unique within a register and **not** across this project's life.

That is a real cost and it is worth stating rather than glossing.
[`TESTING-RECORD.md`](TESTING-RECORD.md) gives accounts of builds 1, 5, 6 and 9
at length, and those accounts are not edited after the fact; when recording
resumes, a row numbered 1 will exist again and will describe a different image
entirely. **What tells the two apart is the date**, which every row carries and
every account in that document carries, and nothing else does. A reader who takes
a number alone as an identity will be wrong, and that document says so at its
head.

The alternative was considered and rejected by the owner: a directive naming the
highest number ever issued, which `record` would allocate above and `check` would
expect the first row to follow, so that a number never meant two things. It was
written, tested both ways, and removed again — because it made the register carry
a fact about a register that no longer exists, and because a number that is only
unique within a register is a defensible thing for a number to be. **What is not
defensible is leaving machinery in place that nothing uses**, which is why the
directive went rather than being kept dormant against a second clearing.

**What is lost is what the section above predicted would be lost.** Of the twelve
images, three still existed; the other nine had gone to `make clean` within hours
of being recorded, which that section records as the defect archiving exists to
answer. The three that survived are now gone too, by decision rather than by
neglect — and the rows describing them are gone with them, so the register no
longer claims anything it cannot show. That is the one property this clearing
strictly improves: there is no longer a row in this file describing an image
nobody can produce.

**What is kept is the writing.** Everything above and below this section — why a
register exists, why it is tab-separated, why archiving is not the default, and
what became of the first eight builds — stands unchanged, because none of it was
a fact about a particular row. The account of the six unrecoverable builds is
still the reason `ARCHIVE=1` exists, and it is still true that it happened.
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

Every build is in [`builds.tsv`](builds.tsv). What follows is generated from it —
and it is presently empty, the register having been cleared; the summary has
nothing to report but zeroes.

<!-- BEGIN GENERATED: tools/builds.sh render -->

*This section is generated from [`builds.tsv`](builds.tsv) by
[`../../tools/builds.sh`](../../tools/builds.sh). Do not edit it: `make lint`
regenerates it and fails if what is here differs.*

## Summary

| | |
| --- | --- |
| Builds recorded | 0 |
| Verified | 0 passed, 0 not |
| Compilers | - |
| Environments | - |
| First | - |
| Latest | - |

## The builds

| # | Date (UTC) | Commit | Compiler | Kernel | ISO | Result | Environments | Note |
| - | ---------- | ------ | -------- | ------ | --- | ------ | ------------ | ---- |

<!-- END GENERATED -->
