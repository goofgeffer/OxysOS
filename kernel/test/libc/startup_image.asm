; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; File: kernel/test/libc/startup_image.asm
; Purpose: Carries the linked user program of sub-task 7.5 inside the kernel
;          image, so that the self-test which runs it has an ELF file to write
;          to the volume it composes.
; Key definitions: KernelStartupProgramBegin, KernelStartupProgramEnd.
; References:
;   - kernel/test/libc/startup.c: the self-test that writes these bytes to a
;     volume and asks execve to load them.
;   - kernel/arch/x86_64/smp/smp_trampoline.asm: the same technique one phase
;     earlier, for the real-mode trampoline, and the Makefile dependency that
;     guarantees the file exists before this is assembled.
;   - docs/design/LIBC.md: why the program is carried in the image
;     rather than placed upon a disk.
;
; Why the program is embedded rather than read from a medium.
;
;   `execve` loads from a path, and a path must lead to something. The self-tests
;   of Phase 5 onward compose an EXT2 volume in memory rather than trusting the
;   machine to carry one, because a test that needs a particular volume cannot
;   depend upon a disk it did not write; the program has the same problem and
;   takes the same answer. The bytes are in the kernel image, the self-test
;   writes them into the composed volume through the filesystem layer, and the
;   whole arrangement works upon a machine with no disk at all.
;
;   The path in the `incbin` is relative to the directory make runs in, which is
;   the repository root — the same convention smp_trampoline.asm uses. The
;   Makefile makes this object depend upon the program, so a stale embedding is
;   not a state this build can reach.
;
; Why the bounds are two labels and not a length.
;
;   A length would be a number that has to agree with a file, and nothing would
;   check it. Two labels are computed by the assembler from the bytes it actually
;   included, so the size the kernel reads is the size of what is there.

bits 64

section .rodata

align 8

global KernelStartupProgramBegin
global KernelStartupProgramEnd

KernelStartupProgramBegin:
    incbin "build/user/startup-check.embed.elf"
KernelStartupProgramEnd:
