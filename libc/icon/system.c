/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/icon/system.c
 * Purpose: The one place the icon reader of libc/icon/icon.c touches the
 *          system: the file opened, read whole into a buffer, and handed to the
 *          parser.
 * Key functions: OxysIconRead.
 * References:
 *   - libc/include/icon.h: the seam this implements, and the format.
 *   - docs/design/SESSION.md, Section 8: what reads icons and when.
 *
 * This is the arrangement of libc/config/system.c a fifth time, and for the
 * same reason: the understanding of the bytes can be asserted by the kernel's
 * boot-time self-test, which cannot execute a system call, and the reading can
 * be asserted only by something that can.
 *
 * **Nothing here may be called by the kernel.** Every path reaches SYSCALL, and
 * SYSRET returns to privilege level 3 unconditionally.
 *
 * A file one byte longer than an icon of its extent is refused rather than
 * truncated to fit, which is the parser's rule and is why the read asks for one
 * byte more than the largest icon: a file that fills the buffer exactly is
 * indistinguishable from one that was cut off at it, and the extra byte is what
 * tells the two apart.
 */

#include <icon.h>
#include <syscall.h>

bool OxysIconRead(OxysIcon *icon, const char *path)
{
    static uint8_t IconBytes[ICON_BYTES_MAXIMUM + 1U];
    int64_t descriptor;
    size_t held = 0U;

    if ((icon == NULL) || (path == NULL))
    {
        return false;
    }

    /* Emptied before the file is opened, so that a caller which ignores the
     * result of a failed read draws nothing rather than the last icon. */
    (void)OxysIconParse(icon, NULL, 0U);

    descriptor = OxysOpen(path, SYSCALL_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        return false;
    }

    for (;;)
    {
        const int64_t taken =
            OxysRead((int)descriptor, &IconBytes[held], (sizeof IconBytes) - held);

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

        if (held >= sizeof IconBytes)
        {
            /* Larger than any icon this holds. The parser would refuse it for
             * its length anyway; it is stopped here so that a file of a
             * megabyte is not read a megabyte at a time first. */
            break;
        }
    }

    (void)OxysClose((int)descriptor);

    return OxysIconParse(icon, IconBytes, held);
}
