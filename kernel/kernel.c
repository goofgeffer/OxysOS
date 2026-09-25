/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/kernel.c
 * Purpose: Contains the C entry point of the Oxys-OS kernel, KernelMain, which
 *          calls the phases of the boot in kernel/init/ in dependency order;
 *          and what every phase and every subsystem writes through: the
 *          diagnostic channel, the display's quiet and its mode, the command
 *          line, the panic, the halt, the boot and power screens, and the
 *          power call.
 * Key functions: KernelMain, KernelPanic, KernelHalt, KernelWriteString,
 *          KernelWriteHexadecimal, KernelWriteDecimal,
 *          KernelCommandLineHasOption, KernelServiceDisplay, KernelPower,
 *          KernelBootScreen, KernelDisplaySetMode, KernelSerialWriteTranslated.
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
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2B,
 *     "HLT": the instruction halts the processor until an interrupt, a debug
 *     exception, a non-maskable interrupt, or a reset occurs.
 *   - ECMA-48, Section 8.3.39 (ED, erase in display, parameter 2: the whole
 *     display) and Section 8.3.21 (CUP, cursor position, no parameters: the
 *     first position): what a form feed becomes upon the serial line.
 *   - docs/design/ARCHITECTURE.md: the dependency ordering that fixes
 *     the sequence of the phases.
 *
 * The order of initialisation is the substance of the boot, and it is not
 * arbitrary in a single place. Each subsystem is established only once
 * everything it reads exists, and each is asserted only once it is established;
 * where a test must be deferred past its own phase because it depends upon a
 * later one, that is said where the deferral occurs.
 *
 * Neither the phases nor the self-tests are here. The self-tests left at
 * sub-task 6.1, when this file had grown to some nine thousand lines, for
 * kernel/test/, one file per subsystem. The phases left on 2026-09-25, when it
 * had grown back to two thousand six hundred, for kernel/init/, one file per
 * phase; kernel/init/internal.h says why, and KernelMain below is the list of
 * them.
 */

#include <oxys/kernel.h>
#include "init/internal.h"
#include <oxys/test/verify.h>
#include <oxys/boot/bootinfo.h>
#include <oxys/syscall_abi.h>
#include <oxys/arch/cpu/percpu.h>
#include <oxys/arch/cpu/spinlock.h>
#include <oxys/arch/smp/ipi.h>
#include <oxys/fs/persist.h>
#include <oxys/dev/keyboard.h>
#include <oxys/dev/mouse.h>
#include <oxys/dev/io.h>
#include <oxys/dev/vga.h>
#include <oxys/gfx/framebuffer.h>
#include <oxys/gfx/graphics.h>
#include <oxys/gfx/compositor.h>
#include <oxys/gfx/font.h>
#include <logo.h>
#include <palette.h>
#include <oxys/gfx/console.h>
#include <oxys/gfx/cursor.h>
#include <oxys/gfx/faultscreen.h>
#include <oxys/gfx/client.h>
#include <oxys/gfx/window.h>
#include <oxys/dev/serial.h>

/*
 * Halts the processor permanently with interrupts masked. Execution does not
 * proceed beyond this function. The halt is placed within a loop because the
 * HLT instruction resumes execution upon a non-maskable interrupt or a system
 * management interrupt, neither of which is masked by the CLI instruction.
 */
_Noreturn void KernelHalt(void)
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
 * The mark of art/logo.h upon the boot screen and the power screen, drawn over
 * the ground the caller has already cleared the screen to. It is the owner's
 * artwork and needs no font.
 */
static void KernelDrawMark(GraphicsSurface *surface, int32_t centre_x, int32_t centre_y,
                           int32_t scale)
{
    const int32_t size = LOGO_UNITS * scale;
    const int32_t left = centre_x - (size / 2);
    const int32_t top = centre_y - (size / 2);

    /*
     * One screen pixel at a time, each mixed from the ground, the disc and the
     * ink in the proportions LogoSample gives for it. **The ground is mixed in
     * and not skipped**: a pixel on the edge of the disc is partly the ground,
     * and a mark that drew only where it covered wholly would have its edge
     * back as the staircase this table replaced. That is why the colour of
     * the ground is passed to LogoMix and why the caller must have cleared to
     * it — a mark drawn over anything else carries a fringe of the ground.
     *
     * A pixel at a time, and no faster. It is drawn twice in the life of a
     * machine — once upon the boot screen and once upon the page that says the
     * machine may be turned off — and a specialisation for that would be a
     * specialisation nobody could measure.
     */
    for (int32_t row = 0; row < size; ++row)
    {
        for (int32_t column = 0; column < size; ++column)
        {
            unsigned covered;
            unsigned inked;
            GraphicsRectangle pixel;

            LogoSample(column, row, size, &covered, &inked);

            if (covered == 0U)
            {
                continue;
            }

            pixel.x = left + column;
            pixel.y = top + row;
            pixel.width = 1;
            pixel.height = 1;

            GraphicsFillRectangle(
                surface, pixel,
                FramebufferEncode(
                    (uint8_t)LogoMix(OXYS_GROUND_RED, OXYS_DISC_RED, OXYS_INK_RED, covered, inked),
                    (uint8_t)LogoMix(OXYS_GROUND_GREEN, OXYS_DISC_GREEN, OXYS_INK_GREEN, covered,
                                     inked),
                    (uint8_t)LogoMix(OXYS_GROUND_BLUE, OXYS_DISC_BLUE, OXYS_INK_BLUE, covered,
                                     inked)));
        }
    }
}

/* A line of the scaled face, centred upon x, in ink upon paper. */
static void KernelDrawCentredText(GraphicsSurface *surface, int32_t centre_x, int32_t y,
                                  const char *text, int32_t scale, uint32_t ink, uint32_t paper)
{
    int32_t length = 0;
    int32_t x;

    for (const char *at = text; *at != '\0'; ++at)
    {
        ++length;
    }

    x = centre_x - ((length * (int32_t)FONT_WIDTH * scale) / 2);

    for (const char *at = text; *at != '\0'; ++at)
    {
        FontDrawGlyphScaled(surface, x, y, (uint8_t)*at, ink, paper, scale);
        x += (int32_t)FONT_WIDTH * scale;
    }
}

/*
 * The boot screen: the mark and a wordmark upon the slate ground, drawn to the
 * compositor's back buffer and presented once, since sub-task 9.3. The default
 * entry boots quiet — the screen shows nothing of the log — so until the
 * desktop drew there was nothing there; this is what a person sees while the
 * kernel starts, in place of it. It is not drawn upon the entries that give the
 * shell the screen, where the log or the prompt is what belongs there.
 */
void KernelBootScreen(void)
{
    GraphicsSurface *const surface = CompositorSurface();
    int32_t centre_x;
    int32_t centre_y;
    int32_t scale;
    const uint32_t ground = FramebufferEncode(OXYS_GROUND_RED, OXYS_GROUND_GREEN, OXYS_GROUND_BLUE);
    const uint32_t ink = FramebufferEncode(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE);
    const uint32_t dim = FramebufferEncode(OXYS_DIM_RED, OXYS_DIM_GREEN, OXYS_DIM_BLUE);

    if (surface == NULL)
    {
        return;
    }

    /*
     * The scale is a whole number of pixels to a layout unit, and is two upon
     * a screen wide enough for the mark at that size to leave room for the
     * words beneath it. The words are the bitmap face enlarged, and a
     * fractional scale would need a filter the face was not drawn for; the
     * mark is LOGO_UNITS units across at either scale, and art/logo.h is
     * drawn one to one at two and averaged down at one.
     */
    centre_x = (int32_t)surface->width / 2;
    centre_y = (int32_t)surface->height / 2;
    scale = ((int32_t)surface->width >= 1024) ? 2 : 1;

    GraphicsClear(surface, ground);
    KernelDrawMark(surface, centre_x, centre_y - (24 * scale), scale);
    KernelDrawCentredText(surface, centre_x, centre_y + (36 * scale), "OXYS-OS", 3 * scale, ink,
                          ground);
    KernelDrawCentredText(surface, centre_x, centre_y + (64 * scale),
                          "version " OXYS_VERSION_BANNER, scale, dim, ground);
    KernelDrawCentredText(surface, centre_x, (int32_t)surface->height - (40 * scale),
                          "starting the desktop", scale, dim, ground);

    CompositorInvalidateAll();
    CompositorPresent();
}

/*
 * The power call of sub-task 9.3, made by `init` alone: it stops the machine.
 *
 * SYSCALL_POWER_HALT draws a full-screen page saying the machine may be turned
 * off and halts every processor — the graphical counterpart of the fault
 * screen, drawn straight upon the framebuffer with the compositor suspended,
 * because the machine is stopping and the back buffer holds a desktop that is
 * no longer what should be shown. SYSCALL_POWER_REBOOT pulses the reset line of
 * the 8042 keyboard controller, which the IBM Personal Computer AT technical
 * reference assigns to bit 0 of the controller's output port and which command
 * 0xFE asserts for a few microseconds; the processor restarts from its reset
 * vector and this call does not return by any path.
 *
 * It does not return upon success. It returns SYSCALL_EINVAL for an action that
 * is neither, having done nothing, so that `init` learns of a mistake rather
 * than halting a machine upon one.
 */
int64_t KernelPower(uint64_t action)
{
    GraphicsSurface surface;
    const uint32_t ground = FramebufferEncode(OXYS_GROUND_RED, OXYS_GROUND_GREEN, OXYS_GROUND_BLUE);
    const uint32_t ink = FramebufferEncode(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE);
    const uint32_t dim = FramebufferEncode(OXYS_DIM_RED, OXYS_DIM_GREEN, OXYS_DIM_BLUE);

    if ((action != SYSCALL_POWER_HALT) && (action != SYSCALL_POWER_REBOOT))
    {
        return SYSCALL_EINVAL;
    }

    /* The other processors are stopped first, as a panic stops them, so that
     * none goes on drawing over the page or writing to a device mid-reset. */
    IpiHaltOtherProcessors();

    /*
     * The persistent `/etc` is written back and released before the machine
     * stops, so that the volume is marked cleanly unmounted and every edit is
     * upon the medium: docs/storage/PERSIST.md. Where nothing is
     * mounted over `/etc` the unmount is refused and nothing has changed.
     */
    (void)PersistRelease(PERSIST_ETC_POINT);

    CompositorSuspend();

    if (GraphicsSurfaceFromFramebuffer(&surface))
    {
        const int32_t centre_x = (int32_t)surface.width / 2;
        const int32_t centre_y = (int32_t)surface.height / 2;
        const int32_t scale = ((int32_t)surface.width >= 1024) ? 2 : 1;

        GraphicsClear(&surface, ground);
        KernelDrawMark(&surface, centre_x, centre_y - (24 * scale), scale);
        KernelDrawCentredText(&surface, centre_x, centre_y + (36 * scale),
                              (action == SYSCALL_POWER_REBOOT) ? "RESTARTING" : "OXYS-OS",
                              3 * scale, ink, ground);
        KernelDrawCentredText(
            &surface, centre_x, centre_y + (64 * scale),
            (action == SYSCALL_POWER_REBOOT) ? "the machine is restarting"
                                             : "it is now safe to turn off the machine",
            scale, dim, ground);
    }

    KernelWriteString(action == SYSCALL_POWER_REBOOT
                          ? "\nThe machine is restarting.\n"
                          : "\nIt is now safe to turn off the machine.\n");
    SerialFlush();

    if (action == SYSCALL_POWER_REBOOT)
    {
        /*
         * The reset line, pulsed through the keyboard controller. The status
         * register's bit 1 is the input-buffer-full flag; the command is
         * written only when it is clear, so that it is not lost behind a byte
         * the controller has not yet taken. A bounded wait, because a machine
         * with no controller must fall through to the halt rather than spin.
         */
        for (uint32_t attempt = 0U; attempt < 100000U; ++attempt)
        {
            if ((PortReadByte(0x64U) & 0x02U) == 0U)
            {
                PortWriteByte(0x64U, 0xFEU);
                break;
            }
        }
    }

    KernelHalt();
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

/*
 * Whether the display is silent, which is the default boot since sub-task 8.2.
 *
 * The boot log is some four hundred lines of self-test verdicts and device
 * reports, and it is written for a machine — `make verify` reads it off the
 * serial line — and for a person diagnosing a boot, who selects the
 * `diagnostics` entry of the menu to see it upon the screen. Everybody else
 * boots to a shell and should see a shell. **The serial channel is never
 * silenced**: it is the record, it is what the automated assertion reads, and a
 * bug report from a quiet boot is still a complete log. What this governs is
 * the two paths a person at the machine sees, the text-mode display and the
 * framebuffer console.
 *
 * It is lifted in two places: before the shell is started, because the shell's
 * output reaches the display through the same routine; and by KernelPanic,
 * because a machine that has stopped must say why upon whatever is in front of
 * the person, quiet or not.
 */
static bool KernelDisplayQuiet;

void KernelDisplaySetQuiet(bool quiet)
{
    KernelDisplayQuiet = quiet;
}

bool KernelDisplayIsQuiet(void)
{
    return KernelDisplayQuiet;
}

/* What the display is doing while the machine runs; init/internal.h gives the
 * modes. */
static KernelDisplayMode KernelDisplay;

void KernelDisplaySetMode(KernelDisplayMode mode)
{
    KernelDisplay = mode;
}

KernelDisplayMode KernelDisplayCurrentMode(void)
{
    return KernelDisplay;
}

/* Moves the pointer to where the mouse is, presenting only if it moved. */
static void KernelFollowMouse(void)
{
    const int32_t x = MouseX();
    const int32_t y = MouseY();

    if ((x != CursorX()) || (y != CursorY()))
    {
        CursorMoveTo(x, y);
    }
}

void KernelServiceDisplay(void)
{
    MouseEvent movement;
    KeyEvent key;

    if (KernelDisplay == KERNEL_DISPLAY_IDLE)
    {
        return;
    }

    if (KernelDisplay == KERNEL_DISPLAY_POINTER)
    {
        if (!MouseIsPresent() || !CursorIsAvailable())
        {
            return;
        }

        /*
         * Drained whole and moved once, as the echo loop does and for its
         * reason: the intermediate positions were never displayed. The
         * position is read from the driver rather than from the last event,
         * so that a buffer which overflowed still leaves the pointer where
         * the mouse actually is. The present is asked for only where the
         * pointer moved: CompositorPresent writes the framebuffer, and a tick
         * that wrote it a hundred times a second for a pointer standing still
         * would be paying for nothing.
         */
        while (MouseReadEvent(&movement))
        {
            (void)movement;
        }

        if ((MouseX() != CursorX()) || (MouseY() != CursorY()))
        {
            KernelFollowMouse();
            CompositorPresent();
        }

        return;
    }

    /*
     * The window manager: every movement is routed, not merely the last, so
     * that a press and its release within one tick both arrive; then, since
     * sub-task 9.2, every program asleep for an event is woken where any was
     * routed, so that it drains its queue and draws when it next runs; then
     * whatever changed is composed into the back buffer and carried out with
     * the pointer over it. A tick in which nothing moved, nothing was pressed
     * and nothing was drawn composes nothing and presents nothing, which is
     * most ticks. The windows' owners draw between ticks, from their own
     * calls; what they drew is composed at the next.
     */
    {
        const uint64_t routed_before = WindowManagerEventsRouted();

        while (MouseReadEvent(&movement))
        {
            WindowManagerHandleMouse(&movement);
        }

        while (KeyboardReadEvent(&key))
        {
            WindowManagerHandleKey(&key);
        }

        if (WindowManagerEventsRouted() != routed_before)
        {
            WindowClientWakeAll();
        }
    }

    {
        const GraphicsRectangle changed = WindowManagerCompose();

        if (!GraphicsRectangleIsEmpty(changed))
        {
            CompositorInvalidate(changed);
        }
    }

    if (CursorIsAvailable())
    {
        KernelFollowMouse();
    }

    CompositorPresent();
}



/*
 * Writes a string to the serial line, with each form feed translated to the
 * sequence a terminal at the far end clears its screen upon.
 *
 * A form feed clears the text-mode display and the console — a new page, which
 * upon a screen is the screen cleared, since 2026-09-16 for the shell's
 * `clear` — but a terminal emulator upon a serial line does not treat it so:
 * most print nothing and a few print a glyph. ECMA-48 gives the two sequences
 * every terminal of that lineage acts upon: ED with parameter 2, `CSI 2 J`,
 * "erase all of the display" (Section 8.3.39), and CUP with no parameters,
 * `CSI H`, the cursor to the first position (Section 8.3.21). The translation
 * is made here and not in the serial driver, which carries bytes and gives
 * them no meaning, and not in the display drivers, which do not know a
 * terminal is listening; the diagnostic path is the one place that writes to
 * all three and is therefore the one place that knows the same byte must mean
 * the same thing upon each.
 */
static void KernelSerialWriteTranslated(const char *string)
{
    for (const char *at = string; *at != '\0'; ++at)
    {
        if (*at == '\f')
        {
            SerialWriteString("\x1B[2J\x1B[H");
        }
        else
        {
            SerialPutCharacter(*at);
        }
    }
}

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

    /* The serial line is written whether or not the display is quiet: it is the
     * record, and the quiet is a courtesy to a person and not a change to what
     * the machine says of itself. */
    KernelSerialWriteTranslated(string);

    if (!KernelDisplayQuiet)
    {
        VgaWriteString(string);
        ConsoleWriteString(string);
        CompositorPresent();
    }

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

    /* A quiet display is quiet no longer: the machine has stopped and must say
     * why upon whatever is in front of the person. */
    KernelDisplaySetQuiet(false);

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
void KernelMain(uint32_t multiboot_information_address, uint32_t multiboot_magic)
{
    /*
     * The boot, one phase to a call, in the dependency order of
     * docs/design/ARCHITECTURE.md, Section 4. Each phase is in kernel/init/
     * and says there why its steps stand in the order they do; what is said
     * here is why the phases stand in this one where it is not the obvious
     * order of the phases' numbers.
     */
    KernelInitialiseEarly(multiboot_information_address, multiboot_magic);
    KernelInitialiseMemory();

    /* The display takes the framebuffer from the arena before anything else
     * fragments it, and so precedes everything that follows but memory. */
    KernelInitialiseDisplay();
    KernelInitialiseFrameReferences();

    KernelInitialiseInterrupts();
    KernelInitialiseDevices();
    KernelInitialiseProcesses();

    /* The APICs adopt every request line claimed so far, so they follow the
     * devices that claim them; the scheduler and the other processors follow. */
    KernelInitialiseProcessors();

    /* Phase 5 after the processors: the bus is enumerated once everything
     * driven so far is proved, so that its failures are reported through
     * channels already known to work (storage.c). */
    KernelInitialiseStorage();

    KernelVerifyUserland();
    KernelEnterSession();
}
