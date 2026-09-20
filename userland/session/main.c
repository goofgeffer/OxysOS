/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/session/main.c
 * Purpose: The session of sub-task 9.5: the program that claims the display,
 *          paints the desktop root beneath every window, holds the panel above
 *          them, and starts the programs a person chooses from its launcher.
 * Key functions: main, SessionClaim, SessionDrawRoot, SessionDrawPanel,
 *          SessionOpenLauncher, SessionLaunch, SessionHandlePress,
 *          SessionReapChildren.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: `window_session`, the three layers, and
 *     `window_text`.
 *   - libc/include/config.h: the format `/etc/session.conf` is written in.
 *   - docs/design/SESSION.md: the design, the appearance, and every assertion
 *     made upon this.
 *
 * What a session is, and what it is not.
 *
 *   It is the one program that owns the screen: it claims the session, and the
 *   kernel then refuses a root or a panel to every other program. It is not a
 *   window manager — the stacking, the focus and the routing are the kernel's,
 *   sub-task 9.1 — and it is not a supervisor: `init` starts it and starts it
 *   again if it ends, sub-task 9.3, which is why nothing here tries to survive
 *   its own faults.
 *
 * Why the launcher is a panel window and not a window of its own.
 *
 *   A menu that were an ordinary window would stack among the programs' windows
 *   and the first press upon one of them would bury it. It is drawn into the
 *   panel's own layer instead, where nothing a program does can put anything
 *   over it, which is the whole reason the layers exist.
 */

#include <config.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define SESSION_CONFIGURATION "/etc/session.conf"

/* The panel's height, and the launcher's, in units of the scale. */
#define SESSION_PANEL_UNITS  14
#define SESSION_ENTRY_UNITS  16
#define SESSION_LAUNCH_WIDTH 56

/*
 * The least extent a window may have, which the panel's height must not fall
 * below. Fourteen units at a scale of one is fourteen pixels, and the manager
 * refuses a window shorter than sixteen — so upon a screen small enough to be
 * drawn at that scale the panel was refused, the session exited, and `init`
 * started it five times and gave up. VirtualBox's 640 by 480 is where that
 * happened; the failure was reported plainly by the supervision of sub-task
 * 9.3 and by nothing else, which is what that bound is for.
 */
#define SESSION_EXTENT_MINIMUM 16

/* How many programs the launcher may offer. */
#define SESSION_ENTRIES_MAXIMUM 8

/* The palette, in the client's format. The ground is the one the kernel's boot
 * screen uses, so that the boot screen and the desktop are the same colour and
 * the hand-over between them is not a flash. */
#define SESSION_GROUND UINT32_C(0x002B3440)
#define SESSION_PANEL  UINT32_C(0x00202830)
#define SESSION_INK    UINT32_C(0x00E6E9EE)
#define SESSION_DIM    UINT32_C(0x00969EAA)
#define SESSION_ACCENT UINT32_C(0x004F86F7)
#define SESSION_PAPER  UINT32_C(0x00F5F3EE)

typedef struct SessionEntry
{
    char name[CONFIG_VALUE_MAXIMUM + 1U];
    char run[CONFIG_VALUE_MAXIMUM + 1U];
} SessionEntry;

static SessionEntry SessionEntries[SESSION_ENTRIES_MAXIMUM];
static size_t SessionEntryCount;

static OxysConfig SessionConfig;

static SyscallWindowRectangle SessionScreen;
static int32_t SessionScale = 2;

static int64_t SessionRoot = -1;
static int64_t SessionPanel = -1;
static int64_t SessionMenu = -1;

/* A child ended: the loop reaps when it next goes round. The handler does
 * nothing else, for the reason `init`'s handlers do nothing else. */
static volatile sig_atomic_t SessionChildEnded;

static void SessionChildHandler(int signal)
{
    (void)signal;
    SessionChildEnded = 1;
}

/* ------------------------------------------------------------- drawing */

/* One rectangle of one colour, as a blit of a small tile repeated: the client
 * protocol carries pixels and nothing else, so a fill is a buffer. */
#define SESSION_TILE 4096U
static uint32_t SessionTile[SESSION_TILE];

static void SessionFill(int64_t window, int32_t x, int32_t y, int32_t width, int32_t height,
                        uint32_t colour)
{
    SyscallWindowRectangle area;
    int32_t drawn = 0;

    if ((window < 0) || (width <= 0) || (height <= 0))
    {
        return;
    }

    /* The tile is filled once and blitted in bands as tall as it can hold, so
     * that a panel the width of the screen costs one buffer and not one the
     * size of the panel. */
    for (size_t index = 0U; index < SESSION_TILE; ++index)
    {
        SessionTile[index] = colour;
    }

    while (drawn < height)
    {
        int32_t band = (int32_t)(SESSION_TILE / (uint32_t)width);

        if (band < 1)
        {
            return; /* Wider than the tile: nothing this program draws is. */
        }

        if (band > (height - drawn))
        {
            band = height - drawn;
        }

        area.x = x;
        area.y = y + drawn;
        area.width = width;
        area.height = band;

        if (OxysWindowBlit(window, &area, SessionTile) != 0)
        {
            return;
        }

        drawn += band;
    }
}

static void SessionText(int64_t window, int32_t x, int32_t y, const char *text, uint32_t ink,
                        uint32_t paper, int32_t scale)
{
    SyscallWindowText placement;

    placement.x = x;
    placement.y = y;
    placement.ink = ink;
    placement.paper = paper;
    placement.scale = scale;

    (void)OxysWindowText(window, &placement, text);
}

/*
 * The desktop root: the ground, and the mark the boot screen drew, so that what
 * a person saw while the machine started and what they see when it has started
 * are recognisably the same system.
 */
static void SessionDrawRoot(void)
{
    const int32_t unit = 6 * SessionScale;
    const int32_t centre_x = SessionScreen.width / 2;
    const int32_t centre_y = (SessionScreen.height / 2) - (4 * unit);
    static const int32_t points[8][2] = {
        { 64, 0 },   { 45, 45 },   { 0, 64 },   { -45, 45 },
        { -64, 0 },  { -45, -45 }, { 0, -64 },  { 45, -45 }
    };
    const uint32_t ring[3] = { SESSION_ACCENT, UINT32_C(0x00F26B5B), UINT32_C(0x003FBF9F) };

    SessionFill(SessionRoot, 0, 0, SessionScreen.width, SessionScreen.height, SESSION_GROUND);

    /* The mark, as squares rather than discs: a program has no circle, the
     * primitives being the kernel's, and a ring of squares about a square is
     * the same figure at this size. docs/design/SESSION.md, Section 3.2. */
    for (size_t index = 0U; index < 8U; ++index)
    {
        SessionFill(SessionRoot, centre_x + ((points[index][0] * (7 * unit)) / 64) - (unit / 2),
                    centre_y + ((points[index][1] * (7 * unit)) / 64) - (unit / 2), unit, unit,
                    ring[index % 3U]);
    }

    SessionFill(SessionRoot, centre_x - unit, centre_y - unit, 2 * unit, 2 * unit, SESSION_PAPER);
    SessionFill(SessionRoot, centre_x - (unit / 2), centre_y - (unit / 2), unit, unit,
                SESSION_ACCENT);

    /* Centred by measurement rather than by guess: the face is eight pixels
     * wide and every glyph of it the same, so a run of `n` characters at a
     * scale is exactly `n * 8 * scale` across. */
    {
        static const char wordmark[] = "OXYS-OS";
        const int32_t scale = SessionScale * 2;
        const int32_t width = (int32_t)(sizeof wordmark - 1U) * 8 * scale;

        SessionText(SessionRoot, centre_x - (width / 2), centre_y + (10 * unit), wordmark,
                    SESSION_DIM, SESSION_GROUND, scale);
    }
}

/*
 * The panel: a bar across the top, with the launcher's name at its left. It is
 * drawn whole each time, there being one of it and it being a few thousand
 * pixels.
 */
/* The panel's height in pixels, never below what a window may be. */
static int32_t SessionPanelHeight(void)
{
    const int32_t height = SESSION_PANEL_UNITS * SessionScale;

    return (height < SESSION_EXTENT_MINIMUM) ? SESSION_EXTENT_MINIMUM : height;
}

static void SessionDrawPanel(bool open)
{
    const int32_t height = SessionPanelHeight();
    const int32_t inset = 3 * SessionScale;

    SessionFill(SessionPanel, 0, 0, SessionScreen.width, height, SESSION_PANEL);
    SessionFill(SessionPanel, 0, 0, SESSION_LAUNCH_WIDTH * SessionScale, height,
                open ? SESSION_ACCENT : SESSION_PANEL);
    SessionText(SessionPanel, inset * 2, inset, "OXYS", open ? SESSION_PAPER : SESSION_INK,
                open ? SESSION_ACCENT : SESSION_PANEL, SessionScale);

    /* The line beneath, which is what separates the panel from a window that
     * happens to be the same colour standing under it. */
    SessionFill(SessionPanel, 0, height - 1, SessionScreen.width, 1, SESSION_GROUND);
}

/* The launcher: one row per program, drawn into a panel-layer window. */
static void SessionDrawMenu(void)
{
    const int32_t row = SESSION_ENTRY_UNITS * SessionScale;
    const int32_t inset = 3 * SessionScale;

    SessionFill(SessionMenu, 0, 0, SESSION_LAUNCH_WIDTH * 3 * SessionScale,
                row * (int32_t)SessionEntryCount, SESSION_PANEL);

    for (size_t index = 0U; index < SessionEntryCount; ++index)
    {
        SessionText(SessionMenu, inset * 2, (int32_t)index * row + inset, SessionEntries[index].name,
                    SESSION_INK, SESSION_PANEL, SessionScale);
    }
}

/* ------------------------------------------------------- the launcher */

static void SessionCloseMenu(void)
{
    if (SessionMenu >= 0)
    {
        (void)OxysWindowDestroy(SessionMenu);
        SessionMenu = -1;
        SessionDrawPanel(false);
    }
}

static void SessionOpenMenu(void)
{
    SyscallWindowRectangle geometry;

    if ((SessionMenu >= 0) || (SessionEntryCount == 0U))
    {
        return;
    }

    geometry.x = 0;
    geometry.y = SessionPanelHeight();
    geometry.width = SESSION_LAUNCH_WIDTH * 3 * SessionScale;
    geometry.height = SESSION_ENTRY_UNITS * SessionScale * (int32_t)SessionEntryCount;

    if (geometry.height < SESSION_EXTENT_MINIMUM)
    {
        geometry.height = SESSION_EXTENT_MINIMUM;
    }

    SessionMenu = OxysWindowCreate(&geometry, "launcher", SYSCALL_WINDOW_LAYER_PANEL);

    if (SessionMenu < 0)
    {
        (void)fprintf(stderr, "session: the launcher could not be opened.\n");

        return;
    }

    SessionDrawMenu();
    SessionDrawPanel(true);
}

/* Starts a program, and does not wait for it: the session reaps at its leisure
 * and a program that takes a moment to draw must not stop the panel. */
static void SessionLaunch(const char *path)
{
    const int64_t child = OxysFork();

    if (child == 0)
    {
        char *const argument_vector[] = { (char *)path, NULL };

        (void)OxysExecve(path, argument_vector, NULL);
        OxysExit(127);
    }

    if (child < 0)
    {
        (void)fprintf(stderr, "session: %s could not be started.\n", path);
    }
}

/*
 * Collects whatever the launcher started and has ended. A session that never
 * reaped would fill the process table with the programs a person had opened and
 * closed — `init` adopts an orphan only when its parent ends, and the session
 * does not end.
 */
static void SessionReapChildren(void)
{
    int64_t status = 0;

    while (OxysWaitFor((int64_t)-1, &status, SYSCALL_WAIT_NO_HANG) > 0)
    {
    }

    SessionChildEnded = 0;
}

/* ------------------------------------------------------- configuration */

static void SessionCopy(char *destination, size_t capacity, const char *source)
{
    size_t index = 0U;

    while ((source[index] != '\0') && (index < capacity))
    {
        destination[index] = source[index];
        ++index;
    }

    destination[index] = '\0';
}

static void SessionReadConfiguration(void)
{
    size_t blocks;

    if (!OxysConfigRead(&SessionConfig, SESSION_CONFIGURATION))
    {
        for (size_t index = 0U; index < OxysConfigFaultCount(&SessionConfig); ++index)
        {
            (void)fprintf(stderr, "session: %s, line %lu: %s.\n", SESSION_CONFIGURATION,
                          (unsigned long)OxysConfigFaultLine(&SessionConfig, index),
                          OxysConfigFaultReason(&SessionConfig, index));
        }
    }

    {
        const long scale = OxysConfigNumber(&SessionConfig, "session", 0U, "scale", 0);

        SessionScale = ((scale >= 1) && (scale <= 4))
                           ? (int32_t)scale
                           : ((SessionScreen.width >= 1024) ? 2 : 1);
    }

    blocks = OxysConfigCount(&SessionConfig, "launch");

    for (size_t index = 0U; (index < blocks) && (SessionEntryCount < SESSION_ENTRIES_MAXIMUM);
         ++index)
    {
        const char *const run = OxysConfigValue(&SessionConfig, "launch", index, "run");
        const char *const name = OxysConfigValue(&SessionConfig, "launch", index, "name");

        if (run == NULL)
        {
            (void)fprintf(stderr, "session: a launcher entry with no `run` is not offered.\n");
            continue;
        }

        SessionCopy(SessionEntries[SessionEntryCount].run, CONFIG_VALUE_MAXIMUM, run);
        SessionCopy(SessionEntries[SessionEntryCount].name, CONFIG_VALUE_MAXIMUM,
                    (name != NULL) ? name : run);
        ++SessionEntryCount;
    }
}

/* ------------------------------------------------------------ the loop */

static void SessionHandlePress(const SyscallWindowEvent *event)
{
    if (event->window == (uint32_t)SessionPanel)
    {
        if (event->x < (SESSION_LAUNCH_WIDTH * SessionScale))
        {
            if (SessionMenu >= 0)
            {
                SessionCloseMenu();
            }
            else
            {
                SessionOpenMenu();
            }
        }
        else
        {
            SessionCloseMenu();
        }

        return;
    }

    if (event->window == (uint32_t)SessionMenu)
    {
        const int32_t row = SESSION_ENTRY_UNITS * SessionScale;
        const size_t chosen = (event->y >= 0) ? ((size_t)(event->y / row)) : SessionEntryCount;

        if (chosen < SessionEntryCount)
        {
            SessionLaunch(SessionEntries[chosen].run);
        }

        SessionCloseMenu();

        return;
    }

    /* A press upon the root closes the launcher, which is what a person means
     * by clicking away from an open menu. */
    if (event->window == (uint32_t)SessionRoot)
    {
        SessionCloseMenu();
    }
}

int main(void)
{
    if (OxysWindowScreen(&SessionScreen) != 0)
    {
        (void)fprintf(stderr, "session: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    /*
     * The claim comes first. Everything below it — the root, the panel — is
     * refused to a program that does not hold the session, so a failure here is
     * a failure to be the desktop and there is nothing further to attempt.
     */
    if (OxysWindowSession() != 0)
    {
        (void)fprintf(stderr, "session: another program holds the session: %s\n",
                      strerror(errno));

        return EXIT_FAILURE;
    }

    (void)signal(SIGCHLD, SessionChildHandler);

    SessionReadConfiguration();

    {
        SyscallWindowRectangle geometry;

        geometry.x = 0;
        geometry.y = 0;
        geometry.width = SessionScreen.width;
        geometry.height = SessionScreen.height;
        SessionRoot = OxysWindowCreate(&geometry, "root", SYSCALL_WINDOW_LAYER_ROOT);

        geometry.height = SessionPanelHeight();
        SessionPanel = OxysWindowCreate(&geometry, "panel", SYSCALL_WINDOW_LAYER_PANEL);
    }

    if ((SessionRoot < 0) || (SessionPanel < 0))
    {
        (void)fprintf(stderr, "session: the root or the panel could not be made.\n");

        return EXIT_FAILURE;
    }

    SessionDrawRoot();
    SessionDrawPanel(false);

    for (;;)
    {
        SyscallWindowEvent event;
        int64_t result;

        if (SessionChildEnded != 0)
        {
            SessionReapChildren();
        }

        result = OxysWindowEvent((int64_t)SYSCALL_WINDOW_ANY, &event, SYSCALL_WINDOW_WAIT);

        if (result < 0)
        {
            /* EINTR is a child that ended, which the top of the loop reaps.
             * EBADF is a session with no windows left, which cannot happen
             * while the root stands and means something is badly wrong. */
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
                SessionHandlePress(&event);
            }

            result = OxysWindowEvent((int64_t)SYSCALL_WINDOW_ANY, &event, 0U);
        }
    }

    return EXIT_SUCCESS;
}
