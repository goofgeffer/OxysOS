; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; File: kernel/test/libc/line_image.asm
; Purpose: Carries the two programs of sub-task 8.1 inside the kernel image:
;          `line-check`, which the self-test runs against a session it placed
;          upon the terminal, and `sh`, whose copy upon the initial ramdisk the
;          self-test of sub-task 7.7 compares against this one.
; Key definitions: KernelProgramLineCheckBegin, KernelProgramLineCheckEnd,
;          KernelProgramShellBegin, KernelProgramShellEnd.
; References:
;   - kernel/test/libc/line.c: the self-test that loads the first and runs it.
;   - kernel/test/storage/initrd.c: the self-test that compares the second
;     against /bin/sh, byte for byte.
;   - kernel/test/libc/utilities_image.asm: the same technique at sub-task 7.6,
;     and the reasoning it records applies unchanged here.
;
; Why the shell is embedded when nothing here runs it.
;
;   The initial ramdisk's self-test compares each program in /bin against the
;   copy of the same file this image carries, so that a block read from the
;   wrong offset is caught rather than returned as data. The shell is in /bin
;   from this sub-task, so it is here too; the twenty kibibytes are the price
;   of that comparison and are paid for every utility.

bits 64

section .rodata

align 8

global KernelProgramLineCheckBegin
global KernelProgramLineCheckEnd
global KernelProgramShellBegin
global KernelProgramShellEnd

KernelProgramLineCheckBegin:
    incbin "build/user/line-check.embed.elf"
KernelProgramLineCheckEnd:

align 8
KernelProgramShellBegin:
    incbin "build/user/sh.embed.elf"
KernelProgramShellEnd:
