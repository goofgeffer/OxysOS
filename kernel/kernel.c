/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/kernel.c
 * Purpose: Contains the C entry point of the Oxys-OS kernel. It validates the
 *          state established by the boot loader, initialises in dependency order
 *          every subsystem the kernel presently has, runs the boot-time
 *          self-tests declared in <oxys/verify.h>, mounts a volume the machine
 *          carries at the root, and then either enters the echo loop, where a
 *          keyboard or a mouse is present, or halts the processor where neither
 *          is.
 * Key functions: KernelMain, KernelPanic, KernelHalt, KernelWriteString,
 *          KernelWriteHexadecimal, KernelWriteDecimal,
 *          KernelCommandLineHasOption, KernelMountRootVolume,
 *          KernelAttachPointer, KernelEchoLoop, KernelEchoBackspace,
 *          KernelSerialCursorToColumn.
 * References:
 *   - Multiboot2 Specification 2.0, Section 3.3 ("I386 machine state"): EAX
 *     contains 0x36D76289 and EBX the physical address of the Multiboot2
 *     information structure. Those values are preserved by boot/boot.asm and
 *     supplied as the arguments of this function.
 *   - Multiboot2 Specification 2.0, Section 3.6 ("Boot information format"): the
 *     information structure commences with a 32-bit total size followed by a
 *     32-bit reserved field, and is aligned on an 8-byte boundary.
 *   - System V Application Binary Interface, AMD64 Architecture Processor
 *     Supplement, Section 3.2.3: the first two integer arguments are passed in
 *     RDI and RSI respectively.
 *   - Intel 64 and IA-32 Architectures Software Developer\'s Manual, Volume 2B,
 *     "HLT": the instruction halts the processor until an interrupt, a debug
 *     exception, a non-maskable interrupt, or a reset occurs.
 *   - Intel SDM, Volume 2B, "STI": the instruction\'s effect upon the interrupt
 *     flag is delayed by one instruction, so that an interrupt cannot be
 *     delivered until after the instruction following it. This is what makes the
 *     sequence STI followed immediately by HLT free of the window in which a
 *     keyboard echo loop would otherwise service an interrupt and then halt with
 *     nothing left to wake it.
 *   - docs/design/ARCHITECTURE.md, Section 4: the dependency ordering that fixes
 *     the sequence of initialisation below, and with it the order of the phases.
 *
 * The order of initialisation is the substance of this file, and it is not
 * arbitrary in a single place. Each subsystem is established only once
 * everything it reads exists, and each is asserted only once it is established;
 * where a test must be deferred past its own phase because it depends upon a
 * later one, that is said where the deferral occurs.
 *
 * The self-tests themselves are not here. Until sub-task 6.1 they were, and this
 * file had grown to some nine thousand lines of which the entry point was the
 * last two hundred and fifty. They now stand in kernel/test/, one file per
 * subsystem, declared by <oxys/verify.h>; kernel/test/README.md records the
 * arrangement and the reason for it.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/bootinfo.h>
#include <oxys/pmm.h>
#include <oxys/paging.h>
#include <oxys/addrspace.h>
#include <oxys/vmm.h>
#include <oxys/heap.h>
#include <oxys/gdt.h>
#include <oxys/tss.h>
#include <oxys/syscall.h>
#include <oxys/idt.h>
#include <oxys/interrupts.h>
#include <oxys/exceptions.h>
#include <oxys/cpu.h>
#include <oxys/percpu.h>
#include <oxys/spinlock.h>
#include <oxys/ipi.h>
#include <oxys/shootdown.h>
#include <oxys/smp.h>
#include <oxys/sched.h>
#include <oxys/pic.h>
#include <oxys/irq.h>
#include <oxys/acpi.h>
#include <oxys/lapic.h>
#include <oxys/ioapic.h>
#include <oxys/pit.h>
#include <oxys/ps2.h>
#include <oxys/keyboard.h>
#include <oxys/mouse.h>
#include <oxys/vga.h>
#include <oxys/framebuffer.h>
#include <oxys/graphics.h>
#include <oxys/compositor.h>
#include <oxys/console.h>
#include <oxys/cursor.h>
#include <oxys/faultscreen.h>
#include <oxys/serial.h>
#include <oxys/pci.h>
#include <oxys/ata.h>
#include <oxys/ahci.h>
#include <oxys/sdhci.h>
#include <oxys/block.h>
#include <oxys/buffer.h>
#include <oxys/elf.h>
#include <oxys/process.h>
#include <oxys/ext2.h>
#include <oxys/vfs.h>
#include <oxys/ext2_vfs.h>

/*
 * Halts the processor permanently with interrupts masked. Execution does not
 * proceed beyond this function. The halt is placed within a loop because the
 * HLT instruction resumes execution upon a non-maskable interrupt or a system
 * management interrupt, neither of which is masked by the CLI instruction.
 */
static _Noreturn void KernelHalt(void)
{
    /*
     * The diagnostic channel is buffered once its interrupts are active, and the
     * halt below clears the interrupt flag permanently, so anything still queued
     * would never be carried. A machine that stops has usually just written the
     * one thing worth reading.
     */
    SerialFlush();

    for (;;)
    {
        __asm__ __volatile__("cli; hlt");
    }
}

/*
 * Converts an unsigned 64-bit value into its hexadecimal representation and
 * writes it, prefixed by "0x", to both output devices. A minimal conversion
 * routine is provided here because the formatted output facilities of the C
 * library are not implemented until Phase 7.
 */
void KernelWriteHexadecimal(uint64_t value)
{
    static const char HexadecimalDigits[] = "0123456789ABCDEF";

    /* Sixteen digits suffice for a 64-bit value, plus a null terminator. */
    char buffer[17];
    size_t index = sizeof(buffer) - 1U;

    buffer[index] = '\0';

    do
    {
        --index;
        buffer[index] = HexadecimalDigits[value & UINT64_C(0x0F)];
        value >>= 4;
    } while (value != 0U);

    /*
     * Emitted through KernelWriteString, which is the one place that knows how
     * many output paths there are.
     *
     * This named the display and the serial port itself until sub-task 6.4, and
     * the graphical console added there was therefore shown every word of the
     * boot log and not one of its numbers — a fault that looked like a
     * formatting error in the messages rather than like a missing output path.
     * Nothing below this line may name an output device.
     */
    KernelWriteString("0x");
    KernelWriteString(&buffer[index]);
}

/*
 * Writes an unsigned value in decimal to both output devices. The digits are
 * generated least significant first and therefore emitted from the end of the
 * buffer backwards.
 */
void KernelWriteDecimal(uint64_t value)
{
    /* Twenty digits suffice for the greatest 64-bit value, plus a terminator. */
    char buffer[21];
    size_t index = sizeof(buffer) - 1U;

    buffer[index] = '\0';

    do
    {
        --index;
        buffer[index] = (char)('0' + (unsigned char)(value % 10U));
        value /= 10U;
    } while (value != 0U);

    KernelWriteString(&buffer[index]);
}

/*
 * Writes a string to both the text console and the serial port, so that the
 * diagnostic record is complete irrespective of which device the operator is
 * observing.
 */
/*
 * The lock that governs the diagnostic channel.
 *
 * It exists from sub-task 6.14, which is the sub-task that gives this kernel a
 * second writer. Before it there was one flow of control and interleaving was
 * impossible; from it, a processor announcing that it has come online writes
 * through the same three drivers the bootstrap processor is writing through, and
 * a log whose lines have to be reassembled before they can be read is one whose
 * figures cannot be trusted.
 */
static Spinlock KernelDiagnosticLock = SPINLOCK_INITIALISER("diagnostic channel");

void KernelWriteString(const char *string)
{
    /*
     * Three paths, of which the operator can see at most two.
     *
     * The text-mode display and the graphical console are the same channel
     * addressed two ways: which of them is visible depends upon the mode the
     * boot loader left the adapter in, and neither knows about the other. Both
     * are written to unconditionally, because deciding between them here would
     * put the knowledge of the display mode in the one routine that must work
     * before anything has established what the mode is.
     *
     * The console records what it is given until it has a framebuffer to draw
     * upon, and replays it then, so the screen shows the boot from its first
     * line rather than from the middle.
     */
    /*
     * The three sinks are written, and the display is then carried out once.
     *
     * Until sub-task 6.6 this routine also concealed the pointer and revealed it
     * again, because the pointer kept the pixels beneath it and that store was
     * correct only while nothing else drew. It no longer keeps them: the console
     * draws into the back buffer, the pointer is a layer over it, and what is
     * beneath the pointer is simply still there. This routine has stopped
     * knowing that a pointer exists, which is what it should never have known.
     *
     * The presentation carries only what changed. A line of text is some tens of
     * character cells, so the cost is the cells and not the screen.
     *
     * The lock is the whole of the synchronisation sub-task 6.14 applies, and it
     * is applied here because this is the whole of what a started processor
     * touches: four unsynchronised structures — the text-mode display's cursor,
     * the console's rows, the serial adapter's transmit buffer and the
     * compositor's back buffer and damage rectangle — reached through one
     * function, so one lock covers all four. Each of those files' headers says
     * it is unlocked and names this as where the lock is taken.
     *
     * The section is a call and not a line. Composing a line before writing it
     * is therefore the caller's business, and the one caller that runs upon
     * several processors at once — SmpAnnounceArrival — does exactly that.
     *
     * **The lock is taken only once there is an area to take it through**, and
     * that condition is not a nicety. A spinlock acquire masks interrupts by way
     * of the per-processor area, which it reaches through GS.base; this routine
     * prints the banner, and the banner is printed before PerCpuInitialise has
     * run. Taking the lock unconditionally read address sixteen through a
     * segment base of zero, before the interrupt descriptor table existed, and
     * the machine reset with an empty log — which is the only symptom a fault in
     * the diagnostic channel can have.
     *
     * There is nothing to protect on that side of the line in any case. Before
     * the area exists no processor has been started and none can be, so there is
     * one writer by construction; the lock begins to mean something at exactly
     * the moment the mechanism it is built upon begins to work.
     */
    const bool locked = PerCpuIsEstablished();

    if (locked)
    {
        SpinlockAcquire(&KernelDiagnosticLock);
    }

    VgaWriteString(string);
    ConsoleWriteString(string);
    SerialWriteString(string);

    CompositorPresent();

    if (locked)
    {
        SpinlockRelease(&KernelDiagnosticLock);
    }
}

void KernelDiagnosticChannelReset(void)
{
    SpinlockInitialise(&KernelDiagnosticLock, "diagnostic channel");
}

void KernelPanic(const char *message)
{
    /*
     * The other processors are stopped before a word is printed.
     *
     * A machine that has failed keeps running everywhere else, and everywhere
     * else goes on modifying the structures this report is about — so a report
     * written while they run describes a machine that no longer exists by the
     * time anybody reads it. The request is made first for that reason, and it
     * is not waited for: a panic that waited could be stopped by the very
     * processors it is trying to stop.
     *
     * Upon a machine with one processor started it sends nothing, there being
     * nobody to tell.
     */
    IpiHaltOtherProcessors();

    /*
     * And the channel's lock is reset rather than waited for.
     *
     * A panic reaches this line from anywhere, including from inside
     * KernelWriteString's own critical section — a fault raised by the console
     * or by the compositor would do exactly that — and a ticket lock reacquired
     * by the processor already holding it does not deadlock loudly; it spins
     * until the bound of sub-task 6.13 fires and panics about the lock instead
     * of about the fault. It could equally be held by one of the processors just
     * halted, which will never release it.
     *
     * Both cases have the same answer, and it is the right one for the same
     * reason: after the halt request above, no processor but this one is going
     * to write anything, so there is nothing left for the lock to protect. The
     * report is the last thing this machine will do, and it must not be the
     * thing that stops it being written.
     */
    KernelDiagnosticChannelReset();

    VgaSetColour(VGA_COLOUR_WHITE, VGA_COLOUR_RED);
    KernelWriteString("\nKERNEL PANIC: ");
    KernelWriteString(message);
    KernelWriteString("\nThe system has been halted.\n");

    /*
     * The screen for a panic the kernel raised itself, and only where an
     * exception has not already drawn one of its own.
     *
     * Every fatal exception ends here, having first drawn the screen composed
     * for it. Drawing the general panic screen over that would replace an
     * account of the actual fault — its address, its selector, its instruction
     * bytes — with the sentence "an unrecoverable processor exception was
     * raised", which is the one thing the reader already knows.
     */
    if (!FaultScreenWasDrawn())
    {
        FaultScreenShowPanic(message);
    }

    KernelHalt();
}

/*
 * The parsed, boot-protocol-neutral description of the machine. It is held at
 * file scope rather than upon the stack because it is substantial, and the boot
 * stack is only 64 KiB.
 */
BootInformation KernelBootInformation;







/*
 * True if the boot loader's command line contains the stated option as a
 * complete word.
 *
 * The kernel has no other means of being told anything at the moment it starts,
 * and one thing it must be told is whether it is permitted to write to a disk.
 * Comparing complete words rather than substrings matters: an option is a
 * decision the operator made, and a decision must not be triggered by a longer
 * word that happens to contain it.
 */
bool KernelCommandLineOptionNumber(const char *option, uint64_t *value)
{
    const char *const line = KernelBootInformation.command_line;
    size_t position = 0U;

    if ((option == NULL) || (value == NULL))
    {
        return false;
    }

    while (line[position] != '\0')
    {
        size_t length = 0U;

        while ((line[position] == ' ') || (line[position] == '\t'))
        {
            ++position;
        }

        while ((option[length] != '\0') && (line[position + length] == option[length]))
        {
            ++length;
        }

        if ((option[length] == '\0') && (line[position + length] == '='))
        {
            size_t digit = position + length + 1U;
            uint64_t accumulated = 0U;
            bool any = false;

            while ((line[digit] >= '0') && (line[digit] <= '9'))
            {
                accumulated = (accumulated * 10U) + (uint64_t)(line[digit] - '0');
                any = true;
                ++digit;
            }

            /*
             * The value must end where the word does. "fault-screen=13x" is a
             * mistake, and reading it as thirteen would act upon a command line
             * its author did not write.
             */
            if (any && ((line[digit] == '\0') || (line[digit] == ' ') ||
                        (line[digit] == '\t')))
            {
                *value = accumulated;
                return true;
            }

            return false;
        }

        while ((line[position] != '\0') && (line[position] != ' ') &&
               (line[position] != '\t'))
        {
            ++position;
        }
    }

    return false;
}

bool KernelCommandLineHasOption(const char *option)
{
    const char *const line = KernelBootInformation.command_line;
    size_t position = 0U;

    while (line[position] != '\0')
    {
        size_t length = 0U;

        while ((line[position] == ' ') || (line[position] == '\t'))
        {
            ++position;
        }

        while ((option[length] != '\0') && (line[position + length] == option[length]))
        {
            ++length;
        }

        if ((option[length] == '\0') &&
            ((line[position + length] == '\0') || (line[position + length] == ' ') ||
             (line[position + length] == '\t')))
        {
            return true;
        }

        while ((line[position] != '\0') && (line[position] != ' ') && (line[position] != '\t'))
        {
            ++position;
        }
    }

    return false;
}













/*
 * Mounts a volume the machine actually carries at the root, and reports what it
 * holds.
 *
 * It is mounted read-only unless the operator booted the entry of the GRUB menu
 * that permits this kernel to write to their volumes. A kernel that mounted a
 * stranger's disk for writing would mark it as not cleanly unmounted merely by
 * having been booted, and every such disk would then demand a check before its
 * owner could mount it again — which is a real cost imposed for nothing.
 *
 * A machine carrying no volume is not in error. `make verify` runs upon one.
 */
static void KernelMountRootVolume(void)
{
    const bool writable = KernelCommandLineHasOption("ext2-write-test");

    VfsInitialise();

    if (!Ext2VfsInitialise())
    {
        KernelWriteString("VFS: the EXT2 filesystem could not be registered.\n");
        return;
    }

    if (!VfsMountRoot("ext2", !writable))
    {
        KernelWriteString("VFS: no volume was mounted at the root: ");
        KernelWriteString(VfsLastError());
        KernelWriteString("\n");
        return;
    }

    VfsReport();
    VfsReportDirectory("/");
    KernelVfsProbeVolume();
}

/*
 * Moves the cursor of a serial terminal to a column of the current line, the
 * column being counted from one, by the sequence ECMA-48 calls CHA — Cursor
 * Character Absolute, CSI Pn G. The display driver crosses a row boundary upon a
 * backspace by moving the cursor itself, which a terminal at the far end of a
 * serial line will not do upon receiving a backspace; the movement must
 * therefore be described to it.
 *
 * The two devices agree only so far as the terminal is eighty columns wide, the
 * kernel having no way to ask it. A wider or narrower terminal will have wrapped
 * the line elsewhere and the correction will land upon the wrong column of it.
 * The proper remedy is a line discipline that knows the width of its terminal,
 * which belongs to Phase 8.
 */
static void KernelSerialCursorToColumn(size_t column)
{
    /* Two digits suffice for a column of an eighty-column line. */
    char sequence[8];
    size_t index = 0U;
    const size_t number = column + 1U;

    sequence[index] = '\x1B';
    ++index;
    sequence[index] = '[';
    ++index;

    if (number >= 10U)
    {
        sequence[index] = (char)('0' + (unsigned char)(number / 10U));
        ++index;
    }

    sequence[index] = (char)('0' + (unsigned char)(number % 10U));
    ++index;
    sequence[index] = 'G';
    ++index;
    sequence[index] = '\0';

    SerialWriteString(sequence);
}

/*
 * Echoes a backspace upon both devices as an erasure.
 *
 * A backspace moves the cursor without erasing, upon the display and upon a
 * serial terminal alike, so an echo that wrote it alone would leave the
 * character the user meant to delete upon the screen and then overwrite it with
 * whatever was typed next. The erasure is the echo's business, not the driver's:
 * the sequence steps back, writes a space over the character, and steps back
 * again to stand where the character was.
 *
 * The display is driven first, and what it did then determines what is sent to
 * the serial line: the driver may have refused to move, the cursor standing at
 * the erase limit, or it may have crossed into the row above, which is a
 * movement the serial terminal must be told about explicitly.
 *
 * The graphical console is given the same three characters and left to reach its
 * own conclusion. It implements the same rules and keeps its own erase limit, so
 * it needs no direction from here; and it must not be given the serial
 * terminal's escape sequences, which are a property of a terminal and not of a
 * display. Its geometry differs from the text display's in any case, so the row
 * and column below are not its rows and columns and could not be used to steer
 * it if one wanted to.
 */
static void KernelEchoBackspace(void)
{
    size_t row;
    size_t column;
    size_t resulting_row;
    size_t resulting_column;

    VgaCursorPosition(&row, &column);
    VgaWriteString("\b \b");
    VgaCursorPosition(&resulting_row, &resulting_column);

    ConsoleWriteString("\b \b");

    if ((resulting_row == row) && (resulting_column == column))
    {
        /* The cursor stood at the erase limit; there was nothing to erase. */
        return;
    }

    if (resulting_row == row)
    {
        SerialWriteString("\b \b");
        return;
    }

    /* CUU, CSI A, moves the terminal's cursor up one line without erasing. */
    SerialWriteString("\x1B[A");
    KernelSerialCursorToColumn(resulting_column);
    SerialPutCharacter(' ');
    KernelSerialCursorToColumn(resulting_column);
}

/*
 * Echoes characters from the keyboard and from the serial line upon both output
 * devices, indefinitely.
 *
 * This is the one thing the self-tests cannot establish. They drive the decoder
 * directly, which exercises the whole of scan code set 1 upon a machine at which
 * nobody is typing, but leaves the path from the physical key to the decoder —
 * the controller raising its request line, the interrupt controller routing it,
 * the handler reading the data port — asserted only as configured state. The
 * same is true of the serial receiver, whose self-test can say what becomes of a
 * character that has arrived but not that one arrives. Here both paths are
 * exercised in full, by the only means available: a person, or a virtual machine
 * monitor, actually sending a character.
 *
 * The loop halts the processor between keystrokes rather than spinning. The
 * sequence STI followed immediately by HLT is the correct idiom and not merely a
 * compact one: Intel SDM, Volume 2B, "STI", provides that the effect of the
 * instruction is delayed by one instruction, so the HLT is executed before any
 * interrupt can be taken. Were the order reversed, or a further instruction
 * placed between them, a keystroke arriving in the interval would be serviced
 * and the processor would then halt with nothing left to wake it.
 */
/*
 * Gives the pointer a display, and tells the mouse how large that display is.
 *
 * Neither is done by either driver, and for the same reason in both directions.
 * The mouse has no idea what it is pointing at, so the bounds are told to it by
 * whoever knows the display; the pointer draws in pixel values, so the encoding
 * of black and white is supplied by whoever knows the framebuffer. This routine
 * is where those two pieces of knowledge meet, and it is in the entry point
 * because the entry point is what establishes both.
 *
 * A machine the boot loader left in a text mode has no surface to draw upon.
 * That is not a failure: the mouse still reports, its events are still decoded,
 * and nothing is drawn.
 */
static void KernelAttachPointer(void)
{
    const GraphicsSurface *const display = CompositorSurface();

    if (display == NULL)
    {
        return;
    }

    MouseSetBounds((int32_t)display->width, (int32_t)display->height);
    MouseSetPosition((int32_t)(display->width / 2U), (int32_t)(display->height / 2U));

    /*
     * Black within white. The two are chosen so that the pointer is visible upon
     * whatever it stands over: a single colour disappears against itself, and the
     * console draws light text upon a dark ground, which either alone would be
     * lost in.
     */
    (void)CursorInitialise(FramebufferEncode(0U, 0U, 0U),
                           FramebufferEncode(255U, 255U, 255U));

    CursorMoveTo(MouseX(), MouseY());
}

static _Noreturn void KernelEchoLoop(void)
{
    VgaSetColour(VGA_COLOUR_LIGHT_CYAN, VGA_COLOUR_BLACK);
    KernelWriteString("\nEcho loop. Characters typed upon the console or sent "
                      "upon COM1 appear upon both.\n");
    KernelWriteString("A backspace erases, and crosses to the line above.\n");
    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * Everything printed up to this point is the kernel's, and everything after
     * it is the user's. The erase limit records the boundary, which is what
     * permits the backspace to cross from one row to the row above: the driver
     * cannot tell the boot log from a line of input, and would otherwise consume
     * the log a character at a time.
     */
    VgaSetEraseLimit();
    ConsoleSetEraseLimit();

    /*
     * The pointer becomes visible here and not at its initialisation. Everything
     * above this line is the boot log being printed, and each of those lines
     * would conceal and reveal the pointer again — several hundred times, each
     * time reading back the pixels beneath it from write-combining memory, for a
     * pointer nobody is yet moving.
     */
    if (MouseIsPresent())
    {
        CursorShow();
        CompositorPresent();
    }

    for (;;)
    {
        char character;

        __asm__ __volatile__("sti; hlt");

        /*
         * The mouse's events are drained and the pointer moved once, not once per
         * event. A hundred packets arrive each second while the operator is
         * moving the mouse and the intermediate positions were never displayed;
         * drawing them would be paying for the erasing and redrawing of the
         * pointer a hundred times to show a path the eye cannot follow anyway.
         *
         * Where the position has not changed CursorMoveTo does nothing, which is
         * what makes it safe to call upon every packet — including the button
         * packets a stationary mouse continues to send.
         */
        {
            MouseEvent movement;

            while (MouseReadEvent(&movement))
            {
                /*
                 * The event is read for its side effect of emptying the buffer.
                 * The position it carries is the position the driver holds, and
                 * that is read below rather than from the last event, so that a
                 * buffer which overflowed still leaves the pointer where the
                 * mouse actually is.
                 */
                (void)movement;
            }

            CursorMoveTo(MouseX(), MouseY());

            /*
             * And carried out, here rather than only as a side effect of writing
             * text.
             *
             * This is what a movement costs since sub-task 6.6, and it is the
             * whole of it: the pointer's old rectangle and its new one, some
             * four hundred pixels, written once. It must be asked for explicitly
             * because moving the pointer is the one thing that changes the
             * display without anything being written to it — and the first form
             * of this loop did not ask, so a pointer moved across a silent
             * machine never appeared at all until somebody typed.
             */
            CompositorPresent();
        }

        /*
         * The serial line is a source of characters equally, now that its
         * receiver is interrupt-driven. Echoing them exercises the receive path
         * end to end, which the self-test cannot: it needs a character actually
         * arriving from outside the machine.
         */
        while (SerialReadCharacter(&character))
        {
            const char text[2] = { character, '\0' };

            if (character == '\b')
            {
                KernelEchoBackspace();
            }
            else
            {
                KernelWriteString(text);
            }
        }

        while (KeyboardReadCharacter(&character))
        {
            /* A one-character string, the output routines taking no other form. */
            const char text[2] = { character, '\0' };

            if (character == '\b')
            {
                KernelEchoBackspace();
            }
            else
            {
                KernelWriteString(text);
            }
        }
    }
}

void KernelMain(uint32_t multiboot_information_address, uint32_t multiboot_magic)
{
    /*
     * The serial port is initialised first, so that any subsequent failure is
     * recorded even should the display be unavailable or illegible. A negative
     * result is not an error; it indicates only that no adapter is present.
     */
    (void)SerialInitialise(SERIAL_COM1_PORT);

    VgaInitialise();

    VgaSetColour(VGA_COLOUR_LIGHT_CYAN, VGA_COLOUR_BLACK);
    KernelWriteString(OXYS_SYSTEM_NAME "\n");

    /*
     * The release, in the ordinal form of docs/project/VERSIONING.md, Section 3,
     * or the word "unreleased" where this image belongs to no release — which is
     * every image built so far.
     *
     * It said "Version 0.1.0" until the versioning scheme was written, naming a
     * release that had been withdrawn three days after it was published. A boot
     * banner is the one line of the log a person reads without being asked to,
     * and a version number in it that names nothing is worse than no number:
     * somebody would eventually cite it.
     */
    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);
    KernelWriteString("Release " OXYS_VERSION_STRING
                      ", x86_64, long mode active, higher-half kernel.\n");

    /*
     * The magic value is validated a second time here, the first validation
     * having been performed in 32-bit mode by boot/boot.asm. The repetition
     * guards against a transfer of control that bypasses the assembly entry
     * point, and costs nothing measurable.
     */
    if (multiboot_magic != MULTIBOOT2_BOOTLOADER_MAGIC)
    {
        KernelPanic("The boot loader is not Multiboot2 compliant.");
    }

    KernelWriteString("Multiboot2 magic value verified.\n");

    /*
     * The per-processor area of sub-task 6.13, established before anything that
     * could take a lock.
     *
     * It stands here, above the frame allocator and above everything else, for
     * one reason: a spinlock acquire reaches the area, and the allocators below
     * are the first structures a lock will ever be taken over. An area
     * established afterwards would leave every acquire before that point
     * reaching through a segment base of zero.
     *
     * It needs nothing to exist. The area is a static structure, the identifier
     * comes from CPUID and the segment base from a model-specific register; there
     * is no allocation to fail and nothing to parse. Sub-task 6.14 calls the same
     * function upon each application processor as it starts.
     */
    if (!PerCpuInitialise())
    {
        KernelPanic("This machine has more processors than the kernel reserves "
                    "per-processor areas for.");
    }

    /*
     * Reduce the Multiboot2 structure to the neutral description upon which the
     * remainder of the kernel depends. A failure here is unrecoverable: without a
     * memory map the physical frame allocator cannot be constructed, and without
     * that the kernel can do nothing further.
     */
    if (!BootInformationParseMultiboot2(multiboot_information_address,
                                        &KernelBootInformation))
    {
        KernelPanic("The Multiboot2 boot information structure could not be parsed.");
    }

    /*
     * The display is tested next, because it is the instrument through which
     * every later test reports.
     *
     * It was tested before the parse until sub-task 6.2, needing nothing the
     * handover had not already supplied. It cannot be any longer: from that
     * sub-task the boot loader may leave the adapter in a graphics mode, and
     * then the memory this test reads character cells back out of is not the
     * text buffer and nothing it asserts means anything. Which mode the machine
     * is in is stated by the boot information and nowhere else, so the test must
     * follow the parse in order to know whether to run at all.
     */
    KernelVerifyVga();

    BootInformationReport(&KernelBootInformation);

    PhysicalMemoryInitialise(&KernelBootInformation);
    PhysicalMemoryReport();
    KernelVerifyFrameAllocator();

    PagingInitialise(&KernelBootInformation);
    PagingReport();
    KernelVerifyPaging();

    KernelVirtualInitialise();
    KernelHeapInitialise();
    KernelVerifyAllocators();
    KernelVirtualReport();
    KernelHeapReport();

    /*
     * Phase 6, sub-task 6.2. The framebuffer the boot loader left the machine
     * with.
     *
     * It is acquired here, after the arena exists and before anything else
     * competes for it, because it is mapped out of the arena and its extent is
     * fixed by the hardware rather than chosen: a display of 1024 by 768 at four
     * bytes a pixel is three mebibytes of contiguous virtual address space, and
     * taking it first means taking it from a region nothing has fragmented.
     *
     * A false return is not a failure. It means the boot loader left the adapter
     * in a text mode, or described no display at all, and in either case the
     * VGA driver of sub-task 4.2 continues to own the screen. The report states
     * which it was.
     */
    (void)FramebufferInitialise(&KernelBootInformation);
    FramebufferReport();
    KernelVerifyFramebuffer();

    /*
     * Phase 6, sub-task 6.3. The primitives that draw upon it.
     *
     * They need nothing but the framebuffer above, and the greater part of what
     * they are asserted against is a surface composed in memory, so this runs
     * here rather than later: a fault in the arithmetic that computes a byte
     * offset into a surface is better found before anything else has drawn.
     */
    GraphicsReport();
    KernelVerifyGraphics();

    /*
     * Phase 6, sub-task 6.4. The console, which takes the screen.
     *
     * It is started after the drawing self-tests and not before, because it
     * clears the framebuffer and replays the boot log over it: started first, it
     * would be drawn upon by the figures those tests paint, and the log would be
     * unreadable. Started here, it erases them.
     *
     * That the two cannot coexist is why the figures are drawn only when the
     * boot loader's command line asks for them, and why this is then not started
     * at all. Whoever wants to look at the figures of sub-tasks 6.2 and 6.3 asks
     * for them and gives up the console for that boot; everybody else gets the
     * console, which is what a screen is for.
     */
    if (!KernelCommandLineHasOption("graphics-figure"))
    {
        /*
         * Sub-task 6.6. The compositor takes the display first, and the console
         * then draws into its back buffer rather than upon the framebuffer.
         *
         * The order is fixed by that: a console started first would hold a
         * surface describing the framebuffer, and every presentation would copy
         * the back buffer over the top of what it had drawn. Where the
         * compositor cannot be prepared — no framebuffer, or an arena that
         * cannot supply the pages — the console falls back to the framebuffer
         * and behaves as it did before this sub-task.
         */
        (void)CompositorInitialise();
        (void)ConsoleInitialise();
    }

    ConsoleReport();

    /*
     * The compositing primitives are asserted first, upon surfaces composed in
     * memory, and the compositor itself after: the second uses the first, and a
     * failure in the clip or the blend would otherwise be reported as a failure
     * of the compositor that merely called them.
     */
    KernelVerifyCompositing();
    KernelVerifyCompositor();
    CompositorReport();
    KernelVerifyConsole();
    KernelVerifyFaultScreen();

    FrameReferenceInitialise();
    KernelVerifyReferenceCounting();
    PhysicalMemoryReport();

    /*
     * The table established by boot/boot.asm resides at a low address that
     * sub-task 2.3 unmapped. It must be replaced before any interrupt gate is
     * installed, because delivering an interrupt obliges the processor to read
     * the descriptor named by the gate's selector.
     */
    GdtInitialise();
    GdtReport();

    IdtInitialise();
    InterruptInitialise();
    ExceptionInitialise();

    /*
     * Phase 6, sub-task 6.1. The apparatus of a privilege transition: the
     * user-mode descriptors, the task state segment that names the stacks the
     * processor loads, and the three registers that configure SYSCALL.
     *
     * It is established here, after the gates exist, and not beside the global
     * descriptor table it extends. LTR reads the descriptor this builds and
     * raises a general-protection exception where it is malformed; done before
     * the interrupt descriptor table existed, that exception would have found no
     * gate and escalated to a reset, and the diagnosis would have been a machine
     * that reboots. Done here it is reported.
     */
    TssInitialise();

    if (!ExceptionInstallInterruptStacks())
    {
        KernelPanic("The double fault could not be given a stack of its own.");
    }

    /*
     * A processor that cannot report SYSCALL cannot run a user program at all,
     * every one capable of long mode supporting it. The kernel proceeds so that
     * the machine may still be examined, and the report and the self-test both
     * state the absence.
     */
    (void)SyscallInitialise();

    /*
     * The global descriptor table reports itself above, at its initialisation;
     * every field of that report is a constant of the table's layout and none of
     * it changes when the task state segment descriptor is filled in, so it is
     * not repeated here.
     */
    TssReport();
    SyscallReport();
    IdtReport();
    InterruptReport();
    KernelVerifyIdt();
    KernelVerifyInterruptStubs();
    KernelVerifyDispatcher();
    KernelVerifyExceptions();
    KernelVerifyCopyOnWrite();
    KernelVerifyAddressSpaces();

    /*
     * The controllers are remapped only now, after the exception handlers exist.
     * Remapping them earlier would have placed device vectors clear of the
     * exceptions without providing anywhere for them to go.
     *
     * IrqInitialise performs the remapping and installs the routing above it. The
     * 8259A pair is what answers here and for the whole of the initialisation
     * that follows; sub-task 6.12 retires it in favour of the APIC only once
     * every driver has claimed the line it wants, which is after the serial
     * adapter is promoted to interrupts below.
     */
    IrqInitialise();
    KernelVerifyPic();
    KernelVerifyIrq();
    PicReport();
    IrqReport();

    /*
     * The timer is the first device to claim a request line, and therefore the
     * first proof that the whole path from a device to a handler is sound.
     */
    PitInitialise(PIT_DEFAULT_FREQUENCY);
    KernelVerifyPit();
    PitReport();

    /*
     * The 8042 controller, before either of the devices upon it.
     *
     * It is one device shared by two drivers, and its configuration byte governs
     * both ports and is written whole. Establishing it here, once, is what allows
     * the keyboard and the mouse to be drivers for their own devices rather than
     * two parties each reconfiguring a controller the other is using.
     */
    (void)Ps2Initialise();
    Ps2Report();

    /*
     * The keyboard is the last device of Phase 3. A machine without one is not
     * in error, so the return value is recorded rather than acted upon; the
     * report and the self-test both accommodate its absence.
     */
    (void)KeyboardInitialise();
    KernelVerifyKeyboard();
    KeyboardReport();

    /*
     * Sub-task 6.5: the mouse upon the controller's second port, and the pointer
     * drawn from it.
     *
     * Both are established here, among the devices, rather than with the drawing
     * of Phase 6 above. The reason is the interrupt flag: this is the last point
     * at which it is still clear, and a decoder self-test that composed packets
     * while a real mouse was delivering its own would be asserting upon a stream
     * it did not compose. The self-tests therefore run before anything sets it,
     * and the pointer is attached to the display afterwards.
     */
    (void)MouseInitialise();
    KernelVerifyMouse();

    /*
     * The pointer is attached before it is asserted, and that order is new at
     * sub-task 6.6.
     *
     * Until then the pointer's self-test composed a surface of its own and drew
     * upon it, so it needed nothing to have been attached. It now asserts the
     * rendering the compositor will actually draw from, which does not exist
     * until the pointer has a layer — and a test that ran first would report,
     * every time and correctly, that there was nothing to look at.
     */
    KernelAttachPointer();
    KernelVerifyCursor();
    MouseReport();
    CursorReport();

    /*
     * The apparatus of a privilege transition, and the system calls that will
     * arrive through it.
     *
     * The first asserts the configuration — the descriptors, the task state
     * segment's stacks, and the three registers that decide where SYSCALL goes
     * and what it clears. It executed SYSCALL until sub-task 6.7 and no longer
     * does; the reason is recorded where the routine that did it stood.
     *
     * The second asserts the dispatch and the validation of a caller's
     * arguments, which need no transition: the dispatcher is an ordinary
     * function of an ordinary structure. It composes a page accessible to
     * privilege level 3 in order to have something real to validate against, and
     * takes it away again, so it runs after the frame allocator and the paging
     * hierarchy are both established and asserted.
     */
    KernelVerifyPrivilege();
    KernelVerifySyscall();

    /*
     * The loader that will turn a file into a program. It composes an image in
     * memory and an address space to put it in, so it needs the frame allocator,
     * the paging hierarchy and the address spaces of Phase 2 — all of which are
     * established and asserted well above this line — and nothing of Phase 4.
     */
    KernelVerifyElf();

    /*
     * And the structures a loaded program will be held in. They are established
     * here rather than beside the memory manager because a process owns an
     * address space and a thread owns a kernel stack taken from the arena, so
     * both of those must exist and have been asserted first — and neither the
     * loader nor these structures needs anything of Phase 4.
     */
    ProcessInitialise();
    KernelVerifyProcess();

    /*
     * And the two transfers of sub-task 6.10: one thread exchanged for another,
     * and the first descent to privilege level 3.
     *
     * The switch is asserted first and alone, between two threads of the kernel,
     * because a failure there is a failure of the switch — where a failure in
     * the descent could be a failure of the switch, the loader, the address
     * space, the system call path or the exception dispositions, all of which
     * the descent puts together at once.
     *
     * Both run with the interrupt flag set, which is where it stands by this
     * point: a program that could not be interrupted could not be pre-empted,
     * and the descent sets the flag in the program's own RFLAGS regardless.
     */
    KernelVerifyContextSwitch();
    KernelVerifyUserMode();
    ProcessReport();

    /*
     * Phase 4 begins here. The serial adapter was configured in the first
     * instruction of this function, so that a failure anywhere above would be
     * recorded; only now, the interrupt controller existing, can it be promoted
     * from polling to interrupts and become a driver rather than a routine.
     */
    SerialActivateInterrupts();
    KernelVerifySerial();
    SerialReport();
    VgaReport();

    /*
     * Sub-task 6.12: the firmware's description tables, the two APICs, and the
     * retirement of the 8259A pair.
     *
     * It stands here, and not among the interrupt work of Phase 3, because of
     * what it needs on either side. It needs the kernel arena, which is why it
     * cannot precede Phase 2; and it needs every driver that will ever claim a
     * request line to have claimed it, because the adoption carries the claimed
     * lines across and a line claimed afterwards would have to be programmed by
     * a second path. The serial adapter, immediately above, is the last of them.
     *
     * The interrupt flag is clear throughout, as IrqAdoptApic requires: between
     * the masking of the 8259A and the programming of the redirection tables
     * there is no controller that would deliver a device's request, and one
     * raised in that interval would be lost.
     */
    (void)AcpiInitialise(&KernelBootInformation);
    KernelVerifyAcpi();
    AcpiReport();

    (void)LocalApicInitialise();
    KernelVerifyLocalApic();
    LocalApicReport();

    (void)IoApicInitialise();
    KernelVerifyIoApic();
    IoApicReport();

    (void)IrqAdoptApic();
    KernelVerifyApicRouting();
    PicReport();
    IoApicReport();
    IrqReport();

    /*
     * Sub-task 6.13: the locks, the per-processor data and the interrupt one
     * processor sends to another.
     *
     * The area itself was established before the frame allocator, a lock needing
     * it; what stands here is the half that needs the local controller. An
     * inter-processor interrupt is written into the controller's command
     * register and delivered through the same gate a device's request uses, so
     * neither the layer nor the shootdown above it can exist before the
     * controller is enabled and the routing has been adopted.
     *
     * The shootdown registers its handler after the layer that carries it, and
     * before anything may broadcast one — which upon a machine with one processor
     * is never, the broadcast returning at once for want of an audience. It is
     * nevertheless exercised in full below, by a request this processor addresses
     * to itself.
     */
    IpiInitialise();
    ShootdownInitialise();

    KernelVerifyPerCpu();
    KernelVerifySpinlock();
    KernelVerifyIpi();
    KernelVerifyShootdown();

    /*
     * Sub-task 6.15: the scheduler, prepared before the processors it will
     * schedule upon.
     *
     * It stands before SmpInitialise because a processor that comes online goes
     * straight into SchedulerEnterIdle, and there must be a tick handler
     * registered and a calibrated rate for it to find. The calibration is a busy
     * wait upon the interval timer, so it must stand after that timer is
     * running — and it runs upon the bootstrap processor alone, which at this
     * point in the boot it does by construction.
     *
     * A failure here is reported and survived. The machine then runs
     * unpre-empted, which is what it did until this sub-task, rather than upon a
     * quantum computed from a rate nothing measured.
     */
    if (!SchedulerInitialise())
    {
        KernelWriteString("Scheduler: the local timer could not be calibrated; "
                          "nothing will be pre-empted.\n");
    }

    /*
     * Sub-task 6.14: the application processors.
     *
     * It stands after everything above it because a starting processor is given
     * everything and takes nothing: a stack and a double-fault stack from the
     * kernel arena, a task state segment descriptor in a table already built,
     * the interrupt descriptor table already filled, and a per-processor area
     * from a reservation that exists. It needs the local controller enabled,
     * because it is started by a command written into that controller; the
     * inter-processor interrupt layer and the shootdown, because the first thing
     * done after the last processor is up is a shootdown addressed to all of
     * them; and the interval timer running, because the delays the startup
     * protocol prescribes are measured by polling its counter with interrupts
     * masked.
     *
     * It stands before the bus enumeration and the storage drivers so that the
     * whole of the remainder of the boot runs upon a machine that has more than
     * one processor, rather than upon one that acquires them at the end.
     */
    SmpInitialise();

    /*
     * And this processor's own timer, last.
     *
     * It is started after the bring-up rather than before it because the
     * bring-up masks interrupts for its whole duration and measures the
     * protocol's delays by polling the interval timer's counter. A local timer
     * running through that would deliver its ticks the moment the flag was
     * restored — a burst of pre-emptions against a processor that had just
     * finished starting the machine, for no purpose.
     */
    (void)SchedulerStartOnThisProcessor();

    PerCpuReport();
    IpiReport();
    ShootdownReport();
    SmpReport();

    KernelVerifyApplicationProcessors();
    KernelVerifyScheduler();

    /*
     * The scheduler's report comes after its self-test and not before it, which
     * is the other way round from every report above.
     *
     * The reason is that there is nothing to report until something has been
     * scheduled. The bring-up of sub-task 6.14 had done its work by the time
     * SmpReport ran; the scheduler has admitted nobody until its own test admits
     * somebody, so a report before it would print four zeroes and a queue length
     * of none — which reads exactly like a scheduler that does not work.
     */
    SchedulerReport();

    /*
     * The bus is enumerated once every device driven so far is working, so that
     * a failure in the enumeration is reported through channels already proved.
     * Nothing is claimed or configured here; the enumeration only establishes
     * what the machine contains, which the disk driver then searches.
     */
    (void)PciInitialise();
    KernelVerifyPci();
    PciReport();

    /*
     * The disk is the last device of this phase and the first whose failure is
     * silent in the ordinary case: a driver that reads the wrong sector returns
     * data, and data that arrived is indistinguishable from data that is right
     * until something tries to interpret it.
     */
    (void)AtaInitialise();
    KernelVerifyAta();
    AtaReport();

    /*
     * The same command set, reached the other way. Sub-task 4.4 drives an IDE
     * controller through I/O ports; a firmware that presents its serial ATA
     * controller in AHCI mode puts the disks behind memory-mapped registers that
     * driver cannot reach, and upon most machines made in the last fifteen years
     * that is where the disks are. The two run one after the other because a
     * machine may carry both, and each finds only what belongs to it.
     */
    (void)AhciInitialise();
    KernelVerifyAhci();
    AhciReport();

    /*
     * And the storage that is of neither class. An inexpensive laptop keeps its
     * system upon an embedded MultiMediaCard behind a host controller the
     * assignment specification classes as a system peripheral, and carries no
     * mass-storage controller whatever: neither driver above will ever find
     * anything upon such a machine, and no firmware setting would give them one.
     */
    (void)SdhciInitialise();
    KernelVerifySdhci();
    SdhciReport();

    /*
     * Every disk found presents itself through the generic layer, which is what
     * everything above will address it by. The layer is asserted against a
     * device of memory rather than against a disk: the machine this is verified
     * upon has no disk, and one that has holds data a self-test must not write.
     */
    (void)AtaRegisterBlockDevices();
    (void)AhciRegisterBlockDevices();
    (void)SdhciRegisterBlockDevices();
    KernelVerifyBlock();
    BlockReport();

    /*
     * The cache stands between the block layer and everything that will read a
     * medium. Its storage comes from the kernel heap, so it cannot be prepared
     * until that exists, which it has since Phase 2.
     */
    (void)BufferInitialise();
    KernelVerifyBuffer();
    BufferReport();

    /*
     * Phase 5 begins here. Nothing is mounted: the superblock of any volume the
     * machine actually carries is read and reported, and the parser itself is
     * asserted against a volume composed in memory.
     */
    KernelVerifyExt2();
    KernelReportVolumes();

    /*
     * Sub-task 5.8. The layer is asserted against the two volumes of memory it
     * composes, which is where a mount, a descriptor and a mount point crossing
     * can be stated exactly; only then is a volume the machine actually carries
     * mounted at the root, read-only unless the operator permitted otherwise.
     */
    KernelVerifyVfs();

    /*
     * Sub-task 6.11, and it stands here rather than beside the rest of Phase 6
     * for one reason: `execve` loads a program from a path, and a path leads
     * nowhere until a volume is mounted.
     *
     * The dependency is upon the line above and not upon the one below.
     * `KernelVerifyVfs` is what initialises the filesystem layer and registers
     * the EXT2 type; the lifecycle test then composes a volume of memory,
     * presents it as a device of its own, mounts it, writes the program it means
     * to execute, and withdraws all three before it returns — so it needs
     * nothing of the machine's own root volume, and `KernelMountRootVolume`
     * below would in any case find nothing upon a machine with no disk.
     *
     * The fork alone is asserted first and needs none of that, being an
     * operation upon two address spaces and a table; it is run here beside the
     * test it explains rather than three hundred lines above it, so that a
     * reader of the log meets the cheap assertion immediately before the
     * expensive one it makes interpretable.
     */
    KernelVerifyFork();
    KernelVerifyLifecycle();
    ProcessReport();

    /*
     * Sub-task 7.1, and the first assertion here whose subject is not the
     * kernel.
     *
     * The C library's string and memory functions are freestanding: they call
     * nothing, allocate nothing and depend upon nothing but the C language. They
     * are therefore placed at the end of the sequence rather than within it —
     * they have no dependency upon any subsystem above, and nothing above has
     * any dependency upon them, the kernel not being compiled against them at
     * all. Everything else in this function is ordered by what must exist
     * before it; this is ordered by what a reader of the log should meet last.
     */
    KernelVerifyString();

    KernelMountRootVolume();

    IrqReport();
    LocalApicReport();
    InterruptReport();
    PagingReport();
    AddressSpaceReport();

    VgaSetColour(VGA_COLOUR_LIGHT_GREEN, VGA_COLOUR_BLACK);
    /*
     * The words "initialisation complete." are what the `verify` target of the
     * Makefile greps the serial log for, and are therefore load-bearing: they
     * establish that the kernel reached the end of its initialisation rather
     * than faulting, hanging or resetting on the way. What follows them says
     * which sub-task the boot got as far as, and must be revised with the boot.
     */
    KernelWriteString("Phase 7 initialisation complete: a program has been loaded, "
                      "run at privilege level 3,\nhas made a child of itself, replaced "
                      "that child's program with one read from a\nvolume, collected "
                      "what it ended with, and ended; every device request is now "
                      "delivered\nby the I/O APIC and completed at the Local APIC, the "
                      "8259A pair having been retired;\nevery processor this machine "
                      "has holds an area of its own, takes locks that\nmask its "
                      "interrupts, answers a translation-lookaside-buffer shootdown "
                      "sent\nto it by another, and rotates between the threads upon a "
                      "run queue of its own;\nand the C library's string and memory "
                      "functions stand, asserted by the kernel\nbecause there is not yet "
                      "a userland to assert them in.\n");

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * The fault screens of sub-task 6.4, upon request.
     *
     * Two options, because there are two different things to establish and one
     * of them cannot be established safely.
     *
     * `fault-screen=<vector>` composes a trap frame and draws that vector's
     * page. It proves the page: that its text fits the display, that its panels
     * lay out one beneath another, that its colour and title are its own. It
     * proves nothing about the processor, and the frame it draws from is filled
     * with values no machine would produce so that a photograph of it cannot be
     * mistaken for a real report.
     *
     * `fault-raise` writes to an address that is not mapped, which raises a
     * genuine page fault. That proves the wiring — handler, report, screen — end
     * to end, and it is chosen because it is the one severe fault that can be
     * raised deliberately without endangering the machine. A double fault is
     * raised by destroying the stack, and a machine check cannot be asked for at
     * all.
     */
    {
        uint64_t demonstration = 0U;

        if (KernelCommandLineOptionNumber("fault-screen", &demonstration))
        {
            KernelWriteString("Fault screen demonstration for vector ");
            KernelWriteDecimal(demonstration);
            KernelWriteString(". No fault has occurred.\n");
            FaultScreenDemonstrate(demonstration);
            KernelHalt();
        }
    }

    if (KernelCommandLineHasOption("fault-raise"))
    {
        volatile uint64_t *const unmapped = (volatile uint64_t *)UINT64_C(0xFFFF900000000000);

        KernelWriteString("Raising a page fault deliberately, upon request.\n");
        *unmapped = 1U;
    }

    /*
     * With a keyboard the kernel has something to wait for, and waiting for it
     * demonstrates the interrupt path end to end. Without one there is nothing
     * further to do.
     */
    if (KeyboardIsPresent() || MouseIsPresent())
    {
        KernelEchoLoop();
    }

    KernelWriteString("No further subsystems are implemented. Halting.\n");

    KernelHalt();
}
