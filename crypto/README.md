<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `crypto/` — Randomness and Cryptography

**Phase**: 10, from sub-task 10.1.
**Detailed design**: [`../docs/design/ENTROPY.md`](../docs/design/ENTROPY.md).

## Purpose

The kernel's cryptography, as Phase 10 builds it: the entropy pool of 10.1 now,
and later the generator of 10.2, SHA-256 of 10.3, AES of 10.4 and its modes of
10.5, written for this project.

The code here is portable. It includes no `<oxys/arch/...>` header: the
processor's sources of randomness are `kernel/arch/x86_64/cpu/random.c`, and
the boot phase that draws from them feeds this pool through its interface.

## Contents

| Path | Description |
| ---- | ----------- |
| [`entropy.c`](entropy.c) | Sub-task 10.1: the entropy pool. Samples mixed into 4096 bits by a step that never destroys what the pool holds, credit counted in eighths of a bit, and the Repetition Count Test of NIST SP 800-90B applied to the jitter source. The interface is [`../kernel/include/oxys/crypto/entropy.h`](../kernel/include/oxys/crypto/entropy.h). |

## Specifications implemented

- NIST SP 800-90B, Section 4.4.1: the Repetition Count Test.
- Intel DRNG Software Implementation Guide: the credits given to `RDSEED` and
  `RDRAND`, applied at boot in `../kernel/init/entropy.c`.

## Present limitations

The pool gathers and does not give; output comes with the generator of 10.2.
[`../docs/design/ENTROPY.md`](../docs/design/ENTROPY.md), Limitations.
