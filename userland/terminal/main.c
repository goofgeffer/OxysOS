/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/terminal/main.c
 * Purpose: The terminal emulator of sub-task 9.6 — a window with a shell
 *          beneath it. It starts /bin/sh upon a pair of pipes, draws what the
 *          shell writes, and sends the shell what a person types at the window.
 * Key functions: main, TerminalStartShell, TerminalDraw, TerminalHandleKey,
 *          TerminalHandleEvents, TerminalDrainShell, TerminalResize.
 * References:
 *   - libc/include/term.h: the grid and the key translation, both of which are
 *     the C library's and are asserted without a window by
 *     kernel/test/libc/term.c. What is left here is what cannot be.
 *   - kernel/abi/oxys/syscall_abi.h: `poll`, and the window calls of 9.2.
 *   - docs/design/TERMINAL.md: what this is, what it does not do, and why the
 *     shell beneath it runs without job control.
 *
 * What this program is, in one paragraph.
 *
 *   It owns a window and a shell, and it carries bytes between them. The shell
 *   is an ordinary /bin/sh with pipes where its terminal would be, so it edits
 *   its line, echoes what it is given and prints its prompt exactly as it does
 *   upon the serial line; nothing in the shell knows it is in a window. What
 *   arrives from the shell goes into the grid, which decides what stands where,
 *   and the rows the grid says have changed are drawn with `window_text`. What
 *   arrives from the window is a key, which becomes the bytes a terminal would
 *   send and goes down the other pipe.
 *
 * Why it waits in `poll` and not in `window_event`.
 *
 *   It has two things to wait upon and they are not the same kind of thing: the
 *   keys its window receives, and the bytes the shell writes. Sleeping in
 *   either one alone is being deaf to the other — a terminal that woke only for
 *   keys would show the output of `ls` at the next keystroke — and sleeping in
 *   neither is spinning. `poll` of sub-task 9.6 exists for this, and this is
 *   its only caller.
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <term.h>

/* The shell this hosts. It is the same program the kernel starts upon the
 * serial line; there is one shell. */
#define TERMINAL_SHELL "/bin/sh"

/* The face is eight pixels square and every glyph of it the same, so a cell is
 * eight by the scale and a run of `n` characters is exactly `n * 8 * scale`
 * across. The session measures its wordmark the same way. */
#define TERMINAL_GLYPH 8

/*
 * Two pixels of leading between rows, at the scale.
 *
 * The face is eight pixels tall and a row of it eight pixels high leaves no
 * space at all between one line and the next, which is what the kernel's
 * console looks like and is legible there because a boot log is read in
 * paragraphs. A person reads a terminal line by line.
 *
 * **The gap is painted once and never again.** Nothing draws into it — a row is
 * drawn as glyph cells and the glyphs are eight pixels tall — so what shows
 * there is whatever the window's content held when it was made, which is the
 * window manager's warm paper. That was invisible while this drew upon the same
 * paper and would have been two-pixel white stripes across the window the
 * moment it drew upon black. TerminalPaintGround is the answer, and it runs at
 * start alone.
 */
#define TERMINAL_LEADING 2
#define TERMINAL_PITCH   (TERMINAL_GLYPH + TERMINAL_LEADING)

/*
 * The colours: white upon black, at the project owner's direction on
 * 2026-09-22.
 *
 * **They are this program's own and not art/palette.h's**, and that is the
 * rule the demonstration's coral and mint already follow: a colour belongs in
 * the shared header when two things must agree about it, and nothing else in
 * this system draws a terminal. The paper and the ink the header carries are
 * what a frame, a boot screen and a desktop must agree upon; a window full of
 * text that a person reads for minutes at a time is a different problem from a
 * label upon a panel, and it was answered here with the system's warm paper
 * until somebody had to look at it.
 */
#define TERMINAL_PAPER UINT32_C(0x00000000)
#define TERMINAL_INK   UINT32_C(0x00FFFFFF)

static TermScreen TerminalGrid;
static int64_t TerminalWindow = -1;
static int32_t TerminalScale = 2;

/* The content's extent, which since 2026-09-23 may be larger than the grid:
 * a window made full is given what the screen has, and the grid takes as many
 * whole cells of it as it holds. */
static int32_t TerminalContentWidth;
static int32_t TerminalContentHeight;

/* The pipes, from this program's side: what it writes to the shell, and what it
 * reads from the shell. */
static int TerminalToShell = -1;
static int TerminalFromShell = -1;

static int64_t TerminalShell = -1;

/* Where the cursor was last drawn, so that the cell it has left can be put back
 * without redrawing the row it was in. */
static uint32_t TerminalCursorColumn;
static uint32_t TerminalCursorRow;
static bool TerminalCursorDrawn;

/*
 * Paints the whole content in the terminal's ground, once, before anything is
 * drawn upon it.
 *
 * A window is made carrying the window manager's paper, and this draws upon
 * black; without this the leading between the rows, and every cell no glyph has
 * reached yet, would show the paper through. **There is no fill across the
 * protocol** — the session met the same wall for its panel,
 * docs/design/SESSION.md, Section 7, limitation 4 — so it is a buffer blitted
 * in bands: one row's pitch at a time, which is a few tens of kilobytes rather
 * than the megabyte the whole content would be.
 *
 * It runs at start alone. Nothing afterwards uncovers the ground: a row redrawn
 * writes its own cells whole, paper and glyph together.
 */
static bool TerminalPaintGround(void)
{
    const int32_t width = TerminalContentWidth;
    const int32_t band = TERMINAL_PITCH * TerminalScale;
    const size_t count = (size_t)width * (size_t)band;
    uint32_t *const pixels = malloc(count * sizeof *pixels);
    SyscallWindowRectangle area;

    if ((pixels == NULL) || (width <= 0))
    {
        free(pixels);

        return false;
    }

    for (size_t index = 0U; index < count; ++index)
    {
        pixels[index] = TERMINAL_PAPER;
    }

    area.x = 0;
    area.width = width;

    /* The whole content and not the grid alone: a window made full has a
     * margin below and to the right of the last whole cell, and that margin
     * would otherwise stand in the manager's paper. */
    for (int32_t top = 0; top < TerminalContentHeight; top += band)
    {
        area.y = top;
        area.height = ((top + band) <= TerminalContentHeight) ? band
                                                              : (TerminalContentHeight - top);

        if (OxysWindowBlit(TerminalWindow, &area, pixels) != 0)
        {
            free(pixels);

            return false;
        }
    }

    free(pixels);

    return true;
}

/*
 * The content has a new extent — the window was made full, or ceased to be:
 * the grid is given as many whole cells as it holds, keeping its text, and the
 * whole content is painted and drawn again. The shell is not told; nothing in
 * this system can tell a program its terminal's size, docs/design/TERMINAL.md,
 * Section 7, and a program that laid out a screen by the old size will lay it
 * out wrongly until it next looks — which is every program that exists here,
 * none of them laying out screens.
 */
static void TerminalResize(int32_t width, int32_t height)
{
    uint32_t columns = (uint32_t)(width / (TERMINAL_GLYPH * TerminalScale));
    uint32_t rows = (uint32_t)(height / (TERMINAL_PITCH * TerminalScale));

    columns = (columns > TERM_COLUMNS_MAXIMUM) ? TERM_COLUMNS_MAXIMUM : columns;
    rows = (rows > TERM_ROWS_MAXIMUM) ? TERM_ROWS_MAXIMUM : rows;

    TerminalContentWidth = width;
    TerminalContentHeight = height;

    if ((columns == 0U) || (rows == 0U) || !TermResize(&TerminalGrid, columns, rows))
    {
        return;
    }

    TerminalCursorDrawn = false;
    (void)TerminalPaintGround();
}

/* Draws one run of text at a cell position. */
static void TerminalDrawAt(uint32_t column, uint32_t row, const char *text, uint32_t ink,
                           uint32_t paper)
{
    SyscallWindowText placement;

    placement.x = (int32_t)column * TERMINAL_GLYPH * TerminalScale;
    placement.y = (int32_t)row * TERMINAL_PITCH * TerminalScale;
    placement.ink = ink;
    placement.paper = paper;
    placement.scale = TerminalScale;

    (void)OxysWindowText(TerminalWindow, &placement, text);
}

/*
 * Draws the rows the grid says have changed, and the cursor.
 *
 * **Only what changed.** Each row is a system call, and a screen of twenty rows
 * redrawn for every keystroke is twenty calls and twenty rows of glyphs for one
 * character. The grid keeps the account; this asks it.
 *
 * The cursor is a cell drawn in the colours reversed, and the cell it has left
 * is put back before the new one is drawn. Two calls, and never the row.
 */
static void TerminalDraw(void)
{
    const uint32_t column = TermCursorColumn(&TerminalGrid);
    const uint32_t row = TermCursorRow(&TerminalGrid);
    char cell[2] = { ' ', '\0' };
    const char *text;

    for (uint32_t index = 0U; index < TermRows(&TerminalGrid); ++index)
    {
        if (!TermRowChanged(&TerminalGrid, index))
        {
            continue;
        }

        text = TermRow(&TerminalGrid, index);

        if (text != NULL)
        {
            TerminalDrawAt(0U, index, text, TERMINAL_INK, TERMINAL_PAPER);
        }

        TermRowDrawn(&TerminalGrid, index);

        /* A row redrawn has taken the cursor's reversed cell with it. */
        if (index == TerminalCursorRow)
        {
            TerminalCursorDrawn = false;
        }
    }

    if (TerminalCursorDrawn &&
        ((TerminalCursorColumn != column) || (TerminalCursorRow != row)))
    {
        text = TermRow(&TerminalGrid, TerminalCursorRow);

        if (text != NULL)
        {
            cell[0] = text[TerminalCursorColumn];
            TerminalDrawAt(TerminalCursorColumn, TerminalCursorRow, cell, TERMINAL_INK,
                           TERMINAL_PAPER);
        }

        TerminalCursorDrawn = false;
    }

    text = TermRow(&TerminalGrid, row);

    if (text != NULL)
    {
        cell[0] = text[column];
        TerminalDrawAt(column, row, cell, TERMINAL_PAPER, TERMINAL_INK);
    }

    TerminalCursorColumn = column;
    TerminalCursorRow = row;
    TerminalCursorDrawn = true;
}

/*
 * Starts the shell upon the two pipes.
 *
 * The child places the ends and closes every one it does not need, which is the
 * arrangement the shell's own pipelines use: an end left open in the child is
 * an end the pipe counts, and a pipe with a writer that never writes is a read
 * that never ends.
 *
 * **It puts itself in a group of its own.** The emulator sends control-C to
 * that group, so the shell and everything the shell starts must be in it and
 * the emulator must not: a program that interrupted its own group would
 * interrupt itself.
 */
static bool TerminalStartShell(void)
{
    int to_shell[2];
    int from_shell[2];

    if (OxysPipe(to_shell) < 0)
    {
        (void)fprintf(stderr, "terminal: a pipe to the shell could not be made.\n");

        return false;
    }

    if (OxysPipe(from_shell) < 0)
    {
        (void)OxysClose(to_shell[0]);
        (void)OxysClose(to_shell[1]);
        (void)fprintf(stderr, "terminal: a pipe from the shell could not be made.\n");

        return false;
    }

    TerminalShell = OxysFork();

    if (TerminalShell < 0)
    {
        (void)fprintf(stderr, "terminal: the shell could not be forked.\n");

        return false;
    }

    if (TerminalShell == 0)
    {
        char *const arguments[] = { (char *)TERMINAL_SHELL, NULL };

        (void)OxysSetProcessGroup(0, 0);

        (void)OxysDuplicate(to_shell[0], 0);
        (void)OxysDuplicate(from_shell[1], 1);
        (void)OxysDuplicate(from_shell[1], 2);

        (void)OxysClose(to_shell[0]);
        (void)OxysClose(to_shell[1]);
        (void)OxysClose(from_shell[0]);
        (void)OxysClose(from_shell[1]);

        (void)OxysExecve(TERMINAL_SHELL, arguments, NULL);

        /* Reached only where the shell is not there to become. The message goes
         * down the pipe, so it is drawn in the window a person is looking at
         * rather than upon a serial line they may not have. */
        (void)fprintf(stderr, "terminal: %s could not be run.\n", TERMINAL_SHELL);
        OxysExit(127);
    }

    (void)OxysClose(to_shell[0]);
    (void)OxysClose(from_shell[1]);

    TerminalToShell = to_shell[1];
    TerminalFromShell = from_shell[0];

    return true;
}

/* Sends what a key contributes down to the shell. */
static void TerminalHandleKey(const SyscallWindowEvent *event)
{
    char bytes[TERM_KEY_BYTES_MAXIMUM];
    size_t count;

    if (event->key_pressed == 0U)
    {
        return;
    }

    count = TermKeyBytes(event->key_character, event->key_scancode, event->key_modifiers,
                        event->key_extended != 0U, bytes, sizeof bytes);

    if (count == 0U)
    {
        return;
    }

    /*
     * **Control-C is a signal and not a byte.** The shell beneath has no
     * terminal — its standard input is a pipe — so nothing in the kernel turns
     * a control byte into an interrupt, which is what the terminal of Phase 8
     * does for the shell upon the serial line. The emulator is the line
     * discipline here, and this is the whole of it: the group the shell leads
     * is interrupted, the shell itself ignoring the signal and whatever it is
     * running taking the default action. docs/design/TERMINAL.md, Section 5.
     */
    if ((count == 1U) && (bytes[0] == 0x03) && (TerminalShell > 0))
    {
        (void)OxysKill(-TerminalShell, SIGINT);

        return;
    }

    (void)OxysWrite(TerminalToShell, bytes, count);
}

/* Drains the window's events. Returns false where the window is to be closed. */
static bool TerminalHandleEvents(void)
{
    SyscallWindowEvent event;

    while (OxysWindowEvent(TerminalWindow, &event, 0U) > 0)
    {
        switch (event.kind)
        {
        case SYSCALL_WINDOW_EVENT_KEY:
            TerminalHandleKey(&event);
            break;

        case SYSCALL_WINDOW_EVENT_RESIZE:
            TerminalResize(event.x, event.y);
            break;

        case SYSCALL_WINDOW_EVENT_CLOSE:
            return false;

        default:
            break;
        }
    }

    return true;
}

/* Reads what the shell has written into the grid. Returns false at the end of
 * the pipe, which is the shell having ended. */
static bool TerminalDrainShell(void)
{
    char buffer[256];
    const int64_t read = OxysRead(TerminalFromShell, buffer, sizeof buffer);

    if (read <= 0)
    {
        return false;
    }

    TermWrite(&TerminalGrid, buffer, (size_t)read);

    return true;
}

int main(void)
{
    SyscallWindowRectangle screen;
    SyscallWindowRectangle geometry;
    uint32_t columns;
    uint32_t rows;
    bool running = true;

    if (OxysWindowScreen(&screen) != 0)
    {
        (void)fprintf(stderr, "terminal: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    /*
     * The size is chosen from the screen, as the desktop's and the session's
     * are: twice the face upon a screen at least 1024 wide and once below it,
     * and then as many whole cells as three quarters of the screen holds,
     * within what the grid can carry. Whole cells, because a window with half a
     * column in it draws that half column every time and a person sees a stripe.
     */
    TerminalScale = (screen.width >= 1024) ? 2 : 1;

    columns = (uint32_t)((screen.width * 3 / 4) / (TERMINAL_GLYPH * TerminalScale));
    rows = (uint32_t)((screen.height * 3 / 5) / (TERMINAL_PITCH * TerminalScale));

    if (columns > TERM_COLUMNS_MAXIMUM)
    {
        columns = TERM_COLUMNS_MAXIMUM;
    }

    if (rows > TERM_ROWS_MAXIMUM)
    {
        rows = TERM_ROWS_MAXIMUM;
    }

    if (!TermInitialise(&TerminalGrid, columns, rows))
    {
        (void)fprintf(stderr, "terminal: a grid of %u by %u could not be made.\n",
                      (unsigned)columns, (unsigned)rows);

        return EXIT_FAILURE;
    }

    geometry.width = (int32_t)columns * TERMINAL_GLYPH * TerminalScale;
    geometry.height = (int32_t)rows * TERMINAL_PITCH * TerminalScale;
    geometry.x = (screen.width - geometry.width) / 2;
    geometry.y = (screen.height - geometry.height) / 2;

    TerminalContentWidth = geometry.width;
    TerminalContentHeight = geometry.height;
    TerminalWindow = OxysWindowCreate(&geometry, "Terminal", SYSCALL_WINDOW_LAYER_NORMAL);

    if (TerminalWindow < 0)
    {
        (void)fprintf(stderr, "terminal: a window could not be made.\n");

        return EXIT_FAILURE;
    }

    /* The ground before the shell, so that no part of the window is ever seen
     * in the window manager's paper. A window that could not be painted is a
     * window with white stripes across it and is worth saying so about, but it
     * is not worth refusing to run over. */
    if (!TerminalPaintGround())
    {
        (void)fprintf(stderr, "terminal: the window's ground could not be painted.\n");
    }

    if (!TerminalStartShell())
    {
        (void)OxysWindowDestroy(TerminalWindow);

        return EXIT_FAILURE;
    }

    TerminalDraw();

    while (running)
    {
        SyscallPollEntry watched[2];

        watched[0].descriptor = TerminalFromShell;
        watched[1].descriptor = SYSCALL_POLL_WINDOWS;

        if (OxysPoll(watched, 2U, 0U) < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        if (watched[1].ready != 0U)
        {
            running = TerminalHandleEvents();
        }

        /*
         * The shell is drained after the keys and not before, so that what a
         * key causes is drawn in the same pass as the key: the echo of a
         * character typed is already in the pipe by the time this runs, and a
         * terminal that drew it at the next wake would feel slow for no reason
         * but the order of two lines.
         */
        if (running && (watched[0].ready != 0U))
        {
            running = TerminalDrainShell();
        }

        TerminalDraw();
    }

    /*
     * The shell is ended rather than left. It is the child of this program and
     * nothing else would collect it; a shell whose window has gone is a shell
     * nobody can type at, which is the definition of a process to stop.
     */
    if (TerminalShell > 0)
    {
        (void)OxysKill(-TerminalShell, SIGHUP);
        (void)OxysClose(TerminalToShell);
        (void)OxysWaitFor(TerminalShell, NULL, 0U);
    }

    (void)OxysClose(TerminalFromShell);
    (void)OxysWindowDestroy(TerminalWindow);

    return EXIT_SUCCESS;
}
