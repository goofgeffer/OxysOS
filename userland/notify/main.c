/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/notify/main.c
 * Purpose: Posts a notification from the shell: `notify [-i|-s|-w|-e] text...`,
 *          the words joined by spaces, shown by the session at the bottom right
 *          of the screen for a few seconds. Added on 2026-09-25 with the
 *          notifications, so that a script can say it has finished and a person
 *          can see what one looks like.
 * Key functions: main.
 * References:
 *   - kernel/abi/oxys/syscall_abi.h: `notify`, the kinds and the bound.
 *   - docs/design/SESSION.md: how the session shows one.
 *
 * The option chooses the symbol: -i information (the default), -s success,
 * -w warning, -e error. A text longer than the kernel's bound is refused by the
 * kernel and said here, rather than cut, because a person who typed it should
 * know it was not shown as typed.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

int main(int argc, char **argv)
{
    char text[SYSCALL_NOTIFICATION_TEXT_MAXIMUM + 2U];
    uint64_t kind = SYSCALL_NOTIFY_INFORMATION;
    size_t used = 0U;
    int first = 1;

    if ((argc > 1) && (argv[1][0] == '-') && (argv[1][1] != '\0') && (argv[1][2] == '\0'))
    {
        switch (argv[1][1])
        {
        case 'i':
            kind = SYSCALL_NOTIFY_INFORMATION;
            break;
        case 's':
            kind = SYSCALL_NOTIFY_SUCCESS;
            break;
        case 'w':
            kind = SYSCALL_NOTIFY_WARNING;
            break;
        case 'e':
            kind = SYSCALL_NOTIFY_ERROR;
            break;
        default:
            (void)fprintf(stderr, "usage: notify [-i|-s|-w|-e] text...\n");
            return EXIT_FAILURE;
        }

        first = 2;
    }

    if (first >= argc)
    {
        (void)fprintf(stderr, "usage: notify [-i|-s|-w|-e] text...\n");
        return EXIT_FAILURE;
    }

    /* The words joined, stopping one past the bound so that a text too long
     * is still too long when it reaches the kernel, and is refused there. */
    for (int index = first; (index < argc) && (used < (sizeof text - 1U)); ++index)
    {
        for (size_t at = 0U; (argv[index][at] != '\0') && (used < (sizeof text - 1U)); ++at)
        {
            text[used++] = argv[index][at];
        }

        if (((index + 1) < argc) && (used < (sizeof text - 1U)))
        {
            text[used++] = ' ';
        }
    }

    text[used] = '\0';

    if (OxysNotify(kind, 0U, text) < 0)
    {
        (void)fprintf(stderr, "notify: %s%s\n", strerror(errno),
                      (errno == EINVAL) ? " (at most 63 characters)" : "");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
