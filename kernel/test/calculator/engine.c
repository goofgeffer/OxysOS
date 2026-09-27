/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/calculator/engine.c
 * Purpose: Asserts the calculator's arithmetic and keys, of 2026-09-27, upon
 *          the code the calculator ships: userland/calculator/engine.c calls
 *          nothing, and is compiled into this image as the shell's grammar is.
 * Key functions: KernelVerifyCalculator.
 * References:
 *   - userland/calculator/engine.h: the fixed point, and what each key does.
 *   - docs/design/UTILITIES.md: every assertion here paired with the failure
 *     it would catch.
 *
 * The keys are asserted as a person presses them: a string of keys given one
 * by one to CalcKey, and the display compared with what a desk calculator
 * shows. The window gives CalcKey the same characters for its buttons and for
 * the keyboard, so these are the presses and the typing both.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include "../../../userland/calculator/engine.h"

static bool VerifyCalculatorSucceeded;

static void VerifyCalculatorRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyCalculatorSucceeded = false;
    }
}

static bool VerifyCalculatorSame(const char *first, const char *second)
{
    size_t index = 0U;

    while ((first[index] != '\0') && (first[index] == second[index]))
    {
        ++index;
    }

    return first[index] == second[index];
}

/* Whether `operation` upon two values in millionths gives `expected`. */
static bool VerifyCalculatorGives(char operation, int64_t first, int64_t second, int64_t expected)
{
    const char *fault = NULL;
    int64_t result = 0;

    return CalcApply(operation, first, second, &result, &fault) && (result == expected);
}

/* Whether the keys, pressed in order from a cleared calculator, leave the
 * display showing `expected`. */
static bool VerifyCalculatorShows(const char *keys, const char *expected)
{
    CalcState state;

    CalcReset(&state);

    for (const char *key = keys; *key != '\0'; ++key)
    {
        CalcKey(&state, *key);
    }

    return VerifyCalculatorSame(CalcDisplay(&state), expected);
}

void KernelVerifyCalculator(void)
{
    const int64_t S = CALC_SCALE;
    const char *fault = NULL;
    int64_t result = 0;
    char text[CALC_DISPLAY_MAXIMUM];

    VerifyCalculatorSucceeded = true;
    KernelWriteString("Calculator: asserting the arithmetic and the keys.\n");

    /* The arithmetic in fixed point: decimals exact where a float's are not,
     * the last place rounded half away from zero, the sign of a product. */
    VerifyCalculatorRequire(VerifyCalculatorGives('+', S / 10, (2 * S) / 10, (3 * S) / 10),
                            "0.1 + 0.2 was not 0.3 exactly");
    VerifyCalculatorRequire(VerifyCalculatorGives('/', 7 * S, 2 * S, (7 * S) / 2) &&
                                VerifyCalculatorGives('/', S, 3 * S, 333333) &&
                                VerifyCalculatorGives('/', 2 * S, 3 * S, 666667),
                            "a quotient was not carried to six places and rounded");
    VerifyCalculatorRequire(VerifyCalculatorGives('*', -5 * S, (3 * S) / 2, (-15 * S) / 2) &&
                                VerifyCalculatorGives('*', 999999 * S, 999999 * S,
                                                      INT64_C(999998000001) * S),
                            "a product was wrong in its sign or its size");
    VerifyCalculatorRequire(
        VerifyCalculatorGives('*', INT64_C(123456789000), 1000 * S, INT64_C(123456789) * S),
        "a product of a fraction was wrong");

    /* The limits, said rather than wrapped. */
    VerifyCalculatorRequire(!CalcApply('+', INT64_C(999999999999) * S, S, &result, &fault) &&
                                (fault != NULL) && VerifyCalculatorSame(fault, "Overflow"),
                            "a sum past twelve digits was not an overflow");
    VerifyCalculatorRequire(!CalcApply('*', 1000000 * S, 1000000 * S, &result, &fault) &&
                                VerifyCalculatorSame(fault, "Overflow"),
                            "a product past twelve digits was not an overflow");
    VerifyCalculatorRequire(!CalcApply('/', S, 0, &result, &fault) &&
                                VerifyCalculatorSame(fault, "Cannot divide by zero"),
                            "a division by zero was not refused by name");

    /* The text: no trailing zeros, no point where there is none. */
    VerifyCalculatorRequire(CalcFormat((7 * S) / 2, text, sizeof text) &&
                                VerifyCalculatorSame(text, "3.5") &&
                                CalcFormat((-15 * S) / 2, text, sizeof text) &&
                                VerifyCalculatorSame(text, "-7.5") &&
                                CalcFormat(0, text, sizeof text) &&
                                VerifyCalculatorSame(text, "0") &&
                                CalcFormat(1, text, sizeof text) &&
                                VerifyCalculatorSame(text, "0.000001") &&
                                CalcFormat(12 * S, text, sizeof text) &&
                                VerifyCalculatorSame(text, "12"),
                            "a value was not written as a person reads it");

    /* The keys, as a person presses them. */
    VerifyCalculatorRequire(VerifyCalculatorShows("12+7=", "19") &&
                                VerifyCalculatorShows("7/2=", "3.5") &&
                                VerifyCalculatorShows(".1+.2=", "0.3"),
                            "a sum or a quotient typed did not show its result");
    VerifyCalculatorRequire(VerifyCalculatorShows("2+3*4=", "20"),
                            "operators were not taken left to right, as a desk calculator's are");
    VerifyCalculatorRequire(VerifyCalculatorShows("5+=", "10"),
                            "= after an operator did not take the number shown as its operand");
    VerifyCalculatorRequire(VerifyCalculatorShows("3.50", "3.50") &&
                                VerifyCalculatorShows("3.", "3."),
                            "a number was not shown as it was typed");
    VerifyCalculatorRequire(VerifyCalculatorShows("123b", "12") &&
                                VerifyCalculatorShows("1.25bb", "1."),
                            "backspace did not take back the last digit");
    VerifyCalculatorRequire(VerifyCalculatorShows("5n", "-5") &&
                                VerifyCalculatorShows("5n+2=", "-3"),
                            "the sign did not change, or was not carried into the sum");
    VerifyCalculatorRequire(VerifyCalculatorShows("50%", "0.5") &&
                                VerifyCalculatorShows("200*50%=", "100"),
                            "percent did not divide the number shown by a hundred");
    VerifyCalculatorRequire(VerifyCalculatorShows("1234567890123", "123456789012"),
                            "a thirteenth digit was taken before the point");
    VerifyCalculatorRequire(VerifyCalculatorShows("1/0=", "Cannot divide by zero") &&
                                VerifyCalculatorShows("1/0=3", "3") &&
                                VerifyCalculatorShows("12+7=C", "0"),
                            "a division by zero was not shown, or was not cleared by the next key");

    KernelWriteString(VerifyCalculatorSucceeded
                          ? "Calculator self-test passed: the arithmetic in fixed point, its "
                            "limits said, and every key as a person presses it.\n"
                          : "Calculator self-test FAILED.\n");
}
