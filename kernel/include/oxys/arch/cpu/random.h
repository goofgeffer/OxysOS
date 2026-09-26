/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/include/oxys/arch/cpu/random.h
 * Purpose: Declares the processor's sources of randomness for the entropy pool
 *          of sub-task 10.1: the RDSEED and RDRAND instructions, where the
 *          processor has them, and the time-stamp counter the jitter of the
 *          pool's third source is measured with.
 * Key definitions: ArchRandomHasSeed, ArchRandomHasValue, ArchRandomSeed,
 *          ArchRandomValue, ArchRandomHasCycleCounter, ArchCycleCounter,
 *          ARCH_RANDOM_VALUE_RETRIES, ARCH_RANDOM_SEED_RETRIES.
 * References:
 *   - Intel SDM, Volume 2A, "CPUID": leaf 01H, ECX bit 30 (RDRAND) and EDX
 *     bit 4 (TSC); leaf 07H, sub-leaf 0, EBX bit 18 (RDSEED); leaf 0, EAX, the
 *     highest basic leaf, which must reach 07H before that leaf is read.
 *   - Intel SDM, Volume 2B, "RDRAND" and "RDSEED": CF=1 where the destination
 *     holds a valid value; otherwise CF=0, the destination is zero, and software
 *     is expected to retry.
 *   - Intel SDM, Volume 2B, "RDTSC": EDX:EAX receives the counter; the
 *     instruction is not serialising.
 *   - Intel Digital Random Number Generator (DRNG) Software Implementation
 *     Guide, Section 5.2.1 (ten retries of RDRAND) and Section 5.3.1 (RDSEED
 *     given up after "somewhere between 1 and 100" retries, with PAUSE).
 *   - docs/design/ENTROPY.md.
 */

#ifndef OXYS_ARCH_CPU_RANDOM_H
#define OXYS_ARCH_CPU_RANDOM_H

#include <oxys/types.h>

/*
 * How many times each instruction is tried before a draw is reported failed.
 * RDRAND fails only when its generator is momentarily drained, and the DRNG
 * guide's ten is its own statement that ten failures in a row mean something
 * is wrong with the processor. RDSEED fails whenever its conditioner has not
 * caught up, which under load is often; the guide's bound for a caller that
 * must not wait indefinitely is at most a hundred, each after a PAUSE.
 */
#define ARCH_RANDOM_VALUE_RETRIES 10U
#define ARCH_RANDOM_SEED_RETRIES  100U

/* Whether the processor has the instruction, from CPUID; read once. */
bool ArchRandomHasSeed(void);
bool ArchRandomHasValue(void);
bool ArchRandomHasCycleCounter(void);

/*
 * One 64-bit value from RDSEED or RDRAND, retried as above. False, with
 * `*value` zero, where the processor lacks the instruction or every retry
 * failed; the caller decides what that costs.
 */
bool ArchRandomSeed(uint64_t *value);
bool ArchRandomValue(uint64_t *value);

/* The time-stamp counter, or zero where the processor has none. */
uint64_t ArchCycleCounter(void);

#endif /* OXYS_ARCH_CPU_RANDOM_H */
