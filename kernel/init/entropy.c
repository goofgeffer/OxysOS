/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/init/entropy.c
 * Purpose: The seeding of the system's entropy pool at boot, sub-task 10.1:
 *          RDSEED where the processor has it, RDRAND where it has that, and the
 *          jitter of the time-stamp counter always, until the pool holds
 *          KERNEL_ENTROPY_TARGET bits or every source is spent.
 * Key functions: KernelInitialiseEntropy.
 * References:
 *   - docs/design/ARCHITECTURE.md, Section 4: where this phase stands.
 *   - Intel DRNG Software Implementation Guide, Section 5.2.6: a 128-bit seed
 *     from 512 128-bit values of RDRAND, which is the eighth of a bit a 64-bit
 *     value is credited with here.
 *   - docs/design/ENTROPY.md: every credit, and why jitter is drawn even where
 *     the processor's sources have filled the target.
 */

#include "internal.h"
#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/arch/cpu/random.h>
#include <oxys/crypto/entropy.h>

/*
 * The credit sought: 256 bits, the highest security strength NIST SP 800-90A
 * gives its generators, which is what the generator of sub-task 10.2 will be
 * seeded at.
 */
#define KERNEL_ENTROPY_TARGET 256U

/* RDSEED's value is full entropy (the DRNG guide, Section 3.2.4: compliant with
 * SP 800-90B and C); RDRAND's is a generator's output, an eighth of a bit. */
#define KERNEL_ENTROPY_SEED_CREDIT  (64U * ENTROPY_EIGHTHS)
#define KERNEL_ENTROPY_VALUE_CREDIT 1U

/* The most draws made of each source, so that a source that fails or credits
 * nothing costs a bounded time at boot. RDRAND's bound is the draws its credit
 * needs to reach the target on its own. */
#define KERNEL_ENTROPY_SEED_DRAWS   64U
#define KERNEL_ENTROPY_VALUE_DRAWS  (KERNEL_ENTROPY_TARGET * ENTROPY_EIGHTHS)
#define KERNEL_ENTROPY_JITTER_MINIMUM 512U
#define KERNEL_ENTROPY_JITTER_MAXIMUM 16384U

/*
 * The work a jitter sample times: reads of a buffer at positions that depend on
 * the previous delta, so that each measures a different path through the
 * caches and the memory beneath them. What it computes is thrown away; the
 * time it took is the sample.
 */
static volatile uint8_t KernelEntropyScratch[4096];

static uint64_t KernelEntropyJitterWork(uint64_t previous)
{
    uint64_t sum = 0U;
    size_t position = (size_t)(previous & (sizeof KernelEntropyScratch - 1U));

    for (size_t step = 0U; step < 32U; ++step)
    {
        sum += KernelEntropyScratch[position];
        KernelEntropyScratch[position] = (uint8_t)(sum + step);
        position = (position + 67U + (size_t)(sum & 0xFFU)) & (sizeof KernelEntropyScratch - 1U);
    }

    return sum;
}

void KernelInitialiseEntropy(void)
{
    EntropyPool *const pool = EntropySystemPool();
    uint64_t value;
    uint64_t previous = 0U;

    EntropyPoolInitialise(pool);

    for (uint32_t draw = 0U; (draw < KERNEL_ENTROPY_SEED_DRAWS) &&
                             (EntropyBits(pool) < KERNEL_ENTROPY_TARGET);
         ++draw)
    {
        if (ArchRandomSeed(&value))
        {
            EntropyAdd(pool, ENTROPY_SOURCE_SEED, value, KERNEL_ENTROPY_SEED_CREDIT);
        }
    }

    for (uint32_t draw = 0U; (draw < KERNEL_ENTROPY_VALUE_DRAWS) &&
                             (EntropyBits(pool) < KERNEL_ENTROPY_TARGET);
         ++draw)
    {
        if (!ArchRandomValue(&value))
        {
            break;
        }

        EntropyAdd(pool, ENTROPY_SOURCE_VALUE, value, KERNEL_ENTROPY_VALUE_CREDIT);
    }

    /*
     * Jitter is drawn whether or not the processor filled the target, at least
     * KERNEL_ENTROPY_JITTER_MINIMUM samples: a pool resting upon the processor
     * alone rests upon one component nobody here can inspect. Beyond that it is
     * drawn only until the target is met, and never beyond the maximum, which
     * is where a counter that has stopped (an emulator that counts
     * instructions, not time) is given up on rather than waited for.
     */
    if (ArchRandomHasCycleCounter())
    {
        for (uint32_t sample = 0U;
             (sample < KERNEL_ENTROPY_JITTER_MAXIMUM) &&
             ((sample < KERNEL_ENTROPY_JITTER_MINIMUM) ||
              (EntropyBits(pool) < KERNEL_ENTROPY_TARGET));
             ++sample)
        {
            previous = KernelEntropyJitterWork(previous) ^ pool->jitter_delta;
            (void)EntropyAddJitter(pool, ArchCycleCounter());
        }
    }

    KernelWriteString("Entropy: RDSEED ");
    KernelWriteString(ArchRandomHasSeed() ? "present" : "absent");
    KernelWriteString(", RDRAND ");
    KernelWriteString(ArchRandomHasValue() ? "present" : "absent");
    KernelWriteString(", time-stamp counter ");
    KernelWriteString(ArchRandomHasCycleCounter() ? "present" : "absent");
    KernelWriteString(".\n");
    EntropyReport(pool);

    if (EntropyBits(pool) < KERNEL_ENTROPY_TARGET)
    {
        KernelWriteString("  The pool is short of the ");
        KernelWriteDecimal(KERNEL_ENTROPY_TARGET);
        KernelWriteString(" bits sought: every source was spent.\n");
    }

    KernelVerifyEntropy();
}
