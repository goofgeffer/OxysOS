/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/terminal/terminal.h
 * Purpose: Declares the terminal input path of sub-task 8.1 — the one byte
 *          stream a program reads as its standard input, assembled from the
 *          keyboard's key events and the serial adapter's received characters,
 *          with the keys that produce no character of their own translated into
 *          the control sequences a terminal would send for them.
 * Key definitions: TERMINAL_QUEUE_CAPACITY, TerminalInitialise, TerminalInject,
 *          TerminalPoll, TerminalRead, TerminalWaitForInput, TerminalHasInput,
 *          TerminalFlush, TerminalBytesQueued, TerminalBytesDelivered,
 *          TerminalBytesDiscarded, TerminalKeysTranslated,
 *          TerminalAttachKeyboard, TerminalSequenceForScancode, TerminalReport.
 * References:
 *   - ECMA-48, 5th edition (1991), Section 5.4: the structure of a control
 *     sequence, CSI followed by parameter bytes and a final byte; and Sections
 *     8.3.18 (CUB), 8.3.19 (CUD), 8.3.20 (CUF) and 8.3.22 (CUU), the four
 *     cursor movements whose final bytes D, B, C and A a terminal's cursor keys
 *     send.
 *   - XTerm Control Sequences (Dickey), "PC-Style Function Keys": Home and End
 *     are CSI H and CSI F, and the VT220 editing keypad's Delete is CSI 3 ~.
 *     These are what every terminal emulator this project is tested through
 *     sends for those keys, so they are what the keyboard is made to send too.
 *   - IBM Personal Computer AT technical reference, scan code set 1: the codes
 *     of the extended keys translated below, each prefixed by 0xE0.
 *   - docs/design/SHELL.md, Section 2: why the translation is made here and not
 *     in the keyboard driver or in the program, and what a program may assume of
 *     the stream.
 *
 * What this is, and what it is not.
 *
 *   It is the thing a program's `read` of descriptor 0 reaches. Before this
 *   sub-task there was no such thing: the keyboard driver produced events and
 *   the serial driver produced characters, and the kernel's own echo loop was
 *   the only consumer of either. A shell needs one stream of bytes, in the order
 *   they were typed, whichever device they were typed at.
 *
 *   It is *not* a line discipline. Nothing here echoes, nothing edits, nothing
 *   waits for a newline: every byte is delivered as it arrives, and the program
 *   that reads it does the editing — which is the arrangement a terminal in raw
 *   mode has, and the only arrangement under which a line editor can move a
 *   cursor. A canonical mode, in which the kernel assembles lines and a program
 *   reads them whole, would need to be built in front of this one; nothing yet
 *   wants it, and docs/design/SHELL.md, Section 6, records what it would cost.
 *
 * Why the keys are translated to control sequences rather than to a code of this
 * kernel's own.
 *
 *   A byte stream must carry the cursor keys somehow, and the two choices are a
 *   private encoding or the one every terminal already uses. The private one
 *   would be a convention this kernel's programs alone understood, and a serial
 *   terminal — which sends what it sends — would then be a second dialect the
 *   editor had to parse anyway. Translating the keyboard into the terminal's own
 *   sequences means a program parses one dialect, and the same program behaves
 *   the same whether the person typing is at the machine or at the far end of
 *   a serial line.
 */

#ifndef OXYS_TERMINAL_TERMINAL_H
#define OXYS_TERMINAL_TERMINAL_H

#include <oxys/types.h>

/*
 * The capacity of the byte queue. A power of two, so that an index is reduced to
 * a subscript by a mask, as the keyboard's buffer is.
 *
 * It is larger than the keyboard's buffer because one key may become four
 * bytes, and because the self-test of sub-task 8.1 places an editing session's
 * worth of keystrokes here before the program that reads them is started.
 */
#define TERMINAL_QUEUE_CAPACITY 1024U

/* Empties the queue and the counters. Requires nothing: the devices it will
 * later draw upon are consulted by TerminalPoll and not here, so it may run
 * before either exists. */
void TerminalInitialise(void);

/*
 * Appends bytes to the queue, as if they had arrived from a device.
 *
 * This is the one entry by which bytes reach the queue — the keyboard's
 * translation and the serial drain both call it — and it is exposed for the
 * reason KeyboardProcessScancode is: the boot-time self-test places a session's
 * keystrokes here and then starts the program that reads them, upon a machine at
 * which nobody is typing.
 *
 * Bytes for which there is no room are discarded, and the discard is counted.
 * The newest are dropped rather than the oldest, for the reason the keyboard
 * driver gives: what was typed first is the beginning of a line, and a buffer
 * that dropped from the front would silently rewrite text a program had not yet
 * read.
 */
void TerminalInject(const char *bytes, size_t count);

/*
 * Moves whatever the keyboard and the serial adapter hold into the queue,
 * translating key events on the way. Returns the number of bytes appended.
 *
 * A release produces nothing. A depression that produces a character produces
 * that character, or its control counterpart where the control key is held. A
 * depression of an extended key that produces no character produces the control
 * sequence named in the references above, or nothing where the key is one no
 * sequence is assigned to.
 */
size_t TerminalPoll(void);

/*
 * Removes up to `capacity` bytes from the queue into `buffer`, after polling the
 * devices, and returns how many were removed — zero where the queue was empty.
 * It never waits; the caller that wants to wait calls TerminalWaitForInput
 * first.
 */
size_t TerminalRead(char *buffer, size_t capacity);

/*
 * Waits until at least one byte is available: yielding the processor to any
 * thread the run queue holds, and halting it — with interrupts enabled for the
 * duration and masked again upon return — only when the queue is empty.
 *
 * The halt is `sti; hlt` in a loop and not a spin, for the reason the kernel's
 * echo loop records: the two instructions together are the one idiom under
 * which an interrupt cannot arrive between the enable and the halt. The yield
 * before it is sub-task 8.6's: the caller is the `read` system call upon the
 * bootstrap processor, and since a pipeline's children share that processor a
 * reader that halted would halt them too. docs/design/SHELL.md, Sections 2.3
 * and 22.3, say why the reader yields rather than sleeping upon a channel.
 * Returns false, since sub-task 8.7, where a signal is pending upon the
 * caller, so that the read reports EINTR and the signal is delivered.
 */
bool TerminalWaitForInput(void);

/* Reports whether the queue holds at least one byte, polling the devices first. */
bool TerminalHasInput(void);

/* Discards every byte queued, and every event and character the devices hold. */
void TerminalFlush(void);

/* The number of bytes presently queued. */
size_t TerminalBytesQueued(void);

/* The bytes delivered to readers, the bytes discarded for want of room, and the
 * key events translated to a control sequence, since initialisation. */
uint64_t TerminalBytesDelivered(void);
uint64_t TerminalBytesDiscarded(void);
uint64_t TerminalKeysTranslated(void);

/*
 * Job control at the terminal, of sub-task 8.7.
 *
 * The foreground process group is the group control-C and control-Z are
 * delivered to — as SIGINT and SIGTSTP, the two bytes removed from the input
 * rather than delivered — and the only group whose members may read the
 * terminal without being stopped by SIGTTIN. Zero is no group, upon which the
 * two bytes are discarded. The shell sets it to each foreground job's group
 * and back to its own.
 *
 * TerminalService polls the devices and acts upon a control byte at the head
 * of the queue; it is called by the bootstrap processor's timer tick, so that
 * control-C reaches a program that never reads. Only the head of the queue is
 * looked at, for the reason terminal.c gives beside TerminalInterceptHead.
 * TerminalBytesIntercepted counts the bytes so removed, which a session
 * placed upon the terminal must add to what was delivered to account for
 * itself.
 */
uint64_t TerminalForegroundGroup(void);
void TerminalSetForegroundGroup(uint64_t group);
void TerminalService(void);
uint64_t TerminalBytesIntercepted(void);

/*
 * Whether the terminal reads the keyboard, since sub-task 9.1. It does until
 * told otherwise; the window manager, when it takes the screen, takes the
 * keyboard with it, and a terminal that went on translating keys into bytes
 * would hand every keystroke to two readers — the shell upon the serial line
 * and the window that holds the focus — of which each would act upon half.
 * The serial line is read regardless: it is the shell's whichever of the two
 * has the keyboard.
 */
void TerminalAttachKeyboard(bool attached);
bool TerminalKeyboardIsAttached(void);

/* Emits a summary upon both output devices. */
/*
 * The control sequence an extended scancode is translated into, or null where
 * it is translated into none. Exposed since sub-task 9.6 so that the self-test
 * can compare this table with the C library's copy of it — the copy a program
 * under MIT needs, not being able to link this — rather than compare each
 * against a third table of its own, which would drift with neither.
 */
const char *TerminalSequenceForScancode(uint8_t scancode);

void TerminalReport(void);

#endif /* OXYS_TERMINAL_TERMINAL_H */
