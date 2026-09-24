/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/config/system.c
 * Purpose: The one place the configuration parser of libc/config/config.c
 *          touches the system: the file opened, read whole into a buffer, and
 *          handed to the parser.
 * Key functions: OxysConfigRead.
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
#include <syscall.h>

/* Declared here rather than in the header: it is the one thing this half tells
 * the parser, and no program has any business calling it. */
void OxysConfigRecordTruncation(OxysConfig *config, size_t line);

bool OxysConfigRead(OxysConfig *config, const char *path)
{
    static char ConfigText[CONFIG_TEXT_MAXIMUM + 1U];
    int64_t descriptor;
    size_t held = 0U;
    bool truncated = false;
    bool parsed;

    if ((config == NULL) || (path == NULL))
    {
        return false;
    }

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
        const int64_t taken =
            OxysRead((int)descriptor, &ConfigText[held], (CONFIG_TEXT_MAXIMUM - held));

        if (taken < 0)
        {
            (void)OxysClose((int)descriptor);

            return false;
        }

        if (taken == 0)
        {
            break;
        }

        held += (size_t)taken;

        if (held >= CONFIG_TEXT_MAXIMUM)
        {
            /*
             * There may be more, and what there is will not be read. It is
             * recorded as a fault rather than passed over, a configuration cut
             * off in silence being a configuration whose last settings do
             * nothing for a reason nobody can see.
             */
            truncated = true;
            break;
        }
    }

    (void)OxysClose((int)descriptor);

    ConfigText[held] = '\0';
    parsed = OxysConfigParse(config, ConfigText, held);

    if (truncated)
    {
        size_t lines = 1U;

        for (size_t index = 0U; index < held; ++index)
        {
            if (ConfigText[index] == '\n')
            {
                ++lines;
            }
        }

        OxysConfigRecordTruncation(config, lines);
        parsed = false;
    }

    return parsed;
}
