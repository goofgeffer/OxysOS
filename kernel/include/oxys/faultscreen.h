/*
 * File: kernel/include/oxys/faultscreen.h
 * Purpose: Declares the graphical fault screens: the full-screen page drawn upon
 *          the framebuffer when the kernel stops, one composed for each severe
 *          fault rather than one shared by all of them.
 * Key definitions: FaultScreenShowException, FaultScreenShowPanic,
 *          FaultScreenDemonstrate, FaultScreenWasDrawn, FaultScreenEntryCount,
 *          FaultScreenEntryAt, FaultScreenEntry.
 * References:
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Chapter 6 and Table 6-1: the architecture-defined exceptions, their
 *     mnemonics, and which of them deliver an error code. Each screen's text is
 *     an account of what the processor is reporting, and is cited there.
 *   - docs/design/GRAPHICS.md, Sections 24 and 25: the design, and the assertion
 *     table for it.
 *
 * Why these exist, and why there is more than one of them.
 *
 * The kernel already reports every fatal fault in full upon the serial port and,
 * where the adapter is in a text mode, upon the display. Neither is available to
 * a person looking at a machine that the boot loader put into a graphics mode:
 * from sub-task 6.2 the text buffer is not displayed, and a serial adapter is
 * not something most machines have. Such a person previously saw the boot log
 * stop, and nothing else.
 *
 * They are one screen for each severe fault and not one screen for all of them
 * because the faults are not one thing. A page fault names an address and asks
 * what was expected to be mapped there; a general-protection fault names a
 * selector; an invalid opcode asks what the bytes at the instruction pointer
 * are; a double fault says that the processor could not deliver something else
 * and that the earlier fault is the one worth finding. A single screen carrying
 * a register dump would present all of that as an undifferentiated wall and
 * would tell the reader nothing about which question to ask.
 *
 * What is drawn is therefore chosen by the fault: its own title, its own colour,
 * a sentence saying what the processor is reporting, a sentence saying what to
 * examine first, and the evidence that bears upon that fault and not upon the
 * others.
 */

#ifndef OXYS_FAULTSCREEN_H
#define OXYS_FAULTSCREEN_H

#include <oxys/types.h>
#include <oxys/interrupts.h>

/*
 * The evidence a screen carries, beyond the instruction pointer and stack
 * pointer that every one of them shows.
 *
 * These are flags rather than a kind, because a fault may want more than one:
 * a stack-segment fault wants both the stack words and the selector its error
 * code names.
 */
#define FAULT_EVIDENCE_FAULT_ADDRESS UINT32_C(0x01) /* CR2, and the page-fault cause. */
#define FAULT_EVIDENCE_SELECTOR      UINT32_C(0x02) /* The selector-form error code. */
#define FAULT_EVIDENCE_OPCODE        UINT32_C(0x04) /* The bytes at the instruction. */
#define FAULT_EVIDENCE_STACK         UINT32_C(0x08) /* Quadwords at the stack pointer. */
#define FAULT_EVIDENCE_OPERANDS      UINT32_C(0x10) /* RAX, RCX, RDX — a divide's. */
#define FAULT_EVIDENCE_CONTROL       UINT32_C(0x20) /* CR0, CR3, CR4. */

/*
 * One fault's screen, as data rather than as code.
 *
 * It is a table because the alternative is a chain of conditionals in the one
 * routine that must not go wrong, and because a table can be asserted: the
 * self-test of Section 25 requires that no two of these share a title and that
 * none of them is missing a sentence, which is the fault a fourteenth entry
 * added in a later phase would otherwise arrive with.
 */
typedef struct FaultScreenEntry
{
    uint64_t vector;      /* The vector this describes; FAULT_SCREEN_SOFTWARE for a panic. */
    const char *title;    /* The banner. Short: it is drawn several times life size. */
    const char *mnemonic; /* The Intel mnemonic and vector number, drawn beside the title. */
    const char *meaning;  /* What the processor is reporting. */
    const char *examine;  /* What to look at first. Not a remedy: a direction. */
    uint8_t red;          /* The accent colour, as channel intensities, because the */
    uint8_t green;        /* framebuffer's encoding is not known until it is mapped */
    uint8_t blue;         /* and a table cannot hold an encoded pixel. */
    uint32_t evidence;    /* Which of the panels above this screen carries. */
} FaultScreenEntry;

/* The vector under which a panic raised by the kernel itself is filed. It is
 * outside the architecture-defined range deliberately: no processor raises it,
 * so it cannot collide with a real exception. */
#define FAULT_SCREEN_SOFTWARE UINT64_C(0x100)

/*
 * Draws the screen for an exception. Does nothing where the adapter is not in a
 * graphics mode, that being the case in which the display driver is already
 * showing the report.
 *
 * This is called after the full report has been written to the diagnostic path,
 * not instead of it. The screen is a summary for a person standing at the
 * machine; the serial log remains the record.
 */
void FaultScreenShowException(const TrapFrame *frame, uint64_t fault_address);

/*
 * Draws the screen for a panic the kernel raised itself — a condition it checked
 * for and found, rather than one the processor reported.
 *
 * The distinction is worth a screen of its own. An exception is the machine
 * saying something went wrong; a panic is this kernel saying it has found the
 * world in a state it does not know how to continue from, and the message names
 * the check that failed.
 */
void FaultScreenShowPanic(const char *message);

/* Whether a screen has already been drawn for the failure now in progress. The
 * panic path consults this so that a fault which has drawn its own detailed
 * screen is not overwritten by the general one a moment later. */
bool FaultScreenWasDrawn(void);

/*
 * Draws the screen for a vector from a composed trap frame, so that a screen may
 * be looked at without the fault it belongs to having occurred.
 *
 * This exists because the screens must be judged by eye — a page nobody has
 * looked at is a page whose text runs off the edge — and because raising the
 * real faults to do it is either impossible or unwise. A machine check cannot be
 * asked for; a double fault is raised by deliberately destroying the stack; an
 * invalid task state segment requires a malformed descriptor to be installed
 * first. Composing the frame shows the page and asserts nothing about the
 * processor, and the two are kept separate for that reason: the wiring from a
 * real fault to a real screen is proved by raising a real page fault, which is
 * safe, and is a different test.
 *
 * The values in the composed frame are recognisable rather than plausible, so
 * that nobody mistakes this page for the report of an actual fault.
 */
void FaultScreenDemonstrate(uint64_t vector);

/* The table, for the self-test. Nothing else has cause to walk it. */
size_t FaultScreenEntryCount(void);
const FaultScreenEntry *FaultScreenEntryAt(size_t index);

#endif /* OXYS_FAULTSCREEN_H */
