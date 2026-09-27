/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: userland/calculator/engine.c
 * Purpose: The calculator's arithmetic in fixed point and what each key does,
 *          of 2026-09-27. It calls nothing, so the kernel's self-test links it
 *          and asserts the code the calculator ships.
 * Key functions: CalcReset, CalcKey, CalcDisplay, CalcApply, CalcFormat.
 * References:
 *   - userland/calculator/engine.h: the representation, and why it is fixed
 *     point.
 *
 * How a product and a quotient stay within 64 bits.
 *
 *   Every magnitude is below 10^18 in millionths. A product of two is up to
 *   10^36 before its scale is taken off, so each operand is split into its
 *   whole part and its millionths, a = ah·S + al, and the product formed as
 *   ah·bh·S + ah·bl + al·bh + al·bl/S: the first is checked for overflow, and
 *   each of the others is below 10^18 by construction. A quotient is the whole
 *   part of the division and then six digits of it, one at a time, the
 *   remainder times ten staying below 10^19, within an unsigned 64 bits.
 */

#include "engine.h"

static const char CalcFaultDivide[] = "Cannot divide by zero";
static const char CalcFaultOverflow[] = "Overflow";

static uint64_t CalcMagnitude(int64_t value)
{
    return (value < 0) ? (0U - (uint64_t)value) : (uint64_t)value;
}

/* A magnitude with a sign, or false where it is CALC_LIMIT or more. */
static bool CalcSigned(uint64_t magnitude, bool negative, int64_t *result)
{
    if (magnitude >= (uint64_t)CALC_LIMIT)
    {
        return false;
    }

    *result = negative ? -(int64_t)magnitude : (int64_t)magnitude;

    return true;
}

bool CalcApply(char operation, int64_t first, int64_t second, int64_t *result,
               const char **fault)
{
    const uint64_t a = CalcMagnitude(first);
    const uint64_t b = CalcMagnitude(second);
    const bool negative = (first < 0) != (second < 0);
    const uint64_t scale = (uint64_t)CALC_SCALE;

    *fault = CalcFaultOverflow;

    switch (operation)
    {
    case '+':
    case '-':
    {
        /* Both below 10^18, so neither the sum nor the difference can leave
         * a signed 64 bits before it is judged. */
        const int64_t value = (operation == '+') ? (first + second) : (first - second);

        return CalcSigned(CalcMagnitude(value), value < 0, result);
    }
    case '*':
    {
        const uint64_t ah = a / scale;
        const uint64_t al = a % scale;
        const uint64_t bh = b / scale;
        const uint64_t bl = b % scale;
        uint64_t whole;
        uint64_t magnitude;

        if (__builtin_mul_overflow(ah, bh, &whole) || (whole >= (uint64_t)CALC_LIMIT / scale))
        {
            return false;
        }

        magnitude = (whole * scale) + (ah * bl) + (al * bh) + (((al * bl) + (scale / 2U)) / scale);

        return CalcSigned(magnitude, negative, result);
    }
    case '/':
    {
        uint64_t quotient;
        uint64_t remainder;
        uint64_t fraction = 0U;

        if (b == 0U)
        {
            *fault = CalcFaultDivide;

            return false;
        }

        /* a / b in millionths is (a·S) / b: the whole part, then each place. */
        quotient = a / b;
        remainder = a % b;

        if (quotient >= (uint64_t)CALC_LIMIT / scale)
        {
            return false;
        }

        for (unsigned place = 0U; place < CALC_DECIMALS; ++place)
        {
            remainder *= 10U;
            fraction = (fraction * 10U) + (remainder / b);
            remainder %= b;
        }

        /* Half away from zero, by the next digit. */
        if (((remainder * 10U) / b) >= 5U)
        {
            ++fraction;
        }

        return CalcSigned((quotient * scale) + fraction, negative, result);
    }
    default:
        return false;
    }
}

bool CalcFormat(int64_t value, char *text, size_t capacity)
{
    const uint64_t magnitude = CalcMagnitude(value);
    uint64_t whole = magnitude / (uint64_t)CALC_SCALE;
    uint64_t fraction = magnitude % (uint64_t)CALC_SCALE;
    char digits[24];
    size_t count = 0U;
    size_t at = 0U;
    unsigned places = CALC_DECIMALS;

    do
    {
        digits[count++] = (char)('0' + (int)(whole % 10U));
        whole /= 10U;
    } while (whole != 0U);

    /* Trailing zeros after the point are not shown. */
    while ((places > 0U) && (fraction != 0U) && ((fraction % 10U) == 0U))
    {
        fraction /= 10U;
        --places;
    }

    if ((count + places + 3U) > capacity)
    {
        return false;
    }

    if ((value < 0) && (magnitude != 0U))
    {
        text[at++] = '-';
    }

    while (count > 0U)
    {
        text[at++] = digits[--count];
    }

    if (fraction != 0U)
    {
        text[at++] = '.';

        for (unsigned place = places; place > 0U; --place)
        {
            uint64_t power = 1U;

            for (unsigned step = 1U; step < place; ++step)
            {
                power *= 10U;
            }

            text[at++] = (char)('0' + (int)((fraction / power) % 10U));
        }
    }

    text[at] = '\0';

    return true;
}

/* The number being typed, with its sign. */
static int64_t CalcEntryValue(const CalcState *state)
{
    return state->negative ? -state->entry : state->entry;
}

/* The power of ten that the `place`-th digit after the point is worth, in
 * millionths: 100000 for the first, 1 for the sixth. */
static int64_t CalcPlaceValue(unsigned place)
{
    int64_t value = CALC_SCALE;

    for (unsigned step = 0U; step < place; ++step)
    {
        value /= 10;
    }

    return value;
}

/*
 * The display while a number is typed: as typed, so that a point and the
 * zeros after it show before any other digit follows them — `3.`, `3.50`.
 */
static void CalcShowEntry(CalcState *state)
{
    char text[CALC_DISPLAY_MAXIMUM];
    size_t at = 0U;
    const int64_t whole = state->entry / CALC_SCALE;
    char digits[24];
    size_t count = 0U;
    uint64_t rest = (uint64_t)whole;

    do
    {
        digits[count++] = (char)('0' + (int)(rest % 10U));
        rest /= 10U;
    } while (rest != 0U);

    if (state->negative)
    {
        text[at++] = '-';
    }

    while (count > 0U)
    {
        text[at++] = digits[--count];
    }

    if (state->point)
    {
        text[at++] = '.';

        for (unsigned place = 1U; place <= state->decimals; ++place)
        {
            text[at++] = (char)('0' + (int)((state->entry / CalcPlaceValue(place)) % 10));
        }
    }

    text[at] = '\0';

    for (size_t index = 0U; index <= at; ++index)
    {
        state->display[index] = text[index];
    }
}

/* The display showing a value, or its message. */
static void CalcShowValue(CalcState *state, int64_t value)
{
    if (!CalcFormat(value, state->display, sizeof state->display))
    {
        state->error = CalcFaultOverflow;
    }
}

static void CalcShowError(CalcState *state)
{
    size_t index = 0U;

    while ((state->error[index] != '\0') && (index + 1U < sizeof state->display))
    {
        state->display[index] = state->error[index];
        ++index;
    }

    state->display[index] = '\0';
}

void CalcReset(CalcState *state)
{
    state->accumulator = 0;
    state->operation = 0;
    state->entry = 0;
    state->negative = false;
    state->entering = false;
    state->point = false;
    state->decimals = 0U;
    state->error = NULL;
    state->display[0] = '0';
    state->display[1] = '\0';
}

/* Begins a new number where one is not being typed. */
static void CalcBeginEntry(CalcState *state)
{
    if (!state->entering)
    {
        state->entry = 0;
        state->negative = false;
        state->point = false;
        state->decimals = 0U;
        state->entering = true;
    }
}

static void CalcDigit(CalcState *state, int digit)
{
    CalcBeginEntry(state);

    if (state->point)
    {
        /* Past the sixth place a digit is not taken: it would be a place the
         * value does not hold. */
        if (state->decimals < CALC_DECIMALS)
        {
            ++state->decimals;
            state->entry += digit * CalcPlaceValue(state->decimals);
        }
    }
    else if (((state->entry / CALC_SCALE) * 10) + digit < (CALC_LIMIT / CALC_SCALE))
    {
        /* A thirteenth digit before the point is not taken, for the same
         * reason. */
        state->entry = (((state->entry / CALC_SCALE) * 10) + digit) * CALC_SCALE +
                       (state->entry % CALC_SCALE);
    }

    CalcShowEntry(state);
}

/* Takes back the last digit typed, or the point where it was the last. */
static void CalcBackspace(CalcState *state)
{
    if (!state->entering)
    {
        return;
    }

    if (state->point && (state->decimals > 0U))
    {
        const int64_t place = CalcPlaceValue(state->decimals);

        state->entry -= ((state->entry / place) % 10) * place;
        --state->decimals;
    }
    else if (state->point)
    {
        state->point = false;
    }
    else
    {
        state->entry = ((state->entry / CALC_SCALE) / 10) * CALC_SCALE;
    }

    CalcShowEntry(state);
}

/*
 * Carries out the pending operation upon the accumulator and the number
 * typed, leaving the result in the accumulator and upon the display. False,
 * with the message shown, where it could not be carried out.
 */
static bool CalcResolve(CalcState *state)
{
    const char *fault = NULL;
    int64_t result = CalcEntryValue(state);

    if ((state->operation != 0) &&
        !CalcApply(state->operation, state->accumulator, CalcEntryValue(state), &result, &fault))
    {
        state->error = fault;
        CalcShowError(state);

        return false;
    }

    state->accumulator = result;
    CalcShowValue(state, result);

    if (state->error != NULL)
    {
        CalcShowError(state);

        return false;
    }

    return true;
}

/* An operator: the pending one resolved where a second number was typed, and
 * this one pending upon the result. Pressed twice, the second replaces the
 * first. */
static void CalcOperator(CalcState *state, char operation)
{
    if (state->entering || (state->operation == 0))
    {
        if (!state->entering)
        {
            state->entry = CalcMagnitude(state->accumulator);
            state->negative = state->accumulator < 0;
        }

        if (!CalcResolve(state))
        {
            return;
        }
    }

    state->operation = operation;
    state->entering = false;
}

/* `=`: the pending operation resolved, with the number typed; where none was
 * typed after the operator, with the result itself, as `5 + =` gives 10. */
static void CalcEquals(CalcState *state)
{
    if (state->operation == 0)
    {
        return;
    }

    if (!state->entering)
    {
        state->entry = CalcMagnitude(state->accumulator);
        state->negative = state->accumulator < 0;
    }

    if (CalcResolve(state))
    {
        state->operation = 0;
        state->entering = false;
    }
}

/* Changes the sign of the number typed, or of the result shown. */
static void CalcNegate(CalcState *state)
{
    if (state->entering)
    {
        state->negative = !state->negative && (state->entry != 0);
        CalcShowEntry(state);
    }
    else
    {
        state->accumulator = -state->accumulator;
        CalcShowValue(state, state->accumulator);
    }
}

/* Divides the number shown by a hundred, which then stands as typed. */
static void CalcPercent(CalcState *state)
{
    const char *fault = NULL;
    int64_t value = state->entering ? CalcEntryValue(state) : state->accumulator;

    if (!CalcApply('/', value, 100 * CALC_SCALE, &value, &fault))
    {
        state->error = fault;
        CalcShowError(state);

        return;
    }

    state->entry = (int64_t)CalcMagnitude(value);
    state->negative = value < 0;
    state->entering = true;
    state->point = (state->entry % CALC_SCALE) != 0;
    state->decimals = 0U;

    /* Shown as the value it is, with its places counted, so that a digit
     * typed after it is taken as the next place. */
    for (int64_t rest = state->entry % CALC_SCALE; rest != 0;
         rest = rest % CalcPlaceValue(state->decimals))
    {
        ++state->decimals;
    }

    CalcShowEntry(state);
}

void CalcKey(CalcState *state, char key)
{
    /* After a message, any key begins again, and a digit is then typed. */
    if (state->error != NULL)
    {
        CalcReset(state);

        if ((key == 'C') || (key == 'b'))
        {
            return;
        }
    }

    if ((key >= '0') && (key <= '9'))
    {
        CalcDigit(state, key - '0');
    }
    else if (key == '.')
    {
        CalcBeginEntry(state);
        state->point = true;
        CalcShowEntry(state);
    }
    else if ((key == '+') || (key == '-') || (key == '*') || (key == '/'))
    {
        CalcOperator(state, key);
    }
    else if (key == '=')
    {
        CalcEquals(state);
    }
    else if (key == 'C')
    {
        CalcReset(state);
    }
    else if (key == 'b')
    {
        CalcBackspace(state);
    }
    else if (key == 'n')
    {
        CalcNegate(state);
    }
    else if (key == '%')
    {
        CalcPercent(state);
    }
}

const char *CalcDisplay(const CalcState *state)
{
    return state->display;
}
