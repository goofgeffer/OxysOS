/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/stream.h
 * Purpose: Declares the two things the streams of <stdio.h> need that the C
 *          standard does not describe — where their bytes go and where their
 *          bytes come from — together with the memory stream that stands in for
 *          both, and the census a caller may take of the machinery.
 * Key definitions: OxysStreamWrite, OxysStreamFill, OxysStreamOpenMemoryWrite,
 *          OxysStreamOpenMemoryRead, OxysStreamClose, OxysStreamDelivered,
 *          OxysStreamDescriptor, OxysStreamCensus, OxysStreamInspect,
 *          OXYS_STREAM_NO_DESCRIPTOR.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.21.3: the stream model this is the underside
 *     of, and paragraph 3's "characters are transmitted to the host
 *     environment", which is the sentence these two functions are.
 *   - libc/include/syscall.h: OxysWrite, which is what the shipped
 *     implementation of OxysStreamWrite is written in terms of.
 *   - libc/include/heap.h: the same seam, one sub-task earlier, and the note
 *     there upon why a named function is better than a call buried inside the
 *     policy. This header is that argument applied a second time.
 *   - docs/design/LIBC.md: the division of sub-task 7.4 into a
 *     policy that runs anywhere and a pair of transfers that run only at
 *     privilege level 3.
 *
 * Why a stream's transfers are named functions rather than calls inside the
 * buffering.
 *
 * The reason is the one <heap.h> gives, and it has not changed: the two are
 * different kinds of thing and fail in different ways. **The policy** — when a
 * buffer is emptied, what a partial transfer means, how a pushback interacts
 * with an end-of-file indicator, what a conversion specification produces — is
 * ordinary C that runs anywhere and is wrong in ways a test can see. **The
 * transfer** is a system call, and this kernel cannot execute one: SYSRET
 * returns to privilege level 3 unconditionally, so a kernel that flushed a
 * stream would leave its own entry path as a user program.
 *
 * Naming the seam is what lets each be asserted where it can be. The kernel's
 * boot-time self-test opens a memory stream and exercises the whole of the
 * policy against it — the code this library actually ships, not a reconstruction
 * of it — while the shipped transfers are asserted from the other side, by a
 * program at privilege level 3. Sub-task 7.5 is where such a program first
 * exists, and it is the sub-task that closes this gap rather than a later one.
 *
 * The memory stream is not a test hook. A program composing a string by the
 * formatted conversion has the same need and no other way to meet it — it is
 * what a hosted implementation spells fmemopen — and the self-test is merely its
 * first caller. That is the same claim <heap.h> makes about OxysHeapAdopt, and
 * it is made here for the same reason: a seam that exists only for a test is a
 * seam the shipped code does not have, and asserting it proves nothing about
 * what ships.
 */

#ifndef OXYS_LIBC_STREAM_H
#define OXYS_LIBC_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <stdio.h>

/*
 * The descriptor of a stream that has none, which is every memory stream.
 *
 * It is -1 and not zero, zero being the standard input's descriptor and
 * therefore a real one. A caller reading this from OxysStreamDescriptor is
 * asking "does this stream reach the system", and the answer must not be a
 * number that also means "yes, the first one".
 */
#define OXYS_STREAM_NO_DESCRIPTOR (-1)

/*
 * Transmits `length` bytes to the system upon the given descriptor, and returns
 * how many were transmitted, or -1.
 *
 * A result short of `length` is not an error: the kernel bounds a single
 * transfer and a caller that means to write more must call again from where this
 * one stopped. The buffering above does exactly that, and the loop is in the
 * policy rather than here so that this function remains the one statement of
 * what the system does.
 *
 * The shipped implementation is libc/stdio/system.c and is written in terms of
 * OxysWrite. It is declared here so that the policy may be compiled and asserted
 * without it, which is the division this header exists for.
 */
int64_t OxysStreamWrite(int descriptor, const void *buffer, size_t length);

/*
 * Obtains at most `capacity` bytes from the system upon the given descriptor,
 * and returns how many were obtained, zero at end-of-file, or -1.
 *
 * **The shipped implementation reads the descriptor, since sub-task 8.1.** It
 * returned zero before that: sub-task 7.6's `read` read a file through a
 * descriptor `open` gave out, and no call read the thing `stdin` is connected
 * to, so a stream whose source was this function was permanently at
 * end-of-file. It was written as a function that reported end-of-file rather
 * than as an absent function so that every program above it would already be
 * correct on the day the call existed — and on that day the body of this
 * function changed and nothing above it did. A fill of `stdin` now waits until
 * something has been typed and returns short, which the policy above treats as
 * ordinary.
 *
 * Zero and -1 are distinguished because the policy above must distinguish them:
 * end-of-file sets one indicator and an error sets another, and a stream that
 * conflated them would report a broken transfer as a finished one.
 */
int64_t OxysStreamFill(int descriptor, void *buffer, size_t capacity);

/*
 * Opens a stream whose device is a region of memory the caller supplies, for
 * writing.
 *
 * Everything written to the stream — after the buffering has decided when —
 * is appended to the region. A region that fills stops accepting bytes and the
 * stream's error indicator is set, which is the same report a device that
 * refused a transfer would produce; nothing is written past the end of the
 * region under any circumstance.
 *
 * The region is **not** terminated with a null character. A caller that wants a
 * string must reserve a byte for one and place it at OxysStreamDelivered, and
 * that is deliberate: a stream that terminated its region would be writing a
 * byte nobody asked it to write, and a caller using the region as a buffer of
 * bytes would find one of them altered.
 *
 * Returns a null pointer where the pool of streams is exhausted, where the
 * region is null, or where its capacity is zero. The stream is returned to the
 * pool by OxysStreamClose and by nothing else.
 */
FILE *OxysStreamOpenMemoryWrite(char *region, size_t capacity);

/*
 * Opens a stream whose source is a region of memory the caller supplies, for
 * reading.
 *
 * The region is read once from its beginning to its end and end-of-file follows
 * it. The region must remain valid and unaltered for as long as the stream is
 * open; nothing is copied out of it before it is asked for, so a caller that
 * overwrites it changes what the stream will deliver.
 *
 * A length of zero is permitted and yields a stream immediately at end-of-file,
 * which is a thing worth being able to construct: it is the case every loop over
 * a stream gets wrong first.
 */
FILE *OxysStreamOpenMemoryRead(const char *region, size_t length);

/*
 * Closes a stream obtained from either of the two above, flushing it first, and
 * returns zero upon success or EOF where the flush failed.
 *
 * It refuses the three standard streams and returns EOF for them. They are the
 * program's for as long as the program runs, and a library that let one be
 * closed would have to answer what `printf` does afterwards — for which there is
 * no good answer, only a choice between a silent discard and a fault.
 *
 * This is not fclose. fclose closes a file, and the name is reserved for the day
 * this library can open one; see the head of <stdio.h>.
 */
int OxysStreamClose(FILE *stream);

/*
 * How many bytes a stream has delivered to its device, which for a memory stream
 * is how much of the region holds output.
 *
 * It counts what has left the buffer and not what has been written to the
 * stream, so a caller that has not flushed will see a number smaller than what
 * it wrote — which is the correct answer to the question asked and is the
 * commonest surprise in a buffered library. Returns zero for a null stream.
 */
size_t OxysStreamDelivered(FILE *stream);

/*
 * The descriptor a stream transmits upon, or OXYS_STREAM_NO_DESCRIPTOR where it
 * has none. Returns the latter for a null stream.
 */
int OxysStreamDescriptor(FILE *stream);

/*
 * What the stream machinery has done, since the program started.
 *
 * It is a census of the whole machinery and not of one stream, for the reason
 * the heap's is a census of the heap: the numbers worth having are the ones that
 * say whether a policy behaved, and a policy is a property of the machinery. The
 * one a caller is most likely to want is `write_calls` — a self-test that must
 * not reach the system can assert it is zero, and so discovers a change that
 * made it reach the system rather than suffering one.
 *
 * The two seams are counted separately and that is not tidiness. Until sub-task
 * 8.1 only one of them executed SYSCALL: OxysStreamWrite did, and
 * OxysStreamFill did not, there being no call that reads for it to make, so a
 * self-test conducted inside this kernel could read a stream and could not
 * write one, and needed to assert that `write_calls` was zero while
 * `fill_calls` was whatever its own reading made it. A single counter for both
 * would have forced that assertion to be made loosely or not at all — and the
 * first version of this structure had one, which is how the distinction came
 * to be noticed. Since 8.1 both seams execute SYSCALL and the self-test asserts
 * both counters are zero; the two are kept apart because a census that could
 * not say which seam was reached would be a census that could not say what a
 * program did.
 */
typedef struct OxysStreamCensus
{
    size_t open;           /* Streams presently open, the three standard ones included. */
    size_t highest_open;   /* The most that were open at once. */
    uint64_t opens;        /* Memory streams handed out. */
    uint64_t closes;       /* Streams given back. */
    uint64_t refusals;     /* Opens and closes refused. */

    uint64_t flushes;      /* Times a buffer was emptied towards a device. */
    uint64_t fills;        /* Times a buffer was filled from a source. */
    uint64_t write_calls;  /* Calls made to OxysStreamWrite. */
    uint64_t fill_calls;   /* Calls made to OxysStreamFill. */
    uint64_t short_writes; /* Transfers that moved fewer bytes than were offered. */
    uint64_t errors;       /* Transfers that failed. */

    uint64_t written;      /* Bytes delivered to a device. */
    uint64_t read;         /* Bytes obtained from a source. */
    uint64_t pushbacks;    /* Characters pushed back by ungetc. */
    uint64_t conversions;  /* Conversion specifications the formatter performed. */
    uint64_t rejections;   /* Conversion specifications it refused. */
} OxysStreamCensus;

/* Takes the census. A null argument does nothing. */
void OxysStreamInspect(OxysStreamCensus *census);

#endif /* OXYS_LIBC_STREAM_H */
