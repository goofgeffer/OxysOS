/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/image/system.c
 * Purpose: The one place the image reader of libc/image/image.c touches the
 *          system: the file opened, read whole into the caller's buffer, and
 *          handed to the parser.
 * Key functions: OxysImageRead.
 * References:
 *   - libc/include/image.h: the seam this implements, and the format.
 *   - libc/icon/system.c: the same arrangement for icons, whose reasons this
 *     shares.
 *
 * **Nothing here may be called by the kernel.** Every path reaches SYSCALL,
 * and SYSRET returns to privilege level 3 unconditionally.
 *
 * A file that fills the buffer is refused: one that fills it exactly cannot be
 * told apart from one that was cut off at it, and a background cut off at the
 * end of the buffer parses as nothing rather than as the top of a picture only
 * because the parser counts rows — which is one check standing where two
 * should.
 */

#include <image.h>
#include <syscall.h>

bool OxysImageRead(OxysImage *image, const char *path, uint8_t *buffer, size_t capacity)
{
    int64_t descriptor;
    size_t held = 0U;

    if ((image == NULL) || (path == NULL) || (buffer == NULL) || (capacity == 0U))
    {
        return false;
    }

    (void)OxysImageParse(image, NULL, 0U);

    descriptor = OxysOpen(path, SYSCALL_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        return false;
    }

    for (;;)
    {
        const int64_t taken = OxysRead((int)descriptor, &buffer[held], capacity - held);

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

        if (held >= capacity)
        {
            (void)OxysClose((int)descriptor);

            return false;
        }
    }

    (void)OxysClose((int)descriptor);

    return OxysImageParse(image, buffer, held);
}
