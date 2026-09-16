; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; File: kernel/test/proc/directory_image.asm
; Purpose: Carries the program of sub-task 8.3 that asserts the working
;          directory inside the kernel image, so that the self-test which runs
;          it has an ELF file to load.
; Key definitions: KernelProgramDirCheckBegin, KernelProgramDirCheckEnd.
; References:
;   - kernel/test/proc/directory.c: the self-test that loads this and runs it.
;   - kernel/test/libc/startup_image.asm: the same technique, and the reasoning
;     it records applies unchanged here.

bits 64

section .rodata

align 8

global KernelProgramDirCheckBegin
global KernelProgramDirCheckEnd

KernelProgramDirCheckBegin:
    incbin "build/user/dir-check.embed.elf"
KernelProgramDirCheckEnd:
