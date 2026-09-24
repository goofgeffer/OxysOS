<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# History

An index of every change, newest first: one line each, pointing at the commit.
The commit message holds the reasoning, what was found and the negative tests;
the design documents hold how the system works now. A row is added in the same
commit as the change and its hash filled in by the second commit,
`PROJECT_GUIDELINES.md`, Section 7. A correction to documentation alone has no
row.

| Date | Phase | Change | Commit |
| ---- | ----- | ------ | ------ |
| 2026-09-24 | Phase 9 | Launcher icons for Files and Windows, drawn by the project owner, shipped on the ramdisk and named in `/etc/session.conf` | — |
| 2026-09-24 | Phase 9 | Settings a person can recover without knowing anything, and see without restarting | `9382d5e` |
| 2026-09-24 | Phase 9 | The launcher opens whatever the file says, and the list of windows is not left stale | `e381ca2` |
| 2026-09-24 | Phase 9 | A write of nothing returns zero, and `micro` saves files with blank lines in them | `393b437` |
| 2026-09-24 | Phase 9 | `clear` works in a terminal window | `2b250f0` |
| 2026-09-23 | Phase 9 | The persistent `/etc` | `ccdd12e` |
| 2026-09-23 | Phase 9 | Sub-task 9.7: the file manager, the text viewer and the clock | `4cab1ea` |
| 2026-09-23 | Phase 9 | A background upon the system's own filesystem, and windows that minimise and fill the screen | `27cf149` |
| 2026-09-23 | Phase 9 | The mark and the icons drawn smooth, at the resolution they are shown at | `7f1c530` |
| 2026-09-22 | Phase 9 | The launcher draws icons, and they are files | `74b8443` |
| 2026-09-22 | Phase 9 | `fg` and `bg` continue a job whether or not the shell has noticed it stopped | `e8bb681` |
| 2026-09-22 | Phase 9 | The terminal draws white upon black | `e8bb681` |
| 2026-09-22 | Phase 9 | Sub-task 9.6: the terminal emulator — a window with an unmodified `/bin/sh` beneath it upon a pair of pipes | `182a724` |
| 2026-09-21 | Phase 9 | The screen is handed back to the console before the reason is written, and cleared when it is | `bb2e6de` |
| 2026-09-21 | Phase 9 | The demonstration draws the mark | `b1f1f65` |
| 2026-09-21 | Phase 9 | The build register's archive check is a machine's, not the project's | `b1f1f65` |
| 2026-09-21 | Phase 9 | The desktop's accent points at the shared palette | `3a1d910` |
| 2026-09-21 | Phase 9 | The shell taken out of the launcher | `3a98088` |
| 2026-09-21 | Phase 9 | The appearance the project owner asked for, and the terminal freeze asking for it found | `dc53e07` |
| 2026-09-21 | Phase 9 | The terminal livelock, found by the launcher and fixed | `dc53e07` |
| 2026-09-20 | Phase 9 | Sub-task 9.5: the session — the root, the panel, the launcher, and who may draw | `7bddae2` |
| 2026-09-18 | Phase 9 | Sub-task 9.4: the system configuration, its parser and `/etc` | `9827f82` |
| 2026-09-17 | Phase 9 | Sub-task 9.3: `init`, the shutdown, and the boot screen | `7775299` |
| 2026-09-17 | Phase 9 | Sub-task 9.2: the client protocol | `fa72ce2` |
| 2026-09-17 | Phase 9 | Sub-task 9.1: the window manager, the first of Phase 9 | `d9b084a` |
| 2026-09-16 | — | Every build is recorded from now on | `ee505ef` |
| 2026-09-16 | Release | `Oxys 1 Alpha` cut | `6bcb5fd` |
| 2026-09-16 | Phase 8 | Cleaned up before the alpha | `bc67b2b` |
| 2026-09-16 | Phase 8 | `head`, `tail`, `grep`, `sort`, `mv` and `ps`, with the `link` and `procinfo` calls | `530ea4e` |
| 2026-09-16 | Phase 8 | Sub-task 8.7: job control, process groups and terminal signal delivery, closing the phase; the alpha it cuts is held back at the project owner's direction | `b63eaf7` |
| 2026-09-16 | Phase 8 | Three things the project owner asked for at the prompt | `9f7b8e9` |
| 2026-09-16 | Phase 8 | `clear` | `db9b891` |
| 2026-09-16 | Phase 8 | `micro`, a line editor | `db80add` |
| 2026-09-16 | Phase 8 | Sub-task 8.6: pipelines, and beneath them the first two programs this kernel ever ran at once | `171b085` |
| 2026-09-15 | Phase 8 | Sub-task 8.5: input and output redirection, performed in the child between `fork` and `execve` in the order written | `977ebfb` |
| 2026-09-15 | Phase 8 | Sub-task 8.4: external program execution by `fork` and `execve` — what the phase was for | `c7d8399` |
| 2026-09-15 | — | The line the quiet boot printed before the prompt removed | `858551b` |
| 2026-09-15 | Phase 8 | Sub-task 8.3: the built-ins `cd`, `pwd`, `export` and `exit`, and beneath the first of them the working directory this kernel did not have | `2c1b1d0` |
| 2026-09-15 | — | The boot banner made one line | `ebfd7fd` |
| 2026-09-15 | Phase 8 | Sub-task 8.2: the tokeniser and the command parser | `f2451c7` |
| 2026-09-15 | — | The default boot made quiet upon the screen, and a `diagnostics` menu entry added | `23984bf` |
| 2026-09-15 | Phase 8 | Sub-task 8.1: line editing with history, and Phase 8 opened | `4f3516a` |
| 2026-09-14 | Phase 7 | Sub-task 7.7: the initial ramdisk, and Phase 7 closed | `16e7e5a` |
| 2026-09-13 | Phase 7 | Sub-task 7.6: the utilities `ls`, `cat`, `echo`, `mkdir` and `rm`, the six system calls by which a program reaches the filesystem | `546b36c` |
| 2026-09-13 | — | Numbering restarts at 1, and the retirement directive is removed | `c73539b` |
| 2026-09-13 | — | The build register cleared and recording suspended until the alpha | `dc5e9e1` |
| 2026-09-13 | Phase 7 | Sub-task 7.5: `crt0`, the termination functions of §7.22.4, `libc/user.ld`, and the same library sources compiled a second time into an archive | `1c02fbf` |
| 2026-09-13 | Phase 7 | Sub-task 7.4: the buffered stream of ISO/IEC 9899:2011 §7.21 and the formatted conversion above it — `FILE` incomplete, `stdout` line buffered and `stderr` unbuffered | `f11c4ca` |
| 2026-09-13 | — | The register given an archive, so that a row can be more than a description of a missing thing | `ae99b0a` |
| 2026-09-12 | — | The architecture boundary given a check, and the claim it rested on corrected | `a496a56` |
| 2026-09-12 | — | The two fault-screen boot entries withdrawn | `f605407` |
| 2026-09-12 | — | The header corpus and the self-tests grouped | `f605407` |
| 2026-09-12 | — | `kernel/arch/x86_64/` established | `7a87df0` |
| 2026-09-12 | — | Three files moved to the directories their own documents said they were in | `7a87df0` |
| 2026-09-11 | Phase 7 | Sub-task 7.3: `malloc`, `calloc`, `realloc` and `free` of ISO/IEC 9899:2011 §7.22.3 over a first-fit allocator upon an address-ordered free list | `5765338` |
| 2026-09-11 | — | The alpha and the beta confined to `Oxys 1`, and the internal debug build defined as not a release | `af57623` |
| 2026-09-11 | — | Three releases planned, and `VERSIONING.md`, Section 5.4, amended to permit two of them | `3a136b9` |
| 2026-09-11 | — | The build register given a record, three builds after it was given a table | `436acf3` |
| 2026-09-11 | — | The build register, and `PROJECT_GUIDELINES.md`, Section 3, amended to admit it | `3c0490b` |
| 2026-09-11 | — | Two claims the corpus made about VirtualBox and Bochs, corrected by running them | `3c0490b` |
| 2026-09-11 | Phase 6 | The scheduler self-test's wait was bounded in yields and is now bounded in time | `d102108` |
| 2026-09-11 | Phase 7 | Sub-task 7.2: a wrapper for each of the seven calls the kernel numbers, the `SYSCALL` instruction beneath them in a NASM translation unit, the `errno` of ISO/IEC 9899 | `3c0490b` |
| 2026-09-10 | — | `tinycc` recorded as a compiler candidate the project owner has assented to | `4acd02a` |
| 2026-09-10 | Phase 7 | Sub-task 7.1, which opens Phase 7: the nineteen functions of ISO/IEC 9899:2011, Section 7.24, that need no locale and no `errno` | `fa5c1e3` |
| 2026-09-10 | — | `PROJECT_GUIDELINES.md`, Section 3's list of build targets brought current, and made self-enforcing | `b371ee0` |
| 2026-09-10 | — | `tools/`, and the checks that used to be somebody's memory | `3c26301` |
| 2026-09-10 | — | `LICENSING.md`'s path table completed, and the corpus aligned to the porting decision | `44f46b0` |
| 2026-09-10 | — | The compiler will be ported, not written | `88b7fa8` |
| 2026-09-10 | — | Self-hosting recorded as the long-term objective | `15ddaa1` |
| 2026-09-10 | Phase 6 | Sub-task 6.15, which closes Phase 6: per-processor run queues each under a lock of its own, round-robin rotation, affinity as a hard constraint | `a6a355f` |
| 2026-09-10 | Phase 6 | Sub-task 6.14: the application processors started by INIT-startup-startup into a real-mode trampoline assembled as a flat binary and embedded with `incbin` | `54940df` |
| 2026-09-09 | — | The three largest documents divided, by the rule `ARCHITECTURE.md` §2.2 already stated for a translation unit | `b95a32f` |
| 2026-09-09 | — | A release versioning scheme adopted, at the project owner's direction, and written before the first release rather than at it | `b95a32f` |
| 2026-09-09 | Phase 6 | Sub-task 6.13: the ticket spinlock, the per-processor area, the inter-processor interrupt and the translation-lookaside-buffer shootdown | `b95a32f` |
| 2026-09-09 | — | The `v0.1.0` release, its tag and the version badge withdrawn at the project owner's direction | `bb3e5b0` |
| 2026-09-08 | — | `SECURITY.md` added, and `CONTRIBUTING.md`, `README.md` and `LICENSING.md` pointed at it | `e7c14bf` |
| 2026-09-08 | — | `PROJECT_GUIDELINES.md` amended at the project owner's explicit direction, per its own Section 7 | `12e7452` |
| 2026-09-08 | — | `CODE_OF_CONDUCT.md` added, and `CONTRIBUTING.md`, `README.md` and `LICENSING.md` pointed at it | `b50a232` |
| 2026-09-08 | Phase 6 | Sub-task 6.12: the ACPI tables, the Local APIC, the I/O APIC, and the retirement of the 8259A pair | `e8354da` |
| 2026-09-08 | All | Three build scripts at the root, and `.gitattributes` | `4a93317` |
| 2026-09-08 | All | A GitHub workflow added, and badges upon the root `README.md` | `9d21e97` |
| 2026-09-08 | All | The historical asides swept out of the living documents | `4ec63c7` |
| 2026-09-08 | Phase 9 | The appearance intended stated, and SerenityOS withdrawn as an inspiration | `4ec63c7` |
| 2026-09-07 | Phase 6 | Sub-task 6.11: `fork`, `execve`, `exit` and `wait` | `72432a5` |
| 2026-09-07 | All | The repository licensed, at the project owner's decision: the kernel `LGPL-3.0-or-later`, the userland `MIT`, the documentation `CC0-1.0` | `907bba2` |
| 2026-09-07 | All | A second compiler adopted as an independent judge, and the first thing it found corrected | `71fec28` |
| 2026-09-07 | Phase 1 | Sub-task 1.12 closed | `d77ade8` |
| 2026-09-07 | Phase 4 | The machine every storage fault was reported from identified precisely, at the project owner's correction: an HP Laptop 14-dq0052dx — Intel Celeron N4120, four cores | `1dd970e` |
| 2026-09-07 | All | `PLAN.md` refactored into a roadmap | `889342d` |
| 2026-09-07 | All | The three files next largest after `kernel/fs/ext2.c` divided likewise, at the project owner's direction and by the rule that sub-task's division established | `bc5078f` |
| 2026-09-07 | All | A review of the documentation and then of the code, at the project owner's direction, with no new sub-task begun | `9c3c148` |
| 2026-09-06 | Phase 4 | Sub-task 4.7: an AHCI driver, so that a machine whose firmware presents its serial ATA controller in AHCI mode has a disk at all | `bb530ea` |
| 2026-09-06 | Phase 3 | The serial driver corrected in three places, all of them a status bit believed without having been seen to change | `c0ac856` |
| 2026-09-06 | Phase 4 | Sub-task 4.8: an SD host controller driver, so that a machine whose system is upon an embedded MultiMediaCard has storage at all | `1bff1fe` |
| 2026-09-06 | Phase 6 | Sub-task 6.10: context switching, and the first descent to privilege level 3 | `9296c3f` |
| 2026-09-06 | Phase 6 | Sub-task 6.9: the process control block, the thread structure and the saved context | `dc5ecd1` |
| 2026-09-06 | Phase 6 | Sub-task 6.8: the ELF64 loader for statically linked executables | `9e11d4d` |
| 2026-09-06 | Phase 6 | Sub-task 6.7: the system-call entry path, the dispatch table and the validation of a caller's arguments | `687c14b` |
| 2026-09-06 | Phase 6 | Sub-task 6.6: the compositor | `3661dc4` |
| 2026-09-06 | Phase 6 | The graphical console's backspace corrected where it crosses a line separator | `e16de00` |
| 2026-09-06 | Phase 4 | The disk driver taught where a channel actually answers, and to say what storage it cannot reach | `8ea8658` |
| 2026-09-04 | Phase 6 | Sub-task 6.5: the PS/2 mouse and the pointer | `e2774f7` |
| 2026-09-04 | Phase 6 | Exceptions given a disposition, at the project owner's report that faults which threaten no more than one program were halting the machine | `0a21e34` |
| 2026-09-04 | Phase 6 | Sub-task 6.4 continued: the console made fast, and given fault screens | `a75a965` |
| 2026-09-04 | Phase 6 | Sub-task 6.4: the bitmap font and the graphical console, which end the blank screen sub-task 6.2 left | `5a755c9` |
| 2026-09-03 | Phase 6 | Sub-task 6.3: the 2D primitives — pixel, line, rectangle, blit and clipping — upon a surface rather than upon the framebuffer | `7bcbc98` |
| 2026-09-03 | Phase 6 | Sub-task 6.2: the linear framebuffer | `cdf74b0` |
| 2026-09-03 | Phases 6, 9 | Phase 9 split at the project owner's decision | `1a42a8a` |
| 2026-09-03 | Phase 6 | Sub-tasks 6.8 and 6.9 exchanged — their numbers that day; they are 6.13 and 6.14 since the renumbering above | `c3befd8` |
| 2026-09-03 | All | The boot-time self-tests moved out of `kernel.c` into `kernel/test/`, one file per subsystem | `8e778e2` |
| 2026-09-03 | Phase 6 | Sub-task 6.1: the apparatus of a privilege transition | `1903603` |
| 2026-09-02 | Phase 5 | Sub-task 5.8: the virtual filesystem layer, and an EXT2 root volume mounted through it | `f83f498` |
| 2026-09-02 | Phase 5 | A defect in sub-task 5.7 found by `e2fsck` | `f83f498` |
| 2026-09-02 | Phase 5 | Sub-task 5.7: names, and the creation of files | `ebc5987` |
| 2026-09-02 | Phase 5 | Sub-task 5.6: allocation, file writing and truncation | `c550424` |
| 2026-09-02 | Phase 5 | Sub-task 5.5: file reading and symbolic links | `4e453f1` |
| 2026-09-02 | Phase 2 | `KernelPagesFree` now establishes that the whole range released lies within the arena, not merely its first page | `08fcaeb` |
| 2026-09-02 | Phase 2 | Three integer-wrap defects corrected in the sub-task 2.5 allocators, found by review rather than by failure | `00d5472` |
| 2026-09-02 | Phase 5 | Sub-task 5.4: directory traversal and path resolution | `c9421d4` |
| 2026-09-02 | Phase 5 | Sub-task 5.3: inode retrieval and block-pointer resolution, through the direct, indirect, doubly and triply indirect pointers | `50fcf28` |
| 2026-09-02 | Phase 5 | Sub-task 5.2: the block group descriptor table, read and validated | `665a66d` |
| 2026-09-01 | Phase 1 | The `LOAD` segments of the image separated by permission — an accepted condition since Phase 1, discharged ahead of sub-task 13.3 | `d52828d` |
| 2026-09-01 | All | `INSPIRATIONS.md` added at the project owner's request, recording ToaruOS as the principal inspiration and stating expressly what is not taken from it | `ad6da48` |
| 2026-09-01 | Phase 5 | Sub-task 5.1: the EXT2 superblock, read through the buffer cache and decoded field by field from a buffer of bytes rather than by overlaying a structure | `a000311` |
| 2026-09-01 | All | `docs/` reorganised into four groups at the project owner's request: `project/`, `design/`, `devices/` and `storage/` | `50e72e6` |
| 2026-09-01 | Phase 4 | Sub-task 4.6: the buffer cache, completing Phase 4 | `6a9c176` |
| 2026-09-01 | Phase 4 | Sub-task 4.5: the generic block-device layer | `eea928c` |
| 2026-09-01 | Phase 4 | Sub-task 4.4: the ATA driver in programmed input/output mode, in both the 28-bit and 48-bit addressing forms | `792c9e5` |
| 2026-09-01 | Phase 4 | Sub-task 4.3: PCI enumeration by access mechanism one | `90c32aa` |
| 2026-09-01 | Phase 4 | The backspace across a row boundary corrected: it consumed the separator and the character before it, so one keystroke deleted two things | `bfbbf09` |
| 2026-09-01 | Phase 4 | Sub-task 4.2: the formal text-mode display driver | `9f168cd` |
| 2026-08-31 | Phase 4 | Sub-task 4.1: the interrupt-driven serial driver, claiming IR4 | `f0722ce` |
| 2026-08-31 | Phase 3 | The backspace key repaired | `96dc484` |
| 2026-08-31 | Phase 3 | Sub-task 3.7: the PS/2 keyboard, completing Phase 3 | `32ee3b4` |
| 2026-08-31 | Phase 3 | Sub-task 3.6: the interval timer as a rate generator | `28264d7` |
| 2026-08-31 | Phase 3 | Sub-task 3.5: the 8259A pair remapped to vectors 32–47 | `421ac38` |
| 2026-08-31 | Phase 2 | Sub-task 2.8: address-space cloning, completing Phase 2 | `8b05d27` |
| 2026-08-31 | — | This document reviewed and corrected: the status section had accumulated as a chronological narrative duplicating this table | `e235f6f` |
| 2026-08-30 | Phase 3 | Sub-task 3.4: the exception handlers and their diagnostics | `80ba170` |
| 2026-08-30 | Phase 3 | Sub-task 3.3: the interrupt dispatcher, a table of 256 entries with a registration interface | `b56ec8b` |
| 2026-08-30 | Phase 3 | Sub-task 3.2: a stub for each of the 256 vectors, normalising the vector number and the presence of an error code into a uniform trap frame | `35eaaa6` |
| 2026-08-30 | Phase 3 | Sub-task 3.1: the interrupt descriptor table and the 64-bit gate descriptor, loaded with `LIDT` and read back with `SIDT` | `6e8bccd` |
| 2026-08-30 | Phase 2 | Sub-task 2.7: copy-on-write fault resolution, using bit 9 of the page-table entry, which Intel SDM Table 4-19 records as ignored by the processor | `c574fb2` |
| 2026-08-30 | — | The project guidelines consolidated under `PROJECT_GUIDELINES.md` and the documentation index relocated to the repository root | `48c678e` |
| 2026-08-30 | Phase 2 | Sub-task 2.6: per-frame reference counting over a 255 KiB table | `ed48893` |
| 2026-08-30 | Phase 2 | Sub-task 2.5: the kernel virtual address allocator over the 32 TiB arena, and a slab heap above it | `cfe5bad` |
| 2026-08-30 | Phase 2 | Sub-task 2.4: the direct physical map at `0xFFFF800000000000` with 2 MiB pages | `aff2abc` |
| 2026-08-30 | Phase 2 | Sub-task 2.3: the permanent kernel paging hierarchy constructed and activated, the text and read-only data mapped read-only, and the low identity mapping removed | `d4c98ba` |
| 2026-08-30 | Phase 2 | Sub-task 2.2: the bitmap physical frame allocator. 131,039 frames governed under QEMU with 512 MiB, of which 288 are reserved | `b098c95` |
| 2026-08-30 | Phase 2 | Sub-task 2.1: the Multiboot2 information structure parsed into the boot-protocol-neutral `BootInformation` description | `7c379e5` |
| 2026-08-30 | Phase 1 | `PROJECT_GUIDELINES.md` amended at the project owner's request by the addition of Section 10, requiring directory-level documentation | `d97fa4d` |
| 2026-08-30 | Phase 1 | Project initialised | `64c4c42` |
