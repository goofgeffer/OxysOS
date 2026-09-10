# The Advanced Programmable Interrupt Controllers

**Corresponding phase**: Phase 6, sub-tasks 6.12 and 6.13. This document is
revised whenever either controller is programmed differently, and will be revised
again at sub-task 6.14, whose bring-up uses the command register Section 7
describes.

**Specifications**: Intel 64 and IA-32 Architectures Software Developer's Manual,
Volume 3A, Chapter 10 (Advanced Programmable Interrupt Controller), Sections
10.4.1 to 10.4.8, 10.5.1, 10.5.2, 10.6, 10.6.1, 10.6.2.1, 10.8.3, 10.8.5, 10.8.6
and 10.9, Table 10-1 and Figures 10-5, 10-6, 10-8, 10-12 and 10-23; Intel 82093AA I/O Advanced Programmable Interrupt
Controller datasheet (order number 290566-001), Sections 3.1, 3.2.1 to 3.2.4;
ACPI Specification 6.5, Sections 5.2.12.3, 5.2.12.4, 5.2.12.5 and 5.2.12.7.

**A note upon the chapter number.** The APIC is Chapter 10 in the edition of the
manual whose numbering the rest of this corpus cites — the one in which Memory
Cache Control is Chapter 11 and the page attribute table is Section 11.12.2.
Intel has since renumbered, and in a current manual the APIC is Chapter 11 and
Memory Cache Control is Chapter 12. [`../project/REFERENCES.md`](../project/REFERENCES.md)
records the discrepancy once, so that a reader with either edition can find what
is cited here.

**Implementation**: [`../../drivers/apic/lapic.c`](../../drivers/apic/lapic.c),
[`../../drivers/apic/ioapic.c`](../../drivers/apic/ioapic.c),
[`../../kernel/include/oxys/lapic.h`](../../kernel/include/oxys/lapic.h),
[`../../kernel/include/oxys/ioapic.h`](../../kernel/include/oxys/ioapic.h). The
layer that routes a request to a driver is not here: it is
[`../design/INTERRUPTS.md`](../design/INTERRUPTS.md), Section 10.

## 1. Two devices, and what each is for

They are always named together and they are not alike.

The **Local APIC** is part of the processor. There is one per logical processor,
each reached at the same physical address by the processor that owns it. It
receives interrupts and presents them to its own core, it signals their
completion, and — from sub-task 6.13 — it is how one processor interrupts
another. Nothing outside the processor can address a particular one of them
through this interface.

The **I/O APIC** is part of the chipset. There is one, or a few. It has a set of
interrupt input pins, and a table saying, for each pin, what vector it is to
present and to which processor. It is the 8259A's replacement, and it replaces it
in a way the 8259A could not have been extended into: the priority of an
interrupt is no longer the position of its pin, but the vector software chose for
it.

The 8259A had neither property. It could deliver only to the one processor its
INTR line was attached to, and its priority was wired.

## 2. Why the 8259A is retired rather than kept

It would be possible to keep the pair and use the APIC only for the
inter-processor interrupts of sub-task 6.13. It would also be wrong, for two
reasons that are not the obvious one.

The obvious reason — that the APIC is faster, or newer — is not among them.

**A device delivering to one processor cannot be scheduled.** Sub-task 6.15 puts
threads upon several processors. An interrupt that can only ever reach the
bootstrap processor makes that processor the one where every device's work is
done, whatever the scheduler decides. The redirection table is what makes the
destination a decision rather than a wire.

**Two controllers presenting one device is worse than either alone.** ACPI 6.5,
Table 5.20, states the requirement plainly for a machine declaring `PCAT_COMPAT`:
"The 8259 vectors must be disabled (that is, masked) when enabling the ACPI APIC
operation." If both were live, every request would arrive twice, and the second
arrival would be acknowledged at a controller that had not sent it — which, at
the 8259A, resets the in-service bit of an unrelated line and loses a real
interrupt belonging to something else.

## 3. The Local APIC

### 3.1 The two enables, which are separate mechanisms

A controller with either clear accepts nothing whatever, while its local vector
table reads back exactly as intended. This is the characteristic way to have an
APIC that is perfectly programmed and entirely silent.

| Enable | Where | Reset state | Reference |
| ------ | ----- | ----------- | --------- |
| Global | `IA32_APIC_BASE` (MSR `0x1B`) bit 11 | Set | SDM 10.4.3, Figure 10-5 |
| Software | Spurious-interrupt vector register bit 8 | **Clear** | SDM 10.4.3, 10.4.7.1, Figure 10-23 |

Section 10.4.7.1 records that the spurious-interrupt vector register is
initialised to `0x000000FF` after a reset, and that "by setting bit 8 to 0,
software disables the local APIC". The controller therefore arrives disabled and
`LocalApicInitialise` is what enables it.

The global enable is written **before** the register page is mapped and touched.
Section 10.4.3 provides that with bit 11 clear "the processor is functionally
equivalent to an IA-32 processor without an on-chip APIC", and upon the Pentium
an access to the register space in that state may raise an invalid-opcode
exception. The write preserves the base address the register already holds: the
register is written whole, and dropping the address would relocate the
controllers.

### 3.2 Where the registers are

Section 10.4.1 places them in a 4 KiB region at `0xFEE00000` and requires that
region to be "mapped to an area of memory that has been designated as strong
uncacheable (UC)".

The address is nevertheless read from `IA32_APIC_BASE` rather than assumed,
Section 10.4.5 permitting the firmware to have relocated it; and where the MADT
declares a Local APIC Address Override, that is preferred over both, being the
firmware's own statement about every processor rather than about the one
executing.

The mapping is made with the page-level cache-disable flag and the write-through
flag clear, which selects entry 2 of `IA32_PAT`. The processor leaves that entry
holding UC-, and `graphics/framebuffer.c` did not disturb it, having written only
entry 4. Table 11-7 gives the effective type of UC- over a region the memory-type
range registers call uncacheable as uncacheable, which the register page of a
memory-mapped controller invariably is.

A cacheable mapping is the sort of defect that reads correctly and behaves
wrongly: a read of the in-service register would be answered from a cache line
rather than from the controller, and an end-of-interrupt would sit in a write
buffer while the next interrupt waited for it.

### 3.3 What is programmed, and why each

| Register | Value | Reason |
| -------- | ----- | ------ |
| Task priority | 0 | Section 10.8.6: the register blocks every interrupt of a priority class at or below what it holds. A reset clears it, but the firmware ran first and is under no obligation to have left it alone. A task priority the firmware raised presents as a machine where every controller is correct and nothing ever interrupts. |
| LVT timer | Masked | The local timer is not used before sub-task 6.15. A firmware that left it running would deliver an unregistered vector the instant the software enable was set. |
| LVT performance counters | Masked | The same. |
| LVT thermal | Masked, **if the version register reports enough entries** | Section 10.4.8: the entry exists only upon processors having six or more. Writing a register the implementation lacks is recorded as an illegal register access in the error status register. |
| LVT error | Vector `0xFE` | Programmed **before** the software enable, so that an error arising from the enabling itself reaches a handler rather than whatever the entry held. |
| LINT0, LINT1 | Masked, then the one ACPI names given the NMI delivery mode | Section 3.4 below. |
| Spurious vector | `0xFF`, with bit 8 set | Section 3.5 below. |

### 3.4 The local interrupt pins

Both are masked first and only then is the pin the firmware declares given the
non-maskable delivery mode. The order is what matters.

A firmware that booted the machine may have left LINT0 in the **external**
delivery mode, which is how the 8259A reaches a processor through its local
controller — the processor runs a special acknowledge cycle and the external
controller supplies the vector. Leaving that in place while the 8259A is retired
would have the processor waiting for a vector from a controller that no longer
presents one.

The ACPI Local APIC NMI structures, Section 5.2.12.7, say which pin of which
processor the non-maskable interrupt is attached to; an entry naming processor
`0xFF` applies to every processor. The vector field is ignored for the NMI
delivery mode, per Section 10.5.1, and is left zero. Entries naming a particular
processor other than this one are left for sub-task 6.14 to apply as each
application processor starts.

### 3.5 The spurious vector, and why it is `0xFF`

Section 10.9 describes the condition: a processor raises its task priority above
an interrupt that is already being dispensed, or the interrupt becomes masked
between acceptance and delivery. The controller then delivers the
spurious-interrupt vector. Nothing stands in the in-service register, so **the
handler must return without an end-of-interrupt**; one issued there would reset
the bit of whatever interrupt was genuinely in service and lose it.

The vector is `0xFF` because its low four bits are all ones. Section 10.9 records
that upon the P6 family and the Pentium those four bits are hardwired to one and
that writes to them have no effect. A vector chosen without that property would
be programmed and then silently altered by the hardware, and the vector delivered
would be one nothing had registered. The self-test reads the register back for
exactly this reason.

Section 10.9 also carries a warning this kernel obeys: **no local vector table
entry and no redirection entry may be given the spurious vector**, even masked. A
spurious handler performs no end-of-interrupt, so a real interrupt arriving at
that vector would leave its bit standing in the in-service register for ever,
masking every interrupt of equal or lower priority.

The error vector is `0xFE`, one below, and is separate on purpose: an error the
controller detects in itself is a different event from a request it withdrew, and
a single counter covering both would hide either. Its handler writes the error
status register before reading it, as Section 10.5.3 requires — the write is what
causes the latched value to be updated, and a read without it returns the state
before the error being reported.

## 4. The I/O APIC

### 4.1 The registers are reached indirectly

The 82093AA datasheet, Section 3.1, gives two memory-mapped registers: `IOREGSEL`
at offset `0x00`, which selects a register by its 8-bit address, and `IOWIN` at
offset `0x10`, through which the selected register is read and written.

This is a stateful interface — a write to the first decides what the second refers
to — and it has two consequences.

**The page must be uncacheable.** A cached read of `IOWIN` could be answered
without the controller being consulted, returning the value of whichever register
was selected when the line was filled.

**The pair must never be left half used.** A routine that selected a register and
returned without accessing it would change the meaning of the next access
somewhere else entirely. Every access here writes the selector and uses the
window together.

The sequence requires the spinlock governing the unit: two flows of control
performing it would interleave into an access of the wrong register. That lock
was built by sub-task 6.13 and this sequence has not been brought under it,
there being one flow of control; sub-task 6.14 is what makes it contended. It
is the same obligation the 8259A's mask registers carry, for the same reason,
and discharged at the same moment.

### 4.2 The registers themselves

| Index | Register | What is read from it |
| ----- | -------- | -------------------- |
| `0x00` | `IOAPICID` | Bits 27:24, the unit's identifier. |
| `0x01` | `IOAPICVER` | Bits 7:0 the version; bits 23:16 the **maximum redirection entry**, which Section 3.2.2 defines as "the number of interrupt input pins for the IOAPIC minus one". |
| `0x02` | `IOAPICARB` | Not read. |
| `0x10` upward | `IOREDTBL[n]` | Two 32-bit registers to each 64-bit entry. |

**The input count is read from the hardware and not from ACPI.** The MADT records
only where a unit's inputs begin; Section 5.2.12.3 refers the reader to this
register for how many there are. A unit answering with more than the 240 the
datasheet admits is not answering at all, and the commonest cause is a mapping
that never reached a controller, in which case the read returned whatever the bus
drives upon an unclaimed address. Such a unit is refused, not used.

### 4.3 The redirection entry

Per Section 3.2.4:

| Bits | Field | Value used here |
| ---- | ----- | --------------- |
| 63:56 | Destination | The identifier of the Local APIC of the bootstrap processor. |
| 16 | Mask | Set until a driver unmasks the line. |
| 15 | Trigger mode | 0 edge, 1 level. From the ACPI MPS INTI flags. |
| 14 | Remote IRR | Read only. |
| 13 | Input polarity | 0 active high, 1 active low. From the same flags. |
| 12 | Delivery status | Read only. |
| 11 | Destination mode | 0, physical. |
| 10:8 | Delivery mode | `000`, fixed. |
| 7:0 | Vector | 32 plus the request line number. |

**The entry is written high half first.** Two 32-bit accesses make one 64-bit
entry, and the mask lives in the low half, so writing the low half last means the
entry becomes deliverable only once its destination and its trigger mode are
already in place. The reverse order has a window — short, and therefore
reproducible nowhere — in which the input is unmasked and directed at whatever
the entry held before.

**Physical destination mode, deliberately.** The logical modes exist to
distribute an interrupt across a set of processors. There is one processor until
sub-task 6.14, so choosing a distribution before there is anything to distribute
across would be a decision made without its reason. Sub-task 6.15 is where it
acquires one.

**A vector below 16 is refused.** Section 10.5.2 records that the Local APIC
treats such a vector as illegal and notes it in the error status register.
Programming one would produce a device that never interrupts and an error count
that rises, which is a considerably harder thing to read than a refusal.

### 4.4 Every input begins masked

For the same reason every 8259A line does, and one more. An input whose device
has no driver delivers a vector nothing registered; and where that input is
**level triggered**, it delivers it without end, because the remote in-service
flag is cleared only by an end-of-interrupt that never comes.

## 5. How a request line reaches a driver

The routing layer is described in [`../design/INTERRUPTS.md`](../design/INTERRUPTS.md),
Section 10. In outline:

```
Device asserts an input pin
      |
      v
I/O APIC redirection entry for that global system interrupt
      |  vector = 32 + line, destination = a local APIC identifier
      v
Local APIC of that processor  ->  the core  ->  vector 32 + line
      |
      v
IDT gate -> stub -> InterruptDispatch -> IrqRoute
      |  line = vector - 32
      v
The handler the driver registered for that line
      |
      v
LocalApicSignalEndOfInterrupt
```

The line number is the durable fact through all of this. A driver claims IR1 and
keeps IR1, and whether the request arrives from a 8259A or from an I/O APIC input
carrying global system interrupt 1 is not its business.

## 6. What the Local APIC's end-of-interrupt does that the 8259A's did not

Section 10.8.5: the write clears the highest priority bit in the in-service
register and dispatches the next. For a **level-triggered** interrupt the local
controller additionally sends an end-of-interrupt message to the I/O APICs, which
is what clears the remote in-service flag of the redirection entry that raised it.

There is consequently no cascade to think about and no second controller to
signal. The one write completes the request wherever it came from.

## 7. What is not here

The APIC timer is masked. That is not an omission: it belongs to sub-task 6.15,
and programming it now would be programming a mechanism with nothing to use it.

**The interrupt command register was in this section until sub-task 6.13.** It is
now written, by `LocalApicSendCommand`, and the layer above it is
[`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 5. This driver
owns the register and the waits upon its delivery status; it owns none of the
meanings a vector carries, exactly as it owns none of the meanings a device
request line carries.

The register's fields, from Intel SDM, Volume 3A, Section 10.6.1 and Figure
10-12: the vector in bits 7:0, the delivery mode in 10:8, the destination mode in
11, the read-only delivery status in 12, the level in 14, the trigger mode in 15,
and the destination shorthand in 19:18. The destination occupies bits 31:24 of
the high half in xAPIC mode, per Section 10.6.2.1.

**The high half is written first**, because the manual states that "the act of
writing to the low doubleword of the ICR causes the IPI to be sent" — a
destination written afterwards is the destination of the next interrupt and not
of this one.

## 8. Verification

`KernelVerifyLocalApic` and `KernelVerifyIoApic` assert what was programmed;
`KernelVerifyApicRouting` asserts that a device pin still reaches its driver
afterwards, which is the only thing the change was for.

Every value is read back from the hardware. An interrupt controller programmed
wrongly reports nothing: it produces a device that is silent, and a silent device
is indistinguishable from an absent one, from a driver that was never
initialised, and from a machine with nothing attached.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| The global enable of `IA32_APIC_BASE` is set | A controller that is perfectly programmed and accepts nothing. |
| The software enable of the spurious vector register is set | The same, by the other mechanism — and the one that is clear after a reset, so its absence is the default state and not an unusual one. |
| The spurious vector reads back as the one programmed | The hardwired low four bits of Section 10.9. A vector altered by the hardware would be delivered to whatever handler stood at the altered number. |
| The task priority register is zero | A firmware that raised it. Every controller correct, nothing ever delivered. |
| Neither LINT pin is in the external delivery mode | A processor waiting for a vector from a 8259A that has been retired. |
| The LVT timer is masked | An unregistered vector delivered at whatever rate the firmware left the local timer at. |
| The version register reads as an APIC's (`0x00`–`0x15`) | **The register page is not mapped to a controller at all** — the wrong address, or the wrong memory type. |
| The processor is the bootstrap processor | The initialisation running somewhere it was not meant to. |
| The error count is zero | Any of the illegal accesses above, which the controller records and nothing else reports. |
| Every I/O APIC unit reports between 1 and 240 inputs | A register read that did not reach a controller. |
| Every input above the request lines is masked | An input no driver claimed delivering a vector nothing registered — without end, if it is level triggered. |
| The 8259A pair is fully masked, and reports itself retired | Two controllers presenting one device upon one vector. |
| Every claimed line's entry carries the vector the line has always had | The keyboard's requests delivered to the timer's handler. A driver holds no vector and could not detect this. |
| Every claimed line's entry names this processor | A device programmed, unmasked and silent — sub-task 6.14's characteristic failure, two sub-tasks early. |
| Every claimed line's mask agrees with what its driver asked for | A device that worked before the adoption and does not after. |
| **The interval timer still ticks** | The end-to-end assertion. It is the timer because it is the only device that interrupts without anybody touching the machine, and because its line is the one most likely to have moved: an override for request line 0 is among the commonest a firmware declares. |

## 9. Observed state

Under QEMU with `-machine q35 -cpu qemu64 -smp cores=2`:

| Quantity | Value |
| -------- | ----- |
| Local APIC identifier | 0, the bootstrap processor |
| Local APIC version | `0x14`, six local vector table entries |
| Local APIC registers | `0xFEE00000` |
| LINT0 | `0x10000` — masked |
| LINT1 | `0x400` — the non-maskable delivery mode, unmasked |
| Spurious and error vectors | 255 and 254; neither has yet been delivered |
| Interprocessor vectors | 253 the shootdown, 252 the halt |
| Commands sent through the command register | 3, none refused, none abandoned |
| I/O APIC units | 1, at `0xFEC00000`, version `0x20`, 24 inputs |
| Global interrupt 1 | vector 33, processor 0, active high, edge — the keyboard |
| Global interrupt 2 | vector 32, processor 0, active high, edge — **the timer**, by override |
| Global interrupt 4 | vector 36, processor 0, active high, edge — the serial adapter |
| Global interrupt 12 | vector 44, processor 0, active high, edge — the mouse |
| Request line 2 | Carried by no input; global interrupt 2 belongs to line 0 |

## 10. Limitations

1. **Sixteen request lines are addressable**, being the range ACPI 6.5, Section
   5.2.12.4, guarantees carries the same devices under either controller. The 24
   inputs of the unit above the first sixteen are mapped and masked but cannot be
   claimed. A PCI device that is not upon an ISA request line needs the ACPI
   namespace's `_PRT` object to be routed at all, which requires the interpreter
   [`ACPI.md`](ACPI.md), limitation 2, records as absent.
2. **One processor is a destination.** Every entry names the bootstrap processor.
   Sub-tasks 6.14 and 6.15 give the destination something to choose between.
3. **~~The inter-processor interrupt is not implemented.~~** Discharged at
   sub-task 6.13. The command register is written, two vectors are reserved —
   `0xFD` for the shootdown and `0xFC` for the halt — and the three audiences a
   sender may address are the two shorthands and a named identifier. See
   [`../design/CONCURRENCY.md`](../design/CONCURRENCY.md), Section 5.
4. **The APIC timer is masked and uncalibrated.** The 8253 remains the only time
   source. Sub-task 6.15.
5. **Nothing here is safe against concurrent access.** The select-then-window
   sequence of the I/O APIC and the mask state of the request layer both require
   a lock. The lock now **exists** — sub-task 6.13 built it — but neither has
   been put under one, there being one processor; sub-task 6.14 is what makes
   them contended. The local controller's own registers need none: each processor
   reaches its own controller at the same physical address, and no lock could
   make an access there refer to another's.
6. **x2APIC mode is not entered**, even where the processor reports it. The xAPIC
   register interface addresses 255 processors, which is more than this kernel
   will schedule for some time, and the extended mode's benefit is entirely in
   the count of processors addressable.
