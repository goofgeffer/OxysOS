<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Firmware Description Tables

**Phase**: sub-task 6.12 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../kernel/acpi/acpi.c`](../../kernel/acpi/acpi.c),
[`../../kernel/include/oxys/acpi/acpi.h`](../../kernel/include/oxys/acpi/acpi.h).
**Specifications**: ACPI Specification 6.5, Sections 5.2.5.1–5.2.5.3 and
Table 5.3 (RSDP), 5.2.6 and Table 5.4 (description header), 5.2.7 (RSDT), 5.2.8
(XSDT), 5.2.12 and Tables 5.19–5.21 (MADT), 5.2.12.2–5.2.12.5, 5.2.12.7,
5.2.12.8, 5.2.12.12 (controller structures), Table 5.26 (MPS INTI flags);
Multiboot2 Specification 2.0, Sections 3.6.16 and 3.6.17.

A reader of one ACPI table, the MADT, for one purpose: what interrupt
controllers the machine has, where they are, and how the sixteen ISA request
lines reach them. There is no AML interpreter, namespace, power management or
thermal support; "the kernel has ACPI support" would be false. It is a device
document because [`APIC.md`](APIC.md) cannot be read without it and because,
like [`PCI.md`](PCI.md), it is how the machine is asked what it contains.

## 1. From nothing to a controller address

```
Boot loader tag, or a search of low memory
      v
RSDP ("RSD PTR ")    checksum over 20 bytes; if revision >= 2, over its length too
      v
RSDT (32-bit entries) or XSDT (64-bit entries)    whole table sums to zero
      v
Each table; the one signed "APIC" is the MADT
      v
MADT                 local controller address, PCAT_COMPAT, then a list of
                     typed, variable-length controller structures
```

## 2. Finding the pointer

- **The boot loader's copy is preferred.** Multiboot2 tag 14 carries the 20-byte
  ACPI 1.0 pointer and tag 15 the 36-byte ACPI 2.0 one; with both, the newer is
  taken, since only it names the XSDT. On a UEFI machine the pointer is in the
  EFI System Table and the legacy regions need not hold it (Section 5.2.5.2), so
  the tag is the path Phase 12 relies on.
- **Otherwise the search** of Section 5.2.5.1: the first KiB of the Extended
  BIOS Data Area (its paragraph address is the word at physical `0x40E`), then
  `0x0E0000`–`0x0FFFFF`, on 16-byte boundaries.
- **A supplied pointer is validated exactly as a found one**, by the same
  function. A rule applied in two places drifts.

## 3. Checksums

Both RSDP checksums are applied when the revision claims the extended structure.
The first alone would accept a corrupt 64-bit XSDT address, and the directory
walk would start wherever the corruption pointed. Every description table must
sum to zero (Section 5.2.6); a table that does not is counted and refused. It is
the only integrity check ACPI offers, and without it a single bad byte in an
I/O APIC address is programmed into the interrupt hardware.

## 4. Parsing rules

- **Every field is assembled from its bytes**, little-endian. MADT structures
  are 6 to 16 bytes long and unpadded, and XSDT entries start at offset 36, so
  multi-byte fields are routinely unaligned. Reading them through a structure
  pointer is undefined behaviour (`PROJECT_GUIDELINES.md`, Section 8); it works
  on x86_64, which is what makes it a latent defect.
- **Nothing keeps a pointer into the tables.** The memory holding them is ACPI
  reclaimable. Each table is mapped with `KernelDeviceMap`, copied into fixed
  arrays and unmapped before `AcpiInitialise` returns; a retained pointer would
  program a controller from whatever was later written there.
- **Each table is mapped twice**: the 36-byte header to learn the length, then
  the whole. A fixed generous mapping could run past the end of the firmware's
  region.

## 5. The MADT

| Structure | Type | Use |
| --------- | ---- | --- |
| Processor Local APIC | 0 | Recorded; the usable ones are started ([`../design/SMP.md`](../design/SMP.md)). |
| I/O APIC | 1 | Mapped and programmed ([`APIC.md`](APIC.md)). |
| Interrupt Source Override | 2 | Changes the routing of an ISA line (below). |
| NMI Source | 3 | Counted as unrecognised; no input here is non-maskable. |
| Local APIC NMI | 4 | Programmed into the LINT pin it names. |
| Local APIC Address Override | 5 | Replaces the header's 32-bit address. |
| Processor Local x2APIC | 9 | Recorded as a processor. |
| Any other | — | Counted as unrecognised. |

**Overrides are the part whose absence is invisible.** Global interrupts 0 to 15
carry ISA lines 0 to 15 except where an override says otherwise (Section
5.2.12.4), and only the exceptions are declared. QEMU declares
`ISA request 0 is carried by global interrupt 2`: a kernel ignoring it programs
input 0 and never receives a timer tick, from a timer that looks correctly
configured.

**An override displaces a line as well as moving one.** With the override above,
line 0 resolves to global interrupt 2, and so does line 2 by identity. The rule:
**an explicit declaration wins over the implicit identity mapping**. A line owns
its global interrupt unless another line was declared to be carried by it; line
2 therefore has no input, which is true, being the 8259A cascade. The rule is
applied when entries are programmed, when a line is masked, and when a line is
unmasked; without it, masking line 2 masks the timer.

**Flags** (Table 5.26): polarity in bits 1–0, trigger mode in bits 3–2. `00`
means the bus convention (ISA: active high, edge); `01` active high or edge;
`11` active low or level; `10` reserved. A wrong polarity is an input asserted
permanently or never.

## Verification

`KernelVerifyAcpi` in [`../../kernel/test/arch/apic.c`](../../kernel/test/arch/apic.c).
A machine without tables is not a failure (the kernel runs on the 8259A pair);
what is asserted is that a parse reporting success is coherent.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A pointer address and a directory address were recorded. | Success reported without either found. |
| The directory names at least one table. | The length read from the wrong offset, giving zero entries. |
| The MADT declares at least one usable processor. | An entry walk advancing by the wrong amount and recognising nothing. |
| The MADT names a local controller address. | The header field read from the wrong offset. |
| Every override names the ISA bus. | A misread structure; Section 5.2.12.5 admits no other bus. |
| Every line without an override resolves to its own number. | A resolution that routes the timer to an unattached input. |

Under QEMU (`-machine q35 -cpu qemu64 -smp cores=2`) the log reports:

| Quantity | Value |
| -------- | ----- |
| Pointer | The loader's copy, tag 14, revision 0; so the RSDT is walked |
| Tables | `FACP`, `APIC`, `HPET`, `MCFG`, `WAET` |
| Local controller | `0xFEE00000` |
| Processors | 2, both usable |
| I/O APICs | 1, at `0xFEC00000`, global interrupts from 0 |
| Overrides | ISA 0 to global 2; ISA 5, 9, 10, 11 with flags `0xD` (active low, level) |
| Local NMI | LINT1 of every processor |
| Checksum failures, unrecognised entries | 0, 0 |

GRUB supplies tag 14 under BIOS boot even when the firmware's pointer is
revision 2, so the XSDT path is written but not exercised; see
[`../project/STATUS.md`](../project/STATUS.md) for the paths no run has taken.

## Limitations

1. Only the MADT is read. Nothing is known of the FADT, the DSDT, the SCI, power
   management, or devices described only in the namespace.
2. No AML interpreter, and none before Phase 13. PCI interrupt routing through
   `_PRT`, which the network driver of Phase 11 needs, depends on it.
3. At most 64 processors, 8 I/O APICs, 32 overrides, 8 local NMI connections and
   32 tables are recorded; any truncation is reported.
4. The tables are read once; no hot plug.
5. The ACPI reclaimable memory is not returned to the frame allocator.
