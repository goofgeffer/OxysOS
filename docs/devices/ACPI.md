<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Firmware Description Tables

**Corresponding phase**: Phase 6, sub-task 6.12. This document is revised
whenever the kernel reads a further table, or reads more of one it already reads.

**Specifications**: ACPI Specification 6.5, Sections 5.2.5.1 (Finding the RSDP on
IA-PC Systems), 5.2.5.2 (Finding the RSDP on UEFI Enabled Systems), 5.2.5.3 and
Table 5.3 (RSDP Structure), 5.2.6 and Table 5.4 (DESCRIPTION_HEADER Fields),
5.2.7 (RSDT), 5.2.8 (XSDT), 5.2.12 and Tables 5.19 to 5.21 (Multiple APIC
Description Table), 5.2.12.2, 5.2.12.3, 5.2.12.4, 5.2.12.5, 5.2.12.7, 5.2.12.8
and 5.2.12.12 (the interrupt controller structures), and Table 5.26 (MPS INTI
Flags); Multiboot2 Specification 2.0, Sections 3.6.16 and 3.6.17.

**Implementation**: [`../../kernel/acpi/acpi.c`](../../kernel/acpi/acpi.c),
[`../../kernel/include/oxys/acpi.h`](../../kernel/include/oxys/acpi.h).

## 1. What this is for, and what it is not

ACPI is an enormous specification. This kernel reads one table of it and reads
that table for one purpose: to find out what interrupt controllers the machine
has, where they are, and how the sixteen ISA request lines are wired to them.
Everything the Advanced Configuration and Power Interface is otherwise famous
for — the bytecode interpreter, the device namespace, the power states, the
thermal zones — is absent, and none of it is planned before Phase 13.

The distinction is worth stating because "the kernel has ACPI support" would be a
false description of what is here. What is here is a table reader.

**Why it is a device document.** The tables are not a device, and nothing here
programs hardware. It sits beside [`PCI.md`](PCI.md) because it answers the same
question that document does — how a machine is asked what it contains — and
because [`APIC.md`](APIC.md), which is unambiguously a device document, is
unreadable without it.

## 2. The path from nothing to a controller address

```
The boot loader's tag, or a search of low memory
      |
      v
Root System Description Pointer  ("RSD PTR ")
      |  Signature, and a checksum over its first 20 bytes.
      |  Revision >= 2? then also a checksum over its declared length.
      v
RSDT (32-bit entries)  or  XSDT (64-bit entries)
      |  A description header, whose whole must sum to zero.
      |  Followed by n addresses of further tables.
      v
Each table in turn; the one signed "APIC" is the MADT
      |
      v
Multiple APIC Description Table
      |  A 32-bit local controller address, and the PCAT_COMPAT flag.
      |  A list of interrupt controller structures, each a type and a length.
      v
Processors, I/O APICs, interrupt source overrides, local NMI connections
```

## 3. Finding the pointer

Section 5.2.5.1 gives two regions to search, on 16-byte boundaries: the first
kibibyte of the Extended BIOS Data Area, whose paragraph address lies in the two
bytes at physical `0x40E`, and the read-only memory between `0x0E0000` and
`0x0FFFFF`.

**The boot loader's copy is preferred over both.** Multiboot2, Sections 3.6.16
and 3.6.17, defines two tags carrying a copy of the pointer — type 14 for the
twenty bytes ACPI 1.0 defined, type 15 for the thirty-six of ACPI 2.0 and later.
Where both are supplied the newer is taken, because only it names the XSDT.

The preference is not a convenience. Section 5.2.5.2 places the pointer, upon a
UEFI machine, in the EFI System Table, and expressly does *not* guarantee that
either of the two regions contains it. The search is what a legacy BIOS boot
uses today; the tag is what a UEFI boot will use at Phase 12, and the code that
consumes it is already written and already exercised.

**Nothing supplied is trusted.** The tag is validated by exactly the checks a
searched pointer is validated by, in the same function. A rule applied in two
places is a rule described in two places, and they do not stay in agreement.

## 4. The checksums, and why both are applied

Table 5.3 defines two. The first covers bytes 0 to 19 — all that ACPI 1.0 had —
and the second covers the whole of the declared length. Both are applied where
the revision claims the extended structure.

Applying only the second would accept a pointer whose ACPI 1.0 half was corrupt;
applying only the first would accept one whose 64-bit XSDT address was. The
second case is the dangerous one: the XSDT address would be believed, and a
directory walk would begin at whatever the corruption named.

Every description table is checked the same way, per Section 5.2.6: the whole
table, header included, must sum to zero. A table that fails is counted and
refused rather than parsed. This is the only integrity check ACPI provides, and
the consequence of skipping it is precise — a single corrupted byte in an I/O
APIC address would be programmed into the redirection hardware, and the machine
would receive its interrupts at an address that is not a controller.

## 5. Two things this parse does deliberately

### 5.1 Every field is assembled from its bytes

The interrupt controller structures of the MADT follow one another with no
padding whatever, and their lengths are 6, 8, 10, 12 and 16 bytes. An entry
therefore begins wherever the entries before it happened to end, and a 32-bit
field within it is frequently not upon a four-byte boundary. The XSDT is the same
case by construction: its 64-bit entries begin at offset 36.

Reading such a field through a pointer to a structure is undefined behaviour,
which `PROJECT_GUIDELINES.md`, Section 8, forbids. Upon x86_64 it would in fact
work, which is exactly what makes the practice dangerous to adopt: the defect
would be latent, and would surface upon the first architecture that trapped it.

Every multi-byte field is therefore assembled from its bytes in the little-endian
order the specification records. The same reasoning governs
`kernel/handoff/multiboot2.h`, which declares the framebuffer tag's fields
as offsets for the same reason.

### 5.2 Nothing retains a pointer into the tables

The memory map classifies the region holding these tables as ACPI reclaimable,
meaning the kernel may take it back once the tables have been consumed. Every
table is therefore mapped through `KernelDeviceMap` for the duration of its
parse, copied from into fixed arrays, and unmapped before `AcpiInitialise`
returns.

A retained pointer would become a defect at the moment the memory was reclaimed,
and the defect would present as an interrupt controller programmed from whatever
the allocator had since written there — which is to say, at a time and in a
manner having nothing to do with this file.

The mapping is made twice for each table: once for the 36-byte header, to learn
the length, and once for the whole. Mapping a fixed generous amount instead would
be simpler and wrong, since a table lying at the end of the region the firmware
reserved would have the mapping run past it.

## 6. What the MADT says, and the one part of it that is easy to miss

| Structure | Type | What this kernel does with it |
| --------- | ---- | ----------------------------- |
| Processor Local APIC | 0 | Recorded. Sub-task 6.14 starts the usable ones, and does. |
| I/O APIC | 1 | Mapped and programmed by [`APIC.md`](APIC.md). |
| Interrupt Source Override | 2 | **See below.** |
| NMI Source | 3 | Counted as unrecognised; no input of this kernel's is non-maskable. |
| Local APIC NMI | 4 | Programmed into the LINT pin it names. |
| Local APIC Address Override | 5 | Replaces the header's 32-bit address. |
| Processor Local x2APIC | 9 | Recorded as a processor, alongside type 0. |
| Everything else | — | Counted as unrecognised. |

**The interrupt source override is the part whose absence is invisible.** Section
5.2.12.4 establishes that global system interrupts 0 to 15 carry the 8259A
request lines 0 to 15 — *except* where an override says otherwise. Only the
exceptions are declared, so a kernel that never read them would find nothing
missing and would be wrong upon most machines.

QEMU is the ordinary case, and its tables say:

```
ISA request 0 is carried by global interrupt 2, flags 0x0.
```

The interval timer is not upon input 0. A kernel that assumed it was would
programme input 0, unmask it, and never receive a tick — and the symptom is a
timer that is correctly configured, correctly unmasked, and silent, which is
indistinguishable from a timer that was never programmed at all.

### 6.1 An override displaces a line as well as moving one

This is the consequence that is easy to state and easy to forget, and it cost
this sub-task its first passing run.

The resolution from request line to global system interrupt is **not injective**.
With the override above, line 0 resolves to global interrupt 2 — and so does line
2, by the identity mapping, no override having been declared for it. Programming
the lines in ascending order therefore has line 2 overwrite line 0, and the
machine loses its tick.

The rule that resolves it: **an explicit declaration wins over the implicit
identity mapping.** A line owns its global interrupt unless some other line was
expressly declared to be carried by it. Line 2 accordingly has no input at all,
which is the truth — it is the cascade of the 8259A and is not a device line
under any controller.

The rule is applied at three places, not one: when the entries are programmed,
when a line is masked, and when a line is unmasked. Masking line 2 without it
would mask the timer.

### 6.2 The MPS INTI flags

Table 5.26 gives the polarity in bits 1:0 and the trigger mode in bits 3:2. A
value of `00` in either means the source conforms to the convention of its bus,
which for the ISA bus is active high and edge triggered. `01` is active high or
edge; `11` is active low or level; `10` is reserved in both fields.

The flags matter because an input programmed with the wrong polarity is an input
that is asserted permanently or never.

## 7. Verification

`KernelVerifyAcpi` asserts the properties whose violation would be silent. A
machine supplying no tables at all is not a failure — this kernel runs upon one,
through the 8259A pair — so what is asserted is that a parse claiming to have
succeeded is coherent.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| A pointer address and a directory address were recorded | A parse that reported success without having found either. |
| The directory named at least one table | A directory whose length was read from the wrong offset, yielding an entry count of zero. |
| The MADT declares at least one processor, and at least one usable one | **A walk of the entry list that advanced by the wrong amount**, which would read every entry from the wrong offset and recognise none of them. The kernel is executing upon a processor, so a table declaring none cannot be right. |
| The MADT names a local controller address | The header's field read from the wrong offset. |
| Every override names the ISA bus | A structure read from the wrong offset. Section 5.2.12.5 admits no other bus, so a non-zero bus number is a misread and would be applied to a request line it has nothing to do with. |
| Every line without an override resolves to the global interrupt of its own number | The resolution itself, rather than the table. A lookup returning the wrong answer would route the timer to an input nothing is attached to. |

## 8. Observed state

Under QEMU with `-machine q35 -cpu qemu64 -smp cores=2`:

| Quantity | Value |
| -------- | ----- |
| Pointer source | The boot loader's copy, tag type 14 |
| Pointer revision | 0 |
| Directory | RSDT |
| Tables named | `FACP`, `APIC`, `HPET`, `MCFG`, `WAET` |
| Local controller address | `0xFEE00000` |
| Processors declared | 2, both usable |
| I/O APICs | 1, at `0xFEC00000`, global interrupts from 0 |
| Overrides | ISA 0 to global 2; ISA 5, 9, 10 and 11 with flags `0xD` (active low, level triggered) |
| Local NMI | LINT1 of every processor |
| Checksum failures | 0 |
| Unrecognised entries | 0 |

The revision of 0 is worth a note: GRUB supplies the ACPI 1.0 tag under legacy
BIOS boot even where the firmware's own pointer is revision 2, so the RSDT is
what is walked here. The XSDT path is written and is what a UEFI boot will take.

## 9. Limitations

1. **One table is read.** The FADT, the DSDT and everything the namespace holds
   are not. The kernel therefore knows nothing of power management, of the SCI,
   or of any device that is described only in the namespace.
2. **The AML interpreter does not exist and is not planned before Phase 13.**
   PCI interrupt routing for devices that are not upon an ISA request line
   requires the `_PRT` object, and is a prerequisite of the network driver of
   Phase 11.
3. **At most 64 processors, 8 I/O APICs, 32 overrides, 8 local NMI connections
   and 32 tables are recorded.** A machine declaring more has its description
   truncated, which is reported and never silent.
4. **The tables are read once and never again.** Hot-plug is not supported and
   the descriptions are not revisited.
5. **The reclaimable memory is not in fact reclaimed.** Nothing retains a pointer
   into it, which is what makes reclaiming it possible, but the frame allocator
   is not told it may have those regions back. That is a Phase 13 matter.
