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
#define SETTINGS_PITCH   12
#define SETTINGS_COLUMNS 46
#define SETTINGS_ROWS    18

#define SETTINGS_PAPER OXYS_RGB(OXYS_PAPER_RED, OXYS_PAPER_GREEN, OXYS_PAPER_BLUE)
#define SETTINGS_INK   OXYS_RGB(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE)
#define SETTINGS_DIM   OXYS_RGB(OXYS_DIM_RED, OXYS_DIM_GREEN, OXYS_DIM_BLUE)
#define SETTINGS_CHOSEN OXYS_RGB(OXYS_BAR_RED, OXYS_BAR_GREEN, OXYS_BAR_BLUE)
#define SETTINGS_BUTTON OXYS_RGB(OXYS_BAR_QUIET_RED, OXYS_BAR_QUIET_GREEN, OXYS_BAR_QUIET_BLUE)

/* The most backgrounds offered, and the most launcher entries shown. */
#define SETTINGS_BACKGROUNDS_MAXIMUM 4U
#define SETTINGS_ENTRIES_MAXIMUM     6U


/* The sizes: 0 is the session's own choice from the screen. */
static const char *const SettingsScaleNames[] = { "Automatic", "Small", "Large" };
static const char *const SettingsScaleValues[] = { "0", "1", "2" };
#define SETTINGS_SCALES 3U

/* What is chosen, and what the files held when they were read. */
typedef struct SettingsChoice
{
    char background[CONFIG_VALUE_MAXIMUM + 1U]; /* Empty for none. */
    size_t scale;
    bool pinned[SETTINGS_ENTRIES_MAXIMUM];
} SettingsChoice;

static SettingsChoice SettingsNow;
static SettingsChoice SettingsRead;

static char SettingsEntryNames[SETTINGS_ENTRIES_MAXIMUM][CONFIG_VALUE_MAXIMUM + 1U];
static size_t SettingsEntryCount;

static char SettingsBackgrounds[SETTINGS_BACKGROUNDS_MAXIMUM][CONFIG_VALUE_MAXIMUM + 1U];
static size_t SettingsBackgroundCount;

static char SettingsStatus[SETTINGS_COLUMNS + 1];

static OxysConfig SettingsConfig;
static char SettingsText[CONFIG_TEXT_MAXIMUM];

static int64_t SettingsWindow = -1;
static int32_t SettingsScale = 2;

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

    while ((OxysReadDirectory((int)descriptor, &entry) > 0) &&
           (SettingsBackgroundCount < SETTINGS_BACKGROUNDS_MAXIMUM))
    {
        const size_t length = strlen(entry.name);

        if ((length > 5U) && (strcmp(&entry.name[length - 5U], ".oxim") == 0))
        {
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

    if (SettingsReadConfig(SETTINGS_SESSION, "session.conf"))
    {
        const char *const background =
            OxysConfigValue(&SettingsConfig, "session", 0U, "background");
        const long scale = OxysConfigNumber(&SettingsConfig, "session", 0U, "scale", 0);

        SettingsCopy(choice.background, CONFIG_VALUE_MAXIMUM, background);
        choice.scale = ((scale >= 0) && (scale < (long)SETTINGS_SCALES)) ? (size_t)scale : 0U;
        SettingsEntryCount = OxysConfigCount(&SettingsConfig, "launch");

        if (SettingsEntryCount > SETTINGS_ENTRIES_MAXIMUM)
        {
            SettingsEntryCount = SETTINGS_ENTRIES_MAXIMUM;
        }

        for (size_t index = 0U; index < SettingsEntryCount; ++index)
        {
            const char *name = OxysConfigValue(&SettingsConfig, "launch", index, "name");

            if (name == NULL)
            {
                name = OxysConfigValue(&SettingsConfig, "launch", index, "run");
            }

            SettingsCopy(SettingsEntryNames[index], CONFIG_VALUE_MAXIMUM, name);
            choice.pinned[index] = OxysConfigBoolean(&SettingsConfig, "launch", index, "pin",
                                                     false);
        }
    }

    SettingsNow = choice;
    SettingsRead = choice;
}

/* ------------------------------------------------------------- writing */

/* Reads a whole file into SettingsText; its length, or -1. */
static int64_t SettingsSlurp(const char *path)
{
    const int64_t descriptor = OxysOpen(path, SYSCALL_OPEN_READ, 0U);
    int64_t total = 0;

    if (descriptor < 0)
    {
        return -1;
    }

    for (;;)
    {
        const int64_t got = OxysRead((int)descriptor, &SettingsText[total],
                                     sizeof SettingsText - (size_t)total);

        if (got <= 0)
        {
            (void)OxysClose((int)descriptor);

            return (got < 0) ? -1 : total;
        }

        total += got;

        if ((size_t)total == sizeof SettingsText)
        {
            /* Full: a file this long is more than the parser reads, and an
             * edit of its start would drop its end. */
            (void)OxysClose((int)descriptor);

            return -1;
        }
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

    if ((OxysWrite((int)descriptor, SettingsText, length) != (int64_t)length) ||
        (OxysClose((int)descriptor) < 0))
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

/* Applies one edit to SettingsText; false where it could not be made. */
static bool SettingsEdit(size_t *length, const char *section, size_t occurrence, const char *key,
                         const char *value)
{
    const size_t result = OxysConfigEdit(SettingsText, *length, sizeof SettingsText, section,
                                         occurrence, key, value);

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
        (memcmp(SettingsNow.pinned, SettingsRead.pinned, sizeof SettingsNow.pinned) != 0))
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
                                 SettingsNow.pinned[index] ? "yes" : NULL);
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
}

/* ------------------------------------------------------------- drawing */

/* A button: its row, its first column, its width in columns, what it does, and
 * the one of several it is. */
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
    int32_t column;
    int32_t width;
    SettingsAction action;
    size_t which;
} SettingsButton;

#define SETTINGS_BUTTONS_MAXIMUM 24U
static SettingsButton SettingsButtons[SETTINGS_BUTTONS_MAXIMUM];
static size_t SettingsButtonCount;

static void SettingsLabel(int32_t row, int32_t column, const char *text, uint32_t ink,
                          uint32_t paper)
{
    SyscallWindowText placement;

    placement.x = column * SETTINGS_GLYPH * SettingsScale;
    placement.y = (row * SETTINGS_PITCH * SettingsScale) +
                  (((SETTINGS_PITCH - SETTINGS_GLYPH) / 2) * SettingsScale);
    placement.ink = ink;
    placement.paper = paper;
    placement.scale = SettingsScale;
    (void)OxysWindowText(SettingsWindow, &placement, text);
}

static void SettingsFill(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t colour)
{
    static uint32_t tile[1024];
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
        (void)OxysWindowBlit(SettingsWindow, &area, tile);
    }
}

/* Draws a button and records where it stands. A chosen one is drawn in the
 * panel's colour with its label marked, so that what is chosen is plain even
 * to a person who cannot tell the two colours apart. */
static int32_t SettingsButtonAt(int32_t row, int32_t column, const char *label, bool chosen,
                                SettingsAction action, size_t which)
{
    char text[CONFIG_VALUE_MAXIMUM + 4U];
    const int32_t width = (int32_t)strlen(label) + 2;
    const uint32_t ground = chosen ? SETTINGS_CHOSEN : SETTINGS_BUTTON;
    const int32_t cell = SETTINGS_GLYPH * SettingsScale;
    const int32_t pitch = SETTINGS_PITCH * SettingsScale;

    (void)snprintf(text, sizeof text, "%c%s%c", chosen ? '>' : ' ', label, chosen ? '<' : ' ');
    SettingsFill(column * cell, (row * pitch) + SettingsScale, width * cell,
                 pitch - (2 * SettingsScale), ground);
    SettingsLabel(row, column, text, SETTINGS_INK, ground);

    if (SettingsButtonCount < SETTINGS_BUTTONS_MAXIMUM)
    {
        SettingsButton *const button = &SettingsButtons[SettingsButtonCount++];

        button->row = row;
        button->column = column;
        button->width = width;
        button->action = action;
        button->which = which;
    }

    return column + width + 1;
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

static void SettingsDraw(void)
{
    const int32_t cell = SETTINGS_GLYPH * SettingsScale;
    const int32_t pitch = SETTINGS_PITCH * SettingsScale;
    int32_t row = 1;
    int32_t column = 2;
    bool known = SettingsNow.background[0] == '\0';

    SettingsButtonCount = 0U;
    SettingsFill(0, 0, SETTINGS_COLUMNS * cell, SETTINGS_ROWS * pitch, SETTINGS_PAPER);

    SettingsLabel(row++, 1, "Background", SETTINGS_INK, SETTINGS_PAPER);

    for (size_t index = 0U; index < SettingsBackgroundCount; ++index)
    {
        char name[CONFIG_VALUE_MAXIMUM + 1U];
        const bool chosen = strcmp(SettingsNow.background, SettingsBackgrounds[index]) == 0;

        known = known || chosen;
        SettingsBackgroundName(SettingsBackgrounds[index], name, sizeof name - 1U);
        column = SettingsButtonAt(row, column, name, chosen, SETTINGS_BACKGROUND, index);
    }

    (void)SettingsButtonAt(row, column, "None", SettingsNow.background[0] == '\0',
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
    column = 2;

    for (size_t index = 0U; index < SETTINGS_SCALES; ++index)
    {
        column = SettingsButtonAt(row, column, SettingsScaleNames[index],
                                  SettingsNow.scale == index, SETTINGS_SCALE, index);
    }

    row += 2;
    SettingsLabel(row++, 1, "Pinned beside the launcher", SETTINGS_INK, SETTINGS_PAPER);

    for (size_t index = 0U; index < SettingsEntryCount; ++index)
    {
        char label[CONFIG_VALUE_MAXIMUM + 8U];

        (void)snprintf(label, sizeof label, "[%c] %s", SettingsNow.pinned[index] ? 'x' : ' ',
                       SettingsEntryNames[index]);
        (void)SettingsButtonAt(row++, 2, label, false, SETTINGS_PIN, index);
    }


    row = SETTINGS_ROWS - 3;
    column = SettingsButtonAt(row, 2, "Save", false, SETTINGS_SAVE, 0U);
    (void)SettingsButtonAt(row, column, "Revert", false, SETTINGS_REVERT, 0U);
    SettingsLabel(row + 1, 2, SettingsStatus, SETTINGS_DIM, SETTINGS_PAPER);
}

/* A press: the button beneath it, if any, acted upon. */
static void SettingsPress(int32_t x, int32_t y)
{
    const int32_t column = x / (SETTINGS_GLYPH * SettingsScale);
    const int32_t row = y / (SETTINGS_PITCH * SettingsScale);

    for (size_t index = 0U; index < SettingsButtonCount; ++index)
    {
        const SettingsButton *const button = &SettingsButtons[index];

        if ((row != button->row) || (column < button->column) ||
            (column >= (button->column + button->width)))
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
            SettingsNow.pinned[button->which] = !SettingsNow.pinned[button->which];
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

        SettingsDraw();

        return;
    }
}

int main(void)
{
    SyscallWindowRectangle screen;
    SyscallWindowRectangle geometry;
    bool running = true;

    if (OxysWindowScreen(&screen) != 0)
    {
        (void)fprintf(stderr, "settings: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    SettingsFindBackgrounds();
    SettingsLoad();

    SettingsScale = (screen.width >= 1024) ? 2 : 1;
    geometry.width = SETTINGS_COLUMNS * SETTINGS_GLYPH * SettingsScale;
    geometry.height = SETTINGS_ROWS * SETTINGS_PITCH * SettingsScale;
    geometry.x = (screen.width - geometry.width) / 2;
    geometry.y = (screen.height - geometry.height) / 3;

    if (geometry.y < 0)
    {
        geometry.y = 0;
    }

    SettingsWindow = OxysWindowCreate(&geometry, "Settings", SYSCALL_WINDOW_LAYER_NORMAL);

    if (SettingsWindow < 0)
    {
        (void)fprintf(stderr, "settings: a window could not be made.\n");

        return EXIT_FAILURE;
    }

    SettingsDraw();

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
                SettingsDraw();
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
