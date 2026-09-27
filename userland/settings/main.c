/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/settings/main.c
 * Purpose: The settings application of sub-task 9.8: a window in which the
 *          configuration of sub-task 9.4 is chosen with the pointer rather than
 *          written by hand — the background, the size the desktop is drawn at,
 *          and which programs are pinned beside the launcher — and saved to
 *          /etc, the session told to read it again and a notification saying
 *          so.
 * Key functions: main, SettingsLoad, SettingsSave, SettingsWriteFile,
 *          SettingsDraw, SettingsPress.
 * References:
 *   - libc/include/config.h: OxysConfigRead, and OxysConfigEdit, which changes
 *     one line of a file and leaves its comments as written.
 *   - kernel/abi/oxys/syscall_abi.h: the window calls, `notify` and
 *     SYSCALL_NOTIFY_RECONFIGURE.
 *   - docs/design/SETTINGS.md: the design, and what only looking establishes.
 *
 * What it edits, and what it leaves to `micro`.
 *
 *   The choices a person makes of their desktop: /etc/session.conf's
 *   background, scale and each launcher entry's `pin`. (Until 2026-09-25 it
 *   also set /etc/desktop.conf's accent, which went with the window
 *   demonstration.) It does not edit /etc/system.conf, which is the services `init`
 *   starts: a service removed by a stray press is a machine that does not
 *   start its desktop, and the file says so in its own comments; `micro` edits
 *   it, where a person can see every line they change. Nor does it add or
 *   remove launcher entries, which are a program's path and a name and are
 *   typed rather than chosen.
 *
 * Why nothing is written until Save.
 *
 *   A choice written the moment it was pressed would make every press an edit
 *   of /etc, and a person trying the backgrounds one after another would leave
 *   whichever they tried last. Choices are held until Save, and Revert reads
 *   the files again.
 */

#include <config.h>
#include <palette.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define SETTINGS_SESSION  "/etc/session.conf"
#define SETTINGS_DEFAULTS "/share/defaults/etc/"
#define SETTINGS_BACKGROUNDS "/share/backgrounds"
#define SETTINGS_SAVE_SUFFIX ".settings"

/* The grid the window is drawn in: the system face, in cells a little taller
 * than a glyph so that rows of buttons do not touch. */
#define SETTINGS_GLYPH   8
#define SETTINGS_ADVANCE ((int32_t)SYSCALL_WINDOW_TEXT_ADVANCE)
#define SETTINGS_PITCH   12
#define SETTINGS_COLUMNS 46
#define SETTINGS_ROWS_LEAST 18 /* The rows before any wrap or any pin past five. */

/* The space between a button's edge and its label, in units of the scale. */
#define SETTINGS_MARGIN  4

/* The rows of the screen a window of Settings leaves free: its title band and
 * the session's panel, neither of whose heights a program is told. Four rows
 * are more than both at either scale, 96 pixels against 60 at scale two and
 * 48 against 42 at one, so that Save is never drawn beneath the panel. */
#define SETTINGS_ROWS_KEPT 4

#define SETTINGS_PAPER OXYS_RGB(OXYS_PAPER_RED, OXYS_PAPER_GREEN, OXYS_PAPER_BLUE)
#define SETTINGS_INK   OXYS_RGB(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE)
#define SETTINGS_DIM   OXYS_RGB(OXYS_DIM_RED, OXYS_DIM_GREEN, OXYS_DIM_BLUE)
#define SETTINGS_CHOSEN OXYS_RGB(OXYS_BAR_QUIET_RED, OXYS_BAR_QUIET_GREEN, OXYS_BAR_QUIET_BLUE)
#define SETTINGS_BUTTON OXYS_RGB(OXYS_BUTTON_RED, OXYS_BUTTON_GREEN, OXYS_BUTTON_BLUE)
#define SETTINGS_EDGE \
    OXYS_RGB(OXYS_BUTTON_EDGE_RED, OXYS_BUTTON_EDGE_GREEN, OXYS_BUTTON_EDGE_BLUE)

/* The sizes: 0 is the session's own choice from the screen. */
static const char *const SettingsScaleNames[] = { "Automatic", "Small", "Large" };
static const char *const SettingsScaleValues[] = { "0", "1", "2" };
#define SETTINGS_SCALES 3U

/* What is chosen, and what the files held when they were read. The pins are
 * apart from the rest, one per launcher entry, however many the file has. */
typedef struct SettingsChoice
{
    char background[CONFIG_VALUE_MAXIMUM + 1U]; /* Empty for none. */
    size_t scale;
} SettingsChoice;

typedef char SettingsName[CONFIG_VALUE_MAXIMUM + 1U];

static SettingsChoice SettingsNow;
static SettingsChoice SettingsRead;

/* The launcher's entries and their pins, and the backgrounds on offer: as many
 * as there are, grown as they are found. A list with a bound would leave the
 * entries past it with no button, and a person could not tell why. */
static SettingsName *SettingsEntryNames;
static bool *SettingsPinnedNow;
static bool *SettingsPinnedRead;
static size_t SettingsEntryCount;
static size_t SettingsEntryCapacity;

static SettingsName *SettingsBackgrounds;
static size_t SettingsBackgroundCount;
static size_t SettingsBackgroundCapacity;

static char SettingsStatus[SETTINGS_COLUMNS + 1];

static OxysConfig SettingsConfig;

/* The text of the file being edited, in the heap: as long as the file, with
 * room made before each edit for the line it may add. */
static char *SettingsText;
static size_t SettingsTextCapacity;

static int64_t SettingsWindow = -1;
static int32_t SettingsScale = 2;

/* The screen, kept for laying out within it and for making the window again
 * when it must change size. */
static SyscallWindowRectangle SettingsScreen;

/* The rows the window has: counted by laying it out, since the background
 * buttons wrap and there is a pin for every launcher entry. */
static int32_t SettingsRows = SETTINGS_ROWS_LEAST;

/* The window's width in columns: SETTINGS_COLUMNS, or more where the pins
 * stand in columns of their own; and the right-most edge the last layout drew
 * a button to, in pixels, from which that is counted. */
static int32_t SettingsColumns = SETTINGS_COLUMNS;
static int32_t SettingsRightmost;

/* Whether a button past the right margin begins the next row: so for the rows
 * of choices, and not for the columns of pins, which widen the window. */
static bool SettingsWrapping = true;

/* ------------------------------------------------------------- memory */

/*
 * The array `array`, of `*capacity` elements of `size` bytes, with room for
 * `wanted`: itself where it has it, or grown by doubling. Null where the heap
 * refuses, the array then left as it was, and the caller shows what it has
 * room for.
 */
static void *SettingsGrow(void *array, size_t *capacity, size_t wanted, size_t size)
{
    size_t larger = (*capacity > 0U) ? *capacity : 4U;
    void *grown;

    if ((wanted <= *capacity) && (array != NULL))
    {
        return array;
    }

    while (larger < wanted)
    {
        larger *= 2U;
    }

    if (larger > (SIZE_MAX / size))
    {
        return NULL;
    }

    grown = realloc(array, larger * size);

    if (grown != NULL)
    {
        *capacity = larger;
    }

    return grown;
}

/* Room for `wanted` launcher entries: their names and both sets of pins, all
 * of one capacity, so that the three are grown together or not at all. */
static bool SettingsReserveEntries(size_t wanted)
{
    size_t names = SettingsEntryCapacity;
    size_t now = SettingsEntryCapacity;
    size_t read = SettingsEntryCapacity;
    SettingsName *const grown_names =
        SettingsGrow(SettingsEntryNames, &names, wanted, sizeof(SettingsName));
    bool *grown_now;
    bool *grown_read;

    if (grown_names == NULL)
    {
        return false;
    }

    SettingsEntryNames = grown_names;
    grown_now = SettingsGrow(SettingsPinnedNow, &now, wanted, sizeof(bool));

    if (grown_now == NULL)
    {
        return false;
    }

    SettingsPinnedNow = grown_now;
    grown_read = SettingsGrow(SettingsPinnedRead, &read, wanted, sizeof(bool));

    if (grown_read == NULL)
    {
        return false;
    }

    SettingsPinnedRead = grown_read;
    SettingsEntryCapacity = names;

    return true;
}

/* ------------------------------------------------------------- reading */

/* Copies at most `capacity` characters and a terminator. */
static void SettingsCopy(char *destination, size_t capacity, const char *source)
{
    size_t index = 0U;

    while ((source != NULL) && (source[index] != '\0') && (index < capacity))
    {
        destination[index] = source[index];
        ++index;
    }

    destination[index] = '\0';
}

/* Reads a configuration file, or the shipped copy where the person's cannot
 * be read — the same fall-back the session makes, so that what is shown is
 * what the desktop is doing. */
static bool SettingsReadConfig(const char *path, const char *name)
{
    char shipped[64];

    if (OxysConfigRead(&SettingsConfig, path))
    {
        return true;
    }

    (void)snprintf(shipped, sizeof shipped, "%s%s", SETTINGS_DEFAULTS, name);

    return OxysConfigRead(&SettingsConfig, shipped);
}

/* The backgrounds this system carries, the files of /share/backgrounds. */
static void SettingsFindBackgrounds(void)
{
    const int64_t descriptor =
        OxysOpen(SETTINGS_BACKGROUNDS, SYSCALL_OPEN_READ | SYSCALL_OPEN_DIRECTORY, 0U);
    SyscallDirectoryEntry entry;

    SettingsBackgroundCount = 0U;

    if (descriptor < 0)
    {
        return;
    }

    while (OxysReadDirectory((int)descriptor, &entry) > 0)
    {
        const size_t length = strlen(entry.name);

        if ((length > 5U) && (strcmp(&entry.name[length - 5U], ".oxim") == 0))
        {
            SettingsName *const grown =
                SettingsGrow(SettingsBackgrounds, &SettingsBackgroundCapacity,
                             SettingsBackgroundCount + 1U, sizeof(SettingsName));

            if (grown == NULL)
            {
                (void)fprintf(stderr, "settings: not every background could be offered.\n");
                break;
            }

            SettingsBackgrounds = grown;
            (void)snprintf(SettingsBackgrounds[SettingsBackgroundCount], CONFIG_VALUE_MAXIMUM + 1U,
                           "%s/%s", SETTINGS_BACKGROUNDS, entry.name);
            ++SettingsBackgroundCount;
        }
    }

    (void)OxysClose((int)descriptor);
}

/* Reads both files into the choices, and remembers them as read. */
static void SettingsLoad(void)
{
    SettingsChoice choice;

    (void)memset(&choice, 0, sizeof choice);
    SettingsEntryCount = 0U;

    if (SettingsReadConfig(SETTINGS_SESSION, "session.conf"))
    {
        const char *const background =
            OxysConfigValue(&SettingsConfig, "session", 0U, "background");
        const long scale = OxysConfigNumber(&SettingsConfig, "session", 0U, "scale", 0);
        const size_t entries = OxysConfigCount(&SettingsConfig, "launch");

        SettingsCopy(choice.background, CONFIG_VALUE_MAXIMUM, background);
        choice.scale = ((scale >= 0) && (scale < (long)SETTINGS_SCALES)) ? (size_t)scale : 0U;

        /* An entry with no room for its pin is not shown rather than shown
         * wrongly, and the person is told, since Save leaves its line alone. */
        SettingsEntryCount = SettingsReserveEntries(entries)
                                 ? entries
                                 : ((SettingsEntryCapacity < entries) ? SettingsEntryCapacity
                                                                      : entries);

        if (SettingsEntryCount < entries)
        {
            (void)OxysNotify(SYSCALL_NOTIFY_WARNING, 0U,
                             "Settings could not show every launcher entry.");
        }

        for (size_t index = 0U; index < SettingsEntryCount; ++index)
        {
            const char *name = OxysConfigValue(&SettingsConfig, "launch", index, "name");

            if (name == NULL)
            {
                name = OxysConfigValue(&SettingsConfig, "launch", index, "run");
            }

            SettingsCopy(SettingsEntryNames[index], CONFIG_VALUE_MAXIMUM, name);
            SettingsPinnedNow[index] =
                OxysConfigBoolean(&SettingsConfig, "launch", index, "pin", false);
            SettingsPinnedRead[index] = SettingsPinnedNow[index];
        }
    }

    SettingsNow = choice;
    SettingsRead = choice;
}

/* ------------------------------------------------------------- writing */

/* Makes room in SettingsText for `wanted` bytes; false where the heap refuses. */
static bool SettingsTextRoom(size_t wanted)
{
    char *const grown = SettingsGrow(SettingsText, &SettingsTextCapacity, wanted, 1U);

    if (grown == NULL)
    {
        return false;
    }

    SettingsText = grown;

    return true;
}

/* Reads a whole file into SettingsText, however long; its length, or -1. */
static int64_t SettingsSlurp(const char *path)
{
    const int64_t descriptor = OxysOpen(path, SYSCALL_OPEN_READ, 0U);
    size_t total = 0U;

    if (descriptor < 0)
    {
        return -1;
    }

    for (;;)
    {
        int64_t got;

        if (!SettingsTextRoom(total + CONFIG_TEXT_INLINE))
        {
            /* A file only partly held would lose its end at the write. */
            (void)OxysClose((int)descriptor);

            return -1;
        }

        got = OxysRead((int)descriptor, &SettingsText[total], SettingsTextCapacity - total);

        if (got <= 0)
        {
            (void)OxysClose((int)descriptor);

            return (got < 0) ? -1 : (int64_t)total;
        }

        total += (size_t)got;
    }
}

/*
 * Writes SettingsText to `path` as `micro` saves: into a file beside it, and
 * only once that is whole does it take the name — so that a failure part way
 * leaves the file as it was. There is no rename; the name is moved by an
 * unlink and a link, as `mv` moves one.
 */
static bool SettingsWriteFile(const char *path, size_t length)
{
    char beside[SYSCALL_PATH_MAXIMUM + 1U];
    int64_t descriptor;

    (void)snprintf(beside, sizeof beside, "%s%s", path, SETTINGS_SAVE_SUFFIX);
    descriptor = OxysOpen(beside, SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE | SYSCALL_OPEN_TRUNCATE,
                          0644U);

    if (descriptor < 0)
    {
        return false;
    }

    /* A write may take less than it was given — the kernel moves at most
     * SYSCALL_TRANSFER_MAXIMUM at once — and a file longer than that is the
     * rest of it written after. */
    for (size_t at = 0U; at < length;)
    {
        const int64_t written = OxysWrite((int)descriptor, &SettingsText[at], length - at);

        if (written <= 0)
        {
            (void)OxysClose((int)descriptor);
            (void)OxysUnlink(beside);

            return false;
        }

        at += (size_t)written;
    }

    if (OxysClose((int)descriptor) < 0)
    {
        (void)OxysUnlink(beside);

        return false;
    }

    if (((OxysUnlink(path) < 0) && (errno != ENOENT)) || (OxysLink(beside, path) < 0))
    {
        (void)OxysUnlink(beside);

        return false;
    }

    (void)OxysUnlink(beside);

    return true;
}

/* Applies one edit to SettingsText; false where it could not be made. Room is
 * made first for the most one edit adds — a block's heading and one setting —
 * so that an edit fails for what it says and never for want of space. */
static bool SettingsEdit(size_t *length, const char *section, size_t occurrence, const char *key,
                         const char *value)
{
    size_t result;

    if (!SettingsTextRoom(*length + (2U * (CONFIG_SECTION_MAXIMUM + CONFIG_KEY_MAXIMUM +
                                            CONFIG_VALUE_MAXIMUM + 8U))))
    {
        return false;
    }

    result = OxysConfigEdit(SettingsText, *length, SettingsTextCapacity, section, occurrence, key,
                            value);

    if (result == CONFIG_EDIT_FAILED)
    {
        return false;
    }

    *length = result;

    return true;
}

/* Reads the person's file, or the shipped one where theirs is missing, as the
 * text an edit starts from. */
static int64_t SettingsStart(const char *path, const char *name)
{
    char shipped[64];
    const int64_t length = SettingsSlurp(path);

    if (length >= 0)
    {
        return length;
    }

    (void)snprintf(shipped, sizeof shipped, "%s%s", SETTINGS_DEFAULTS, name);

    return SettingsSlurp(shipped);
}

/* Writes what changed, one file at a time, and says how it went. */
static void SettingsSave(void)
{
    bool saved = true;
    bool changed = false;

    if ((strcmp(SettingsNow.background, SettingsRead.background) != 0) ||
        (SettingsNow.scale != SettingsRead.scale) ||
        ((SettingsEntryCount > 0U) &&
         (memcmp(SettingsPinnedNow, SettingsPinnedRead, SettingsEntryCount * sizeof(bool)) != 0)))
    {
        int64_t read = SettingsStart(SETTINGS_SESSION, "session.conf");
        size_t length = (read >= 0) ? (size_t)read : 0U;

        changed = true;
        saved = (read >= 0) &&
                SettingsEdit(&length, "session", 0U, "background",
                             (SettingsNow.background[0] != '\0') ? SettingsNow.background : NULL) &&
                SettingsEdit(&length, "session", 0U, "scale",
                             SettingsScaleValues[SettingsNow.scale]);

        for (size_t index = 0U; saved && (index < SettingsEntryCount); ++index)
        {
            saved = SettingsEdit(&length, "launch", index, "pin",
                                 SettingsPinnedNow[index] ? "yes" : NULL);
        }

        saved = saved && SettingsWriteFile(SETTINGS_SESSION, length);
    }


    if (!changed)
    {
        SettingsCopy(SettingsStatus, SETTINGS_COLUMNS, "Nothing has changed.");

        return;
    }

    if (!saved)
    {
        SettingsCopy(SettingsStatus, SETTINGS_COLUMNS, "The settings could not be saved.");
        (void)fprintf(stderr, "settings: %s could not be written: %s\n", SETTINGS_SESSION,
                      strerror(errno));
        (void)OxysNotify(SYSCALL_NOTIFY_ERROR, 0U, "The settings could not be saved.");

        return;
    }

    SettingsCopy(SettingsStatus, SETTINGS_COLUMNS, "Saved.");
    (void)OxysNotify(SYSCALL_NOTIFY_SUCCESS, SYSCALL_NOTIFY_RECONFIGURE, "Settings saved.");

    /* The size is the one choice the session reads only as it starts: its
     * panel and clock are made that size, and remaking them is a start. */
    if (SettingsNow.scale != SettingsRead.scale)
    {
        (void)OxysNotify(SYSCALL_NOTIFY_INFORMATION, 0U,
                         "The new size is used when the desktop next starts.");
    }

    SettingsRead = SettingsNow;

    if (SettingsEntryCount > 0U)
    {
        (void)memcpy(SettingsPinnedRead, SettingsPinnedNow, SettingsEntryCount * sizeof(bool));
    }
}

/* ------------------------------------------------------------- drawing */

/* A button: its row, its left edge and width in pixels, what it does, and the
 * one of several it is. Pixels and not columns, because its label is
 * proportional and a column is not a width any label has. */
typedef enum SettingsAction
{
    SETTINGS_BACKGROUND,
    SETTINGS_NO_BACKGROUND,
    SETTINGS_SCALE,
    SETTINGS_PIN,
    SETTINGS_SAVE,
    SETTINGS_REVERT
} SettingsAction;

typedef struct SettingsButton
{
    int32_t row;
    int32_t x;
    int32_t width;
    SettingsAction action;
    size_t which;
} SettingsButton;

/* The buttons as last laid out, as many as there are; and whether SettingsDraw
 * paints, or only lays the window out to count the rows it needs. */
static SettingsButton *SettingsButtons;
static size_t SettingsButtonCapacity;
static bool SettingsPainting = true;
static size_t SettingsButtonCount;

/* Draws, or with SYSCALL_WINDOW_TEXT_MEASURE only measures, a proportional run
 * at `x` pixels on `row`; its width in pixels, or 0. */
static int32_t SettingsTextRun(int32_t row, int32_t x, const char *text, uint32_t ink,
                               uint32_t paper, uint32_t flags)
{
    SyscallWindowText placement;
    int64_t width;

    if (!SettingsPainting && ((flags & SYSCALL_WINDOW_TEXT_MEASURE) == 0U))
    {
        return 0;
    }

    placement.x = x;
    placement.y = (row * SETTINGS_PITCH * SettingsScale) +
                  (((SETTINGS_PITCH - SETTINGS_GLYPH) / 2) * SettingsScale);
    placement.ink = ink;
    placement.paper = paper;
    placement.scale = SettingsScale;
    placement.flags = SYSCALL_WINDOW_TEXT_PROPORTIONAL | flags;
    width = OxysWindowText(SettingsWindow, &placement, text);

    return (width > 0) ? (int32_t)width : 0;
}

static void SettingsLabel(int32_t row, int32_t column, const char *text, uint32_t ink,
                          uint32_t paper)
{
    (void)SettingsTextRun(row, column * SETTINGS_ADVANCE * SettingsScale, text, ink, paper, 0U);
}

static void SettingsFill(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colour)
{
    static uint32_t tile[1024];
    SyscallWindowRectangle area;
    const int32_t rows = (int32_t)(sizeof tile / sizeof tile[0]) / ((width > 0) ? width : 1);

    if (!SettingsPainting || (width <= 0) || (height <= 0) || (rows <= 0))
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
        (void)OxysWindowBlit(SettingsWindow, &area, tile);
    }
}

/* Draws a button, in the project owner's style since 2026-09-26, and records
 * where it stands. A chosen one is drawn in the quiet colour with its label
 * marked, so that what is chosen is plain even to a person who cannot tell
 * the two colours apart.
 *
 * It is as wide as its label measures, with a margin either side, and returns
 * where the next button along begins, in pixels. The width is the marked
 * label's whether the button is chosen or not: a button that widened when
 * pressed would push the buttons beside it along, and the next press would
 * land on a different one than the pointer was over when the person aimed. */
static int32_t SettingsButtonAt(int32_t *at_row, int32_t x, const char *label, bool chosen,
                                SettingsAction action, size_t which)
{
    char marked[CONFIG_VALUE_MAXIMUM + 4U];
    const uint32_t ground = chosen ? SETTINGS_CHOSEN : SETTINGS_BUTTON;
    const int32_t cell = SETTINGS_ADVANCE * SettingsScale;
    const int32_t pitch = SETTINGS_PITCH * SettingsScale;
    const int32_t margin = SETTINGS_MARGIN * SettingsScale;
    int32_t row = *at_row;
    int32_t text;
    int32_t width;

    (void)snprintf(marked, sizeof marked, ">%s<", label);
    text = SettingsTextRun(row, 0, marked, 0U, 0U, SYSCALL_WINDOW_TEXT_MEASURE);

    if (text == 0)
    {
        /* Not measured: the grid's width, which is wider than the run. */
        text = ((int32_t)strlen(marked)) * cell;
    }

    width = text + (2 * margin);

    /* A button that would pass the window's right-hand margin begins the next
     * row, unless it is the first of its row and could stand nowhere wider. */
    if (SettingsWrapping && (x > (2 * cell)) && ((x + width) > ((SETTINGS_COLUMNS - 2) * cell)))
    {
        row = ++*at_row;
        x = 2 * cell;
    }

    SettingsRightmost = ((x + width) > SettingsRightmost) ? (x + width) : SettingsRightmost;
    SettingsFill(x, (row * pitch) + SettingsScale, width, pitch - (2 * SettingsScale), ground);

    if (chosen)
    {
        (void)SettingsTextRun(row, x + margin, marked, SETTINGS_INK, ground, 0U);
    }
    else
    {
        const int32_t plain = SettingsTextRun(row, 0, label, 0U, 0U, SYSCALL_WINDOW_TEXT_MEASURE);

        (void)SettingsTextRun(row, x + ((plain > 0) ? ((width - plain) / 2) : margin), label,
                              SETTINGS_INK, ground, 0U);
    }

    /* The edge of the owner's button, a unit wide, drawn last: a glyph's ink
     * may overhang its run and would otherwise cross it. */
    {
        const int32_t left = x;
        const int32_t top = (row * pitch) + SettingsScale;
        const int32_t across = width;
        const int32_t down = pitch - (2 * SettingsScale);

        SettingsFill(left, top, across, SettingsScale, SETTINGS_EDGE);
        SettingsFill(left, top + down - SettingsScale, across, SettingsScale, SETTINGS_EDGE);
        SettingsFill(left, top, SettingsScale, down, SETTINGS_EDGE);
        SettingsFill(left + across - SettingsScale, top, SettingsScale, down, SETTINGS_EDGE);
    }

    {
        SettingsButton *const grown = SettingsGrow(SettingsButtons, &SettingsButtonCapacity,
                                                   SettingsButtonCount + 1U, sizeof *grown);
        SettingsButton *button;

        if (grown == NULL)
        {
            /* Drawn but not pressable; said, since it looks like any other. */
            (void)fprintf(stderr, "settings: no memory for the button \"%s\".\n", label);

            return x + width + cell;
        }

        SettingsButtons = grown;
        button = &SettingsButtons[SettingsButtonCount++];
        button->row = row;
        button->x = x;
        button->width = width;
        button->action = action;
        button->which = which;
    }

    return x + width + cell;
}

/* The name a background is offered under: its file's name without the
 * directory or the extension, with a capital. */
static void SettingsBackgroundName(const char *path, char *name, size_t capacity)
{
    const char *base = strrchr(path, '/');
    size_t length;

    SettingsCopy(name, capacity, (base != NULL) ? base + 1 : path);
    length = strlen(name);

    if ((length > 5U) && (strcmp(&name[length - 5U], ".oxim") == 0))
    {
        name[length - 5U] = '\0';
    }

    if ((name[0] >= 'a') && (name[0] <= 'z'))
    {
        name[0] = (char)(name[0] - 'a' + 'A');
    }
}

/* Lays the window out and, while SettingsPainting, draws it; the rows it
 * needed. The rows below the pins follow the last of them, so the window is as
 * tall as the launcher entries make it and never less than it always was. */
static int32_t SettingsDraw(void)
{
    const int32_t cell = SETTINGS_ADVANCE * SettingsScale;
    const int32_t pitch = SETTINGS_PITCH * SettingsScale;
    int32_t row = 1;
    int32_t x = 2 * cell;
    bool known = SettingsNow.background[0] == '\0';

    SettingsButtonCount = 0U;
    SettingsRightmost = 0;
    SettingsFill(0, 0, SettingsColumns * cell, SettingsRows * pitch, SETTINGS_PAPER);

    SettingsLabel(row++, 1, "Background", SETTINGS_INK, SETTINGS_PAPER);

    for (size_t index = 0U; index < SettingsBackgroundCount; ++index)
    {
        char name[CONFIG_VALUE_MAXIMUM + 1U];
        const bool chosen = strcmp(SettingsNow.background, SettingsBackgrounds[index]) == 0;

        known = known || chosen;
        SettingsBackgroundName(SettingsBackgrounds[index], name, sizeof name - 1U);
        x = SettingsButtonAt(&row, x, name, chosen, SETTINGS_BACKGROUND, index);
    }

    (void)SettingsButtonAt(&row, x, "None", SettingsNow.background[0] == '\0',
                           SETTINGS_NO_BACKGROUND, 0U);

    /* A path of the person's own, which none of the buttons names, is kept
     * until another is chosen, and shown so they know it is there. */
    if (!known)
    {
        char other[SETTINGS_COLUMNS + 1];

        (void)snprintf(other, sizeof other, "Now: %s", SettingsNow.background);
        SettingsLabel(row + 1, 2, other, SETTINGS_DIM, SETTINGS_PAPER);
    }

    row += 3;
    SettingsLabel(row++, 1, "Size of the desktop", SETTINGS_INK, SETTINGS_PAPER);
    x = 2 * cell;

    for (size_t index = 0U; index < SETTINGS_SCALES; ++index)
    {
        x = SettingsButtonAt(&row, x, SettingsScaleNames[index], SettingsNow.scale == index,
                             SETTINGS_SCALE, index);
    }

    row += 2;
    SettingsLabel(row++, 1, "Pinned beside the launcher", SETTINGS_INK, SETTINGS_PAPER);

    /*
     * The pins stand in a column, and in further columns to its right when the
     * screen has not the rows for them all: a window taller than the screen
     * would have Save and Revert cut from its foot, and the settings could be
     * chosen but never saved. Each column begins a cell past the widest button
     * of the one before, and the window is widened to hold them.
     */
    {
        const int32_t first = row;
        const int32_t before = SettingsRightmost;
        const int32_t fits = (SettingsScreen.height / pitch) - SETTINGS_ROWS_KEPT;
        const int32_t below = 4; /* A blank row, Save, the status and a margin. */
        const size_t down = ((fits - first - below) > 1) ? (size_t)(fits - first - below) : 1U;
        int32_t left = 2 * cell;
        int32_t widest = left;

        SettingsWrapping = false;

        for (size_t index = 0U; index < SettingsEntryCount; ++index)
        {
            char label[CONFIG_VALUE_MAXIMUM + 8U];
            int32_t at = first + (int32_t)(index % down);

            if ((index > 0U) && ((index % down) == 0U))
            {
                left = widest + cell;
            }

            (void)snprintf(label, sizeof label, "[%c] %s", SettingsPinnedNow[index] ? 'x' : ' ',
                           SettingsEntryNames[index]);
            SettingsRightmost = left;
            (void)SettingsButtonAt(&at, left, label, false, SETTINGS_PIN, index);
            widest = (SettingsRightmost > widest) ? SettingsRightmost : widest;
        }

        row = first + (int32_t)((SettingsEntryCount < down) ? SettingsEntryCount : down);
        SettingsRightmost = (widest > before) ? widest : before;
        SettingsWrapping = true;
    }

    row = ((row + 1) > (SETTINGS_ROWS_LEAST - 3)) ? (row + 1) : (SETTINGS_ROWS_LEAST - 3);
    x = SettingsButtonAt(&row, 2 * cell, "Save", false, SETTINGS_SAVE, 0U);
    (void)SettingsButtonAt(&row, x, "Revert", false, SETTINGS_REVERT, 0U);
    SettingsLabel(row + 1, 2, SettingsStatus, SETTINGS_DIM, SETTINGS_PAPER);

    return row + 3;
}

static bool SettingsOpen(void);

/* A press: the button beneath it, if any, acted upon. */
static void SettingsPress(int32_t x, int32_t y)
{
    const int32_t row = y / (SETTINGS_PITCH * SettingsScale);

    for (size_t index = 0U; index < SettingsButtonCount; ++index)
    {
        const SettingsButton *const button = &SettingsButtons[index];

        if ((row != button->row) || (x < button->x) || (x >= (button->x + button->width)))
        {
            continue;
        }

        SettingsStatus[0] = '\0';

        switch (button->action)
        {
        case SETTINGS_BACKGROUND:
            SettingsCopy(SettingsNow.background, CONFIG_VALUE_MAXIMUM,
                         SettingsBackgrounds[button->which]);
            break;
        case SETTINGS_NO_BACKGROUND:
            SettingsNow.background[0] = '\0';
            break;
        case SETTINGS_SCALE:
            SettingsNow.scale = button->which;
            break;
        case SETTINGS_PIN:
            SettingsPinnedNow[button->which] = !SettingsPinnedNow[button->which];
            break;
        case SETTINGS_SAVE:
            SettingsSave();
            break;
        case SETTINGS_REVERT:
        default:
            SettingsLoad();
            SettingsCopy(SettingsStatus, SETTINGS_COLUMNS, "The files were read again.");
            break;
        }

        /* Laid out again, and the window made again should Revert have found
         * a different number of launcher entries. */
        if (!SettingsOpen())
        {
            exit(EXIT_FAILURE);
        }

        return;
    }
}


/* Makes a window of SettingsRows rows, or none; false then. */
static bool SettingsCreate(void)
{
    SyscallWindowRectangle geometry;

    geometry.width = SettingsColumns * SETTINGS_ADVANCE * SettingsScale;
    geometry.height = SettingsRows * SETTINGS_PITCH * SettingsScale;
    geometry.x = (SettingsScreen.width - geometry.width) / 2;
    geometry.y = (SettingsScreen.height - geometry.height) / 3;

    if (geometry.y < 0)
    {
        geometry.y = 0;
    }

    SettingsWindow = OxysWindowCreate(&geometry, "Settings", SYSCALL_WINDOW_LAYER_NORMAL);

    return SettingsWindow >= 0;
}

/*
 * Makes the window as large as its layout needs. A window cannot be resized by
 * its program, and the labels are measured through a window, so the layout is
 * counted within the window there is and the window made again where the count
 * differs — before anything is drawn in it, so nothing is seen twice. The pins
 * go into columns before the window outgrows the screen's height, so only a
 * screen too narrow for those columns has anything cut, and that is said.
 */
static bool SettingsOpen(void)
{
    const int32_t cell = SETTINGS_ADVANCE * SettingsScale;
    const int32_t fits =
        (SettingsScreen.height / (SETTINGS_PITCH * SettingsScale)) - SETTINGS_ROWS_KEPT;
    const int32_t across = SettingsScreen.width / cell;
    int32_t rows;
    int32_t columns;

    if ((SettingsWindow < 0) && !SettingsCreate())
    {
        return false;
    }

    SettingsPainting = false;
    rows = SettingsDraw();
    SettingsPainting = true;
    columns = (SettingsRightmost + (2 * cell) + cell - 1) / cell;
    columns = (columns > SETTINGS_COLUMNS) ? columns : SETTINGS_COLUMNS;

    if (((rows > fits) && (fits > 0)) || ((columns > across) && (across > 0)))
    {
        (void)fprintf(stderr, "settings: the screen is too small for every launcher entry.\n");
        rows = ((rows > fits) && (fits > 0)) ? fits : rows;
        columns = ((columns > across) && (across > 0)) ? across : columns;
    }

    if ((rows != SettingsRows) || (columns != SettingsColumns))
    {
        (void)OxysWindowDestroy(SettingsWindow);
        SettingsRows = rows;
        SettingsColumns = columns;

        if (!SettingsCreate())
        {
            return false;
        }
    }

    (void)SettingsDraw();

    return true;
}

int main(void)
{
    bool running = true;

    if (OxysWindowScreen(&SettingsScreen) != 0)
    {
        (void)fprintf(stderr, "settings: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    SettingsFindBackgrounds();
    SettingsLoad();

    SettingsScale = (SettingsScreen.width >= 1024) ? 2 : 1;

    if (!SettingsOpen())
    {
        (void)fprintf(stderr, "settings: a window could not be made.\n");

        return EXIT_FAILURE;
    }

    while (running)
    {
        SyscallWindowEvent event;
        int64_t result = OxysWindowEvent(SettingsWindow, &event, SYSCALL_WINDOW_WAIT);

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
                SettingsPress(event.x, event.y);
            }
            else if (event.kind == SYSCALL_WINDOW_EVENT_RESIZE)
            {
                (void)SettingsDraw();
            }
            else if (event.kind == SYSCALL_WINDOW_EVENT_CLOSE)
            {
                running = false;
            }

            result = OxysWindowEvent(SettingsWindow, &event, 0U);
        }
    }

    (void)OxysWindowDestroy(SettingsWindow);

    return EXIT_SUCCESS;
}
