; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; File: kernel/test/shell/programs_image.asm
; Purpose: Carries the program of sub-task 8.4 that asserts what the shell
;          gives a program it runs, so that the self-test may write it onto the
;          root and have the shell find it there.
; Key definitions: KernelProgramEnvCheckBegin, KernelProgramEnvCheckEnd.
; References:
;   - kernel/test/shell/parser.c: the self-test that writes this to
;     /verify/env-check, runs the shell upon a session that invokes it, and
;     removes it.
;   - kernel/test/libc/startup_image.asm: the same technique, and the reasoning
;     it records applies unchanged here.

bits 64

section .rodata

align 8

global KernelProgramEnvCheckBegin
global KernelProgramEnvCheckEnd

KernelProgramEnvCheckBegin:
    incbin "build/user/env-check.embed.elf"
KernelProgramEnvCheckEnd:
