; ==============================================================================
; File: kernel/proc/switch.asm
;
; Purpose:
;   The two transfers this kernel could not previously make: the exchange of one
;   thread's execution for another's, and the first descent from privilege level
;   0 to privilege level 3.
;
; Key routines:
;   ThreadSwitchContext - saves the calling thread's context and resumes another.
;   ThreadEnterUser     - leaves the kernel for privilege level 3 by IRETQ.
;   ThreadResumeUser    - the same, with a whole saved register set restored,
;                         which is how a thread made by fork continues its
;                         parent's program rather than beginning a new one.
;
; References:
;   - System V Application Binary Interface, AMD64 supplement, Section 3.2.1:
;     RBX, RBP and R12 to R15 are preserved across a call. A switch performed as
;     an ordinary function call therefore need save no others; the compiler has
;     already spilled anything else it cared about at the call site.
;   - System V ABI, AMD64, Section 3.2.3: the first two integer arguments arrive
;     in RDI and RSI.
;   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2A,
;     "IRET/IRETD/IRETQ": in 64-bit mode the instruction pops RIP, CS, RFLAGS,
;     RSP and SS from the stack in that order. A return to an outer privilege
;     level — one whose CS has a greater requested privilege level than the
;     current one — pops the stack pointer and stack segment as well, which is
;     what makes IRETQ the instruction that can enter privilege level 3.
;   - Intel SDM, Volume 3A, Section 6.14.4: the interrupt return of long mode,
;     and the five quadwords it expects.
;   - Intel SDM, Volume 1, Section 3.4.3: bit 1 of RFLAGS reads as one and is
;     reserved; a value with it clear is not a legal RFLAGS.
;   - Intel SDM, Volume 2B, "SYSRET": the instruction takes the address to return
;     to from RCX and the flags from R11, and raises a general-protection
;     exception — at privilege level 0, before the return — where RCX does not
;     hold a canonical address. That is the pair of facts ThreadResumeUser
;     declines it for; see the note upon that routine.
;
; Why the switch is a function call and not an interrupt.
;
;   A switch performed by an ordinary call inherits the calling convention's
;   promise: the compiler has already saved whatever it wanted to keep across the
;   call, so six registers and a stack pointer are the whole of a context. A
;   switch performed from an interrupt would have to save every register, because
;   the interrupted code made no such promise — and would then need a second,
;   different context format for threads switched voluntarily. One format is
;   better than two, and the voluntary switch is the one that happens most.
;
;   The instruction pointer is not saved and does not need to be. The call put a
;   return address upon the stack; saving the stack pointer therefore saves the
;   return address with it, and switching back returns through it.
; ==============================================================================

section .text
bits 64

global ThreadSwitchContext
global ThreadEnterUser
global ThreadTrampoline

extern ThreadTrampolineEntry

; The offsets of ThreadContext. The C structure is asserted against these by a
; _Static_assert in kernel/proc/process.c; the assembler cannot see it.
%define CONTEXT_RBX 0
%define CONTEXT_RBP 8
%define CONTEXT_R12 16
%define CONTEXT_R13 24
%define CONTEXT_R14 32
%define CONTEXT_R15 40
%define CONTEXT_RSP 48

; ------------------------------------------------------------------------------
; ThreadSwitchContext(ThreadContext *from, ThreadContext *to)
;
; RDI names where to save the calling thread's context; RSI names the context to
; resume. Returns — to its own caller — only when somebody switches back to the
; context saved through RDI.
; ------------------------------------------------------------------------------
ThreadSwitchContext:
    ; Saved into the outgoing context. The stack pointer is saved last, after
    ; nothing further will be pushed, because it is the field that describes
    ; where the rest of the thread's state lives.
    mov     [rdi + CONTEXT_RBX], rbx
    mov     [rdi + CONTEXT_RBP], rbp
    mov     [rdi + CONTEXT_R12], r12
    mov     [rdi + CONTEXT_R13], r13
    mov     [rdi + CONTEXT_R14], r14
    mov     [rdi + CONTEXT_R15], r15
    mov     [rdi + CONTEXT_RSP], rsp

    ; And the incoming context is restored. The stack pointer first, because
    ; everything after it is read from the stack it names.
    mov     rsp, [rsi + CONTEXT_RSP]
    mov     rbx, [rsi + CONTEXT_RBX]
    mov     rbp, [rsi + CONTEXT_RBP]
    mov     r12, [rsi + CONTEXT_R12]
    mov     r13, [rsi + CONTEXT_R13]
    mov     r14, [rsi + CONTEXT_R14]
    mov     r15, [rsi + CONTEXT_R15]

    ; The return address upon the incoming stack is where that thread last left
    ; off — or, for a thread that has never run, the trampoline its stack was
    ; prepared with.
    ret

; ------------------------------------------------------------------------------
; ThreadTrampoline
;
; Where a thread that has never run begins. It is reached by the RET above,
; because ThreadPrepareStart put its address upon the thread's kernel stack where
; a return address belongs — and nothing else there. The six preserved registers
; live in the context structure and are moved, not pushed, so a prepared stack
; that also held six saved registers would have the RET take the lowest of them
; instead of the address.
;
; It takes no arguments and can take none: it was reached by a return and not by
; a call. It asks the kernel which thread is current instead, which is the one
; ThreadSwitchTo made current before switching here.
; ------------------------------------------------------------------------------
ThreadTrampoline:
    ; The stack is aligned to sixteen bytes by construction — ThreadPrepareStart
    ; sees to it — and the ABI requires that at a call.
    xor     rbp, rbp                ; The first frame of a thread has no caller.
    call    ThreadTrampolineEntry

    ; ThreadTrampolineEntry does not return. If it somehow did, halting here is
    ; better than returning into a stack whose contents are prepared bytes.
.halt:
    cli
    hlt
    jmp     .halt

; ------------------------------------------------------------------------------
; ThreadEnterUser(uint64_t entry, uint64_t user_stack, uint16_t code_selector,
;                 uint16_t stack_selector)
;
; Descends to privilege level 3 and does not return. The arguments arrive in RDI,
; RSI, RDX and RCX.
;
; IRETQ is the instruction that can do this. A far return could too, but IRETQ is
; the one that also loads RFLAGS, and the flags a program starts with are part of
; the state it is entitled to.
; ------------------------------------------------------------------------------
ThreadEnterUser:
    ; The data segment registers are loaded before the stack segment is, because
    ; IRETQ loads SS itself and a fault between here and there would be delivered
    ; with these already pointing at the user's descriptor — which is harmless,
    ; where the reverse would leave SS naming a user descriptor at privilege
    ; level 0.
    ;
    ; In 64-bit mode DS, ES, FS and GS are largely ignored for addressing, but
    ; they are not ignored by an IRET that returns to compatibility mode and they
    ; are not ignored by everything a debugger will show. Loading them is one
    ; instruction each and leaves no register naming a kernel descriptor.
    mov     ax, cx
    mov     ds, ax
    mov     es, ax

    ; The five quadwords IRETQ pops, pushed in the reverse of the order it pops
    ; them: SS, RSP, RFLAGS, CS, RIP.
    push    rcx                     ; SS: the user stack selector, RPL 3.
    push    rsi                     ; RSP: the user stack pointer.

    ; RFLAGS. The interrupt flag is set, because a program that could not be
    ; interrupted could not be pre-empted and would own the machine. Bit 1 is set
    ; because the architecture reserves it as one — though the processor forces
    ; it whether or not it is written, which was established by clearing it here
    ; and observing that nothing changed. It is written for the reader rather
    ; than for the processor. Nothing else is set: the program begins with no
    ; carry, no direction, and an I/O privilege level of zero.
    push    qword 0x202
    push    rdx                     ; CS: the user code selector, RPL 3.
    push    rdi                     ; RIP: where the program begins.

    ; Every register the program has no business inheriting is cleared. What is
    ; left in a register at this moment is a kernel address as often as not, and
    ; handing one to a program that then prints it is a disclosure that no fault
    ; would report.
    xor     rax, rax
    xor     rbx, rbx
    xor     rcx, rcx
    xor     rdx, rdx
    xor     rsi, rsi
    xor     rdi, rdi
    xor     rbp, rbp
    xor     r8, r8
    xor     r9, r9
    xor     r10, r10
    xor     r11, r11
    xor     r12, r12
    xor     r13, r13
    xor     r14, r14
    xor     r15, r15

    iretq

; ------------------------------------------------------------------------------
; ThreadResumeUser(const SyscallFrame *frame, uint64_t code_selector,
;                  uint64_t stack_selector)
;
; Sub-task 6.11. Returns to privilege level 3 with a whole register set restored
; rather than with none, which is what a thread created by `fork` needs: it is
; not entered at an entry point but continues a program already running, at the
; instruction after its parent's SYSCALL and with its parent's registers.
;
; The arguments arrive in RDI, RSI and RDX. RDI names a SyscallFrame exactly as
; kernel/cpu/syscall_entry.asm builds one; the offsets below are asserted against
; the C structure by _Static_assert in kernel/proc/process.c.
;
; Why IRETQ and not SYSRET.
;
;   SYSRET is the shorter path and cannot be used here. It takes the address to
;   return to from RCX and the flags from R11, which is exactly what SYSCALL put
;   there — but a thread reaching this routine was placed here by a context
;   switch and not by a SYSCALL, so nothing has loaded those registers, and
;   loading them by hand would make the two the only registers of the set that
;   could not simply be restored. IRETQ takes both from the stack instead, so
;   every register in the frame is restored in the same way as every other, and
;   RCX and R11 arrive at privilege level 3 holding what the frame says they
;   should.
;
;   SYSRET is also refused a non-canonical address by raising a general
;   protection fault *in the kernel*, at privilege level 0; IRETQ faults with the
;   address on the stack and the fault belongs to the return. The frame here was
;   built by this kernel and neither case should arise, but where two
;   instructions differ in which privilege level absorbs a malformed value, the
;   one that does not absorb it into the kernel is the one to choose.
; ------------------------------------------------------------------------------

; The offsets of SyscallFrame, in the order syscall_entry.asm pushes them.
%define FRAME_R15        0
%define FRAME_R14        8
%define FRAME_R13        16
%define FRAME_R12        24
%define FRAME_R11        32
%define FRAME_R10        40
%define FRAME_R9         48
%define FRAME_R8         56
%define FRAME_RBP        64
%define FRAME_RDI        72
%define FRAME_RSI        80
%define FRAME_RDX        88
%define FRAME_RCX        96
%define FRAME_RBX        104
%define FRAME_RAX        112
%define FRAME_USER_STACK 120

global ThreadResumeUser

ThreadResumeUser:
    ; The data segment registers, before the stack segment, for the reason
    ; ThreadEnterUser gives above.
    mov     ax, dx
    mov     ds, ax
    mov     es, ax

    ; The five quadwords IRETQ pops, pushed in the reverse of the order it pops
    ; them. RIP is the frame's RCX, which is where SYSCALL put the address of the
    ; instruction after it; RFLAGS is the frame's R11, which is where SYSCALL put
    ; the caller's flags. The child therefore resumes at the instruction its
    ; parent will resume at, with the flags its parent had.
    push    rdx                         ; SS: the user stack selector, RPL 3.
    push    qword [rdi + FRAME_USER_STACK]
    push    qword [rdi + FRAME_R11]     ; RFLAGS, as SYSCALL preserved them.
    push    rsi                         ; CS: the user code selector, RPL 3.
    push    qword [rdi + FRAME_RCX]     ; RIP: after the parent's SYSCALL.

    ; And the registers. RDI is loaded last because it is the pointer every one
    ; of these is read through; loading it earlier would destroy the only means
    ; of reaching the rest.
    mov     rax, [rdi + FRAME_RAX]
    mov     rbx, [rdi + FRAME_RBX]
    mov     rcx, [rdi + FRAME_RCX]
    mov     rdx, [rdi + FRAME_RDX]
    mov     rsi, [rdi + FRAME_RSI]
    mov     rbp, [rdi + FRAME_RBP]
    mov     r8,  [rdi + FRAME_R8]
    mov     r9,  [rdi + FRAME_R9]
    mov     r10, [rdi + FRAME_R10]
    mov     r11, [rdi + FRAME_R11]
    mov     r12, [rdi + FRAME_R12]
    mov     r13, [rdi + FRAME_R13]
    mov     r14, [rdi + FRAME_R14]
    mov     r15, [rdi + FRAME_R15]
    mov     rdi, [rdi + FRAME_RDI]

    iretq
