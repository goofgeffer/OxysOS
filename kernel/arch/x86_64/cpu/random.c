/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/arch/x86_64/cpu/random.c
 * Purpose: Implements the processor's sources of randomness, sub-task 10.1:
 *          the detection of RDSEED, RDRAND and the time-stamp counter, and the
 *          three instructions with the retries Intel's guide prescribes.
 * Key functions: ArchRandomHasSeed, ArchRandomHasValue,
 *          ArchRandomHasCycleCounter, ArchRandomSeed, ArchRandomValue,
 *          ArchCycleCounter.
 * References:
 *   - kernel/include/oxys/arch/cpu/random.h: the citations, which are the
 *     SDM's CPUID, RDRAND, RDSEED and RDTSC pages and the DRNG guide.
 *   - docs/design/ENTROPY.md.
 *
 * The instructions are written out with their mnemonics rather than taken from
 * <immintrin.h>, which is a compiler-supplied header rather than a standard
 * one, for the reason percpu.c records for CPUID.
 *
 * Concurrency. The three detections are read once and cached; two processors
 * racing to cache them write the same values. The instructions themselves are
 * the processor's and need nothing.
 */

#include <oxys/arch/cpu/random.h>

/* Whether the detection has run, and what it found. */
static bool RandomDetected;
static bool RandomHasSeed;
static bool RandomHasValue;
static bool RandomHasCounter;

static void RandomCpuid(uint32_t leaf, uint32_t subleaf, uint32_t *eax, uint32_t *ebx,
                        uint32_t *ecx, uint32_t *edx)
{
    __asm__ __volatile__("cpuid"
                         : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                         : "a"(leaf), "c"(subleaf));
}

static void RandomDetect(void)
{
    uint32_t eax = 0U;
    uint32_t ebx = 0U;
    uint32_t ecx = 0U;
    uint32_t edx = 0U;
    uint32_t highest;

    if (RandomDetected)
    {
        return;
    }

    RandomCpuid(0U, 0U, &eax, &ebx, &ecx, &edx);
    highest = eax;

    RandomCpuid(1U, 0U, &eax, &ebx, &ecx, &edx);
    RandomHasValue = (ecx & (UINT32_C(1) << 30)) != 0U;
    RandomHasCounter = (edx & (UINT32_C(1) << 4)) != 0U;

    /* Leaf 07H is read only where leaf 0 says it exists: a processor asked for
     * a leaf beyond its highest returns the highest's values, which would be
     * read as features it does not have. */
    if (highest >= 7U)
    {
        RandomCpuid(7U, 0U, &eax, &ebx, &ecx, &edx);
        RandomHasSeed = (ebx & (UINT32_C(1) << 18)) != 0U;
    }

    RandomDetected = true;
}

bool ArchRandomHasSeed(void)
{
    RandomDetect();

    return RandomHasSeed;
}

bool ArchRandomHasValue(void)
{
    RandomDetect();

    return RandomHasValue;
}

bool ArchRandomHasCycleCounter(void)
{
    RandomDetect();

    return RandomHasCounter;
}

bool ArchRandomSeed(uint64_t *value)
{
    *value = 0U;

    if (!ArchRandomHasSeed())
    {
        return false;
    }

    for (uint32_t attempt = 0U; attempt < ARCH_RANDOM_SEED_RETRIES; ++attempt)
    {
        uint64_t drawn;
        uint8_t valid;

        __asm__ __volatile__("rdseed %0\n\tsetc %1" : "=r"(drawn), "=qm"(valid) : : "cc");

        if (valid != 0U)
        {
            *value = drawn;

            return true;
        }

        __asm__ __volatile__("pause");
    }

    return false;
}

bool ArchRandomValue(uint64_t *value)
{
    *value = 0U;

    if (!ArchRandomHasValue())
    {
        return false;
    }

    for (uint32_t attempt = 0U; attempt < ARCH_RANDOM_VALUE_RETRIES; ++attempt)
    {
        uint64_t drawn;
        uint8_t valid;

        __asm__ __volatile__("rdrand %0\n\tsetc %1" : "=r"(drawn), "=qm"(valid) : : "cc");

        if (valid != 0U)
        {
            *value = drawn;

            return true;
        }
    }

    return false;
}

uint64_t ArchCycleCounter(void)
{
    uint32_t low;
    uint32_t high;

    if (!ArchRandomHasCycleCounter())
    {
        return 0U;
    }

    __asm__ __volatile__("rdtsc" : "=a"(low), "=d"(high));

    return ((uint64_t)high << 32) | low;
}
