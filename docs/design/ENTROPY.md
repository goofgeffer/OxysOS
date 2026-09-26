<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Entropy Pool

**Phase**: sub-task 10.1 of [`../project/PLAN.md`](../project/PLAN.md), which
opens Phase 10.
**Source**: [`../../crypto/entropy.c`](../../crypto/entropy.c),
[`../../kernel/include/oxys/crypto/entropy.h`](../../kernel/include/oxys/crypto/entropy.h);
the processor's sources in
[`../../kernel/arch/x86_64/cpu/random.c`](../../kernel/arch/x86_64/cpu/random.c);
the seeding at boot in [`../../kernel/init/entropy.c`](../../kernel/init/entropy.c).
**Specifications**: Intel SDM, Volume 2A, "CPUID", and Volume 2B, "RDRAND",
"RDSEED" and "RDTSC"; the Intel DRNG Software Implementation Guide, Sections
3.2.3, 3.2.4, 5.2.1, 5.2.6 and 5.3.1; NIST SP 800-90B, Sections 4.4 and 4.4.1.
[`../project/REFERENCES.md`](../project/REFERENCES.md) records each.

## 1. What it is

A store of 4096 bits into which samples from the machine's sources of
randomness are mixed, with a count of how much of what it holds is
unpredictable. It gathers and it counts. It does not give: output must pass
through a cryptographic conditioning function, or it would expose the samples
that went in, and that function is the generator of sub-task 10.2.
`EntropyPoolCopy` is the one way out, for that step and the self-test.

**The mixing is not cryptographic, and need not be.** Its one duty is never to
destroy what the pool holds. A sample is exclusive-ored into one word after a
rotation. The word is then multiplied by an odd constant, has its own upper
bits exclusive-ored into it, and has its neighbour added. Each step is a
bijection of the pool's state for a fixed sample, so no two states become one
and the uncertainty about the pool can only stay or grow. Successive samples
land seven words apart, which visits all sixty-four before any twice. Hiding the
samples from somebody who reads the words is the conditioning function's duty,
at the moment output is drawn.

**Credit is counted in eighths of a bit** and never exceeds the pool's 4096
bits. A pool claiming more than it can hold would be claiming bits it
overwrote.

## 2. The sources and their credits

| Source | Where | Credit per 64-bit sample | Why that much |
| ------ | ----- | ------------------------ | ------------- |
| `RDSEED` | CPUID leaf 07H, EBX bit 18 | 64 bits | The DRNG guide, Section 3.2.4: its output is compliant with SP 800-90B and C, a seed and not a generator's output. |
| `RDRAND` | CPUID leaf 01H, ECX bit 30 | An eighth of a bit | It is a generator's output, reseeded at least every 1022 values (Section 3.2.3). The guide's own recipe for a seed from it is 512 128-bit values, 1024 of 64 bits, for 128 bits (Section 5.2.6). |
| Jitter | CPUID leaf 01H, EDX bit 4; `RDTSC` | Half a bit, while the health test passes | Section 3 below. |

**Retries** are the guide's. `RDRAND` is tried ten times (Section 5.2.1): it
fails only when momentarily drained, and ten failures in a row mean something is
wrong. `RDSEED` fails whenever its conditioner has not caught up, so it is given
up on after a hundred tries, each after a `PAUSE`. That is the upper end of
"somewhere between 1 and 100", the bound for a caller that must not wait
indefinitely (Section 5.3.1). Leaf 07H is read only where leaf 0 reports it
exists: a processor asked for a higher leaf than it has answers with its
highest, which would be read as features it lacks.

## 3. Jitter, and its health test

A jitter sample is a reading of the time-stamp counter taken after a short piece
of work: 32 reads of a 4 KiB buffer at positions that depend on the previous
delta, so that each sample times a different path through the caches. The delta
between readings is what varies. The whole reading is mixed in; the delta is
what is tested and credited.

**Half a bit per sample is assumed, not assessed.** SP 800-90B, Section 3, asks
for an entropy source's min-entropy to be established by statistical assessment
of the noise source on its hardware. That has not been done here, and cannot be
done once for every machine this boots on. Half a bit is low for real hardware.
Where an emulator's counter is too regular even for that, the test below is what
notices.

**The Repetition Count Test** of SP 800-90B, Section 4.4.1, counts identical
consecutive samples and declares the source failed at a cutoff
C = 1 + ⌈−log₂ α / H⌉. With α = 2⁻²⁰, the value Section 4.4 uses throughout, and
H = ½, C is 41. A delta repeated 41 times is a counter that has stopped, or an
emulator counting instructions rather than time. The run is counted as one
failure, and nothing from it is credited until the delta moves again. The first
reading has no delta and is credited nothing.

## 4. Seeding at boot

`KernelInitialiseEntropy` runs after the devices of Phase 4, so the counter is
read against a machine doing something. It seeks 256 bits, the highest security
strength SP 800-90A gives its generators, and therefore what 10.2's generator
will be seeded at:

1. `RDSEED`, at most 64 draws, until the target is met.
2. `RDRAND`, if still short, at most 2048 draws, which is exactly the target at
   an eighth of a bit each.
3. **Jitter always**, at least 512 samples even where the processor met the
   target. A pool resting on the processor alone rests on one component nobody
   here can inspect. Beyond that, jitter is drawn until the target is met or
   16384 samples have been tried.

A pool left short of the target says so on the boot log. It does not stop the
boot: nothing draws from the pool until 10.2, and that generator is where a pool
too thin to seed from must be refused.

The system's pool is written only here, at boot, on the bootstrap processor,
before any other processor starts, so it has no lock
([`CONCURRENCY.md`](CONCURRENCY.md)). 10.2's reseeding while the machine runs
will need one.

## Verification

`KernelVerifyEntropy`, run as the last step of the seeding, on pools of its own:

| Asserted | The failure it would catch |
| -------- | -------------------------- |
| A new pool holds nothing; fifteen eighths are one bit, rounded down; a hundred full-entropy samples hold the credit at 4096. | A pool reported seeded that is not: a credit that rounds up or overflows. |
| A sample changes the pool; two different samples leave different pools; a bit that differs in the first of 1001 samples still shows at the end. | A mixing that drops samples or merges states, losing what the credit says is there. |
| A repeated delta is credited exactly C − 1 times; the run past the cutoff is one failure; a delta that moves again is credited at once. | A stopped counter credited forever, or a working one never credited again. |
| Where the processor has `RDSEED` or `RDRAND`, eight draws succeed and are not all one value; the counter advances; the system's pool holds jitter. | A generator returning a constant, as some processors' `RDRAND` has done. |

**Every path is exercised by a processor model QEMU offers**: `-cpu qemu64` has
neither instruction and seeds from jitter alone; `-cpu qemu64,+rdrand` seeds
from 2048 values of `RDRAND`; `-cpu max`, like VirtualBox upon a recent host,
from four values of `RDSEED`.

**Bochs is the caution.** It has neither instruction, and its jitter passes the
test and is credited the full target. But its counter is expected to advance
with the instructions it emulates rather than with time, so the deltas there
vary because the work's memory pattern varies, which is deterministic. The
Repetition Count Test catches a source that has stopped, not one that moves
predictably; that is limitation 2.

What cannot be asserted is that the pool is unpredictable. No test of a
process's output establishes that; the credits are an argument, made above, and
the self-test asserts that the arithmetic carrying them out is what the argument
says.

## Limitations

1. **Nothing draws from it yet.** The generator of 10.2 is the first consumer,
   and the conditioning that makes the pool safe to read is its duty.
2. **Jitter's half a bit is assumed**, not assessed per SP 800-90B, Section 3.
3. **Seeded once, at boot.** Interrupts, keyboard and mouse timings, and disk
   completions are not yet sources; each would need the pool locked first.
4. **Only the Repetition Count Test.** SP 800-90B's second approved test, the
   Adaptive Proportion Test of Section 4.4.2, is not run. It catches a source
   that has lost entropy without sticking on one value.
5. **No lock**, as Section 4 says.
