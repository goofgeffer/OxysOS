/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/shell/parser.c
 * Purpose: Asserts the shell's tokeniser and parser of sub-task 8.2 — the
 *          code the shell ships, compiled into this image — against lines
 *          whose tokens and structure are known; and then runs the shell
 *          itself at privilege level 3 upon a session placed upon the
 *          terminal, so that the same code is seen to parse, continue and
 *          refuse where a program reads it — and, session by session since,
 *          to run built-ins, programs, redirections and pipelines.
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
#include <oxys/fs/pipe.h>
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
 * Sub-task 8.3: assignments, variables and expansion.
 * ------------------------------------------------------------------------- */

/* The table the expansion is asserted against, which is not the shell's own:
 * `?` is answered here as the shell answers it, and the rest by name. */
static const char *VerifyShellLookup(void *context, const char *name)
{
    (void)context;

    if (strcmp(name, "HOME") == 0) { return "/bin"; }
    if (strcmp(name, "EMPTY") == 0) { return ""; }
    if (strcmp(name, "Q") == 0) { return "it's \"quoted\""; }
    if (strcmp(name, "?") == 0) { return "3"; }

    return NULL;
}

static bool VerifyShellExpandsTo(const char *word, const char *expected)
{
    char out[LINE_CAPACITY];

    return ShellExpandWord(word, out, sizeof out, VerifyShellLookup, NULL) &&
           (strcmp(out, expected) == 0);
}

static void VerifyShellAssignments(void)
{
    size_t offending = 0U;

    /* Assignment words before the name are assignments; after it, words. */
    VerifyShellRequire(VerifyShellParseLine("A=1 B='two words' cmd C=3", &offending) == SHELL_PARSE_OK,
                       "a command with assignments did not parse");
    {
        const ShellCommand *const command = &VerifyShellList.pipeline[0].command[0];

        VerifyShellRequire((command->assignment_count == 2U) &&
                               (strcmp(command->assignment[0], "A=1") == 0) &&
                               (strcmp(command->assignment[1], "B='two words'") == 0),
                           "the two assignment words before the name were not recorded whole");
        VerifyShellRequire((command->word_count == 2U) && VerifyShellWordIs(command->word[1], "C=3"),
                           "an assignment-shaped word after the name was not a word");
    }

    /* A line of assignments alone is a command with no name. */
    VerifyShellRequire((VerifyShellParseLine("X=5 Y=", &offending) == SHELL_PARSE_OK) &&
                           (VerifyShellList.pipeline[0].command[0].word_count == 0U) &&
                           (VerifyShellList.pipeline[0].command[0].assignment_count == 2U),
                       "assignments alone did not parse as a command without a name");

    /* What is and is not a name: Section 3.235. */
    VerifyShellRequire(ShellIsAssignmentWord("_a1=x") == 3U, "_a1= is not an assignment");
    VerifyShellRequire(ShellIsAssignmentWord("1a=x") == 0U, "1a= was taken for an assignment");
    VerifyShellRequire(ShellIsAssignmentWord("a-b=x") == 0U, "a-b= was taken for an assignment");
    VerifyShellRequire(ShellIsAssignmentWord("=x") == 0U, "=x was taken for an assignment");
    VerifyShellRequire(ShellIsAssignmentWord("abc") == 0U, "a word with no = was an assignment");

    /* Nine assignments are too many, by name. */
    VerifyShellRequire(VerifyShellParseLine("a=1 b=1 c=1 d=1 e=1 f=1 g=1 h=1 i=1 x", &offending) ==
                           SHELL_PARSE_TOO_MANY_ASSIGNMENTS,
                       "nine assignments were not refused as too many");
}

static void VerifyShellExpansion(void)
{
    /* Parameter expansion, Section 2.6.2, in every position the quoting
     * permits and none it forbids. */
    VerifyShellRequire(VerifyShellExpandsTo("$HOME", "/bin"), "$HOME did not expand");
    VerifyShellRequire(VerifyShellExpandsTo("${HOME}x", "/binx"), "${HOME} did not expand");
    VerifyShellRequire(VerifyShellExpandsTo("$HOMEx", ""), "$HOMEx did not take the longest name");
    VerifyShellRequire(VerifyShellExpandsTo("\"$HOME\"", "/bin"),
                       "$HOME within double quotes did not expand");
    VerifyShellRequire(VerifyShellExpandsTo("'$HOME'", "$HOME"),
                       "$HOME within single quotes expanded");
    VerifyShellRequire(VerifyShellExpandsTo("\\$HOME", "$HOME"), "an escaped $ expanded");
    VerifyShellRequire(VerifyShellExpandsTo("\"\\$HOME\"", "$HOME"),
                       "an escaped $ within double quotes expanded");
    VerifyShellRequire(VerifyShellExpandsTo("$?", "3"), "$? did not expand");
    VerifyShellRequire(VerifyShellExpandsTo("a$UNSET-b", "a-b"), "an unset variable was not empty");
    VerifyShellRequire(VerifyShellExpandsTo("$EMPTY", ""), "an empty variable was not empty");
    VerifyShellRequire(VerifyShellExpandsTo("$", "$"), "a lone $ was not literal");
    VerifyShellRequire(VerifyShellExpandsTo("$1", "$1"),
                       "a $ before a digit was not literal, there being no positional parameters");
    VerifyShellRequire(VerifyShellExpandsTo("${HOME", "${HOME"), "an unclosed ${ was not literal");

    /* The characters a value supplies are never quoting characters: a value
     * holding quotes comes through as it is. Section 2.6.7. */
    VerifyShellRequire(VerifyShellExpandsTo("$Q", "it's \"quoted\""),
                       "quotes within a value were treated as quoting");
    VerifyShellRequire(VerifyShellExpandsTo("\"$Q\"", "it's \"quoted\""),
                       "quotes within a value expanded inside double quotes were treated as quoting");

    /* And quote removal still holds around an expansion. */
    VerifyShellRequire(VerifyShellExpandsTo("a'b'\"c$HOME\"d", "abc/bind"),
                       "quote removal and expansion did not compose");

    /* The variable table: set, get, export, and the two refusals. */
    ShellVariablesInitialise();
    VerifyShellRequire(ShellVariableSet("PATH", "/bin") && (strcmp(ShellVariableGet("PATH"), "/bin") == 0),
                       "a variable set could not be read back");
    VerifyShellRequire(ShellVariableSet("PATH", "/usr") && (strcmp(ShellVariableGet("PATH"), "/usr") == 0),
                       "a variable set again did not take the new value");
    VerifyShellRequire(ShellVariableGet("NOPE") == NULL, "an unset variable was not null");
    VerifyShellRequire(!ShellVariableIsExported("PATH") && ShellVariableExport("PATH") &&
                           ShellVariableIsExported("PATH"),
                       "a variable was not marked exported by export");
    VerifyShellRequire(ShellVariableExport("NEW") && (strcmp(ShellVariableGet("NEW"), "") == 0) &&
                           ShellVariableIsExported("NEW"),
                       "exporting an unset name did not create an empty exported variable");
    VerifyShellRequire(!ShellVariableSet("1bad", "x"), "a name beginning with a digit was set");
    VerifyShellRequire(ShellVariableCount() == 2U, "the table does not hold the two variables set");

    {
        char big[SHELL_VALUE_MAXIMUM + 2U];

        memset(big, 'v', sizeof big - 1U);
        big[sizeof big - 1U] = '\0';
        VerifyShellRequire(!ShellVariableSet("BIG", big), "a value beyond the bound was set");
    }

    ShellVariablesInitialise();
}

/* ---------------------------------------------------------------------------
 * The shell itself, upon a session.
 * ------------------------------------------------------------------------- */

/*
 * What the shell is given. A command, a command continued across two lines by
 * an open quote and one by a trailing pipe, a syntax error, a refusal, a
 * comment, and the end. Since 8.6 the pipelines run rather than being refused,
 * so the first names no file: a session run at every boot must leave nothing
 * upon the root. The shell's answers are read by a person in the log;
 * what is asserted is that it consumed the whole session, ended at the end of
 * its input with status zero, and left nothing open.
 */
static const char VerifyShellSession[] =
    "echo hello world 2>&1 | wc -l ;\n"
    "echo \"two\n"
    "lines\"\n"
    "ls |\n"
    "wc\n"
    "ls ; ; wc\n"
    "if true\n"
    "# a comment\n"
    "\x04";

/*
 * A second session, of sub-task 8.3, whose evidence is the status the shell
 * ends with: `exit $F$G$H$Y` is 137 only if the assignment, the export, the
 * expansion, `cd` succeeding into a directory, `cd` failing into nothing and
 * into a file, and `||` and `&&` acting upon those statuses all worked. `pwd`
 * and `export` print, which a person reads; the status is what is asserted.
 */
static const char VerifyShellBuiltinSession[] =
    "X=5\n"
    "export Y=7\n"
    "cd /bin && H=3\n"
    "cd /nope || F=1\n"
    "cd /bin/echo && G=2\n"
    "cd .. ; pwd\n"
    "export\n"
    "exit $F$G$H$Y\n";

#define VERIFY_SHELL_BUILTIN_STATUS 137

/*
 * A third session, of sub-task 8.4, whose evidence is again the status: the
 * shell runs programs — `env-check`, written to /verify by this test and
 * asserting what it was given; `cat` failing with 1; a name not found with
 * 127; `echo` found upon the default PATH — and `exit $A$C$N$E` is 111272,
 * which is 168 in the eight bits a status has, only if each of them behaved.
 * The number is checked for what a wrong combination would produce, and none
 * of the near ones coincides.
 */
static const char VerifyShellProgramSession[] =
    "export MARK=abcd\n"
    "HIDDEN=1\n"
    "/verify/env-check alpha \"b c\" && A=1\n"
    "/bin/cat /nope; C=$?\n"
    "nothing; N=$?\n"
    "echo hi && E=2\n"
    "exit $A$C$N$E\n";

#define VERIFY_SHELL_PROGRAM_STATUS (111272 & 0xFF)

/* Where env-check is placed for the session, and removed after it. */
#define VERIFY_SHELL_DIRECTORY "/verify"
#define VERIFY_SHELL_PROGRAM_PATH "/verify/env-check"

extern const uint8_t KernelProgramEnvCheckBegin[];
extern const uint8_t KernelProgramEnvCheckEnd[];

/*
 * A fourth session, of sub-task 8.5, whose evidence is what the files hold
 * afterwards: every redirection operator used from the prompt, and the three
 * utilities that write. The kernel reads each file back below and compares
 * it; `exit $D` carries the one thing a file cannot show, that `rmdir`
 * removed a directory `mkdir` made.
 */
static const char VerifyShellRedirectSession[] =
    "echo written >/verify/out\n"
    "echo more >>/verify/out\n"
    "cat </verify/out >/verify/copy\n"
    "cat /verify/nonexistent 2>/verify/err\n"
    "cat /verify/nonexistent >/verify/both 2>&1\n"
    "echo clobbered >|/verify/clob\n"
    "touch /verify/t && cp /verify/out /verify/c2\n"
    "mkdir /verify/d && rmdir /verify/d && D=4\n"
    "exit $D\n";

#define VERIFY_SHELL_REDIRECT_STATUS 4

/* What the session should have left. `cat`'s diagnostic is its own text. */
typedef struct VerifyShellFile
{
    const char *path;
    const char *contents;
} VerifyShellFile;

static const VerifyShellFile VerifyShellRedirectFiles[] = {
    { "/verify/out", "written\nmore\n" },
    { "/verify/copy", "written\nmore\n" },
    { "/verify/clob", "clobbered\n" },
    { "/verify/err", "cat: /verify/nonexistent: No such file, or one that will not load\n" },
    { "/verify/both", "cat: /verify/nonexistent: No such file, or one that will not load\n" },
    { "/verify/t", "" },
    { "/verify/c2", "written\nmore\n" },
};

#define VERIFY_SHELL_REDIRECT_FILE_COUNT \
    (sizeof VerifyShellRedirectFiles / sizeof VerifyShellRedirectFiles[0])

/* Reads a file whole and compares it, length first; then removes it. */
static void VerifyShellFileHolds(const VerifyShellFile *expected)
{
    char buffer[256];
    uint64_t read = 0U;
    const size_t length = strlen(expected->contents);
    const int descriptor = VfsOpen(expected->path, VFS_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        KernelWriteString("  ");
        KernelWriteString(expected->path);
        KernelWriteString(" was not made by the redirection FAILED.\n");
        VerifyShellSucceeded = false;

        return;
    }

    if (!VfsRead(descriptor, buffer, sizeof buffer, &read) || (read != length) ||
        (memcmp(buffer, expected->contents, length) != 0))
    {
        KernelWriteString("  ");
        KernelWriteString(expected->path);
        KernelWriteString(" does not hold what the redirection should have written FAILED.\n");
        VerifyShellSucceeded = false;
    }

    (void)VfsClose(descriptor);
    (void)VfsUnlink(expected->path);
}
/* Defined below, with the run procedure. */
static void VerifyShellProgram(const char *session, size_t length, int64_t expected,
                               const char *what);


static void VerifyShellRedirections(void)
{
    VerifyShellProgram(VerifyShellRedirectSession, sizeof VerifyShellRedirectSession - 1U,
                       VERIFY_SHELL_REDIRECT_STATUS, "upon the redirections' session");

    for (size_t index = 0U; index < VERIFY_SHELL_REDIRECT_FILE_COUNT; ++index)
    {
        VerifyShellFileHolds(&VerifyShellRedirectFiles[index]);
    }

    /* And the directory rmdir removed is gone: a status of 4 said so, and
     * the layer is asked as well. */
    {
        VfsAttributes attributes;

        VerifyShellRequire(!VfsStat("/verify/d", &attributes),
                           "the directory rmdir removed is still there");
    }
}


/*
 * A fifth session, of sub-task 8.6, whose evidence is what the files hold and
 * the status: pipelines of two and three commands, a built-in run in a
 * pipeline's child, a diagnostic sent down a pipe by `2>&1`, the whole of
 * `/bin/sh` carried through a pipe a page at a time — many times the pipe's
 * buffer, so that the writer and the reader must take turns — and the
 * statuses Section 2.9.2 assigns: the last command's, and `!` inverting it.
 * `exit $T$F$N` is 010, which is 10, only if `false | true` was 0, `true |
 * false` was 1, and `! true | false` was 0.
 */
static const char VerifyShellPipelineSession[] =
    "echo one two three | wc -w >/verify/p1\n"
    "echo alpha >/verify/pa\n"
    "cat /verify/pa | cat | cat >/verify/p2\n"
    "help | wc -l >/verify/p3\n"
    "cat /verify/nonexistent 2>&1 | wc -l >/verify/p4\n"
    "cat /bin/sh | wc -c >/verify/p5\n"
    "false | true; T=$?\n"
    "true | false; F=$?\n"
    "! true | false; N=$?\n"
    "exit $T$F$N\n";

#define VERIFY_SHELL_PIPELINE_STATUS 10

/* How many lines `help` prints: one per command, built-ins and programs. */
#define VERIFY_SHELL_HELP_LINES "23"

static const VerifyShellFile VerifyShellPipelineFiles[] = {
    { "/verify/p1", "3\n" },
    { "/verify/pa", "alpha\n" },
    { "/verify/p2", "alpha\n" },
    { "/verify/p3", VERIFY_SHELL_HELP_LINES "\n" },
    { "/verify/p4", "1\n" },
};

#define VERIFY_SHELL_PIPELINE_FILE_COUNT \
    (sizeof VerifyShellPipelineFiles / sizeof VerifyShellPipelineFiles[0])

/* `/verify/p5` holds the size of `/bin/sh` in decimal, which the layer is
 * asked for rather than written here: the shell upon the ramdisk is a build
 * product and its size is nobody's to remember. */
static void VerifyShellPipedSize(void)
{
    VfsAttributes attributes;
    char expected[24];
    size_t length = 0U;
    uint64_t size;
    VerifyShellFile file;

    if (!VfsStat("/bin/sh", &attributes))
    {
        VerifyShellRequire(false, "the size of /bin/sh could not be asked for");

        return;
    }

    size = attributes.size;

    /* Decimal, most significant digit first, by writing the digits backwards
     * and reversing them. */
    do
    {
        expected[length] = (char)('0' + (int)(size % 10U));
        ++length;
        size /= 10U;
    } while (size > 0U);

    for (size_t index = 0U; index < length / 2U; ++index)
    {
        const char swap = expected[index];

        expected[index] = expected[length - 1U - index];
        expected[length - 1U - index] = swap;
    }

    expected[length] = '\n';
    expected[length + 1U] = '\0';

    file.path = "/verify/p5";
    file.contents = expected;
    VerifyShellFileHolds(&file);
}

static void VerifyShellPipelines(void)
{
    const size_t pipes_before = VfsPipeCount();

    VerifyShellProgram(VerifyShellPipelineSession, sizeof VerifyShellPipelineSession - 1U,
                       VERIFY_SHELL_PIPELINE_STATUS, "upon the pipelines' session");

    for (size_t index = 0U; index < VERIFY_SHELL_PIPELINE_FILE_COUNT; ++index)
    {
        VerifyShellFileHolds(&VerifyShellPipelineFiles[index]);
    }

    VerifyShellPipedSize();

    VerifyShellRequire(VfsPipeCount() == pipes_before,
                       "the session left a pipe behind it");
    VerifyShellRequire(VfsPipeBytesCarried() > 0U, "no bytes were carried by any pipe");
}


/*
 * A sixth session, of sub-task 8.7, whose evidence is the status: job
 * control from the prompt, driven by the two bytes the terminal turns into
 * signals. `cat` reading the terminal is ended by a control-C, which is 130;
 * stopped by a control-Z, which is 148 and a job; put in the background by
 * `bg`, where its next read stops it with SIGTTIN; brought back by `fg`,
 * where it reads a line and is ended by a second control-C, 130 again. A `cat &` is
 * stopped at once by SIGTTIN and ended by `kill %1`, which continues it so
 * that the signal is acted upon; a pipeline run in the background completes
 * while a foreground pipeline runs, and is collected at the prompt before the
 * exit — a shell that exits with a background job still running leaves it an
 * orphan, which is Phase 9's `init` to collect. `exit $S$T$F` is 130148130, which is
 * 34 in the eight bits a status has, only if all three statuses were what
 * they should be.
 *
 * Each control byte stands behind a line only the running `cat` will read,
 * so that it becomes the head of the queue — and so a signal — only once
 * `cat` holds the terminal. A control-Z immediately after `cat\n` would be
 * the head while the shell was still forking, and the tick would deliver it
 * to the shell's own group, which ignores it; the line before it is what
 * makes the session say what it means whatever the timing. And `cat` is ended
 * by a control-C rather than a control-D, because a control-D is a byte `cat`
 * reads — with everything queued behind it in the same read, which it would
 * discard as what followed the end; the session is placed upon the terminal
 * whole, and a person types one line at a time.
 */
static const char VerifyShellJobSession[] =
    "cat\n"
    "abc\n"
    "\x03"
    "S=$?\n"
    "cat\n"
    "zzz\n"
    "\x1a"
    "T=$?\n"
    "jobs\n"
    "bg %1\n"
    "fg %1\n"
    "xyz\n"
    "\x03"
    "F=$?\n"
    "cat &\n"
    "kill %1\n"
    "echo one | wc -l &\n"
    "cat /bin/sh | wc -c >/verify/j\n"
    "exit $S$T$F\n";

#define VERIFY_SHELL_JOB_STATUS (130148130 & 0xFF)

/* The two control bytes the terminal removes and turns into signals. */
#define VERIFY_SHELL_JOB_INTERCEPTED 3U

static void VerifyShellJobs(void)
{
    const uint64_t intercepted_before = TerminalBytesIntercepted();

    VerifyShellProgram(VerifyShellJobSession, sizeof VerifyShellJobSession - 1U,
                       VERIFY_SHELL_JOB_STATUS, "upon the job-control session");

    VerifyShellRequire(TerminalBytesIntercepted() - intercepted_before ==
                           VERIFY_SHELL_JOB_INTERCEPTED,
                       "the control-C and the control-Z were not both turned into signals");
    (void)VfsUnlink("/verify/j");
    VerifyShellRequire(TerminalForegroundGroup() == 0U,
                       "the terminal's foreground group outlived the shell");
}

/* Writes env-check onto the root. Returns false having said why. */
static bool VerifyShellPlaceProgram(void)
{
    const uint64_t length = (uint64_t)(KernelProgramEnvCheckEnd - KernelProgramEnvCheckBegin);
    uint64_t written = 0U;
    int descriptor;

    if (!VfsCreateDirectory(VERIFY_SHELL_DIRECTORY, 0755U))
    {
        VerifyShellRequire(false, "the /verify directory could not be made");

        return false;
    }

    descriptor = VfsOpen(VERIFY_SHELL_PROGRAM_PATH,
                         VFS_OPEN_WRITE | VFS_OPEN_CREATE | VFS_OPEN_TRUNCATE, 0755U);

    if (descriptor < 0)
    {
        VerifyShellRequire(false, "env-check could not be created upon the root");

        return false;
    }

    if (!VfsWrite(descriptor, KernelProgramEnvCheckBegin, length, &written) || (written != length))
    {
        (void)VfsClose(descriptor);
        VerifyShellRequire(false, "env-check could not be written whole");

        return false;
    }

    (void)VfsClose(descriptor);

    return true;
}

static void VerifyShellRemoveProgram(void)
{
    VerifyShellRequire(VfsUnlink(VERIFY_SHELL_PROGRAM_PATH), "env-check could not be removed");
    VerifyShellRequire(VfsRemoveDirectory(VERIFY_SHELL_DIRECTORY),
                       "the /verify directory could not be removed");
}

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

static void VerifyShellProgram(const char *session, size_t length, int64_t expected,
                               const char *what)
{
    int64_t status = 0;
    const uint64_t delivered_before = TerminalBytesDelivered();
    const uint64_t intercepted_before = TerminalBytesIntercepted();

    VerifyShellBoot = ThreadAdoptCurrent("boot");

    if (VerifyShellBoot == NULL)
    {
        VerifyShellRequire(false, "the kernel's own flow of control could not be adopted");

        return;
    }

    TerminalFlush();
    TerminalInject(session, length);

    if (VerifyShellRun(&status))
    {
        if (status != expected)
        {
            KernelWriteString("  the shell, ");
            KernelWriteString(what);
            KernelWriteString(", FAILED: the status was ");
            KernelWriteHexadecimal((uint64_t)status);
            KernelWriteString(" and not ");
            KernelWriteHexadecimal((uint64_t)expected);
            KernelWriteString(".\n");
            VerifyShellSucceeded = false;
        }

        /* Every byte of the session is accounted for: delivered to a
         * program, or — since 8.7 — removed as a control-C or control-Z and
         * turned into a signal. */
        VerifyShellRequire((TerminalBytesDelivered() - delivered_before) +
                                   (TerminalBytesIntercepted() - intercepted_before) ==
                               length,
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
    VerifyShellAssignments();
    VerifyShellExpansion();
    VerifyShellProgram(VerifyShellSession, sizeof VerifyShellSession - 1U, 0, "upon the parser's session");
    VerifyShellProgram(VerifyShellBuiltinSession, sizeof VerifyShellBuiltinSession - 1U,
                       VERIFY_SHELL_BUILTIN_STATUS, "upon the built-ins' session");

    if (VerifyShellPlaceProgram())
    {
        VerifyShellProgram(VerifyShellProgramSession, sizeof VerifyShellProgramSession - 1U,
                           VERIFY_SHELL_PROGRAM_STATUS, "upon the programs' session");
        VerifyShellRedirections();
        VerifyShellPipelines();
        VerifyShellJobs();
        VerifyShellRemoveProgram();
    }

    if (VerifyShellSucceeded)
    {
        KernelWriteString("Shell self-test passed: every operator, quote and io_number tokenised "
                          "as Section 2.3 requires, the grammar's subset parsed and its "
                          "remainder was refused by name, assignments and expansion behaved, and the shell read sessions of ");
        KernelWriteDecimal((uint64_t)(sizeof VerifyShellSession - 1U));
        KernelWriteString(", ");
        KernelWriteDecimal((uint64_t)(sizeof VerifyShellBuiltinSession - 1U));
        KernelWriteString(", ");
        KernelWriteDecimal((uint64_t)(sizeof VerifyShellProgramSession - 1U));
        KernelWriteString(", ");
        KernelWriteDecimal((uint64_t)(sizeof VerifyShellRedirectSession - 1U));
        KernelWriteString(", ");
        KernelWriteDecimal((uint64_t)(sizeof VerifyShellPipelineSession - 1U));
        KernelWriteString(" and ");
        KernelWriteDecimal((uint64_t)(sizeof VerifyShellJobSession - 1U));
        KernelWriteString(" bytes at privilege level 3, ending with zero, with the status its built-ins composed, with the status the programs it ran composed, with every redirection's file holding what was written, with every pipeline's file holding what came through the pipe, and with the statuses control-C, control-Z, bg, fg and kill composed.\n");
    }
    else
    {
        KernelWriteString("Shell self-test FAILED.\n");
    }
}
