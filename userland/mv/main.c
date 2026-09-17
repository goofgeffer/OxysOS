/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/mv/main.c
 * Purpose: Renames a file: a second name made by `link`, the first removed by
 *          `unlink`. Added on 2026-09-16 beside sub-task 8.7, with the `link`
 *          call it needed.
 * Key functions: main, MoveOne.
 * References:
 *   - IEEE Std 1003.1-2017, `mv`: `mv source target`, and `mv source...
 *     directory`, where the target is a directory and each source is moved
 *     into it under its own last component; a rename across filesystems is
 *     done by copying, which this does not do — EXDEV is reported instead.
 *   - IEEE Std 1003.1-2017, `link()` and `unlink()`: the two calls a rename
 *     is made of where there is no `rename()`, which this kernel has not.
 *   - docs/design/SHELL.md, Section 30.
 *
 * Why two calls and not one.
 *
 *   The filesystem layer links and unlinks and does not rename, and a rename
 *   is a link followed by an unlink — the same file under a second name, then
 *   without the first. It is not atomic: a machine that stopped between the
 *   two would have the file under both names, which is a state `ls` shows and
 *   `rm` mends, and not one in which anything is lost. A directory cannot be
 *   moved this way, `link` refusing a directory, and is refused with the
 *   layer's own reason.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* Whether a path names a directory: it can be opened as one. */
static bool MoveIsDirectory(const char *path)
{
    const int64_t descriptor = OxysOpen(path, SYSCALL_OPEN_READ | SYSCALL_OPEN_DIRECTORY, 0U);

    if (descriptor < 0)
    {
        return false;
    }

    (void)OxysClose((int)descriptor);

    return true;
}

static const char *MoveLastComponent(const char *path)
{
    const char *last = path;

    for (const char *at = path; *at != '\0'; ++at)
    {
        if ((*at == '/') && (at[1] != '\0'))
        {
            last = at + 1;
        }
    }

    return last;
}

/* Links `source` at `target` and unlinks `source`. Returns false having
 * said why; a source left under both names is said too. */
static bool MoveOne(const char *source, const char *target)
{
    if (OxysLink(source, target) < 0)
    {
        (void)fprintf(stderr, "mv: %s to %s: %s\n", source, target, strerror(errno));

        return false;
    }

    if (OxysUnlink(source) < 0)
    {
        (void)fprintf(stderr, "mv: %s: %s; it stands under both names.\n", source,
                      strerror(errno));

        return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
    const char *target;
    int status = EXIT_SUCCESS;

    if (argc < 3)
    {
        (void)fprintf(stderr, "mv: a source and a target are required.\n");

        return EXIT_FAILURE;
    }

    target = argv[argc - 1];

    if (MoveIsDirectory(target))
    {
        for (int index = 1; index < argc - 1; ++index)
        {
            char into[SYSCALL_PATH_MAXIMUM + 1U];
            const int written = snprintf(into, sizeof into, "%s/%s", target,
                                         MoveLastComponent(argv[index]));

            if ((written < 0) || ((size_t)written >= sizeof into))
            {
                (void)fprintf(stderr, "mv: %s: the path is too long.\n", argv[index]);
                status = EXIT_FAILURE;
                continue;
            }

            if (!MoveOne(argv[index], into))
            {
                status = EXIT_FAILURE;
            }
        }

        return status;
    }

    if (argc != 3)
    {
        (void)fprintf(stderr, "mv: %s is not a directory.\n", target);

        return EXIT_FAILURE;
    }

    return MoveOne(argv[1], target) ? EXIT_SUCCESS : EXIT_FAILURE;
}
