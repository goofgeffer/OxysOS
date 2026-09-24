; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; File: kernel/test/libc/config_image.asm
; Purpose: Carries config-check, the program that asserts the file-reading half
;          of the configuration parser of sub-task 9.4, in the kernel image for
;          kernel/test/libc/config.c to load and run.
; Key definitions: KernelProgramConfigCheckBegin, KernelProgramConfigCheckEnd.
; References:
;   - docs/design/CONFIG.md: what the program asserts.

bits 64

section .rodata

align 8

global KernelProgramConfigCheckBegin
global KernelProgramConfigCheckEnd

KernelProgramConfigCheckBegin:
    incbin "build/user/config-check.embed.elf"
KernelProgramConfigCheckEnd:
