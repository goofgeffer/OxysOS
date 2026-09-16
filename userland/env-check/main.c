/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/env-check/main.c
 * Purpose: Asserts what the shell of sub-task 8.4 gives a program it runs —
 *          the argument vector as the person typed it, quotes removed, and the
 *          environment of the variables the shell exported and none it did not
 *          — and ends with the number of assertions that failed.
 * Key functions: main, EnvironmentRequire.
 * References:
 *   - IEEE Std 1003.1-2017, Section 2.9.1 (Simple Commands): the words after
 *     expansion are the command's arguments; Section 2.12 (Shell Execution
 *     Environment): the exported variables are the environment of a command.
 *   - ISO/IEC 9899:2011, Section 7.22.4.6: `getenv`, present since 8.4.
 *   - kernel/test/shell/parser.c: the self-test that places this program upon
 *     the root, runs the shell upon a session that invokes it, and checks the
 *     status the shell ends with. The two must agree about the vector and the
 *     environment below.
 *   - docs/design/SHELL.md, Section 18.
 *
 * Why this program is written onto the root by the test rather than embedded
 * in the shell's path.
 *
 *   The shell finds a program by executing a pathname, and a check program
 *   is not shipped in /bin (docs/storage/INITRD.md, Section 2). So the test
 *   writes it to /verify/env-check for the duration of the session and removes
 *   it afterwards — which is itself an assertion that a program written onto
 *   the root at run time can be found, loaded and run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int EnvironmentFailures;

static void EnvironmentRequire(int condition, const char *statement)
{
    if (!condition)
    {
        ++EnvironmentFailures;
        (void)printf("  %s FAILED.\n", statement);
    }
}

int main(int argc, char *argv[], char *envp[])
{
    const char *mark;

    (void)printf("env-check: the vector and the environment the shell gave a program.\n");

    /* The vector: the name as typed, and the two operands with their quotes
     * removed — `"b c"` one word, not two. */
    EnvironmentRequire(argc == 3, "the argument count is not three");
    EnvironmentRequire((argc >= 1) && (strcmp(argv[0], "/verify/env-check") == 0),
                       "argv[0] is not the pathname the shell was given");
    EnvironmentRequire((argc >= 2) && (strcmp(argv[1], "alpha") == 0),
                       "argv[1] is not alpha");
    EnvironmentRequire((argc >= 3) && (strcmp(argv[2], "b c") == 0),
                       "argv[2] is not the quoted operand as one word");

    /* The environment: exported reaches, assigned does not, and the shell's
     * own PWD comes along because the shell exports it. */
    EnvironmentRequire(envp != NULL, "the environment vector is null");
    mark = getenv("MARK");
    EnvironmentRequire((mark != NULL) && (strcmp(mark, "abcd") == 0),
                       "an exported variable did not reach the environment");
    EnvironmentRequire(getenv("HIDDEN") == NULL,
                       "a variable that was assigned and not exported reached the environment");
    EnvironmentRequire(getenv("PWD") != NULL, "PWD did not reach the environment");
    EnvironmentRequire(getenv("MAR") == NULL, "getenv matched a prefix of a name");

    (void)printf("env-check: %d failure(s).\n", EnvironmentFailures);
    (void)fflush(stdout);

    return EnvironmentFailures;
}
