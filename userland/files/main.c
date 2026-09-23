/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/files/main.c
 * Purpose: The file manager of sub-task 9.7: a window listing one directory,
 *          directories first, in which a person moves into a directory, back
 *          out of it, and opens a file in the text viewer.
 * Key functions: main, FilesLoad, FilesOpen, FilesDraw, FilesPaint,
 *          FilesHandleKey, FilesHandlePress, FilesReap.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: `open` with SYSCALL_OPEN_DIRECTORY,
 *     `readdir` and SyscallEntryType; the window calls and the resize event.
 *   - userland/view/main.c: the viewer a file is opened in.
 *   - docs/design/UTILITIES.md, Section 2: the design, and what only looking
 *     establishes.
 *
 * How a person uses it.
 *
 *   A press upon a row selects it; a press upon the row already selected opens
 *   it, which is a double press without a clock to time one by — the press
 *   that selects and the press that opens are never confused, because the
 *   first always only selects. The keys do the same: the arrows and the page
 *   keys move the selection, Enter opens, Backspace goes up. A directory opens
 *   in this window; a file opens in a window of its own, `/bin/view` started
 *   as a child; anything else — a device, a pipe — is said to be unopenable
 *   upon the status row.
 *
 * What it does not do, and why.
 *
 *   It moves, copies and deletes nothing. `mv`, `cp` and `rm` do that at the
 *   shell, asserted by the utilities' self-tests; a file manager that also did
 *   it would be a second implementation of each with none of their tests, upon
 *   a ramdisk whose contents are lost at every reboot in any case.
 */

#include <palette.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

#define FILES_GLYPH 8
#define FILES_PITCH 10
#define FILES_CHUNK 120
#define FILES_COLUMNS_MAXIMUM 512

/* How many entries one directory may show. A directory with more is listed to
 * that many, and the status says so. */
#define FILES_ENTRIES_MAXIMUM 256U

#define FILES_VIEWER "/bin/view"

#define FILES_PAPER OXYS_RGB(OXYS_PAPER_RED, OXYS_PAPER_GREEN, OXYS_PAPER_BLUE)
#define FILES_INK   OXYS_RGB(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE)
#define FILES_DIM   OXYS_RGB(OXYS_DIM_RED, OXYS_DIM_GREEN, OXYS_DIM_BLUE)
#define FILES_MARK  OXYS_RGB(OXYS_BAR_RED, OXYS_BAR_GREEN, OXYS_BAR_BLUE)

typedef struct FilesEntry
{
    char name[SYSCALL_NAME_MAXIMUM + 1U];
    uint32_t type;
} FilesEntry;

static FilesEntry FilesEntries[FILES_ENTRIES_MAXIMUM];
static size_t FilesCount;
static bool FilesCut;

static char FilesPath[SYSCALL_PATH_MAXIMUM + 1U] = "/";
static char FilesStatus[128];

static size_t FilesSelected;
static size_t FilesTop;

static int64_t FilesWindow = -1;
static int32_t FilesScale = 2;
static int32_t FilesWidth;
static int32_t FilesHeight;
static int32_t FilesColumns;

/* The rows of entries: the window's rows less the path at the top and the
 * status at the foot. */
static int32_t FilesRows;

static volatile sig_atomic_t FilesChildEnded;

static void FilesChildHandler(int signal)
{
    (void)signal;
    FilesChildEnded = 1;
}

/* ---------------------------------------------------------- the listing */

/* Directories before everything else, then by name as bytes: the order `ls`
 * gives within each, so a person who knows one knows the other. */
static bool FilesBefore(const FilesEntry *first, const FilesEntry *second)
{
    const bool first_directory = (first->type == SYSCALL_TYPE_DIRECTORY);
    const bool second_directory = (second->type == SYSCALL_TYPE_DIRECTORY);

    if (strcmp(first->name, "..") == 0)
    {
        return true;
    }

    if (strcmp(second->name, "..") == 0)
    {
        return false;
    }

    if (first_directory != second_directory)
    {
        return first_directory;
    }

    return strcmp(first->name, second->name) < 0;
}

/*
 * Reads the directory at FilesPath. `.` is left out — it is this directory,
 * and opening it would do nothing — and `..` is kept everywhere but at the
 * root, where it would be the root again. False, with the status saying why,
 * where the directory cannot be opened; the listing is then empty.
 */
static bool FilesLoad(void)
{
    const int64_t descriptor = OxysOpen(FilesPath, SYSCALL_OPEN_READ | SYSCALL_OPEN_DIRECTORY, 0U);
    SyscallDirectoryEntry entry;

    FilesCount = 0U;
    FilesCut = false;
    FilesSelected = 0U;
    FilesTop = 0U;

    if (descriptor < 0)
    {
        (void)snprintf(FilesStatus, sizeof FilesStatus, "%s: %s", FilesPath, strerror(errno));

        return false;
    }

    while (OxysReadDirectory((int)descriptor, &entry) > 0)
    {
        if ((strcmp(entry.name, ".") == 0) ||
            ((strcmp(entry.name, "..") == 0) && (strcmp(FilesPath, "/") == 0)))
        {
            continue;
        }

        if (FilesCount == FILES_ENTRIES_MAXIMUM)
        {
            FilesCut = true;
            break;
        }

        memcpy(FilesEntries[FilesCount].name, entry.name, sizeof FilesEntries[FilesCount].name);
        FilesEntries[FilesCount].type = entry.type;
        ++FilesCount;
    }

    (void)OxysClose((int)descriptor);

    /* An insertion sort: a directory is a few dozen entries, and the sort is
     * run once per directory entered. */
    for (size_t index = 1U; index < FilesCount; ++index)
    {
        const FilesEntry moving = FilesEntries[index];
        size_t place = index;

        while ((place > 0U) && FilesBefore(&moving, &FilesEntries[place - 1U]))
        {
            FilesEntries[place] = FilesEntries[place - 1U];
            --place;
        }

        FilesEntries[place] = moving;
    }

    (void)snprintf(FilesStatus, sizeof FilesStatus, "%lu entr%s%s",
                   (unsigned long)FilesCount, (FilesCount == 1U) ? "y" : "ies",
                   FilesCut ? ", and more not shown" : "");

    return true;
}

/*
 * The path of an entry of the present directory into `path`. False where it
 * would not fit, which is refused rather than cut: a path silently shortened
 * names another file.
 */
static bool FilesJoin(char *path, size_t capacity, const char *name)
{
    const int written = snprintf(path, capacity, "%s%s%s", FilesPath,
                                 (strcmp(FilesPath, "/") == 0) ? "" : "/", name);

    return (written > 0) && ((size_t)written < capacity);
}

/* Up one directory, by removing the last part of the path. */
static void FilesUp(void)
{
    char *last = strrchr(FilesPath, '/');

    if (strcmp(FilesPath, "/") == 0)
    {
        return;
    }

    if (last == FilesPath)
    {
        FilesPath[1] = '\0';
    }
    else if (last != NULL)
    {
        *last = '\0';
    }

    (void)FilesLoad();
}

/* Starts the viewer upon a file. It is not waited for; FilesReap collects it
 * when it ends. */
static void FilesView(const char *path)
{
    const int64_t child = OxysFork();

    if (child == 0)
    {
        static char name[] = "view";
        char *const argument_vector[] = { name, (char *)path, NULL };

        (void)OxysExecve(FILES_VIEWER, argument_vector, NULL);
        OxysExit(127);
    }

    (void)snprintf(FilesStatus, sizeof FilesStatus, (child > 0) ? "opened %s" : "%s could not be opened",
                   path);
}

/* Opens the selected entry: into a directory, up for `..`, or the viewer for a
 * file. */
static void FilesOpen(void)
{
    const FilesEntry *entry;
    char path[SYSCALL_PATH_MAXIMUM + 1U];

    if (FilesSelected >= FilesCount)
    {
        return;
    }

    entry = &FilesEntries[FilesSelected];

    if (strcmp(entry->name, "..") == 0)
    {
        FilesUp();

        return;
    }

    if (!FilesJoin(path, sizeof path, entry->name))
    {
        (void)snprintf(FilesStatus, sizeof FilesStatus, "the path is too long to open");

        return;
    }

    if (entry->type == SYSCALL_TYPE_DIRECTORY)
    {
        memcpy(FilesPath, path, sizeof FilesPath);
        (void)FilesLoad();
    }
    else if (entry->type == SYSCALL_TYPE_REGULAR)
    {
        FilesView(path);
    }
    else
    {
        (void)snprintf(FilesStatus, sizeof FilesStatus, "%s is not a file the viewer can show",
                       entry->name);
    }
}

/* Collects every viewer that has ended. */
static void FilesReap(void)
{
    int64_t status = 0;

    while (OxysWaitFor((int64_t)-1, &status, SYSCALL_WAIT_NO_HANG) > 0)
    {
    }

    FilesChildEnded = 0;
}

/* ------------------------------------------------------------ drawing */

static void FilesDrawText(int32_t row, const char *text, int32_t count, uint32_t ink,
                          uint32_t paper)
{
    char piece[FILES_CHUNK + 1];

    for (int32_t first = 0; first < count; first += FILES_CHUNK)
    {
        const int32_t length = ((count - first) < FILES_CHUNK) ? (count - first) : FILES_CHUNK;
        SyscallWindowText placement;

        memcpy(piece, &text[first], (size_t)length);
        piece[length] = '\0';

        placement.x = first * FILES_GLYPH * FilesScale;
        placement.y = row * FILES_PITCH * FilesScale;
        placement.ink = ink;
        placement.paper = paper;
        placement.scale = FilesScale;

        (void)OxysWindowText(FilesWindow, &placement, piece);
    }
}

/* A row of the window: the text given, padded with spaces to the width, so that
 * it covers whatever the row showed before. */
static void FilesDrawLine(int32_t row, const char *text, uint32_t ink, uint32_t paper)
{
    char line[FILES_COLUMNS_MAXIMUM + 1];
    int32_t column = 0;

    while ((text[column] != '\0') && (column < FilesColumns))
    {
        line[column] = text[column];
        ++column;
    }

    while (column < FilesColumns)
    {
        line[column++] = ' ';
    }

    FilesDrawText(row, line, FilesColumns, ink, paper);
}

static void FilesDraw(void)
{
    char text[SYSCALL_NAME_MAXIMUM + 8U];

    FilesDrawLine(0, FilesPath, FILES_PAPER, FILES_INK);

    for (int32_t row = 0; row < FilesRows; ++row)
    {
        const size_t index = FilesTop + (size_t)row;

        if (index >= FilesCount)
        {
            FilesDrawLine(row + 1, "", FILES_INK, FILES_PAPER);
            continue;
        }

        /* A directory is marked by the slash `ls -F` would give it, and nothing
         * else is marked: a picture per type would be a picture the list does
         * not have yet. */
        (void)snprintf(text, sizeof text, " %s%s", FilesEntries[index].name,
                       (FilesEntries[index].type == SYSCALL_TYPE_DIRECTORY) ? "/" : "");
        FilesDrawLine(row + 1, text, FILES_INK,
                      (index == FilesSelected) ? FILES_MARK : FILES_PAPER);
    }

    FilesDrawLine(FilesRows + 1, FilesStatus, FILES_DIM, FILES_PAPER);
}

/* Paints the whole content, the margin past the last whole cell included. */
static void FilesPaint(void)
{
    const int32_t band = FILES_PITCH * FilesScale;
    const size_t count = (size_t)FilesWidth * (size_t)band;
    uint32_t *const pixels = malloc(count * sizeof *pixels);
    SyscallWindowRectangle area;

    if (pixels == NULL)
    {
        return;
    }

    for (size_t index = 0U; index < count; ++index)
    {
        pixels[index] = FILES_PAPER;
    }

    area.x = 0;
    area.width = FilesWidth;

    for (int32_t top = 0; top < FilesHeight; top += band)
    {
        area.y = top;
        area.height = ((top + band) <= FilesHeight) ? band : (FilesHeight - top);
        (void)OxysWindowBlit(FilesWindow, &area, pixels);
    }

    free(pixels);
}

static void FilesLayout(int32_t width, int32_t height)
{
    FilesWidth = width;
    FilesHeight = height;
    FilesColumns = width / (FILES_GLYPH * FilesScale);
    FilesRows = (height / (FILES_PITCH * FilesScale)) - 2;

    FilesColumns = (FilesColumns > FILES_COLUMNS_MAXIMUM) ? FILES_COLUMNS_MAXIMUM : FilesColumns;
    FilesColumns = (FilesColumns < 1) ? 1 : FilesColumns;
    FilesRows = (FilesRows < 1) ? 1 : FilesRows;
}

/* Keeps the selection within the list and upon the window. */
static void FilesFollowSelection(void)
{
    if ((FilesCount != 0U) && (FilesSelected >= FilesCount))
    {
        FilesSelected = FilesCount - 1U;
    }

    if (FilesSelected < FilesTop)
    {
        FilesTop = FilesSelected;
    }

    if (FilesSelected >= (FilesTop + (size_t)FilesRows))
    {
        FilesTop = FilesSelected - (size_t)FilesRows + 1U;
    }
}

/* ------------------------------------------------------------ the input */

static void FilesHandleKey(const SyscallWindowEvent *event)
{
    const size_t page = (size_t)FilesRows;

    if (event->key_pressed == 0U)
    {
        return;
    }

    if (event->key_extended != 0U)
    {
        switch (event->key_scancode)
        {
        case 0x48U: FilesSelected = (FilesSelected > 0U) ? (FilesSelected - 1U) : 0U; break;
        case 0x50U: FilesSelected += 1U; break;
        case 0x49U: FilesSelected = (FilesSelected > page) ? (FilesSelected - page) : 0U; break;
        case 0x51U: FilesSelected += page; break;
        case 0x47U: FilesSelected = 0U; break;
        case 0x4FU: FilesSelected = (FilesCount > 0U) ? (FilesCount - 1U) : 0U; break;
        default: break;
        }
    }
    else if ((event->key_character == '\n') || (event->key_character == '\r'))
    {
        FilesOpen();
    }
    else if (event->key_character == '\b')
    {
        FilesUp();
    }

    FilesFollowSelection();
}

static void FilesHandlePress(const SyscallWindowEvent *event)
{
    const int32_t row = (event->y / (FILES_PITCH * FilesScale)) - 1;
    size_t index;

    if ((row < 0) || (row >= FilesRows))
    {
        return;
    }

    index = FilesTop + (size_t)row;

    if (index >= FilesCount)
    {
        return;
    }

    if (index == FilesSelected)
    {
        FilesOpen();
    }
    else
    {
        FilesSelected = index;
    }

    FilesFollowSelection();
}

int main(void)
{
    SyscallWindowRectangle screen;
    SyscallWindowRectangle geometry;
    bool running = true;

    if (OxysWindowScreen(&screen) != 0)
    {
        (void)fprintf(stderr, "files: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    (void)signal(SIGCHLD, FilesChildHandler);

    FilesScale = (screen.width >= 1024) ? 2 : 1;
    geometry.width = ((screen.width / 2) / (FILES_GLYPH * FilesScale)) * FILES_GLYPH * FilesScale;
    geometry.height = ((screen.height * 3 / 5) / (FILES_PITCH * FilesScale)) * FILES_PITCH * FilesScale;
    geometry.x = screen.width / 8;
    geometry.y = screen.height / 6;

    FilesWindow = OxysWindowCreate(&geometry, "Files", SYSCALL_WINDOW_LAYER_NORMAL);

    if (FilesWindow < 0)
    {
        (void)fprintf(stderr, "files: a window could not be made.\n");

        return EXIT_FAILURE;
    }

    FilesLayout(geometry.width, geometry.height);
    (void)FilesLoad();
    FilesPaint();
    FilesDraw();

    while (running)
    {
        SyscallWindowEvent event;
        bool changed = false;
        bool resized = false;
        int64_t result;

        if (FilesChildEnded != 0)
        {
            FilesReap();
        }

        result = OxysWindowEvent(FilesWindow, &event, SYSCALL_WINDOW_WAIT);

        if (result < 0)
        {
            /* EINTR is a viewer that ended, collected at the top. */
            if (errno == EINTR)
            {
                continue;
            }

            break;
        }

        while (result == 1)
        {
            switch (event.kind)
            {
            case SYSCALL_WINDOW_EVENT_KEY:
                FilesHandleKey(&event);
                changed = true;
                break;

            case SYSCALL_WINDOW_EVENT_BUTTON_PRESS:
                FilesHandlePress(&event);
                changed = true;
                break;

            case SYSCALL_WINDOW_EVENT_RESIZE:
                FilesLayout(event.x, event.y);
                FilesFollowSelection();
                resized = true;
                break;

            case SYSCALL_WINDOW_EVENT_CLOSE:
                running = false;
                break;

            default:
                break;
            }

            result = OxysWindowEvent(FilesWindow, &event, 0U);
        }

        if (resized)
        {
            FilesPaint();
        }

        if (running && (changed || resized))
        {
            FilesDraw();
        }
    }

    (void)OxysWindowDestroy(FilesWindow);
    FilesReap();

    return EXIT_SUCCESS;
}
