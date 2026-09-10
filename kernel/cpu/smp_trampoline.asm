; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: LGPL-3.0-or-later
; ==============================================================================
; File: kernel/cpu/smp_trampoline.asm
;
; Purpose:
;   Carries the assembled real-mode trampoline of boot/trampoline.asm into the
;   kernel image as read-only data, and gives its two ends symbols the C of
;   kernel/cpu/smp.c can take the size from.
;
; Key data:
;   SmpTrampolineImageStart - The first byte of the assembled image.
;   SmpTrampolineImageEnd   - One past its last byte.
;
; References:
;   - docs/design/SMP.md, Section 3: why the trampoline is a flat binary
;     assembled at a fixed origin rather than a section of this image, and why
;     it is therefore embedded rather than linked.
;
; Why the image is embedded rather than read from a file at run time.
;
;   There is no filesystem at the moment the processors are started, and there
;   would be no reason to use one if there were: the trampoline is a few hundred
;   bytes and is as much a part of this kernel as any function in it. Embedding
;   it also makes the build order state the dependency — the image cannot be out
;   of step with the source it came from, because the link fails if it was not
;   assembled.
;
;   The path is relative to the directory make runs in, which is the repository
;   root; the Makefile's dependency upon $(TRAMPOLINE_BINARY) is what guarantees
;   the file exists before this file is assembled.
; ==============================================================================

section .rodata

; Aligned to a page although it is copied rather than mapped, so that the source
; and the destination of the copy share an alignment and the copy is not the
; place an unaligned access would first show itself.
align 4096

global SmpTrampolineImageStart
global SmpTrampolineImageEnd

SmpTrampolineImageStart:
    incbin "build/trampoline.bin"
SmpTrampolineImageEnd:
