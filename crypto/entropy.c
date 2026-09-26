/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: crypto/entropy.c
 * Purpose: Implements the entropy pool of sub-task 10.1: the mixing of a
 *          sample into 4096 bits of state, the conservative count of what the
 *          state holds, and the Repetition Count Test the jitter source is held
 *          to.
 * Key functions: EntropyPoolInitialise, EntropyAdd, EntropyAddJitter,
 *          EntropyBits, EntropyPoolCopy, EntropySystemPool, EntropyReport.
 * References:
 *   - NIST SP 800-90B, Section 4.4.1: the Repetition Count Test.
 *   - kernel/include/oxys/crypto/entropy.h: the credits and the cutoff.
 *   - docs/design/ENTROPY.md.
 *
 * Why the mixing is not cryptographic, and need not be.
 *
 *   Its one duty is never to destroy what the pool holds. Each step below is a
 *   bijection of the pool's state for a fixed sample — an exclusive-or, a
 *   multiplication by an odd constant, a shift-and-exclusive-or of a word into
 *   itself, an addition of a different word — so no two states before a sample
 *   become one state after it, and the uncertainty about the pool can only
 *   stay or grow. What it does not do is hide the samples from somebody who
 *   reads the words, which is the conditioning function's duty at the moment
 *   output is drawn, sub-task 10.2. A cryptographic mix here would be spent
 *   twice.
 *
 * Concurrency. Unsynchronised. The system's pool is seeded at boot, upon the
 * bootstrap processor, before any other processor is started or any interrupt
 * handler reaches it; the generator of 10.2 is what will reseed it while the
 * machine runs, and must bring a lock with it: docs/design/CONCURRENCY.md.
 */

#include <oxys/crypto/entropy.h>
#include <oxys/kernel.h>

/* An odd constant, so that the multiplication is invertible modulo 2^64: the
 * sixty-four bits of the golden ratio's fraction, which has no structure a
 * sample could line up with. */
#define ENTROPY_MULTIPLIER UINT64_C(0x9E3779B97F4A7C15)

/* The stride between the words successive samples land in. Odd, so that it
 * visits every word of the sixty-four before one is visited twice. */
#define ENTROPY_STRIDE 7U

static EntropyPool EntropySystem;

static const char *const EntropySourceNames[ENTROPY_SOURCE_COUNT] = { "rdseed", "rdrand",
                                                                      "jitter" };

static uint64_t EntropyRotate(uint64_t value, uint32_t by)
{
    by &= 63U;

    return (by == 0U) ? value : ((value << by) | (value >> (64U - by)));
}

void EntropyPoolInitialise(EntropyPool *pool)
{
    uint8_t *const bytes = (uint8_t *)pool;

    for (size_t index = 0U; index < sizeof *pool; ++index)
    {
        bytes[index] = 0U;
    }
}

void EntropyAdd(EntropyPool *pool, EntropySource source, uint64_t sample, uint32_t eighths)
{
    const uint32_t here = pool->position;
    const uint32_t before = (here + ENTROPY_POOL_WORDS - 1U) % ENTROPY_POOL_WORDS;
    uint64_t word = pool->words[here];
    const uint64_t ceiling = (uint64_t)ENTROPY_POOL_BITS * ENTROPY_EIGHTHS;

    /* Each a bijection of `word` for a fixed sample, and the last a bijection
     * of it for a fixed neighbour. */
    word ^= EntropyRotate(sample, pool->rotation);
    word *= ENTROPY_MULTIPLIER;
    word ^= word >> 29;
    word += pool->words[before];
    pool->words[here] = word;

    pool->position = (here + ENTROPY_STRIDE) % ENTROPY_POOL_WORDS;
    pool->rotation = (pool->rotation + 13U) & 63U;

    if ((uint32_t)source < (uint32_t)ENTROPY_SOURCE_COUNT)
    {
        ++pool->samples[source];
        pool->credited[source] += eighths;
    }

    pool->credit = ((pool->credit + eighths) > ceiling) ? ceiling : (pool->credit + eighths);
}

bool EntropyAddJitter(EntropyPool *pool, uint64_t counter)
{
    const uint64_t delta = counter - pool->jitter_last;
    const bool first = (pool->jitter_last == 0U);
    bool credit = false;

    pool->jitter_last = counter;

    if (!first)
    {
        /* SP 800-90B, Section 4.4.1: count identical consecutive samples, and
         * declare the source failed at the cutoff. A delta that differs resets
         * the count; one that repeats up to the cutoff is still credited, which
         * is what the test's false-positive bound was computed for. */
        if (delta == pool->jitter_delta)
        {
            ++pool->jitter_repeats;
        }
        else
        {
            pool->jitter_repeats = 1U;
        }

        pool->jitter_delta = delta;

        if (pool->jitter_repeats >= ENTROPY_JITTER_CUTOFF)
        {
            /* Counted once per run, where the run reaches the cutoff. */
            if (pool->jitter_repeats == ENTROPY_JITTER_CUTOFF)
            {
                ++pool->jitter_failures;
            }
        }
        else
        {
            credit = true;
        }
    }

    /* The whole reading is mixed in, credited or not: its high bits are
     * predictable and its low bits are the point, and mixing both costs
     * nothing the bijection does not give back. */
    EntropyAdd(pool, ENTROPY_SOURCE_JITTER, counter ^ EntropyRotate(delta, 32U),
               credit ? ENTROPY_JITTER_CREDIT : 0U);

    return credit;
}

uint64_t EntropyBits(const EntropyPool *pool)
{
    return pool->credit / ENTROPY_EIGHTHS;
}

void EntropyPoolCopy(const EntropyPool *pool, uint64_t *words)
{
    for (size_t index = 0U; index < ENTROPY_POOL_WORDS; ++index)
    {
        words[index] = pool->words[index];
    }
}

EntropyPool *EntropySystemPool(void)
{
    return &EntropySystem;
}

void EntropyReport(const EntropyPool *pool)
{
    for (size_t source = 0U; source < (size_t)ENTROPY_SOURCE_COUNT; ++source)
    {
        KernelWriteString("  ");
        KernelWriteString(EntropySourceNames[source]);
        KernelWriteString(": ");
        KernelWriteDecimal(pool->samples[source]);
        KernelWriteString(" sample(s), ");
        KernelWriteDecimal(pool->credited[source] / ENTROPY_EIGHTHS);
        KernelWriteString(" bit(s) credited.\n");
    }

    KernelWriteString("  The pool holds an estimated ");
    KernelWriteDecimal(EntropyBits(pool));
    KernelWriteString(" bit(s) of ");
    KernelWriteDecimal((uint64_t)ENTROPY_POOL_BITS);
    KernelWriteString("; the jitter source failed its repetition count test ");
    KernelWriteDecimal(pool->jitter_failures);
    KernelWriteString(" time(s).\n");
}
