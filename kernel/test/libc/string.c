/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/string.c
 * Purpose: Asserts the work of sub-task 7.1: the nineteen string and memory
 *          functions of ISO/IEC 9899:2011, Section 7.24, that this C library
 *          implements.
 * Key functions: KernelVerifyString.
 * References:
 *   - docs/design/LIBC.md: the table pairing every property asserted
 *     below with the silent failure that assertion exists to catch.
 *   - ISO/IEC 9899:2011, Section 7.24: the behaviour each assertion is made
 *     against. Where the standard's behaviour is surprising — strncpy not
 *     terminating, strstr of an empty needle, strchr finding the terminator —
 *     the assertion is written against the standard and not against what a
 *     reader might expect, and says so.
 *
 * Why a library is asserted by the kernel.
 *
 *   These functions are freestanding: they depend upon nothing but the C
 *   language — no allocator, no descriptor, no system call — so anything that
 *   can execute C can run them. `make verify` is the only thing in this project
 *   that can execute anything at all, there being no userland to run a harness
 *   in until this phase completes, so the four translation units of `libc/` are
 *   compiled into the kernel image and this file is what calls them.
 *
 *   **The kernel does not use them.** No kernel translation unit is compiled
 *   against `libc/include`, and this file is the single exception, named
 *   explicitly by a rule of its own in the Makefile. What the image links is
 *   code that is here to be asserted and for no other purpose, which is the same
 *   arrangement every other file in this directory already has.
 *
 * Why the assertions are what they are.
 *
 *   Every function here has a correct implementation and several plausible
 *   wrong ones, and the wrong ones are wrong in ways that produce a right answer
 *   for the inputs anybody tests with. Three recur:
 *
 *   **The signed byte.** The standard requires the comparing and searching
 *   functions to work upon `unsigned char`. An implementation that used plain
 *   char would agree with a correct one for every byte below 128 and disagree
 *   for every byte above it — upon x86_64, where plain char is signed. Every
 *   ASCII test passes; the first UTF-8 sequence, hash or binary buffer sorts
 *   backwards. So the comparisons here are asserted upon 0x80 and 0xFF and not
 *   upon letters.
 *
 *   **The byte just past the end.** A loop that runs one too far writes into
 *   whatever follows, and in a test whose buffers are adjacent zeroes that is
 *   invisible. Every destination here is therefore a region within a buffer
 *   filled with a sentinel, and the bytes either side of it are asserted to
 *   still hold it.
 *
 *   **The empty case.** A length of zero, an empty string, an empty set, an
 *   empty needle. Each is where the standard says something a natural loop does
 *   not do, and each is asserted first in its group.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>

#include <string.h>

static bool VerifyStringSucceeded;

static void VerifyStringRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString(" FAILED.\n");
        VerifyStringSucceeded = false;
    }
}

/*
 * The working buffer, and the sentinel that surrounds every use of it.
 *
 * A destination is never the whole buffer. The routines below fill the buffer
 * with VERIFY_STRING_SENTINEL, operate upon a region starting at
 * VERIFY_STRING_MARGIN, and then assert that the margin either side is
 * untouched. Without that margin a function writing one byte too many would
 * write a zero into a buffer that already held zeroes, and every assertion about
 * the contents would still pass.
 *
 * The sentinel is 0x5A rather than 0x00 or 0xFF because both of those are values
 * the functions under test legitimately write: a terminator and a memset fill.
 * A sentinel that a correct function may produce is not a sentinel.
 */
#define VERIFY_STRING_CAPACITY 48U
#define VERIFY_STRING_MARGIN    8U
#define VERIFY_STRING_SENTINEL  0x5A

static char VerifyStringBuffer[VERIFY_STRING_CAPACITY];

/* Fills the whole buffer with the sentinel and returns the working region. */
static char *VerifyStringPrepare(void)
{
    for (size_t index = 0U; index < VERIFY_STRING_CAPACITY; ++index)
    {
        VerifyStringBuffer[index] = (char)VERIFY_STRING_SENTINEL;
    }

    return &VerifyStringBuffer[VERIFY_STRING_MARGIN];
}

/*
 * Whether the margin either side of a working region of the given length still
 * holds the sentinel.
 *
 * This is the assertion that catches a loop bounded by `<=` where it should be
 * `<`, which is the most common defect in every function in this file and the
 * one that leaves no trace in the result.
 */
static bool VerifyStringMarginIsIntact(size_t used)
{
    for (size_t index = 0U; index < VERIFY_STRING_MARGIN; ++index)
    {
        if (VerifyStringBuffer[index] != (char)VERIFY_STRING_SENTINEL)
        {
            return false;
        }
    }

    for (size_t index = VERIFY_STRING_MARGIN + used;
         index < VERIFY_STRING_CAPACITY;
         ++index)
    {
        if (VerifyStringBuffer[index] != (char)VERIFY_STRING_SENTINEL)
        {
            return false;
        }
    }

    return true;
}

/* --------------------------------------------------------------- 7.24.2, 7.24.3 */

static void VerifyStringCopying(void)
{
    static const unsigned char pattern[8] = {
        0x01U, 0x7FU, 0x80U, 0xFFU, 0x00U, 0x41U, 0xFEU, 0x02U
    };

    char *region;

    /* memcpy: every byte, the return value, and nothing beyond n. */
    region = VerifyStringPrepare();
    VerifyStringRequire(memcpy(region, pattern, sizeof pattern) == region,
                        "memcpy did not return its destination");
    VerifyStringRequire(memcmp(region, pattern, sizeof pattern) == 0,
                        "memcpy did not reproduce every byte");
    VerifyStringRequire(VerifyStringMarginIsIntact(sizeof pattern),
                        "memcpy wrote outside the range it was given");

    /*
     * A length of zero must write nothing. A loop written as do-while — which is
     * what a reader reaches for when the length is known to be positive —
     * copies one byte here, and the whole buffer is still the sentinel except
     * for the one byte that is not.
     */
    region = VerifyStringPrepare();
    (void)memcpy(region, pattern, 0U);
    VerifyStringRequire(VerifyStringMarginIsIntact(0U),
                        "memcpy of zero bytes wrote something");

    /*
     * memmove, forwards: the destination lies *above* the source and the regions
     * overlap. A copy made in the wrong direction here writes the first byte
     * over the second source byte before reading it, and so smears byte zero
     * across the whole overlap — a plausible-looking result, which is why the
     * pattern is eight distinguishable bytes and the assertion is upon all of
     * them.
     */
    region = VerifyStringPrepare();
    (void)memcpy(region, pattern, sizeof pattern);
    VerifyStringRequire(memmove(region + 2, region, 6U) == region + 2,
                        "memmove did not return its destination");
    VerifyStringRequire(memcmp(region + 2, pattern, 6U) == 0,
                        "memmove upwards over an overlap smeared a byte");
    VerifyStringRequire((unsigned char)region[0] == pattern[0],
                        "memmove upwards altered the bytes before its destination");

    /* memmove, backwards: the destination lies below the source. The failure is
     * the mirror of the one above and is caught by the mirror of the test. */
    region = VerifyStringPrepare();
    (void)memcpy(region + 2, pattern, sizeof pattern);
    (void)memmove(region, region + 2, 6U);
    VerifyStringRequire(memcmp(region, pattern, 6U) == 0,
                        "memmove downwards over an overlap smeared a byte");

    /* The degenerate overlap. A branch written with `<=` instead of `<` copies
     * backwards here, which is still correct; a branch that copies nothing at
     * all is also correct. The assertion is that the object is unchanged, which
     * is the only thing the standard promises. */
    region = VerifyStringPrepare();
    (void)memcpy(region, pattern, sizeof pattern);
    (void)memmove(region, region, sizeof pattern);
    VerifyStringRequire(memcmp(region, pattern, sizeof pattern) == 0,
                        "memmove of an object onto itself altered it");

    /* strcpy: the terminator is copied, and nothing after it is. */
    region = VerifyStringPrepare();
    VerifyStringRequire(strcpy(region, "Oxys") == region,
                        "strcpy did not return its destination");
    VerifyStringRequire(strcmp(region, "Oxys") == 0,
                        "strcpy did not reproduce the string");
    VerifyStringRequire(VerifyStringMarginIsIntact(5U),
                        "strcpy wrote past the terminator");

    /*
     * strncpy pads. A source of four bytes into a bound of ten must leave six
     * null bytes behind it — which is the behaviour that makes this function
     * expensive and the behaviour a reader least expects.
     */
    region = VerifyStringPrepare();
    (void)strncpy(region, "Oxys", 10U);
    VerifyStringRequire(strcmp(region, "Oxys") == 0,
                        "strncpy did not copy a short source");
    VerifyStringRequire((region[4] == '\0') && (region[9] == '\0'),
                        "strncpy did not pad to its bound");
    VerifyStringRequire(VerifyStringMarginIsIntact(10U),
                        "strncpy wrote beyond its bound");

    /*
     * strncpy does not terminate. Four bytes of a longer source into a bound of
     * four must leave no terminator at all, so the byte after the copy is
     * whatever was there — here, the sentinel.
     *
     * This asserts the standard's behaviour and not the useful one. An
     * implementation that terminated would pass every other test in this file
     * and would silently truncate one byte of every maximal copy a caller made,
     * for ever, under a standard name.
     */
    region = VerifyStringPrepare();
    (void)strncpy(region, "Oxys-OS", 4U);
    VerifyStringRequire(memcmp(region, "Oxys", 4U) == 0,
                        "strncpy did not copy up to its bound");
    VerifyStringRequire(region[4] == (char)VERIFY_STRING_SENTINEL,
                        "strncpy terminated a destination it filled");

    /* strcat: appended at the terminator and not at the start, and terminated. */
    region = VerifyStringPrepare();
    (void)strcpy(region, "Oxys");
    VerifyStringRequire(strcat(region, "-OS") == region,
                        "strcat did not return its destination");
    VerifyStringRequire(strcmp(region, "Oxys-OS") == 0,
                        "strcat did not append at the terminator");
    VerifyStringRequire(VerifyStringMarginIsIntact(8U),
                        "strcat wrote past the terminator");

    /* Appending nothing must leave the string, and its terminator, exactly. */
    region = VerifyStringPrepare();
    (void)strcpy(region, "Oxys");
    (void)strcat(region, "");
    VerifyStringRequire(strcmp(region, "Oxys") == 0,
                        "strcat of an empty string altered the destination");

    /*
     * strncat terminates and does not pad — the opposite of strncpy in both
     * respects. Three bytes taken from a longer source must yield a string of
     * exactly the original plus three, terminated, with the tenth byte of the
     * region untouched.
     */
    region = VerifyStringPrepare();
    (void)strcpy(region, "Oxys");
    VerifyStringRequire(strncat(region, "-OSX", 3U) == region,
                        "strncat did not return its destination");
    VerifyStringRequire(strcmp(region, "Oxys-OS") == 0,
                        "strncat did not bound the bytes taken from its source");
    VerifyStringRequire(VerifyStringMarginIsIntact(8U),
                        "strncat wrote beyond the terminator it added");
}

/* ---------------------------------------------------------------------- 7.24.4 */

static void VerifyStringComparison(void)
{
    /*
     * The signed-byte trap, asserted three times because three functions can
     * fall into it independently.
     *
     * 0x80 is greater than 0x01 as an unsigned char and less than it as a
     * signed one. An implementation that compared plain char would return a
     * negative value here and would be wrong about every byte above 127 in the
     * system, while passing every test written with letters.
     */
    static const unsigned char high[1] = { 0x80U };
    static const unsigned char low[1] = { 0x01U };

    VerifyStringRequire(memcmp(high, low, 1U) > 0,
                        "memcmp compared bytes as signed rather than unsigned");
    VerifyStringRequire(memcmp(low, high, 1U) < 0,
                        "memcmp is not antisymmetric upon a high byte");
    VerifyStringRequire(memcmp(high, high, 1U) == 0,
                        "memcmp reported equal objects unequal");

    /* Zero bytes compare equal, whatever the objects hold. A loop that read one
     * byte anyway would report these two unequal. */
    VerifyStringRequire(memcmp(high, low, 0U) == 0,
                        "memcmp of zero bytes did not report equality");

    /* memcmp stops at n. The difference beyond the bound must not be seen. */
    VerifyStringRequire(memcmp("ab\x80", "ab\x01", 2U) == 0,
                        "memcmp read beyond the length it was given");

    VerifyStringRequire(strcmp("\x80", "\x01") > 0,
                        "strcmp compared bytes as signed rather than unsigned");
    VerifyStringRequire(strncmp("\x80", "\x01", 1U) > 0,
                        "strncmp compared bytes as signed rather than unsigned");

    /* A string is greater than its own prefix, the terminator being the least
     * byte. A comparison that stopped at the shorter string's end without
     * comparing the terminator would report these equal. */
    VerifyStringRequire(strcmp("abc", "abcd") < 0,
                        "strcmp did not order a prefix before the string");
    VerifyStringRequire(strcmp("abcd", "abc") > 0,
                        "strcmp did not order a string after its prefix");
    VerifyStringRequire(strcmp("", "") == 0,
                        "strcmp reported two empty strings unequal");
    VerifyStringRequire(strcmp("Oxys", "Oxys") == 0,
                        "strcmp reported identical strings unequal");

    /* strncmp's bound. Equal within it, different beyond it. */
    VerifyStringRequire(strncmp("abcX", "abcY", 3U) == 0,
                        "strncmp read beyond the length it was given");
    VerifyStringRequire(strncmp("abcX", "abcY", 4U) < 0,
                        "strncmp did not compare the byte at its bound");
    VerifyStringRequire(strncmp("abc", "abc", 100U) == 0,
                        "strncmp compared past a terminator");

    /*
     * The bound of zero. A caller may legitimately ask for a comparison of no
     * bytes, and the answer is equality — including for strings that differ in
     * their first byte, which is the case a loop written as do-while gets wrong.
     */
    VerifyStringRequire(strncmp("a", "b", 0U) == 0,
                        "strncmp of zero bytes did not report equality");
}

/* ---------------------------------------------------------------------- 7.24.5 */

static void VerifyStringSearch(void)
{
    static const unsigned char object[4] = { 0x00U, 0x80U, 0xFFU, 0x41U };
    static const char subject[] = "Oxys-OS-x";

    const void *found_object;
    const char *found;

    /* memchr converts to unsigned char. Searching for 0x80 must find it; an
     * implementation comparing plain char would find nothing. */
    found_object = memchr(object, 0x80, sizeof object);
    VerifyStringRequire(found_object == &object[1],
                        "memchr did not find a byte above 127");

    /* And it must find a zero byte, which is an ordinary byte to memchr. */
    found_object = memchr(object, 0x00, sizeof object);
    VerifyStringRequire(found_object == &object[0],
                        "memchr did not find a zero byte");

    VerifyStringRequire(memchr(object, 0x7F, sizeof object) == NULL,
                        "memchr found a byte the object does not hold");

    /* A length of zero finds nothing, whatever the first byte is. */
    VerifyStringRequire(memchr(object, 0x00, 0U) == NULL,
                        "memchr searched an object of zero bytes");

    /* memchr must not read past n: 0x41 is at index 3 and is outside a search
     * bounded to 3. */
    VerifyStringRequire(memchr(object, 0x41, 3U) == NULL,
                        "memchr read beyond the length it was given");

    /*
     * strchr finds the first, strrchr the last, and the subject holds 'x' twice
     * — at index 1 and at index 8 — so the two must disagree. A strrchr that
     * returned the first occurrence, which is what a search written as strchr
     * with a different name does, passes every assertion whose subject holds one
     * occurrence and fails only here.
     */
    found = strchr(subject, 'x');
    VerifyStringRequire(found == &subject[1],
                        "strchr did not find the first occurrence");
    found = strrchr(subject, 'x');
    VerifyStringRequire(found == &subject[8],
                        "strrchr did not find the last occurrence");

    /* A byte occurring once must be found at the same place by both. */
    found = strchr(subject, 'S');
    VerifyStringRequire(found == &subject[6],
                        "strchr did not find a byte occurring once");
    found = strrchr(subject, 'S');
    VerifyStringRequire(found == &subject[6],
                        "strrchr did not find a byte occurring once");

    /*
     * Both must find the terminator, the standard including it in the string
     * they search. A loop that tested for the terminator *before* comparing
     * returns null here, which is the natural way to write it and is wrong.
     */
    found = strchr(subject, '\0');
    VerifyStringRequire(found == &subject[sizeof subject - 1U],
                        "strchr did not find the terminator");
    found = strrchr(subject, '\0');
    VerifyStringRequire(found == &subject[sizeof subject - 1U],
                        "strrchr did not find the terminator");

    VerifyStringRequire(strchr(subject, 'Z') == NULL,
                        "strchr found a byte the string does not hold");
    VerifyStringRequire(strrchr(subject, 'Z') == NULL,
                        "strrchr found a byte the string does not hold");

    /* The spans. An empty set accepts nothing and rejects nothing, which is the
     * pair of answers a set-membership loop gets wrong in opposite directions. */
    VerifyStringRequire(strspn("aabbc", "ab") == 4U,
                        "strspn did not measure the accepted run");
    VerifyStringRequire(strspn("aabbc", "") == 0U,
                        "strspn accepted a byte against an empty set");
    VerifyStringRequire(strspn("", "ab") == 0U,
                        "strspn of an empty string was not zero");
    VerifyStringRequire(strcspn("aabbc", "c") == 4U,
                        "strcspn did not measure the rejected run");
    VerifyStringRequire(strcspn("aabbc", "") == 5U,
                        "strcspn rejected a byte against an empty set");

    /*
     * The terminator is in no set. strcspn(s, "x") where s holds no 'x' must be
     * the length of s — an implementation that treated the set's terminator as
     * a member would stop at the first byte and return zero, always.
     */
    VerifyStringRequire(strcspn("abc", "x") == 3U,
                        "strcspn treated a terminator as a member of the set");

    found = strpbrk(subject, "-");
    VerifyStringRequire(found == &subject[4],
                        "strpbrk did not find the first member of the set");
    VerifyStringRequire(strpbrk(subject, "Z") == NULL,
                        "strpbrk found a byte the string does not hold");
    VerifyStringRequire(strpbrk(subject, "") == NULL,
                        "strpbrk matched against an empty set");

    /*
     * The empty needle, asserted twice, and the second is the one that earns its
     * place.
     *
     * The standard's answer is the haystack itself. Against a haystack that is
     * not empty a plain loop already produces that, the needle's terminator
     * being reached at offset zero; against an **empty haystack** it does not,
     * because the loop performs no iteration and falls out to null. Only the
     * second case can tell a correct implementation from one that omits the
     * guard, and the first version of this test asserted only the first — so
     * deleting the guard from libc/string/search.c changed nothing the test
     * could see. docs/design/LIBC.md records that run.
     */
    VerifyStringRequire(strstr(subject, "") == subject,
                        "strstr of an empty needle did not return the haystack");

    {
        static const char empty[] = "";

        VerifyStringRequire(strstr(empty, "") == empty,
                            "strstr of an empty needle in an empty haystack did "
                            "not return the haystack");
        VerifyStringRequire(strstr(empty, "a") == NULL,
                            "strstr found a needle in an empty haystack");
    }

    found = strstr(subject, "OS");
    VerifyStringRequire(found == &subject[5],
                        "strstr did not find the needle");
    VerifyStringRequire(strstr(subject, "Oxys-OS-x") == subject,
                        "strstr did not match a needle equal to the haystack");
    VerifyStringRequire(strstr(subject, "OSX") == NULL,
                        "strstr matched a needle the haystack does not hold");

    /*
     * The restart. "aab" occurs in "aaab" only at offset 1, and a search that
     * failed to restart at the byte after the one it began at — rather than at
     * the byte where the comparison failed — misses it.
     */
    {
        static const char haystack[] = "aaab";

        found = strstr(haystack, "aab");
        VerifyStringRequire(found == &haystack[1],
                            "strstr did not restart after a partial match");
    }

    /* A needle longer than the haystack must not be read past the haystack's
     * terminator, which the comparison's terminator test is what prevents. */
    VerifyStringRequire(strstr("ab", "abc") == NULL,
                        "strstr matched a needle longer than the haystack");
}

/* ------------------------------------------------------------------ 7.24.5.8 */

static void VerifyStringTokens(void)
{
    /*
     * strtok writes into the string it is given, replacing each separator it
     * consumes with a terminator, so the subject must be a writable array and
     * may never be a string literal.
     */
    char subject[] = ",,alpha,,beta,";
    char *token;

    /* Leading separators are skipped rather than yielding empty tokens, which is
     * the behaviour that distinguishes strtok from a field splitter and the
     * thing a caller most often expects it not to do. */
    token = strtok(subject, ",");
    VerifyStringRequire((token != NULL) && (strcmp(token, "alpha") == 0),
                        "strtok did not skip the leading separators");

    /* Runs of separators between tokens are likewise one separator. */
    token = strtok(NULL, ",");
    VerifyStringRequire((token != NULL) && (strcmp(token, "beta") == 0),
                        "strtok did not collapse a run of separators");

    /* A trailing separator yields no token, and the scan is then finished. */
    token = strtok(NULL, ",");
    VerifyStringRequire(token == NULL,
                        "strtok yielded a token after the last one");

    /*
     * And it stays finished. A scan whose position was left pointing at the
     * subject's terminator gives the same answer here by accident, and gives a
     * wrong one the moment a caller begins a new scan of a different string.
     */
    token = strtok(NULL, ",");
    VerifyStringRequire(token == NULL,
                        "strtok resumed a scan that had finished");

    /* A string of separators alone yields nothing at all. */
    {
        char separators_only[] = "::::";

        token = strtok(separators_only, ":");
        VerifyStringRequire(token == NULL,
                            "strtok found a token in a string of separators");
    }

    /* A string with no separator is one token. */
    {
        char whole[] = "single";

        token = strtok(whole, ",");
        VerifyStringRequire((token != NULL) && (strcmp(token, "single") == 0),
                            "strtok did not return a string holding no separator");
        VerifyStringRequire(strtok(NULL, ",") == NULL,
                            "strtok yielded a second token from a single one");
    }
}

/* ---------------------------------------------------------------------- 7.24.6 */

static void VerifyStringMiscellaneous(void)
{
    char *region;

    /*
     * memset with a byte above 127, which is the value the conversion to
     * unsigned char is visible at, and the value a caller reaches for when
     * poisoning memory.
     */
    region = VerifyStringPrepare();
    VerifyStringRequire(memset(region, 0xFF, 6U) == region,
                        "memset did not return its object");

    {
        bool filled = true;

        for (size_t index = 0U; index < 6U; ++index)
        {
            if ((unsigned char)region[index] != 0xFFU)
            {
                filled = false;
            }
        }

        VerifyStringRequire(filled, "memset did not fill with the byte it was given");
    }

    VerifyStringRequire(VerifyStringMarginIsIntact(6U),
                        "memset wrote outside the range it was given");

    /*
     * A length of zero writes nothing — the same do-while trap as memcpy's.
     *
     * The parentheses around the length are the second compiler's, and are the
     * remedy it documents rather than a suppression. clang's
     * -Wmemset-transposed-args reads `memset(p, 0xFF, 0)` as a caller who meant
     * `memset(p, 0, 0xFF)`, which is the far commoner mistake and a fair
     * diagnostic; parenthesising the argument is how a caller states that the
     * zero is deliberate. No warning is turned off, here or in the Makefile.
     */
    region = VerifyStringPrepare();
    (void)memset(region, 0xFF, (0U));
    VerifyStringRequire(VerifyStringMarginIsIntact(0U),
                        "memset of zero bytes wrote something");

    VerifyStringRequire(strlen("") == 0U,
                        "strlen of an empty string was not zero");
    VerifyStringRequire(strlen("Oxys") == 4U,
                        "strlen did not exclude the terminator");

    /*
     * A string holding a byte above 127 has the same length as one that does
     * not. An implementation that stopped upon a negative char would report two
     * here, which is a length that is wrong in the direction nothing faults
     * upon: short, so every copy made from it truncates.
     */
    VerifyStringRequire(strlen("ab\xFF" "cd") == 5U,
                        "strlen stopped upon a byte above 127");
}

void KernelVerifyString(void)
{
    VerifyStringSucceeded = true;

    KernelWriteString("String: asserting the C library's string and memory "
                      "functions.\n");

    VerifyStringCopying();
    VerifyStringComparison();
    VerifyStringSearch();
    VerifyStringTokens();
    VerifyStringMiscellaneous();

    KernelWriteString(VerifyStringSucceeded ? "String self-test passed.\n"
                                            : "String self-test FAILED.\n");
}
