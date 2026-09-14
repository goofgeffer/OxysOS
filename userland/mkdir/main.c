/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/mkdir/main.c
 * Purpose: Creates each directory operand — the first program upon this system
 *          that changes a volume.
 * Key functions: main, MakeParents, MakeOne.
 * References:
 *   - IEEE Std 1003.1-2017 (POSIX.1-2017), `mkdir`: the synopsis is
 *     `mkdir [-p] [-m mode] dir...`; without -m the mode is the bitwise
 *     inclusive OR of S_IRWXU, S_IRWXG and S_IRWXO, reduced by the file mode
 *     creation mask; with -p, missing intermediate components are created and an
 *     existing directory is not an error. The exit status is 0 where every
 *     directory was created and greater than zero otherwise.
 *   - kernel/abi/oxys/syscall_abi.h: SYSCALL_MKDIR, beneath `OxysMakeDirectory`.
 *   - docs/design/LIBC.md, Section 12.3: the five utilities and what each is
 *     for.
 *
 * The mode, and what this system does not have.
 *
 *   The default is 0777, which is what POSIX names. **It is not reduced**, this
 *   system having no file mode creation mask — and having no credentials for one
 *   to belong to. So a directory made here is world-writable, and that is a
 *   property of a system with one user and no permission checking rather than a
 *   decision this program makes. docs/design/PROCESS.md records the absence of
 *   credentials; docs/design/LIBC.md, Section 12.7, limitation 6, records this
 *   consequence of it.
 *
 *   **-m is not implemented.** Its argument is a symbolic mode of the form
 *   `chmod` accepts, which is a parser of its own, and no caller of this program
 *   can presently observe the difference a mode makes: nothing in this system
 *   checks a permission bit before an operation. A flag that was accepted and
 *   had no effect would be worse than one that is refused.
 *
 * What -p does here, and the one thing it cannot do.
 *
 *   It creates each missing component of the path in turn and treats an existing
 *   directory as success, which is the whole of what POSIX requires of it. It
 *   cannot tell a component that already exists as a *directory* from one that
 *   exists as a file: `OxysMakeDirectory` reports EEXIST for both, and there is
 *   no call here that asks what a path names. So `mkdir -p /a/b` where `/a` is a
 *   regular file reports the failure at `/a/b` — as ENOTDIR — rather than at
 *   `/a`, which is the right outcome reached by the wrong route.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

/* The mode POSIX names when -m is not given. It is written in octal because a
 * mode is nine bits in three groups of three and every other notation obscures
 * that. */
#define MKDIR_DEFAULT_MODE 0777

/* One component of a path being built up by -p may be no longer than the whole
 * path, and the kernel bounds the whole path. */
#define MKDIR_PATH_BYTES (SYSCALL_PATH_MAXIMUM + 1U)

static void MakeComplain(const char *path)
{
    (void)fprintf(stderr, "mkdir: %s: %s\n", path, strerror(errno));
}

/* Creates one directory. Returns false where it could not, having said why.
 * `tolerate_existing` is what -p asks for. */
static bool MakeOne(const char *path, bool tolerate_existing)
{
    if (OxysMakeDirectory(path, MKDIR_DEFAULT_MODE) >= 0)
    {
        return true;
    }

    if (tolerate_existing && (errno == EEXIST))
    {
        return true;
    }

    MakeComplain(path);

    return false;
}

/*
 * Creates every missing component of a path, and then the path itself.
 *
 * The prefix is built up in a buffer of its own rather than by writing a
 * terminator into the operand and putting the separator back. The operand is
 * `argv[index]`, which stands upon this program's own stack, and modifying it
 * would work — but a utility that scribbles upon its arguments is one whose
 * diagnostics afterwards name a path that has been cut short.
 */
static bool MakeParents(const char *path)
{
    char prefix[MKDIR_PATH_BYTES];
    size_t length = strlen(path);
    size_t index;

    if (length >= sizeof prefix)
    {
        errno = ENAMETOOLONG;
        MakeComplain(path);

        return false;
    }

    for (index = 0U; index < length; ++index)
    {
        /*
         * A separator at the start of the path, and a run of them anywhere, name
         * no component: `/a` has one component and `//a//b` has two. Skipping an
         * empty component is what stops this program trying to create the root.
         */
        if ((path[index] != '/') || (index == 0U) || (path[index - 1U] == '/'))
        {
            continue;
        }

        memcpy(prefix, path, index);
        prefix[index] = '\0';

        if (!MakeOne(prefix, true))
        {
            return false;
        }
    }

    return MakeOne(path, true);
}

int main(int argc, char *argv[])
{
    bool parents = false;
    bool succeeded = true;
    int first = 1;

    while ((first < argc) && (argv[first][0] == '-') && (argv[first][1] != '\0'))
    {
        if (strcmp(argv[first], "--") == 0)
        {
            ++first;
            break;
        }

        if (strcmp(argv[first], "-p") == 0)
        {
            parents = true;
            ++first;
            continue;
        }

        (void)fprintf(stderr, "mkdir: %s: this option is not implemented\n", argv[first]);

        return EXIT_FAILURE;
    }

    if (first >= argc)
    {
        (void)fprintf(stderr, "mkdir: no directory was named\n");

        return EXIT_FAILURE;
    }

    for (int index = first; index < argc; ++index)
    {
        const bool made = parents ? MakeParents(argv[index]) : MakeOne(argv[index], false);

        if (!made)
        {
            succeeded = false;
        }
    }

    return succeeded ? EXIT_SUCCESS : EXIT_FAILURE;
}
