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
#include <icon.h>
#include <logo.h>
#include <palette.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define SESSION_CONFIGURATION "/etc/session.conf"

/*
 * The panel's height, and the launcher's, in units of the scale.
 *
 * A launcher row is twenty-eight units where it was sixteen, because since
 * sub-task 9.6 an entry may carry a picture: twenty-four for the icon and four
 * for the space about it. A row the height of the icon would leave the icons
 * touching one another, which reads as one column of noise rather than as
 * three things.
 */
#define SESSION_PANEL_UNITS  14
#define SESSION_ENTRY_UNITS  28
#define SESSION_LAUNCH_WIDTH 56

/* The icon slot within a row: its extent in units, and the margin before it.
 * An icon of any extent is fitted to the slot, SessionDrawIcon; one drawn at
 * twice the units, as the shipped icons are, is drawn one to one at a scale of
 * two. ICON_EXTENT_MAXIMUM is what the library will hold at all. */
#define SESSION_ICON_UNITS  24
#define SESSION_ICON_MARGIN 2

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

/*
 * The palette, in the client's format, and every colour of it from art/
 * palette.h — the same header the kernel's boot screen and the window
 * manager's frames read. A second set of numbers here would be a desktop that
 * drifted from the screen shown a second before it.
 */
#define SESSION_GROUND OXYS_RGB(OXYS_GROUND_RED, OXYS_GROUND_GREEN, OXYS_GROUND_BLUE)
#define SESSION_PANEL  OXYS_RGB(OXYS_BAR_RED, OXYS_BAR_GREEN, OXYS_BAR_BLUE)
#define SESSION_QUIET  OXYS_RGB(OXYS_BAR_QUIET_RED, OXYS_BAR_QUIET_GREEN, OXYS_BAR_QUIET_BLUE)
#define SESSION_INK    OXYS_RGB(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE)
#define SESSION_DIM    OXYS_RGB(OXYS_DIM_RED, OXYS_DIM_GREEN, OXYS_DIM_BLUE)
#define SESSION_DISC   OXYS_RGB(OXYS_DISC_RED, OXYS_DISC_GREEN, OXYS_DISC_BLUE)
#define SESSION_PAPER  OXYS_RGB(OXYS_PAPER_RED, OXYS_PAPER_GREEN, OXYS_PAPER_BLUE)

typedef struct SessionEntry
{
    char name[CONFIG_VALUE_MAXIMUM + 1U];
    char run[CONFIG_VALUE_MAXIMUM + 1U];

    /*
     * The picture, read from the file the entry names, and whether there is
     * one. An entry without an icon is drawn without one and not with a
     * substitute: a launcher that invented a picture for every program would
     * be telling a person something it does not know.
     */
    OxysIcon picture;
    bool has_picture;
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
    const int32_t centre_x = SessionScreen.width / 2;
    const int32_t centre_y = SessionScreen.height / 2;
    const int32_t size = LOGO_UNITS * SessionScale;
    const int32_t left = centre_x - (size / 2);
    const int32_t top = centre_y - (24 * SessionScale) - (size / 2);
    /* Zero, and nothing drawn, for a mark wider than the tile: a scale this
     * session never sets. */
    const int32_t band = (size > 0) ? (int32_t)(SESSION_TILE / (uint32_t)size) : 0;

    SessionFill(SessionRoot, 0, 0, SessionScreen.width, SessionScreen.height, SESSION_GROUND);

    /*
     * The mark of art/logo.h, composed into the tile a band of rows at a time
     * and carried across in one blit for each band. Every pixel is mixed from
     * the ground, the disc and the ink in the proportions LogoSample gives,
     * so the edge of the disc is smooth rather than the staircase the table of
     * three states drew; and since the square about the mark is composed as
     * the ground, the whole square may be blitted without a pixel of it being
     * wrong. Ten blits for a mark of 192 pixels, where a fill for every run of
     * one colour would be thousands once the edge is mixed.
     *
     * It is the same mark, at the same size and in the same colours, as the
     * boot screen the kernel drew a moment before — art/palette.h and
     * art/logo.h are one copy shared by both, which is what keeps the
     * hand-over from looking like two pictures replacing each other rather
     * than one machine finishing what it started.
     */
    for (int32_t first = 0; (band > 0) && (first < size); first += band)
    {
        const int32_t rows = ((first + band) <= size) ? band : (size - first);
        SyscallWindowRectangle area;

        for (int32_t row = 0; row < rows; ++row)
        {
            for (int32_t column = 0; column < size; ++column)
            {
                unsigned covered;
                unsigned inked;

                LogoSample(column, first + row, size, &covered, &inked);
                SessionTile[(row * size) + column] =
                    LogoMixPacked(SESSION_GROUND, SESSION_DISC, SESSION_INK, covered, inked);
            }
        }

        area.x = left;
        area.y = top + first;
        area.width = size;
        area.height = rows;

        if (OxysWindowBlit(SessionRoot, &area, SessionTile) != 0)
        {
            break;
        }
    }

    /* Centred by measurement rather than by guess: the face is eight pixels
     * wide and every glyph of it the same, so a run of `n` characters at a
     * scale is exactly `n * 8 * scale` across. */
    {
        static const char wordmark[] = "OXYS-OS";
        const int32_t scale = SessionScale * 3;
        const int32_t width = (int32_t)(sizeof wordmark - 1U) * 8 * scale;

        SessionText(SessionRoot, centre_x - (width / 2), centre_y + (36 * SessionScale), wordmark,
                    SESSION_INK, SESSION_GROUND, scale);
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
                open ? SESSION_QUIET : SESSION_PANEL);
    SessionText(SessionPanel, inset * 2, inset, "OXYS", SESSION_INK,
                open ? SESSION_QUIET : SESSION_PANEL, SessionScale);

    /* The line beneath, which is what separates the panel from a window that
     * happens to be the same colour standing under it. */
    SessionFill(SessionPanel, 0, height - 1, SessionScreen.width, 1, SESSION_GROUND);
}

/*
 * Draws an icon at a position, fitted to the slot, over `paper`.
 *
 * It is composed into the tile and carried across in one blit. **The
 * transparency is resolved here and not by the window manager**: the protocol
 * carries pixels and has no notion of a pixel that is not there, so what a
 * caller means by "nothing" is "the colour behind me", and the caller is the
 * only one that knows what that is — SESSION.md, Section 8. OxysIconCompose
 * does the mixing, a pixel partly transparent becoming partly the paper, which
 * is what lets the edge of a picture be smooth.
 *
 * The icon is fitted to the slot whatever its extent — averaged down when it
 * is larger, repeated when it is smaller — so that a picture drawn at the
 * slot's own extent is drawn one to one, and a picture drawn at any other is
 * still the whole picture in the place the row was sized for.
 */
static void SessionDrawIcon(int64_t window, int32_t x, int32_t y, const OxysIcon *icon,
                            uint32_t paper)
{
    const int32_t slot = SESSION_ICON_UNITS * SessionScale;
    SyscallWindowRectangle area;

    if ((window < 0) || (icon == NULL) || ((uint32_t)(slot * slot) > SESSION_TILE))
    {
        return;
    }

    for (int32_t row = 0; row < slot; ++row)
    {
        for (int32_t column = 0; column < slot; ++column)
        {
            SessionTile[(row * slot) + column] = OxysIconCompose(
                icon, (uint32_t)column, (uint32_t)row, (uint32_t)slot, paper);
        }
    }

    area.x = x;
    area.y = y;
    area.width = slot;
    area.height = slot;

    (void)OxysWindowBlit(window, &area, SessionTile);
}

/* The launcher: one row per program, drawn into a panel-layer window. */
static void SessionDrawMenu(void)
{
    const int32_t row = SESSION_ENTRY_UNITS * SessionScale;
    const int32_t slot = SESSION_ICON_UNITS * SessionScale;
    const int32_t margin = SESSION_ICON_MARGIN * SessionScale;
    /* The text begins after the icon's slot whether or not an entry has a
     * picture, so that the names stand in one column and a launcher of three
     * entries does not read as three margins. */
    const int32_t text_x = margin + slot + margin;

    SessionFill(SessionMenu, 0, 0, SESSION_LAUNCH_WIDTH * 3 * SessionScale,
                row * (int32_t)SessionEntryCount, SESSION_PANEL);

    for (size_t index = 0U; index < SessionEntryCount; ++index)
    {
        const int32_t top = (int32_t)index * row;

        if (SessionEntries[index].has_picture)
        {
            SessionDrawIcon(SessionMenu, margin, top + ((row - slot) / 2),
                            &SessionEntries[index].picture, SESSION_PANEL);
        }

        SessionText(SessionMenu, text_x, top + ((row - (8 * SessionScale)) / 2),
                    SessionEntries[index].name, SESSION_INK, SESSION_PANEL, SessionScale);
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
        const char *const icon = OxysConfigValue(&SessionConfig, "launch", index, "icon");
        SessionEntry *const entry = &SessionEntries[SessionEntryCount];

        if (run == NULL)
        {
            (void)fprintf(stderr, "session: a launcher entry with no `run` is not offered.\n");
            continue;
        }

        SessionCopy(entry->run, CONFIG_VALUE_MAXIMUM, run);
        SessionCopy(entry->name, CONFIG_VALUE_MAXIMUM, (name != NULL) ? name : run);

        /*
         * The picture, of sub-task 9.6, read here rather than where it is drawn
         * — the launcher is drawn every time it opens and a file read at each
         * opening is a file read for nothing.
         *
         * **An icon that cannot be read costs the icon and not the entry.** A
         * launcher that refused to offer a program because its picture was
         * missing would be a desktop a person cannot use for a reason that has
         * nothing to do with the program; the fault is said plainly upon the
         * standard error and the entry is offered with no picture, which is
         * what an entry that named none gets.
         */
        entry->has_picture = false;

        if (icon != NULL)
        {
            entry->has_picture = OxysIconRead(&entry->picture, icon);

            if (!entry->has_picture)
            {
                (void)fprintf(stderr, "session: %s: the icon could not be read; the entry "
                                      "stands without one.\n", icon);
            }
        }

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
