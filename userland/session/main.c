/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/session/main.c
 * Purpose: The session of sub-task 9.5: the program that claims the display,
 *          paints the desktop root beneath every window, holds the panel above
 *          them, and starts the programs a person chooses from its launcher.
 * Key functions: main, SessionClaim, SessionDrawRoot, SessionDrawPanel,
 *          SessionOpenLauncher, SessionLaunch, SessionHandlePress,
 *          SessionReapChildren, SessionDrawBackground, SessionDrawTasks,
 *          SessionRefreshTasks, SessionDrawClock.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: `window_session`, the three layers, and
 *     `window_text`; and `window_state` and `window_list`, by which the panel
 *     lists the windows and brings a minimised one back.
 *   - libc/include/config.h: the format `/etc/session.conf` is written in.
 *   - libc/include/image.h: the background's format and the scaler that
 *     covers the screen with it.
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
#include <image.h>
#include <logo.h>
#include <palette.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <time.h>

#define SESSION_CONFIGURATION "/etc/session.conf"

/* What the launcher offers where the configuration offers nothing: the
 * terminal, from which the configuration can be mended. SessionReadConfiguration
 * says why it is the terminal alone. */
#define SESSION_FALLBACK_NAME "Terminal"
#define SESSION_FALLBACK_RUN  "/bin/terminal"
#define SESSION_FALLBACK_ICON "/share/icons/terminal.oxi"

/* The shipped copy of the configuration, which a persistent `/etc` does not
 * cover, read where the person's offers nothing: SessionReadConfiguration. */
#define SESSION_DEFAULTS "/share/defaults/etc/session.conf"

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
 * The list of windows upon the panel, of 2026-09-23: one button per ordinary
 * window, in units of the scale, beginning after the launcher's name. It is
 * what brings a minimised window back — nothing else can, the window being
 * neither drawn nor hit — and so it lists every window and not only the
 * minimised ones, a list that changed its length whenever a window was hidden
 * being a list a person could not learn the places of.
 */
#define SESSION_TASK_UNITS   72
#define SESSION_TASK_GAP     2
#define SESSION_TASKS_MAXIMUM 16U

/*
 * The background, of 2026-09-23: the file `/etc/session.conf` names, read once
 * at start into this buffer, which the parsed image points into for as long as
 * the session runs. A mebibyte holds the photograph that ships, some 760 KiB,
 * with room for one a little larger.
 */
#define SESSION_BACKGROUND_BYTES (1024U * 1024U)

/* The band the root is composed in when a background covers it: many rows at
 * a time, so that a screen of 800 rows is a few dozen blits and not hundreds. */
#define SESSION_BAND 65536U

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

static SyscallWindowEntry SessionTasks[SESSION_TASKS_MAXIMUM];
static size_t SessionTaskCount;

static uint8_t SessionBackgroundBytes[SESSION_BACKGROUND_BYTES];
static OxysImage SessionBackground;
static bool SessionHasBackground;

/* The background's path as last read, empty for none, so that the launcher's
 * reading at every opening reads the image only when the path has changed. */
static char SessionBackgroundPath[CONFIG_VALUE_MAXIMUM + 1U];

/* Whether the shipped configuration stands in for the person's. */
static bool SessionUsingDefaults;

/* Whether the shipped background stands in for one the person's file names
 * and that could not be read. */
static bool SessionBackgroundIsDefault;
static OxysImageScaler SessionScaler;
static uint32_t SessionBand[SESSION_BAND];

/* A child ended: the loop reaps when it next goes round. The handler does
 * nothing else, for the reason `init`'s handlers do nothing else. */
static volatile sig_atomic_t SessionChildEnded;

static void SessionChildHandler(int signal)
{
    (void)signal;
    SessionChildEnded = 1;
}

/* The clock of sub-task 9.7: the alarm the session asks for at each minute,
 * and nothing else done in the handler, for the reason `init`'s handlers do
 * nothing else. */
static volatile sig_atomic_t SessionClockDue;

static void SessionAlarmHandler(int signal)
{
    (void)signal;
    SessionClockDue = 1;
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
 * The background, scaled to cover the screen and composed a band of rows at a
 * time. False where it cannot be begun, upon which the root falls back to the
 * ground and the mark rather than being left undrawn.
 */
static bool SessionDrawBackground(void)
{
    const int32_t width = SessionScreen.width;
    const int32_t height = SessionScreen.height;
    const int32_t band = (width > 0) ? (int32_t)(SESSION_BAND / (uint32_t)width) : 0;

    if ((band < 1) ||
        !OxysImageScalerBegin(&SessionScaler, &SessionBackground, (uint32_t)width,
                              (uint32_t)height))
    {
        return false;
    }

    for (int32_t first = 0; first < height; first += band)
    {
        const int32_t rows = ((first + band) <= height) ? band : (height - first);
        SyscallWindowRectangle area;

        for (int32_t row = 0; row < rows; ++row)
        {
            (void)OxysImageScalerRow(&SessionScaler, &SessionBand[row * width]);
        }

        area.x = 0;
        area.y = first;
        area.width = width;
        area.height = rows;

        if (OxysWindowBlit(SessionRoot, &area, SessionBand) != 0)
        {
            break;
        }
    }

    return true;
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

    /*
     * A background, where one is named and could be read, is the whole of the
     * root: the mark and the wordmark are not drawn over it. The background is
     * a drawing somebody chose for the desktop, and the mark upon it would be
     * a second picture placed over the first by nobody's choice. The boot
     * screen still carries the mark; the hand-over to the desktop is then a
     * change of picture, which is what choosing a background asks for.
     */
    if (SessionHasBackground && SessionDrawBackground())
    {
        return;
    }

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


/*
 * The clock's width upon the panel: five characters, `HH:MM`, with an inset
 * either side. The list of windows stops short of it, so that a long list is
 * cut before the time rather than drawn under it.
 */
static int32_t SessionClockWidth(void)
{
    return (5 * 8 * SessionScale) + (2 * 3 * SessionScale * 2);
}

/*
 * The notice upon the panel that something shipped stands in for what the
 * person's file names: the whole configuration, or the background alone.
 * NULL where nothing does.
 */
#define SESSION_NOTICE            "using defaults"
#define SESSION_NOTICE_BACKGROUND "default background"

static const char *SessionNotice(void)
{
    if (SessionUsingDefaults)
    {
        return SESSION_NOTICE;
    }

    return SessionBackgroundIsDefault ? SESSION_NOTICE_BACKGROUND : NULL;
}

/* The notice's width, where it is shown, and nothing where it is not. */
static int32_t SessionNoticeWidth(void)
{
    const char *const notice = SessionNotice();

    return (notice != NULL)
               ? (((int32_t)strlen(notice) * 8 * SessionScale) + (4 * 3 * SessionScale))
               : 0;
}

/*
 * The clock, at the right of the panel: the hours and minutes of the machine's
 * clock, as it holds them — there is no time zone here, SYSCALL_TIME — and
 * nothing where the machine gave no time. Seconds are not shown: a panel
 * redrawn every second is a blit every second for a digit nobody reads.
 *
 * It asks for SIGALRM at the start of the next minute, from the seconds the
 * kernel counts; so the minute changes upon the panel within a second of the
 * clock's, the kernel having read the clock at a whole second at start.
 */
static void SessionDrawClock(bool open)
{
    const int32_t inset = 3 * SessionScale;
    const int32_t width = SessionClockWidth();
    const int32_t x = SessionScreen.width - width;
    const time_t now = time(NULL);
    struct tm broken;
    char text[6];

    (void)open;

    if ((now < 0) || (gmtime_r(&now, &broken) == NULL))
    {
        return;
    }

    text[0] = (char)('0' + (broken.tm_hour / 10));
    text[1] = (char)('0' + (broken.tm_hour % 10));
    text[2] = ':';
    text[3] = (char)('0' + (broken.tm_min / 10));
    text[4] = (char)('0' + (broken.tm_min % 10));
    text[5] = '\0';

    SessionFill(SessionPanel, x, 0, width, SessionPanelHeight() - 1, SESSION_PANEL);
    SessionText(SessionPanel, x + (inset * 2), inset, text, SESSION_INK, SESSION_PANEL,
                SessionScale);

    (void)OxysAlarm((uint64_t)(60 - (now % 60)) * 1000U);
}
static void SessionDrawTasks(void);
static bool SessionReadConfiguration(bool starting);

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

    SessionDrawTasks();
    SessionDrawClock(open);

    /* The notice that the shipped configuration stands in for the person's,
     * left of the clock: upon a desktop it is the one place a person sees it,
     * the standard error going to the serial line. */
    if (SessionNotice() != NULL)
    {
        SessionText(SessionPanel,
                    SessionScreen.width - SessionClockWidth() - SessionNoticeWidth() +
                        (2 * 3 * SessionScale),
                    3 * SessionScale, SessionNotice(), SESSION_DIM, SESSION_PANEL, SessionScale);
    }
}

/* Where the list of windows begins upon the panel, and how wide one of its
 * buttons is with the gap after it. */
static int32_t SessionTaskLeft(void)
{
    return (SESSION_LAUNCH_WIDTH + SESSION_TASK_GAP) * SessionScale;
}

static int32_t SessionTaskStride(void)
{
    return (SESSION_TASK_UNITS + SESSION_TASK_GAP) * SessionScale;
}

/* How many buttons fit upon the panel: a list longer than the screen is cut
 * at its edge rather than drawn over it. */
static size_t SessionTasksShown(void)
{
    const int32_t room =
        SessionScreen.width - SessionTaskLeft() - SessionClockWidth() - SessionNoticeWidth();
    const int32_t fit = (room > 0) ? ((room + (SESSION_TASK_GAP * SessionScale)) /
                                      SessionTaskStride())
                                   : 0;

    return ((size_t)fit < SessionTaskCount) ? (size_t)fit : SessionTaskCount;
}

/*
 * The list's buttons: the window holding the focus drawn upon the quiet colour
 * the open launcher is, a minimised one with its title dimmed, and every title
 * cut to the button rather than run past it.
 */
static void SessionDrawTasks(void)
{
    const int32_t height = SessionPanelHeight();
    const int32_t inset = 3 * SessionScale;
    const int32_t width = SESSION_TASK_UNITS * SessionScale;
    const size_t characters = (size_t)((width - (2 * inset)) / (8 * SessionScale));
    const size_t shown = SessionTasksShown();

    for (size_t index = 0U; index < shown; ++index)
    {
        const SyscallWindowEntry *const task = &SessionTasks[index];
        const int32_t x = SessionTaskLeft() + ((int32_t)index * SessionTaskStride());
        const bool focused = (task->flags & SYSCALL_WINDOW_ENTRY_FOCUSED) != 0U;
        const bool minimised = (task->flags & SYSCALL_WINDOW_ENTRY_MINIMISED) != 0U;
        const uint32_t paper = focused ? SESSION_QUIET : SESSION_PANEL;
        char title[SYSCALL_WINDOW_TITLE_MAXIMUM + 1U];
        size_t length = 0U;

        while ((task->title[length] != '\0') && (length < characters) &&
               (length < SYSCALL_WINDOW_TITLE_MAXIMUM))
        {
            title[length] = task->title[length];
            ++length;
        }

        title[length] = '\0';

        SessionFill(SessionPanel, x, 0, width, height - 1, paper);
        SessionFill(SessionPanel, x - SessionScale, inset, 1,
                    height - (2 * inset), SESSION_GROUND);
        SessionText(SessionPanel, x + inset, inset, title, minimised ? SESSION_DIM : SESSION_INK,
                    paper, SessionScale);
    }
}

/* Asks the kernel for the windows again, and draws the panel with them. */
static void SessionRefreshTasks(void)
{
    const int64_t count = OxysWindowList(SessionTasks, SESSION_TASKS_MAXIMUM);

    SessionTaskCount = (count < 0) ? 0U
                       : ((uint64_t)count > SESSION_TASKS_MAXIMUM) ? SESSION_TASKS_MAXIMUM
                                                                  : (size_t)count;
    SessionDrawPanel(SessionMenu >= 0);
}

/*
 * Draws an icon at a position, fitted to the slot, over `paper`.
 *
 * It is composed into the tile and carried across in one blit. **The
 * transparency is resolved here and not by the window manager**: the protocol
 * carries pixels and has no notion of a pixel that is not there, so what a
 * caller means by "nothing" is "the colour behind me", and the caller is the
 * only one that knows what that is — SESSION.md. OxysIconCompose
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

    if (SessionMenu >= 0)
    {
        return;
    }

    /*
     * The configuration is read again at every opening, since 2026-09-24, so
     * that an entry added or removed by an edit is offered at the next press,
     * and a background named by an edit is drawn then — without the session
     * being started again, which was the only way before and which nothing
     * upon the desktop does. SessionReadConfiguration never leaves the
     * launcher empty.
     */
    if (SessionReadConfiguration(false))
    {
        SessionDrawRoot();
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

/*
 * Reads one configuration file into SessionConfig, saying each fault upon the
 * standard error, and returns how many `[launch]` blocks of it name something
 * to run — the measure of whether the file offers a launcher at all.
 */
static size_t SessionReadFile(const char *path)
{
    size_t usable = 0U;

    if (!OxysConfigRead(&SessionConfig, path))
    {
        for (size_t index = 0U; index < OxysConfigFaultCount(&SessionConfig); ++index)
        {
            (void)fprintf(stderr, "session: %s, line %lu: %s.\n", path,
                          (unsigned long)OxysConfigFaultLine(&SessionConfig, index),
                          OxysConfigFaultReason(&SessionConfig, index));
        }
    }

    for (size_t index = 0U; index < OxysConfigCount(&SessionConfig, "launch"); ++index)
    {
        if (OxysConfigValue(&SessionConfig, "launch", index, "run") != NULL)
        {
            ++usable;
        }
    }

    return usable;
}

/*
 * The background, named by a path as an icon is, read only where the path is
 * not the one already read — a background is seventy kilobytes to read and a
 * screen to draw, and the launcher asks for the configuration every time it
 * opens. Returns whether the root must be drawn again.
 *
 * **One that cannot be read falls back to the shipped background**, the one
 * SESSION_DEFAULTS names, and the panel says `default background` while it
 * does. A path left stale by a background renamed in an update, or typed
 * wrongly, would otherwise leave the desktop bare with the reason only upon the
 * standard error, which upon a desktop nobody reads. Where the shipped one
 * cannot be read either, the root is the ground and the mark, which is what a
 * file naming no background gets; naming none is a choice, and no fallback is
 * made for it.
 *
 * A path that failed is not tried again while its fallback stands: a failed
 * read empties the one image buffer, so each retry would mean reading the
 * shipped background again. Mending the line changes the path, which is read
 * at the next opening.
 *
 * The shipped file is read into SessionConfig, so this runs after the entries,
 * which copy what they need out of it.
 */
static bool SessionLoadBackground(void)
{
    const char *const background = OxysConfigValue(&SessionConfig, "session", 0U, "background");
    const char *const wanted = (background != NULL) ? background : "";
    const bool had = SessionHasBackground;

    if ((strcmp(wanted, SessionBackgroundPath) == 0) &&
        ((wanted[0] == '\0') || SessionHasBackground))
    {
        return false;
    }

    SessionCopy(SessionBackgroundPath, sizeof SessionBackgroundPath, wanted);
    SessionHasBackground = false;
    SessionBackgroundIsDefault = false;

    if (SessionBackgroundPath[0] == '\0')
    {
        return had;
    }

    SessionHasBackground = OxysImageRead(&SessionBackground, SessionBackgroundPath,
                                         SessionBackgroundBytes, sizeof SessionBackgroundBytes);

    if (!SessionHasBackground)
    {
        const char *shipped;

        (void)SessionReadFile(SESSION_DEFAULTS);
        shipped = OxysConfigValue(&SessionConfig, "session", 0U, "background");

        if ((shipped != NULL) && (strcmp(shipped, SessionBackgroundPath) != 0) &&
            OxysImageRead(&SessionBackground, shipped, SessionBackgroundBytes,
                          sizeof SessionBackgroundBytes))
        {
            SessionHasBackground = true;
            SessionBackgroundIsDefault = true;
            (void)fprintf(stderr, "session: %s: the background could not be read; the "
                                  "shipped %s is drawn until the `background` line of %s "
                                  "is mended.\n",
                          SessionBackgroundPath, shipped, SESSION_CONFIGURATION);
        }
        else
        {
            (void)fprintf(stderr, "session: %s: the background could not be read; the "
                                  "desktop is drawn without it.\n", SessionBackgroundPath);
        }
    }

    return had || SessionHasBackground;
}

/* The launcher's entries, from whatever SessionConfig now holds. */
static void SessionLoadEntries(void)
{
    const size_t blocks = OxysConfigCount(&SessionConfig, "launch");

    SessionEntryCount = 0U;

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
         * **An icon that cannot be read costs the icon and not the entry.** A
         * launcher that refused to offer a program because its picture was
         * missing would be a desktop a person cannot use for a reason that has
         * nothing to do with the program; the fault is said plainly upon the
         * standard error and the entry is offered with no picture, which is
         * what an entry that named none gets. It is read with the entries, at
         * every opening since 2026-09-24, so that an icon named by an edit is
         * drawn at the next.
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

/*
 * Reads the configuration: at start, and since 2026-09-24 every time the
 * launcher opens, so that an edit to `/etc/session.conf` is seen at the next
 * press rather than at the next start of the session. The file is a few
 * kilobytes and a press is a person's, so the reading costs nothing anybody
 * will notice. The scale is taken at start alone: the panel and every window
 * of the session are sized by it, and a scale changed under them would be a
 * panel of one size holding a launcher of another. Returns whether the root
 * must be drawn again.
 *
 * **A file that offers nothing falls back to the shipped one.** A save cut
 * short, a file emptied, a file removed: each leaves a launcher with nothing in
 * it, and a person at a desktop with an empty launcher has no way to reach the
 * terminal that would mend it. The shipped copy at SESSION_DEFAULTS is read
 * instead — the whole of it, the background with the entries — the panel says
 * `using defaults` while it is, and the standard error says which file to
 * mend. Where even that offers nothing, the terminal alone, which is the
 * repair.
 */
static bool SessionReadConfiguration(bool starting)
{
    bool defaults = false;
    bool redraw;

    if (SessionReadFile(SESSION_CONFIGURATION) == 0U)
    {
        defaults = true;

        if (!SessionUsingDefaults)
        {
            (void)fprintf(stderr, "session: %s offers nothing to launch; the shipped %s is "
                                  "used until it is mended. `cp %s %s` restores it.\n",
                          SESSION_CONFIGURATION, SESSION_DEFAULTS, SESSION_DEFAULTS,
                          SESSION_CONFIGURATION);
        }

        (void)SessionReadFile(SESSION_DEFAULTS);
    }

    if (starting)
    {
        const long scale = OxysConfigNumber(&SessionConfig, "session", 0U, "scale", 0);

        SessionScale = ((scale >= 1) && (scale <= 4))
                           ? (int32_t)scale
                           : ((SessionScreen.width >= 1024) ? 2 : 1);
    }

    SessionLoadEntries();

    if (SessionEntryCount == 0U)
    {
        SessionEntry *const entry = &SessionEntries[0];

        (void)fprintf(stderr, "session: neither %s nor %s offers anything to launch; the "
                              "launcher offers the terminal, with which they may be mended.\n",
                      SESSION_CONFIGURATION, SESSION_DEFAULTS);

        SessionCopy(entry->name, CONFIG_VALUE_MAXIMUM, SESSION_FALLBACK_NAME);
        SessionCopy(entry->run, CONFIG_VALUE_MAXIMUM, SESSION_FALLBACK_RUN);
        entry->has_picture = OxysIconRead(&entry->picture, SESSION_FALLBACK_ICON);
        SessionEntryCount = 1U;
    }

    /* After the entries: a background that falls back reads the shipped file
     * into SessionConfig, which the entries have finished with. */
    redraw = SessionLoadBackground();
    SessionUsingDefaults = defaults;

    return redraw;
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
            const int32_t offset = event->x - SessionTaskLeft();
            const size_t index = (offset >= 0) ? (size_t)(offset / SessionTaskStride())
                                               : SessionTaskCount;

            SessionCloseMenu();

            /*
             * A button restores its window — shown, raised and focused — unless
             * that window already holds the focus, in which case it is
             * minimised: the one button does both, as a person expects of a
             * list of windows. The press upon the panel did not take the
             * focus, which is what lets the list still say who holds it.
             */
            if ((index < SessionTasksShown()) &&
                ((offset % SessionTaskStride()) < (SESSION_TASK_UNITS * SessionScale)))
            {
                const SyscallWindowEntry *const task = &SessionTasks[index];
                const bool focused = (task->flags & SYSCALL_WINDOW_ENTRY_FOCUSED) != 0U;
                const bool minimised = (task->flags & SYSCALL_WINDOW_ENTRY_MINIMISED) != 0U;

                (void)OxysWindowState((int64_t)task->window,
                                      (focused && !minimised) ? SYSCALL_WINDOW_STATE_MINIMISE
                                                              : SYSCALL_WINDOW_STATE_RESTORE);
            }
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
    (void)signal(SIGALRM, SessionAlarmHandler);

    (void)SessionReadConfiguration(true);

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
    SessionRefreshTasks();

    for (;;)
    {
        SyscallWindowEvent event;
        int64_t result;

        if (SessionChildEnded != 0)
        {
            SessionReapChildren();
        }

        /* The minute turned: the clock is drawn again, which asks for the
         * next. The alarm ends the wait below with EINTR, which is what brings
         * the loop back here. */
        if (SessionClockDue != 0)
        {
            SessionClockDue = 0;
            SessionDrawClock(SessionMenu >= 0);
        }

        result = OxysWindowEvent((int64_t)SYSCALL_WINDOW_ANY, &event, SYSCALL_WINDOW_WAIT);

        if (result < 0)
        {
            /* EINTR is a child that ended, or the minute turning upon the
             * clock, each of which the top of the loop acts upon.
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
            if (event.kind == SYSCALL_WINDOW_EVENT_WINDOWS)
            {
                SessionRefreshTasks();
            }
            else if (event.kind == SYSCALL_WINDOW_EVENT_BUTTON_PRESS)
            {
                SessionHandlePress(&event);
            }

            result = OxysWindowEvent((int64_t)SYSCALL_WINDOW_ANY, &event, 0U);
        }
    }

    return EXIT_SUCCESS;
}
