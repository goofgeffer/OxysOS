/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/session.c
 * Purpose: The end of the boot: the completion banner, then the screen given
 *          to the window manager and `init`, or to the shell, or to the echo
 *          loop, as the menu entry and the machine decide.
 * Key functions: KernelEnterSession, KernelDesktopEntry.
 * References:
 *   - docs/design/ARCHITECTURE.md, Section 4: the dependency order that fixes
 *     where this phase stands in KernelMain, and the order within it.
 *   - Intel SDM, Volume 2B, "STI": the effect upon the interrupt flag is
 *     delayed by one instruction, which makes STI followed by HLT in the echo
 *     loop free of the window in which a keystroke is serviced and the
 *     processor then halts with nothing left to wake it.
 *   - ECMA-48, Section 8.3.9 (CHA, cursor character absolute): how the echo
 *     loop moves a serial terminal's cursor back across a row it has wrapped.
 *
 * Moved out of kernel/kernel.c on 2026-09-25, unchanged in order, when
 * KernelMain was reduced to the driver that calls one function per phase;
 * kernel/init/internal.h says why.
 */

#include "internal.h"
#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/dev/keyboard.h>
#include <oxys/dev/mouse.h>
#include <oxys/terminal/terminal.h>
#include <oxys/dev/vga.h>
#include <oxys/gfx/framebuffer.h>
#include <oxys/gfx/graphics.h>
#include <oxys/gfx/compositor.h>
#include <palette.h>
#include <oxys/gfx/console.h>
#include <oxys/gfx/cursor.h>
#include <oxys/gfx/window.h>
#include <oxys/dev/serial.h>
#include <oxys/exec/elf.h>
#include <oxys/proc/process.h>
#include <oxys/fs/vfs.h>

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

/* Where the shell stands upon the root, and the name its process carries. */
#define KERNEL_SHELL_PATH "/bin/sh"
#define KERNEL_SHELL_NAME "sh"

/*
 * Whether this is the entry that gives the window manager the screen: the
 * default one, which names no option at all. It is asked twice — once before
 * the banner, to keep the screen clear for the boot screen, and once where the
 * screen is given out — and is a function rather than a variable because the
 * command line is the only thing it depends upon and that cannot change.
 */
bool KernelDesktopEntry(void)
{
    return !KernelCommandLineHasOption("diagnostics") &&
           !KernelCommandLineHasOption("shell-only") &&
           !KernelCommandLineHasOption("graphics-figure");
}

/* Where `init` of sub-task 9.3 stands, and its name. It is the first user
 * process, and it starts and supervises the desktop and collects the orphans;
 * the kernel starts it and no other program, the window demonstration of 9.2
 * being `init`'s to run since 9.3. */
#define KERNEL_INIT_PATH "/bin/init"
#define KERNEL_INIT_NAME "init"

/*
 * Gives the window manager the screen: the compositor's back buffer, the
 * palette, and the framebuffer's encoding for a client's pixels. Returns false
 * where there is no compositor.
 *
 * The palette, which docs/design/WINDOWS.md records: a dark slate
 * ground, a warm white paper, one blue accent for the band that holds the
 * focus and a quiet grey for the bands that do not. It is here because the
 * entry point is what knows the framebuffer's encoding, as it is for the
 * pointer's two colours.
 */
static bool KernelStartWindowManager(void)
{
    GraphicsSurface *const screen = CompositorSurface();
    WindowPalette palette;

    if (screen == NULL)
    {
        return false;
    }

    palette.ground = FramebufferEncode(OXYS_GROUND_RED, OXYS_GROUND_GREEN, OXYS_GROUND_BLUE);
    palette.paper = FramebufferEncode(OXYS_PAPER_RED, OXYS_PAPER_GREEN, OXYS_PAPER_BLUE);
    palette.border = FramebufferEncode(OXYS_BORDER_RED, OXYS_BORDER_GREEN, OXYS_BORDER_BLUE);
    palette.title_focused = FramebufferEncode(OXYS_BAR_RED, OXYS_BAR_GREEN, OXYS_BAR_BLUE);
    palette.title_unfocused = FramebufferEncode(OXYS_BAR_QUIET_RED, OXYS_BAR_QUIET_GREEN, OXYS_BAR_QUIET_BLUE);
    palette.text_focused = FramebufferEncode(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE);
    palette.text_unfocused = FramebufferEncode(OXYS_DIM_RED, OXYS_DIM_GREEN, OXYS_DIM_BLUE);

    return WindowManagerInitialise(screen, &palette, FramebufferEncode);
}

/*
 * Gives the screen back to the console, before anything is written upon it
 * rather than after.
 *
 * The desktop entry quiets the console, hands the framebuffer to the window
 * manager, and draws the boot screen upon it. Every path that then gives up —
 * an `init` that could not be started, a shell that could not be started, a
 * shell that ended by a fault — wrote its reason first and reclaimed the
 * screen afterwards. The reason therefore went to the serial line alone, and
 * the screen kept a boot screen that nothing was drawing any longer. The echo
 * loop beneath then wrote into it from wherever the console's cursor had
 * stopped when it went quiet, taking out one row of the mark for each line.
 *
 * **Reported from a laptop on 2026-09-21**: the boot screen standing frozen,
 * black lines eating into it a row at a time as the reporter typed, and
 * nothing anywhere saying what had happened. Each of those was the correct
 * behaviour of a part that had not been told the screen had changed hands —
 * which is the shape of every defect that looks inexplicable from the front.
 *
 * So, in this order: the keyboard returns to the terminal, the window manager
 * having taken it; the tick stops composing, there being nothing left to
 * compose; the pointer is hidden, a pointer that no longer follows the hand
 * being worse than none; the quiet is lifted, so that what follows is read by
 * the person in front of the machine and not only by whoever holds the serial
 * line; and the console is cleared, which resets its cursor and its erase
 * limit so that the next line is written at the top of an empty screen.
 *
 * It does nothing where the console already has the screen: a shell's log upon
 * it is not something to erase. That also makes it idempotent, which is what
 * lets every path that gives up call it without knowing what the others did.
 */
static void KernelScreenBackToConsole(void)
{
    if (KernelDisplayCurrentMode() != KERNEL_DISPLAY_WINDOWS)
    {
        return;
    }

    TerminalAttachKeyboard(true);
    KernelDisplaySetMode(KERNEL_DISPLAY_IDLE);

    if (MouseIsPresent())
    {
        CursorHide();
    }

    KernelDisplaySetQuiet(false);
    ConsoleClear();
    CompositorPresent();
}

/*
 * Starts a program from the root and does not wait for it, of sub-task 9.2:
 * what KernelRunShell does up to the start, and then ThreadLaunch in place of
 * ThreadStart. Returns the new process's identifier, or zero where it could
 * not be started. Since sub-task 9.3 this starts one program, `init`, whose
 * children collect one another and whose own ending is the machine's; a
 * program `init` launches has `init` for a parent and is collected.
 */
static uint64_t KernelLaunchProgram(const char *path, const char *name)
{
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    ElfImage image;
    uint64_t stack;
    size_t length = 0U;

    while (name[length] != '\0')
    {
        ++length;
    }

    arguments.argument_count = 1U;
    arguments.environment_count = 0U;
    arguments.argument[0] = 0U;
    arguments.storage_used = (uint32_t)(length + 1U);

    for (size_t index = 0U; index <= length; ++index)
    {
        arguments.storage[index] = name[index];
    }

    process = ProcessCreate(name, NULL);

    if (process == NULL)
    {
        return 0U;
    }

    if (ElfLoadFile(&process->space, path, &image) != ELF_OK)
    {
        ProcessDestroy(process);

        return 0U;
    }

    ProcessRecordImage(process, &image);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);

        return 0U;
    }

    thread = ThreadCreate(process, image.entry, stack);

    if ((thread == NULL) || !ThreadLaunch(thread))
    {
        ProcessDestroy(process);

        return 0U;
    }

    return process->id;
}

/*
 * Runs the shell from the root filesystem, at privilege level 3, and returns
 * true when it ends, with its status; or false, having changed nothing, where
 * it could not be started at all.
 *
 * This is sub-task 8.1's one addition to the entry point, and it is what the
 * whole of Phase 7 was built towards: a program read from a volume, entered
 * with an argument vector, reading a terminal and writing a console, that a
 * person can type at. The procedure is the one every self-test that runs a
 * program performs — create, load, stack, thread, start — and it is repeated
 * here rather than shared, for the reason kernel/test/storage/initrd.c gives:
 * a helper shared between a test and the thing it asserts can be wrong in
 * both at once.
 *
 * The shell runs upon this processor's own flow of control, as every program
 * so far has: ThreadStart does not return until the program ends, and the
 * `read` the shell blocks in halts this processor until a key arrives. That is
 * the arrangement docs/design/SHELL.md records, and it holds
 * until something else needs the bootstrap processor while a program waits.
 */
static bool KernelRunShell(int64_t *status)
{
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    Thread *boot;
    ElfImage image;
    uint64_t stack;

    arguments.argument_count = 1U;
    arguments.environment_count = 0U;
    arguments.argument[0] = 0U;
    arguments.storage_used = (uint32_t)(sizeof KERNEL_SHELL_NAME);

    for (size_t index = 0U; index < sizeof KERNEL_SHELL_NAME; ++index)
    {
        arguments.storage[index] = KERNEL_SHELL_NAME[index];
    }

    boot = ThreadAdoptCurrent("boot");

    if (boot == NULL)
    {
        return false;
    }

    process = ProcessCreate(KERNEL_SHELL_NAME, NULL);

    if (process == NULL)
    {
        ThreadDestroy(boot);

        return false;
    }

    if (ElfLoadFile(&process->space, KERNEL_SHELL_PATH, &image) != ELF_OK)
    {
        ProcessDestroy(process);
        ThreadDestroy(boot);

        return false;
    }

    ProcessRecordImage(process, &image);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        ThreadDestroy(boot);

        return false;
    }

    thread = ThreadCreate(process, image.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        ThreadDestroy(boot);

        return false;
    }

    *status = process->exit_status;

    ProcessDestroy(process);
    ThreadDestroy(boot);

    return true;
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

_Noreturn void KernelEnterSession(void)
{
    VgaSetColour(VGA_COLOUR_LIGHT_GREEN, VGA_COLOUR_BLACK);
    /*
     * The words "initialisation complete." are what the `verify` target of the
     * Makefile greps the serial log for, and are therefore load-bearing: they
     * establish that the kernel reached the end of its initialisation rather
     * than faulting, hanging or resetting on the way. What follows them says
     * which sub-task the boot got as far as, and must be revised with the boot.
     */
    KernelWriteString("Phase 9 initialisation complete: the window manager of sub-task 9.1 "
                      "is about to take the\nscreen, the keyboard and the mouse — a stack of "
                      "windows, one of them holding the focus,\nevery key routed to that one "
                      "and every movement to the one beneath the pointer — with\nthree "
                      "windows a person can operate upon it; and the shell of Phase 8 is "
                      "about to be read\noff the root filesystem and entered at privilege "
                      "level 3 upon the serial line, or upon\nthe console where the "
                      "shell-only or the diagnostics entry was chosen, where it prompts,\n"
                      "edits a line with a history, and runs what is typed: built-ins, "
                      "programs sought upon\nPATH, their redirections, pipelines of them, "
                      "and jobs.\n");

    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);

    /*
     * Which of two things the screen is for, decided by the menu entry.
     *
     * Since sub-task 9.1 the default entry gives the screen to the window
     * manager, and the shell runs upon the serial line; the `shell-only`
     * entry, and the `diagnostics` entry that shows the boot log first, give
     * the screen to the shell as every entry did through Phase 8. The window
     * manager needs a compositor, so a machine the boot loader left in a
     * text mode gets the shell whichever entry was chosen, and is told so.
     */
    {
        const bool shell_only = KernelCommandLineHasOption("shell-only") ||
                                KernelCommandLineHasOption("diagnostics");
        bool windows = !shell_only && CompositorIsActive() && VfsRootIsMounted();

        if (!shell_only && !windows)
        {
            KernelWriteString("The window manager needs a compositor and a root, and one is "
                              "absent; the shell takes the screen.\n");
        }

        /*
         * The display speaks again, if it was quiet — where the shell has it. A
         * person who booted the shell-only entry sees the banner and a prompt
         * and nothing between: the boot log is upon the serial line, and upon
         * the screen of the `diagnostics` entry. It said so in a line here
         * until 2026-09-15, at the project owner's request removed — the menu
         * entry's name is the notice. Where the window manager has the screen
         * the display stays quiet, the shell's output being the serial line's
         * and the screen being the windows'.
         */
        KernelDisplaySetQuiet(windows);

        /*
         * Sub-task 8.1: the shell, where there is a root to read it from and a
         * terminal to type at. It is started again when it ends, because the
         * alternative is a machine that halts the first time somebody presses
         * control-D — and each ending is reported, so that a shell which faulted
         * is not mistaken for one that was asked to stop.
         *
         * The erase limit is set before the first prompt for the reason the echo
         * loop sets it: everything above this line is the boot log, and a
         * backspace must not consume it. The editor never backspaces past its
         * own prompt, so the limit is a guard and not a mechanism the shell
         * depends upon.
         *
         * Where there is a keyboard or a mouse but no root, the echo loop of
         * Phase 3 remains, as the demonstration of the interrupt path it always
         * was.
         */
        if (VfsRootIsMounted() && (KeyboardIsPresent() || SerialIsPresent()))
        {
            VgaSetEraseLimit();
            ConsoleSetEraseLimit();

            if (windows)
            {
                /*
                 * Sub-task 9.1: the window manager takes the screen and the
                 * keyboard, and the pointer becomes visible over its windows.
                 * The terminal stops reading the keyboard, or every keystroke
                 * would reach both the focused window and the shell; the serial
                 * line remains the shell's. The tick handler does the rest.
                 */
                if (KernelStartWindowManager())
                {
                    TerminalAttachKeyboard(false);

                    if (MouseIsPresent())
                    {
                        CursorShow();
                    }

                    KernelDisplaySetMode(KERNEL_DISPLAY_WINDOWS);

                    /*
                     * The boot screen, of sub-task 9.3: the mark and the
                     * wordmark upon the slate ground, in place of the blank
                     * quiet screen a person saw while the desktop was got
                     * ready. It is drawn only here, upon the entry that gives
                     * the window manager the screen; the entries that give the
                     * shell the screen show the log or the prompt, which is
                     * what belongs there. The desktop composes over it when
                     * `init` has started its first window.
                     */
                    KernelBootScreen();
                    WindowManagerReport();
                }
                else
                {
                    KernelWriteString("The window manager could not take the screen; the "
                                      "shell takes it.\n");
                    KernelDisplaySetQuiet(false);
                    windows = false;
                }
            }

            if (KernelDisplayCurrentMode() != KERNEL_DISPLAY_WINDOWS)
            {
                /*
                 * The pointer becomes visible with the shell, and follows the
                 * mouse while the shell runs, since 2026-09-16. Until then it
                 * was shown by the echo loop below alone, which the shell had
                 * replaced at 8.1, and the shell path drained no mouse event —
                 * so a person with a mouse in hand saw nothing move and asked
                 * why. Nothing upon this path uses the pointer; it is shown
                 * because a pointer that follows the hand is the one thing a
                 * machine with a mouse is expected to do without being asked.
                 * The tick handler moves it.
                 */
                if (MouseIsPresent())
                {
                    CursorShow();
                    CompositorPresent();
                    KernelDisplaySetMode(KERNEL_DISPLAY_POINTER);
                }
            }

            /*
             * `init`, of sub-task 9.3: the first user process, launched here
             * and named to the kernel so that `power` may be reserved to it.
             * It starts and supervises the desktop where there is one — the
             * window demonstration of 9.2 is its child now, not the kernel's —
             * and it collects the orphans a background job leaves, on every
             * entry. It is launched beside the shell and waited for by nobody;
             * a machine whose `init` could not be started keeps the shell,
             * which is the interactive thing regardless, and says so.
             */
            {
                const uint64_t init = KernelLaunchProgram(KERNEL_INIT_PATH, KERNEL_INIT_NAME);

                if (init != 0U)
                {
                    ProcessSetInit(init);
                }
                else
                {
                    /*
                     * The desktop will not appear: `init` is what starts the
                     * session, and nothing else does. So the screen goes back
                     * to the console before this is written, or it would be
                     * written behind a boot screen that is now the last thing
                     * this machine ever draws.
                     */
                    KernelScreenBackToConsole();
                    KernelWriteString("init " KERNEL_INIT_PATH " could not be started; "
                                      "there is no supervisor, no desktop and no shutdown.\n");
                }
            }

            for (;;)
            {
                int64_t status = 0;

                if (!KernelRunShell(&status))
                {
                    KernelScreenBackToConsole();
                    KernelWriteString("The shell " KERNEL_SHELL_PATH " could not be started.\n");
                    break;
                }

                if (status < 0)
                {
                    /* A shell that faulted — a negative status is the negated
                     * vector — is not started again: a shell that faulted at
                     * once would be started at once, for ever, and the log
                     * would be that. A status the shell chose, since 8.3's
                     * `exit [n]`, is an ending and not a failure, whatever the
                     * number. */
                    KernelScreenBackToConsole();
                    KernelWriteString("The shell ended by a fault, status ");
                    KernelWriteHexadecimal((uint64_t)status);
                    KernelWriteString(", and is not started again.\n");
                    break;
                }

                KernelWriteString("The shell ended with status ");
                KernelWriteHexadecimal((uint64_t)status);
                KernelWriteString("; starting it again.\n");
            }

            /*
             * The echo loop below drains the mouse and the keyboard for itself,
             * and needs the console rather than the window manager to write
             * upon. Every path that breaks out of the loop above has already
             * asked for it; this is the path that did not break — the shell
             * upon a display the window manager never took — and the call is
             * the same call, which is why it is idempotent.
             */
            KernelScreenBackToConsole();
            KernelDisplaySetMode(KERNEL_DISPLAY_IDLE);
            TerminalAttachKeyboard(true);
            KernelDisplaySetQuiet(false);
        }
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
