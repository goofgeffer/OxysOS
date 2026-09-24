<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# Starting the Application Processors

**Phase**: sub-task 6.14 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../kernel/arch/x86_64/smp/smp.c`](../../kernel/arch/x86_64/smp/smp.c),
[`../../boot/trampoline.asm`](../../boot/trampoline.asm),
[`../../kernel/arch/x86_64/smp/smp_trampoline.asm`](../../kernel/arch/x86_64/smp/smp_trampoline.asm),
[`../../kernel/include/oxys/arch/smp/smp.h`](../../kernel/include/oxys/arch/smp/smp.h);
per-processor TSS in [`../../kernel/arch/x86_64/cpu/tss.c`](../../kernel/arch/x86_64/cpu/tss.c)
and [`gdt.c`](../../kernel/arch/x86_64/cpu/gdt.c).
**Specifications**: Intel SDM, Volume 3A, Sections 8.4.3, 8.4.4.1 (MP
initialisation and the inhibition of interrupts across it), 8.2.2 (memory
ordering), 9.1.4 and Table 9-1 (state after reset), 9.9.1, 4.1.2 and Table 4-14,
3.4.5 and Figure 3-8 (the `L` flag), 2.5 (`CR0.WP`), 10.6.1 and Figure 10-12,
4.10.4.4 and 4.10.5; Volume 2A, `LTR`; ACPI 6.5, Section 5.2.12.2 and Table 5.23;
Multiboot2, Section 3.6.8.

How every processor the firmware declares usable is started: the INIT-SIPI-SIPI
protocol, the real-mode trampoline that carries a processor to 64-bit mode on the
kernel's paging hierarchy, what the bootstrap processor prepares for it, and what
the processor does on arrival. It then enters its scheduler idle thread
([`SCHEDULER.md`](SCHEDULER.md)). The mechanisms it relies on (per-processor
areas, spinlocks, IPIs, shootdown) are [`CONCURRENCY.md`](CONCURRENCY.md).

## 1. The protocol

`SmpStartProcessor` follows SDM 8.4.4.1:

```
INIT IPI to one processor              (level assert, level trigger)
wait 10 ms
Start-Up IPI, vector = trampoline page number
wait 200 µs
if not acknowledged: Start-Up IPI again
wait for the acknowledgement, up to 100 ms
```

- **Every send is checked.** An INIT without its startup interrupt leaves the
  processor held in reset: declared by the firmware, removed by the kernel, and
  never mentioned. `LocalApicSendCommand` reports a send it could not complete.
- **The vector is the page number** (SDM 8.4.3: vector `VV` starts at `000VV000H`).
  `smp.h` makes them one fact with `_Static_assert((VECTOR << 12) == ADDRESS)`, and
  requires the address below 1 MiB.
- **The second startup interrupt is conditional** on no acknowledgement: sent to a
  processor already running kernel code, its effect is undefined.
- **Interrupts are masked throughout** (SDM 8.4.4.1 requires devices inhibited
  between INIT and the last startup), inside one counted critical section, so the
  flag is restored as found. The delays therefore use `PitBusyWaitMicroseconds`,
  which reads the 8254 directly ([`../devices/TIME.md`](../devices/TIME.md)).
- **Two separate waits**: for `acknowledged` in the parameter block (the processor
  reached 64-bit mode on the kernel's hierarchy), then for the online count (it
  finished the kernel's own initialisation). A processor passing the first and not
  the second failed in code this kernel wrote.
- **`SMP_ANSWER_WAIT_MICROSECONDS` is 100 ms**, the kernel's own figure: four orders
  of magnitude of margin, short enough that a machine declaring an absent processor
  still boots and reports the absence.
- **One processor at a time.** There is one parameter block at one address; the
  claimed per-processor index is checked against the prepared one, which holds
  only because starts are serial. INITs are addressed to one processor, and the
  entry whose APIC identifier is the bootstrap processor's is skipped: an INIT to a
  running processor resets the machine.

## 2. The trampoline

A processor answering a startup interrupt starts in real mode, `CR0 = 60000010H`,
interrupts off, `CS` from the vector, other segments undefined (SDM 9.1.4).
`boot/trampoline.asm` carries it to 64-bit mode.

- **A flat binary at `0x8000`.** Its addresses must be those of the low page, which
  NASM's `org` states, and `org` exists only in the flat format. The `Makefile`
  assembles it to `build/trampoline.bin`, and `smp_trampoline.asm` embeds it with
  `incbin`, so the link fails if it was not built; there is no filesystem when
  processors start. A fixed origin avoids computing every real-mode offset at run
  time. A `%error` fails the build if the image exceeds a page.
- **Why `0x8000`**: below 1 MiB, which `pmm.c` reserves entirely (no frame there is
  ever issued), and above the IVT, the BIOS data area and `0x7C00`.
- **The page is proved usable**, not assumed: `SmpTrampolinePageIsUsable` requires
  the memory map to call it available and requires it clear of the kernel image
  and boot information (the exclusions `pmm.c` applies). Otherwise bring-up is
  refused, rather than overwriting firmware data.
- **An identity mapping of the page exists for the bring-up only**: the instruction
  after enabling paging is fetched from the same low address. While it stands a
  stray low pointer works instead of faulting. **Its removal is a shootdown** to
  every started processor, made after all have come online and left the page.

| Step | Does | Why there |
| ---- | ---- | --------- |
| Far jump to `0000:SmpTrampolineReal` | `CS` = 0 | The processor arrives with `CS = 0800H`; after this every offset means what `org` says. |
| `LGDT`, `CR0.PE` | 32-bit protected mode | The table first (SDM 9.9.1). |
| Far jump to 32-bit code | Loads `CS` | Discards prefetched instructions. |
| `CR4.PAE`, `CR3`, `IA32_EFER.LME`, `CR0.PG` (with `CR0.WP`) | Long mode | The order of SDM 4.1.2. `WP` is per processor; without it the kernel could write its own read-only text on that processor only. |
| Far jump to 64-bit code | 64-bit mode | The `L` flag (Figure 3-8); `D` clear. |

Only the low doubleword of `page_table` goes into `CR3` (32-bit mode), so
`SmpInitialise` refuses a hierarchy above 4 GiB rather than truncating. No stack is
used before `RSP` is loaded in 64-bit mode. The trampoline's GDT is discarded as
soon as the C entry loads the real one.

**The parameter block** is laid out in assembly and read through a C structure; the
assembler writes the magic `"XSMP"` first, and the bootstrap processor reads it
back **through the structure** before starting anyone, so a field that moved on one
side is caught before a processor jumps to the wrong address. The structure is
`packed` (the layout is not the compiler's to choose), but every field is naturally
aligned so the attribute changes nothing; it is fixed at 48 bytes by assertion. It
is `volatile`, since `acknowledged` is written by another processor. No barrier is
needed: stores are not reordered with other stores (SDM 8.2.2).

| Offset | Field | Written by | Read by |
| ------ | ----- | ---------- | ------- |
| `+0x00` | `magic` | Assembler | Bootstrap processor, as proof |
| `+0x08` | `page_table` | Bootstrap processor | Trampoline → `CR3` |
| `+0x10` | `entry_point` | Bootstrap processor | Trampoline, jump target |
| `+0x18` | `stack_top` | Bootstrap processor | Trampoline → `RSP` |
| `+0x20` | `argument` | Bootstrap processor | Trampoline → `RDI` |
| `+0x28` | `acknowledged` | **Starting** processor | Bootstrap processor |

## 3. What the bootstrap processor prepares

> **A starting processor allocates nothing, maps nothing and claims nothing** but a
> thread-table slot.

Its stack (`SMP_PROCESSOR_STACK_PAGES`), its double-fault stack and its TSS
descriptor are prepared before the startup interrupt, in a table keyed by index,
since the parameter block is reused for the next processor. The allocators are
unsynchronised, and a corruption at this moment would look like a processor that
failed to start.

**One TSS per processor**: `RSP0` and the interrupt stack table are per processor,
and `LTR` marks a descriptor busy and faults on a busy one, so sharing is
impossible. The array is sized to `PER_CPU_MAXIMUM` (64) and never moves, because
processors hold its addresses. `GdtTaskStateSegmentSelector(index)` derives each
selector; `GDT_TSS_SELECTOR` is its value at zero.

## 4. On arrival

`SmpApplicationProcessorEntry` starts in 64-bit mode with a stack and nothing else.
Each step can be skipped with the processor running happily until the moment it
exists for:

| Step | Why here |
| ---- | -------- |
| `GdtLoadOnThisProcessor`, `IdtLoadOnThisProcessor` | The trampoline's table is about to be unmapped. The GDT is the kernel's one table, loaded, not rebuilt; `GS.base` is re-established after the reload. |
| `PerCpuInitialise` | Every lock goes through the area; nothing above takes one. |
| The index check | The claimed index must equal the prepared one: a panic, since two processors on one stack cannot continue. |
| `TssInitialiseProcessor` | A double fault with no task register is a triple fault. |
| `SyscallInitialise`, `SyscallSetKernelStack` | Per-processor MSRs, including `IA32_KERNEL_GS_BASE`. |
| `FramebufferEstablishWriteCombiningOnThisProcessor` | `IA32_PAT` is per processor ([`FRAMEBUFFER.md`](FRAMEBUFFER.md)). |
| `LocalApicInitialiseThisProcessor` | Without it no interrupt arrives, including the shootdown about to be sent. |
| Record, announce, `SchedulerEnterIdle` | Below. |

**The record**: each processor stores its task register, GDT and IDT bases and
limits, `CR0`, `CR3` and `CR4` into `SmpRecords[index]`, read with `STR`, `SGDT`,
`SIDT` and the control registers, not copied from what the kernel meant to load.

**The announcement** is composed into a buffer and written in one
`KernelWriteString` call, since the diagnostic lock covers a call: two processors
arriving together must not produce `Processor Processor 1 is online…2 is online…`.

## 5. The diagnostic lock

`KernelWriteString` takes `KernelDiagnosticLock` for a whole call, covering the four
structures it reaches: the VGA cursor, the console rows, the serial transmit buffer
and the compositor's back buffer and damage. Each of those files' headers names
this as where its lock is taken.

- **Taken only once a per-processor area exists** (`PerCpuIsEstablished()`): a
  spinlock masks interrupts through the area via `GS.base`, and the banner is
  printed before the area exists. Taking it unconditionally read address 16 and
  reset the machine with an empty log. Before the area exists there is one writer
  by construction.
- **A panic halts the other processors, then reinitialises the lock.** The panic
  may come from inside `KernelWriteString`, or the holder may be a processor just
  halted; either would spin until the lock's own panic replaced the real report.

## 6. What is declined

| Condition | Outcome |
| --------- | ------- |
| No MADT, or one usable processor | Nothing attempted; `SmpDeclinedReason` says which. |
| Local APIC not enabled | Nothing can be sent. |
| Kernel hierarchy above 4 GiB | The trampoline cannot name it. |
| The 8254 not running | The delays cannot be measured. |
| The low page not available | Section 2. |
| An APIC identifier above 8 bits | Refused, counted: xAPIC cannot address it, and truncating would name another processor. |
| More than `PER_CPU_MAXIMUM` processors | The excess refused, counted. |
| Started but never acknowledged | Counted as a failure; the machine carries on. |

Each is a sentence in the report: "started none" and "had none to start" look
identical from outside.

## Verification

`KernelVerifyApplicationProcessors` in [`../../kernel/test/arch/smp.c`](../../kernel/test/arch/smp.c).
A count of started processors proves nothing; an acknowledgement to an interrupt
does. The core is a shootdown broadcast, with each target's own
`shootdowns_serviced` read afterwards (the broadcast itself fails if any target
does not acknowledge).

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The trampoline's identity mapping is gone. | Stray low pointers silently working. |
| Every counted area exists, is online, holds its own index, and its `self` points to itself. | An area counted but not claimed, or aliasing another. |
| No two areas share an APIC identifier; only index 0 is the bootstrap processor. | Two processors on one stack and TSS, with every count right. |
| Online = started + 1. | Accounting disagreeing with the areas. |
| Each started processor's task register names its own TSS. | A processor with no TSS, running until its first double fault. |
| Its GDT and IDT bases equal the bootstrap processor's; its `CR3` is the kernel root. | A processor still on the trampoline's table; on its own hierarchy. |
| `CR0.PG` and `CR4.PAE` set; `CR0.WP` equal to the bootstrap processor's. | Long mode not entered; a processor that can write kernel text. |
| **Every started processor's `shootdowns_serviced` rose across a broadcast.** | A processor halted with interrupts masked, never at the gate, or with its APIC disabled. |

On a single-processor machine it asserts that nobody was started, that
`SmpDeclinedReason` gives the reason, and that one processor is online.
`KernelVerifyPerCpu` runs **before** `SmpInitialise` and asserts one processor
online for that reason. On QEMU (`-smp cores=2`) the log reports two processors
declared, one started, the trampoline at `0x8000` with vector `0x8`, and its
identity mapping removed.

## Limitations

1. User threads run only on the bootstrap processor until the structures of
   [`CONCURRENCY.md`](CONCURRENCY.md) are locked; application processors run
   kernel threads.
2. No x2APIC: APIC identifiers above 8 bits cannot be started.
3. Bring-up is serial: about 10 ms per processor.
4. At most 64 processors (`PER_CPU_MAXIMUM`).
5. The trampoline page is fixed; if the firmware reserved it, bring-up is refused.
6. Nothing takes a processor offline except the panic path's halt.
7. A processor that acknowledges but then takes no interrupts shows as a hang:
   `ShootdownBroadcast` spins `SHOOTDOWN_WAIT_LIMIT` (10⁸ iterations) before
   panicking, longer than `make verify`'s 25-second bound under QEMU. A bound in
   time rather than spins would fix it.
8. The index check cannot fire with one application processor; it is kept as an
   unproven guard.
