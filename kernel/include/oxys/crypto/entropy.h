/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/crypto/entropy.h
 * Purpose: Declares the entropy pool of sub-task 10.1: a store of 4096 bits
 *          into which samples from the machine's sources of randomness are
 *          mixed, with a count, deliberately conservative, of how much of what
 *          it holds is unpredictable.
 * Key definitions: EntropyPool, EntropySource, ENTROPY_POOL_WORDS,
 *          ENTROPY_EIGHTHS, ENTROPY_JITTER_CUTOFF, EntropyPoolInitialise,
 *          EntropyAdd, EntropyAddJitter, EntropyBits, EntropyPoolCopy,
 *          EntropySystemPool, EntropyReport.
 * References:
 *   - NIST SP 800-90B, Section 4.4.1: the Repetition Count Test the jitter
 *     source is held to, and its cutoff C = 1 + ceil(-log2(alpha) / H) at the
 *     alpha = 2^-20 of Section 4.4.
 *   - docs/design/ENTROPY.md: the design, every credit and why.
 *
 * What this is not yet. It gathers and it counts; it does not give. Output
 * drawn from the pool must pass through a cryptographic conditioning function,
 * or it would expose the samples that went in, and that function is the
 * generator of sub-task 10.2 upon the hash of 10.3. EntropyPoolCopy is the one
 * way out, for that step alone.
 */

#ifndef OXYS_CRYPTO_ENTROPY_H
#define OXYS_CRYPTO_ENTROPY_H

#include <oxys/types.h>

/* The pool's extent: sixty-four words, 4096 bits, which is also the most it
 * can be credited with holding. */
#define ENTROPY_POOL_WORDS 64U
#define ENTROPY_POOL_BITS  (ENTROPY_POOL_WORDS * 64U)

/*
 * Credit is counted in eighths of a bit, because the honest credit for one
 * value of RDRAND is an eighth: docs/design/ENTROPY.md, Section 2.
 */
#define ENTROPY_EIGHTHS 8U

/*
 * The jitter source's assumed min-entropy, in eighths of a bit per sample, and
 * the Repetition Count Test's cutoff for it: C = 1 + ceil(20 / H) with H one
 * half, which is 41. A delta repeated 41 times in a row is a source that has
 * stopped, and nothing it yields is credited until it moves again.
 */
#define ENTROPY_JITTER_CREDIT 4U
#define ENTROPY_JITTER_CUTOFF 41U

/* Where a sample came from, for the accounting. */
typedef enum EntropySource
{
    ENTROPY_SOURCE_SEED = 0,  /* RDSEED */
    ENTROPY_SOURCE_VALUE,     /* RDRAND */
    ENTROPY_SOURCE_JITTER,    /* The time-stamp counter's variation */
    ENTROPY_SOURCE_COUNT
} EntropySource;

typedef struct EntropyPool
{
    uint64_t words[ENTROPY_POOL_WORDS];

    /* Where the next sample goes, and the rotation it is given. */
    uint32_t position;
    uint32_t rotation;

    /* The credit, in eighths of a bit, never above the pool's extent. */
    uint64_t credit;

    /* Per source: samples mixed in, and eighths credited for them. */
    uint64_t samples[ENTROPY_SOURCE_COUNT];
    uint64_t credited[ENTROPY_SOURCE_COUNT];

    /* The jitter source's state: the last counter reading, the last delta,
     * how many times in a row it has repeated, and how often the Repetition
     * Count Test has failed. */
    uint64_t jitter_last;
    uint64_t jitter_delta;
    uint32_t jitter_repeats;
    uint64_t jitter_failures;
} EntropyPool;

/* Clears a pool: no samples, no credit. */
void EntropyPoolInitialise(EntropyPool *pool);

/*
 * Mixes one sample in and credits it `eighths`, the credit capped at the
 * pool's extent. The mixing never lessens what the pool holds, so a sample of
 * no entropy at all costs nothing but the time.
 */
void EntropyAdd(EntropyPool *pool, EntropySource source, uint64_t sample, uint32_t eighths);

/*
 * Mixes one reading of the time-stamp counter in. The delta from the last
 * reading is what varies; it is held to the Repetition Count Test and credited
 * ENTROPY_JITTER_CREDIT eighths only while the test passes. The first reading
 * only sets the reference and is credited nothing. Returns whether the sample
 * was credited.
 */
bool EntropyAddJitter(EntropyPool *pool, uint64_t counter);

/* The pool's credit, in whole bits. */
uint64_t EntropyBits(const EntropyPool *pool);

/* Copies the pool's words out, for the conditioning of sub-task 10.2 and for
 * the self-test. Never to be given to anything as random output as it is. */
void EntropyPoolCopy(const EntropyPool *pool, uint64_t *words);

/* The system's pool, seeded at boot by kernel/init/entropy.c. */
EntropyPool *EntropySystemPool(void);

/* Emits one line per source and one for the whole. */
void EntropyReport(const EntropyPool *pool);

#endif /* OXYS_CRYPTO_ENTROPY_H */
