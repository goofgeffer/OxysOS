/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/stdio/stream.c
 * Purpose: The buffered stream of ISO/IEC 9899:2011, Section 7.21 — the FILE
 *          object, the three standard streams, the decision of when a buffer is
 *          emptied or filled, and every transfer of Sections 7.21.7 and 7.21.8
 *          above it.
 * Key functions: fflush, setvbuf, setbuf, fputc, putc, putchar, fputs, puts,
 *          fwrite, fgetc, getc, getchar, fgets, fread, ungetc, feof, ferror,
 *          clearerr, OxysStreamOpenMemoryWrite, OxysStreamOpenMemoryRead,
 *          OxysStreamClose, OxysStreamDelivered, OxysStreamDescriptor,
 *          OxysStreamInspect, OxysStreamCountConversion,
 *          OxysStreamCountRejection.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.21.2: a stream is a sequence of characters
 *     and the buffering between it and the host environment is the
 *     implementation's business; Section 7.21.3, paragraphs 3 and 7, the three
 *     buffering modes and what each of the standard streams may be.
 *   - ISO/IEC 9899:2011, Sections 7.21.5, 7.21.7, 7.21.8 and 7.21.10: each
 *     function below, cited at the function.
 *   - libc/include/stream.h: the seam — OxysStreamWrite and OxysStreamFill —
 *     which is the whole of what this file knows about the machine beneath it.
 *   - docs/design/LIBC.md, Section 10: the design of this sub-task, and Section
 *     10.5 the table pairing every asserted property with the silent failure it
 *     exists to catch.
 *
 * This file holds the policy and nothing about where bytes go.
 *
 *   Every byte that leaves a stream leaves it through StreamDeliver, and every
 *   byte that enters one enters through StreamObtain. Those two are the only
 *   places a device is touched, and for a stream with a descriptor they call the
 *   seam of <stream.h>, which this kernel cannot execute. For a memory stream
 *   they copy, and that is what allows the whole of this file to be asserted by
 *   the kernel's boot-time self-test against code the library actually ships.
 *
 * The transfers are byte-at-a-time above the buffer, deliberately.
 *
 *   fread, fwrite, fgets and fputs are each a loop over the single-character
 *   operation rather than a block copy into the buffer. It is the same judgement
 *   the string functions of sub-task 7.1 record: a block copy is faster and has
 *   four more ways to be wrong — the buffer boundary, the pushback, the
 *   line-buffering decision and the partial transfer — and there is no workload
 *   here to measure the difference against. docs/design/LIBC.md, Section 10.7,
 *   limitation 1, records it as the thing a ported compiler will change.
 *
 * Concurrency.
 *
 *   None. ISO/IEC 9899:2011, Section 7.21.2, paragraph 7, makes a stream's
 *   access a matter for the implementation, and this one performs none: there
 *   are no userland threads, and the census below is a plain object with no
 *   atomic access. The day a program has two threads, every stream needs a lock
 *   and this paragraph is where the change begins.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>
#include <stream.h>

#include "internal.h"

/*
 * The bytes a stream buffers by default.
 *
 * It is BUFSIZ for every stream in the pool and not only for the standard ones,
 * because a stream obtained later is a stream whose buffering matters as much —
 * and because a pool in which entries differ is a pool in which a defect appears
 * for some streams and not others, which is the hardest kind to find.
 */
#define STREAM_BUFFER_BYTES BUFSIZ

/* The pushback slot holds this when nothing has been pushed back. It is not a
 * character: every character a stream delivers is an unsigned char converted to
 * int, which is never negative. */
#define STREAM_NO_PUSHBACK (-1)

/*
 * A stream.
 *
 * The division of the buffer between the two directions is the thing to
 * understand here, and it is deliberate that a stream is one or the other and
 * never both. `pending` counts bytes written into the buffer and not yet
 * delivered; `filled` and `cursor` count bytes obtained from the source and not
 * yet handed to a caller. A stream that could be read and written would have to
 * answer what happens to one when the other is used, which ISO/IEC 9899:2011,
 * Section 7.21.5.3, paragraph 6, answers for a file opened for update by
 * requiring an intervening flush or positioning call. There is no positioning
 * here and no file, so the question is removed rather than answered: a stream is
 * readable or writable, fixed when it is opened.
 */
struct OxysStream
{
    bool used;      /* This entry of the pool is a stream. */
    bool standard;  /* One of the three; OxysStreamClose refuses it. */
    bool readable;
    bool writable;
    bool touched;   /* An operation has been performed; setvbuf is refused after. */
    bool at_end;    /* The end-of-file indicator of 7.21.10.2. */
    bool failed;    /* The error indicator of 7.21.10.3. */

    int descriptor; /* OXYS_STREAM_NO_DESCRIPTOR for a memory stream. */
    int mode;       /* _IOFBF, _IOLBF or _IONBF. */

    char *buffer;
    size_t capacity;
    size_t pending;
    size_t filled;
    size_t cursor;

    int pushback;

    size_t delivered; /* Bytes that have reached the device. */

    /* The memory device, used where `descriptor` is OXYS_STREAM_NO_DESCRIPTOR.
     * `region` is written to and `source` is read from; a stream has at most one
     * of them, the two directions being exclusive. */
    char *region;
    size_t region_capacity;
    const char *source;
    size_t source_length;
    size_t source_cursor;
};

static char StreamBuffers[FOPEN_MAX][STREAM_BUFFER_BYTES];

/*
 * The pool, of which the first three entries are the standard streams.
 *
 * They are initialised statically rather than by a function called before main,
 * because there is nothing that calls one: this library has no constructor
 * mechanism and sub-task 7.5's startup object deliberately does not acquire one
 * for this. A stream that is correct because the loader zeroed its .bss and the
 * initialisers filled in the rest is a stream that works in a program whose
 * first statement is `puts`.
 *
 * Descriptor 0 is the standard input, 1 the output and 2 the error, which is the
 * numbering kernel/abi/oxys/syscall_abi.h's `write` already uses for the latter
 * two. Nothing reads descriptor 0; see OxysStreamFill.
 */
static struct OxysStream StreamPool[FOPEN_MAX] = {
    {
        .used = true, .standard = true, .readable = true, .writable = false,
        .touched = false, .at_end = false, .failed = false,
        .descriptor = 0, .mode = _IOFBF,
        .buffer = StreamBuffers[0], .capacity = STREAM_BUFFER_BYTES,
        .pending = 0U, .filled = 0U, .cursor = 0U,
        .pushback = STREAM_NO_PUSHBACK, .delivered = 0U,
        .region = NULL, .region_capacity = 0U,
        .source = NULL, .source_length = 0U, .source_cursor = 0U
    },
    {
        .used = true, .standard = true, .readable = false, .writable = true,
        .touched = false, .at_end = false, .failed = false,
        .descriptor = 1, .mode = _IOLBF,
        .buffer = StreamBuffers[1], .capacity = STREAM_BUFFER_BYTES,
        .pending = 0U, .filled = 0U, .cursor = 0U,
        .pushback = STREAM_NO_PUSHBACK, .delivered = 0U,
        .region = NULL, .region_capacity = 0U,
        .source = NULL, .source_length = 0U, .source_cursor = 0U
    },
    {
        .used = true, .standard = true, .readable = false, .writable = true,
        .touched = false, .at_end = false, .failed = false,
        .descriptor = 2, .mode = _IONBF,
        .buffer = StreamBuffers[2], .capacity = STREAM_BUFFER_BYTES,
        .pending = 0U, .filled = 0U, .cursor = 0U,
        .pushback = STREAM_NO_PUSHBACK, .delivered = 0U,
        .region = NULL, .region_capacity = 0U,
        .source = NULL, .source_length = 0U, .source_cursor = 0U
    }
};

/*
 * ISO/IEC 9899:2011, Section 7.21.1, paragraph 3, requires these to be
 * expressions of type "pointer to FILE". They are const pointers so that a
 * program cannot make stdout name something else: a library that permitted it
 * would have every diagnostic in every other translation unit follow, which is
 * a redirection nobody asked for and nobody can see.
 */
FILE *const stdin = &StreamPool[0];
FILE *const stdout = &StreamPool[1];
FILE *const stderr = &StreamPool[2];

static OxysStreamCensus StreamRecord = {
    .open = 3U, .highest_open = 3U, .opens = 0U, .closes = 0U, .refusals = 0U,
    .flushes = 0U, .fills = 0U, .write_calls = 0U, .fill_calls = 0U,
    .short_writes = 0U, .errors = 0U,
    .written = 0U, .read = 0U, .pushbacks = 0U, .conversions = 0U, .rejections = 0U
};

void OxysStreamCountConversion(void)
{
    ++StreamRecord.conversions;
}

void OxysStreamCountRejection(void)
{
    ++StreamRecord.rejections;
}

void OxysStreamInspect(OxysStreamCensus *census)
{
    if (census == NULL)
    {
        return;
    }

    *census = StreamRecord;
}

/* ------------------------------------------------------------- the device */

/*
 * Delivers bytes to a stream's device, and reports whether every one of them
 * arrived.
 *
 * The loop is here and not in the seam because a short transfer is the system's
 * normal behaviour and not a failure: the kernel bounds a single write, so a
 * caller that means to write more must call again from where it stopped. A
 * library that treated the first short result as the whole answer would truncate
 * every message longer than that bound, and would do it silently — the count
 * fwrite returned would be the truncated one, and almost nothing checks it.
 *
 * A transfer that returns zero without failing is treated as a failure rather
 * than retried. Retrying would be an unbounded loop against a device that has
 * said it will take nothing, and a program that hangs in `printf` is worse than
 * one that is told its output did not go.
 */
static bool StreamDeliver(FILE *stream, const char *bytes, size_t length)
{
    size_t sent = 0U;

    if (length == 0U)
    {
        return true;
    }

    ++StreamRecord.flushes;

    if (stream->descriptor == OXYS_STREAM_NO_DESCRIPTOR)
    {
        size_t room;

        if (stream->region == NULL)
        {
            stream->failed = true;

            return false;
        }

        room = stream->region_capacity - stream->delivered;

        if (room > length)
        {
            room = length;
        }

        for (size_t index = 0U; index < room; ++index)
        {
            stream->region[stream->delivered + index] = bytes[index];
        }

        stream->delivered += room;
        StreamRecord.written += (uint64_t)room;

        if (room < length)
        {
            ++StreamRecord.short_writes;
            ++StreamRecord.errors;
            stream->failed = true;

            return false;
        }

        return true;
    }

    while (sent < length)
    {
        const int64_t moved = OxysStreamWrite(stream->descriptor, &bytes[sent],
                                              length - sent);

        ++StreamRecord.write_calls;

        if (moved <= 0)
        {
            ++StreamRecord.errors;
            stream->failed = true;

            return false;
        }

        if ((uint64_t)moved < (uint64_t)(length - sent))
        {
            ++StreamRecord.short_writes;
        }

        sent += (size_t)moved;
        stream->delivered += (size_t)moved;
        StreamRecord.written += (uint64_t)moved;
    }

    return true;
}

/*
 * Obtains bytes for a stream from its source, returning how many, zero at
 * end-of-file, or -1 upon an error.
 *
 * Unlike the delivery above there is no loop: a short read is the correct answer
 * to "give me what you have", and a library that looped until the buffer was
 * full would block a program that asked for one character behind a source with
 * one character to give.
 */
static int64_t StreamObtain(FILE *stream, char *into, size_t capacity)
{
    if (capacity == 0U)
    {
        return 0;
    }

    ++StreamRecord.fills;

    if (stream->descriptor == OXYS_STREAM_NO_DESCRIPTOR)
    {
        size_t available;

        if (stream->source == NULL)
        {
            return 0;
        }

        available = stream->source_length - stream->source_cursor;

        if (available > capacity)
        {
            available = capacity;
        }

        for (size_t index = 0U; index < available; ++index)
        {
            into[index] = stream->source[stream->source_cursor + index];
        }

        stream->source_cursor += available;
        StreamRecord.read += (uint64_t)available;

        return (int64_t)available;
    }

    {
        const int64_t moved = OxysStreamFill(stream->descriptor, into, capacity);

        ++StreamRecord.fill_calls;

        if (moved < 0)
        {
            ++StreamRecord.errors;

            return -1;
        }

        StreamRecord.read += (uint64_t)moved;

        return moved;
    }
}

/* ------------------------------------------------------------- 7.21.5 */

/* Empties a writable stream's buffer towards its device. A stream that is not
 * writable has nothing to empty and is a success; see fflush. */
static bool StreamFlushOne(FILE *stream)
{
    size_t pending;

    if (!stream->writable || (stream->pending == 0U))
    {
        return true;
    }

    pending = stream->pending;

    /*
     * The count is cleared *before* the delivery and not after it.
     *
     * A delivery that fails leaves the bytes undelivered, and a stream that kept
     * them would attempt them again at the next flush — so a device that has
     * stopped accepting output produces a buffer that grows, is retried whole
     * every time, and eventually delivers the same prefix repeatedly. The error
     * indicator is what reports the loss; the bytes are dropped.
     */
    stream->pending = 0U;

    return StreamDeliver(stream, stream->buffer, pending);
}

int fflush(FILE *stream)
{
    if (stream == NULL)
    {
        bool succeeded = true;

        /*
         * ISO/IEC 9899:2011, Section 7.21.5.2, paragraph 3: a null argument
         * flushes every stream. Every one is attempted even after one has
         * failed, because stopping would silently discard the output of the
         * streams after it — and the whole reason a program calls fflush(NULL)
         * is that it is about to do something it may not come back from.
         */
        for (size_t index = 0U; index < (size_t)FOPEN_MAX; ++index)
        {
            if (StreamPool[index].used && !StreamFlushOne(&StreamPool[index]))
            {
                succeeded = false;
            }
        }

        return succeeded ? 0 : EOF;
    }

    return StreamFlushOne(stream) ? 0 : EOF;
}

int setvbuf(FILE *stream, char *buffer, int mode, size_t size)
{
    if (stream == NULL)
    {
        return EOF;
    }

    if ((mode != _IOFBF) && (mode != _IOLBF) && (mode != _IONBF))
    {
        return EOF;
    }

    /*
     * 7.21.5.6, paragraph 2: the call must be made before any other operation is
     * performed upon the stream. It is refused rather than obeyed for the reason
     * given in <stdio.h>: a buffer replaced while it holds data loses that data,
     * and loses it without any report.
     */
    if (stream->touched)
    {
        return EOF;
    }

    if (mode == _IONBF)
    {
        stream->mode = _IONBF;

        return 0;
    }

    if (buffer != NULL)
    {
        if (size == 0U)
        {
            return EOF;
        }

        stream->buffer = buffer;
        stream->capacity = size;
    }

    stream->mode = mode;
    stream->pending = 0U;
    stream->filled = 0U;
    stream->cursor = 0U;

    return 0;
}

void setbuf(FILE *stream, char *buffer)
{
    (void)setvbuf(stream, buffer, (buffer == NULL) ? _IONBF : _IOFBF, BUFSIZ);
}

/* ------------------------------------------------------------- 7.21.7 output */

int fputc(int character, FILE *stream)
{
    const char byte = (char)(unsigned char)character;

    if ((stream == NULL) || !stream->writable)
    {
        return EOF;
    }

    stream->touched = true;

    if (stream->mode == _IONBF)
    {
        return StreamDeliver(stream, &byte, 1U) ? (int)(unsigned char)character : EOF;
    }

    /*
     * The buffer is emptied when it is *already* full rather than after the byte
     * that fills it, so that the byte just written is never the one left out.
     * Written the other way round — append, then flush if full — the buffer is
     * emptied one byte later and the difference is invisible until a caller
     * gives a stream a buffer of one byte, whereupon the append writes past its
     * end. setvbuf accepts a size of one, so that is a reachable state and not a
     * hypothetical one.
     */
    if (stream->pending >= stream->capacity)
    {
        if (!StreamFlushOne(stream))
        {
            return EOF;
        }
    }

    stream->buffer[stream->pending] = byte;
    ++stream->pending;

    if ((stream->mode == _IOLBF) && (byte == '\n'))
    {
        if (!StreamFlushOne(stream))
        {
            return EOF;
        }
    }

    return (int)(unsigned char)character;
}

int putc(int character, FILE *stream)
{
    return fputc(character, stream);
}

int putchar(int character)
{
    return fputc(character, stdout);
}

int fputs(const char *string, FILE *stream)
{
    if ((string == NULL) || (stream == NULL))
    {
        return EOF;
    }

    while (*string != '\0')
    {
        if (fputc((int)(unsigned char)*string, stream) == EOF)
        {
            return EOF;
        }

        ++string;
    }

    return 0;
}

int puts(const char *string)
{
    if (fputs(string, stdout) == EOF)
    {
        return EOF;
    }

    /* 7.21.7.9: a newline is written after the string, which fputs does not do.
     * It is the whole difference between the two functions. */
    return (fputc('\n', stdout) == EOF) ? EOF : 0;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream)
{
    const char *bytes = (const char *)buffer;
    size_t written = 0U;

    if ((buffer == NULL) || (stream == NULL) || (size == 0U) || (count == 0U))
    {
        return 0U;
    }

    /*
     * The product is not formed. It would be the natural way to write this loop
     * and it is the arithmetic fault calloc exists to avoid: `size * count` may
     * wrap, whereupon a large request becomes a small transfer and the caller is
     * told it wrote every element. Iterating over elements and then over the
     * bytes of one keeps every quantity within its own operand's range.
     */
    while (written < count)
    {
        for (size_t index = 0U; index < size; ++index)
        {
            if (fputc((int)(unsigned char)bytes[index], stream) == EOF)
            {
                /*
                 * 7.21.8.2, paragraph 2: the count returned is of *elements*
                 * written. An element whose bytes went out only in part did not
                 * go out, so the count is what was completed before this one.
                 */
                return written;
            }
        }

        bytes += size;
        ++written;
    }

    return written;
}

/* ------------------------------------------------------------- 7.21.7 input */

int fgetc(FILE *stream)
{
    if ((stream == NULL) || !stream->readable)
    {
        return EOF;
    }

    stream->touched = true;

    if (stream->pushback != STREAM_NO_PUSHBACK)
    {
        const int character = stream->pushback;

        stream->pushback = STREAM_NO_PUSHBACK;

        return character;
    }

    if (stream->cursor >= stream->filled)
    {
        int64_t obtained;

        /*
         * The end-of-file indicator is consulted before the source is, so that a
         * stream at its end is not asked again. A source may be expensive — a
         * system call, once one exists — and a loop that read past the end
         * would make one call for every iteration of a caller that keeps asking
         * after being told.
         */
        if (stream->at_end || stream->failed)
        {
            return EOF;
        }

        stream->filled = 0U;
        stream->cursor = 0U;

        obtained = StreamObtain(stream, stream->buffer,
                                (stream->mode == _IONBF) ? 1U : stream->capacity);

        if (obtained < 0)
        {
            stream->failed = true;

            return EOF;
        }

        if (obtained == 0)
        {
            stream->at_end = true;

            return EOF;
        }

        stream->filled = (size_t)obtained;
    }

    {
        const unsigned char byte = (unsigned char)stream->buffer[stream->cursor];

        ++stream->cursor;

        return (int)byte;
    }
}

int getc(FILE *stream)
{
    return fgetc(stream);
}

int getchar(void)
{
    return fgetc(stdin);
}

int ungetc(int character, FILE *stream)
{
    if ((stream == NULL) || !stream->readable || (character == EOF))
    {
        return EOF;
    }

    /* 7.21.7.10, paragraph 3: one character of pushback is guaranteed and a
     * second without an intervening read may fail. It fails here rather than
     * silently replacing the first, which would lose a character. */
    if (stream->pushback != STREAM_NO_PUSHBACK)
    {
        return EOF;
    }

    stream->pushback = (int)(unsigned char)character;
    ++StreamRecord.pushbacks;

    /* 7.21.7.10, paragraph 2: a successful call clears the end-of-file
     * indicator. A stream with a character waiting is not at its end, and a
     * caller that pushed one back after reading EOF must be able to read it. */
    stream->at_end = false;

    return (int)(unsigned char)character;
}

char *fgets(char *buffer, int count, FILE *stream)
{
    int written = 0;

    if ((buffer == NULL) || (count <= 0) || (stream == NULL))
    {
        return NULL;
    }

    while (written < (count - 1))
    {
        const int character = fgetc(stream);

        if (character == EOF)
        {
            break;
        }

        buffer[written] = (char)(unsigned char)character;
        ++written;

        if (character == (int)(unsigned char)'\n')
        {
            break;
        }
    }

    /*
     * 7.21.7.2, paragraph 3: a null pointer is returned where end-of-file is
     * encountered and no characters have been read, and where a read error
     * occurs — in which case the contents of the array are indeterminate, which
     * is why nothing is terminated on that path.
     *
     * A partial line followed by end-of-file is *not* the first of those:
     * characters were read, and discarding them would lose the last line of
     * every source that does not end with a newline.
     *
     * The condition is "end-of-file *and* nothing read" and not "nothing read",
     * and the difference is a count of one: an array with room for the
     * terminator alone reads no characters and has met no end, so the array is
     * terminated and returned. Written as "nothing read" this returns a null
     * pointer for that call, which no hosted implementation does and which a
     * caller looping upon the result reads as an end that has not arrived.
     */
    if (ferror(stream) != 0)
    {
        return NULL;
    }

    if ((written == 0) && (feof(stream) != 0))
    {
        return NULL;
    }

    buffer[written] = '\0';

    return buffer;
}

size_t fread(void *buffer, size_t size, size_t count, FILE *stream)
{
    char *bytes = (char *)buffer;
    size_t obtained = 0U;

    if ((buffer == NULL) || (stream == NULL) || (size == 0U) || (count == 0U))
    {
        return 0U;
    }

    /* The product is not formed, for the reason recorded at fwrite. */
    while (obtained < count)
    {
        for (size_t index = 0U; index < size; ++index)
        {
            const int character = fgetc(stream);

            if (character == EOF)
            {
                return obtained;
            }

            bytes[index] = (char)(unsigned char)character;
        }

        bytes += size;
        ++obtained;
    }

    return obtained;
}

/* ------------------------------------------------------------- 7.21.10 */

void clearerr(FILE *stream)
{
    if (stream == NULL)
    {
        return;
    }

    stream->at_end = false;
    stream->failed = false;
}

int feof(FILE *stream)
{
    return ((stream != NULL) && stream->at_end) ? 1 : 0;
}

int ferror(FILE *stream)
{
    return ((stream != NULL) && stream->failed) ? 1 : 0;
}

/* ------------------------------------------------------- the memory stream */

/* Takes an unused entry of the pool and gives it the state common to both kinds
 * of memory stream. A null result is an exhausted pool, which is counted. */
static FILE *StreamTake(void)
{
    for (size_t index = 0U; index < (size_t)FOPEN_MAX; ++index)
    {
        FILE *const stream = &StreamPool[index];

        if (!stream->used)
        {
            stream->used = true;
            stream->standard = false;
            stream->readable = false;
            stream->writable = false;
            stream->touched = false;
            stream->at_end = false;
            stream->failed = false;
            stream->descriptor = OXYS_STREAM_NO_DESCRIPTOR;
            stream->mode = _IOFBF;
            stream->buffer = StreamBuffers[index];
            stream->capacity = STREAM_BUFFER_BYTES;
            stream->pending = 0U;
            stream->filled = 0U;
            stream->cursor = 0U;
            stream->pushback = STREAM_NO_PUSHBACK;
            stream->delivered = 0U;
            stream->region = NULL;
            stream->region_capacity = 0U;
            stream->source = NULL;
            stream->source_length = 0U;
            stream->source_cursor = 0U;

            ++StreamRecord.open;
            ++StreamRecord.opens;

            if (StreamRecord.open > StreamRecord.highest_open)
            {
                StreamRecord.highest_open = StreamRecord.open;
            }

            return stream;
        }
    }

    ++StreamRecord.refusals;

    return NULL;
}

FILE *OxysStreamOpenMemoryWrite(char *region, size_t capacity)
{
    FILE *stream;

    if ((region == NULL) || (capacity == 0U))
    {
        ++StreamRecord.refusals;

        return NULL;
    }

    stream = StreamTake();

    if (stream == NULL)
    {
        return NULL;
    }

    stream->writable = true;
    stream->region = region;
    stream->region_capacity = capacity;

    return stream;
}

FILE *OxysStreamOpenMemoryRead(const char *region, size_t length)
{
    FILE *stream;

    /*
     * A length of zero is accepted and a null region is not, which is not the
     * same rule as the writing case above. A source of no bytes is a stream
     * immediately at end-of-file and is a thing worth being able to construct —
     * it is the case every loop over a stream gets wrong first — whereas a
     * destination of no bytes can hold no output and is a mistake.
     */
    if (region == NULL)
    {
        ++StreamRecord.refusals;

        return NULL;
    }

    stream = StreamTake();

    if (stream == NULL)
    {
        return NULL;
    }

    stream->readable = true;
    stream->source = region;
    stream->source_length = length;

    return stream;
}

int OxysStreamClose(FILE *stream)
{
    int result;

    if (stream == NULL)
    {
        ++StreamRecord.refusals;

        return EOF;
    }

    /* The three standard streams belong to the program for as long as it runs;
     * see <stream.h> for why there is no good answer to what printf would do
     * afterwards. */
    if (stream->standard || !stream->used)
    {
        ++StreamRecord.refusals;

        return EOF;
    }

    /*
     * A stream whose error indicator is set closes with a failure even where
     * the flush had nothing to do. The indicator is the record that output was
     * lost, and a close that reported success would be the last opportunity a
     * caller had to learn of it — fflush having been called already, or the
     * buffer having been empty because the loss happened upon an unbuffered
     * write.
     */
    result = (StreamFlushOne(stream) && !stream->failed) ? 0 : EOF;

    stream->used = false;
    stream->readable = false;
    stream->writable = false;
    stream->region = NULL;
    stream->source = NULL;

    --StreamRecord.open;
    ++StreamRecord.closes;

    return result;
}

size_t OxysStreamDelivered(FILE *stream)
{
    return (stream == NULL) ? 0U : stream->delivered;
}

int OxysStreamDescriptor(FILE *stream)
{
    return (stream == NULL) ? OXYS_STREAM_NO_DESCRIPTOR : stream->descriptor;
}
