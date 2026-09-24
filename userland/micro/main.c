/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/micro/main.c
 * Purpose: A very small line editor — the first thing upon this system that
 *          can change a file rather than write one: `micro FILE` loads the
 *          file, prints it with line numbers, and takes commands that append,
 *          insert, edit and delete lines, each line edited through the same
 *          editor the shell reads its commands with, until `w` writes it back.
 * Key functions: main, MicroLoad, MicroWrite, MicroPrint, MicroCommand,
 *          MicroReadLines, MicroParseNumber.
 * References:
 *   - IEEE Std 1003.1-2017, `ed`: the shape of a line editor — numbered lines,
 *     `a`, `i`, `d`, `p`, `w` and `q`, input ended by a line holding one `.` —
 *     which this borrows the letters and the manner of and none of the rest
 *     of; there is no address syntax, no regular expression and no `s`.
 *   - libc/include/line.h: LineRead and LineEdit, the second of which hands a
 *     line back to be changed, which is what makes `e` an editor's command.
 *   - docs/design/SHELL.md.
 *
 * Why a line editor and not a screen.
 *
 *   Neither the text-mode display nor the framebuffer console interprets a
 *   cursor-positioning sequence, and the line editor beneath the shell draws
 *   with printable characters, spaces and backspaces alone so that it draws
 *   the same upon a serial terminal. A screen editor would need the display
 *   to move the cursor up and clear a line, and a program to learn the
 *   screen's size; none of that exists, and an editor that assumed it would
 *   scroll the screen into nonsense. A line editor asks nothing of the display
 *   the shell does not already ask, and is therefore the editor this system
 *   can have today. The project owner asked for the smallest editor that
 *   could edit, on 2026-09-16, and this is it.
 *
 * The file is held whole and written whole. There is no `lseek` a program can
 * reach, so a change in the middle of a file is the file rewritten from its
 * start — every line written again — which is what `w` does, and why nothing
 * touches the file between the load and the write: a person who quits with
 * `q!` has a file exactly as it was. Since 2026-09-24 the lines are written
 * into a file beside it, which takes its name only once whole, MicroWrite.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syscall.h>
#include <line.h>

/* How many lines a file may hold here, and the buffer a read is made into. A
 * line longer than the editor's capacity is cut when it is edited, and is said
 * to be. */
#define MICRO_LINE_MAXIMUM 1024U
#define MICRO_READ_BYTES   4096U

#define MICRO_PROMPT "micro> "
#define MICRO_END    "."

/* What is added to a file's name for the file a save is written into before it
 * takes the name: MicroWrite. */
#define MICRO_SAVE_SUFFIX ".micro-save"

static char *MicroLines[MICRO_LINE_MAXIMUM];
static size_t MicroLineCount;
static bool MicroModified;
static const char *MicroPath;

static LineEditor MicroEditor;

/* ---------------------------------------------------------------- the lines */

static char *MicroDuplicate(const char *text, size_t length)
{
    char *const copy = malloc(length + 1U);

    if (copy == NULL)
    {
        return NULL;
    }

    memcpy(copy, text, length);
    copy[length] = '\0';

    return copy;
}

/* Places a line before `position`, which may be the count for the end. */
static bool MicroInsertLine(size_t position, const char *text)
{
    char *copy;

    if ((MicroLineCount >= MICRO_LINE_MAXIMUM) || (position > MicroLineCount))
    {
        (void)fprintf(stderr, "micro: the file cannot hold more than %u lines.\n",
                      (unsigned)MICRO_LINE_MAXIMUM);

        return false;
    }

    copy = MicroDuplicate(text, strlen(text));

    if (copy == NULL)
    {
        (void)fprintf(stderr, "micro: out of memory.\n");

        return false;
    }

    memmove(&MicroLines[position + 1U], &MicroLines[position],
            (MicroLineCount - position) * sizeof MicroLines[0]);
    MicroLines[position] = copy;
    ++MicroLineCount;
    MicroModified = true;

    return true;
}

static void MicroDeleteLine(size_t position)
{
    free(MicroLines[position]);
    memmove(&MicroLines[position], &MicroLines[position + 1U],
            (MicroLineCount - position - 1U) * sizeof MicroLines[0]);
    --MicroLineCount;
    MicroModified = true;
}

/* ---------------------------------------------------------- load and write */

/* Reads the file into the lines, splitting at each newline; a last line
 * without one is a line all the same. A file that is not there is an empty
 * buffer and not a failure — `micro new.txt` is how a file is begun. */
static bool MicroLoad(void)
{
    static char buffer[MICRO_READ_BYTES];
    static char line[LINE_CAPACITY];
    size_t length = 0U;
    bool cut = false;
    const int64_t descriptor = OxysOpen(MicroPath, SYSCALL_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        if (errno == ENOENT)
        {
            (void)printf("micro: %s is a new file.\n", MicroPath);

            return true;
        }

        (void)fprintf(stderr, "micro: %s: %s\n", MicroPath, strerror(errno));

        return false;
    }

    for (;;)
    {
        const int64_t read = OxysRead((int)descriptor, buffer, sizeof buffer);

        if (read < 0)
        {
            (void)fprintf(stderr, "micro: %s: %s\n", MicroPath, strerror(errno));
            (void)OxysClose((int)descriptor);

            return false;
        }

        if (read == 0)
        {
            break;
        }

        for (int64_t index = 0; index < read; ++index)
        {
            const char byte = buffer[index];

            if (byte == '\n')
            {
                line[length] = '\0';

                if (!MicroInsertLine(MicroLineCount, line))
                {
                    (void)OxysClose((int)descriptor);

                    return false;
                }

                length = 0U;
                continue;
            }

            if (length + 1U < sizeof line)
            {
                line[length] = byte;
                ++length;
            }
            else
            {
                cut = true;
            }
        }
    }

    (void)OxysClose((int)descriptor);

    if (length > 0U)
    {
        line[length] = '\0';

        if (!MicroInsertLine(MicroLineCount, line))
        {
            return false;
        }
    }

    if (cut)
    {
        (void)fprintf(stderr, "micro: a line longer than %u bytes was cut to fit.\n",
                      (unsigned)(LINE_CAPACITY - 1U));
    }

    /* Loading is not a change. */
    MicroModified = false;

    return true;
}

/*
 * Writes every line, each followed by a newline — into a file beside the one
 * edited, and only once the whole of it is written does that file take the
 * edited one's name. Since 2026-09-24.
 *
 * Written in place, as it was until then, the file was truncated at the open
 * and rewritten line by line, so a write that failed part way left it cut at
 * that line: a write of nothing, for a blank line, was once refused by the
 * kernel, and saving `/etc/session.conf` left it cut at its first blank line
 * with every entry of the launcher gone. Written beside, a failure leaves the
 * edited file exactly as it was and the partial one is removed.
 *
 * There is no rename, so the name is moved as `mv` moves one: the edited file
 * is unlinked and the written one linked in its place. Between the two the
 * name is missing for the length of a call; should the link fail there, the
 * edit is kept under the file beside and `micro` says where.
 */
static bool MicroWrite(void)
{
    char beside[SYSCALL_PATH_MAXIMUM + 1U];
    const int composed = snprintf(beside, sizeof beside, "%s%s", MicroPath, MICRO_SAVE_SUFFIX);
    int64_t descriptor;
    uint64_t bytes = 0U;

    if ((composed < 0) || ((size_t)composed >= sizeof beside))
    {
        (void)fprintf(stderr, "micro: %s: the path is too long to save beside; nothing was "
                              "changed.\n", MicroPath);

        return false;
    }

    descriptor = OxysOpen(beside, SYSCALL_OPEN_WRITE | SYSCALL_OPEN_CREATE | SYSCALL_OPEN_TRUNCATE,
                          0644U);

    if (descriptor < 0)
    {
        (void)fprintf(stderr, "micro: %s: %s; nothing was changed.\n", beside, strerror(errno));

        return false;
    }

    for (size_t index = 0U; index < MicroLineCount; ++index)
    {
        const size_t length = strlen(MicroLines[index]);

        if ((OxysWrite((int)descriptor, MicroLines[index], length) != (int64_t)length) ||
            (OxysWrite((int)descriptor, "\n", 1U) != 1))
        {
            (void)fprintf(stderr, "micro: %s: %s; %s was not changed.\n", beside,
                          strerror(errno), MicroPath);
            (void)OxysClose((int)descriptor);
            (void)OxysUnlink(beside);

            return false;
        }

        bytes += length + 1U;
    }

    if (OxysClose((int)descriptor) < 0)
    {
        (void)fprintf(stderr, "micro: %s: %s; %s was not changed.\n", beside, strerror(errno),
                      MicroPath);
        (void)OxysUnlink(beside);

        return false;
    }

    /* A file being made for the first time has no name to remove. */
    if ((OxysUnlink(MicroPath) < 0) && (errno != ENOENT))
    {
        (void)fprintf(stderr, "micro: %s: %s; it was not changed.\n", MicroPath, strerror(errno));
        (void)OxysUnlink(beside);

        return false;
    }

    if (OxysLink(beside, MicroPath) < 0)
    {
        (void)fprintf(stderr, "micro: %s: %s; the edit is kept in %s.\n", MicroPath,
                      strerror(errno), beside);

        return false;
    }

    (void)OxysUnlink(beside);
    (void)printf("micro: %s written, %u line(s), %llu byte(s).\n", MicroPath,
                 (unsigned)MicroLineCount, (unsigned long long)bytes);
    MicroModified = false;

    return true;
}

/* ------------------------------------------------------------- the commands */

static void MicroPrint(void)
{
    if (MicroLineCount == 0U)
    {
        (void)printf("micro: the file is empty.\n");

        return;
    }

    for (size_t index = 0U; index < MicroLineCount; ++index)
    {
        (void)printf("%4u  %s\n", (unsigned)(index + 1U), MicroLines[index]);
    }
}

/* A line number as typed, 1 to the count — or, where `allow_end`, one more,
 * for an insertion after the last line. Returns false having said why. */
static bool MicroParseNumber(const char *text, bool allow_end, size_t *number)
{
    size_t value = 0U;

    if ((text == NULL) || (*text == '\0'))
    {
        (void)fprintf(stderr, "micro: a line number is needed.\n");

        return false;
    }

    for (; *text != '\0'; ++text)
    {
        if ((*text < '0') || (*text > '9') || (value > MICRO_LINE_MAXIMUM))
        {
            (void)fprintf(stderr, "micro: not a line number.\n");

            return false;
        }

        value = (value * 10U) + (size_t)(*text - '0');
    }

    if ((value == 0U) || (value > MicroLineCount + (allow_end ? 1U : 0U)))
    {
        (void)fprintf(stderr, "micro: the file has %u line(s).\n", (unsigned)MicroLineCount);

        return false;
    }

    *number = value;

    return true;
}

/* Reads lines and places each before `position`, advancing, until a line
 * holding only `.` or the end of input. */
static void MicroReadLines(size_t position)
{
    (void)printf("micro: type lines; a line holding only . ends them.\n");

    for (;;)
    {
        char prompt[16];
        const char *line;

        (void)snprintf(prompt, sizeof prompt, "%4u  ", (unsigned)(position + 1U));
        line = LineRead(&MicroEditor, prompt);

        if ((line == NULL) || (strcmp(line, MICRO_END) == 0))
        {
            return;
        }

        if (!MicroInsertLine(position, line))
        {
            return;
        }

        ++position;
    }
}

static void MicroHelp(void)
{
    (void)printf("p, prints the file with line numbers.\n"
                 "a, appends lines typed until a line holding only . .\n"
                 "i N, inserts lines typed before line N, until a line holding only . .\n"
                 "e N, edits line N: the line is shown for editing, Return keeps it.\n"
                 "d N, deletes line N.\n"
                 "w, writes the file.\n"
                 "q, quits; q! quits discarding changes; wq writes and quits.\n"
                 "h, prints this list.\n");
}

/* Runs one command. Returns false when the editor should end. */
static bool MicroCommand(char *text)
{
    const char *argument;
    size_t number;

    while (*text == ' ')
    {
        ++text;
    }

    if (*text == '\0')
    {
        return true;
    }

    argument = &text[1];

    while (*argument == ' ')
    {
        ++argument;
    }

    if ((strcmp(text, "q") == 0) || (strcmp(text, "q!") == 0))
    {
        if (MicroModified && (text[1] != '!'))
        {
            (void)fprintf(stderr, "micro: unsaved changes; w to write, q! to discard.\n");

            return true;
        }

        return false;
    }

    if (strcmp(text, "w") == 0)
    {
        (void)MicroWrite();

        return true;
    }

    if (strcmp(text, "wq") == 0)
    {
        return !MicroWrite();
    }

    if (strcmp(text, "p") == 0)
    {
        MicroPrint();

        return true;
    }

    if ((strcmp(text, "h") == 0) || (strcmp(text, "?") == 0))
    {
        MicroHelp();

        return true;
    }

    if (strcmp(text, "a") == 0)
    {
        MicroReadLines(MicroLineCount);

        return true;
    }

    if ((text[0] == 'i') && ((text[1] == ' ') || (text[1] == '\0')))
    {
        if (MicroParseNumber(argument, true, &number))
        {
            MicroReadLines(number - 1U);
        }

        return true;
    }

    if ((text[0] == 'd') && ((text[1] == ' ') || (text[1] == '\0')))
    {
        if (MicroParseNumber(argument, false, &number))
        {
            MicroDeleteLine(number - 1U);
        }

        return true;
    }

    if ((text[0] == 'e') && ((text[1] == ' ') || (text[1] == '\0')))
    {
        if (MicroParseNumber(argument, false, &number))
        {
            char prompt[16];
            const char *edited;

            (void)snprintf(prompt, sizeof prompt, "%4u  ", (unsigned)number);
            edited = LineEdit(&MicroEditor, prompt, MicroLines[number - 1U]);

            /* Control-D upon the line leaves it as it was: an edit abandoned
             * is not an edit made. */
            if ((edited != NULL) && (strcmp(edited, MicroLines[number - 1U]) != 0))
            {
                char *const copy = MicroDuplicate(edited, strlen(edited));

                if (copy == NULL)
                {
                    (void)fprintf(stderr, "micro: out of memory.\n");

                    return true;
                }

                free(MicroLines[number - 1U]);
                MicroLines[number - 1U] = copy;
                MicroModified = true;
            }
        }

        return true;
    }

    (void)fprintf(stderr, "micro: %s: no such command; h lists them.\n", text);

    return true;
}

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        (void)fprintf(stderr, "micro: one operand, the file to edit, is required.\n");

        return EXIT_FAILURE;
    }

    MicroPath = argv[1];
    LineInitialise(&MicroEditor, NULL, NULL);

    if (!MicroLoad())
    {
        return EXIT_FAILURE;
    }

    if (MicroLineCount > 0U)
    {
        MicroPrint();
    }

    for (;;)
    {
        char *line;
        char command[LINE_CAPACITY];

        (void)fflush(stdout);
        line = LineRead(&MicroEditor, MICRO_PROMPT);

        if (line == NULL)
        {
            /* Control-D at the prompt is `q`: refused the same way while
             * there are unsaved changes, so that a reflex does not lose them. */
            if (MicroModified)
            {
                (void)fprintf(stderr, "micro: unsaved changes; w to write, q! to discard.\n");
                continue;
            }

            break;
        }

        LineRemember(&MicroEditor);

        /* Copied, because the command's own reading of lines reuses the
         * editor's buffer the text stands in. */
        strcpy(command, line);

        if (!MicroCommand(command))
        {
            break;
        }
    }

    return (fflush(stdout) == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
