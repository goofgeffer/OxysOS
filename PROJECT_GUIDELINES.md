<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Oxys-OS Project Guidelines

The rules binding upon all work in this repository, by any contributor, human
or automated. Where a document elsewhere disagrees with this one, this one
governs and the other is wrong.

## 1. Project identity

- **Name**: Oxys-OS (Oxys).
- **Purpose**: a monolithic, Unix-like x86_64 operating system whose kernel and
  userland are written from scratch in ISO C11 and assembly, and which may run
  and depend upon ported third-party tools.
- **Root directory**: `~/oxys-os`, within WSL2 on Windows.
- **Languages**: C11 for the kernel and userland; NASM for boot and
  architecture-specific routines.

## 2. Rules of engagement

- **Formality.** Documentation, comments and commit messages are formal,
  technical and objective: no emoji, slang or humour. This governs the register
  of the work; conduct between people is governed by `CODE_OF_CONDUCT.md`.
- **Specification first.** Before a subsystem is implemented, the authoritative
  specification is retrieved and cited (Intel SDM, Multiboot2, EXT2, System V
  ABI, UEFI, the relevant RFCs and data sheets). A value recalled rather than
  looked up is not a citation.
- **Documentation in the same change.** A change is complete only when every
  document it makes untrue has been corrected in the same commit, under the
  rules of Sections 7 and 11.
- **Original kernel and userland.** Everything under `kernel/`, `boot/`,
  `drivers/`, `graphics/`, `libc/`, `net/`, `crypto/`, `uefi/` and `userland/`
  is original. Reference implementations may be studied, never transcribed. The
  only exceptions are public-domain headers and stubs the toolchain requires.
- **Ported tools are permitted.** Third-party tools, toolchains and their
  libraries may be ported to run upon Oxys-OS and depended upon. A port lives in
  a directory of its own, carries its upstream name, version and licence, is
  recorded in `LICENSING.md` before it is committed, and is modified no further
  than porting requires.
- **Testing mandate.** Every milestone boots and is tested under QEMU
  (`-machine q35 -cpu qemu64 -smp cores=2`), VirtualBox and Bochs; UEFI under
  QEMU with OVMF. Real hardware is considered from the first image.

## 3. Technical stack and build

- **Toolchain**: `x86_64-elf-gcc`, `x86_64-elf-ld`, `nasm`, `grub-mkrescue`,
  `make`, installed in WSL2. A native, ported toolchain is the long-term
  successor.
- **Boot**: Multiboot2 through GRUB for BIOS; a UEFI PE32+ path in Phase 12.
- **Kernel image**: ELF64, higher-half, linked by `linker.ld`.
- **Build System**: GNU Make. The targets are, by group — building: `all`, `clean`, `iso`; running: `run-qemu`, `run-vbox`, `run-uefi`; checking: `verify` (boots the image headless under QEMU and fails on a missing banner or any `FAILED`), `clang-check`, `docs-check`, `spdx-check`, `lint` (both of the previous two), `spdx-apply` (the one checking target that writes) and `toolcheck`; recording: `build-record`. `tools/check-docs.sh` holds this list to the Makefile, so a target is added here in the same change that adds it there.
- **Every build is recorded.** Each image `make verify` boots gets one row in
  `docs/project/builds.tsv` through `make build-record`, including negative-test
  images (recorded dirty). Archiving an image with `ARCHIVE=1` is a judgement
  made for any build of consequence. `docs/project/BUILDS.md` is the procedure.
- **Debugging**: COM1 serial output from the earliest stage.

## 4. Code standards

- **File header.** Every source file opens with a block giving its path, a
  one-sentence purpose, its key functions or definitions, and the
  specifications it implements. `docs-check` verifies the path.
- **Names.** Types and global functions `PascalCase`; macros and constants
  `UPPER_SNAKE_CASE`; locals `snake_case`.
- **Comments.** Complete English sentences that explain *why*, above all the
  silent failure a decision prevents. A comment that restates the code is
  removed.
- **Diagnostics.** `-Wall -Wextra -Werror`; any exception is documented in the
  Makefile.
- The full standard is `docs/project/CODING-STANDARDS.md`.

## 5. Roadmap

Thirteen phases, ordered by dependency; `docs/project/PLAN.md` enumerates their
sub-tasks and is the single source of truth for progress.

1. Bootstrapping and early output. 2. Memory management with copy-on-write.
3. Interrupts, exceptions and the keyboard. 4. Basic device drivers.
5. EXT2. 6. Graphics, system calls, processes and SMP. 7. Userland and a
minimal C library. 8. The shell. 9. The desktop, its services and its
configuration. 10. Cryptography. 11. Networking. 12. UEFI. 13. Polish,
optimisation and hardening.

Graphics that need no process (framebuffer, primitives, font, console,
pointer, compositor) belong to Phase 6; everything that needs processes
(window manager, client protocol, desktop) to Phase 9. Beyond Phase 13 the
objective is self-hosting: Oxys-OS building Oxys-OS with ported compiler,
assembler and linker.

## 6. Research and references

- Permitted sources: the Intel SDM, Multiboot2, the EXT2 specification, the
  System V AMD64 ABI, ATA-8/ATAPI, IEEE 802.3 and the IETF RFCs, the UEFI
  specification, VBE and GOP documentation, and manufacturers' data sheets.
- Every specification relied upon is registered in
  `docs/project/REFERENCES.md`, with the sections relied upon, and cited by
  section where it is applied.

## 7. Update discipline

- A change touches, in the same commit: the design document of the subsystem;
  the directory `README.md` if a file was added, removed or repurposed;
  `PLAN.md` if a sub-task changed state; `STATUS.md` if a capability, an
  environment result or a known gap changed; and one line of `HISTORY.md`.
  Documentation-only corrections need no `HISTORY.md` line.
- Before a commit, `make verify` and `make lint` pass. Commits go to `main`; a
  second commit records the first's hash in `HISTORY.md` together with the
  build-register rows.
- This document is amended only by explicit decision of the project owner, and
  the commit that amends it records the reason.

## 8. Prohibited practices

- A non-standard or compiler-specific extension without the rationale recorded
  in `docs/project/CODING-STANDARDS.md`.
- Floating point in the kernel without a documented, specific justification.
- Reliance on undefined behaviour; pointer arithmetic, type punning and bitwise
  operations must be defined by C11.
- Third-party code inside the kernel proper. The kernel depends on GRUB and the
  toolchain only; ported tools run upon the system, not within the kernel.

## 9. Session checklist

At the start of a working session: the working directory is `~/oxys-os`; the
cross-compiler is on `PATH`; `docs/project/PLAN.md` reflects the state of the
work; the tree builds.

## 10. Directory-level documentation

Every source directory has a `README.md` stating its purpose, its files (one
line each), the specifications applied and the phase it belongs to, and it is
updated in the same change as the directory. A directory created ahead of its
phase receives its `README.md` with its first file.

## 11. Documentation

The documentation is part of the codebase and is held to the same standard:
accurate, current and without duplication. `docs/README.md` is the full
standard; its rules are binding and summarised here.

- **One home per fact.** Each fact is written in exactly one place; every other
  place links to it. A statement repeated is a statement that will drift.
- **Documents describe the present.** A design, device or storage document
  says how the system works now, in the present tense. It holds no dated
  amendments, no struck-through text and no account of how it came to be. When
  behaviour changes, the text is rewritten to the new behaviour; the old text
  is deleted, and git keeps it.
- **History lives in commits.** The narrative of a change — what was found,
  what was tried, the negative tests — is the commit message.
  `docs/project/HISTORY.md` is a one-line index of changes pointing at commits;
  `docs/project/TESTING-RECORD.md` is a one-line record of each test run.
- **Fixed forms.** Each kind of document has a fixed structure, given in
  `docs/README.md`: a design document ends with its verification table and its
  current limitations; a directory `README.md` is an index of its files.
