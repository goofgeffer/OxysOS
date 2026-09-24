; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; File: kernel/test/gfx/client_image.asm
; Purpose: Carries window-check, the program that asserts the client protocol
;          of sub-task 9.2, in the kernel image for kernel/test/gfx/client.c to
;          load and run, as kernel/test/proc/signal_image.asm carries
;          signal-check.
; Key definitions: KernelProgramWindowCheckBegin, KernelProgramWindowCheckEnd.
; References:
;   - docs/design/WINDOWS.md: what the program asserts.

bits 64

section .rodata

align 8

global KernelProgramWindowCheckBegin
global KernelProgramWindowCheckEnd

KernelProgramWindowCheckBegin:
    incbin "build/user/window-check.embed.elf"
KernelProgramWindowCheckEnd:
