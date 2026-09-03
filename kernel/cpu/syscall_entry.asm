; ==============================================================================
; File: kernel/cpu/syscall_entry.asm
;
; Purpose:
;   The provisional entry point of the fast system-call mechanism, installed in
;   IA32_LSTAR by sub-task 6.1 so that the register names an instruction rather
;   than address zero. It records what the processor loaded, so that the
;   configuration may be asserted, and returns to its caller.
;
; Key routines:
;   SyscallEntry - the address IA32_LSTAR holds.
;
; References:
;   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2B,
;     "SYSCALL": RCX receives the address of the instruction following SYSCALL,
;     R11 receives RFLAGS as it stood before the mask was applied, CS and SS are
;     loaded from IA32_STAR, and RFLAGS is then cleared of every bit set in
;     IA32_FMASK. The stack pointer is NOT changed.
;   - Intel SDM, Volume 2B, "SYSRET": returns to privilege level 3
;     unconditionally, the RPL of the selectors it loads being forced to 3.
;   - Intel SDM, Volume 3A, Section 5.8.8: the selectors derived from IA32_STAR.
;
; What this is, and what it is not.
;
; This is a placeholder. Sub-task 6.7 replaces it with the real entry path: one
; that exchanges GS with IA32_KERNEL_GS_BASE to find the per-processor data,
; switches to the kernel stack the task state segment names, builds a frame,
; validates the call number and its arguments, and returns by SYSRET.
;
; Two properties of the placeholder follow from that and are deliberate.
;
; It does not switch stacks. SYSCALL leaves RSP exactly as the caller had it, so
; a genuine entry from privilege level 3 would arrive here executing kernel code
; upon a user stack — which is why the real path's first act must be to leave
; that stack. Nothing enters here from privilege level 3, there being no user
; program until sub-task 6.10, and the only caller is the self-test of this
; sub-task, which executes SYSCALL from privilege level 0 where RSP is already
; the kernel stack. The pushes below are safe for that caller and for no other.
;
; It does not return by SYSRET. SYSRET returns to privilege level 3 whatever
; privilege it was reached from, so returning by it would drop the self-test into
; user mode with no user mapping to execute in. Control is returned instead by
; restoring the flags from R11 and jumping to the address in RCX, which arrives
; back in the caller at privilege level 0 with the kernel code selector the
; processor loaded still in force.
; ==============================================================================

section .text
bits 64

global SyscallEntry

extern SyscallObservedCodeSelector
extern SyscallObservedStackSelector
extern SyscallObservedFlagsValue
extern SyscallEntryCount

; ------------------------------------------------------------------------------
; SyscallEntry
;
; Entered by the SYSCALL instruction. RCX holds the return address and R11 the
; caller's RFLAGS; neither may be destroyed before it has been used.
; ------------------------------------------------------------------------------
SyscallEntry:
    push    rax

    ; The selectors the processor loaded from IA32_STAR. They are recorded here
    ; because they exist nowhere else: the instruction loads them and the return
    ; replaces them, so nothing outside this routine can observe what they were.
    mov     ax, cs
    mov     [rel SyscallObservedCodeSelector], ax
    mov     ax, ss
    mov     [rel SyscallObservedStackSelector], ax

    ; RFLAGS as it stands now, which is the caller's value with every bit of
    ; IA32_FMASK cleared. The assertion that matters is that the interrupt flag
    ; is among the bits cleared.
    pushfq
    pop     qword [rel SyscallObservedFlagsValue]

    inc     qword [rel SyscallEntryCount]

    pop     rax

    ; Restore the caller's flags and resume after the SYSCALL. An interrupt may
    ; be delivered between the two instructions once the flag is restored; that
    ; is harmless, the stack being the kernel's and the privilege level zero.
    push    r11
    popfq
    jmp     rcx
