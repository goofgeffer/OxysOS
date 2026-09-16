/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/dir-check/main.c
 * Purpose: Asserts the working directory of sub-task 8.3 from the only place
 *          it can be asserted — a program — and ends with the number of
 *          assertions that failed: that a process begins at the root, that
 *          `chdir` moves it and `getcwd` reports it in canonical form, that a
 *          relative path is resolved against it by every call, that a child of
 *          `fork` inherits it, and that each refusal is the named one.
 * Key functions: main, DirectoryRequire, DirectoryIs.
 * References:
 *   - IEEE Std 1003.1-2017, `chdir()`, `getcwd()`, and Section 4.13, Pathname
 *     Resolution: a relative pathname is resolved from the working directory,
 *     and `..` at the root is the root.
 *   - kernel/abi/oxys/syscall_abi.h: the two calls and the refusals — ENOENT,
 *     ENOTDIR, ENAMETOOLONG — and ERANGE, which the C library reports where
 *     the kernel says ENAMETOOLONG of a buffer.
 *   - kernel/test/proc/directory.c: the self-test that runs this and checks
 *     the number it ends with.
 *   - docs/design/SHELL.md, Section 14.
 *
 * Why the refusals are asserted by name, as file-check's are.
 *
 *   A `chdir` that failed for the wrong reason would tell a person to look in
 *   the wrong place, and a `chdir` into a *file* that succeeded would make
 *   every later relative path fail somewhere else entirely. The working
 *   directory is the one piece of state every path-taking call reads, so a
 *   defect in it appears as a defect in whichever call was made next.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

static int DirectoryFailures;

static void DirectoryRequire(int condition, const char *statement)
{
    if (!condition)
    {
        ++DirectoryFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

/* Whether the working directory, as the kernel reports it, is `expected`. */
static int DirectoryIs(const char *expected)
{
    char directory[SYSCALL_PATH_MAXIMUM + 1U];

    if (OxysGetWorkingDirectory(directory, sizeof directory) < 0)
    {
        return 0;
    }

    return strcmp(directory, expected) == 0;
}

int main(void)
{
    static char long_name[SYSCALL_PATH_MAXIMUM];
    char small[4];
    int64_t descriptor;
    int64_t child;

    (void)printf("dir-check: the working directory, and the calls that read it.\n");

    /* A process begins at the root. */
    DirectoryRequire(DirectoryIs("/"), "a program does not begin at the root");

    /* chdir moves it, getcwd reports it, and a relative path is resolved
     * against it — by open, which is not one of the two calls. */
    DirectoryRequire(OxysChangeDirectory("/bin") == 0, "chdir to /bin failed");
    DirectoryRequire(DirectoryIs("/bin"), "getcwd did not report /bin after chdir");

    descriptor = OxysOpen("echo", SYSCALL_OPEN_READ);
    DirectoryRequire(descriptor >= 0, "a relative path was not resolved against the working "
                                      "directory by open");

    if (descriptor >= 0)
    {
        (void)OxysClose((int)descriptor);
    }

    /* `.` and `..` are honoured and the report is canonical: no `.`, no `..`,
     * no repeated separator, and `..` at the root stays at the root. */
    DirectoryRequire(OxysChangeDirectory("..") == 0, "chdir to .. failed");
    DirectoryRequire(DirectoryIs("/"), "chdir to .. from /bin did not reach the root");
    DirectoryRequire(OxysChangeDirectory("bin/../bin/./") == 0,
                     "chdir through . and .. failed");
    DirectoryRequire(DirectoryIs("/bin"), "getcwd did not report the canonical /bin");
    DirectoryRequire(OxysChangeDirectory("../..") == 0, "chdir above the root failed");
    DirectoryRequire(DirectoryIs("/"), ".. above the root did not stay at the root");
    DirectoryRequire((OxysChangeDirectory("//bin//") == 0) && DirectoryIs("/bin"),
                     "repeated separators were not reduced");

    /* The refusals, each by name, and each leaving the directory as it was. */
    errno = 0;
    DirectoryRequire((OxysChangeDirectory("nonexistent") < 0) && (errno == ENOENT),
                     "chdir to a name that does not exist was not ENOENT");
    DirectoryRequire(DirectoryIs("/bin"), "a refused chdir changed the directory");

    errno = 0;
    DirectoryRequire((OxysChangeDirectory("/bin/echo") < 0) && (errno == ENOTDIR),
                     "chdir to a file was not ENOTDIR");
    DirectoryRequire(DirectoryIs("/bin"), "a chdir to a file changed the directory");

    errno = 0;
    DirectoryRequire((OxysGetWorkingDirectory(small, sizeof small) < 0) && (errno == ERANGE),
                     "getcwd into a buffer too small was not ERANGE");

    /* A relative path that fits by itself and not once joined to the working
     * directory is refused as too long, the bound being upon the whole. */
    memset(long_name, 'a', SYSCALL_PATH_MAXIMUM - 3U);
    long_name[SYSCALL_PATH_MAXIMUM - 3U] = '\0';
    errno = 0;
    DirectoryRequire((OxysChangeDirectory(long_name) < 0) && (errno == ENAMETOOLONG),
                     "a relative path too long once joined was not ENAMETOOLONG");

    /* A child of fork inherits it, and the parent's is untouched by the
     * child's chdir. The child ends with 0 where it found itself in /bin and
     * then reached the root, and with 1 otherwise. */
    child = OxysFork();

    if (child == 0)
    {
        int inherited = DirectoryIs("/bin");

        (void)OxysChangeDirectory("/");
        OxysExit((inherited && DirectoryIs("/")) ? 0 : 1);
    }

    DirectoryRequire(child > 0, "fork failed");

    if (child > 0)
    {
        int64_t status = -1;

        DirectoryRequire(OxysWait(&status) == child, "wait did not collect the child");
        DirectoryRequire(status == 0, "the child did not inherit the working directory");
    }

    DirectoryRequire(DirectoryIs("/bin"), "the child's chdir changed the parent's directory");

    DirectoryRequire(OxysChangeDirectory("/") == 0, "chdir back to the root failed");

    (void)printf("dir-check: %d failure(s).\n", DirectoryFailures);
    (void)fflush(stdout);

    return DirectoryFailures;
}
