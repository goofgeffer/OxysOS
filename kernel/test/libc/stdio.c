/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/stdio.c
 * Purpose: Asserts the work of sub-task 7.4 — the buffered stream and the
 *          formatted conversion — against the code the C library ships, by
 *          giving it streams whose device is memory rather than a descriptor.
 * Key functions: KernelVerifyStdio.
 * References:
 *   - docs/design/LIBC.md: the table pairing every property
 *     asserted below with the silent failure that assertion exists to catch.
 *   - ISO/IEC 9899:2011, Section 7.21: the behaviour asserted, cited at each
 *     group of assertions.
 *   - libc/include/stream.h: OxysStreamOpenMemoryWrite and
 *     OxysStreamOpenMemoryRead, which are how a stream is exercised without a
 *     system call, and the census this test ends by examining.
 *
 * **Nothing here may write to stdout or stderr, and nothing here may flush
 * them.**
 *
 *   Either would reach OxysStreamWrite, which executes SYSCALL, which this
 *   kernel cannot survive: SYSRET returns to privilege level 3 unconditionally,
 *   so the kernel would leave its own entry path as a user program. That is the
 *   same hazard kernel/test/libc/heap.c carries with OxysHeapExtend and
 *   kernel/test/libc/wrappers.c with every wrapper in the image.
 *
 *   It is guarded the same way: the last assertion this file makes is that the
 *   census records no transfer at all, so a change that made one is reported
 *   rather than suffered. The three standard streams are examined — their
 *   descriptors and their buffering — and never written to or read.
 *
 *   Reading stdin was safe and was done, until sub-task 8.1. OxysStreamFill was
 *   ordinary C that returned zero, this kernel having no call that reads, and
 *   the assertion that stdin was permanently at end-of-file was one the kernel
 *   could make itself. Since 8.1 that function reads the terminal through
 *   SYSCALL, so stdin is now as untouchable here as stdout, and the last
 *   assertion covers both seams.
 *
 * Why the shipped transfers are not asserted here.
 *
 *   They cannot be, for the reason above, and they are not left unasserted: they
 *   are the subject of the program sub-task 7.5 builds and runs at privilege
 *   level 3, which calls printf and has its output arrive upon the serial
 *   channel. The gap between the two halves of this sub-task is therefore one
 *   sub-task wide and not one phase wide, which is why 7.5 follows immediately.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <stdio.h>
#include <stream.h>
#include <string.h>

static bool VerifyStdioSucceeded;

static void VerifyStdioRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyStdioSucceeded = false;
    }
}

/*
 * The region every memory stream in this file writes into, and the sentinel that
 * surrounds it.
 *
 * The reasoning is the one kernel/test/libc/string.c records: a stream is given
 * a region in the middle of a larger array, and the margin either side is
 * asserted to be untouched. A stream that wrote one byte past the end of the
 * region it was given would otherwise be invisible, the bytes beyond it being
 * whatever .bss holds.
 *
 * The sentinel is 0x5A for the same reason it is there: it is not a value any
 * correct operation writes, whereas a zero or a space is.
 */
#define VERIFY_STDIO_CAPACITY 256U
#define VERIFY_STDIO_MARGIN    16U
#define VERIFY_STDIO_SENTINEL  0x5A

static char VerifyStdioArea[VERIFY_STDIO_CAPACITY];

/*
 * The buffer given to setvbuf where the assertion is about a buffer of one byte,
 * and the sentinel that stands immediately after it.
 *
 * It is an array of its own rather than a byte borrowed from the sentinel margin
 * of VerifyStdioArea, so that the margin assertions keep meaning what they say.
 *
 * **The second byte is the whole point of it.** A stream that empties its buffer
 * *after* appending rather than before writes one byte past a buffer of one, and
 * the bytes it delivers are the same bytes in the same order — so every
 * assertion about what arrived at the device passes, and the only trace is the
 * byte beyond the array. The negative test that removed the guard reported
 * nothing at all until this sentinel was added.
 */
static char VerifyStdioOneByte[2];

/* Fills the array with the sentinel and returns the region a stream is given. */
static char *VerifyStdioPrepare(void)
{
    for (size_t index = 0U; index < VERIFY_STDIO_CAPACITY; ++index)
    {
        VerifyStdioArea[index] = (char)VERIFY_STDIO_SENTINEL;
    }

    return &VerifyStdioArea[VERIFY_STDIO_MARGIN];
}

/* Whether the margin either side of a region of `used` bytes still holds the
 * sentinel. */
static bool VerifyStdioMarginIsIntact(size_t used)
{
    for (size_t index = 0U; index < VERIFY_STDIO_MARGIN; ++index)
    {
        if (VerifyStdioArea[index] != (char)VERIFY_STDIO_SENTINEL)
        {
            return false;
        }
    }

    for (size_t index = VERIFY_STDIO_MARGIN + used;
         index < VERIFY_STDIO_CAPACITY;
         ++index)
    {
        if (VerifyStdioArea[index] != (char)VERIFY_STDIO_SENTINEL)
        {
            return false;
        }
    }

    return true;
}

/* Whether a stream has delivered exactly these bytes to its region. The length
 * is compared before the contents, so that a stream which delivered a prefix of
 * what was expected is reported as the wrong length rather than as the right
 * one. */
static bool VerifyStdioDelivered(FILE *stream, const char *region,
                                 const char *expected)
{
    const size_t length = strlen(expected);

    if (OxysStreamDelivered(stream) != length)
    {
        return false;
    }

    return memcmp(region, expected, length) == 0;
}

/* ------------------------------------------------------- 7.21.3: buffering */

/*
 * The three buffering modes, and the moment at which each empties its buffer.
 *
 * This is the property the whole sub-task rests upon and the one with no visible
 * symptom when it is wrong: a stream that flushes too eagerly is merely slow,
 * and a stream that flushes too late loses output only when the program ends
 * unexpectedly — which is the run nobody is watching.
 */
static void VerifyStdioBuffering(void)
{
    char *region;
    FILE *stream;

    /* Fully buffered: nothing reaches the device until the buffer fills or the
     * stream is flushed. A newline changes nothing, which is the difference from
     * line buffering and is asserted rather than assumed. */
    region = VerifyStdioPrepare();
    stream = OxysStreamOpenMemoryWrite(region, 64U);
    VerifyStdioRequire(stream != NULL, "a fully buffered memory stream could not be opened");
    VerifyStdioRequire(setvbuf(stream, NULL, _IOFBF, 0U) == 0,
                       "setvbuf refused a mode change upon an untouched stream");
    (void)fputs("alpha\n", stream);
    VerifyStdioRequire(OxysStreamDelivered(stream) == 0U,
                       "a fully buffered stream delivered a line before it was flushed");
    VerifyStdioRequire(fflush(stream) == 0, "flushing a memory stream failed");
    VerifyStdioRequire(VerifyStdioDelivered(stream, region, "alpha\n"),
                       "a fully buffered stream did not deliver what was written to it");
    VerifyStdioRequire(VerifyStdioMarginIsIntact(64U),
                       "a stream wrote outside the region it was given");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "a memory stream could not be closed");

    /* Line buffered: the buffer is emptied by the newline and by nothing before
     * it. The assertion is made either side of the newline, because a stream
     * that flushed upon every character would pass an assertion made only
     * afterwards. */
    region = VerifyStdioPrepare();
    stream = OxysStreamOpenMemoryWrite(region, 64U);
    VerifyStdioRequire(setvbuf(stream, NULL, _IOLBF, 0U) == 0,
                       "setvbuf refused _IOLBF upon an untouched stream");
    (void)fputs("beta", stream);
    VerifyStdioRequire(OxysStreamDelivered(stream) == 0U,
                       "a line buffered stream delivered a partial line");
    (void)fputc('\n', stream);
    VerifyStdioRequire(VerifyStdioDelivered(stream, region, "beta\n"),
                       "a line buffered stream did not deliver upon the newline");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "a line buffered stream could not be closed");

    /* Unbuffered: every byte reaches the device as it is written, which is what
     * makes stderr useful in a program that is about to fault. */
    region = VerifyStdioPrepare();
    stream = OxysStreamOpenMemoryWrite(region, 64U);
    VerifyStdioRequire(setvbuf(stream, NULL, _IONBF, 0U) == 0,
                       "setvbuf refused _IONBF upon an untouched stream");
    (void)fputc('g', stream);
    VerifyStdioRequire(VerifyStdioDelivered(stream, region, "g"),
                       "an unbuffered stream did not deliver a byte immediately");
    (void)fputs("amma", stream);
    VerifyStdioRequire(VerifyStdioDelivered(stream, region, "gamma"),
                       "an unbuffered stream did not deliver every byte immediately");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "an unbuffered stream could not be closed");

    /*
     * A buffer that fills is emptied by the byte that would overflow it, and the
     * stream keeps working afterwards.
     *
     * The buffer is one byte, which is the size at which the difference between
     * "flush when full" and "flush after appending" is a write past the end of
     * the array. setvbuf accepts a size of one, so this is a reachable state.
     */
    region = VerifyStdioPrepare();
    stream = OxysStreamOpenMemoryWrite(region, 64U);
    VerifyStdioOneByte[0] = (char)VERIFY_STDIO_SENTINEL;
    VerifyStdioOneByte[1] = (char)VERIFY_STDIO_SENTINEL;
    VerifyStdioRequire(setvbuf(stream, VerifyStdioOneByte, _IOFBF, 1U) == 0,
                       "setvbuf refused a buffer of one byte");
    (void)fputs("xyz", stream);
    VerifyStdioRequire(OxysStreamDelivered(stream) == 2U,
                       "a one-byte buffer did not deliver each byte as the next arrived");
    VerifyStdioRequire(VerifyStdioOneByte[1] == (char)VERIFY_STDIO_SENTINEL,
                       "a stream wrote past the end of the buffer it was given");
    VerifyStdioRequire(fflush(stream) == 0, "the last byte of a one-byte buffer could not be flushed");
    VerifyStdioRequire(VerifyStdioDelivered(stream, region, "xyz"),
                       "a one-byte buffer lost a byte");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "a stream with a caller's buffer could not be closed");

    /*
     * setvbuf is refused once the stream has been used, and for a mode that is
     * not one of the three.
     *
     * The first is ISO/IEC 9899:2011, Section 7.21.5.6, paragraph 2, and is
     * refused rather than obeyed because obeying it discards whatever the buffer
     * already holds — silently, which is the worst way to lose output.
     */
    region = VerifyStdioPrepare();
    stream = OxysStreamOpenMemoryWrite(region, 64U);
    VerifyStdioRequire(setvbuf(stream, NULL, 99, 0U) != 0,
                       "setvbuf accepted a mode that is not one of the three");
    (void)fputc('q', stream);
    VerifyStdioRequire(setvbuf(stream, NULL, _IONBF, 0U) != 0,
                       "setvbuf accepted a call made after the stream had been used");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "a used stream could not be closed");
}

/* --------------------------------------------------- 7.21.7, 7.21.8: output */

static void VerifyStdioOutput(void)
{
    char *region;
    FILE *stream;

    /* puts writes a newline and fputs does not. It is the whole difference
     * between them and the commonest surprise in the pair; here it is asserted
     * upon fputs alone, stdout being unreachable from this kernel. */
    region = VerifyStdioPrepare();
    stream = OxysStreamOpenMemoryWrite(region, 64U);
    VerifyStdioRequire(fputs("delta", stream) >= 0, "fputs reported a failure");
    (void)fflush(stream);
    VerifyStdioRequire(VerifyStdioDelivered(stream, region, "delta"),
                       "fputs wrote a terminator or a newline it should not have");

    /* fwrite counts whole elements, and a partial element is not one. The region
     * is exhausted part way through an element, which is the only way to reach
     * that branch without a device that fails. */
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "the fputs stream could not be closed");

    region = VerifyStdioPrepare();
    stream = OxysStreamOpenMemoryWrite(region, 7U);
    VerifyStdioRequire(setvbuf(stream, NULL, _IONBF, 0U) == 0,
                       "setvbuf refused _IONBF for the element count assertion");
    VerifyStdioRequire(fwrite("aabbccdd", 2U, 4U, stream) == 3U,
                       "fwrite counted an element whose bytes went out only in part");
    VerifyStdioRequire(ferror(stream) != 0,
                       "fwrite did not set the error indicator when the device refused");
    VerifyStdioRequire(OxysStreamDelivered(stream) == 7U,
                       "a stream counted bytes its device would not take as delivered");
    VerifyStdioRequire(VerifyStdioMarginIsIntact(7U),
                       "fwrite wrote outside the region the stream was given");
    VerifyStdioRequire(OxysStreamClose(stream) != 0,
                       "closing a stream whose flush fails reported success");

    /* A size or a count of zero writes nothing and returns zero, which 7.21.8.2
     * paragraph 2 requires and which a loop written without the guard gets wrong
     * by writing one element. */
    region = VerifyStdioPrepare();
    stream = OxysStreamOpenMemoryWrite(region, 64U);
    VerifyStdioRequire(fwrite("zz", 0U, 4U, stream) == 0U, "fwrite of zero-sized elements wrote something");
    VerifyStdioRequire(fwrite("zz", 2U, 0U, stream) == 0U, "fwrite of no elements wrote something");
    (void)fflush(stream);
    VerifyStdioRequire(OxysStreamDelivered(stream) == 0U,
                       "fwrite of nothing delivered a byte");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "the zero-transfer stream could not be closed");

    /*
     * The error indicator is sticky and is cleared only by clearerr.
     *
     * That is the point of it: a program that checks once after a sequence of
     * writes must not be told everything was well because the last write
     * happened to succeed.
     */
    region = VerifyStdioPrepare();
    stream = OxysStreamOpenMemoryWrite(region, 2U);
    VerifyStdioRequire(setvbuf(stream, NULL, _IONBF, 0U) == 0,
                       "setvbuf refused _IONBF for the sticky-error assertion");
    (void)fputs("abcd", stream);
    VerifyStdioRequire(ferror(stream) != 0, "a refused write did not set the error indicator");
    (void)fputc('e', stream);
    VerifyStdioRequire(ferror(stream) != 0, "the error indicator was cleared by a later write");
    clearerr(stream);
    VerifyStdioRequire(ferror(stream) == 0, "clearerr did not clear the error indicator");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "the sticky-error stream could not be closed");
}

/* ---------------------------------------------------- 7.21.7, 7.21.8: input */

static void VerifyStdioInput(void)
{
    static const char source[] = "one\ntwo";

    char line[16];
    char block[8];
    FILE *stream;

    /* fgetc delivers every byte and then end-of-file, and the indicator is set
     * by reaching the end rather than by being at it. */
    stream = OxysStreamOpenMemoryRead(source, strlen(source));
    VerifyStdioRequire(stream != NULL, "a memory stream for reading could not be opened");
    VerifyStdioRequire(feof(stream) == 0, "a stream with bytes to give reported end-of-file");

    for (size_t index = 0U; index < strlen(source); ++index)
    {
        VerifyStdioRequire(fgetc(stream) == (int)(unsigned char)source[index],
                           "fgetc did not deliver the bytes of its source in order");
    }

    VerifyStdioRequire(fgetc(stream) == EOF, "fgetc did not report end-of-file");
    VerifyStdioRequire(feof(stream) != 0, "fgetc did not set the end-of-file indicator");
    VerifyStdioRequire(ferror(stream) == 0, "end-of-file was reported as an error");

    /*
     * A stream that has reported its end does not ask its source again.
     *
     * The end-of-file indicator is consulted before the source is, and this is
     * what asserts that order. A stream that asked again would make one system
     * call per call after the end — for a program looping upon fgetc, one per
     * iteration for ever — and nothing about the characters it delivered would
     * differ, so no assertion upon the bytes can see it. The census can.
     */
    {
        OxysStreamCensus before;
        OxysStreamCensus after;

        OxysStreamInspect(&before);
        VerifyStdioRequire(fgetc(stream) == EOF, "a stream past its end delivered a character");
        VerifyStdioRequire(fgetc(stream) == EOF, "a stream past its end delivered a character");
        VerifyStdioRequire(fgetc(stream) == EOF, "a stream past its end delivered a character");
        OxysStreamInspect(&after);
        VerifyStdioRequire(after.fills == before.fills,
                           "a stream at its end asked its source for more");
    }

    /*
     * ungetc: one character is guaranteed, a second is refused, the pushed back
     * character is the next one read, and the pushback clears the end-of-file
     * indicator.
     *
     * The last of those is ISO/IEC 9899:2011, Section 7.21.7.10, paragraph 2,
     * and is the one an implementation forgets: a stream with a character
     * waiting is not at its end, and a caller that pushes one back after reading
     * EOF must be able to read it.
     */
    VerifyStdioRequire(ungetc('Z', stream) == (int)(unsigned char)'Z',
                       "ungetc refused the one character it must accept");
    VerifyStdioRequire(feof(stream) == 0, "ungetc did not clear the end-of-file indicator");
    VerifyStdioRequire(ungetc('Y', stream) == EOF,
                       "ungetc accepted a second pushback without an intervening read");
    VerifyStdioRequire(fgetc(stream) == (int)(unsigned char)'Z',
                       "the character pushed back was not the next one read");
    VerifyStdioRequire(fgetc(stream) == EOF, "the stream did not return to end-of-file");
    VerifyStdioRequire(ungetc(EOF, stream) == EOF, "ungetc of EOF was accepted");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "the reading stream could not be closed");

    /* fgets keeps the newline, terminates what it read, and stops at the bound
     * of the array it was given. */
    stream = OxysStreamOpenMemoryRead(source, strlen(source));
    VerifyStdioRequire(fgets(line, (int)sizeof line, stream) == line,
                       "fgets did not return the array it was given");
    VerifyStdioRequire(strcmp(line, "one\n") == 0, "fgets did not stop after the newline, or discarded it");

    /* A partial line followed by end-of-file is returned and not discarded,
     * which is what keeps the last line of a source with no final newline. */
    VerifyStdioRequire(fgets(line, (int)sizeof line, stream) == line,
                       "fgets discarded a partial line at end-of-file");
    VerifyStdioRequire(strcmp(line, "two") == 0, "fgets did not deliver the partial line");
    VerifyStdioRequire(fgets(line, (int)sizeof line, stream) == NULL,
                       "fgets returned a line after end-of-file with nothing read");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "the fgets stream could not be closed");

    /* A count of one leaves room for the terminator and nothing else. The array
     * is terminated and returned — no end has been met, only the bound — and the
     * byte after the terminator is untouched, which is the bound a loop written
     * with `<=` writes past. */
    stream = OxysStreamOpenMemoryRead(source, strlen(source));
    line[1] = (char)VERIFY_STDIO_SENTINEL;
    VerifyStdioRequire(fgets(line, 1, stream) == line,
                       "fgets with room for the terminator alone reported an end that had not arrived");
    VerifyStdioRequire(line[0] == '\0',
                       "fgets with a count of one did not terminate the array");
    VerifyStdioRequire(line[1] == (char)VERIFY_STDIO_SENTINEL,
                       "fgets with a count of one wrote past the terminator");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "the bounded fgets stream could not be closed");

    /* fread returns whole elements, and a source that runs out part way through
     * one does not yield it. */
    stream = OxysStreamOpenMemoryRead(source, strlen(source));
    VerifyStdioRequire(fread(block, 2U, 4U, stream) == 3U,
                       "fread counted an element its source could not complete");
    VerifyStdioRequire(memcmp(block, "one\ntw", 6U) == 0,
                       "fread did not deliver the bytes of its source in order");
    VerifyStdioRequire(feof(stream) != 0, "fread did not set the end-of-file indicator");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "the fread stream could not be closed");

    /* A source of no bytes is a stream immediately at its end, which is the case
     * every loop over a stream gets wrong first. */
    stream = OxysStreamOpenMemoryRead(source, 0U);
    VerifyStdioRequire(stream != NULL, "a source of no bytes was refused");
    VerifyStdioRequire(fgetc(stream) == EOF, "a source of no bytes delivered a character");
    VerifyStdioRequire(feof(stream) != 0, "a source of no bytes did not report end-of-file");
    VerifyStdioRequire(OxysStreamClose(stream) == 0, "the empty stream could not be closed");
}

/* ------------------------------------------------------------- 7.21.6 */

/* Whether a conversion produced exactly this string, and reported exactly its
 * length. The two are asserted together because a formatter that produces the
 * right characters and returns the wrong count is wrong in the way snprintf's
 * measuring idiom depends upon. */
static void VerifyStdioProduces(const char *produced, int count,
                                const char *expected, const char *statement)
{
    VerifyStdioRequire(strcmp(produced, expected) == 0, statement);
    VerifyStdioRequire(count == (int)strlen(expected), statement);
}

static void VerifyStdioFormat(void)
{
    char buffer[64];
    int count;

    count = snprintf(buffer, sizeof buffer, "plain");
    VerifyStdioProduces(buffer, count, "plain", "a format with no conversion was altered");

    count = snprintf(buffer, sizeof buffer, "100%% done");
    VerifyStdioProduces(buffer, count, "100% done", "%% did not produce one per cent sign");

    count = snprintf(buffer, sizeof buffer, "%d|%i|%u", -42, 42, 4294967295U);
    VerifyStdioProduces(buffer, count, "-42|42|4294967295",
                        "the decimal conversions did not agree with their arguments");

    /* The most negative representable value. Forming its magnitude as `-value`
     * is undefined behaviour and is the single input every hand-written
     * formatter gets wrong, in the direction that produces a plausible answer. */
    count = snprintf(buffer, sizeof buffer, "%d", -2147483647 - 1);
    VerifyStdioProduces(buffer, count, "-2147483648",
                        "the most negative int was not converted");

    count = snprintf(buffer, sizeof buffer, "%lld", (long long)INT64_MIN);
    VerifyStdioProduces(buffer, count, "-9223372036854775808",
                        "the most negative long long was not converted");

    count = snprintf(buffer, sizeof buffer, "%o|%#o|%#o", 8U, 8U, 0U);
    VerifyStdioProduces(buffer, count, "10|010|0",
                        "the octal conversion or its alternative form is wrong");

    count = snprintf(buffer, sizeof buffer, "%x|%X|%#x|%#X|%#x", 48879U, 48879U,
                     48879U, 48879U, 0U);
    VerifyStdioProduces(buffer, count, "beef|BEEF|0xbeef|0XBEEF|0",
                        "the hexadecimal conversion or its prefix is wrong");

    count = snprintf(buffer, sizeof buffer, "[%5d][%-5d][%05d][%05d]", 42, 42, 42, -42);
    VerifyStdioProduces(buffer, count, "[   42][42   ][00042][-0042]",
                        "a field width or the zero flag was applied wrongly");

    /* The classic way to get this wrong is to pad with zeroes outside the sign,
     * which produces `0-042`. */
    count = snprintf(buffer, sizeof buffer, "[%+d][% d][%+d]", 42, 42, -42);
    VerifyStdioProduces(buffer, count, "[+42][ 42][-42]",
                        "the sign and space flags were applied wrongly");

    /* The sign and space flags have meaning only for a signed conversion,
     * 7.21.6.1 paragraph 6. A formatter that lets them through prints `+42` for
     * `%+u`, which is one character wider than the caller's field asked for and
     * which no standard library does. */
    count = snprintf(buffer, sizeof buffer, "[%+u][% u][%+x]", 42U, 42U, 42U);
    VerifyStdioProduces(buffer, count, "[42][42][2a]",
                        "a sign flag was applied to an unsigned conversion");

    count = snprintf(buffer, sizeof buffer, "[%.5d][%.0d][%8.5d][%-8.5d]", 42, 0, 42, 42);
    VerifyStdioProduces(buffer, count, "[00042][][   00042][00042   ]",
                        "a precision was applied wrongly to an integer");

    /* The zero flag is ignored where a precision was given, which 7.21.6.1
     * paragraph 6 states and which a formatter applies twice if it is not
     * careful. */
    count = snprintf(buffer, sizeof buffer, "[%08.5d]", 42);
    VerifyStdioProduces(buffer, count, "[   00042]",
                        "the zero flag was not ignored where a precision was given");

    count = snprintf(buffer, sizeof buffer, "[%c][%5c][%-5c]", 'A', 'A', 'A');
    VerifyStdioProduces(buffer, count, "[A][    A][A    ]",
                        "the character conversion or its field is wrong");

    count = snprintf(buffer, sizeof buffer, "[%s][%10s][%-10s][%.2s]", "Oxys", "Oxys",
                     "Oxys", "Oxys");
    VerifyStdioProduces(buffer, count, "[Oxys][      Oxys][Oxys      ][Ox]",
                        "the string conversion, its field or its precision is wrong");

    /* A negative width given by an asterisk is a left justification with a
     * positive width, which 7.21.6.1 paragraph 4 states explicitly. */
    count = snprintf(buffer, sizeof buffer, "[%*d][%*d][%.*s]", 6, 42, -6, 42, 2, "Oxys");
    VerifyStdioProduces(buffer, count, "[    42][42    ][Ox]",
                        "a width or precision given by an asterisk was applied wrongly");

    /* The narrow length modifiers are a conversion back from int and not a
     * different va_arg type, the default argument promotions having already
     * widened them. */
    count = snprintf(buffer, sizeof buffer, "%hhd|%hd", 300, 70000);
    VerifyStdioProduces(buffer, count, "44|4464",
                        "a narrow length modifier did not narrow its argument");

    count = snprintf(buffer, sizeof buffer, "%zu|%ju", (size_t)123456U,
                     (uintmax_t)UINT64_C(18446744073709551615));
    VerifyStdioProduces(buffer, count, "123456|18446744073709551615",
                        "the size or maximum length modifier is wrong");

    count = snprintf(buffer, sizeof buffer, "%p|%p", (void *)(uintptr_t)0x401000U,
                     (void *)0);
    VerifyStdioProduces(buffer, count, "0x401000|0x0",
                        "the pointer conversion is not this library's stated form");

    /*
     * The return value is what *would* have been produced, and the array is
     * always terminated within the size given.
     *
     * A formatter that returned the stored count would make the measuring idiom
     * — a call with a size of zero — report that nothing is needed.
     */
    {
        char small[6];

        count = snprintf(small, sizeof small, "%d-%s", 12345, "abc");
        VerifyStdioRequire(count == 9, "snprintf did not report the length it would have needed");
        VerifyStdioRequire(strcmp(small, "12345") == 0,
                           "snprintf did not truncate to the size it was given, or did not terminate");
    }

    VerifyStdioRequire(snprintf(NULL, 0U, "%08.3x", 42) == 8,
                       "snprintf with a size of zero did not measure the result");

    /*
     * A conversion this library does not implement is refused and the refusal is
     * reported.
     *
     * 7.21.6.1, paragraph 9, makes an undefined conversion specification
     * undefined behaviour, so any answer conforms; this library refuses, so that
     * a caller of `%n` is told the write it asked for did not happen rather than
     * being given a plausible count. `%f` cannot be implemented at all here —
     * this translation unit is compiled without a floating-point unit.
     */
    VerifyStdioRequire(snprintf(buffer, sizeof buffer, "%f", 1) < 0,
                       "a floating conversion was not refused");
    VerifyStdioRequire(snprintf(buffer, sizeof buffer, "%n", &count) < 0,
                       "the %n conversion was not refused");
    VerifyStdioRequire(snprintf(buffer, sizeof buffer, "abc%") < 0,
                       "a format ending in a lone per cent sign was not refused");

    /* The formatted conversion and the stream are joined, and the joint is where
     * a defect would otherwise hide: everything above went into an array. */
    {
        char *const region = VerifyStdioPrepare();
        FILE *const stream = OxysStreamOpenMemoryWrite(region, 64U);

        VerifyStdioRequire(fprintf(stream, "%s=%d\n", "ticks", 17) == 9,
                           "fprintf did not report the characters it produced");
        VerifyStdioRequire(OxysStreamDelivered(stream) == 0U,
                           "fprintf upon a fully buffered stream reached the device early");
        VerifyStdioRequire(fflush(stream) == 0, "the fprintf stream could not be flushed");
        VerifyStdioRequire(VerifyStdioDelivered(stream, region, "ticks=17\n"),
                           "fprintf did not deliver what it produced");
        VerifyStdioRequire(VerifyStdioMarginIsIntact(64U),
                           "fprintf wrote outside the region the stream was given");
        VerifyStdioRequire(OxysStreamClose(stream) == 0, "the fprintf stream could not be closed");
    }
}

/* ------------------------------------------------- the three standard streams */

/*
 * What can be asserted of stdin, stdout and stderr without touching the system.
 *
 * Their descriptors and their buffering are properties of the objects and are
 * examined directly. Nothing is written to stdout or stderr, and since sub-task
 * 8.1 nothing is read from stdin, for the reason at the head of this file: each
 * would execute SYSCALL. The end-of-file discipline stdin used to be asserted
 * by is asserted upon a memory stream instead, in VerifyStdioInput, which is
 * the same policy above a different source.
 */
static void VerifyStdioStandardStreams(void)
{
    VerifyStdioRequire(OxysStreamDescriptor(stdin) == 0,
                       "the standard input is not descriptor 0");
    VerifyStdioRequire(OxysStreamDescriptor(stdout) == 1,
                       "the standard output is not descriptor 1");
    VerifyStdioRequire(OxysStreamDescriptor(stderr) == 2,
                       "the standard error is not descriptor 2");

    /* None of the three may be closed: a library that permitted it would have to
     * answer what printf does afterwards, and there is no good answer. */
    VerifyStdioRequire(OxysStreamClose(stdout) == EOF, "the standard output could be closed");
    VerifyStdioRequire(OxysStreamClose(stderr) == EOF, "the standard error could be closed");
    VerifyStdioRequire(OxysStreamClose(stdin) == EOF, "the standard input could be closed");

    /* Flushing a stream with nothing pending must not reach the system, which is
     * what makes fflush(NULL) safe to call here at all. */
    VerifyStdioRequire(fflush(stdout) == 0, "flushing an empty standard output failed");
    VerifyStdioRequire(fflush(NULL) == 0, "flushing every stream failed");
}

void KernelVerifyStdio(void)
{
    OxysStreamCensus census;

    VerifyStdioSucceeded = true;

    KernelWriteString("Stdio: asserting the C library's buffered streams and "
                      "formatted conversion.\n");

    VerifyStdioBuffering();
    VerifyStdioOutput();
    VerifyStdioInput();
    VerifyStdioFormat();
    VerifyStdioStandardStreams();

    OxysStreamInspect(&census);

    /*
     * Every stream this test opened was closed, and nothing reached the system
     * through either seam.
     *
     * The first is the leak assertion: the pool is FOPEN_MAX entries and three
     * of them are the standard streams, so a test that opened a stream without
     * closing it would exhaust the pool before it finished — and would do so
     * only after enough assertions had been added, which is a failure that
     * arrives in a later session for no reason a reader can see.
     *
     * The second and third are what keep this test from resetting the machine.
     * A byte delivered to a descriptor, or asked of one, would execute SYSCALL,
     * which this kernel cannot survive; these are the assertions that say so
     * rather than the comment at the head of the file. Until sub-task 8.1 the
     * third read `1` and not `0`: OxysStreamFill executed nothing then, the
     * standard input was read here to assert it reported its end, and the
     * count said it was asked exactly once. Since 8.1 the fill reads the
     * terminal, and the discipline that a stream at its end is not asked again
     * is asserted upon a memory stream in VerifyStdioInput instead.
     */
    VerifyStdioRequire(census.open == 3U,
                       "a stream this test opened was not closed");
    VerifyStdioRequire(census.write_calls == 0U,
                       "a stream reached the system, which this kernel cannot provide");
    VerifyStdioRequire(census.fill_calls == 0U,
                       "a stream asked the system for characters, which this kernel "
                       "cannot provide");
    VerifyStdioRequire(census.rejections == 3U,
                       "the count of refused conversions is not the three this test makes");

    KernelWriteString("  The streams performed ");
    KernelWriteDecimal(census.flushes);
    KernelWriteString(" flush(es) and ");
    KernelWriteDecimal(census.fills);
    KernelWriteString(" fill(s); ");
    KernelWriteDecimal(census.written);
    KernelWriteString(" byte(s) out, ");
    KernelWriteDecimal(census.read);
    KernelWriteString(" in, ");
    KernelWriteDecimal(census.pushbacks);
    KernelWriteString(" pushback(s); the source was asked ");
    KernelWriteDecimal(census.fill_calls);
    KernelWriteString(" time(s); the formatter performed ");
    KernelWriteDecimal(census.conversions);
    KernelWriteString(" conversion(s) and refused ");
    KernelWriteDecimal(census.rejections);
    KernelWriteString(".\n");

    KernelWriteString(VerifyStdioSucceeded ? "Stdio self-test passed.\n"
                                           : "Stdio self-test FAILED.\n");
}
