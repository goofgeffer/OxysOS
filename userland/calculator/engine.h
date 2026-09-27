/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/calculator/engine.h
 * Purpose: The calculator's arithmetic and its keys, of 2026-09-27: what a
 *          press of each key does to the number shown, held apart from the
 *          window so that the kernel's self-test asserts it without one.
 * Key definitions: CalcState, CALC_SCALE, CALC_LIMIT, CalcReset, CalcKey,
 *          CalcDisplay, CalcApply, CalcFormat.
 * References:
 *   - userland/calculator/main.c: the window, which draws the display and
 *     turns presses and keys into CalcKey.
 *   - kernel/test/calculator/engine.c: the self-test.
 *   - docs/design/UTILITIES.md: the design.
 *
 * Why fixed point, and not floating point.
 *
 *   Every program here is compiled without the floating-point unit — the
 *   kernel saves no x87 or SSE state across a switch, and the flags that
 *   forbid them are what keep a program from using state that would be lost.
 *   A number is therefore a signed 64-bit count of millionths: six decimal
 *   places, and at most twelve digits before the point, so that every value
 *   scaled stays below 10^18 and every step below stays within 64 bits. A
 *   result past that is an overflow, said, and never a wrapped number shown as
 *   if it were right.
 *
 * What it is.
 *
 *   A calculator of the kind a desk has: a number is typed, an operator
 *   pressed, a second number typed, and `=` or the next operator gives the
 *   result, left to right, with no precedence. A result stands until a digit
 *   begins a new number or an operator takes it as its first operand.
 */

#ifndef OXYS_CALCULATOR_ENGINE_H
#define OXYS_CALCULATOR_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* A value is a count of millionths; the largest magnitude is one less than a
 * trillion, in millionths. */
#define CALC_SCALE    INT64_C(1000000)
#define CALC_DECIMALS 6U
#define CALC_LIMIT    INT64_C(1000000000000000000)

/* The longest text the display holds: a sign, twelve digits, the point and
 * six more, and a terminator; or the longest message, which is longer. */
#define CALC_DISPLAY_MAXIMUM 32U

typedef struct CalcState
{
    int64_t accumulator;  /* The first operand, or the last result. */
    char operation;       /* '+', '-', '*', '/', or 0 for none pending. */

    /* The number being typed: its magnitude in millionths, its sign, whether
     * a point has been typed, and how many digits after it. */
    int64_t entry;
    bool negative;
    bool entering;
    bool point;
    unsigned decimals;

    /* A message in place of a number — a division by zero, an overflow — kept
     * until the next key clears it. */
    const char *error;

    char display[CALC_DISPLAY_MAXIMUM];
} CalcState;

/* Everything cleared: zero shown, nothing pending. */
void CalcReset(CalcState *state);

/*
 * One key. The digits and `.`; the operators `+`, `-`, `*` and `/`; `=`;
 * `C`, which clears; `b`, which takes back the last digit typed; `n`, which
 * changes the sign; and `%`, which divides the number shown by a hundred.
 * Anything else does nothing. The display is brought up to date.
 */
void CalcKey(CalcState *state, char key);

/* The text the display shows. */
const char *CalcDisplay(const CalcState *state);

/*
 * `first` operation `second`, both in millionths, into `result`. False, with
 * `*fault` naming why, for a division by zero or a result whose magnitude is
 * CALC_LIMIT or more. Multiplication and division round the last place half
 * away from zero.
 */
bool CalcApply(char operation, int64_t first, int64_t second, int64_t *result,
               const char **fault);

/* A value in millionths as decimal text, with no trailing zeros after the
 * point and no point where there are none. False where it does not fit. */
bool CalcFormat(int64_t value, char *text, size_t capacity);

#endif /* OXYS_CALCULATOR_ENGINE_H */
