/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/shell.h
 * Purpose: Declares what the shell's translation units share — the token the
 *          tokeniser produces, the command structure the parser builds from
 *          tokens, the bounds upon both, and the two functions that make them.
 *          It is the interface the kernel's self-test asserts the shell's
 *          grammar through, so nothing here reaches a system call.
 * Key definitions: ShellTokenKind, ShellToken, ShellTokens, ShellRedirection,
 *          ShellRedirectionKind, ShellCommand, ShellPipeline, ShellList,
 *          ShellSeparator, ShellParseStatus, ShellTokenise, ShellParse,
 *          ShellUnquote, ShellParseStatusName, SHELL_TOKEN_MAXIMUM,
 *          SHELL_TEXT_MAXIMUM, SHELL_WORD_MAXIMUM, SHELL_REDIRECTION_MAXIMUM,
 *          SHELL_COMMAND_MAXIMUM, SHELL_PIPELINE_MAXIMUM — and, of sub-task
 *          8.3, ShellLookup, ShellExpandWord, ShellIsName,
 *          ShellIsAssignmentWord, ShellVariableSet, ShellVariableGet,
 *          ShellVariableExport, ShellVariableIsExported, ShellVariableCount,
 *          ShellVariableAt, ShellVariablesInitialise; of 8.4, ShellRunProgram
 *          and ShellBuildEnvironment; and of 8.6, ShellStageRunner,
 *          ShellExecuteProgram and ShellRunPipeline; and of 8.7, ShellJob and the
 *          job-control functions of jobs.c.
 * References:
 *   - IEEE Std 1003.1-2017, Section 2.2 (Quoting): the escape character, single
 *     quotes and double quotes, and which five characters a backslash escapes
 *     within double quotes.
 *   - IEEE Std 1003.1-2017, Section 2.3 (Token Recognition): the rules by which
 *     a line becomes operators and words, cited by number in lexer.c.
 *   - IEEE Std 1003.1-2017, Section 2.10.2 (Shell Grammar Rules) and the
 *     grammar of Section 2.10: `list`, `and_or`, `pipeline`, `simple_command`
 *     and `io_redirect`, of which parser.c implements the subset below.
 *   - IEEE Std 1003.1-2017, Section 2.7 (Redirection): the operators and what
 *     each means, so that the structure records the meaning and not the text.
 *   - docs/design/SHELL.md: what of the grammar is here, what is
 *     refused by name, and why the tokeniser keeps the quotes.
 *
 * Why the tokeniser keeps the quotes.
 *
 *   POSIX orders the word expansions before quote removal (Section 2.6), and
 *   the expansions need to know which characters were quoted: `"$HOME"` expands
 *   and `'$HOME'` does not. Sub-task 8.2 performs no expansion, so it could have
 *   removed the quotes as it tokenised and been simpler for it. It does not,
 *   because the expansions of 8.3 would then have needed the quotes back, and a
 *   tokeniser rewritten one sub-task after it was written is a tokeniser
 *   asserted twice. A token is the characters of the line, quotes and all, and
 *   ShellUnquote is the last step, applied by whoever wants the string.
 *
 * Why nothing here allocates.
 *
 *   The same reason the line editor does not: a shell that could not parse a
 *   line for want of memory would be a shell that could not report the
 *   failure. Every bound below is a number a person can be told, and a line
 *   that exceeds one is refused with a status that says which.
 */

#ifndef OXYS_SHELL_H
#define OXYS_SHELL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The bounds. A line is at most LINE_CAPACITY of <line.h>, and every token of
 * it is at most that long, so SHELL_TEXT_MAXIMUM holds the whole of a line's
 * tokens with a terminator each even if every character is its own token.
 */
#define SHELL_TOKEN_MAXIMUM       128U
#define SHELL_TEXT_MAXIMUM        1536U
#define SHELL_WORD_MAXIMUM        16U  /* Words of one command: SYSCALL_ARGUMENT_COUNT_MAXIMUM. */
#define SHELL_REDIRECTION_MAXIMUM 8U   /* Redirections of one command. */
#define SHELL_COMMAND_MAXIMUM     8U   /* Commands of one pipeline. */
#define SHELL_PIPELINE_MAXIMUM    16U  /* Pipelines of one list. */
#define SHELL_ASSIGNMENT_MAXIMUM  8U   /* Assignment words before a command's name. */
#define SHELL_NAME_MAXIMUM        64U  /* A variable's name. */
#define SHELL_VALUE_MAXIMUM       255U /* A variable's value. */
#define SHELL_VARIABLE_MAXIMUM    64U  /* Variables held at once. */

/*
 * What a token is. The operators are one kind each rather than one kind with
 * a text, so that the parser compares an enumeration and never a string — and
 * so that an operator this shell does not implement is a kind it can name in a
 * diagnostic rather than a word it mistakes for a command.
 */
typedef enum ShellTokenKind
{
    SHELL_TOKEN_END = 0,       /* The end of the line. */
    SHELL_TOKEN_WORD,          /* A word, quotes and all. */
    SHELL_TOKEN_IO_NUMBER,     /* Digits immediately before < or >. */
    SHELL_TOKEN_PIPE,          /* |  */
    SHELL_TOKEN_AND,           /* &  */
    SHELL_TOKEN_SEMICOLON,     /* ;  */
    SHELL_TOKEN_AND_IF,        /* && */
    SHELL_TOKEN_OR_IF,         /* || */
    SHELL_TOKEN_DSEMI,         /* ;; */
    SHELL_TOKEN_LESS,          /* <  */
    SHELL_TOKEN_GREAT,         /* >  */
    SHELL_TOKEN_DLESS,         /* << */
    SHELL_TOKEN_DGREAT,        /* >> */
    SHELL_TOKEN_LESSAND,       /* <& */
    SHELL_TOKEN_GREATAND,      /* >& */
    SHELL_TOKEN_LESSGREAT,     /* <> */
    SHELL_TOKEN_DLESSDASH,     /* <<- */
    SHELL_TOKEN_CLOBBER,       /* >| */
    SHELL_TOKEN_LEFT_PAREN,    /* (  */
    SHELL_TOKEN_RIGHT_PAREN    /* )  */
} ShellTokenKind;

typedef struct ShellToken
{
    ShellTokenKind kind;
    const char *text;   /* Terminated; the token's characters, quotes kept. */
    size_t length;
    size_t column;      /* Where it began in the line, for a diagnostic. */
} ShellToken;

/* A line's tokens, and the storage their text lives in. */
typedef struct ShellTokens
{
    ShellToken token[SHELL_TOKEN_MAXIMUM];
    size_t count;
    char text[SHELL_TEXT_MAXIMUM];
    size_t text_used;
} ShellTokens;

/* What a redirection means, IEEE Std 1003.1-2017, Section 2.7. */
typedef enum ShellRedirectionKind
{
    SHELL_REDIRECT_INPUT = 0,     /* [n]<word   2.7.1 */
    SHELL_REDIRECT_OUTPUT,        /* [n]>word   2.7.2 */
    SHELL_REDIRECT_CLOBBER,       /* [n]>|word  2.7.2, noclobber ignored */
    SHELL_REDIRECT_APPEND,        /* [n]>>word  2.7.3 */
    SHELL_REDIRECT_DUPLICATE_IN,  /* [n]<&word  2.7.5 */
    SHELL_REDIRECT_DUPLICATE_OUT, /* [n]>&word  2.7.6 */
    SHELL_REDIRECT_READ_WRITE     /* [n]<>word  2.7.7 */
} ShellRedirectionKind;

typedef struct ShellRedirection
{
    ShellRedirectionKind kind;
    int descriptor;     /* The io_number, or the operator's default: 0 for <, 1 for >. */
    const char *target; /* The word, quotes kept. */
} ShellRedirection;

/* A simple command: its words in order, the first being the command's name,
 * and its redirections in order. Section 2.9.1. */
typedef struct ShellCommand
{
    const char *word[SHELL_WORD_MAXIMUM];
    size_t word_count;
    ShellRedirection redirection[SHELL_REDIRECTION_MAXIMUM];
    size_t redirection_count;

    /*
     * The assignment words before the name, of sub-task 8.3: Section 2.10.2,
     * rule 7, has a word of the form `NAME=value` in that position be an
     * assignment and not a word, and Section 2.9.1 has them applied to the
     * command's environment — or, where there is no command name, to the
     * shell's own variables. Each is the whole word, quotes kept, `NAME=`
     * included; the shell splits it after expansion.
     */
    const char *assignment[SHELL_ASSIGNMENT_MAXIMUM];
    size_t assignment_count;
} ShellCommand;

/* What joins one and_or to the next in a list, or ends it. */
typedef enum ShellSeparator
{
    SHELL_SEPARATOR_NONE = 0,   /* The last and_or of the list. */
    SHELL_SEPARATOR_SEQUENCE,   /* ; — or the end of the line, which is the same. */
    SHELL_SEPARATOR_BACKGROUND  /* & */
} ShellSeparator;

/* What joins one pipeline to the next within an and_or. */
typedef enum ShellCondition
{
    SHELL_CONDITION_NONE = 0,   /* The first pipeline of an and_or. */
    SHELL_CONDITION_AND_IF,     /* && : run if the previous succeeded. */
    SHELL_CONDITION_OR_IF       /* || : run if the previous failed. */
} ShellCondition;

/*
 * A pipeline, with what precedes and what follows it. The list of Section
 * 2.10 is `and_or`s joined by separators and an `and_or` is pipelines joined
 * by `&&` and `||`; both levels are flattened here into one sequence of
 * pipelines, each carrying the condition that connects it to the one before
 * and the separator that follows it. A tree would have said the same thing in
 * more structure than a shell that runs left to right needs.
 */
typedef struct ShellPipeline
{
    ShellCommand command[SHELL_COMMAND_MAXIMUM];
    size_t command_count;
    bool negated;              /* ! before the pipeline: the status is inverted. */
    ShellCondition condition;  /* How this pipeline follows the one before. */
    ShellSeparator separator;  /* What follows this pipeline. */
} ShellPipeline;

typedef struct ShellList
{
    ShellPipeline pipeline[SHELL_PIPELINE_MAXIMUM];
    size_t pipeline_count;
} ShellList;

/*
 * What tokenising or parsing a line came to.
 *
 * INCOMPLETE is the one a shell acts upon rather than reports: a quote or an
 * operator that wants more — `echo "a`, or `ls |` — is a line that continues,
 * and an interactive shell prompts for the rest with PS2 and tries again.
 */
typedef enum ShellParseStatus
{
    SHELL_PARSE_OK = 0,
    SHELL_PARSE_EMPTY,               /* No command at all: blank, or a comment. */
    SHELL_PARSE_INCOMPLETE,          /* An unterminated quote, or an operator with nothing after it. */
    SHELL_PARSE_UNEXPECTED_TOKEN,    /* An operator where a word was required, or the reverse. */
    SHELL_PARSE_UNSUPPORTED,         /* A token the grammar names and this shell does not implement. */
    SHELL_PARSE_TOO_MANY_TOKENS,     /* A bound above was exceeded. */
    SHELL_PARSE_TOO_MANY_WORDS,
    SHELL_PARSE_TOO_MANY_ASSIGNMENTS,
    SHELL_PARSE_TOO_MANY_REDIRECTIONS,
    SHELL_PARSE_TOO_MANY_COMMANDS,
    SHELL_PARSE_TOO_MANY_PIPELINES,
    SHELL_PARSE_BAD_DESCRIPTOR       /* An io_number too large to be one. */
} ShellParseStatus;

/*
 * Breaks a line into tokens, IEEE Std 1003.1-2017, Section 2.3. The line must
 * be terminated and may hold a newline only within quotes, where it is kept.
 * Returns OK, INCOMPLETE where a quote is not closed, or TOO_MANY_TOKENS.
 */
ShellParseStatus ShellTokenise(const char *line, ShellTokens *tokens);

/*
 * Builds the command structure of a tokenised line. On any status but OK and
 * EMPTY, `offending` — if given — is set to the index of the token the parser
 * stopped at, so that a diagnostic can name it.
 */
ShellParseStatus ShellParse(const ShellTokens *tokens, ShellList *list, size_t *offending);

/*
 * Quote removal, Section 2.2 as read at Section 2.6.7: a copy of `word` with
 * the quoting characters removed and the characters they quoted kept. Returns
 * false where the copy does not fit, `capacity` including the terminator.
 */
bool ShellUnquote(const char *word, char *destination, size_t capacity);

/* The name of a status, for a diagnostic. */
const char *ShellParseStatusName(ShellParseStatus status);

/* The text of an operator token kind, for a diagnostic; a word's is its text. */
const char *ShellTokenKindText(ShellTokenKind kind);

/*
 * Sub-task 8.3: names, variables and expansion.
 *
 * A name is Section 3.235's: letters, digits and underscores, not beginning
 * with a digit. An assignment word is `NAME=` followed by anything, and
 * ShellIsAssignmentWord returns the length of the name — the position of the
 * `=` — or zero where the word is not one.
 */
bool ShellIsName(const char *text, size_t length);
size_t ShellIsAssignmentWord(const char *word);

/* The variable table: set, read, mark for export, and walk. A value that
 * does not fit, a name that is not one, or a table that is full is refused. */
void ShellVariablesInitialise(void);
bool ShellVariableSet(const char *name, const char *value);
const char *ShellVariableGet(const char *name);
bool ShellVariableExport(const char *name);
bool ShellVariableIsExported(const char *name);
bool ShellVariableUnset(const char *name);
size_t ShellVariableCount(void);
bool ShellVariableAt(size_t position, const char **name, const char **value, bool *exported);

/*
 * What an expansion asks of its caller: the value of a parameter by name, or
 * a null pointer for one that is unset. It is a function and not the table
 * above so that the expansion may be asserted with a table of the test's own,
 * and so that `?` — which is no variable — is the caller's to answer.
 */
typedef const char *(*ShellLookup)(void *context, const char *name);

/*
 * Expands `$NAME`, `${NAME}` and `$?` within `word` — outside single quotes,
 * and within double quotes — and removes the quotes, into `destination`.
 * Section 2.6.2 and Section 2.6.7, in one pass; the characters a value
 * supplies are never quoting characters. Returns false where the result does
 * not fit or a name is too long.
 */
bool ShellExpandWord(const char *word, char *destination, size_t capacity,
                     ShellLookup lookup, void *context);

/*
 * The built-in commands, of sub-task 8.3, in userland/sh/builtins.c — the one
 * unit of the shell's that reaches a system call and is therefore not
 * compiled into the kernel image. ShellRunBuiltin runs the command named by
 * argv[0] and returns its status, or -1 where the name is no built-in;
 * `exit` sets `exit_requested` rather than ending the process itself.
 */
bool ShellIsBuiltin(const char *name);
bool ShellIsSpecialBuiltin(const char *name);
int ShellRunBuiltin(int argc, char **argv, int last_status, bool *exit_requested);

/*
 * External program execution, of sub-task 8.4, in userland/sh/run.c — a unit
 * that reaches system calls and is therefore, like builtins.c, not compiled
 * into the kernel image. ShellRunProgram forks, seeks and executes the program
 * argv[0] names in the child with the environment ShellBuildEnvironment
 * composes from the exported variables, waits, and returns the status: the
 * program's, 127 where it could not be found, 126 where it was found and
 * could not run, or 128 plus the vector where it ended by a fault.
 * ShellExecuteProgram is the child's half alone — the redirections and the
 * search, in the calling process, which it replaces upon success — for a
 * caller that has already forked, which the pipeline is.
 */
int ShellRunProgram(char **argv, const ShellCommand *command, ShellLookup lookup,
                    void *context);
int ShellExecuteProgram(char **argv, const ShellCommand *command, ShellLookup lookup,
                        void *context);
char **ShellBuildEnvironment(void);

/*
 * The pipeline, of sub-task 8.6, in the same unit. ShellRunPipeline makes one
 * child per command with a pipe between each pair, runs each command in its
 * child by `run_stage` — which runs a built-in there or becomes the program,
 * and whose result is the child's status — waits for every child, and returns
 * the last command's status, as IEEE Std 1003.1-2017, Section 2.9.2, has it.
 * A pipeline of one command is not brought here: it runs in the shell itself,
 * where a built-in must run to have any effect.
 */
typedef int (*ShellStageRunner)(const ShellCommand *command, void *context);

int ShellRunPipeline(const ShellPipeline *pipeline, ShellStageRunner run_stage, void *context,
                     bool background);

/*
 * Job control, of sub-task 8.7, in userland/sh/jobs.c — a unit that reaches
 * system calls and is therefore not compiled into the kernel image. A job is
 * a process group made for one pipeline: ShellJobBegin records one,
 * ShellJobPrepareChild is the child's half of joining it and
 * ShellJobAddMember the parent's, ShellJobWaitForeground waits until the job
 * ends or stops and takes the terminal back, ShellJobsNotify reports what
 * happened in the background, and the four commands are `jobs`, `fg`, `bg`
 * and `kill`. ShellJobStatus turns a status of <oxys/syscall_abi.h> into the
 * number the shell reports: the code, or 128 plus the signal.
 */
typedef struct ShellJob ShellJob;

void ShellJobsInitialise(void);
ShellJob *ShellJobBegin(const char *text, bool background);
void ShellJobPrepareChild(const ShellJob *job, bool background);
void ShellJobAddMember(ShellJob *job, int64_t pid);
void ShellJobAnnounce(const ShellJob *job);
int ShellJobWaitForeground(ShellJob *job);
void ShellJobsNotify(void);
void ShellJobsList(void);
int ShellJobForeground(const char *operand);
int ShellJobBackground(const char *operand);
int ShellJobKill(int argc, char **argv);
int ShellJobStatus(int64_t status);

#endif /* OXYS_SHELL_H */
