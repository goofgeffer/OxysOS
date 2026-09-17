; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
;
; File: kernel/test/libc/utilities_image.asm
; Purpose: Carries the eight programs of sub-task 7.6, the three utilities of
;          8.5, the one of 8.6, the editor and the six of 2026-09-16, inside the kernel image,
;          so that the self-test which runs them has an ELF file for each.
; Key definitions: KernelProgramArgCheckBegin, KernelProgramArgCheckEnd,
;          KernelProgramExecCheckBegin, KernelProgramExecCheckEnd,
;          KernelProgramFileCheckBegin, KernelProgramFileCheckEnd,
;          KernelProgramEchoBegin, KernelProgramEchoEnd, KernelProgramCatBegin,
;          KernelProgramCatEnd, KernelProgramListBegin, KernelProgramListEnd,
;          KernelProgramMakeDirBegin, KernelProgramMakeDirEnd,
;          KernelProgramRemoveBegin, KernelProgramRemoveEnd, KernelProgramTouchBegin,
;          KernelProgramTouchEnd, KernelProgramCopyBegin, KernelProgramCopyEnd,
;          KernelProgramRemoveDirBegin, KernelProgramRemoveDirEnd,
;          KernelProgramWordCountBegin, KernelProgramWordCountEnd,
;          KernelProgramMicroBegin, KernelProgramMicroEnd, and the six of
;          2026-09-16: KernelProgramHeadBegin to KernelProgramPsEnd.
; References:
;   - kernel/test/libc/utilities.c: the self-test that loads these and runs them.
;   - kernel/test/libc/startup_image.asm: the same technique at sub-task 7.5,
;     for one program, and the reasoning it records applies unchanged here.
;   - docs/design/LIBC.md, Section 12.5: why the programs are carried in the
;     image rather than placed upon a disk.
;
; Why eight programs are embedded and only one is written to the volume.
;
;   The composed volume of kernel/test/volume.h is a hundred and twenty-eight
;   blocks of a kibibyte, of which ninety-two are free — enough for one program
;   and not for eight. So the self-test loads seven of them straight out of these
;   bytes with ElfLoad, exactly as sub-task 7.5's test does, and writes only
;   `arg-check` onto the volume: that is the one program something must reach by
;   *path*, because `exec-check` names it in an `execve`.
;
;   Nothing is lost by the division. A program loaded from memory and a program
;   loaded from a volume take the same path through ElfLoad; what differs is the
;   route to the bytes, and `execve` exercises that route.
;
; Why the bounds are labels and not lengths: kernel/test/libc/startup_image.asm
; gives the reason, and it is that a length is a number nothing checks.

bits 64

section .rodata

align 8

global KernelProgramArgCheckBegin
global KernelProgramArgCheckEnd
global KernelProgramExecCheckBegin
global KernelProgramExecCheckEnd
global KernelProgramFileCheckBegin
global KernelProgramFileCheckEnd
global KernelProgramEchoBegin
global KernelProgramEchoEnd
global KernelProgramCatBegin
global KernelProgramCatEnd
global KernelProgramListBegin
global KernelProgramListEnd
global KernelProgramMakeDirBegin
global KernelProgramMakeDirEnd
global KernelProgramRemoveBegin
global KernelProgramRemoveEnd

KernelProgramArgCheckBegin:
    incbin "build/user/arg-check.embed.elf"
KernelProgramArgCheckEnd:

align 8
KernelProgramExecCheckBegin:
    incbin "build/user/exec-check.embed.elf"
KernelProgramExecCheckEnd:

align 8
KernelProgramFileCheckBegin:
    incbin "build/user/file-check.embed.elf"
KernelProgramFileCheckEnd:

align 8
KernelProgramEchoBegin:
    incbin "build/user/echo.embed.elf"
KernelProgramEchoEnd:

align 8
KernelProgramCatBegin:
    incbin "build/user/cat.embed.elf"
KernelProgramCatEnd:

align 8
KernelProgramListBegin:
    incbin "build/user/ls.embed.elf"
KernelProgramListEnd:

align 8
KernelProgramMakeDirBegin:
    incbin "build/user/mkdir.embed.elf"
KernelProgramMakeDirEnd:

align 8
KernelProgramRemoveBegin:
    incbin "build/user/rm.embed.elf"
KernelProgramRemoveEnd:

; The three utilities of sub-task 8.5 — the first that write a file, and the
; one that removes a directory — carried here for the ramdisk self-test's
; byte-for-byte comparison, as the five above are.
align 8
global KernelProgramTouchBegin
global KernelProgramTouchEnd
global KernelProgramCopyBegin
global KernelProgramCopyEnd
global KernelProgramRemoveDirBegin
global KernelProgramRemoveDirEnd
global KernelProgramWordCountBegin
global KernelProgramWordCountEnd
global KernelProgramMicroBegin
global KernelProgramMicroEnd
global KernelProgramPsBegin
global KernelProgramPsEnd
global KernelProgramMvBegin
global KernelProgramMvEnd
global KernelProgramSortBegin
global KernelProgramSortEnd
global KernelProgramGrepBegin
global KernelProgramGrepEnd
global KernelProgramTailBegin
global KernelProgramTailEnd
global KernelProgramHeadBegin
global KernelProgramHeadEnd

KernelProgramTouchBegin:
    incbin "build/user/touch.embed.elf"
KernelProgramTouchEnd:

align 8
KernelProgramCopyBegin:
    incbin "build/user/cp.embed.elf"
KernelProgramCopyEnd:

align 8
KernelProgramRemoveDirBegin:
    incbin "build/user/rmdir.embed.elf"
KernelProgramRemoveDirEnd:

align 8
KernelProgramWordCountBegin:
    incbin "build/user/wc.embed.elf"
KernelProgramWordCountEnd:

align 8
KernelProgramMicroBegin:
    incbin "build/user/micro.embed.elf"
KernelProgramMicroEnd:

align 8
KernelProgramHeadBegin:
    incbin "build/user/head.embed.elf"
KernelProgramHeadEnd:

align 8
KernelProgramTailBegin:
    incbin "build/user/tail.embed.elf"
KernelProgramTailEnd:

align 8
KernelProgramGrepBegin:
    incbin "build/user/grep.embed.elf"
KernelProgramGrepEnd:

align 8
KernelProgramSortBegin:
    incbin "build/user/sort.embed.elf"
KernelProgramSortEnd:

align 8
KernelProgramMvBegin:
    incbin "build/user/mv.embed.elf"
KernelProgramMvEnd:

align 8
KernelProgramPsBegin:
    incbin "build/user/ps.embed.elf"
KernelProgramPsEnd:
