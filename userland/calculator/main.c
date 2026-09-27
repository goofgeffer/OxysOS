/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/calculator/main.c
 * Purpose: The calculator, of 2026-09-27: a window with a display and a grid
 *          of buttons, pressed with the pointer or typed at, whose arithmetic
 *          is engine.c's.
 * Key functions: main, CalcDraw, CalcPress, CalcTyped.
 * References:
 *   - userland/calculator/engine.h: what each key does, and why the numbers
 *     are fixed point.
 *   - kernel/abi/oxys/syscall_abi.h: the window calls and window_text's flags.
 *   - docs/design/UTILITIES.md: the design.
 *
 * Why the buttons and the keys are one path.
 *
 *   A press upon a button and a key typed both become one character given to
 *   CalcKey — the button's own, `=` for Enter, `b` for Backspace — so the
 *   calculator a person clicks and the one they type at cannot disagree, and
 *   the self-test that asserts the keys asserts both.
 */

#include <palette.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#include "engine.h"

/* The layout, in units of the scale. */
#define CALC_MARGIN   6
#define CALC_GAP      3
#define CALC_CELL_W   30
#define CALC_CELL_H   20
#define CALC_DISPLAY  26
#define CALC_COLUMNS  4
#define CALC_ROWS     5

#define CALC_PAPER   OXYS_RGB(OXYS_PAPER_RED, OXYS_PAPER_GREEN, OXYS_PAPER_BLUE)
#define CALC_INK     OXYS_RGB(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE)
#define CALC_BUTTON  OXYS_RGB(OXYS_BUTTON_RED, OXYS_BUTTON_GREEN, OXYS_BUTTON_BLUE)
#define CALC_QUIET   OXYS_RGB(OXYS_BAR_QUIET_RED, OXYS_BAR_QUIET_GREEN, OXYS_BAR_QUIET_BLUE)
#define CALC_EDGE \
    OXYS_RGB(OXYS_BUTTON_EDGE_RED, OXYS_BUTTON_EDGE_GREEN, OXYS_BUTTON_EDGE_BLUE)

/* The grid, row by row: each button's label and the key it gives. The
 * operators, `=` and the three at the top stand in the quiet colour, so the
 * digits read as the one block they are. */
typedef struct CalcButton
{
    const char *label;
    char key;
} CalcButton;

static const CalcButton CalcButtons[CALC_ROWS][CALC_COLUMNS] = {
    { { "C", 'C' }, { "+/-", 'n' }, { "%", '%' }, { "/", '/' } },
    { { "7", '7' }, { "8", '8' }, { "9", '9' }, { "x", '*' } },
    { { "4", '4' }, { "5", '5' }, { "6", '6' }, { "-", '-' } },
    { { "1", '1' }, { "2", '2' }, { "3", '3' }, { "+", '+' } },
    { { "0", '0' }, { ".", '.' }, { "<-", 'b' }, { "=", '=' } },
};

static CalcState CalcNow;
static int64_t CalcWindow = -1;
static int32_t CalcScale = 2;
static int32_t CalcWidth;
static int32_t CalcHeight;

static void CalcFill(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colour)
{
    static uint32_t tile[4096];
    SyscallWindowRectangle area;
    const int32_t rows = (int32_t)(sizeof tile / sizeof tile[0]) / ((width > 0) ? width : 1);

    if ((width <= 0) || (height <= 0) || (rows <= 0))
    {
        return;
    }

    for (size_t index = 0U; index < (sizeof tile / sizeof tile[0]); ++index)
    {
        tile[index] = colour;
    }

    area.x = x;
    area.width = width;

    for (int32_t top = 0; top < height; top += rows)
    {
        area.y = y + top;
        area.height = ((top + rows) <= height) ? rows : (height - top);
        (void)OxysWindowBlit(CalcWindow, &area, tile);
    }
}

/* Draws, or with SYSCALL_WINDOW_TEXT_MEASURE only measures, proportional text;
 * its width in pixels, or 0. */
static int32_t CalcText(int32_t x, int32_t y, const char *text, uint32_t paper, int32_t scale,
                        uint32_t flags)
{
    SyscallWindowText placement;
    int64_t width;

    placement.x = x;
    placement.y = y;
    placement.ink = CALC_INK;
    placement.paper = paper;
    placement.scale = scale;
    placement.flags = SYSCALL_WINDOW_TEXT_PROPORTIONAL | flags;
    width = OxysWindowText(CalcWindow, &placement, text);

    return (width > 0) ? (int32_t)width : 0;
}

/* A rectangle's edge, a unit wide, in the owner's button style. */
static void CalcEdge(int32_t x, int32_t y, int32_t width, int32_t height)
{
    CalcFill(x, y, width, CalcScale, CALC_EDGE);
    CalcFill(x, y + height - CalcScale, width, CalcScale, CALC_EDGE);
    CalcFill(x, y, CalcScale, height, CALC_EDGE);
    CalcFill(x + width - CalcScale, y, CalcScale, height, CALC_EDGE);
}

/* The top left of the button at `row` and `column`, in pixels. */
static int32_t CalcButtonX(int32_t column)
{
    return (CALC_MARGIN + (column * (CALC_CELL_W + CALC_GAP))) * CalcScale;
}

static int32_t CalcButtonY(int32_t row)
{
    return (CALC_MARGIN + CALC_DISPLAY + (2 * CALC_GAP) + (row * (CALC_CELL_H + CALC_GAP))) *
           CalcScale;
}

/*
 * The display: the number, right-aligned, at twice the scale where it fits
 * and at the scale where it does not, so that a long result is still shown
 * whole rather than cut at the window's edge.
 */
static void CalcDrawDisplay(void)
{
    const int32_t x = CALC_MARGIN * CalcScale;
    const int32_t y = CALC_MARGIN * CalcScale;
    const int32_t width = ((CALC_COLUMNS * CALC_CELL_W) + ((CALC_COLUMNS - 1) * CALC_GAP)) *
                          CalcScale;
    const int32_t height = CALC_DISPLAY * CalcScale;
    const int32_t pad = 4 * CalcScale;
    const char *const text = CalcDisplay(&CalcNow);
    int32_t scale = CalcScale * 2;
    int32_t measured = CalcText(0, 0, text, CALC_PAPER, scale, SYSCALL_WINDOW_TEXT_MEASURE);

    if (measured > (width - (2 * pad)))
    {
        scale = CalcScale;
        measured = CalcText(0, 0, text, CALC_PAPER, scale, SYSCALL_WINDOW_TEXT_MEASURE);
    }

    CalcFill(x, y, width, height, CALC_PAPER);
    CalcEdge(x, y, width, height);
    (void)CalcText(x + width - pad - measured, y + ((height - (8 * scale)) / 2), text, CALC_PAPER,
                   scale, 0U);
}

static void CalcDrawButton(int32_t row, int32_t column)
{
    const CalcButton *const button = &CalcButtons[row][column];
    const bool quiet = (row == 0) || (column == (CALC_COLUMNS - 1));
    const uint32_t ground = quiet ? CALC_QUIET : CALC_BUTTON;
    const int32_t x = CalcButtonX(column);
    const int32_t y = CalcButtonY(row);
    const int32_t width = CALC_CELL_W * CalcScale;
    const int32_t height = CALC_CELL_H * CalcScale;
    const int32_t measured =
        CalcText(0, 0, button->label, ground, CalcScale, SYSCALL_WINDOW_TEXT_MEASURE);

    CalcFill(x, y, width, height, ground);
    (void)CalcText(x + ((width - measured) / 2), y + ((height - (8 * CalcScale)) / 2),
                   button->label, ground, CalcScale, 0U);
    CalcEdge(x, y, width, height);
}

static void CalcDraw(void)
{
    CalcFill(0, 0, CalcWidth, CalcHeight, CALC_PAPER);
    CalcDrawDisplay();

    for (int32_t row = 0; row < CALC_ROWS; ++row)
    {
        for (int32_t column = 0; column < CALC_COLUMNS; ++column)
        {
            CalcDrawButton(row, column);
        }
    }
}

/* A press: the key of the button beneath it, if any. */
static void CalcPress(int32_t x, int32_t y)
{
    for (int32_t row = 0; row < CALC_ROWS; ++row)
    {
        for (int32_t column = 0; column < CALC_COLUMNS; ++column)
        {
            const int32_t left = CalcButtonX(column);
            const int32_t top = CalcButtonY(row);

            if ((x >= left) && (x < (left + (CALC_CELL_W * CalcScale))) && (y >= top) &&
                (y < (top + (CALC_CELL_H * CalcScale))))
            {
                CalcKey(&CalcNow, CalcButtons[row][column].key);
                CalcDrawDisplay();

                return;
            }
        }
    }
}

/* A key typed: the character a button would give, or nothing. */
static void CalcTyped(const SyscallWindowEvent *event)
{
    char key = event->key_character;

    if (event->key_pressed == 0U)
    {
        return;
    }

    switch (key)
    {
    case '\n':
    case '\r':
        key = '=';
        break;
    case '\b':
        key = 'b';
        break;
    case 'x':
    case 'X':
        key = '*';
        break;
    case ',':
        key = '.';
        break;
    case 'c':
    case 27:
        key = 'C';
        break;
    default:
        break;
    }

    CalcKey(&CalcNow, key);
    CalcDrawDisplay();
}

int main(void)
{
    SyscallWindowRectangle screen;
    SyscallWindowRectangle geometry;
    bool running = true;

    if (OxysWindowScreen(&screen) != 0)
    {
        (void)fprintf(stderr, "calculator: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    CalcReset(&CalcNow);
    CalcScale = (screen.width >= 1024) ? 2 : 1;
    CalcWidth = ((2 * CALC_MARGIN) + (CALC_COLUMNS * CALC_CELL_W) +
                 ((CALC_COLUMNS - 1) * CALC_GAP)) *
                CalcScale;
    CalcHeight = CalcButtonY(CALC_ROWS) - (CALC_GAP * CalcScale) + (CALC_MARGIN * CalcScale);
    geometry.width = CalcWidth;
    geometry.height = CalcHeight;
    geometry.x = (screen.width - geometry.width) / 2;
    geometry.y = (screen.height - geometry.height) / 3;

    CalcWindow = OxysWindowCreate(&geometry, "Calculator", SYSCALL_WINDOW_LAYER_NORMAL);

    if (CalcWindow < 0)
    {
        (void)fprintf(stderr, "calculator: a window could not be made.\n");

        return EXIT_FAILURE;
    }

    CalcDraw();

    while (running)
    {
        SyscallWindowEvent event;
        int64_t result = OxysWindowEvent(CalcWindow, &event, SYSCALL_WINDOW_WAIT);

        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        while (result == 1)
        {
            if (event.kind == SYSCALL_WINDOW_EVENT_BUTTON_PRESS)
            {
                CalcPress(event.x, event.y);
            }
            else if (event.kind == SYSCALL_WINDOW_EVENT_KEY)
            {
                CalcTyped(&event);
            }
            else if (event.kind == SYSCALL_WINDOW_EVENT_RESIZE)
            {
                /* Made full or given back its size: the ground covers what
                 * there is now, and the calculator stands at the top left. */
                CalcWidth = event.x;
                CalcHeight = event.y;
                CalcDraw();
            }
            else if (event.kind == SYSCALL_WINDOW_EVENT_CLOSE)
            {
                running = false;
            }

            result = OxysWindowEvent(CalcWindow, &event, 0U);
        }
    }

    (void)OxysWindowDestroy(CalcWindow);

    return EXIT_SUCCESS;
}
