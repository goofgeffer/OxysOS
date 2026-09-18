; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; File: kernel/test/proc/init_image.asm
; Purpose: Carries init-check, the program that asserts the power and pause
;          calls of sub-task 9.3, in the kernel image for kernel/test/proc/init.c
;          to load and run, as signal_image.asm carries signal-check.
; Key definitions: KernelProgramInitCheckBegin, KernelProgramInitCheckEnd.
; References:
;   - docs/design/WINDOWS.md, Section 13.3: what the program asserts.

bits 64

section .rodata

align 8

global KernelProgramInitCheckBegin
global KernelProgramInitCheckEnd

KernelProgramInitCheckBegin:
    incbin "build/user/init-check.embed.elf"
KernelProgramInitCheckEnd:
