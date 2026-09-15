/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/shell/parser.c
 * Purpose: Asserts the shell's tokeniser and parser of sub-task 8.2 — the
 *          code the shell ships, compiled into this image — against lines
 *          whose tokens and structure are known; and then runs the shell
 *          itself at privilege level 3 upon a session placed upon the
 *          terminal, so that the same code is seen to parse, continue and
 *          refuse where a program reads it.
 * Key functions: KernelVerifyShell.
 * References:
 *   - docs/design/SHELL.md, Section 9: the table pairing every property
 *     asserted below with the silent failure the assertion exists to catch.
 *   - IEEE Std 1003.1-2017, Sections 2.2, 2.3, 2.7 and 2.10: what a line is
 *     required to tokenise and parse into, which is what the expectations
 *     below were written from.
 *   - userland/sh/shell.h: the structure asserted.
 *   - kernel/test/libc/line.c: the run procedure this repeats for the shell.
 *
 * Why the shell's own translation units are compiled into the kernel.
 *
 *   For the reason the C library's are: a tokeniser and a parser are ordinary
 *   C that reach no system call, and asserting them here, against fifty lines
 *   with known answers, is worth more than running the shell and reading what
 *   it printed. The shell is then run too, because a grammar asserted only in
 *   the kernel says nothing about the program that uses it — the continuation
 *   prompt, the accumulation of a command across lines, and the diagnostic
 *   for a refusal all live in main.c, and main.c reaches the terminal.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <oxys/exec/elf.h>
#include <oxys/fs/vfs.h>
#include <oxys/proc/process.h>
#include <oxys/terminal/terminal.h>

#include <line.h>
#include <string.h>

#include "../../../userland/sh/shell.h"

extern const uint8_t KernelProgramShellBegin[];
extern const uint8_t KernelProgramShellEnd[];

static bool VerifyShellSucceeded;

static void VerifyShellRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyShellSucceeded = false;
    }
}

/* The two objects every line is parsed into; static, being some tens of
 * kibibytes between them. */
static ShellTokens VerifyShellTokens;
static ShellList VerifyShellList;

/* Tokenises and parses a line, returning the combined status. */
static ShellParseStatus VerifyShellParseLine(const char *line, size_t *offending)
{
    const ShellParseStatus status = ShellTokenise(line, &VerifyShellTokens);

    if (status != SHELL_PARSE_OK)
    {
        return status;
    }

    return ShellParse(&VerifyShellTokens, &VerifyShellList, offending);
}

/* Whether the tokens of the last line are exactly these kinds, in order,
 * the END token included. */
static bool VerifyShellTokensAre(const ShellTokenKind *kinds, size_t count)
{
    if (VerifyShellTokens.count != count)
    {
        return false;
    }

    for (size_t index = 0U; index < count; ++index)
    {
        if (VerifyShellTokens.token[index].kind != kinds[index])
        {
            return false;
        }
    }

    return true;
}

/* Whether a word, unquoted, is `expected`. */
static bool VerifyShellWordIs(const char *word, const char *expected)
{
    char unquoted[LINE_CAPACITY];

    return ShellUnquote(word, unquoted, sizeof unquoted) && (strcmp(unquoted, expected) == 0);
}

static void VerifyShellTokeniser(void)
{
    /* Words and blanks: rules 7, 8 and 10. Tabs are blanks too. */
    {
        static const ShellTokenKind kinds[] = { SHELL_TOKEN_WORD, SHELL_TOKEN_WORD,
                                                SHELL_TOKEN_WORD, SHELL_TOKEN_END };

        VerifyShellRequire(ShellTokenise("  echo\thello   world ", &VerifyShellTokens) == SHELL_PARSE_OK,
                           "three words separated by blanks did not tokenise");
        VerifyShellRequire(VerifyShellTokensAre(kinds, 4U),
                           "three words and the end were not four tokens of those kinds");
        VerifyShellRequire(strcmp(VerifyShellTokens.token[1].text, "hello") == 0,
                           "the second word is not hello");
        VerifyShellRequire(VerifyShellTokens.token[2].column == 15U,
                           "the third word's column is not where it began");
    }

    /* Every operator, longest first: rules 2, 3 and 6. `a>>b` is three tokens
     * with no blank, and `>>` is one token where `> >` is two. */
    {
        static const ShellTokenKind kinds[] = {
            SHELL_TOKEN_WORD, SHELL_TOKEN_DGREAT, SHELL_TOKEN_WORD, SHELL_TOKEN_GREAT,
            SHELL_TOKEN_GREAT, SHELL_TOKEN_AND_IF, SHELL_TOKEN_OR_IF, SHELL_TOKEN_PIPE,
            SHELL_TOKEN_AND, SHELL_TOKEN_SEMICOLON, SHELL_TOKEN_DSEMI, SHELL_TOKEN_LESS,
            SHELL_TOKEN_DLESS, SHELL_TOKEN_DLESSDASH, SHELL_TOKEN_LESSAND, SHELL_TOKEN_GREATAND,
            SHELL_TOKEN_LESSGREAT, SHELL_TOKEN_CLOBBER, SHELL_TOKEN_LEFT_PAREN,
            SHELL_TOKEN_RIGHT_PAREN, SHELL_TOKEN_END
        };

        VerifyShellRequire(ShellTokenise("a>>b > > && || | & ; ;; < << <<- <& >& <> >| ( )",
                                         &VerifyShellTokens) == SHELL_PARSE_OK,
                           "the operators did not tokenise");
        VerifyShellRequire(VerifyShellTokensAre(kinds, 21U),
                           "the operators were not recognised as the twenty kinds, longest first");
    }

    /* io_number: digits immediately before < or >, and not otherwise. */
    {
        static const ShellTokenKind kinds[] = { SHELL_TOKEN_WORD, SHELL_TOKEN_IO_NUMBER,
                                                SHELL_TOKEN_GREAT, SHELL_TOKEN_WORD,
                                                SHELL_TOKEN_WORD, SHELL_TOKEN_GREAT,
                                                SHELL_TOKEN_WORD, SHELL_TOKEN_END };

        VerifyShellRequire(ShellTokenise("x 2>err 2 >out", &VerifyShellTokens) == SHELL_PARSE_OK,
                           "a line with an io_number did not tokenise");
        VerifyShellRequire(VerifyShellTokensAre(kinds, 8U),
                           "2> was not an io_number, or 2 > was");
    }

    /* Quoting keeps a word together and the quotes are kept upon it. */
    {
        static const ShellTokenKind kinds[] = { SHELL_TOKEN_WORD, SHELL_TOKEN_WORD,
                                                SHELL_TOKEN_WORD, SHELL_TOKEN_WORD,
                                                SHELL_TOKEN_END };

        VerifyShellRequire(ShellTokenise("echo 'a b|c' \"d;e\" f\\ g", &VerifyShellTokens) ==
                               SHELL_PARSE_OK,
                           "quoted words did not tokenise");
        VerifyShellRequire(VerifyShellTokensAre(kinds, 5U),
                           "a quoted blank or operator character delimited a word");
        VerifyShellRequire(strcmp(VerifyShellTokens.token[1].text, "'a b|c'") == 0,
                           "the single quotes were not kept upon the word");
        VerifyShellRequire(strcmp(VerifyShellTokens.token[3].text, "f\\ g") == 0,
                           "the backslash was not kept upon the word");
    }

    /* A comment is no token; a # within a word is a character. */
    {
        static const ShellTokenKind kinds[] = { SHELL_TOKEN_WORD, SHELL_TOKEN_WORD, SHELL_TOKEN_END };

        VerifyShellRequire(ShellTokenise("ls a#b # not this", &VerifyShellTokens) == SHELL_PARSE_OK,
                           "a line with a comment did not tokenise");
        VerifyShellRequire(VerifyShellTokensAre(kinds, 3U) &&
                               (strcmp(VerifyShellTokens.token[1].text, "a#b") == 0),
                           "the comment produced a token, or a # within a word ended it");
    }

    /* An unterminated quote, and a trailing backslash, are incomplete. */
    VerifyShellRequire(ShellTokenise("echo \"open", &VerifyShellTokens) == SHELL_PARSE_INCOMPLETE,
                       "an unterminated double quote was not incomplete");
    VerifyShellRequire(ShellTokenise("echo 'open", &VerifyShellTokens) == SHELL_PARSE_INCOMPLETE,
                       "an unterminated single quote was not incomplete");
    VerifyShellRequire(ShellTokenise("echo a\\", &VerifyShellTokens) == SHELL_PARSE_INCOMPLETE,
                       "a trailing backslash was not incomplete");

    /* An empty line, and a line of blanks, are one END token. */
    VerifyShellRequire((ShellTokenise("", &VerifyShellTokens) == SHELL_PARSE_OK) &&
                           (VerifyShellTokens.count == 1U) &&
                           (VerifyShellTokens.token[0].kind == SHELL_TOKEN_END),
                       "an empty line is not one END token");
    VerifyShellRequire((ShellTokenise("   \t ", &VerifyShellTokens) == SHELL_PARSE_OK) &&
                           (VerifyShellTokens.count == 1U),
                       "a line of blanks is not one END token");

    /* The bound: more tokens than fit is refused and not overrun. */
    {
        static char many[SHELL_TOKEN_MAXIMUM * 2U + 2U];

        for (size_t index = 0U; index < sizeof many - 1U; ++index)
        {
            many[index] = ((index % 2U) == 0U) ? 'x' : ' ';
        }

        many[sizeof many - 1U] = '\0';
        VerifyShellRequire(ShellTokenise(many, &VerifyShellTokens) == SHELL_PARSE_TOO_MANY_TOKENS,
                           "a line of more tokens than the bound was not refused");
    }
}

static void VerifyShellUnquoting(void)
{
    char out[64];

    VerifyShellRequire(ShellUnquote("plain", out, sizeof out) && (strcmp(out, "plain") == 0),
                       "an unquoted word changed under quote removal");
    VerifyShellRequire(ShellUnquote("'a b|c'", out, sizeof out) && (strcmp(out, "a b|c") == 0),
                       "single quotes were not removed, or their contents were altered");
    VerifyShellRequire(ShellUnquote("'\\n'", out, sizeof out) && (strcmp(out, "\\n") == 0),
                       "a backslash within single quotes was not literal");
    VerifyShellRequire(ShellUnquote("\"a\\\"b\\$c\\d\"", out, sizeof out) &&
                           (strcmp(out, "a\"b$c\\d") == 0),
                       "within double quotes a backslash did not escape exactly the "
                       "characters 2.2.3 names");
    VerifyShellRequire(ShellUnquote("f\\ g\\\\", out, sizeof out) && (strcmp(out, "f g\\") == 0),
                       "an unquoted backslash did not preserve the character after it");
    VerifyShellRequire(ShellUnquote("a'b'\"c\"d", out, sizeof out) && (strcmp(out, "abcd") == 0),
                       "adjacent quoted and unquoted parts did not join into one word");
    VerifyShellRequire(ShellUnquote("''", out, sizeof out) && (out[0] == '\0'),
                       "an empty quoted string is not the empty word");
    VerifyShellRequire(ShellUnquote("\"a\\\nb\"", out, sizeof out) && (strcmp(out, "ab") == 0),
                       "an escaped newline within double quotes was not removed");
    VerifyShellRequire(!ShellUnquote("toolong", out, 4U),
                       "a word that does not fit was not refused");
}

static void VerifyShellParser(void)
{
    size_t offending = 0U;

    /* One simple command: words in order, the first the name. */
    VerifyShellRequire(VerifyShellParseLine("echo hello world", &offending) == SHELL_PARSE_OK,
                       "a simple command did not parse");
    VerifyShellRequire((VerifyShellList.pipeline_count == 1U) &&
                           (VerifyShellList.pipeline[0].command_count == 1U) &&
                           (VerifyShellList.pipeline[0].command[0].word_count == 3U) &&
                           VerifyShellWordIs(VerifyShellList.pipeline[0].command[0].word[0], "echo") &&
                           VerifyShellWordIs(VerifyShellList.pipeline[0].command[0].word[2], "world"),
                       "a simple command's words are not the three in order");
    VerifyShellRequire((VerifyShellList.pipeline[0].condition == SHELL_CONDITION_NONE) &&
                           (VerifyShellList.pipeline[0].separator == SHELL_SEPARATOR_NONE) &&
                           !VerifyShellList.pipeline[0].negated,
                       "a lone command has a condition, a separator or a negation");

    /* A pipeline of three, and a redirection on each end with its default
     * descriptor; the redirection between words does not become a word. */
    VerifyShellRequire(VerifyShellParseLine("cat <in | sort | uniq -c > out", &offending) ==
                           SHELL_PARSE_OK,
                       "a pipeline of three did not parse");
    VerifyShellRequire((VerifyShellList.pipeline_count == 1U) &&
                           (VerifyShellList.pipeline[0].command_count == 3U),
                       "a pipeline of three is not one pipeline of three commands");
    {
        const ShellCommand *const first = &VerifyShellList.pipeline[0].command[0];
        const ShellCommand *const last = &VerifyShellList.pipeline[0].command[2];

        VerifyShellRequire((first->word_count == 1U) && (first->redirection_count == 1U) &&
                               (first->redirection[0].kind == SHELL_REDIRECT_INPUT) &&
                               (first->redirection[0].descriptor == 0) &&
                               VerifyShellWordIs(first->redirection[0].target, "in"),
                           "< in was not an input redirection of descriptor 0 to in");
        VerifyShellRequire((last->word_count == 2U) && (last->redirection_count == 1U) &&
                               (last->redirection[0].kind == SHELL_REDIRECT_OUTPUT) &&
                               (last->redirection[0].descriptor == 1) &&
                               VerifyShellWordIs(last->redirection[0].target, "out"),
                           "> out was not an output redirection of descriptor 1 to out");
    }

    /* Every redirection operator, with and without an io_number, and the
     * words on either side of one kept in order. */
    VerifyShellRequire(VerifyShellParseLine("a 2>e >>l <&3 4>&1 <>rw >|c b", &offending) ==
                           SHELL_PARSE_OK,
                       "a command with every redirection did not parse");
    {
        const ShellCommand *const command = &VerifyShellList.pipeline[0].command[0];

        VerifyShellRequire((command->word_count == 2U) &&
                               VerifyShellWordIs(command->word[1], "b"),
                           "the word after the redirections was not the second word");
        VerifyShellRequire(command->redirection_count == 6U,
                           "six redirections were not six");
        VerifyShellRequire((command->redirection[0].kind == SHELL_REDIRECT_OUTPUT) &&
                               (command->redirection[0].descriptor == 2),
                           "2>e was not an output redirection of descriptor 2");
        VerifyShellRequire((command->redirection[1].kind == SHELL_REDIRECT_APPEND) &&
                               (command->redirection[1].descriptor == 1),
                           ">>l was not an append of descriptor 1");
        VerifyShellRequire((command->redirection[2].kind == SHELL_REDIRECT_DUPLICATE_IN) &&
                               (command->redirection[2].descriptor == 0) &&
                               VerifyShellWordIs(command->redirection[2].target, "3"),
                           "<&3 was not a duplication of input from 3");
        VerifyShellRequire((command->redirection[3].kind == SHELL_REDIRECT_DUPLICATE_OUT) &&
                               (command->redirection[3].descriptor == 4),
                           "4>&1 was not a duplication of output upon 4");
        VerifyShellRequire((command->redirection[4].kind == SHELL_REDIRECT_READ_WRITE) &&
                               (command->redirection[5].kind == SHELL_REDIRECT_CLOBBER),
                           "<> and >| were not read-write and clobber");
    }

    /* A list: separators, conditions, negation, and a trailing separator. */
    VerifyShellRequire(VerifyShellParseLine("a; b && ! c || d & e;", &offending) == SHELL_PARSE_OK,
                       "a list did not parse");
    VerifyShellRequire(VerifyShellList.pipeline_count == 5U, "the list is not five pipelines");
    VerifyShellRequire((VerifyShellList.pipeline[0].separator == SHELL_SEPARATOR_SEQUENCE) &&
                           (VerifyShellList.pipeline[1].condition == SHELL_CONDITION_NONE) &&
                           (VerifyShellList.pipeline[2].condition == SHELL_CONDITION_AND_IF) &&
                           VerifyShellList.pipeline[2].negated &&
                           (VerifyShellList.pipeline[3].condition == SHELL_CONDITION_OR_IF) &&
                           (VerifyShellList.pipeline[3].separator == SHELL_SEPARATOR_BACKGROUND) &&
                           (VerifyShellList.pipeline[4].condition == SHELL_CONDITION_NONE) &&
                           (VerifyShellList.pipeline[4].separator == SHELL_SEPARATOR_SEQUENCE),
                       "the separators, conditions and negation of the list are not as written");
    VerifyShellRequire(VerifyShellWordIs(VerifyShellList.pipeline[2].command[0].word[0], "c"),
                       "the ! was taken as a word of the command it negates");

    /* A blank line and a comment are EMPTY, not an error. */
    VerifyShellRequire(VerifyShellParseLine("", &offending) == SHELL_PARSE_EMPTY,
                       "an empty line was not EMPTY");
    VerifyShellRequire(VerifyShellParseLine("# just a comment", &offending) == SHELL_PARSE_EMPTY,
                       "a comment alone was not EMPTY");

    /* Incomplete: an operator with nothing after it wants the next line. */
    VerifyShellRequire(VerifyShellParseLine("ls |", &offending) == SHELL_PARSE_INCOMPLETE,
                       "a pipe with nothing after it was not incomplete");
    VerifyShellRequire(VerifyShellParseLine("a &&", &offending) == SHELL_PARSE_INCOMPLETE,
                       "&& with nothing after it was not incomplete");
    VerifyShellRequire(VerifyShellParseLine("a >", &offending) == SHELL_PARSE_INCOMPLETE,
                       "a redirection with no target was not incomplete");

    /* And a line completed across a newline parses as one. */
    VerifyShellRequire((VerifyShellParseLine("ls |\nwc", &offending) == SHELL_PARSE_OK) &&
                           (VerifyShellList.pipeline[0].command_count == 2U),
                       "a pipeline continued upon a second line did not parse as one");
    VerifyShellRequire((VerifyShellParseLine("echo 'a\nb'", &offending) == SHELL_PARSE_OK) &&
                           VerifyShellWordIs(VerifyShellList.pipeline[0].command[0].word[1], "a\nb"),
                       "a quote continued upon a second line did not keep the newline");

    /* Unexpected: an operator where a command was required, naming it. */
    VerifyShellRequire((VerifyShellParseLine("| ls", &offending) == SHELL_PARSE_UNEXPECTED_TOKEN) &&
                           (offending == 0U),
                       "a leading pipe was not an unexpected token at position 0");
    VerifyShellRequire((VerifyShellParseLine("ls ; ; wc", &offending) == SHELL_PARSE_UNEXPECTED_TOKEN) &&
                           (offending == 2U),
                       "a doubled separator was not an unexpected token at position 2");
    VerifyShellRequire((VerifyShellParseLine("ls > | wc", &offending) == SHELL_PARSE_UNEXPECTED_TOKEN) &&
                           (offending == 2U),
                       "a redirection whose target is an operator was not unexpected");

    /* Unsupported: refused by name, and only in command position. */
    VerifyShellRequire((VerifyShellParseLine("if true", &offending) == SHELL_PARSE_UNSUPPORTED) &&
                           (offending == 0U),
                       "a compound command was not refused as unsupported");
    VerifyShellRequire((VerifyShellParseLine("echo if", &offending) == SHELL_PARSE_OK) &&
                           VerifyShellWordIs(VerifyShellList.pipeline[0].command[0].word[1], "if"),
                       "a reserved word out of command position was not an ordinary word");
    VerifyShellRequire(VerifyShellParseLine("cat <<EOF", &offending) == SHELL_PARSE_UNSUPPORTED,
                       "a here-document was not refused as unsupported");
    VerifyShellRequire(VerifyShellParseLine("(ls)", &offending) == SHELL_PARSE_UNSUPPORTED,
                       "a subshell was not refused as unsupported");
    VerifyShellRequire(VerifyShellParseLine("x 1<<-y", &offending) == SHELL_PARSE_UNSUPPORTED,
                       "an io_number before a here-document was not refused as unsupported");

    /* The bounds, each by name. */
    VerifyShellRequire(VerifyShellParseLine("a b c d e f g h i j k l m n o p q", &offending) ==
                           SHELL_PARSE_TOO_MANY_WORDS,
                       "seventeen words were not refused as too many");
    VerifyShellRequire(VerifyShellParseLine("a >1 >2 >3 >4 >5 >6 >7 >8 >9", &offending) ==
                           SHELL_PARSE_TOO_MANY_REDIRECTIONS,
                       "nine redirections were not refused as too many");
    VerifyShellRequire(VerifyShellParseLine("a|b|c|d|e|f|g|h|i", &offending) ==
                           SHELL_PARSE_TOO_MANY_COMMANDS,
                       "nine commands in a pipeline were not refused as too many");
    VerifyShellRequire(VerifyShellParseLine("a;b;c;d;e;f;g;h;i;j;k;l;m;n;o;p;q", &offending) ==
                           SHELL_PARSE_TOO_MANY_PIPELINES,
                       "seventeen pipelines were not refused as too many");
    VerifyShellRequire(VerifyShellParseLine("a 1234>b", &offending) == SHELL_PARSE_BAD_DESCRIPTOR,
                       "a four-digit io_number was not refused");
}

/* ---------------------------------------------------------------------------
 * The shell itself, upon a session.
 * ------------------------------------------------------------------------- */

/*
 * What the shell is given. A command, a command continued across two lines by
 * an open quote and one by a trailing pipe, a syntax error, a refusal, a
 * comment, and the end. The shell's answers are read by a person in the log;
 * what is asserted is that it consumed the whole session, ended at the end of
 * its input with status zero, and left nothing open.
 */
static const char VerifyShellSession[] =
    "echo hello world >out 2>&1 | wc -l ;\n"
    "echo \"two\n"
    "lines\"\n"
    "ls |\n"
    "wc\n"
    "ls ; ; wc\n"
    "if true\n"
    "# a comment\n"
    "\x04";

static Thread *VerifyShellBoot;

static bool VerifyShellRun(int64_t *status)
{
    const uint64_t length = (uint64_t)(KernelProgramShellEnd - KernelProgramShellBegin);
    ProcessArguments arguments;
    Process *process;
    Thread *thread;
    ElfImage loaded;
    uint64_t stack;
    const size_t open_before = VfsOpenFileCount();

    arguments.argument_count = 1U;
    arguments.environment_count = 0U;
    arguments.argument[0] = 0U;
    arguments.storage_used = (uint32_t)(sizeof "sh");
    memcpy(arguments.storage, "sh", sizeof "sh");

    process = ProcessCreate("sh", NULL);

    if (process == NULL)
    {
        VerifyShellRequire(false, "a process could not be created for the shell");

        return false;
    }

    if (ElfLoad(&process->space, KernelProgramShellBegin, length, &loaded) != ELF_OK)
    {
        ProcessDestroy(process);
        VerifyShellRequire(false, "the shell did not load");

        return false;
    }

    ProcessRecordImage(process, &loaded);
    stack = ProcessCreateUserStack(process, &arguments);

    if (stack == 0U)
    {
        ProcessDestroy(process);
        VerifyShellRequire(false, "the shell was given no stack");

        return false;
    }

    thread = ThreadCreate(process, loaded.entry, stack);

    if ((thread == NULL) || !ThreadStart(thread))
    {
        ProcessDestroy(process);
        VerifyShellRequire(false, "the shell could not be started");

        return false;
    }

    VerifyShellRequire(ThreadCurrent() == VerifyShellBoot,
                       "the kernel did not resume the thread that started the shell");
    VerifyShellRequire(process->state == PROCESS_EXITED,
                       "the shell's process was not marked as ended");

    *status = process->exit_status;

    ProcessDestroy(process);

    VerifyShellRequire(VfsOpenFileCount() == open_before,
                       "the shell left an open file behind it");

    return true;
}

static void VerifyShellProgram(void)
{
    int64_t status = 0;
    const uint64_t delivered_before = TerminalBytesDelivered();

    VerifyShellBoot = ThreadAdoptCurrent("boot");

    if (VerifyShellBoot == NULL)
    {
        VerifyShellRequire(false, "the kernel's own flow of control could not be adopted");

        return;
    }

    TerminalFlush();
    TerminalInject(VerifyShellSession, sizeof VerifyShellSession - 1U);

    if (VerifyShellRun(&status))
    {
        if (status != 0)
        {
            KernelWriteString("  the shell FAILED: the status was ");
            KernelWriteHexadecimal((uint64_t)status);
            KernelWriteString(" and not zero.\n");
            VerifyShellSucceeded = false;
        }

        VerifyShellRequire(TerminalBytesDelivered() - delivered_before ==
                               sizeof VerifyShellSession - 1U,
                           "the shell did not consume exactly the session");
        VerifyShellRequire(TerminalBytesQueued() == 0U,
                           "the shell left bytes of the session unread");
    }

    ThreadDestroy(VerifyShellBoot);
    VerifyShellBoot = NULL;
    TerminalFlush();
}

void KernelVerifyShell(void)
{
    VerifyShellSucceeded = true;

    KernelWriteString("Shell: asserting the tokeniser and the parser, and running the shell "
                      "upon a session.\n");

    VerifyShellTokeniser();
    VerifyShellUnquoting();
    VerifyShellParser();
    VerifyShellProgram();

    if (VerifyShellSucceeded)
    {
        KernelWriteString("Shell self-test passed: every operator, quote and io_number tokenised "
                          "as Section 2.3 requires, the grammar's subset parsed and its "
                          "remainder was refused by name, and the shell read a session of ");
        KernelWriteDecimal((uint64_t)(sizeof VerifyShellSession - 1U));
        KernelWriteString(" bytes at privilege level 3 and ended with zero.\n");
    }
    else
    {
        KernelWriteString("Shell self-test FAILED.\n");
    }
}
