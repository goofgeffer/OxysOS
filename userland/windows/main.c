/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/windows/main.c
 * Purpose: The demonstration the default boot entry presents since sub-task
 *          9.2 — three windows a person can operate, drawn by a program at
 *          privilege level 3 through the client protocol: a figure, a disc
 *          that follows the pointer and grows while a button is held, and a
 *          row of tiles that the keys typed add to and remove from.
 * Key functions: main, DemoDrawFigure, DemoDrawPointer, DemoDrawKeys,
 *          DemoFillDisc, DemoPresent, DemoHandleKey.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: the five window calls, the rectangle,
 *     the event, and the pixel format 0x00RRGGBB.
 *   - docs/design/WINDOWS.md, Section 10: the protocol, and Section 7 what
 *     this program stands in for until there is a desktop.
 *   - J. E. Bresenham, "A linear algorithm for incremental digital display of
 *     circular arcs", CACM 20(2), 1977: the disc, drawn here as the kernel
 *     draws its close control, and written again rather than shared because a
 *     program under this licence may not link the kernel's.
 *
 * This is the first real client, and it is what sub-task 9.1's demonstration
 * was, moved across the boundary: every pixel here is composed in this
 * program's own memory and carried to its window by window_blit, and every
 * event is read by window_event with SYSCALL_WINDOW_ANY and a wait, so that
 * the program sleeps between events and costs the machine nothing standing
 * still. There is no text, because there is no face in userland yet — the
 * titles are the kernel's, drawn upon the frames — and the windows say what
 * they are by what they do.
 */

#include <config.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/*
 * Where the desktop's own configuration stands, of sub-task 9.4. Every key in
 * it has a default here, so that a file which is absent, unreadable or wrong in
 * a line leaves the desktop looking as it did rather than not appearing.
 */
#define DEMO_CONFIGURATION "/etc/desktop.conf"

static OxysConfig DemoConfig;

/* The palette, in the client's format: the accent the kernel's frame uses for
 * the focus, a warm white paper, a dark ink, and two more for the figure. */
#define DEMO_PAPER  UINT32_C(0x00F5F3EE)
#define DEMO_ACCENT UINT32_C(0x004F86F7)
#define DEMO_INK    UINT32_C(0x00262626)
#define DEMO_CORAL  UINT32_C(0x00F26B5B)
#define DEMO_MINT   UINT32_C(0x003FBF9F)

/* One window: its content in this program's memory, and its number. */
typedef struct DemoWindow
{
    int64_t number;
    int32_t width;
    int32_t height;
    uint32_t *pixels;
} DemoWindow;

static DemoWindow DemoFigure;
static DemoWindow DemoPointer;
static DemoWindow DemoKeys;

static int32_t DemoScale = 2;

/* The accent, which the configuration may replace; DEMO_ACCENT is the default
 * and is what stands where the file says nothing or says something that is not
 * three numbers. */
static uint32_t DemoAccent = DEMO_ACCENT;

static int32_t DemoPointerX;
static int32_t DemoPointerY;
static bool DemoPointerHeld;
static bool DemoPointerInside;

#define DEMO_KEYS_CAPACITY 28
static char DemoTyped[DEMO_KEYS_CAPACITY];
static int32_t DemoTypedCount;

static void DemoFillRectangle(DemoWindow *window, int32_t x, int32_t y, int32_t width,
                              int32_t height, uint32_t colour)
{
    const int32_t left = (x < 0) ? 0 : x;
    const int32_t top = (y < 0) ? 0 : y;
    const int32_t right = (x + width > window->width) ? window->width : x + width;
    const int32_t bottom = (y + height > window->height) ? window->height : y + height;

    for (int32_t row = top; row < bottom; ++row)
    {
        for (int32_t column = left; column < right; ++column)
        {
            window->pixels[(row * window->width) + column] = colour;
        }
    }
}

static void DemoFillDisc(DemoWindow *window, int32_t centre_x, int32_t centre_y, int32_t radius,
                         uint32_t colour)
{
    int32_t x = radius;
    int32_t y = 0;
    int32_t error = 1 - radius;

    while (x >= y)
    {
        DemoFillRectangle(window, centre_x - x, centre_y + y, (2 * x) + 1, 1, colour);
        DemoFillRectangle(window, centre_x - x, centre_y - y, (2 * x) + 1, 1, colour);
        DemoFillRectangle(window, centre_x - y, centre_y + x, (2 * y) + 1, 1, colour);
        DemoFillRectangle(window, centre_x - y, centre_y - x, (2 * y) + 1, 1, colour);

        ++y;

        if (error < 0)
        {
            error += (2 * y) + 1;
        }
        else
        {
            --x;
            error += (2 * (y - x)) + 1;
        }
    }
}

/* Carries the whole content to the window. */
static void DemoPresent(const DemoWindow *window)
{
    SyscallWindowRectangle area;

    if (window->number < 0)
    {
        return;
    }

    area.x = 0;
    area.y = 0;
    area.width = window->width;
    area.height = window->height;

    if (OxysWindowBlit(window->number, &area, window->pixels) != 0)
    {
        (void)fprintf(stderr, "windows: a blit was refused.\n");
    }
}

static bool DemoMake(DemoWindow *window, int32_t x, int32_t y, int32_t width, int32_t height,
                     const char *title)
{
    SyscallWindowRectangle geometry;

    window->width = width;
    window->height = height;
    window->pixels = malloc((size_t)width * (size_t)height * sizeof(uint32_t));
    window->number = -1;

    if (window->pixels == NULL)
    {
        return false;
    }

    geometry.x = x;
    geometry.y = y;
    geometry.width = width;
    geometry.height = height;
    window->number = OxysWindowCreate(&geometry, title);

    return window->number >= 0;
}

/*
 * The figure: a ring of discs about a larger one, in the palette's few
 * colours — geometry as the character, and nothing that is not geometry.
 */
static void DemoDrawFigure(void)
{
    DemoWindow *const window = &DemoFigure;
    const int32_t centre_x = window->width / 2;
    const int32_t centre_y = window->height / 2;
    const int32_t large = 22 * DemoScale;
    const int32_t small = 7 * DemoScale;
    const int32_t orbit = 40 * DemoScale;
    /* Eight points about the centre, as (x, y) in sixty-fourths of the orbit:
     * the cosine and sine of the eight compass directions, without floating
     * point, which this program does not use either. */
    static const int32_t points[8][2] = {
        { 64, 0 },   { 45, 45 },   { 0, 64 },   { -45, 45 },
        { -64, 0 },  { -45, -45 }, { 0, -64 },  { 45, -45 }
    };
    const uint32_t colours[3] = { DemoAccent, DEMO_CORAL, DEMO_MINT };

    DemoFillRectangle(window, 0, 0, window->width, window->height, DEMO_PAPER);
    DemoFillDisc(window, centre_x, centre_y, large, DEMO_INK);
    DemoFillDisc(window, centre_x, centre_y, large - (3 * DemoScale), DEMO_PAPER);
    DemoFillDisc(window, centre_x, centre_y, small, DemoAccent);

    for (size_t index = 0U; index < 8U; ++index)
    {
        DemoFillDisc(window, centre_x + ((points[index][0] * orbit) / 64),
                     centre_y + ((points[index][1] * orbit) / 64), small,
                     colours[index % 3U]);
    }

    DemoPresent(window);
}

/* The pointer: a disc where the pointer is, larger while a button is held. */
static void DemoDrawPointer(void)
{
    DemoWindow *const window = &DemoPointer;

    DemoFillRectangle(window, 0, 0, window->width, window->height, DEMO_PAPER);

    if (DemoPointerInside)
    {
        DemoFillDisc(window, DemoPointerX, DemoPointerY,
                     (DemoPointerHeld ? 12 : 7) * DemoScale, DemoAccent);
    }

    DemoPresent(window);
}

/* The keys: one tile per character typed, its colour from the character, so
 * that typing the same word twice draws the same tiles twice. */
static void DemoDrawKeys(void)
{
    DemoWindow *const window = &DemoKeys;
    const int32_t tile = 12 * DemoScale;
    const int32_t gap = 4 * DemoScale;
    const uint32_t colours[4] = { DemoAccent, DEMO_CORAL, DEMO_MINT, DEMO_INK };

    DemoFillRectangle(window, 0, 0, window->width, window->height, DEMO_PAPER);

    for (int32_t index = 0; index < DemoTypedCount; ++index)
    {
        const int32_t x = gap + (index * (tile + gap));
        const uint32_t colour = colours[(unsigned char)DemoTyped[index] % 4U];

        if (DemoTyped[index] == ' ')
        {
            continue;
        }

        DemoFillRectangle(window, x, gap, tile, tile, colour);
    }

    /* The cursor: a disc after the last tile, in the ink. */
    DemoFillDisc(window, gap + (DemoTypedCount * (tile + gap)) + (tile / 2), gap + (tile / 2),
                 2 * DemoScale, DEMO_INK);

    DemoPresent(window);
}

/* Acts upon a key; returns whether the tiles changed. */
static bool DemoHandleKey(const SyscallWindowEvent *event)
{
    if (event->key_pressed == 0U)
    {
        return false;
    }

    if (event->key_character == '\b')
    {
        if (DemoTypedCount != 0)
        {
            --DemoTypedCount;
        }
    }
    else if ((event->key_character == '\n') || (event->key_character == '\r'))
    {
        DemoTypedCount = 0;
    }
    else if ((event->key_character >= ' ') && (event->key_character <= '~') &&
             (DemoTypedCount < DEMO_KEYS_CAPACITY))
    {
        DemoTyped[DemoTypedCount] = event->key_character;
        ++DemoTypedCount;
    }
    else
    {
        return false;
    }

    return true;
}

/*
 * Reads /etc/desktop.conf. Everything it sets has a default already in place,
 * so a file that is absent or wrong leaves those defaults standing and the
 * faults are reported upon the standard error, which is the serial line.
 */
static void DemoReadConfiguration(void)
{
    const char *accent;

    if (!OxysConfigRead(&DemoConfig, DEMO_CONFIGURATION))
    {
        for (size_t index = 0U; index < OxysConfigFaultCount(&DemoConfig); ++index)
        {
            (void)fprintf(stderr, "windows: %s, line %lu: %s.\n", DEMO_CONFIGURATION,
                          (unsigned long)OxysConfigFaultLine(&DemoConfig, index),
                          OxysConfigFaultReason(&DemoConfig, index));
        }
    }

    {
        const long scale = OxysConfigNumber(&DemoConfig, "desktop", 0U, "scale", 0);

        /* A scale outside what the screen can show is refused rather than
         * obeyed: a window drawn at sixteen times the face would not fit upon
         * any screen this system has, and a desktop nobody can see is worse
         * than one that ignored a setting. */
        DemoScale = ((scale >= 1) && (scale <= 4)) ? (int32_t)scale : 0;
    }

    accent = OxysConfigValue(&DemoConfig, "desktop", 0U, "accent");

    if (accent != NULL)
    {
        unsigned long channel[3] = { 0UL, 0UL, 0UL };
        size_t taken = 0U;
        size_t at = 0U;
        bool digits = false;

        /* Three numbers, separated by whatever is not one — a comma, a space,
         * or both, because a person writing `79, 134, 247` and a person
         * writing `79 134 247` have each written the same colour. */
        while ((accent[at] != '\0') && (taken < 3U))
        {
            if ((accent[at] >= '0') && (accent[at] <= '9'))
            {
                channel[taken] = (channel[taken] * 10UL) + (unsigned long)(accent[at] - '0');
                digits = true;
            }
            else if (digits)
            {
                ++taken;
                digits = false;
            }

            ++at;
        }

        if (digits)
        {
            ++taken;
        }

        if ((taken == 3U) && (channel[0] <= 255UL) && (channel[1] <= 255UL) &&
            (channel[2] <= 255UL))
        {
            DemoAccent = (uint32_t)((channel[0] << 16) | (channel[1] << 8) | channel[2]);
        }
        else
        {
            (void)fprintf(stderr, "windows: %s: `accent` is not three numbers from 0 to 255; "
                                  "the default stands.\n", DEMO_CONFIGURATION);
        }
    }
}

int main(void)
{
    /*
     * The windows are placed by the screen, asked for, and drawn at twice the
     * size upon a screen at least 1024 wide: VirtualBox's 640 by 480 is where
     * windows sized for 1280 by 800 stood upon one another and the third was
     * confined to the edge with nothing of it to see.
     */
    SyscallWindowRectangle screen;
    int32_t left;
    int32_t top;

    if (OxysWindowScreen(&screen) != 0)
    {
        (void)fprintf(stderr, "windows: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    /*
     * The configuration of sub-task 9.4, read before anything is drawn. A scale
     * of zero — the default the file ships with — means the program chooses
     * from the screen it was given, which is what it did before there was a
     * file to say otherwise.
     */
    DemoReadConfiguration();

    if (DemoScale == 0)
    {
        DemoScale = (screen.width >= 1024) ? 2 : 1;
    }
    left = screen.width / 16;
    top = screen.height / 12;

    if (!DemoMake(&DemoFigure, left, top, 240 * DemoScale, 100 * DemoScale, "Oxys") ||
        !DemoMake(&DemoPointer, screen.width / 2, screen.height / 4, 180 * DemoScale,
                  130 * DemoScale, "Pointer") ||
        !DemoMake(&DemoKeys, left + (28 * DemoScale), top + (140 * DemoScale), 240 * DemoScale,
                  40 * DemoScale, "Keys"))
    {
        (void)fprintf(stderr, "windows: a window could not be made.\n");

        return EXIT_FAILURE;
    }

    DemoDrawFigure();
    DemoDrawPointer();
    DemoDrawKeys();

    /*
     * One wait, then everything that is already queued, then one drawing of
     * whatever changed: the pointer sends a hundred movements a second while a
     * hand moves, and a program that drew after each would carry its content
     * across a hundred times for positions the eye never saw, which is the
     * echo loop's reasoning of sub-task 6.5 applied from the far side.
     */
    for (;;)
    {
        SyscallWindowEvent event;
        bool pointer_changed = false;
        bool keys_changed = false;
        int64_t result = OxysWindowEvent((int64_t)SYSCALL_WINDOW_ANY, &event,
                                         SYSCALL_WINDOW_WAIT);

        if (result < 0)
        {
            /* EBADF: every window has been closed, and there is nothing left
             * to wait for. EINTR: a signal; there is nothing to do about it
             * either. */
            break;
        }

        while (result == 1)
        {
            if (event.kind == SYSCALL_WINDOW_EVENT_CLOSE)
            {
                DemoWindow *const closed =
                    (event.window == (uint32_t)DemoFigure.number)    ? &DemoFigure
                    : (event.window == (uint32_t)DemoPointer.number) ? &DemoPointer
                                                                     : &DemoKeys;

                (void)OxysWindowDestroy(closed->number);
                closed->number = -1;
            }
            else if (event.window == (uint32_t)DemoPointer.number)
            {
                switch (event.kind)
                {
                case SYSCALL_WINDOW_EVENT_POINTER_MOVE:
                case SYSCALL_WINDOW_EVENT_BUTTON_PRESS:
                case SYSCALL_WINDOW_EVENT_BUTTON_RELEASE:
                    DemoPointerX = event.x;
                    DemoPointerY = event.y;
                    DemoPointerHeld = (event.buttons != 0U);
                    DemoPointerInside = true;
                    pointer_changed = true;
                    break;

                default:
                    break;
                }
            }
            else if ((event.window == (uint32_t)DemoKeys.number) &&
                     (event.kind == SYSCALL_WINDOW_EVENT_KEY))
            {
                keys_changed = DemoHandleKey(&event) || keys_changed;
            }

            result = OxysWindowEvent((int64_t)SYSCALL_WINDOW_ANY, &event, 0U);
        }

        if (pointer_changed)
        {
            DemoDrawPointer();
        }

        if (keys_changed)
        {
            DemoDrawKeys();
        }
    }

    return EXIT_SUCCESS;
}
