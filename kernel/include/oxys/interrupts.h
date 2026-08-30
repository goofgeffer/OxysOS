/*
 * File: kernel/include/oxys/interrupts.h
 * Purpose: Declares the trap frame, the uniform record of processor state that
 *          every interrupt and exception stub constructs, and the operations
 *          that install the stubs and receive control from them.
 * Key definitions: TrapFrame, InterruptInitialise, InterruptDispatch,
 *          InterruptVectorPushesErrorCode, InterruptCount, InterruptLastFrame.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 6.12.1 and Figure 6-4 (Stack Usage on Transfers to Interrupt and
 *     Exception-Handling Routines): in 64-bit mode the processor pushes SS, RSP,
 *     RFLAGS, CS and RIP unconditionally, whether or not a privilege change
 *     occurs, and aligns the stack pointer to a 16-byte boundary beforehand.
 *   - Intel SDM, Volume 3A, Section 6.13 (Error Code): for those exceptions that
 *     produce one, the error code is pushed last, nearest the handler. In 64-bit
 *     mode it is padded with zeros to eight bytes so that it may be popped like
 *     any other value.
 *   - Intel SDM, Volume 3A, Table 6-1 (Protected-Mode Exceptions and
 *     Interrupts): the vectors that push an error code.
 *   - System V ABI, AMD64 supplement, Section 3.2.3: the first integer argument
 *     is passed in RDI, which is how the frame pointer reaches the dispatcher.
 */

#ifndef OXYS_INTERRUPTS_H
#define OXYS_INTERRUPTS_H

#include <oxys/types.h>

/*
 * The state recorded upon entry to a handler, in ascending order of address.
 *
 * The order of the members is dictated by the order in which the stub pushes
 * them, and by the order in which the processor pushes its own frame. It must
 * not be rearranged without the corresponding change to
 * kernel/cpu/interrupt_stubs.asm; the two are a single interface expressed in
 * two languages, and a discrepancy would misreport every register.
 *
 * The general-purpose registers are pushed by the common stub, RAX first, so
 * that R15 occupies the lowest address. RSP is absent from that set: the value
 * the interrupted code held is recorded by the processor in the field of the
 * same name below, and pushing the register would record the stack pointer of
 * the handler instead.
 */
typedef struct TrapFrame
{
    /* Pushed by the common stub, in reverse of this order. */
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;

    /* Pushed by the per-vector stub. */
    uint64_t vector;

    /*
     * The error code, pushed by the processor for the vectors of Table 6-1 that
     * produce one, and by the stub as a zero for every other vector. The
     * uniformity is the entire purpose of the per-vector stubs: without it the
     * dispatcher would have to know, for every vector, where the frame began.
     */
    uint64_t error_code;

    /* Pushed by the processor. */
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} TrapFrame;

/*
 * The count of general-purpose registers the common stub preserves, and the size
 * of the frame the stub must discard before IRETQ. Both are asserted against the
 * structure in kernel/cpu/interrupts.c.
 */
#define TRAP_FRAME_REGISTER_COUNT 15U

/*
 * The signature of a handler. The frame is supplied by address and is not
 * const: a handler may alter the state to which control returns, which is how a
 * fault is corrected and how a system call will deliver its result. Any change
 * made to the frame is restored into the registers by the common stub and
 * becomes the state of the interrupted code.
 */
typedef void (*InterruptHandler)(TrapFrame *frame);

/*
 * Registers a handler for one vector, replacing any handler previously
 * registered for it. A vector for which no handler is registered is treated as
 * described in the commentary upon InterruptDispatch.
 *
 * name: a short description used in diagnostic output. The string is not copied
 *     and must therefore have static storage duration.
 */
void InterruptRegisterHandler(uint8_t vector, InterruptHandler handler,
                              const char *name);

/* Removes the handler registered for a vector, if any. */
void InterruptUnregisterHandler(uint8_t vector);

/* The handler registered for a vector, or NULL if none is registered. */
InterruptHandler InterruptRegisteredHandler(uint8_t vector);

/* The number of times the given vector has been dispatched. */
uint64_t InterruptVectorCount(uint8_t vector);

/*
 * The number of interrupts dispatched for which no handler was registered and
 * which were not fatal, being vectors outside the architecture-defined range.
 */
uint64_t InterruptUnhandledCount(void);

/* The mnemonic of an architecture-defined exception, or a reserved marker. */
const char *InterruptVectorName(uint64_t vector);

/*
 * Reports whether the processor pushes an error code for the given vector, per
 * Intel SDM, Volume 3A, Table 6-1. Declared here so that the same knowledge is
 * available to C as to the assembly stubs, and so that the two may be compared.
 */
bool InterruptVectorPushesErrorCode(uint64_t vector);

/*
 * Installs a gate for every one of the 256 vectors, each referring to the stub
 * for that vector. The interrupt descriptor table must have been initialised by
 * IdtInitialise before this is called.
 */
void InterruptInitialise(void);

/*
 * Receives control from the common stub with a pointer to the frame it
 * constructed. Not called directly by C; it is the callee of the stub.
 */
void InterruptDispatch(TrapFrame *frame);

/* The number of interrupts dispatched since initialisation. */
uint64_t InterruptCount(void);

/* A copy of the frame most recently dispatched, for inspection by the self-test. */
const TrapFrame *InterruptLastFrame(void);

/* Emits a summary of the installed stubs upon the console and the serial port. */
void InterruptReport(void);

/* Writes the contents of a trap frame to both output devices. */
void InterruptReportFrame(const TrapFrame *frame);

#endif /* OXYS_INTERRUPTS_H */
