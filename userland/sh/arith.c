/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/sh/arith.c
 * Purpose: The arithmetic of `$((expression))`, of 2026-09-27: an expression
 *          of the ISO C integer operators, evaluated as signed long, its
 *          variables read by the expansion's lookup and written by the shell's
 *          own table.
 * Key functions: ShellArithmetic, ShellArithmeticFault.
 * References:
 *   - IEEE Std 1003.1-2017, Section 2.6.4 (Arithmetic Expansion): signed long
 *     is required; decimal, octal and hexadecimal constants; `sizeof`, `++`
 *     and `--` not required; changes to variables in effect afterwards; a
 *     variable holding an integer constant, optionally signed, is the same
 *     named with or without `$`.
 *   - IEEE Std 1003.1-2017, Section 1.1.2.1 (Arithmetic Precision and
 *     Operations): the operators are those of the ISO C standard.
 *   - ISO/IEC 9899:2011, Section 6.5: their precedence and associativity,
 *     which the functions below follow level by level.
 *   - userland/sh/expand.c: which finds the expression, expands its parameters
 *     and substitutes the value.
 *
 * How the arithmetic is kept defined.
 *
 *   Signed overflow is undefined in C, and a shell is a program a person types
 *   anything into, so every sum, difference and product is formed upon the
 *   unsigned type and converted back, which wraps as the machine does; a shift
 *   takes its count modulo the width, as the processor does; and division or
 *   remainder by zero, and the one quotient that overflows — the least value
 *   divided by minus one — are refused by name rather than performed. A refused
 *   expression substitutes nothing and fails the command, which is what a shell
 *   reporting an error in an expansion does.
 *
 * Why a branch not taken is parsed and not evaluated.
 *
 *   `&&`, `||` and `?:` evaluate one side only, and an assignment or a division
 *   in the other must neither change a variable nor refuse the expression:
 *   `$((n != 0 && 10 / n))` is how a person guards a division. Every function
 *   below takes whether it is evaluating, and one that is not still parses, so
 *   that a malformed branch is refused whichever way the condition falls.
 */

#include "shell.h"

#include <limits.h>

typedef struct ShellArith
{
    const char *text;
    size_t at;
    ShellLookup lookup;
    void *context;
    const char *fault;
    size_t depth; /* Parentheses open around the current position. */
} ShellArith;

/*
 * The most parentheses one within another. Each costs a descent through every
 * level of precedence, a dozen frames, and without a bound a line of nested
 * parentheses — two hundred fit within one — ran the shell off the end of its
 * stack: a page fault, and a person at the serial console left with no shell.
 * Past the bound the expression is refused by name and nothing more is entered.
 * Thirty-two is far past any expression a person writes, and within the stack
 * with a wide margin.
 */
#define SHELL_ARITH_DEPTH_MAXIMUM 32U

static const char *ShellArithFaultText;

static long ShellArithExpression(ShellArith *arith, bool evaluate);

/* The same arithmetic, defined for every value: formed unsigned and converted
 * back, which on this two's-complement machine is the wrapping result. */
static long ShellArithAdd(long first, long second)
{
    return (long)((unsigned long)first + (unsigned long)second);
}

static long ShellArithSubtract(long first, long second)
{
    return (long)((unsigned long)first - (unsigned long)second);
}

static long ShellArithMultiply(long first, long second)
{
    return (long)((unsigned long)first * (unsigned long)second);
}

static bool ShellArithFail(ShellArith *arith, const char *fault)
{
    if (arith->fault == NULL)
    {
        arith->fault = fault;
    }

    return false;
}

static void ShellArithSkipBlanks(ShellArith *arith)
{
    while ((arith->text[arith->at] == ' ') || (arith->text[arith->at] == '\t') ||
           (arith->text[arith->at] == '\n'))
    {
        ++arith->at;
    }
}

/* Whether the operator `op` stands next, and if so consumes it. An operator
 * that is the start of a longer one — `<` of `<=` or `<<`, `&` of `&&` — is
 * not taken for the shorter, and `=` of `==` is not an assignment. */
static bool ShellArithAccept(ShellArith *arith, const char *op)
{
    size_t length = 0U;
    char after;

    ShellArithSkipBlanks(arith);

    while (op[length] != '\0')
    {
        if (arith->text[arith->at + length] != op[length])
        {
            return false;
        }

        ++length;
    }

    after = arith->text[arith->at + length];

    if ((length == 1U) && (((op[0] == '<') && ((after == '<') || (after == '='))) ||
                           ((op[0] == '>') && ((after == '>') || (after == '='))) ||
                           ((op[0] == '&') && ((after == '&') || (after == '='))) ||
                           ((op[0] == '|') && ((after == '|') || (after == '='))) ||
                           ((op[0] == '=') && (after == '=')) ||
                           ((op[0] == '!') && (after == '=')) ||
                           (((op[0] == '+') || (op[0] == '-') || (op[0] == '*') ||
                             (op[0] == '/') || (op[0] == '%') || (op[0] == '^')) &&
                            (after == '='))))
    {
        return false;
    }

    if ((length == 2U) && (after == '=') &&
        (((op[0] == '<') && (op[1] == '<')) || ((op[0] == '>') && (op[1] == '>'))))
    {
        return false;
    }

    arith->at += length;

    return true;
}

static bool ShellArithIsNameStart(char character)
{
    return ((character >= 'a') && (character <= 'z')) ||
           ((character >= 'A') && (character <= 'Z')) || (character == '_');
}

static bool ShellArithIsDigit(char character)
{
    return (character >= '0') && (character <= '9');
}

/* The value of a digit in any base up to sixteen, or sixteen for none. */
static unsigned ShellArithDigit(char character)
{
    if (ShellArithIsDigit(character))
    {
        return (unsigned)(character - '0');
    }

    if ((character >= 'a') && (character <= 'f'))
    {
        return (unsigned)(character - 'a') + 10U;
    }

    if ((character >= 'A') && (character <= 'F'))
    {
        return (unsigned)(character - 'A') + 10U;
    }

    return 16U;
}

/*
 * An integer constant of ISO C, Section 6.4.4.1, without suffixes: decimal,
 * octal after a leading 0, hexadecimal after 0x. It wraps rather than
 * refusing a constant past the type, as the arithmetic does. `*length` is how
 * many characters it took, zero where there is none.
 */
static bool ShellArithConstant(const char *text, long *value, size_t *length)
{
    unsigned long accumulated = 0UL;
    unsigned base = 10U;
    size_t at = 0U;
    size_t digits = 0U;

    if (!ShellArithIsDigit(text[0]))
    {
        *length = 0U;

        return false;
    }

    if ((text[0] == '0') && ((text[1] == 'x') || (text[1] == 'X')))
    {
        base = 16U;
        at = 2U;
    }
    else if (text[0] == '0')
    {
        base = 8U;
    }

    while (ShellArithDigit(text[at]) < base)
    {
        accumulated = (accumulated * base) + ShellArithDigit(text[at]);
        ++at;
        ++digits;
    }

    /* `0x` with no digit, or a digit past the base — `09`, `0xg` — is not a
     * constant, and neither is one run into a name, `12ab`. */
    if (((base == 16U) && (digits == 0U)) || ShellArithIsNameStart(text[at]) ||
        ShellArithIsDigit(text[at]))
    {
        *length = 0U;

        return false;
    }

    *value = (long)accumulated;
    *length = at;

    return true;
}

/* The value of a variable: unset or empty is zero, and anything else must be
 * an integer constant, optionally signed, as Section 2.6.4 has it. */
static bool ShellArithVariable(ShellArith *arith, const char *name, long *value)
{
    const char *const text = (arith->lookup != NULL) ? arith->lookup(arith->context, name) : NULL;
    size_t at = 0U;
    size_t length;
    bool negative = false;

    *value = 0;

    if ((text == NULL) || (text[0] == '\0'))
    {
        return true;
    }

    while ((text[at] == ' ') || (text[at] == '\t'))
    {
        ++at;
    }

    if ((text[at] == '+') || (text[at] == '-'))
    {
        negative = text[at] == '-';
        ++at;
    }

    if (!ShellArithConstant(&text[at], value, &length))
    {
        return ShellArithFail(arith, "a variable does not hold a number");
    }

    at += length;

    while ((text[at] == ' ') || (text[at] == '\t'))
    {
        ++at;
    }

    if (text[at] != '\0')
    {
        return ShellArithFail(arith, "a variable does not hold a number");
    }

    *value = negative ? ShellArithSubtract(0, *value) : *value;

    return true;
}

/* Writes a value to a variable, in decimal, through the shell's own table. */
static bool ShellArithStore(ShellArith *arith, const char *name, long value)
{
    char text[24];
    char reversed[24];
    size_t length = 0U;
    size_t at = 0U;
    unsigned long magnitude = (value < 0) ? (0UL - (unsigned long)value) : (unsigned long)value;

    do
    {
        reversed[length++] = (char)('0' + (int)(magnitude % 10UL));
        magnitude /= 10UL;
    } while (magnitude != 0UL);

    if (value < 0)
    {
        text[at++] = '-';
    }

    while (length > 0U)
    {
        text[at++] = reversed[--length];
    }

    text[at] = '\0';

    if (!ShellVariableSet(name, text))
    {
        return ShellArithFail(arith, "a variable could not be set");
    }

    return true;
}

/* A name at the current position, copied into `name`; false where there is
 * none, or one too long for a variable. */
static bool ShellArithName(ShellArith *arith, char *name)
{
    size_t length = 0U;

    ShellArithSkipBlanks(arith);

    if (!ShellArithIsNameStart(arith->text[arith->at]))
    {
        return false;
    }

    while (ShellArithIsNameStart(arith->text[arith->at]) ||
           ShellArithIsDigit(arith->text[arith->at]))
    {
        if (length >= SHELL_NAME_MAXIMUM)
        {
            return ShellArithFail(arith, "a name is too long");
        }

        name[length++] = arith->text[arith->at++];
    }

    name[length] = '\0';

    return true;
}

/* primary: a constant, a variable, or a parenthesised expression. */
static long ShellArithPrimary(ShellArith *arith, bool evaluate)
{
    char name[SHELL_NAME_MAXIMUM + 1U];
    long value = 0;
    size_t length;

    ShellArithSkipBlanks(arith);

    if (ShellArithAccept(arith, "("))
    {
        if (arith->depth >= SHELL_ARITH_DEPTH_MAXIMUM)
        {
            /* Refused here, before the descent: every caller above returns
             * with the fault recorded, and nothing nested further is read. */
            (void)ShellArithFail(arith, "parentheses are nested too deeply");

            return 0;
        }

        ++arith->depth;
        value = ShellArithExpression(arith, evaluate);
        --arith->depth;

        if (!ShellArithAccept(arith, ")"))
        {
            (void)ShellArithFail(arith, "a parenthesis is not closed");
        }

        return value;
    }

    if (ShellArithConstant(&arith->text[arith->at], &value, &length))
    {
        arith->at += length;

        return value;
    }

    if (ShellArithName(arith, name))
    {
        if (evaluate)
        {
            (void)ShellArithVariable(arith, name, &value);
        }

        return value;
    }

    (void)ShellArithFail(arith, "a number or a name was expected");

    return 0;
}

/* unary: + - ! ~ before a unary, right to left. */
static long ShellArithUnary(ShellArith *arith, bool evaluate)
{
    if (ShellArithAccept(arith, "+"))
    {
        return ShellArithUnary(arith, evaluate);
    }

    if (ShellArithAccept(arith, "-"))
    {
        return ShellArithSubtract(0, ShellArithUnary(arith, evaluate));
    }

    if (ShellArithAccept(arith, "!"))
    {
        return (ShellArithUnary(arith, evaluate) == 0) ? 1 : 0;
    }

    if (ShellArithAccept(arith, "~"))
    {
        return ~ShellArithUnary(arith, evaluate);
    }

    return ShellArithPrimary(arith, evaluate);
}

/* Applies one binary operator of the multiplicative to the bitwise levels. A
 * division not being evaluated is not refused for its divisor. */
static long ShellArithApply(ShellArith *arith, const char *op, long first, long second,
                            bool evaluate)
{
    switch (op[0])
    {
    case '*': return ShellArithMultiply(first, second);
    case '/':
    case '%':
        if (!evaluate)
        {
            return 0;
        }

        if (second == 0)
        {
            (void)ShellArithFail(arith, "division by zero");

            return 0;
        }

        if ((first == LONG_MIN) && (second == -1))
        {
            (void)ShellArithFail(arith, "the quotient does not fit");

            return 0;
        }

        return (op[0] == '/') ? (first / second) : (first % second);
    case '+': return ShellArithAdd(first, second);
    case '-': return ShellArithSubtract(first, second);
    case '<':
        if (op[1] == '<')
        {
            return (long)((unsigned long)first << ((unsigned long)second & 63UL));
        }

        return (op[1] == '=') ? (first <= second) : (first < second);
    case '>':
        if (op[1] == '>')
        {
            return first >> ((unsigned long)second & 63UL);
        }

        return (op[1] == '=') ? (first >= second) : (first > second);
    case '=': return first == second;
    case '!': return first != second;
    case '&': return first & second;
    case '^': return first ^ second;
    case '|': return first | second;
    default: return 0;
    }
}

/*
 * The binary levels of ISO C, Section 6.5.5 to 6.5.12, highest first, each
 * left to right. `&&` and `||` are apart below, since they do not evaluate
 * their right side always.
 */
static const char *const ShellArithLevels[][5] = {
    { "*", "/", "%", NULL, NULL },
    { "+", "-", NULL, NULL, NULL },
    { "<<", ">>", NULL, NULL, NULL },
    { "<=", ">=", "<", ">", NULL },
    { "==", "!=", NULL, NULL, NULL },
    { "&", NULL, NULL, NULL, NULL },
    { "^", NULL, NULL, NULL, NULL },
    { "|", NULL, NULL, NULL, NULL },
};

#define SHELL_ARITH_LEVELS (sizeof ShellArithLevels / sizeof ShellArithLevels[0])

static long ShellArithBinary(ShellArith *arith, size_t level, bool evaluate)
{
    long value = (level == 0U) ? ShellArithUnary(arith, evaluate)
                               : ShellArithBinary(arith, level - 1U, evaluate);

    for (;;)
    {
        const char *matched = NULL;

        for (size_t index = 0U; (index < 5U) && (ShellArithLevels[level][index] != NULL); ++index)
        {
            if (ShellArithAccept(arith, ShellArithLevels[level][index]))
            {
                matched = ShellArithLevels[level][index];
                break;
            }
        }

        if (matched == NULL)
        {
            return value;
        }

        {
            const long right = (level == 0U) ? ShellArithUnary(arith, evaluate)
                                             : ShellArithBinary(arith, level - 1U, evaluate);

            value = ShellArithApply(arith, matched, value, right, evaluate);
        }
    }
}

static long ShellArithAnd(ShellArith *arith, bool evaluate)
{
    long value = ShellArithBinary(arith, SHELL_ARITH_LEVELS - 1U, evaluate);

    while (ShellArithAccept(arith, "&&"))
    {
        const bool left = value != 0;
        const long right = ShellArithBinary(arith, SHELL_ARITH_LEVELS - 1U, evaluate && left);

        value = (left && (right != 0)) ? 1 : 0;
    }

    return value;
}

static long ShellArithOr(ShellArith *arith, bool evaluate)
{
    long value = ShellArithAnd(arith, evaluate);

    while (ShellArithAccept(arith, "||"))
    {
        const bool left = value != 0;
        const long right = ShellArithAnd(arith, evaluate && !left);

        value = (left || (right != 0)) ? 1 : 0;
    }

    return value;
}

/* conditional: or ? expression : conditional, right to left. */
static long ShellArithConditional(ShellArith *arith, bool evaluate)
{
    const long condition = ShellArithOr(arith, evaluate);
    long first;
    long second;

    if (!ShellArithAccept(arith, "?"))
    {
        return condition;
    }

    first = ShellArithExpression(arith, evaluate && (condition != 0));

    if (!ShellArithAccept(arith, ":"))
    {
        (void)ShellArithFail(arith, "a `?` has no `:`");

        return 0;
    }

    second = ShellArithConditional(arith, evaluate && (condition == 0));

    return (condition != 0) ? first : second;
}

/*
 * expression: a name and an assignment operator, then an expression, right to
 * left; or a conditional. The assignment is tried first and undone — the
 * position restored — where the name is not followed by one, so `x == 1` and
 * `x + 1` read `x` as a value.
 */
static long ShellArithExpression(ShellArith *arith, bool evaluate)
{
    static const char *const ShellArithAssignments[] = {
        "=", "*=", "/=", "%=", "+=", "-=", "<<=", ">>=", "&=", "^=", "|=",
    };
    const size_t start = arith->at;
    char name[SHELL_NAME_MAXIMUM + 1U];

    if (ShellArithName(arith, name))
    {
        for (size_t index = 0U;
             index < (sizeof ShellArithAssignments / sizeof ShellArithAssignments[0]); ++index)
        {
            const char *const op = ShellArithAssignments[index];

            if (ShellArithAccept(arith, op))
            {
                const long right = ShellArithExpression(arith, evaluate);
                long value = right;

                if (op[0] != '=')
                {
                    long current = 0;
                    char binary[3] = { op[0], (op[1] != '=') ? op[1] : '\0', '\0' };

                    if (evaluate)
                    {
                        (void)ShellArithVariable(arith, name, &current);
                    }

                    value = ShellArithApply(arith, binary, current, right, evaluate);
                }

                if (evaluate && (arith->fault == NULL))
                {
                    (void)ShellArithStore(arith, name, value);
                }

                return value;
            }
        }
    }

    arith->at = start;

    return ShellArithConditional(arith, evaluate);
}

bool ShellArithmetic(const char *expression, long *value, ShellLookup lookup, void *context)
{
    ShellArith arith;

    arith.text = expression;
    arith.at = 0U;
    arith.lookup = lookup;
    arith.context = context;
    arith.fault = NULL;
    arith.depth = 0U;

    ShellArithSkipBlanks(&arith);

    /* An empty expression is zero, as the shells of this lineage have it. */
    if (expression[arith.at] == '\0')
    {
        *value = 0;
        ShellArithFaultText = NULL;

        return true;
    }

    *value = ShellArithExpression(&arith, true);
    ShellArithSkipBlanks(&arith);

    if ((arith.fault == NULL) && (arith.text[arith.at] != '\0'))
    {
        arith.fault = "an operator was expected";
    }

    ShellArithFaultText = arith.fault;

    return arith.fault == NULL;
}

const char *ShellArithmeticFault(void)
{
    return ShellArithFaultText;
}

void ShellArithmeticClear(void)
{
    ShellArithFaultText = NULL;
}
