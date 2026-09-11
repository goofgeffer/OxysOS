<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Build Register

**Document status**: Living register, written by a program. A row is appended by
[`../../tools/record-build.sh`](../../tools/record-build.sh), which `make
build-record` invokes; no row is composed by hand, and none is edited after it is
written.

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
register is committed with the source, because a record kept outside the
repository is a record that exists upon one machine.

## What a row holds

| Field | What it is |
| ----- | ---------- |
| `#` | The build number. One greater than the largest already here, read from this table rather than from a counter kept elsewhere — a counter is a second thing that can disagree with the record. |
| Date | When the row was written, in coordinated universal time, so that rows from different machines sort. |
| Commit | The commit the tree stood at. A `-modified` suffix means the working tree differed from it, and the image is therefore **not** the one that commit produces. A row that omitted that would be wrong and look precise. |
| Compiler | The first line of the compiler's own version report. Two compilers build this tree — see [`TOOLCHAIN.md`](TOOLCHAIN.md) — and which one produced an image is a fact about the image. |
| Kernel | The size of the kernel ELF image in bytes, from `$BUILD_DIR/oxys.elf`. |
| ISO | The size of the ISO image in bytes, from `$BUILD_DIR/oxys.iso`. |
| Result | What the serial log of `make verify` says: the banner reached, and no verdict of `FAILED`. The count of assertions is the one [`../../tools/check-docs.sh`](../../tools/check-docs.sh) uses, written the same way, so that two programs cannot report two numbers for it. Where the run was in an environment that leaves no such log — VirtualBox, real hardware — a person supplies the word with `--result` and this column says what they observed. |
| Environments | Where the image was run, if anywhere. Free text: [`TESTING.md`](TESTING.md) names five environments and a sixth must not require editing a program. |
| Note | Why the build was made. The one field a person writes, and the only one that says anything the other eight cannot. |

## What a row *is*

**One image, recorded once**, with the environments it had been run in by the
moment the row was written. It is not one row per run: the same image booted in a
fourth environment a week later gets a second row naming the same commit and the
same sizes, and the two rows together say what happened. That is the price of an
append-only register and it is the right price — a row that could be revised
would be a row whose earlier content nobody could cite.

## How to add one

```sh
make verify                                    # or make all, or make iso
make build-record NOTE="what this build is"
```

An image built somewhere other than `build/` is recorded from where it is:

```sh
BUILD_DIR=build-clang make build-record ENVIRONMENT=QEMU NOTE="the same source by clang"
```

or, for an environment whose evidence the script cannot read:

```sh
tools/record-build.sh --environment VirtualBox --result "booted, log read by hand" \
    "sub-task 7.2 under VirtualBox"
```

The script builds nothing and runs nothing. It reads the tree and the artefacts
that are already there, which is what makes it safe to call after any target and
after a boot a person observed themselves. A script that rebuilt in order to
record would be recording something other than what was run.

## The order of the rows

Ascending by number, which is the opposite of [`HISTORY.md`](HISTORY.md) and is
deliberate. A history is read from the present backwards and a register is read
by looking a number up; appending is also the only operation that cannot disturb
a row already written, and no row here may be disturbed.

## The record

| # | Date (UTC) | Commit | Compiler | Kernel | ISO | Result | Environments | Note |
| - | ---------- | ------ | -------- | ------ | --- | ------ | ------------ | ---- |
| 1 | 2026-09-11 21:39 | `0439d05-modified` | x86_64-elf-gcc 13.2.0 | 1934352 | 7063552 | passed (55 assertions) | QEMU, Bochs, VirtualBox, OVMF (did not boot; the UEFI path is Phase 12) | Sub-task 7.2: the first image carrying the C library's system-call wrappers. |
| 2 | 2026-09-11 21:39 | `0439d05-modified` | Ubuntu clang version 18.1.3 | 1616936 | 6746112 | passed (55 assertions) | QEMU | The same source built by the second compiler, to establish that the wrappers' assembly and static assertions compile and run under both. |
| 3 | 2026-09-11 22:28 | `3bb9b20` | x86_64-elf-gcc 13.2.0 | 1934352 | 7063552 | passed (55 assertions) | QEMU | Sub-task 7.2 as committed at 3c0490b, rebuilt clean from the committed tree. |
