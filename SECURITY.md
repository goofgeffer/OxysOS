<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Security Policy

**Authority**: This document. It records what this project defends today, what it
does not, how to report a defect that bears on either, and what follows from a
report. [`PROJECT_GUIDELINES.md`](PROJECT_GUIDELINES.md) binds the work;
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) governs conduct; this document
governs neither.

## 1. Status, plainly

**Oxys-OS is an operating system under construction and has never been
released.** There is no version to run, no installer, no package, and no user
whose machine or data depends upon it. It boots from an ISO image, prints a boot
log, runs its own self-tests, and stops.

Nothing here should be understood as claiming that this kernel is safe to run
anything of value upon. It is not, and Sections 3 and 4 say exactly why in terms
that can be checked against the source rather than taken on trust.

This document nevertheless exists, and for a reason that is not ceremony: a
project that has published a boundary is obliged to say where the boundary is.
There is one such boundary already, described in Section 2, and it is real
enough that a defect in it would be a genuine finding.

## 2. What is actually being defended

Since sub-task 6.10 the kernel loads a statically linked ELF64 executable into an
address space of its own and enters it at privilege level 3. From that moment
there is a security boundary in this system, and it is the ordinary one: **the
program at privilege level 3 must not be able to read, write or execute anything
of the kernel's that the kernel did not deliberately expose.**

Three surfaces carry that boundary, and they are where a report is most likely to
be worth having.

| Surface | Where | What crossing it would mean |
| ------- | ----- | --------------------------- |
| The system-call interface | `kernel/arch/x86_64/syscall/syscall.c`, `kernel/arch/x86_64/syscall/syscall_entry.asm` | Seven calls, each of which receives arguments a program chose. Every address a caller supplies is validated against the canonical user limit **and** against the paging hierarchy, so that a mapped kernel page is not read on a caller's behalf merely because it is mapped. A gap here is a program reading kernel memory. |
| The ELF64 loader | `kernel/exec/elf.c` | A piece of the kernel that does what an untrusted document tells it to. Its design is the list of things it refuses to be told; [`docs/design/EXECUTABLE.md`](docs/design/EXECUTABLE.md) enumerates them. A gap here is a file placing a segment where it should not go. |
| The privilege apparatus | `kernel/arch/x86_64/cpu/tss.c`, `kernel/arch/x86_64/cpu/gdt.c` | The descriptors, the trusted stacks, and the task state segment whose I/O map base lies beyond the segment limit — which is what denies every I/O port to privilege level 3. A gap here is a program touching hardware directly. |

The filesystem is **not** among them. There are no users, no credentials and no
permission checks; anything with access to the volume has access to all of it.
That is a property of the system today and not a defect to report.

## 3. What is in force today

Each of these can be verified in the source, and the citation is given so that it
can be.

| Protection | Where |
| ---------- | ----- |
| Kernel text and read-only data are mapped without write permission | `kernel/arch/x86_64/mm/paging.c`, `PagingAddressIsReadOnly` |
| `CR0.WP` is set, without which those mappings would be advisory and the kernel could write straight through them | `PagingInitialise`; [`docs/design/INTERRUPTS.md`](docs/design/INTERRUPTS.md) |
| Every thread's kernel stack sits above a guard page, left mapped and read-only so that an overflow faults upon the write that overflows | `kernel/proc/process.c` |
| The double fault is delivered upon a stack of its own, so a fault taken upon a bad stack is reported rather than escalating to a reset | `kernel/arch/x86_64/interrupt/exceptions.c`; [`docs/design/PRIVILEGE.md`](docs/design/PRIVILEGE.md) |
| Every I/O port is denied to privilege level 3 | The I/O map base of the task state segment |
| A fault raised at privilege level 3 ends that program and not the machine | `ExceptionDispositionOf`; [`docs/design/INTERRUPTS.md`](docs/design/INTERRUPTS.md) |
| A machine's own root volume is mounted read-only unless the operator chose the GRUB entry that permits writing | `kernel/kernel.c`, `KernelMountRootVolume` |
| The self-tests that write to a real medium run only when the boot loader's command line asks for them | [`docs/project/TESTING.md`](docs/project/TESTING.md) |

The last two are the only respect in which this kernel currently protects
anything belonging to the person running it, and both exist for the same reason:
a kernel that wrote to a stranger's disk merely by having been booted would
impose a real cost for nothing.

## 4. What is not in force, and when it is due

Stated in full, because a security policy that lists only what is defended is
misleading by omission.

| Absent | Consequence | Due |
| ------ | ----------- | --- |
| **NX / execute-disable.** `IA32_EFER.NXE` is not set, so no page is non-executable. | Every writable page is also executable, kernel and user alike. | Sub-task 13.3 |
| **SMEP.** The `CR4` bit is named in `kernel/include/oxys/arch/cpu/cpu.h` and never written. | The kernel could be induced to execute a user page. | Sub-task 13.3 |
| **SMAP.** Likewise named and never written. | The kernel's reads of user memory are unguarded by hardware; only the software validation of Section 2 stands between them. | Sub-task 13.3 |
| **Stack canaries.** The build uses `-fno-stack-protector`, the canary requiring runtime support that does not exist. | A stack overwrite in the kernel is not detected at return. | Sub-task 13.4 |
| **Kernel address-space layout randomisation.** | Every kernel address is the same on every boot and is printed in the boot log. | Sub-task 13.5, marked *if feasible* |
| **User address-space layout randomisation.** | A program is loaded where its headers ask. | Not yet planned |
| **Any filesystem permission model.** No users, no credentials, no ownership. | Whatever can reach a volume can do anything to it. | Phase 7 at the earliest |
| **Any network stack.** Phase 11 has not begun. | There is no remote attack surface whatever, which is the one respect in which this kernel is currently very secure. | Phase 11 |
| **Concurrency safety.** Since sub-task 6.14 more than one processor is running, but a started processor has nothing to run: it answers inter-processor interrupts and halts. One structure has been put under a lock — the diagnostic channel, which is the whole of what such a processor touches. | Every other shared structure is unsynchronised by design and is documented as such, each in its own file's header. `docs/design/CONCURRENCY.md` enumerates them; `docs/design/SMP.md` records what a started processor may and may not reach. | Sub-task 6.15 |
| **Cryptography of any kind.** | No hashing, no ciphers, no random number generation. | Phase 10 |

None of these is an oversight, and each is recorded as a limitation in the design
document of the subsystem it belongs to as well as here. Reporting their absence
is not a finding; reporting that one of them is absent **where a document claims
it is present** very much is.

## 5. Supported versions

| Version | Supported |
| ------- | --------- |
| `main` | Yes — it is the only thing that exists |
| Anything else | There is nothing else |

This project does not branch and has cut no release. A fix lands on `main` and
that is the whole of the release process.

When there is a release this table has to be revisited, and the scheme it will
have to be written against is `docs/project/VERSIONING.md` — under which a
correction to a released image is a point release rather than a patch level, and
an edition is a variety of a release rather than a separate line to support.
That document records the gap as its own limitation 4 so that the obligation is
not carried by this table alone.

## 6. Reporting

**Use GitHub's private vulnerability reporting** for this repository — the
*Report a vulnerability* control on its Security tab. It reaches the project
owner without the report becoming public first.

Do not open a public issue for a defect that crosses one of the boundaries in
Section 2. Do open one for anything in Section 4, which is public already.

What is useful in a report: what the defect is, which of the three surfaces it
crosses, and how to reach it — a file, a sequence of calls, a boot log. A
reproduction is welcome and is not required; an argument from the source is
worth as much here, the system being small enough to read.

**This route has the same defect as the conduct route and it is recorded rather
than concealed.** There is one contributor, so a report reaches the person whose
code it concerns. Where that is unsuitable, GitHub's own reporting mechanisms are
independent of this project entirely. When there is more than one regular
contributor, a second recipient is to be named in this section.
[`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md), Section 4, carries the same
undertaking for the same reason.

## 7. What follows from a report

There is one contributor, no service to protect and no user to notify, so what
follows is short and is stated rather than dressed up as a process.

- The report is acknowledged.
- It is confirmed or it is not, and either way you are told which, with the
  reasoning.
- Where it is confirmed, a fix lands on `main` together with the documentation
  the change requires, per [`CONTRIBUTING.md`](CONTRIBUTING.md), Section 3, and
  a boot-time self-test asserts the defect is gone — or the absence of one is
  recorded, which is the same rule every other change here is held to.
- The defect and the assertion that now covers it are written into
  [`docs/project/HISTORY.md`](docs/project/HISTORY.md) and into the design
  document of the subsystem. **A fault found is recorded and not quietly
  corrected**; that is the project's standing practice and it does not acquire an
  exception for faults that are embarrassing.
- You are credited by whatever name you ask for, or not at all if you prefer.

**No timeline is promised, and no bounty is offered.** This is one person's
project, worked on irregularly, with no revenue and no users at risk. Promising a
ninety-day response would be a commitment made because it is conventional rather
than because it can be kept.

## 8. Disclosure

Coordinated disclosure is preferred and is not demanded. Nobody is running this
kernel, so there is no window during which an unfixed defect endangers anybody,
and a policy that treated a finding here as though there were would be borrowing
gravity it has not earned.

Publish when you like. Telling the project first is appreciated and is what makes
a fix and a public account arrive together.

## 9. Limitations of this document

1. **It describes a system that changes weekly.** Sections 3 and 4 are accurate
   as of the sub-task recorded in [`docs/project/PLAN.md`](docs/project/PLAN.md)
   and are revised with the work, as every document here is. Where this document
   and a design document differ, the design document is the later of the two and
   governs.
2. **No security review has been performed by anybody.** Nothing here has been
   audited, fuzzed, or examined by a person other than its author. The self-tests
   assert correctness against faults this project thought of; they are not an
   adversary.
3. **There is no threat model beyond Section 2.** Physical access, firmware, the
   boot chain, the toolchain and the build host are all trusted absolutely, and
   the kernel verifies nothing about the image it was booted from.
