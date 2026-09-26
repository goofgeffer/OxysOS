/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sysinfo/main.c
 * Purpose: The System Info program: a window stating what the machine is and
 *          is doing — the system's version, the processors, the memory, the
 *          time since start, the processes and threads, the screen, the date
 *          and the entropy pool's estimate — redrawn every second. Added on
 *          2026-09-25 in place of the window demonstration, at the project
 *          owner's request.
 * Key functions: main, InfoDraw, InfoLine.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: `sysinfo`, `version`, `window_screen`,
 *     `time` and `alarm`.
 *   - docs/design/UTILITIES.md: the design.
 *
 * Why a second, and why the alarm. The uptime is the one figure that changes
 * while a person watches, and a second is its unit; the alarm ends the wait for
 * an event with EINTR, so the window is redrawn without a thread and without
 * polling.
 */

#include <palette.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <time.h>

#define INFO_GLYPH   8
#define INFO_PITCH   12
#define INFO_COLUMNS 42
#define INFO_ROWS    13
#define INFO_LABEL   14

#define INFO_PAPER OXYS_RGB(OXYS_PAPER_RED, OXYS_PAPER_GREEN, OXYS_PAPER_BLUE)
#define INFO_INK   OXYS_RGB(OXYS_INK_RED, OXYS_INK_GREEN, OXYS_INK_BLUE)
#define INFO_DIM   OXYS_RGB(OXYS_DIM_RED, OXYS_DIM_GREEN, OXYS_DIM_BLUE)

static int64_t InfoWindow = -1;
static int32_t InfoScale = 2;
static SyscallWindowRectangle InfoScreen;

static volatile sig_atomic_t InfoTick;

static void InfoAlarmHandler(int signal)
{
    (void)signal;
    InfoTick = 1;
}

/* One row: a label in the dim ink and its value, the row cleared to its end so
 * that a value that shortened leaves nothing of the longer one behind. */
static void InfoLine(int32_t row, const char *label, const char *value)
{
    char text[INFO_COLUMNS + 1];
    SyscallWindowText placement;

    placement.y = (row * INFO_PITCH * InfoScale) + (((INFO_PITCH - INFO_GLYPH) / 2) * InfoScale);
    placement.paper = INFO_PAPER;
    placement.scale = InfoScale;

    placement.x = INFO_GLYPH * InfoScale;
    placement.ink = INFO_DIM;
    (void)OxysWindowText(InfoWindow, &placement, label);

    (void)snprintf(text, sizeof text, "%-*s", INFO_COLUMNS - INFO_LABEL - 1, value);
    placement.x = INFO_LABEL * INFO_GLYPH * InfoScale;
    placement.ink = INFO_INK;
    (void)OxysWindowText(InfoWindow, &placement, text);
}

/* A number of bytes in the largest unit that leaves it at least one. */
static void InfoBytes(char *text, size_t capacity, uint64_t bytes)
{
    if (bytes >= (UINT64_C(1) << 30))
    {
        (void)snprintf(text, capacity, "%llu.%01llu GiB", (unsigned long long)(bytes >> 30),
                       (unsigned long long)(((bytes >> 20) % 1024U) * 10U / 1024U));
    }
    else
    {
        (void)snprintf(text, capacity, "%llu MiB", (unsigned long long)(bytes >> 20));
    }
}

static void InfoDraw(void)
{
    SyscallSystemInformation information;
    char version[32];
    char value[INFO_COLUMNS + 1];
    char first[16];
    char second[16];
    const time_t now = time(NULL);
    struct tm broken;
    int32_t row = 1;

    (void)memset(&information, 0, sizeof information);
    (void)OxysSystemInformation(&information);

    if (OxysVersion(version, sizeof version) < 0)
    {
        (void)snprintf(version, sizeof version, "unknown");
    }

    (void)snprintf(value, sizeof value, "%s", version);
    InfoLine(row++, "System", value);
    row++;

    (void)snprintf(value, sizeof value, "%llu running",
                   (unsigned long long)information.processors);
    InfoLine(row++, "Processors", value);

    InfoBytes(first, sizeof first, information.memory_free_bytes);
    InfoBytes(second, sizeof second, information.memory_bytes);
    (void)snprintf(value, sizeof value, "%s free of %s", first, second);
    InfoLine(row++, "Memory", value);

    {
        const uint64_t seconds = information.uptime_milliseconds / 1000U;

        (void)snprintf(value, sizeof value, "%llu h %02llu min %02llu s",
                       (unsigned long long)(seconds / 3600U),
                       (unsigned long long)((seconds / 60U) % 60U),
                       (unsigned long long)(seconds % 60U));
        InfoLine(row++, "Up for", value);
    }

    (void)snprintf(value, sizeof value, "%llu, with %llu thread(s)",
                   (unsigned long long)information.processes,
                   (unsigned long long)information.threads);
    InfoLine(row++, "Processes", value);
    row++;

    (void)snprintf(value, sizeof value, "%d by %d", (int)InfoScreen.width, (int)InfoScreen.height);
    InfoLine(row++, "Screen", value);

    if ((now >= 0) && (gmtime_r(&now, &broken) != NULL))
    {
        (void)snprintf(value, sizeof value, "%04d-%02d-%02d %02d:%02d", broken.tm_year + 1900,
                       broken.tm_mon + 1, broken.tm_mday, broken.tm_hour, broken.tm_min);
    }
    else
    {
        (void)snprintf(value, sizeof value, "no clock");
    }

    InfoLine(row++, "Date", value);

    (void)snprintf(value, sizeof value, "%llu bit(s) estimated",
                   (unsigned long long)information.entropy_bits);
    InfoLine(row++, "Entropy", value);
}

/* The window's ground, once, and again when it is given a new extent. */
static void InfoPaint(int32_t width, int32_t height)
{
    static uint32_t tile[1024];
    SyscallWindowRectangle area;
    const int32_t rows = (int32_t)(sizeof tile / sizeof tile[0]) / ((width > 0) ? width : 1);

    if ((width <= 0) || (rows <= 0))
    {
        return;
    }

    for (size_t index = 0U; index < (sizeof tile / sizeof tile[0]); ++index)
    {
        tile[index] = INFO_PAPER;
    }

    area.x = 0;
    area.width = width;

    for (int32_t top = 0; top < height; top += rows)
    {
        area.y = top;
        area.height = ((top + rows) <= height) ? rows : (height - top);
        (void)OxysWindowBlit(InfoWindow, &area, tile);
    }
}

int main(void)
{
    SyscallWindowRectangle geometry;
    bool running = true;

    if (OxysWindowScreen(&InfoScreen) != 0)
    {
        (void)fprintf(stderr, "sysinfo: the window manager does not have the screen.\n");

        return EXIT_FAILURE;
    }

    InfoScale = (InfoScreen.width >= 1024) ? 2 : 1;
    geometry.width = INFO_COLUMNS * INFO_GLYPH * InfoScale;
    geometry.height = INFO_ROWS * INFO_PITCH * InfoScale;
    geometry.x = (InfoScreen.width - geometry.width) / 2;
    geometry.y = (InfoScreen.height - geometry.height) / 3;

    InfoWindow = OxysWindowCreate(&geometry, "System Info", SYSCALL_WINDOW_LAYER_NORMAL);

    if (InfoWindow < 0)
    {
        (void)fprintf(stderr, "sysinfo: a window could not be made.\n");

        return EXIT_FAILURE;
    }

    (void)signal(SIGALRM, InfoAlarmHandler);
    InfoPaint(geometry.width, geometry.height);
    InfoDraw();
    (void)OxysAlarm(1000U);

    while (running)
    {
        SyscallWindowEvent event;
        int64_t result = OxysWindowEvent(InfoWindow, &event, SYSCALL_WINDOW_WAIT);

        if (InfoTick != 0)
        {
            InfoTick = 0;
            InfoDraw();
            (void)OxysAlarm(1000U);
        }

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
            if (event.kind == SYSCALL_WINDOW_EVENT_RESIZE)
            {
                InfoPaint(event.x, event.y);
                InfoDraw();
            }
            else if (event.kind == SYSCALL_WINDOW_EVENT_CLOSE)
            {
                running = false;
            }

            result = OxysWindowEvent(InfoWindow, &event, 0U);
        }
    }

    (void)OxysWindowDestroy(InfoWindow);

    return EXIT_SUCCESS;
}
