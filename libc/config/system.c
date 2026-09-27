/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/config/system.c
 * Purpose: The one place the configuration parser of libc/config/config.c
 *          touches the system: the file opened, read whole into a buffer, and
 *          handed to the parser.
 * Key functions: OxysConfigRead, ConfigGrowHeap.
 * References:
 *   - libc/include/config.h: the seam this implements, and why the parsing is
 *     held apart from the file it is read from.
 *   - docs/design/CONFIG.md: the division, and the program at
 *     privilege level 3 that asserts this half of it.
 *
 * This is the arrangement of libc/line/system.c a fourth time, and for the same
 * reason: the parsing can be asserted by the kernel's boot-time self-test,
 * which cannot execute a system call, and the reading can be asserted only by a
 * program, which can.
 *
 * **Nothing here may be called by the kernel.** Every path reaches SYSCALL, and
 * SYSRET returns to privilege level 3 unconditionally.
 *
 * The file is read whole before a line of it is parsed. A parser fed a piece at
 * a time would have to hold a partial line across the boundary between two
 * reads, and a configuration is some hundreds of bytes: the buffer costs less
 * than the state machine that would avoid it.
 */

#include <config.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* Declared here rather than in the header: it is the one thing this half tells
 * the parser, and no program has any business calling it. */
void OxysConfigRecordTruncation(OxysConfig *config, size_t line);

/*
 * Makes room in `more` for at least `wanted` entries, doubling so that a long
 * file grows in a few steps rather than one at a time. A failed realloc leaves
 * the old store as it was, and the entry that wanted room is a fault.
 */
static bool ConfigGrowHeap(OxysConfig *config, size_t wanted)
{
    size_t capacity = (config->more_capacity > 0U) ? config->more_capacity : CONFIG_ENTRIES_INLINE;
    OxysConfigEntry *more;

    while (capacity < wanted)
    {
        capacity *= 2U;
    }

    if (capacity > (SIZE_MAX / sizeof *more))
    {
        return false;
    }

    more = realloc(config->more, capacity * sizeof *more);

    if (more == NULL)
    {
        return false;
    }

    config->more = more;
    config->more_capacity = capacity;

    return true;
}

bool OxysConfigRead(OxysConfig *config, const char *path)
{
    static char ConfigInline[CONFIG_TEXT_INLINE + 1U];
    char *text = ConfigInline;
    size_t capacity = CONFIG_TEXT_INLINE;
    int64_t descriptor;
    size_t held = 0U;
    bool truncated = false;
    bool parsed;

    if ((config == NULL) || (path == NULL))
    {
        return false;
    }

    config->grow = ConfigGrowHeap;

    /* Emptied before the file is opened, so that a caller which ignores the
     * result of a failed read finds no settings rather than the last file's. */
    (void)OxysConfigParse(config, "", 0U);

    descriptor = OxysOpen(path, SYSCALL_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        return false;
    }

    for (;;)
    {
        int64_t taken;

        if (held == capacity)
        {
            /* Full: the rest goes to the heap, the inline text copied over
             * once, and the heap copy doubled after that. */
            char *const larger = (text == ConfigInline) ? malloc((2U * capacity) + 1U)
                                                        : realloc(text, (2U * capacity) + 1U);

            if (larger == NULL)
            {
                /*
                 * There may be more, and it cannot be held. It is recorded as
                 * a fault rather than passed over, a configuration cut off in
                 * silence being one whose last settings do nothing for a
                 * reason nobody can see.
                 */
                truncated = true;
                break;
            }

            if (text == ConfigInline)
            {
                (void)memcpy(larger, ConfigInline, held);
            }

            text = larger;
            capacity *= 2U;
        }

        taken = OxysRead((int)descriptor, &text[held], capacity - held);

        if (taken < 0)
        {
            (void)OxysClose((int)descriptor);

            if (text != ConfigInline)
            {
                free(text);
            }

            return false;
        }

        if (taken == 0)
        {
            break;
        }

        held += (size_t)taken;
    }

    (void)OxysClose((int)descriptor);

    text[held] = '\0';
    parsed = OxysConfigParse(config, text, held);

    if (truncated)
    {
        size_t lines = 1U;

        for (size_t index = 0U; index < held; ++index)
        {
            if (text[index] == '\n')
            {
                ++lines;
            }
        }

        OxysConfigRecordTruncation(config, lines);
        parsed = false;
    }

    /* The entries are copies; the text is not needed once parsed. */
    if (text != ConfigInline)
    {
        free(text);
    }

    return parsed;
}
