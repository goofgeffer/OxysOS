; SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
; SPDX-License-Identifier: MIT
;
; File: libc/crt/crt0.asm
; Purpose: The C runtime startup object — the first instructions of every program
;          this system runs. It takes the argument count, the argument vector and
;          the environment vector from the stack the kernel prepared, calls main
;          with them, and passes what main returned to exit.
; Key definitions: _start.
; References:
;   - System V Application Binary Interface, AMD64 supplement, Section 3.4.1,
;     "Stack State": the initial process stack — the argument count at %rsp, the
;     argument pointers at 8+%rsp, the null pointer ending them at
;     8+8*argc+%rsp, the environment pointers after that and the auxiliary
;     vector after those; %rsp "is guaranteed to be 16-byte aligned at process
;     entry"; %rbp is unspecified and "the user code should mark the deepest
;     stack frame by setting the frame pointer to zero"; and "when main()
;     returns its value is passed to exit()".
;   - System V ABI, AMD64 supplement, Section 3.2.2: the end of the input
;     argument area is aligned on a 16-byte boundary, so %rsp+8 is a multiple of
;     16 when control is transferred to a function's entry point.
;   - System V ABI, AMD64 supplement, Section 3.2.3: the first three integer
;     arguments are passed in %rdi, %rsi and %rdx.
;   - kernel/include/oxys/proc/process.h: PROCESS_USER_STACK_FRAME_BYTES, which
;     is the kernel's half of this contract — the six eightbytes it leaves upon
;     the stack so that the reads below reach mapped memory.
;   - docs/design/LIBC.md, Section 11.2: why this is assembly, what it does not
;     do, and what a program may assume before its first statement.
;
; Why this is assembly and not C.
;
;   A C function cannot read its own stack pointer, and that is the whole of what
;   this file does. It is also entered with no return address upon the stack —
;   there is nowhere to return to — so a compiler's prologue and its epilogue
;   would both be wrong: the epilogue would execute a RET against the argument
;   count.
;
; What this deliberately does not do.
;
;   No constructors are called. There is no .init_array walk and no __libc_init,
;   because nothing in this library has a constructor: the three standard streams
;   are initialised statically, the heap initialises itself upon its first
;   request, and errno is an object in .bss. A startup object that walked a
;   section nothing puts anything into would be machinery whose correctness
;   nothing could demonstrate. It arrives with the first thing that needs it,
;   which is a ported library with static objects of class type.
;
;   The function pointer the ABI leaves in %rdx is not registered with atexit.
;   Section 3.4.1 says an application "should" register it, and it is there so
;   that a dynamic loader may arrange for a shared object to be finalised. This
;   system has no dynamic loader and no shared objects, and this kernel enters a
;   program with every register zero — so the pointer is null, and registering a
;   null pointer is what atexit refuses. It is ignored explicitly rather than by
;   omission, and this paragraph is the record of the decision.

bits 64

section .text.entry progbits alloc exec nowrite align=16

global _start
extern main
extern exit

_start:
    ; The deepest stack frame is marked by a null frame pointer, which Section
    ; 3.4.1 asks of user code: a debugger walking the chain of saved frame
    ; pointers needs something to stop at, and an unspecified %rbp is a chain
    ; that ends wherever the last program left a plausible value.
    xor     rbp, rbp

    ; argc, from the eightbyte the stack pointer holds.
    mov     rdi, [rsp]

    ; argv, which begins at the next eightbyte. The vector is not copied and
    ; nothing about it is checked: it is the kernel's, it is mapped, and its
    ; terminator is the kernel's to place.
    lea     rsi, [rsp + 8]

    ; envp, which follows the argument vector and its null terminator — so it
    ; begins argc+1 eightbytes above argv. Written as a single address
    ; computation because that is what the arithmetic of Section 3.4.1 is:
    ; 8+8*argc+%rsp is the terminator, and the eightbyte after it is envp.
    lea     rdx, [rsi + rdi*8 + 8]

    ; The stack pointer is already 16-byte aligned, Section 3.4.1 guaranteeing
    ; it and the kernel's frame being a multiple of sixteen. It is aligned again
    ; anyway, and the reason is that this instruction is free and the failure it
    ; prevents is not: a misaligned stack faults at the first aligned move inside
    ; some function the program did not write, and nothing about that fault names
    ; the stack. The AND clears at most four bits and is a no-operation whenever
    ; the guarantee holds.
    and     rsp, -16

    ; The call pushes a return address, so %rsp+8 is a multiple of 16 at main's
    ; entry point, which is what Section 3.2.2 requires of every call.
    call    main

    ; "When main() returns its value is passed to exit()" — Section 3.4.1. The
    ; value is in %eax, and it is moved as a 32-bit quantity because main returns
    ; an int: moving %rax would carry whatever main left in the upper half into a
    ; status the parent reads.
    mov     edi, eax
    call    exit

    ; exit does not return. This is here so that a program which somehow came
    ; back faults at an instruction belonging to it, rather than executing
    ; whatever the linker placed next — which is the same reasoning
    ; kernel/test/program.c gives for emitting UD2 after a call that must not
    ; return.
    ud2
