<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Advanced Programmable Interrupt Controllers

**Phase**: sub-tasks 6.12 to 6.15 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../drivers/apic/lapic.c`](../../drivers/apic/lapic.c),
[`../../drivers/apic/ioapic.c`](../../drivers/apic/ioapic.c), and their headers in
[`../../kernel/include/oxys/dev/`](../../kernel/include/oxys/dev/).
**Specifications**: Intel SDM, Volume 3A, Chapter 10 (APIC; Sections 10.4.1–10.4.8,
10.5.1–10.5.4, 10.6.1, 10.6.2.1, 10.8.3–10.8.6, 10.9; Table 10-1; Figures 10-5,
10-6, 10-8, 10-12, 10-23); Intel 82093AA I/O APIC data sheet (290566-001),
Sections 3.1, 3.2.1–3.2.4; ACPI 6.5, Sections 5.2.12.3–5.2.12.5 and 5.2.12.7.
The SDM chapter numbers are those of the edition this corpus cites, in which the
APIC is Chapter 10 and memory cache control Chapter 11; current editions add
one to each ([`../project/REFERENCES.md`](../project/REFERENCES.md)).

The interrupt controllers that replace the 8259A pair. The **Local APIC** is part
of each processor: it presents interrupts to its core, completes them, sends
interrupts to other processors, and runs a timer. The **I/O APIC** is in the
chipset: its redirection table says, for each input pin, which vector to present
to which processor. What they are programmed from is [`ACPI.md`](ACPI.md); the
layer that routes a request line to a driver is
[`../design/INTERRUPTS.md`](../design/INTERRUPTS.md).

## 1. Why the 8259A is retired

- **A device wired to one processor cannot be scheduled.** The 8259A reaches only
  the processor its INTR line is attached to; the redirection table makes the
  destination a decision.
- **Two live controllers deliver every request twice.** The second delivery's
  end-of-interrupt, at a controller that did not send it, resets an unrelated
  in-service bit and loses a real interrupt. ACPI 6.5, Table 5.20, requires the
  8259A masked when the APIC is used on a `PCAT_COMPAT` machine.

## 2. The Local APIC

**Two enables, both needed.** With either clear the controller accepts nothing,
while reading back exactly as programmed.

| Enable | Where | After reset |
| ------ | ----- | ----------- |
| Global | `IA32_APIC_BASE` (MSR `0x1B`) bit 11 | Set |
| Software | Spurious-interrupt vector register bit 8 | **Clear** (SDM 10.4.7.1) |

The global enable is written before the register page is touched (with it clear,
an access may fault on older processors), preserving the base address in the same
register.

**Registers** are a 4 KiB page, normally `0xFEE00000`. The address is taken from a
MADT Local APIC Address Override if present, otherwise from `IA32_APIC_BASE`
(SDM 10.4.5 allows relocation). The page must be uncacheable (SDM 10.4.1): it is
mapped with PCD set and PWT clear, which selects PAT entry 2, still UC- (only entry
4 is changed, for the framebuffer), and UC- over an MTRR-uncacheable region is
uncacheable. A cached mapping would answer reads from a cache line and hold an
end-of-interrupt in a write buffer.

| Register | Value | Reason |
| -------- | ----- | ------ |
| Task priority | 0 | Firmware may have raised it; then everything is correct and nothing interrupts (SDM 10.8.6). |
| LVT timer | Masked until the scheduler starts it | A timer left running by firmware would deliver an unregistered vector at enable. |
| LVT performance counters | Masked | The same. |
| LVT thermal | Masked, if the version register reports six or more entries | Writing a register that does not exist is logged as an illegal access (SDM 10.4.8). |
| LVT error | Vector `0xFE`, **before** the software enable | An error caused by enabling reaches a handler. |
| LINT0, LINT1 | Masked, then the pin ACPI names set to NMI delivery | Below. |
| Spurious vector | `0xFF`, bit 8 set | Below. |

**The LINT pins are masked first.** Firmware may have left LINT0 in ExtINT mode,
through which the 8259A delivers; left so, the processor would await a vector from
a retired controller. The ACPI Local APIC NMI entries (Section 5.2.12.7; processor
`0xFF` means all) name the NMI pin; the vector field is ignored for NMI. Each
application processor runs the same routine on its own controller as it starts,
since a processor can reach only its own.

**The spurious vector is `0xFF`.** A spurious interrupt (SDM 10.9) is delivered
when an accepted interrupt is withdrawn; nothing is in service, so its handler
**must not** signal end-of-interrupt, or it would complete a real one. On P6 and
Pentium processors the vector's low four bits are hardwired to one, so any other
choice would be silently altered; the self-test reads it back. No LVT or
redirection entry may use the spurious vector, even masked: its handler never
completes, so a real interrupt there would block all lower priorities for ever.

**The error vector is `0xFE`**, separate so that controller errors and withdrawn
requests are counted apart. Its handler writes the error status register before
reading it (SDM 10.5.3), which latches the current value.

**Inter-processor interrupts** are sent through the interrupt command register by
`LocalApicSendCommand`: vector in bits 7:0, delivery mode 10:8, destination mode
11, delivery status (read-only) 12, level 14, trigger 15, shorthand 19:18, and in
xAPIC mode the destination in bits 31:24 of the high half (SDM 10.6.1,
10.6.2.1). **The high half is written first**: writing the low half sends the
interrupt. The meanings of the vectors sent are
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md) and
[`../design/SMP.md`](../design/SMP.md).

**The timer** has no architectural rate (SDM 10.5.4: bus or crystal clock over a
divider), so it is calibrated against the 8254 and then started periodic on vector
`0xFB` on each processor ([`../design/SCHEDULER.md`](../design/SCHEDULER.md)).
Elapsed time still comes from the 8254 ([`TIME.md`](TIME.md)).

**End-of-interrupt** (SDM 10.8.5) clears the highest in-service bit; for a
level-triggered interrupt the Local APIC also sends an EOI message to the I/O
APICs, clearing the entry's remote IRR. One write completes a request wherever it
came from; there is no cascade.

## 3. The I/O APIC

**Registers are reached indirectly** (82093AA, Section 3.1): `IOREGSEL` at offset
`0x00` selects a register, `IOWIN` at `0x10` reads or writes it. The page is
uncacheable, and every access writes the selector and uses the window together;
an access half done changes the meaning of the next.

| Index | Register | Use |
| ----- | -------- | --- |
| `0x00` | `IOAPICID` | Bits 27:24, the unit's identifier. |
| `0x01` | `IOAPICVER` | Bits 7:0 version; bits 23:16 **maximum redirection entry** (inputs − 1). |
| `0x10`+ | `IOREDTBL[n]` | Two 32-bit halves per 64-bit entry. |

**The input count comes from the hardware**, not ACPI, which gives only where a
unit's inputs start. A unit reporting more than 240 inputs is not a controller
(usually a mapping that reached nothing) and is refused.

**The redirection entry** (Section 3.2.4):

| Bits | Field | Value |
| ---- | ----- | ----- |
| 63:56 | Destination | The bootstrap processor's Local APIC identifier |
| 16 | Mask | Set until a driver unmasks the line |
| 15 | Trigger | 0 edge, 1 level, from the ACPI flags |
| 14 | Remote IRR | Read-only |
| 13 | Polarity | 0 high, 1 low, from the ACPI flags |
| 12 | Delivery status | Read-only |
| 11 | Destination mode | 0, physical |
| 10:8 | Delivery mode | `000`, fixed |
| 7:0 | Vector | 32 + request line |

- **High half first.** The mask is in the low half, so the entry becomes live only
  after its destination and trigger are in place.
- **Physical destination, the bootstrap processor.** Drivers' structures are not
  synchronised, so a request handled elsewhere would race them.
- **Vectors below 16 are refused**: the Local APIC treats them as illegal
  (SDM 10.5.2), producing a silent device and a rising error count.
- **Every input starts masked.** An unclaimed input delivers a vector nothing
  registered, and if level-triggered does so for ever, since its remote IRR is
  never cleared.

## 4. From pin to driver

```
Device asserts an input
  -> I/O APIC entry for that global interrupt: vector 32 + line, processor 0
  -> Local APIC -> core -> IDT gate -> stub -> InterruptDispatch -> IrqRoute
  -> the handler the driver registered for the line
  -> LocalApicSignalEndOfInterrupt
```

The request line is the durable fact: a driver claims IR1 and keeps it, whichever
controller carries it.

## Verification

`KernelVerifyLocalApic`, `KernelVerifyIoApic` and `KernelVerifyApicRouting` in
[`../../kernel/test/arch/apic.c`](../../kernel/test/arch/apic.c). Everything is read
back from the hardware: a misprogrammed controller produces only a silent device,
indistinguishable from an absent one.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| The global enable is set. | A perfect controller accepting nothing. |
| The software enable is set. | The same; this one is clear after reset. |
| The spurious vector reads back as programmed. | Hardwired low bits altering it. |
| Task priority is zero. | Firmware blocking every interrupt. |
| Neither LINT pin is ExtINT. | A processor waiting on the retired 8259A. |
| The LVT timer is masked at initialisation. | An unregistered vector at the firmware's rate. |
| The version reads as an APIC's (`0x00`–`0x15`). | A register page mapped to nothing, or cached. |
| This is the bootstrap processor; the error count is zero. | Initialisation in the wrong place; logged illegal accesses. |
| Each I/O APIC reports 1–240 inputs. | A read that reached no controller. |
| Inputs above the request lines are masked. | Unregistered vectors, perhaps for ever. |
| The 8259A pair is fully masked and reports itself retired. | Double delivery. |
| Each claimed line's entry has the line's vector, names this processor, and has the mask its driver asked for. | Keyboard requests at the timer's handler; a silent device; a device that stopped working. |
| **The interval timer still ticks.** | The end-to-end check, on the line an override most often moves. |

Under QEMU (`-machine q35 -cpu qemu64 -smp cores=2`) the log reports: Local APIC 0
at `0xFEE00000`, version `0x14`, LINT0 masked, LINT1 NMI; one I/O APIC at
`0xFEC00000`, version `0x20`, 24 inputs; global interrupt 1 on vector 33 (keyboard),
2 on vector 32 (the timer, by override), 4 on 36 (serial), 12 on 44 (mouse); line
2 carried by no input.

| Vector | Use |
| ------ | --- |
| `0xFF` | Spurious |
| `0xFE` | Local APIC error |
| `0xFD` | TLB shootdown |
| `0xFC` | Halt |
| `0xFB` | Local timer |

## Limitations

1. Only the sixteen ISA request lines can be claimed; inputs 16–23 are mapped and
   masked. PCI routing beyond them needs `_PRT` and an AML interpreter
   ([`ACPI.md`](ACPI.md)).
2. Every entry targets the bootstrap processor. Changing that is the same change
   as letting user threads run elsewhere, since drivers are unsynchronised.
3. No lock on the I/O APIC's select-then-window sequence or the request-layer
   masks. Only the bootstrap processor touches them today. Local APIC registers
   need none: each processor reaches only its own.
4. x2APIC mode is not used; xAPIC addresses 255 processors.
