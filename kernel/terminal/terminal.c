/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/terminal/terminal.c
 * Purpose: Implements the terminal input path of sub-task 8.1: one queue of
 *          bytes, filled from the keyboard's events and the serial adapter's
 *          received characters, that a program's `read` of descriptor 0 drains.
 * Key functions: TerminalInitialise, TerminalInject, TerminalPoll, TerminalRead,
 *          TerminalWaitForInput, TerminalHasInput, TerminalFlush,
 *          TerminalBytesQueued, TerminalBytesDelivered, TerminalBytesDiscarded,
 *          TerminalKeysTranslated, TerminalReport.
 * References:
 *   - ECMA-48, 5th edition (1991), Section 5.4 and Sections 8.3.18 to 8.3.22:
 *     the control sequences the cursor keys are translated to. The header
 *     records which final byte is which.
 *   - XTerm Control Sequences (Dickey), "PC-Style Function Keys" and "VT220-Style
 *     Function Keys": CSI H and CSI F for Home and End, CSI 3 ~ for Delete.
 *   - IBM Personal Computer AT technical reference, scan code set 1: the
 *     extended codes 0x47 to 0x53 of the cursor and editing keys.
 *   - kernel/include/oxys/dev/keyboard.h: the event this consumes, and the
 *     modifier flags upon it.
 *   - docs/design/SHELL.md, Section 2: the design, and the reasons recorded in
 *     the header of this file's interface.
 *
 * Why the devices are polled by the reader and not drained by their interrupt
 * handlers.
 *
 *   The keyboard's handler and the serial adapter's handler each already place
 *   what arrived into a buffer of that driver's own, and each of those buffers
 *   is a single-producer, single-consumer ring that is correct without a lock.
 *   Draining them from within the handlers would make this queue a structure
 *   written by two interrupt handlers and read by a system call, which is three
 *   parties and a lock. Draining them from the reader makes this queue a
 *   structure touched by one flow of control at a time — the reader's — and
 *   leaves the interrupt handlers exactly as they were. The cost is that a byte
 *   is not here until something reads; nothing observes the difference, because
 *   the only thing that could observe it is the reader.
 *
 * Why the control key is applied here and not in the keyboard driver.
 *
 *   The driver records the modifiers in force and the character the key would
 *   produce without them, and that is the right record for a consumer that wants
 *   keys — the window system of Phase 9 will want to know that control and `c`
 *   were both held, not that byte 3 arrived. A byte stream has no room for the
 *   distinction, so it is collapsed here, at the boundary where keys become
 *   bytes, and the driver stays a decoder of scan code set 1 and nothing more.
 *
 * Concurrency. The queue is read and written by the flow of control that reads
 *   the terminal, and by TerminalInject, which the self-test calls from that
 *   same flow. No interrupt handler touches it. There is one reader — the `read`
 *   system call upon the bootstrap processor, to which every user thread is
 *   pinned — so no lock is taken; two readers would race upon the read index
 *   and each would receive part of what the other was owed. See
 *   docs/design/CONCURRENCY.md, Section 10, limitation 1.
 */

#include <oxys/terminal/terminal.h>

#include <oxys/kernel.h>
#include <oxys/dev/keyboard.h>
#include <oxys/dev/serial.h>
#include <oxys/proc/sched.h>
#include <oxys/arch/cpu/percpu.h>

/* The queue, and the two indices that are never reduced: their difference is the
 * number of bytes held, and each is reduced to a subscript by the mask. */
static char TerminalQueue[TERMINAL_QUEUE_CAPACITY];
static uint32_t TerminalWriteIndex;
static uint32_t TerminalReadIndex;

/* Accounting. */
static uint64_t TerminalDelivered;
static uint64_t TerminalDiscarded;
static uint64_t TerminalTranslated;

/* The extended scancodes of the keys given a control sequence. Scan code set 1,
 * each prefixed by 0xE0 upon the wire, the prefix being consumed by the driver. */
#define TERMINAL_SCANCODE_HOME   UINT8_C(0x47)
#define TERMINAL_SCANCODE_UP     UINT8_C(0x48)
#define TERMINAL_SCANCODE_LEFT   UINT8_C(0x4B)
#define TERMINAL_SCANCODE_RIGHT  UINT8_C(0x4D)
#define TERMINAL_SCANCODE_END    UINT8_C(0x4F)
#define TERMINAL_SCANCODE_DOWN   UINT8_C(0x50)
#define TERMINAL_SCANCODE_DELETE UINT8_C(0x53)

/*
 * What each of those keys becomes. CSI is ESC followed by `[`, and the final
 * byte is the one ECMA-48 assigns to the movement: A for CUU, B for CUD, C for
 * CUF, D for CUB. Home, End and Delete are xterm's, as the header records.
 */
static const char *TerminalSequenceFor(uint8_t scancode)
{
    switch (scancode)
    {
    case TERMINAL_SCANCODE_UP:
        return "\x1B[A";
    case TERMINAL_SCANCODE_DOWN:
        return "\x1B[B";
    case TERMINAL_SCANCODE_RIGHT:
        return "\x1B[C";
    case TERMINAL_SCANCODE_LEFT:
        return "\x1B[D";
    case TERMINAL_SCANCODE_HOME:
        return "\x1B[H";
    case TERMINAL_SCANCODE_END:
        return "\x1B[F";
    case TERMINAL_SCANCODE_DELETE:
        return "\x1B[3~";
    default:
        return NULL;
    }
}

static size_t TerminalQueued(void)
{
    return (size_t)(TerminalWriteIndex - TerminalReadIndex);
}

/* Appends one byte, or discards it where the queue is full. */
static bool TerminalAppend(char byte)
{
    if (TerminalQueued() >= (size_t)TERMINAL_QUEUE_CAPACITY)
    {
        ++TerminalDiscarded;

        return false;
    }

    TerminalQueue[TerminalWriteIndex & (TERMINAL_QUEUE_CAPACITY - 1U)] = byte;
    ++TerminalWriteIndex;

    return true;
}

void TerminalInitialise(void)
{
    TerminalWriteIndex = 0U;
    TerminalReadIndex = 0U;
    TerminalDelivered = 0U;
    TerminalDiscarded = 0U;
    TerminalTranslated = 0U;
}

void TerminalInject(const char *bytes, size_t count)
{
    if (bytes == NULL)
    {
        return;
    }

    for (size_t index = 0U; index < count; ++index)
    {
        (void)TerminalAppend(bytes[index]);
    }
}

/*
 * Translates one key event into the bytes it contributes, which may be none.
 *
 * A letter with control held becomes the control character the terminal would
 * send — the letter's code with its two high bits cleared, so that control-A is
 * byte 1 whether the letter arrived as `a` or, with shift or capitals lock, as
 * `A`. Nothing else is altered by control: a terminal sends control-1 as `1`,
 * and so does this.
 */
static size_t TerminalTranslate(const KeyEvent *event)
{
    if (!event->pressed)
    {
        return 0U;
    }

    if (event->character != '\0')
    {
        char byte = event->character;

        if ((event->modifiers & KEYBOARD_MODIFIER_CONTROL) != 0U)
        {
            const bool lower = (byte >= 'a') && (byte <= 'z');
            const bool upper = (byte >= 'A') && (byte <= 'Z');

            if (lower || upper)
            {
                byte = (char)(byte & 0x1F);
            }
        }

        return TerminalAppend(byte) ? 1U : 0U;
    }

    if (event->extended)
    {
        const char *const sequence = TerminalSequenceFor(event->scancode);
        size_t appended = 0U;

        if (sequence == NULL)
        {
            return 0U;
        }

        ++TerminalTranslated;

        for (size_t index = 0U; sequence[index] != '\0'; ++index)
        {
            if (TerminalAppend(sequence[index]))
            {
                ++appended;
            }
        }

        return appended;
    }

    return 0U;
}

size_t TerminalPoll(void)
{
    KeyEvent event;
    char character;
    size_t appended = 0U;

    /*
     * The keyboard is drained before the serial line and each is drained whole,
     * so the order of bytes from one device is the order they were typed in.
     * Between the two devices no order is promised: two people typing at once
     * upon two devices have no expectation the machine could meet.
     */
    while (KeyboardReadEvent(&event))
    {
        appended += TerminalTranslate(&event);
    }

    while (SerialReadCharacter(&character))
    {
        if (TerminalAppend(character))
        {
            ++appended;
        }
    }

    return appended;
}

size_t TerminalRead(char *buffer, size_t capacity)
{
    size_t removed = 0U;

    if ((buffer == NULL) || (capacity == 0U))
    {
        return 0U;
    }

    (void)TerminalPoll();

    while ((removed < capacity) && (TerminalQueued() > 0U))
    {
        buffer[removed] = TerminalQueue[TerminalReadIndex & (TERMINAL_QUEUE_CAPACITY - 1U)];
        ++TerminalReadIndex;
        ++removed;
    }

    TerminalDelivered += removed;

    return removed;
}

bool TerminalHasInput(void)
{
    (void)TerminalPoll();

    return TerminalQueued() > 0U;
}

void TerminalWaitForInput(void)
{
    while (!TerminalHasInput())
    {
        /*
         * Another thread that could run is given the processor first, since
         * sub-task 8.6. A program reading the terminal used to halt the
         * processor until an interrupt, which was right while it was the only
         * program; with a pipeline's children admitted to the rotation, a
         * shell that halted would halt them too, and `cat` reading the
         * terminal at the head of a pipeline would starve the command it
         * feeds. The yield returns at once where the queue is empty, and the
         * halt is taken only then. The reader stays runnable rather than
         * sleeping upon a channel, because the bytes arrive through an
         * interrupt handler and a wake performed there would be the first
         * thing in this kernel to enqueue from one — a cost paid at 8.7, when
         * a signal must interrupt a read, and not before.
         */
        if (SchedulerQueueLength(PerCpuIsEstablished() ? PerCpuIndex() : 0U) > 0U)
        {
            SchedulerYield();

            continue;
        }

        /* The idiom the kernel's echo loop records: the enable takes effect
         * after the halt has been entered, so a keystroke cannot fall between
         * the two and leave the processor halted with nothing to wake it. */
        __asm__ __volatile__("sti; hlt; cli");
    }
}

void TerminalFlush(void)
{
    KeyboardFlush();
    SerialFlushBuffers();
    TerminalReadIndex = TerminalWriteIndex;
}

size_t TerminalBytesQueued(void)
{
    return TerminalQueued();
}

uint64_t TerminalBytesDelivered(void)
{
    return TerminalDelivered;
}

uint64_t TerminalBytesDiscarded(void)
{
    return TerminalDiscarded;
}

uint64_t TerminalKeysTranslated(void)
{
    return TerminalTranslated;
}

void TerminalReport(void)
{
    KernelWriteString("Terminal: ");
    KernelWriteDecimal((uint64_t)TerminalQueued());
    KernelWriteString(" bytes queued of ");
    KernelWriteDecimal((uint64_t)TERMINAL_QUEUE_CAPACITY);
    KernelWriteString("; delivered ");
    KernelWriteDecimal(TerminalDelivered);
    KernelWriteString(", discarded ");
    KernelWriteDecimal(TerminalDiscarded);
    KernelWriteString(", keys translated to a control sequence ");
    KernelWriteDecimal(TerminalTranslated);
    KernelWriteString(".\n");
}
