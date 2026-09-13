/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: MIT */
/*
 * File: libc/include/stdio.h
 * Purpose: Declares the input and output of ISO/IEC 9899:2011, Section 7.21, as
 *          this library implements it — the buffered stream, the byte-at-a-time
 *          and block transfers above it, and the formatted conversion — and
 *          states, at its head, which of that section is absent and what each
 *          absence is waiting for.
 * Key definitions: FILE, stdin, stdout, stderr, EOF, BUFSIZ, FOPEN_MAX,
 *          _IOFBF, _IOLBF, _IONBF, fflush, setvbuf, setbuf, fwrite, fputc, putc,
 *          putchar, fputs, puts, fread, fgetc, getc, getchar, ungetc, fgets,
 *          feof, ferror, clearerr, printf, fprintf, vprintf, vfprintf, sprintf,
 *          vsprintf, snprintf, vsnprintf.
 * References:
 *   - ISO/IEC 9899:2011, Section 7.21.1: FILE, the three standard streams, EOF,
 *     BUFSIZ, FOPEN_MAX and the three buffering modes.
 *   - ISO/IEC 9899:2011, Section 7.21.3: the stream model — full buffering, line
 *     buffering and no buffering, and paragraph 7, which requires stderr not to
 *     be fully buffered and permits stdin and stdout to be fully buffered only
 *     where the stream does not refer to an interactive device.
 *   - ISO/IEC 9899:2011, Sections 7.21.5 to 7.21.9: the operations declared
 *     below, each cited again at the function that implements it.
 *   - ISO/IEC 9899:2011, Section 7.21.6.1: the conversion specification the
 *     formatted output functions accept, paragraph by paragraph.
 *   - libc/include/stream.h: the seam beneath a stream — the one place this
 *     machinery touches the system — and the memory stream by which the
 *     buffering policy is exercised without one.
 *   - docs/design/LIBC.md, Section 10: the design of this sub-task, what is
 *     asserted of it and what is not.
 *
 * What of <stdio.h> is here, and what is not.
 *
 * This header is not <stdio.h> as a hosted implementation provides it. ISO/IEC
 * 9899:2011, Section 4, paragraph 6, does not require a freestanding
 * implementation to provide the header at all, and this library provides the
 * part of it sub-task 7.4 is: the stream, the transfers, and the formatted
 * output. What is absent is absent for a reason that can be named in each case,
 * and none of the reasons is that it was not got round to.
 *
 *   **The file operations of 7.21.4 and the opening of 7.21.5** — remove,
 *   rename, tmpfile, tmpnam, fopen, freopen, fclose. This kernel has eight
 *   system calls and not one of them opens a file. A `fopen` here could only
 *   fail, and a function that can only fail is worse than one that does not
 *   exist: a program that calls it compiles, links, and discovers at run time
 *   that the library lied about what it offers. They arrive with the system
 *   calls that can implement them, which is Phase 8's business, the shell being
 *   the first thing that needs a file by name.
 *
 *   **The positioning of 7.21.9** — fseek, ftell, fgetpos, fsetpos, rewind, and
 *   with them fpos_t, SEEK_SET, SEEK_CUR and SEEK_END. Nothing here is
 *   positionable. The two streams that exist write to a diagnostic channel that
 *   has no position, and the memory stream of <stream.h> is a buffer a caller
 *   already holds a pointer into. They arrive with the file operations above.
 *
 *   **The formatted input of 7.21.6.2, 7.21.6.4, 7.21.6.7, 7.21.6.9, 7.21.6.11
 *   and 7.21.6.14** — the scanf family. The conversion is half of it and the
 *   other half is a source of characters to convert; see the note upon input
 *   below, which is why fgetc exists and reports end-of-file.
 *
 *   **Every floating-point conversion** — a, A, e, E, f, F, g and G, in both
 *   directions. PROJECT_GUIDELINES.md, Section 8, prohibits floating-point
 *   arithmetic without a justification, and the kernel is compiled -mno-sse and
 *   -mno-80387 besides, so a conversion that formed a double would not assemble.
 *   A conversion specification naming one is refused rather than ignored: see
 *   the note upon the return value below.
 *
 *   **The wide-character conversions** — the l modifier applied to c and s, and
 *   the whole of <wchar.h>. This system has no locale and no multibyte encoding
 *   to convert between, which is the same reason strcoll and strxfrm are absent
 *   from <string.h>.
 *
 *   **%n.** It is the one conversion that writes through a pointer taken from
 *   the argument list under the direction of the format string, which is the
 *   mechanism by which a format string a program did not compose becomes a write
 *   to an address the attacker chose. It is required by 7.21.6.1 paragraph 8 and
 *   it is refused here, deliberately and with the refusal reported. What it
 *   costs is that a conforming program using %n does not work; what it buys is
 *   that a program which passes a string it received to printf cannot be made to
 *   write memory by it. docs/design/LIBC.md, Section 10.6, records the trade.
 *
 * Why input exists at all when nothing can supply it.
 *
 * The sub-task is buffered input *and* output, and the buffering is the part
 * that is worth getting right: the pushback, the end-of-file and error
 * indicators that stick until they are cleared, a partial read that is not an
 * error, and a byte read back after being pushed back. All of that is policy
 * above a source, exactly as the heap of sub-task 7.3 is policy above a source,
 * and it is asserted the same way — against a memory stream, which supplies
 * characters without a system call. What is absent is only the shipped source:
 * this kernel has no call that reads, so OxysStreamFill reports end-of-file and
 * stdin is a stream that is permanently at it. The day a read call exists, one
 * function of six lines changes and every program above it keeps working.
 *
 * Why these keep their standard names when every other global function in this
 * repository is PascalCase.
 *
 * The same reason malloc does, recorded at the head of <stdlib.h>: their names
 * and their meanings are fixed by ISO/IEC 9899:2011 and a conforming program is
 * entitled to find them. The library's own machinery beneath them — the seam,
 * the memory stream, the census — is this project's and is PascalCase, and that
 * is the line: a name the standard fixes keeps the standard's spelling, and a
 * name this project invented obeys PROJECT_GUIDELINES.md, Section 4.
 */

#ifndef OXYS_LIBC_STDIO_H
#define OXYS_LIBC_STDIO_H

#include <stdarg.h>
#include <stddef.h>

/*
 * ISO/IEC 9899:2011, Section 7.21.1, paragraph 2: an object type capable of
 * recording all the information needed to control a stream.
 *
 * **It is deliberately incomplete.** The standard requires it to be an object
 * type and every operation upon a stream in that section takes a FILE *, so a
 * conforming program never needs its size or its members; leaving it incomplete
 * is what makes that a guarantee rather than an expectation. A program that
 * could see the members would come to depend upon them, and every change to the
 * buffering would then be a change to the interface.
 *
 * The consequence is that a program cannot declare a FILE of its own, and no
 * conforming one wants to: the streams that exist are the three below, and a
 * further one is obtained from OxysStreamOpenMemory in <stream.h>.
 */
typedef struct OxysStream FILE;

/*
 * 7.21.1, paragraph 3: a negative integer constant expression, returned by
 * several functions to indicate end-of-file — that is, no more input from a
 * stream.
 *
 * It is -1 and not some other negative value because fgetc returns an unsigned
 * char converted to int, which is 0 to 255, and EOF must be distinguishable from
 * every one of them. Any negative number would serve; -1 is what every program
 * that compares against it by hand expects.
 */
#define EOF (-1)

/*
 * 7.21.1, paragraph 3: the size suitable for the array used by setbuf.
 *
 * It is 1024 and not 4096. The buffer of a stream this library creates lives in
 * the program's .bss — there is no allocator involved, a stream being usable
 * before a heap exists — so every byte of it is in the image and mapped whether
 * or not the stream is ever written to. One buffer exists for every entry of the
 * pool rather than only for the streams presently open, because an entry whose
 * buffer arrived later would be an entry that behaves differently; FOPEN_MAX
 * buffers of this size is eight kibibytes, which is two pages and is the whole
 * cost of this sub-task in a program's image. No output this system performs
 * comes near filling one.
 */
#define BUFSIZ 1024U

/*
 * 7.21.1, paragraph 3: the minimum number of files that can be open
 * simultaneously — which the standard requires to be at least 8, and which here
 * counts the streams that can exist rather than the files, there being no files.
 *
 * Three are the standard streams and the remainder are what
 * OxysStreamOpenMemory hands out. The pool is static for the reason the buffers
 * are: a stream must be obtainable before a heap has been grown, and the first
 * thing a program does with a heap that failed is try to report it.
 */
#define FOPEN_MAX 8

/*
 * 7.21.1, paragraph 3, and 7.21.5.6: the three buffering modes, which are
 * distinct and which setvbuf takes as its third argument.
 *
 * Their values are not the standard's — the standard fixes none — and nothing
 * may depend upon them being these.
 */
#define _IOFBF 0 /* Fully buffered: the buffer is emptied when it fills. */
#define _IOLBF 1 /* Line buffered: and also when a newline is written. */
#define _IONBF 2 /* Unbuffered: every byte goes to the system as it is written. */

/*
 * 7.21.1, paragraph 3: expressions of type "pointer to FILE" that point to the
 * FILE objects associated with the standard error, input and output streams.
 *
 * They are pointers to objects within the library and not the objects
 * themselves, which is what the standard requires and also what allows them to
 * be incomplete above. The three are:
 *
 *   stdin   Permanently at end-of-file, this kernel having no call that reads.
 *           Its buffering policy is real and is asserted; its source is not.
 *   stdout  Line buffered, which 7.21.3 paragraph 7 permits and which is chosen
 *           because the thing at the far end is a person reading a console. A
 *           fully buffered stdout loses the last partial line whenever a program
 *           faults, and the last partial line before a fault is the one worth
 *           having.
 *   stderr  Unbuffered, which 7.21.3 paragraph 7 requires: it must not be fully
 *           buffered, and a diagnostic that is still in a buffer when the
 *           program dies is a diagnostic that was not issued.
 */
extern FILE *const stdin;
extern FILE *const stdout;
extern FILE *const stderr;

/* -------------------------------------------------------------------------
 * 7.21.5: the file access functions this library has.
 * ------------------------------------------------------------------------- */

/*
 * 7.21.5.2: writes any unwritten data for a stream to the system.
 *
 * A null argument flushes every stream this library holds, which paragraph 3
 * requires, and returns EOF if any of them failed — having attempted all of
 * them, because a flush that stopped at the first failure would leave the
 * streams after it holding output nobody asked to lose.
 *
 * A stream with no unwritten data is a success and touches nothing. Returns zero
 * upon success and EOF upon failure, setting the stream's error indicator.
 */
int fflush(FILE *stream);

/*
 * 7.21.5.6: sets the buffering mode and, optionally, the buffer.
 *
 * It may be called only after the stream has been obtained and before any other
 * operation upon it, which paragraph 2 requires; a call made later is refused
 * with a non-zero result rather than obeyed, because changing a buffer out from
 * under data already in it discards that data silently.
 *
 * A null `buffer` with a mode of _IOFBF or _IOLBF leaves the stream using the
 * one it already has and changes only the mode, which is the behaviour the
 * standard leaves implementation-defined and the only useful one here: this
 * library has no allocator it may call to obtain a buffer, a stream being usable
 * before a heap exists.
 *
 * Returns zero upon success and a non-zero value for an invalid mode, for a size
 * of zero with a buffer, or for a stream that has already been used.
 */
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);

/* 7.21.5.5: equivalent to setvbuf with _IOFBF and BUFSIZ, or with _IONBF where
 * the buffer is null. It returns nothing, so a caller that needs to know whether
 * it worked must call setvbuf. */
void setbuf(FILE *stream, char *buffer);

/* -------------------------------------------------------------------------
 * 7.21.6: formatted input and output. Output only; see the head of this file.
 * ------------------------------------------------------------------------- */

/*
 * 7.21.6.3 and 7.21.6.11: writes to a stream under the control of `format`,
 * which may contain conversion specifications, each of which consumes zero or
 * more arguments.
 *
 * The conversions are d, i, o, u, x, X, c, s, p and the literal %%. The flags
 * are -, +, space, # and 0; the field width and the precision may each be a
 * decimal digit string or a `*` taking an int argument; and the length modifiers
 * are hh, h, l, ll, z, j and t. Every one of those is ISO/IEC 9899:2011, Section
 * 7.21.6.1, and the paragraph each comes from is cited at the code that
 * implements it.
 *
 * Returns the number of characters transmitted, or a negative value if an output
 * or encoding error occurred — and, unlike a hosted implementation, also if the
 * format string named a conversion this library does not implement. That is the
 * one place this departs from the standard, and it is deliberate: 7.21.6.1
 * paragraph 9 makes an unknown conversion undefined behaviour, so any answer is
 * conforming, and the two available answers are "print something arbitrary and
 * return a count that is wrong in a way nobody checks" or "refuse and say so".
 * A caller that checks the result — which is the whole point of it being
 * returned — is told. docs/design/LIBC.md, Section 10.6.
 */
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list arguments);

/* 7.21.6.3 and 7.21.6.10: fprintf and vfprintf with stdout as the stream. */
int printf(const char *format, ...);
int vprintf(const char *format, va_list arguments);

/*
 * 7.21.6.5 and 7.21.6.12: as fprintf, but the output is written into an array
 * and terminated with a null character, and no more than `size` characters
 * — the terminator included — are written.
 *
 * The return value is the number of characters that *would* have been written
 * had the array been large enough, not counting the terminator, so a caller may
 * size an array by calling this with a size of zero. A size of zero permits a
 * null array, which is what makes that idiom legal rather than merely usual.
 */
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list arguments);

/*
 * 7.21.6.6 and 7.21.6.13: as snprintf with no bound whatever.
 *
 * **These two are a hazard and are provided because the standard requires them.**
 * The caller must know, before the call, that the result fits — and the result
 * depends upon arguments, so in almost every case the caller cannot know. A
 * program written today should call snprintf; these exist so that a program
 * written elsewhere and ported here links. The bound they are given internally
 * is the greatest representable size, which is no bound at all and is stated so
 * that nobody reads a protection into it.
 */
int sprintf(char *buffer, const char *format, ...);
int vsprintf(char *buffer, const char *format, va_list arguments);

/* -------------------------------------------------------------------------
 * 7.21.7: character input and output.
 * ------------------------------------------------------------------------- */

/*
 * 7.21.7.1: reads the next character from a stream as an unsigned char converted
 * to an int, or EOF where the stream is at end-of-file or an error occurs.
 *
 * The two are distinguished by feof and ferror and not by the result, which is
 * EOF for both — that being the defect this pair of indicators exists to answer,
 * and the reason a loop written as `while ((c = fgetc(f)) != EOF)` cannot tell a
 * finished file from a broken one without asking.
 */
int fgetc(FILE *stream);

/* 7.21.7.5: equivalent to fgetc. The standard permits this one to be a macro
 * that evaluates its argument more than once; this implementation is a function,
 * so it does not, and a program may pass an expression with side effects. */
int getc(FILE *stream);

/* 7.21.7.6: equivalent to getc with stdin. */
int getchar(void);

/*
 * 7.21.7.2: reads at most one less than `count` characters into the array,
 * stopping after a newline — which is kept — or at end-of-file, and terminates
 * what it read with a null character.
 *
 * Returns the array, or a null pointer where end-of-file is met before any
 * character has been read, or where a read error occurs. **A partial line
 * followed by end-of-file is returned**, not discarded: the characters were read
 * and there is nowhere else for them to go.
 */
char *fgets(char *buffer, int count, FILE *stream);

/*
 * 7.21.7.3 and 7.21.7.7: writes a character, converted to an unsigned char, to a
 * stream. Returns the character written, or EOF and sets the error indicator.
 */
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);

/* 7.21.7.8: equivalent to putc with stdout. */
int putchar(int character);

/*
 * 7.21.7.4: writes a string to a stream, not writing its terminator. Returns a
 * non-negative value upon success and EOF upon a write error.
 */
int fputs(const char *string, FILE *stream);

/*
 * 7.21.7.9: writes a string to stdout and then a newline, which fputs does not.
 * That difference is the whole of what distinguishes them and is the commonest
 * surprise in the two, so it is stated here rather than assumed.
 */
int puts(const char *string);

/*
 * 7.21.7.10: pushes a character back onto a stream, where the next read will
 * return it. Returns the character, or EOF where the pushback could not be made.
 *
 * One character of pushback is guaranteed and is what this provides; a second
 * without an intervening read is refused, which paragraph 3 permits. Pushing
 * back EOF changes nothing and returns EOF, and the end-of-file indicator is
 * cleared by a successful pushback, as paragraph 2 requires — a stream with a
 * character waiting is not at its end.
 */
int ungetc(int character, FILE *stream);

/* -------------------------------------------------------------------------
 * 7.21.8: direct input and output.
 * ------------------------------------------------------------------------- */

/*
 * 7.21.8.1: reads up to `count` elements of `size` bytes each into the array,
 * and returns the number of elements read — which is fewer than `count` only
 * upon a read error or end-of-file.
 *
 * A size or a count of zero reads nothing, returns zero, and leaves the contents
 * of the array and the state of the stream unchanged, which paragraph 2
 * requires. The product is checked for overflow before it is formed, for the
 * reason recorded at calloc in <stdlib.h>: a product that wraps produces a small
 * transfer for a large request and the caller reads bytes that were never
 * written.
 */
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);

/*
 * 7.21.8.2: writes up to `count` elements of `size` bytes each from the array,
 * and returns the number of elements written — which is fewer than `count` only
 * upon a write error.
 *
 * **The count is of whole elements**, so a transfer that failed part way through
 * an element reports that element as not written even though some of its bytes
 * were. That is what the standard requires and it is why a caller that wants to
 * know how many *bytes* went out must call with a size of one.
 */
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);

/* -------------------------------------------------------------------------
 * 7.21.10: the error handling functions.
 * ------------------------------------------------------------------------- */

/*
 * 7.21.10.1: clears the end-of-file and error indicators of a stream.
 *
 * It is the only thing that clears the error indicator. That is the point of the
 * indicator being sticky: an error in the middle of a sequence of writes is
 * still reported at the end of it, so a program that checks once after the last
 * write is not misled by the last write having happened to succeed.
 */
void clearerr(FILE *stream);

/* 7.21.10.2: whether the end-of-file indicator is set. */
int feof(FILE *stream);

/* 7.21.10.3: whether the error indicator is set. */
int ferror(FILE *stream);

#endif /* OXYS_LIBC_STDIO_H */
