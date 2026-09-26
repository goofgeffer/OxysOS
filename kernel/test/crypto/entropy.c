/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/crypto/entropy.c
 * Purpose: Asserts the entropy pool of sub-task 10.1 upon pools of its own —
 *          the credit and its ceiling, the mixing, the Repetition Count Test —
 *          and the processor's sources where the processor has them.
 * Key functions: KernelVerifyEntropy.
 * References:
 *   - NIST SP 800-90B, Section 4.4.1: the test's cutoff and what it counts.
 *   - docs/design/ENTROPY.md: the property each assertion establishes, and
 *     the silent failure each would catch.
 *
 * What is not asserted, because it cannot be: that the pool's contents are
 * unpredictable. No test of the output of a process can establish that; the
 * credits are an argument, made in the design document, and what is asserted
 * here is that the arithmetic carrying them out is what the argument says.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/arch/cpu/random.h>
#include <oxys/crypto/entropy.h>

static bool KernelEntropySucceeded;

static void KernelEntropyRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        KernelEntropySucceeded = false;
    }
}

/* Two pools and their words, the self-test's own. */
static EntropyPool KernelEntropyFirst;
static EntropyPool KernelEntropySecond;
static uint64_t KernelEntropyWordsFirst[ENTROPY_POOL_WORDS];
static uint64_t KernelEntropyWordsSecond[ENTROPY_POOL_WORDS];

static bool KernelEntropySame(void)
{
    EntropyPoolCopy(&KernelEntropyFirst, KernelEntropyWordsFirst);
    EntropyPoolCopy(&KernelEntropySecond, KernelEntropyWordsSecond);

    for (size_t index = 0U; index < ENTROPY_POOL_WORDS; ++index)
    {
        if (KernelEntropyWordsFirst[index] != KernelEntropyWordsSecond[index])
        {
            return false;
        }
    }

    return true;
}

/* The credit: counted in eighths, and never above the pool's extent. A credit
 * that overflowed or rounded upward would report a pool as seeded that is not,
 * which nothing downstream could notice. */
static void KernelVerifyEntropyCredit(void)
{
    EntropyPoolInitialise(&KernelEntropyFirst);
    KernelEntropyRequire(EntropyBits(&KernelEntropyFirst) == 0U, "a new pool was credited");

    for (uint64_t index = 0U; index < 15U; ++index)
    {
        EntropyAdd(&KernelEntropyFirst, ENTROPY_SOURCE_VALUE, index + 1U, 1U);
    }

    KernelEntropyRequire(EntropyBits(&KernelEntropyFirst) == 1U,
                         "fifteen eighths of a bit were not counted as one bit, rounded down");

    for (uint64_t index = 0U; index < 100U; ++index)
    {
        EntropyAdd(&KernelEntropyFirst, ENTROPY_SOURCE_SEED, index, 64U * ENTROPY_EIGHTHS);
    }

    KernelEntropyRequire(EntropyBits(&KernelEntropyFirst) == ENTROPY_POOL_BITS,
                         "the credit was not held at the pool's extent");
}

/* The mixing: a sample changes the pool, a different sample changes it
 * differently, and one bit early in a long sequence is not lost by the end. A
 * mix that dropped samples, or merged two states into one, would lose what the
 * credit says is there. */
static void KernelVerifyEntropyMixing(void)
{
    EntropyPoolInitialise(&KernelEntropyFirst);
    EntropyPoolInitialise(&KernelEntropySecond);
    EntropyAdd(&KernelEntropyFirst, ENTROPY_SOURCE_VALUE, 1U, 0U);
    KernelEntropyRequire(!KernelEntropySame(), "a sample left the pool as it was");

    EntropyAdd(&KernelEntropySecond, ENTROPY_SOURCE_VALUE, 2U, 0U);
    KernelEntropyRequire(!KernelEntropySame(), "two different samples left the same pool");

    EntropyPoolInitialise(&KernelEntropyFirst);
    EntropyPoolInitialise(&KernelEntropySecond);
    EntropyAdd(&KernelEntropyFirst, ENTROPY_SOURCE_VALUE, UINT64_C(0x5A5A), 0U);
    EntropyAdd(&KernelEntropySecond, ENTROPY_SOURCE_VALUE, UINT64_C(0x5A5B), 0U);

    for (uint64_t index = 0U; index < 1000U; ++index)
    {
        const uint64_t sample = index * UINT64_C(0x0123456789ABCDEF);

        EntropyAdd(&KernelEntropyFirst, ENTROPY_SOURCE_VALUE, sample, 0U);
        EntropyAdd(&KernelEntropySecond, ENTROPY_SOURCE_VALUE, sample, 0U);
    }

    KernelEntropyRequire(!KernelEntropySame(),
                         "a bit mixed in first was lost after a thousand samples more");
}

/*
 * The Repetition Count Test: a delta repeated is credited up to the cutoff less
 * one, the run reaching the cutoff is one failure and credits nothing more, and
 * a delta that moves again is credited at once. A test that never tripped would
 * credit a stopped counter forever.
 */
static void KernelVerifyEntropyRepetition(void)
{
    uint64_t counter = 1000U;
    uint32_t credited = 0U;

    EntropyPoolInitialise(&KernelEntropyFirst);

    KernelEntropyRequire(!EntropyAddJitter(&KernelEntropyFirst, counter),
                         "the first reading, which has no delta, was credited");

    for (uint32_t index = 0U; index < (ENTROPY_JITTER_CUTOFF + 20U); ++index)
    {
        counter += 100U;
        credited += EntropyAddJitter(&KernelEntropyFirst, counter) ? 1U : 0U;
    }

    KernelEntropyRequire(credited == (ENTROPY_JITTER_CUTOFF - 1U),
                         "a repeated delta was not credited exactly to the cutoff");
    KernelEntropyRequire(KernelEntropyFirst.jitter_failures == 1U,
                         "a run past the cutoff was not one failure");

    counter += 101U;
    KernelEntropyRequire(EntropyAddJitter(&KernelEntropyFirst, counter),
                         "a delta that moved again was not credited");
}

/* The processor's sources, where it has them: every draw succeeds, and eight
 * are not all one value. The test cannot tell a good generator from a bad one;
 * it can tell one that returns a constant, which is the failure that has
 * shipped in processors before. */
static void KernelVerifyEntropySources(void)
{
    if (ArchRandomHasSeed() || ArchRandomHasValue())
    {
        const bool seed = ArchRandomHasSeed();
        uint64_t first = 0U;
        bool varied = false;
        bool drawn = true;

        for (uint32_t index = 0U; index < 8U; ++index)
        {
            uint64_t value = 0U;

            drawn = (seed ? ArchRandomSeed(&value) : ArchRandomValue(&value)) && drawn;

            if (index == 0U)
            {
                first = value;
            }
            else if (value != first)
            {
                varied = true;
            }
        }

        KernelEntropyRequire(drawn, "the processor's random instruction failed every retry");
        KernelEntropyRequire(varied, "the processor's random instruction returned one value");
    }

    if (ArchRandomHasCycleCounter())
    {
        const uint64_t before = ArchCycleCounter();

        KernelEntropyRequire(ArchCycleCounter() > before,
                             "the time-stamp counter did not advance");
        KernelEntropyRequire(EntropySystemPool()->samples[ENTROPY_SOURCE_JITTER] > 0U,
                             "the system's pool was seeded without jitter");
    }
}

void KernelVerifyEntropy(void)
{
    KernelEntropySucceeded = true;

    KernelVerifyEntropyCredit();
    KernelVerifyEntropyMixing();
    KernelVerifyEntropyRepetition();
    KernelVerifyEntropySources();

    KernelWriteString(KernelEntropySucceeded
                          ? "Entropy self-test passed: the credit and its ceiling, the mixing, "
                            "the repetition count test, and the processor's sources present.\n"
                          : "Entropy self-test FAILED.\n");
}
