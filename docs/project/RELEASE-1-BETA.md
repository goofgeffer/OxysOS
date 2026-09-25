<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Oxys 1 Beta

**Ordinal form**: `1-beta`. **Tag**: `v1-beta`. **Image**: `oxys-1-beta.iso`.
**Cut**: 2026-09-24, after sub-task 9.7, at the project owner's direction.

The release notes [`VERSIONING.md`](VERSIONING.md), Section 9.4, requires. They
say what was true of the image on the day it was cut and are not revised; what
becomes true later is [`STATUS.md`](STATUS.md)'s.

## 1. What kind of release this is

**The second and last pre-release of `Oxys 1`** ([`VERSIONING.md`](VERSIONING.md),
Section 5.4). It supersedes `Oxys 1 Alpha` and is superseded by `Oxys 1`. It takes
no name, point or edition.

**The judgement**: Section 11.1 fixed the beta at sub-task 9.7, the point at which
the system has a desktop a person can use. That point was reached on 2026-09-23;
the cut was held back by the owner for cleanup, and made now. Sub-task 9.8, the
settings application, is not in it, as planned: a desktop whose settings are
edited in a text file is one a person can use and complain about, which is what a
beta is for.

## 2. What it does

A person who boots the image sees the boot screen and then a desktop:

- **A background**: a photograph of a yellow rose by default, or a drawing of a
  cliff and a sun, chosen in `/etc/session.conf`.
- **A bar at the foot of the screen**: the `OXYS` launcher, opening upward with
  Windows, Terminal and Files; Terminal and Files pinned beside it as icons; a
  button per open window that restores or minimises it.
- **A clock** in a box at the top right.
- **Windows** that can be dragged, raised, minimised, made full (between the
  clock and the bar) and closed.
- **A terminal** running the shell of the alpha: line editing, history,
  pipelines, redirection, and the utilities in `/bin`, including the editor
  `micro`.
- **A file manager** and **a text viewer**.
- **Configuration that survives a restart** where a disk labelled `oxys-etc` is
  attached (`tools/etc-disk.sh`), with the shipped defaults always at
  `/share/defaults/etc` and the desktop falling back to them if a file is broken.

The GRUB menu still offers the shell alone and the diagnostic boot log.

## 3. What changed, and which sub-tasks closed

Since `Oxys 1 Alpha` (sub-task 8.7), sub-tasks **9.1 to 9.7** of
[`PLAN.md`](PLAN.md):

| Sub-task | Delivered |
| -------- | --------- |
| 9.1 | The window manager: stacking, focus, the pointer, frames. |
| 9.2 | The client protocol: programs own windows through system calls. |
| 9.3 | `init`: supervision, orphans, `power`, `shutdown`, the boot and power screens. |
| 9.4 | The configuration format and `/etc`. |
| 9.5 | The session: layers, root, panel, launcher, `window_text`. |
| 9.6 | The terminal emulator and `poll`. |
| 9.7 | The file manager, the text viewer, the clock; the real-time clock, `time` and `alarm`. |

Beyond the sub-tasks: the persistent `/etc`; the owner's mark, icons and
backgrounds; minimise and full screen; the bar at the foot with pinned programs;
recoverable configuration; and the documentation rewritten to a standard. Every
change is a line of [`HISTORY.md`](HISTORY.md).

## 4. Editions

None. There is one image.

## 5. Where it was tested

| Condition ([`VERSIONING.md`](VERSIONING.md), Section 10) | Done |
| -------------------------------------------------------- | ---- |
| `make verify` passes at the commit tagged | Yes: 78 self-test assertions, none failing. |
| `make clang-check` passes at that commit | Yes. |
| Every affected document is up to date | Yes, and `make lint` passes. |
| Booted under QEMU and VirtualBox, the runs recorded | Yes: QEMU q35 with two processors at 1280 by 800, with the desktop driven through the monitor; VirtualBox at 640 by 480. Bochs 3.1 at 1024 by 768 as well. Every self-test passed in all three ([`TESTING-RECORD.md`](TESTING-RECORD.md), 2026-09-24). |
| Booted on physical hardware | **No.** The owner's call per release, and not called for. Nothing of Phases 6 to 9 has been seen upon a physical machine. |

## 6. What it does not have

- **BIOS boot only.** UEFI is Phase 12.
- **No hardening.** No NX, SMEP or SMAP, no ASLR, no stack canaries
  ([`../../SECURITY.md`](../../SECURITY.md)). Not a system to expose to anything.
- **User programs run on one processor**; the others run kernel threads
  ([`../design/CONCURRENCY.md`](../design/CONCURRENCY.md)).
- **No networking and no cryptography** (Phases 10 and 11).
- **No settings application** (sub-task 9.8): settings are edited with `micro`.
- **The root is a 4 MiB ramdisk** rebuilt at every boot; only `/etc` persists, and
  only with an `oxys-etc` disk. `/home` does not persist.
- **The shell in a window has no job control** (no pseudo-terminal), and the
  terminal has no escape sequences, so no full-screen programs.
- **No time zone**, and nothing sets the clock.
- **No resize of a window by hand.**

## 7. The image

Built by `make iso` from commit `f224263`, `Cut Oxys 1 Beta`, recorded as **build 33** of
[`BUILDS.md`](BUILDS.md), archived under `~/oxys-builds/` with a copy named
`oxys-1-beta.iso` ([`VERSIONING.md`](VERSIONING.md), Section 9.2).

| Image | SHA-256 |
| ----- | ------- |
| `oxys-1-beta.iso` | `2fc78818b9191f6bda2cc3ab535cc093455d038d7b6aa056f4e3eab3355442e6` |

The image is 13,043,712 bytes. An image rebuilt from the same commit holds the
same kernel and programs but not
the same bytes: the ISO and the ramdisk carry the timestamps of their making.
