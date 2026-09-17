; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
; ==============================================================================
; File: kernel/test/proc/signal_image.asm
;
; Purpose:
;   Carries signal-check inside the kernel image, so that the self-test of
;   sub-task 8.7 has the program to run: a check program is not shipped upon
;   the ramdisk, for the reason userland/README.md gives.
;
; Key definitions:
;   KernelProgramSignalCheckBegin, KernelProgramSignalCheckEnd.
;
; References:
;   - kernel/test/proc/signal.c: the self-test that loads this and runs it.
;   - kernel/test/proc/directory_image.asm: the same arrangement for dir-check.
; ==============================================================================

bits 64

section .rodata

align 8

global KernelProgramSignalCheckBegin
global KernelProgramSignalCheckEnd

KernelProgramSignalCheckBegin:
    incbin "build/user/signal-check.embed.elf"
KernelProgramSignalCheckEnd:
