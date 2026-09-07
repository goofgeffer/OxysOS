; ==============================================================================
; File: kernel/cpu/syscall_entry.asm
;
; Purpose:
;   The entry point of the fast system-call mechanism, installed in IA32_LSTAR.
;   It leaves the caller's stack, saves the caller's registers, calls the
;   dispatcher, restores them, and returns by SYSRET.
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
;   - Intel SDM, Volume 2B, "SYSRET": with the REX.W prefix it returns to 64-bit
;     privilege level 3, loading RIP from RCX and RFLAGS from R11, and forcing
;     the RPL of the selectors it derives from IA32_STAR to 3.
;   - Intel SDM, Volume 2B, "SWAPGS": exchanges GS.base with the contents of
;     IA32_KERNEL_GS_BASE. It is valid only at privilege level 0, and is the
;     means by which a kernel entered with none of its own state addressable
;     reaches a register privilege level 3 could not have written.
;   - Intel SDM, Volume 3A, Section 5.8.8: the selectors derived from IA32_STAR.
;   - System V Application Binary Interface, AMD64 supplement, Section 3.2.3: the
;     first six integer arguments are passed in RDI, RSI, RDX, RCX, R8 and R9,
;     and RDI carries the first argument of the C function called below.
;
; The order of the first three instructions is the whole of the security of this
; path, and none of them may be moved.
;
;   SWAPGS first, because until it has run there is no addressable kernel state
;   at all: RSP belongs to the caller, every general register belongs to the
;   caller, and GS names whatever the caller left in it. It is the only
;   instruction here that needs nothing.
;
;   The caller's RSP is then put into the block GS names, because there is
;   nowhere else to put it. Every register is the caller's and must be given
;   back, and there is no stack yet to push it onto.
;
;   The kernel stack is loaded last, and only then may anything be pushed. A
;   path that pushed before switching would be writing to the caller's stack
;   while executing at privilege level 0 with the caller's page tables — which
;   is a kernel that writes wherever privilege level 3 asks it to.
;
; Why the arguments are not where a C caller would put them.
;
;   The System V convention passes the fourth integer argument in RCX, and
;   SYSCALL destroys RCX: it puts the return address there. The fourth argument
;   is therefore in R10 and everything else stands, which is the convention Linux
;   adopted and is adopted here for the same reason — there is no other register
;   the instruction leaves alone.
;
; What is deliberately absent.
;
;   There is no branch for a caller at privilege level 0. SYSRET returns to
;   privilege level 3 unconditionally, so this path may be entered from ring 3
;   and from nowhere else, and the self-test of sub-task 6.7 therefore does not
;   execute SYSCALL: it calls the dispatcher directly, which is an ordinary
;   function of an ordinary structure. A branch here that returned differently
;   for a caller the kernel trusts would be a test hook in the one path where a
;   test hook is indistinguishable from a privilege-escalation bug. See
;   docs/design/PRIVILEGE.md, Section 9.4.
; ==============================================================================

section .text
bits 64

global SyscallEntry

extern SyscallDispatch
extern SyscallEntryCount
extern SyscallObservedCodeSelector
extern SyscallObservedStackSelector
extern SyscallObservedFlagsValue

; The fields of SyscallProcessorBlock, addressed through GS. Their offsets are
; asserted against the C structure by a _Static_assert in kernel/cpu/syscall.c;
; the assembler cannot see the structure and would otherwise agree with it only
; by inspection.
%define BLOCK_KERNEL_STACK 0
%define BLOCK_USER_STACK   8

; ------------------------------------------------------------------------------
; SyscallEntry
;
; Entered by SYSCALL from privilege level 3. RCX holds the return address and R11
; the caller's RFLAGS; neither may be destroyed before SYSRET consumes it.
; ------------------------------------------------------------------------------
SyscallEntry:
    swapgs
    mov     [gs:BLOCK_USER_STACK], rsp
    mov     rsp, [gs:BLOCK_KERNEL_STACK]

    ; From here a stack exists and the caller's registers may be saved. The push
    ; order is the reverse of the field order of SyscallFrame, the stack growing
    ; downward, so that RSP afterwards is the address of the structure.
    push    qword [gs:BLOCK_USER_STACK]
    push    rax
    push    rbx
    push    rcx
    push    rdx
    push    rsi
    push    rdi
    push    rbp
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    ; Recorded for the report and for the self-test of sub-task 6.10, which is
    ; the first that can execute this path. The selectors exist nowhere else: the
    ; instruction loads them and SYSRET replaces them.
    mov     ax, cs
    mov     [rel SyscallObservedCodeSelector], ax
    mov     ax, ss
    mov     [rel SyscallObservedStackSelector], ax
    pushfq
    pop     qword [rel SyscallObservedFlagsValue]
    inc     qword [rel SyscallEntryCount]

    ; The frame is the argument, and the dispatcher writes the result into its
    ; RAX field.
    mov     rdi, rsp
    call    SyscallDispatch

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rbp
    pop     rdi
    pop     rsi
    pop     rdx
    pop     rcx
    pop     rbx
    pop     rax
    add     rsp, 8              ; The saved user stack, restored below instead.

    ; The reverse of the entry, in the reverse order and for the same reasons:
    ; the caller's stack is recovered while GS still names the kernel's block,
    ; and GS is put back last, when nothing further needs it.
    mov     rsp, [gs:BLOCK_USER_STACK]
    swapgs

    ; SYSRET with the REX.W prefix, which is what returns to 64-bit mode; without
    ; it the processor returns to compatibility mode and the caller's next
    ; instruction is decoded as 32-bit code.
    o64 sysret
