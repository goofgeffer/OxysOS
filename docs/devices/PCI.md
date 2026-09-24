<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The PCI Bus

**Phase**: sub-task 4.3 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../drivers/pci/pci.c`](../../drivers/pci/pci.c),
[`../../kernel/include/oxys/dev/pci.h`](../../kernel/include/oxys/dev/pci.h).
**Specifications**: PCI Local Bus Specification 3.0 (configuration mechanism
one, the type 0 and type 1 headers, base address registers); PCI Code and ID
Assignment Specification (class codes). Both are registered in
[`../project/REFERENCES.md`](../project/REFERENCES.md), which records how the
first, not publicly distributed, was cross-checked.

The enumeration of the PCI configuration space: what functions the machine
contains and where each answers. The legacy devices (timer, keyboard, serial,
interrupt controllers) sit at addresses inherited from the IBM PC; every later
device (disk, network or display controller) answers at an address assigned at
configuration time, which only this space reveals. The enumeration claims and
configures nothing; drivers search what it records.

## 1. Configuration mechanism one

Two 32-bit I/O ports: CONFIG_ADDRESS at `0x0CF8` and CONFIG_DATA at `0x0CFC`.
An address written to the first selects the register that appears at the second.

| Bits | Field |
| ---- | ----- |
| 31 | Enable; CONFIG_DATA accesses become configuration cycles only while set. |
| 30–24 | Reserved. |
| 23–16 | Bus, 0 to 255. |
| 15–11 | Device, 0 to 31. |
| 10–8 | Function, 0 to 7. |
| 7–2 | Register: one of the 64 double words of the 256-byte space. |
| 1–0 | Zero; accesses are double-word aligned. |

- **Every hardware access is a double word.** The 16- and 8-bit readers read the
  containing double word and shift out the field; the 16-bit writer reads,
  replaces its half and writes back. A 16-bit `IN` from `0x0CFE` would rely on
  the host bridge rather than the specification.
- **Callers need not align offsets.** `PciComposeAddress` masks the offset with
  `0xFC`, so a byte field's offset yields the double word that holds it.
- **Detection.** An enabled address is written to CONFIG_ADDRESS and read back;
  a machine without the mechanism returns something else. CONFIG_DATA is not
  touched, so the probe disturbs nothing.

## 2. Absence

A configuration access to a function that does not exist completes without
error and reads all ones. A vendor identifier of `0xFFFF` therefore means
nothing answered. There is no error or timeout to detect, which is why the
verification below asserts that particular things are found: an enumerator with
its address arithmetic wrong reads `0xFFFF` everywhere and reports an empty
machine, indistinguishable from a correct report of one.

## 3. The walk

- **Function zero first.** Functions 1 to 7 are examined only if bit 7 of the
  header type is set (multifunction). A single-function device need not decode
  the function number and may answer all eight as itself.
- **Buses are reached, not swept.** The host bridge at `0:0.0` is examined; if
  it is multifunction, each function is a host bridge rooting the bus of its
  function number. Each bus found is queued. Scanning a bus records every
  function; a PCI-to-PCI bridge (class `0x06`, subclass `0x04`) queues the bus
  named by its secondary bus number (offset `0x19`). Sweeping all 256 buses is
  8,192 probes of buses that mostly do not exist.
- **The queue is explicit**, not the call stack, so a deep topology cannot
  exhaust the 64 KiB boot stack.
- **Each bus is visited once**, recorded in a 256-bit map. The specification
  forbids a bridge naming a bus already visited; one bit per bus is cheaper
  insurance than a walk that cycles forever on hardware that does.

## 4. What is recorded

The unit is a **function**, since each is identified, classified and driven
separately (the q35 board's ICH9 presents its LPC bridge, SATA controller and
SMBus controller as functions 0, 2 and 3 of device 31). Each entry holds:
bus, device and function; vendor and device identifiers; revision, programming
interface, subclass and class; header layout and the multifunction bit;
interrupt line and pin; and the six base address registers.

- **Base address registers are read for header type 0 only.** A bridge's header
  holds bus numbers and address windows where a type 0 header holds registers 4
  to 6.
- **`PciBarBase` strips the type bits.** Bit 0 set means I/O; clear means memory,
  where bits 2–1 give the width (value 2: the next register holds the upper 32
  bits) and bit 3 marks prefetchable. An address still carrying these bits is off
  by up to fifteen, or is a port read as memory: hardware that is nearly right.
- **The table holds 64 functions**, and any beyond are counted and reported.

## Verification

`KernelVerifyPci` in [`../../kernel/test/dev/devices.c`](../../kernel/test/dev/devices.c).

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| Mechanism one answers its own probe. | An enumeration against a machine without it, reporting an empty machine. |
| An address nothing decodes reads all ones. | A bridge signalling absence some other way, invalidating the walk. |
| The 16- and 8-bit readers agree with the 32-bit one. | A shift from the wrong offset bits, giving a plausible wrong number. |
| Something was found and bus 0 was scanned. | A field in the wrong position of the address. |
| No function was dropped for want of room. | Later devices missing without explanation. |
| A host bridge (class `0x06`, subclass `0x00`) stands at `0:0.0`. | The walk reading somewhere other than it believes. |
| An index beyond the table, or a search starting beyond it, finds nothing. | An uninitialised entry returned as a device. |
| Every recorded function has a valid vendor. | Absence recorded as a device. |
| Every base address has its type bits removed. | A driver sent to a nearly-right address. |

The log on QEMU q35, checkable against what QEMU emulates (ICH9, the standard
VGA adapter, an Intel gigabit controller):

```
Bus self-test passed.
PCI: 6 functions upon 1 buses, 0 beyond the table.
  0:0.0  0x8086:0x29C0  host bridge (class 0x6, subclass 0x0, interface 0x0)
  0:1.0  0x1234:0x1111  display controller (class 0x3, subclass 0x0, interface 0x0)
  0:2.0  0x8086:0x10D3  network controller (class 0x2, subclass 0x0, interface 0x0), IRQ 11
  0:31.0  0x8086:0x2918  ISA bridge (class 0x6, subclass 0x1, interface 0x0)
  0:31.2  0x8086:0x2922  serial ATA controller (class 0x1, subclass 0x6, interface 0x1), IRQ 10
  0:31.3  0x8086:0x2930  SMBus controller (class 0xC, subclass 0x5, interface 0x0), IRQ 10
```

## Limitations

1. Nothing is configured: base addresses are read as the firmware assigned them.
2. The capability list (offset `0x34`) is not followed, so MSI, MSI-X and PCI
   Express capabilities are invisible.
3. Interrupt routing is not derived; the interrupt line register the firmware
   filled is believed. Deriving it needs the ACPI routing tables.
4. Mechanism one only. The memory-mapped PCI Express mechanism needs the ACPI
   MCFG table.
5. At most 64 functions.
6. Enumeration happens once; no hot plug.
