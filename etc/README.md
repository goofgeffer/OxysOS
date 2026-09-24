<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `etc/` — The System Configuration

**Phase**: 9, sub-task 9.4, of [`../docs/project/PLAN.md`](../docs/project/PLAN.md).
**Detailed design**: [`../docs/design/CONFIG.md`](../docs/design/CONFIG.md), which
is the format, every decision in it, and what each key means.
**Licence**: CC0-1.0. These are neither kernel nor userland but the settings a
person edits; [`../LICENSING.md`](../LICENSING.md), Section 1.

## Purpose

This directory is `/etc` upon the running machine. The `Makefile` stages every
file here onto the initial ramdisk, and the programs read them from `/etc` at
start.

**They are files in the repository rather than text a recipe writes**, so that
the thing a person edits upon the running machine and the thing they edit in the
source are the same file, and a change to one is a change git can show.

## Contents

| File | Read by | What it says |
| ---- | ------- | ------------ |
| [`system.conf`](system.conf) | `/bin/init` | The banner `init` prints, and a `[service]` block for each program it starts: `run`, `name`, `restart` and `needs`. |
| [`desktop.conf`](desktop.conf) | `/bin/windows` | The scale the desktop draws its windows at, and the accent it draws their contents with — `system` means the one `art/palette.h` carries. |
| [`session.conf`](session.conf) | `/bin/session` | How large the desktop is drawn, the picture that covers it, and a `[launch]` block for each program the launcher offers. |

**These are the shipped copies.** Upon a machine with no persistent disk, `/etc`
is the ramdisk's and an edit made upon the running machine is gone at the next
boot. Upon one carrying a disk labelled `oxys-etc` — `tools/etc-disk.sh create`
makes one — `/etc` is that disk, seeded from these files the first time, and an
edit survives a restart. A change to a file here then reaches only a disk that
does not yet have that file; copy the shipped one over it, below, to take it
again. The shipped copies are also at `/share/defaults/etc`, read-only and never
covered: `cp /share/defaults/etc/session.conf /etc/session.conf` takes one back
at once, and the session falls back to that file when `/etc/session.conf`
offers nothing to launch. [`../docs/storage/PERSIST.md`](../docs/storage/PERSIST.md).

## The format, in one paragraph

A line at a time. `#` begins a comment, `[name]` opens a section, `key = value`
sets a key within the section opened last, and a section repeated is a list —
which is how the services are written. A value is trimmed of the space either
side; quote it where a `#` belongs in it. **A line that cannot be read costs
that line and not the file**: the program reports it with its number and carries
on with what it could read, which is the whole reason the format is shaped this
way and is set out in `CONFIG.md`.

## What belongs here

What a person may change without a rebuild, and what more than a moment's work
would be needed to discover the default of. The kernel reads none of it: a
kernel that parsed text a person may edit would be a kernel whose boot depends
upon that text being right.

## What is not true of it yet

**Nothing here survives a reboot.** `/etc` is upon the initial ramdisk, which is
memory, so a file edited upon the running machine is gone at the next boot. The
complete list is `CONFIG.md`.
