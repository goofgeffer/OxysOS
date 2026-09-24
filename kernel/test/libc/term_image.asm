; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; File: kernel/test/libc/term_image.asm
; Purpose: Carries poll-check, the program that asserts the call the terminal
;          emulator of sub-task 9.6 waits in, in the kernel image for
;          kernel/test/libc/term.c to load and run.
; Key definitions: KernelProgramPollCheckBegin, KernelProgramPollCheckEnd.
; References:
;   - docs/design/TERMINAL.md: what the program asserts.

bits 64

section .rodata

align 8

global KernelProgramPollCheckBegin
global KernelProgramPollCheckEnd

KernelProgramPollCheckBegin:
    incbin "build/user/poll-check.embed.elf"
KernelProgramPollCheckEnd:
