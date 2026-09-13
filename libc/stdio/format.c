/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/stdio/format.c
 * Purpose: The formatted output of ISO/IEC 9899:2011, Section 7.21.6 — one
 *          engine that reads a format string and produces characters, and the
 *          eight standard names above it, which differ only in where the
 *          characters go.
 * Key functions: vfprintf, fprintf, vprintf, printf, vsnprintf, snprintf,
 *          vsprintf, sprintf.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.21.6.1: the fprintf specification — the
 *     flags of paragraph 6, the field width and precision of paragraphs 4 and
 *     5, the length modifiers of paragraph 7 and the conversion specifiers of
 *     paragraph 8. Each is cited again at the code that implements it.
 *   - ISO/IEC 9899:2011, Section 7.21.6.5, paragraph 2: snprintf's return value
 *     is the number of characters that *would* have been written, which is why
 *     the engine counts what it produces rather than what it stored.
 *   - ISO/IEC 9899:2011, Section 7.20.1.5: intmax_t and uintmax_t, which every
 *     integer argument is widened to before it is converted.
 *   - libc/stdio/internal.h: the two counters this file keeps in the census
 *     libc/stdio/stream.c owns.
 *   - docs/design/LIBC.md, Section 10.4: the conversions implemented, those
 *     refused, and why a refusal is reported rather than ignored.
 *
 * One engine, eight names.
 *
 *   Every function in this file is FormatEngine with a different destination.
 *   The alternative — a conversion loop for streams and a second one for arrays
 *   — is how a library comes to format `%#o` correctly in printf and incorrectly
 *   in snprintf, and the defect is invisible because nobody tests both. The
 *   destination is one function pointer and one context, and the engine touches
 *   neither except to hand it a character.
 *
 * The engine counts what it produced, not what was stored.
 *
 *   That is snprintf's return value, and it is also fprintf's: a stream stores
 *   everything it is given or fails, so for a stream the two numbers are the
 *   same. Writing it the other way — counting stored characters — would make
 *   snprintf return the truncated length, and a caller sizing an array by
 *   calling with a size of zero would be told it needs nothing.
 *
 * Nothing here forms a floating-point value, and nothing here can.
 *
 *   PROJECT_GUIDELINES.md, Section 8, prohibits floating-point arithmetic
 *   without a justification, and this translation unit is compiled -mno-sse and
 *   -mno-80387 besides, so a conversion that named a double would not assemble.
 *   The eight floating conversions are therefore refused at the point where they
 *   are recognised, and the refusal is a reported failure rather than a silent
 *   omission; see FormatEngine.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>

#include "internal.h"

/*
 * Where a converted character goes.
 *
 * It returns false where the character could not be accepted, which for a stream
 * is a write error and for a bounded array is never: an array that is full has
 * not failed, it has merely stopped storing, and snprintf must keep counting.
 * That difference is the whole reason this is a predicate and not a void
 * function.
 */
typedef bool (*FormatEmit)(void *context, char byte);

/*
 * The greatest number of digits a conversion can produce, and the room the
 * assembly of one needs.
 *
 * The widest is octal: a 64-bit value in base 8 is 22 digits. The prefix of the
 * `#` flag adds two and a sign adds one, so 32 is comfortably above every case
 * and is a power of two. It is stated as a number with this reasoning beside it
 * because a buffer sized by guess is how a formatter comes to write past its own
 * array for one conversion in a thousand.
 */
#define FORMAT_DIGITS_MAXIMUM 32U

/* The flags of 7.21.6.1, paragraph 6. */
typedef struct FormatFlags
{
    bool left;      /* `-`: the result is left-justified within its field. */
    bool sign;      /* `+`: a signed conversion always begins with a sign. */
    bool space;     /* ` `: and with a space where it would begin with neither. */
    bool alternate; /* `#`: the alternative form. */
    bool zero;      /* `0`: the field is padded with zeroes rather than spaces. */
} FormatFlags;

/* The length modifiers of 7.21.6.1, paragraph 7, as this library accepts them.
 * `L` is absent: it applies only to the floating conversions, which are
 * refused. */
typedef enum FormatLength
{
    FORMAT_LENGTH_INT,
    FORMAT_LENGTH_CHAR,      /* hh */
    FORMAT_LENGTH_SHORT,     /* h  */
    FORMAT_LENGTH_LONG,      /* l  */
    FORMAT_LENGTH_LONG_LONG, /* ll */
    FORMAT_LENGTH_SIZE,      /* z  */
    FORMAT_LENGTH_MAXIMUM,   /* j  */
    FORMAT_LENGTH_POINTER    /* t  */
} FormatLength;

/* --------------------------------------------------------------- the sinks */

/* A stream: every character goes through fputc, so the buffering policy of
 * libc/stdio/stream.c decides when anything leaves. */
static bool FormatEmitToStream(void *context, char byte)
{
    FILE *const stream = (FILE *)context;

    return fputc((int)(unsigned char)byte, stream) != EOF;
}

/*
 * A bounded array.
 *
 * `capacity` is one less than the size the caller gave, the last byte being
 * reserved for the terminator; a size of zero leaves a capacity of zero and
 * stores nothing at all, which is what makes `snprintf(NULL, 0, ...)` a legal
 * way to measure a result.
 */
typedef struct FormatArray
{
    char *buffer;
    size_t capacity;
    size_t used;
} FormatArray;

static bool FormatEmitToArray(void *context, char byte)
{
    FormatArray *const array = (FormatArray *)context;

    if (array->used < array->capacity)
    {
        array->buffer[array->used] = byte;
        ++array->used;
    }

    /*
     * True even when nothing was stored. A full array is not an error: 7.21.6.5
     * requires the count to be what would have been written, so the engine must
     * keep converting after the array has stopped accepting.
     */
    return true;
}

/* ---------------------------------------------------------- the conversions */

/* Emits one character and advances the count, or reports that the destination
 * refused it. */
static bool FormatPut(FormatEmit emit, void *context, char byte, int *produced)
{
    if (!emit(context, byte))
    {
        return false;
    }

    ++(*produced);

    return true;
}

/* Emits `count` copies of a character, which is what every padding decision
 * below reduces to. */
static bool FormatPad(FormatEmit emit, void *context, char byte, int count,
                      int *produced)
{
    for (int index = 0; index < count; ++index)
    {
        if (!FormatPut(emit, context, byte, produced))
        {
            return false;
        }
    }

    return true;
}

/*
 * Converts `magnitude` to `base`, writing the digits into `digits` in reverse,
 * and returns how many there are.
 *
 * A value of zero produces one digit and not none. The caller suppresses it
 * where the precision is zero, which 7.21.6.1, paragraph 8, requires for the
 * integer conversions — "the result of converting a zero value with a precision
 * of zero is no characters" — and that is the only case in which it is
 * suppressed, so the decision belongs to the caller rather than here.
 */
static size_t FormatDigits(uintmax_t magnitude, unsigned int base, bool upper,
                           char *digits)
{
    static const char lower_alphabet[] = "0123456789abcdef";
    static const char upper_alphabet[] = "0123456789ABCDEF";

    const char *const alphabet = upper ? upper_alphabet : lower_alphabet;
    size_t count = 0U;

    do
    {
        digits[count] = alphabet[magnitude % (uintmax_t)base];
        ++count;
        magnitude /= (uintmax_t)base;
    }
    while ((magnitude != 0U) && (count < FORMAT_DIGITS_MAXIMUM));

    return count;
}

/*
 * Writes an integer conversion: the sign or the prefix, the zero padding the
 * precision asks for, the digits, and the field padding — in whichever of the
 * two orders the `-` flag selects.
 *
 * This is the one function in the file with a real amount of arithmetic in it,
 * and the order of the three paddings is what that arithmetic is about. The
 * precision pads with zeroes *inside* the sign and the prefix; the `0` flag pads
 * with zeroes inside them too, but only to the field width and only when the
 * result is not left-justified and no precision was given; and the field pads
 * with spaces outside everything. A formatter that conflates the second and the
 * third prints `-0042` as `0-042`, which is the classic way to get this wrong.
 */
static bool FormatInteger(FormatEmit emit, void *context, uintmax_t magnitude,
                          unsigned int base, bool upper, bool negative,
                          const FormatFlags *flags, int width, int precision,
                          int *produced)
{
    char digits[FORMAT_DIGITS_MAXIMUM];
    char prefix[2];
    size_t prefix_length = 0U;
    size_t count;
    int zeroes;
    int spaces;
    int body;

    count = FormatDigits(magnitude, base, upper, digits);

    /* 7.21.6.1, paragraph 8: a zero value with a precision of zero converts to
     * no characters at all. */
    if ((precision == 0) && (magnitude == 0U))
    {
        count = 0U;
    }

    if (negative)
    {
        prefix[0] = '-';
        prefix_length = 1U;
    }
    else if (flags->sign)
    {
        prefix[0] = '+';
        prefix_length = 1U;
    }
    else if (flags->space)
    {
        prefix[0] = ' ';
        prefix_length = 1U;
    }
    else
    {
        prefix_length = 0U;
    }

    /*
     * 7.21.6.1, paragraph 6, the `#` flag. For `o` it increases the precision so
     * that the first digit is a zero — which is done below by counting it as a
     * precision digit rather than as a prefix, the standard's own wording. For
     * `x` and `X` a non-zero result is prefixed with `0x` or `0X`; a zero result
     * is not, because `0x0` would be a prefix upon nothing.
     */
    if (flags->alternate && (base == 16U) && (magnitude != 0U))
    {
        prefix[0] = '0';
        prefix[1] = upper ? 'X' : 'x';
        prefix_length = 2U;
    }

    zeroes = 0;

    if (precision > (int)count)
    {
        zeroes = precision - (int)count;
    }

    if (flags->alternate && (base == 8U) &&
        ((zeroes == 0) && ((count == 0U) || (digits[count - 1U] != '0'))))
    {
        zeroes = 1;
    }

    body = (int)prefix_length + zeroes + (int)count;

    /*
     * The `0` flag is ignored where `-` is present or where a precision was
     * given for an integer conversion, which 7.21.6.1, paragraph 6, states
     * twice. Where it applies, the field padding becomes precision padding, so
     * it is moved from `spaces` to `zeroes` rather than emitted separately.
     */
    spaces = (width > body) ? (width - body) : 0;

    if (flags->zero && !flags->left && (precision < 0) && (spaces > 0))
    {
        zeroes += spaces;
        spaces = 0;
    }

    if (!flags->left && !FormatPad(emit, context, ' ', spaces, produced))
    {
        return false;
    }

    for (size_t index = 0U; index < prefix_length; ++index)
    {
        if (!FormatPut(emit, context, prefix[index], produced))
        {
            return false;
        }
    }

    if (!FormatPad(emit, context, '0', zeroes, produced))
    {
        return false;
    }

    while (count > 0U)
    {
        --count;

        if (!FormatPut(emit, context, digits[count], produced))
        {
            return false;
        }
    }

    return !flags->left || FormatPad(emit, context, ' ', spaces, produced);
}

/* Writes a string conversion: the characters, bounded by the precision where one
 * was given, within a field padded upon whichever side the `-` flag selects. */
static bool FormatString(FormatEmit emit, void *context, const char *string,
                         const FormatFlags *flags, int width, int precision,
                         int *produced)
{
    size_t length = 0U;
    int spaces;

    /*
     * A null pointer is undefined behaviour under 7.21.6.1, paragraph 8, and is
     * given a defined result here: the six characters `(null)`.
     *
     * The alternative is to dereference it and fault, in the middle of what is
     * almost always a diagnostic — so the program dies in the act of reporting
     * why it was going to die, and the report is lost. A visible marker in the
     * output is a worse thing to print and a far better thing to debug.
     */
    if (string == NULL)
    {
        string = "(null)";
    }

    while (string[length] != '\0')
    {
        if ((precision >= 0) && (length >= (size_t)precision))
        {
            break;
        }

        ++length;
    }

    spaces = (width > (int)length) ? (width - (int)length) : 0;

    if (!flags->left && !FormatPad(emit, context, ' ', spaces, produced))
    {
        return false;
    }

    for (size_t index = 0U; index < length; ++index)
    {
        if (!FormatPut(emit, context, string[index], produced))
        {
            return false;
        }
    }

    return !flags->left || FormatPad(emit, context, ' ', spaces, produced);
}

/* Reads a decimal digit string from the format, advancing the cursor past it.
 * Used for both the field width and the precision, which have the same syntax. */
static int FormatNumber(const char *format, size_t *at)
{
    int value = 0;

    while ((format[*at] >= '0') && (format[*at] <= '9'))
    {
        const int digit = (int)(format[*at] - '0');

        /*
         * A width or precision beyond this is refused by clamping rather than by
         * overflowing. Nothing in this system has a field ten million characters
         * wide, and an unclamped accumulation of a digit string a caller
         * supplied is signed overflow, which is undefined behaviour and is
         * exactly what PROJECT_GUIDELINES.md, Section 8, forbids relying upon.
         */
        if (value <= ((int)INT16_MAX / 10))
        {
            value = (value * 10) + digit;
        }

        ++(*at);
    }

    return value;
}

/* Widens an integer argument to intmax_t according to the length modifier.
 * Every one of the narrow types has already been promoted to int by the
 * variadic call, so the narrow cases are a conversion back rather than a
 * different va_arg type — which is the detail a formatter gets wrong by reading
 * `hh` as `va_arg(arguments, char)`. */
static intmax_t FormatSigned(va_list *arguments, FormatLength length)
{
    switch (length)
    {
    case FORMAT_LENGTH_CHAR:
        return (intmax_t)(signed char)va_arg(*arguments, int);
    case FORMAT_LENGTH_SHORT:
        return (intmax_t)(short)va_arg(*arguments, int);
    case FORMAT_LENGTH_LONG:
        return (intmax_t)va_arg(*arguments, long);
    case FORMAT_LENGTH_LONG_LONG:
        return (intmax_t)va_arg(*arguments, long long);
    case FORMAT_LENGTH_SIZE:
        return (intmax_t)va_arg(*arguments, size_t);
    case FORMAT_LENGTH_MAXIMUM:
        return va_arg(*arguments, intmax_t);
    case FORMAT_LENGTH_POINTER:
        return (intmax_t)va_arg(*arguments, ptrdiff_t);
    case FORMAT_LENGTH_INT:
    default:
        return (intmax_t)va_arg(*arguments, int);
    }
}

static uintmax_t FormatUnsigned(va_list *arguments, FormatLength length)
{
    switch (length)
    {
    case FORMAT_LENGTH_CHAR:
        return (uintmax_t)(unsigned char)va_arg(*arguments, unsigned int);
    case FORMAT_LENGTH_SHORT:
        return (uintmax_t)(unsigned short)va_arg(*arguments, unsigned int);
    case FORMAT_LENGTH_LONG:
        return (uintmax_t)va_arg(*arguments, unsigned long);
    case FORMAT_LENGTH_LONG_LONG:
        return (uintmax_t)va_arg(*arguments, unsigned long long);
    case FORMAT_LENGTH_SIZE:
        return (uintmax_t)va_arg(*arguments, size_t);
    case FORMAT_LENGTH_MAXIMUM:
        return va_arg(*arguments, uintmax_t);
    case FORMAT_LENGTH_POINTER:
        return (uintmax_t)va_arg(*arguments, size_t);
    case FORMAT_LENGTH_INT:
    default:
        return (uintmax_t)va_arg(*arguments, unsigned int);
    }
}

/*
 * The magnitude of a signed value, without forming its negation.
 *
 * `-value` is undefined behaviour when the value is the most negative
 * representable one, which is the single input every hand-written formatter
 * gets wrong — and it gets it wrong in the direction that produces a plausible
 * answer on most machines, so it is never noticed. Subtracting one before
 * negating and adding it back in the unsigned domain keeps every step in range.
 */
static uintmax_t FormatMagnitude(intmax_t value)
{
    if (value < 0)
    {
        return (uintmax_t)(-(value + 1)) + (uintmax_t)1;
    }

    return (uintmax_t)value;
}

/* ----------------------------------------------------------- the engine */

/*
 * Reads the format string and produces characters, returning how many it
 * produced or a negative value.
 *
 * A negative result means one of two things and the caller cannot tell which,
 * which is what ISO/IEC 9899:2011, Section 7.21.6.1, paragraph 3, permits: the
 * destination refused a character, or the format named a conversion this library
 * does not implement. Both are failures of the call and neither leaves a count
 * worth reporting.
 */
static int FormatRun(FormatEmit emit, void *context, const char *format,
                     va_list *arguments)
{
    int produced = 0;
    size_t at = 0U;

    if (format == NULL)
    {
        OxysStreamCountRejection();

        return -1;
    }

    while (format[at] != '\0')
    {
        FormatFlags flags = { false, false, false, false, false };
        FormatLength length = FORMAT_LENGTH_INT;
        int width = 0;
        int precision = -1;
        bool parsing = true;
        char conversion;

        if (format[at] != '%')
        {
            if (!FormatPut(emit, context, format[at], &produced))
            {
                return -1;
            }

            ++at;
            continue;
        }

        ++at;

        /* 7.21.6.1, paragraph 6: zero or more flags, in any order. */
        while (parsing)
        {
            switch (format[at])
            {
            case '-': flags.left = true; ++at; break;
            case '+': flags.sign = true; ++at; break;
            case ' ': flags.space = true; ++at; break;
            case '#': flags.alternate = true; ++at; break;
            case '0': flags.zero = true; ++at; break;
            default:  parsing = false; break;
            }
        }

        /* 7.21.6.1, paragraph 4: the minimum field width, which may be an
         * asterisk taking an int argument. A negative argument is a `-` flag
         * with a positive width, which the paragraph states explicitly. */
        if (format[at] == '*')
        {
            const int given = va_arg(*arguments, int);

            ++at;

            if (given < 0)
            {
                flags.left = true;
                width = (given == INT32_MIN) ? INT16_MAX : -given;
            }
            else
            {
                width = given;
            }
        }
        else
        {
            width = FormatNumber(format, &at);
        }

        /* 7.21.6.1, paragraph 5: the precision, introduced by a period. A
         * negative asterisk argument is as if no precision were given, and a
         * period with no digits is a precision of zero. */
        if (format[at] == '.')
        {
            ++at;

            if (format[at] == '*')
            {
                const int given = va_arg(*arguments, int);

                ++at;
                precision = (given < 0) ? -1 : given;
            }
            else
            {
                precision = FormatNumber(format, &at);
            }
        }

        /* 7.21.6.1, paragraph 7: the length modifier. */
        switch (format[at])
        {
        case 'h':
            ++at;

            if (format[at] == 'h')
            {
                ++at;
                length = FORMAT_LENGTH_CHAR;
            }
            else
            {
                length = FORMAT_LENGTH_SHORT;
            }

            break;
        case 'l':
            ++at;

            if (format[at] == 'l')
            {
                ++at;
                length = FORMAT_LENGTH_LONG_LONG;
            }
            else
            {
                length = FORMAT_LENGTH_LONG;
            }

            break;
        case 'z': ++at; length = FORMAT_LENGTH_SIZE; break;
        case 'j': ++at; length = FORMAT_LENGTH_MAXIMUM; break;
        case 't': ++at; length = FORMAT_LENGTH_POINTER; break;
        default: break;
        }

        conversion = format[at];

        if (conversion != '\0')
        {
            ++at;
        }

        switch (conversion)
        {
        case 'd':
        case 'i':
        {
            const intmax_t value = FormatSigned(arguments, length);

            OxysStreamCountConversion();

            if (!FormatInteger(emit, context, FormatMagnitude(value), 10U, false,
                               value < 0, &flags, width, precision, &produced))
            {
                return -1;
            }

            break;
        }
        case 'o':
        case 'u':
        case 'x':
        case 'X':
        {
            const uintmax_t value = FormatUnsigned(arguments, length);
            const unsigned int base = (conversion == 'o') ? 8U
                                    : ((conversion == 'u') ? 10U : 16U);

            OxysStreamCountConversion();

            /*
             * The sign flags are dropped for an unsigned conversion. 7.21.6.1,
             * paragraph 6, gives `+` and ` ` meaning only for a signed
             * conversion, and a formatter that let them through prints `+42` for
             * `%+u`, which no standard library does and which a caller
             * formatting a field width would find one character wider than it
             * asked for.
             */
            flags.sign = false;
            flags.space = false;

            if (!FormatInteger(emit, context, value, base, conversion == 'X',
                               false, &flags, width, precision, &produced))
            {
                return -1;
            }

            break;
        }
        case 'c':
        {
            const char byte = (char)(unsigned char)va_arg(*arguments, int);
            const int spaces = (width > 1) ? (width - 1) : 0;

            /*
             * `%lc` is a wide character and is refused above with the other wide
             * conversions; this is the narrow one. The argument is an int by the
             * default argument promotions, converted to unsigned char, which is
             * exactly what paragraph 8 requires.
             */
            OxysStreamCountConversion();

            if (!flags.left && !FormatPad(emit, context, ' ', spaces, &produced))
            {
                return -1;
            }

            if (!FormatPut(emit, context, byte, &produced))
            {
                return -1;
            }

            if (flags.left && !FormatPad(emit, context, ' ', spaces, &produced))
            {
                return -1;
            }

            break;
        }
        case 's':
        {
            const char *const string = va_arg(*arguments, const char *);

            OxysStreamCountConversion();

            if (!FormatString(emit, context, string, &flags, width, precision,
                              &produced))
            {
                return -1;
            }

            break;
        }
        case 'p':
        {
            const void *const pointer = va_arg(*arguments, const void *);
            FormatFlags pointer_flags = { flags.left, false, false, true, false };

            /*
             * 7.21.6.1, paragraph 8, makes the form implementation-defined. It
             * is `0x` and lowercase hexadecimal here, which is what every
             * toolchain upon this architecture produces and therefore what a
             * reader comparing a log against a disassembly expects.
             *
             * A null pointer is `0x0` and not the word some libraries print:
             * `(nil)` cannot be compared against an address, and every address
             * in this system's logs is compared against something. It is written
             * by the string conversion rather than by the integer one because
             * the `#` flag deliberately suppresses the prefix upon a zero value
             * — `0x0` would otherwise be a prefix upon nothing — and this is the
             * one case where the prefix is wanted anyway.
             */
            OxysStreamCountConversion();

            if (pointer == NULL)
            {
                if (!FormatString(emit, context, "0x0", &flags, width, -1,
                                  &produced))
                {
                    return -1;
                }

                break;
            }

            if (!FormatInteger(emit, context, (uintmax_t)(uintptr_t)pointer, 16U,
                               false, false, &pointer_flags, width, 1, &produced))
            {
                return -1;
            }

            break;
        }
        case '%':
            OxysStreamCountConversion();

            if (!FormatPut(emit, context, '%', &produced))
            {
                return -1;
            }

            break;
        default:
            /*
             * Every conversion this library does not implement arrives here: the
             * eight floating ones, `n`, `C`, `S`, a `%` at the very end of the
             * string, and anything a caller mistyped.
             *
             * 7.21.6.1, paragraph 9, makes an undefined conversion specification
             * undefined behaviour, so any answer conforms. This one refuses and
             * says so, because the alternative — carry on and produce something
             * — returns a count that is wrong in a way nothing checks, and the
             * caller of a `%n` in particular is entitled to know that the write
             * it asked for did not happen. See the head of <stdio.h>.
             */
            OxysStreamCountRejection();

            return -1;
        }
    }

    return produced;
}

/*
 * The engine, as its callers see it: a va_list by value, which is what a
 * variadic function has to hand.
 *
 * The copy is not a formality. ISO/IEC 9899:2011, Section 7.16.1, paragraph 1,
 * makes va_list an object type which may be an array type, and it is one upon
 * this architecture — so a va_list parameter has already decayed to a pointer
 * and the address of it is the address of that pointer rather than of the list.
 * FormatRun must consume arguments in a way the standard defines for a callee,
 * and va_copy is the operation the standard provides for exactly this. The
 * matching va_end is required by paragraph 3 of Section 7.16.1.2 whether or not
 * it does anything upon a given machine.
 */
static int FormatEngine(FormatEmit emit, void *context, const char *format,
                        va_list incoming)
{
    va_list arguments;
    int produced;

    va_copy(arguments, incoming);
    produced = FormatRun(emit, context, format, &arguments);
    va_end(arguments);

    return produced;
}

/* -------------------------------------------------------- 7.21.6, the names */

int vfprintf(FILE *stream, const char *format, va_list arguments)
{
    if (stream == NULL)
    {
        return -1;
    }

    return FormatEngine(FormatEmitToStream, stream, format, arguments);
}

int fprintf(FILE *stream, const char *format, ...)
{
    va_list arguments;
    int produced;

    va_start(arguments, format);
    produced = vfprintf(stream, format, arguments);
    va_end(arguments);

    return produced;
}

int vprintf(const char *format, va_list arguments)
{
    return vfprintf(stdout, format, arguments);
}

int printf(const char *format, ...)
{
    va_list arguments;
    int produced;

    va_start(arguments, format);
    produced = vfprintf(stdout, format, arguments);
    va_end(arguments);

    return produced;
}

int vsnprintf(char *buffer, size_t size, const char *format, va_list arguments)
{
    FormatArray array;
    int produced;

    if ((buffer == NULL) && (size != 0U))
    {
        return -1;
    }

    array.buffer = buffer;
    array.capacity = (size == 0U) ? 0U : (size - 1U);
    array.used = 0U;

    produced = FormatEngine(FormatEmitToArray, &array, format, arguments);

    /*
     * 7.21.6.5, paragraph 2: a null character is written at the end of what was
     * stored, unless the size was zero — in which case nothing is written and
     * the array may be a null pointer.
     *
     * It is written even where the engine failed. A caller that ignores a
     * negative result and reads the array would otherwise read whatever was
     * there before, which is the difference between a truncated string and an
     * unterminated one.
     */
    if (size != 0U)
    {
        buffer[array.used] = '\0';
    }

    return produced;
}

int snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list arguments;
    int produced;

    va_start(arguments, format);
    produced = vsnprintf(buffer, size, format, arguments);
    va_end(arguments);

    return produced;
}

int vsprintf(char *buffer, const char *format, va_list arguments)
{
    /*
     * The bound given is the greatest a size_t can hold, which is no bound: this
     * function has none, and pretending otherwise here would be a protection a
     * caller might rely upon. <stdio.h> records why it exists at all.
     */
    return vsnprintf(buffer, SIZE_MAX, format, arguments);
}

int sprintf(char *buffer, const char *format, ...)
{
    va_list arguments;
    int produced;

    va_start(arguments, format);
    produced = vsprintf(buffer, format, arguments);
    va_end(arguments);

    return produced;
}
