# The PCI Bus

**Phase**: 4, sub-task 4.3, of [`PLAN.md`](PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion of
hardware behaviour below carries a citation, and every specification named is
registered in [`REFERENCES.md`](REFERENCES.md).

**Implementation**: [`../drivers/pci/pci.c`](../drivers/pci/pci.c),
[`../kernel/include/oxys/pci.h`](../kernel/include/oxys/pci.h).

## 1. What the enumeration is for

Every device the kernel has driven so far was found by knowing where it is. The
interval timer is at `0x0040`, the keyboard controller at `0x0060`, the serial
adapter at `0x03F8`, the interrupt controllers at `0x0020` and `0x00A0`. Those
addresses are not discovered; they are inherited from the IBM Personal Computer
and its successors, and a kernel may assume them because a machine that
contradicted them would not boot anything else either.

No device introduced after that arrangement may be assumed in the same way. A
disk controller, a network controller, a graphics adapter — each answers at an
address assigned to it when the machine was configured, and the only way to learn
that address is to ask. The PCI configuration space is the mechanism for asking,
and it is the reason this sub-task precedes the disk driver rather than following
it.

The enumeration claims nothing and configures nothing. It establishes what the
machine contains and where each part of it answers; the drivers of Phase 4 and
beyond search what it recorded.

## 2. Configuration space access mechanism one

Two I/O locations are used: CONFIG_ADDRESS at `0x0CF8` and CONFIG_DATA at
`0x0CFC`, both thirty-two bits wide. An address is written to the first and the
register then appears at the second. The address is composed as follows:

| Bits | Field |
| ---- | ----- |
| 31 | Enable. Accesses to CONFIG_DATA are translated into configuration cycles only while it is set. |
| 30–24 | Reserved. |
| 23–16 | Bus number, 0 to 255. |
| 15–11 | Device number, 0 to 31. |
| 10–8 | Function number, 0 to 7. |
| 7–2 | Register number, selecting one of the 64 double words of the function's 256-byte configuration space. |
| 1–0 | Always zero; every configuration access is aligned to a double word. |

Two consequences shape the accessors in `pci.c`.

**Every access at the hardware is a double word.** `PciReadConfiguration16` and
`PciReadConfiguration8` read the containing double word and extract the field
from it, using the low bits of the offset as a shift; `PciWriteConfiguration16`
reads, replaces its half and writes back. A kernel that issued a sixteen-bit `IN`
against `0x0CFE` would be relying upon a behaviour of the host bridge rather than
upon the specification, and the specification is what is portable.

**The offset need not be aligned by the caller.** `PciComposeAddress` masks it
with `0xFC`, which is exactly what the field is, so a caller may pass the offset
of a byte-wide field such as the class code and get the double word holding it.

### 2.1 Detecting the mechanism

CONFIG_ADDRESS is a readable register, and that is how its presence is
established: an enabled address is written and read back, and a machine that does
not implement the mechanism returns something other than what was written. The
probe touches CONFIG_DATA not at all, so nothing is disturbed by asking.

## 3. What absence looks like

"When a configuration access attempts to select a device that does not exist, the
host bridge will complete the access without error, dropping all data on writes
and returning all ones on reads." A vendor identifier of `0xFFFF` therefore means
that nothing answered — there is no error to detect, and no timeout to wait for.

This is convenient and it is also the reason Section 6 exists. An enumerator
whose address arithmetic is wrong reads addresses that decode to nothing, finds
`0xFFFF` everywhere, and reports an empty machine. The report is identical to the
one a correct enumerator would produce upon a machine with no devices, so nothing
about the failure is visible in it.

## 4. The walk

A device is examined at function zero first. The remaining seven are examined
only if bit 7 of its header type register was set, which is what marks a device
as multifunction: the specification does not require a device to decode a
function number it does not implement, and a single-function device may answer
every function number with a copy of itself. An enumerator that probed all eight
regardless would report each such device eight times.

Buses are reached rather than swept:

1. The host bridge at `0:0.0` is examined. Where it is multifunction, each of its
   functions is a separate host bridge and is the root of the bus bearing its
   function number.
2. Each bus so identified is queued.
3. Scanning a bus records every function upon it. A function of class `0x06`,
   subclass `0x04` — a PCI-to-PCI bridge — has behind it the bus named by its
   secondary bus number at offset `0x19`, and that bus is queued in turn.
4. The queue is worked through until it is empty.

The alternative, probing all 256 buses, is not wrong but it is 8192 device probes
against buses that mostly do not exist, and it cannot distinguish a bus that is
absent from one that a bridge would have named.

Two properties are worth stating because they are what keep the walk finite:

- **The queue is explicit, not the call stack.** A bridge queues its secondary
  bus rather than descending into it, so the depth of the tree is not the depth
  of the recursion, and a deeply nested topology cannot exhaust the 64 KiB boot
  stack.
- **Each bus is visited once.** A bitmap of 256 bits records which have been
  scanned. The specification does not permit a topology in which a bridge names a
  bus already visited; hardware that presented one would send a recursive walk
  around a cycle forever, and one bit per bus is a cheaper insurance than a hang.

## 5. What is recorded

The unit recorded is a **function**, not a device. A multifunction device
presents as many as it implements, each separately identified, separately
classified and separately driven; the ICH9 chipset of the QEMU q35 board presents
its LPC bridge, its storage controller and its SMBus controller as functions 0, 2
and 3 of device 31.

Each entry holds the geographical address, the vendor and device identifiers, the
revision, the programming interface, the subclass and class code, the header
layout and whether the device is multifunction, the interrupt line and pin, and
the six base address registers.

The base address registers are read only for header type 0. A bridge's header
holds its bus numbers and its address windows where a standard header holds base
addresses four to six, and reading them as base addresses would describe regions
that do not exist.

`PciBarBase` removes the type and attribute bits, which is the whole of the
decoding this kernel needs: bit 0 clear denotes memory and set denotes I/O; for
memory, bits 2 and 1 give the width, the value 2 meaning that the register is the
lower half of a 64-bit address whose upper half is the register following it, and
bit 3 marks the region prefetchable. A base address that still carried those bits
would be a port number, or an address off by as much as fifteen — which is to say
it would address hardware that is nearly right.

The table holds 64 functions. That is far beyond what any machine this kernel
runs upon presents, and the count of any that did not fit is reported rather than
silently dropped.

## 6. Verification

The enumeration is unfalsifiable by inspection, for the reason given in Section
3: a wrong answer looks exactly like a machine with nothing in it. `KernelVerifyPci`
therefore asserts that particular things were found, and that the accessors agree
with one another.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| Mechanism one answers its own probe. | An enumeration conducted against a machine that does not implement it, which would report nothing and appear merely empty. |
| An address nothing decodes reads as all ones. | A host bridge that reports absence some other way, invalidating the test the whole walk rests upon. |
| The 16-bit and 8-bit accessors agree with the 32-bit one. | A shift taken from the wrong bits of the offset, which yields a plausible number rather than an obviously wrong one. |
| Something was found, and the first bus was scanned. | Address arithmetic with a field in the wrong position. |
| No function was discarded for want of room. | A machine larger than the table, whose later devices would be missing without explanation. |
| A host bridge stands at `0:0.0`, of class `0x06` and subclass `0x00`. | The walk reading somewhere other than where it believes it is reading. |
| An index beyond the table reports nothing; a search beginning beyond it finds nothing. | An off-by-one that returns an uninitialised entry as a device. |
| Every recorded function has a valid vendor identifier. | Absence recorded as a device. |
| Every base address has had its type bits removed. | A driver directed to an address off by up to fifteen, or to a port number read as memory. |

Observed upon the QEMU q35 board:

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

Each entry is checkable against what QEMU is known to emulate, which is the
external corroboration the self-test cannot supply: the ICH9 chipset of the q35
board, the QEMU standard VGA adapter, and an Intel gigabit network controller.

## 7. Limitations

1. **Nothing is configured.** Base addresses are read, never assigned. The
   firmware assigns them before the kernel runs, and a kernel that reassigned
   them would have to reassign all of them consistently.
2. **No capability list.** The pointer at offset `0x34` is not followed, so MSI,
   MSI-X and PCI Express capabilities are invisible. Nothing yet needs them; the
   interrupt of a PCI device is taken from its interrupt line, which the firmware
   has routed.
3. **No interrupt routing.** The interrupt line register is recorded and
   believed. Deriving the line from the pin and the bridge topology requires the
   ACPI routing tables, which belong to a later phase.
4. **Mechanism one only.** Mechanism two is not implemented; it was deprecated
   long before any machine this kernel targets, and the enhanced mechanism of PCI
   Express, which maps configuration space into memory, requires the ACPI MCFG
   table to locate it.
5. **A fixed table.** Sixty-four functions, with the excess counted rather than
   recorded.
6. **The enumeration is performed once.** Nothing here supports a device
   appearing or leaving afterwards.
