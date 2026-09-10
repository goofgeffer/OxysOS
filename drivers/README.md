# `drivers/` — Device Drivers

**Phase**: 1, sub-task 1.7, for the early output drivers. Phase 4 in full.
**Detailed design**: [`../docs/design/ARCHITECTURE.md`](../docs/design/ARCHITECTURE.md),
Section 6, records the diagnostic policy these drivers serve.

## Purpose

This directory holds the device drivers, one subdirectory per device class. A
driver implements an interface declared in `kernel/include/oxys/`; it does not
export declarations of its own. The kernel core therefore depends upon the
interface and never upon a driver's location.

The framebuffer is deliberately **not** here. It is in [`../graphics/`](../graphics/),
because nothing programs it: the boot loader sets the mode and hands over an
address, and everything above that is arithmetic upon memory rather than a
conversation with hardware. The pointer drawn for the mouse is there for the same
reason: the mouse is a device and is here, while its picture is arithmetic upon
memory and is not.

## Contents

| Path | Device | Interface | Phase |
| ---- | ------ | --------- | ----- |
| `vga/vga.c` | The VGA text-mode display, mode 3. Displaced by the framebuffer of sub-task 6.2 wherever the boot loader leaves the adapter in a graphics mode; see `docs/devices/DISPLAY.md`, Section 1.1. | `<oxys/vga.h>` | 1, 4.2 |
| `serial/serial.c` | The 16550-compatible UART at COM1, interrupt-driven. | `<oxys/serial.h>` | 1, 4.1 |
| `pic/pic.c` | The pair of cascaded 8259A interrupt controllers. Retired at sub-task 6.12; it holds no handler table and routes nothing. | `<oxys/pic.h>` | 3, 6.12 |
| `apic/lapic.c` | The Local APIC: one per logical processor; what completes every interrupt from sub-task 6.12 onward, and — from sub-task 6.13 — the command register through which one processor interrupts another. | `<oxys/lapic.h>` | 6.12, 6.13 |
| `apic/ioapic.c` | The I/O APIC: the redirection table that decides what vector an interrupt input presents, and to which processor. | `<oxys/ioapic.h>` | 6.12 |
| `pit/pit.c` | Counter 0 of the 8253 interval timer, the system tick. | `<oxys/pit.h>` | 3 |
| `ps2/ps2.c` | The 8042 keyboard controller itself, and the two device ports it presents. | `<oxys/ps2.h>` | 3, 6.5 |
| `keyboard/keyboard.c` | The PS/2 keyboard upon the controller's first port. | `<oxys/keyboard.h>` | 3 |
| `mouse/mouse.c` | The PS/2 mouse upon the controller's second port. | `<oxys/mouse.h>` | 6.5 |
| `pci/pci.c` | The PCI bus: configuration-space enumeration by mechanism one. | `<oxys/pci.h>` | 4.3 |
| `ata/` | The ATA disk, in programmed input/output mode, divided into six translation units and a private header; `../docs/design/ARCHITECTURE.md`, Section 2.2, records why. | `<oxys/ata.h>` | 4.4 |
| `ata/internal.h` | What those units share: the register, status, control and command constants, the table of devices found, the addresses each channel answers at, the accounting, and the register-level discipline. | — | 4.4 |
| `ata/ata.c` | The state, the refusals, the initialisation, the device accessors and the binding to the block layer. | `<oxys/ata.h>` | 4.4 |
| `ata/port.c` | The task file: the settling delay a selection must be followed by, the two waits every command is bracketed by, and the reset of a channel. | — | 4.4 |
| `ata/identify.c` | `IDENTIFY DEVICE`, and the distinction between a device that is absent and one that answers a different command set. | — | 4.4 |
| `ata/channel.c` | Where a channel actually answers: the base address registers of a controller in native mode, and the storage this driver cannot reach. | — | 4.4, 4.7, 4.8 |
| `ata/transfer.c` | The transfer of sectors in both addressing forms, and the cache flush that makes a write durable. | — | 4.4 |
| `ata/report.c` | The report, including every controller the bus carries and why each was or was not reached. | — | 4.4 |
| `ahci/ahci.c` | The AHCI disk, by first-party direct memory access. | `<oxys/ahci.h>` | 4.7 |
| `sdhci/sdhci.c` | The SD card or embedded MultiMediaCard, through its host controller. | `<oxys/sdhci.h>` | 4.8 |
| `block/block.c` | The generic block-device layer above the disk drivers. | `<oxys/block.h>` | 4.5 |
| `block/buffer.c` | The buffer cache above the block layer. | `<oxys/buffer.h>` | 4.6 |

## Planned contents

| Path | Device | Phase, sub-task |
| ---- | ------ | --------------- |
| `net/` | The Ethernet controller. | 11.1 |

## The drivers presently implemented

### `vga/` — the text-mode display

Writes directly to the character frame buffer, reached through the higher-half
mapping. It maintains the cursor position, handles the line feed, carriage
return, horizontal tabulation and backspace characters, scrolls the display when
the final row is passed, and mirrors the cursor position into the CRT controller
so that the hardware cursor is displayed correctly.

Which registers the adapter answers upon is read rather than assumed: bit 0 of
the Miscellaneous Output Register selects between the colour configuration, whose
CRT controller is at `0x03D4` and whose frame buffer is at `0x000B8000`, and the
monochrome one at `0x03B4` and `0x000B0000`. Blinking is disabled at
initialisation, which makes bit 7 of the attribute select a bright background
instead and yields all sixteen colours as backgrounds; the attribute controller
is written through its shared address and data port and read back, that port
being reached through a flip-flop whose state is not otherwise observable.

The backspace moves the cursor and erases nothing, which is what ANSI X3.4-1986
defines it to be; a caller that means to erase writes `"\b \b"`. In the first
column it crosses into the row above and stops immediately after the text
standing there, having consumed the separator between the rows and nothing else,
and it will not retreat past the **erase limit**, a position recorded by
`VgaSetEraseLimit` before which no backspace may pass. The limit is what
distinguishes a line of input from the boot log above it, the driver having no
other way to tell them apart; whoever reads the input sets it. See
[`../docs/devices/DISPLAY.md`](../docs/devices/DISPLAY.md), Section 6.

The driver is asserted at each boot by `KernelVerifyVga`, which checks the cursor
movements, the erase limit, the read-back of the hardware cursor position from
the CRT controller, and that a scroll moves the display by exactly one row. Every
failure this guards against is silent: the machine runs perfectly and only a
person reading the screen can see that anything is wrong.

The frame buffer pointer is declared `volatile`, because the memory is examined
by the display hardware independently of the processor.

### `pci/` — the bus enumeration

Not a device driver but the means of finding one. Every device driven before it
was found by knowing where it is, those addresses being inherited from the IBM
Personal Computer; no device introduced afterwards may be assumed in the same
way, and the configuration space is how a machine is asked what it contains.

Access mechanism one is used: an address composed of a bus, device, function and
register number is written to CONFIG_ADDRESS at `0x0CF8`, and the register then
appears at CONFIG_DATA at `0x0CFC`. Every access at the hardware is a double word,
so the narrower accessors extract their field from the word containing it. A
function that is not there returns all ones rather than failing, which is how
absence is detected and also why the self-test asserts that particular devices
were found: an enumerator with its address arithmetic wrong reports an empty
machine, which looks exactly like a machine with nothing in it.

Buses are reached through the host bridge and through each PCI-to-PCI bridge
found, rather than by sweeping all 256; the queue of buses awaiting a scan is
explicit rather than the call stack, and a bitmap records those already visited so
that malformed hardware cannot send the walk around a cycle. Nothing is claimed or
configured. See [`../docs/devices/PCI.md`](../docs/devices/PCI.md).

### `ata/` — the disk

Reads and writes sectors by programmed input/output. A channel answers at the
compatibility addresses `0x01F0` and `0x0170` inherited from the IBM Personal
Computer AT only while it is in compatibility mode; a PCI IDE controller states
in its programming interface which of its channels are in native mode instead,
and a native channel is addressed from the controller's base address registers,
its control block lying two bytes into the second of the pair. Both channels are
reset, and each of their two devices is identified: an ATA device
answers `IDENTIFY DEVICE` with 256 words describing itself, while a packet device
or a serial ATA device declines the command and leaves a signature in the address
registers, which is the only way to tell them from a disk that failed.

Both addressing modes are implemented. The 28-bit form keeps the four most
significant bits of the address in the register that also selects the device; the
48-bit form writes each register twice, high-order byte first, the device
retaining the previous content in a hidden half. A count register of zero means
the greatest count the mode allows, so the limits are 256 and 65536 sectors. A
write is followed by a cache flush within the same sequence, a device that has
accepted data without committing it reporting success and losing it.

The driver polls, and disables the device's interrupt at the device rather than
masking it, nothing claiming IRQ14 or IRQ15. Its only clock is the read of an I/O
port, the interval timer counting by interrupt and the interrupt flag being clear
throughout initialisation.

This driver speaks to the ATA command block registers, which is an IDE controller
and nothing else. Where nothing answers it reports what storage the machine has
and why none of it was reached — an AHCI controller, whose registers are
memory-mapped; an NVM Express controller, which is not an ATA device; an SD host
controller, where an eMMC part lives; a USB controller, where a drive lives. The
first of those has a remedy in most firmware and it is named. The others do not,
and the report says that instead of offering a setting that would not help.

The self-test reads unconditionally and writes only when the operator has booted
the GRUB entry that passes `disk-write-test`, and then only to a sector it first
read and afterwards restores. Two decisions that no board here can exercise — the
native channel's addresses and the classification of storage outside its class —
are pure functions of a configuration header, and are asserted upon headers this
project cannot obtain the hardware for. See
[`../docs/storage/DISK.md`](../docs/storage/DISK.md).

### `ahci/` — the same disks, reached the other way

The ATA driver above speaks to an IDE controller and to nothing else, and a
firmware that presents its serial ATA controller in AHCI mode puts the disks
behind memory-mapped registers that answer at no I/O port. This driver reaches
those.

It is not an extension of the driver above and could not have been. A command
here is composed in memory as a frame information structure, the adaptor is
handed the physical address of it, and the adaptor fetches the command and
transfers the sectors into the caller's own pages by bus mastering. Every
structure is therefore described by physical address, and a caller's buffer is
named to the device one page at a time, since a buffer contiguous to the
processor need not be contiguous to the device.

The adaptor is taken from the firmware where it says the firmware still holds it,
enabled, and each of the ports its bitmap names is stopped, given a page of its
own, and restarted. A port answers only where the detection and the power state
of its status register agree that a device is present and the interface active,
and the signature it then presents says whether it is a disk, a packet device, an
enclosure or a port multiplier — every one of which ends in the same sixteen bits.

One command slot is used, polled, with no interrupt enabled. A write is followed
by a cache flush within the same sequence. See
[`../docs/storage/AHCI.md`](../docs/storage/AHCI.md).

### `sdhci/` — the storage that is of neither class

An inexpensive laptop keeps its system upon an embedded MultiMediaCard, attached
to a host controller the PCI assignment specification classes as a system
peripheral rather than as mass storage. Such a machine carries no mass-storage
controller at all, so neither driver above will ever find anything upon it, and
no setting in its firmware would give them something to find.

The thing upon the bus is the *controller*; the thing holding the data is a
*card*, and the card is a second device with a command set of its own. Most of
this driver is that conversation: a card is woken, asked what it is, given an
address, asked how large it is, and selected, before a single block may be read.
Which power-up command it answers is how its kind is established — an SD card
answers ACMD41 and an embedded card does not implement it at all, answering CMD1.

The capacity is the arithmetic here most likely to be wrong and least likely to
say so. There are two encodings in the card specific data, chosen by a field of
the same register, differing in where every other field sits, in the units of the
answer, and in whether a multiplier applies; and the controller strips the low
eight bits of the register before presenting it, so every field lies eight bits
from where the specification puts it. Both encodings are asserted at every boot
against values composed for the purpose.

Blocks move through the buffer data port a word at a time, one command per block.
See [`../docs/storage/SDCARD.md`](../docs/storage/SDCARD.md).

### `block/` — the generic block-device layer

Not a driver either, but what the drivers of a medium present themselves through.
A driver registers a device by supplying a read and a write operation, a context
that means something to it and nothing to the layer, and a geometry; a caller
addresses a device by name and block number and knows nothing else about it.

Every request is judged before a driver is reached: a driver is never called with
a null buffer, a count of zero, a range outside the device, or a write to a
read-only device. Those four tests are written here once instead of in each
driver, which is most of the reason the layer exists. The range is bounded by
subtraction rather than addition, a 64-bit block number near its greatest value
making the obvious sum wrap so that a range wholly outside the device would pass.

The adaptor that presents an ATA disk as a block device lives in `ata/ata.c`, so
that the dependency runs one way: a driver knows the layer it presents itself
through, and the layer knows nothing of ATA. See
[`../docs/storage/BLOCK.md`](../docs/storage/BLOCK.md).

`block/buffer.c` holds the buffer cache above it: sixty-four blocks of 512 bytes
found through a hash of the device and the block number, discarded in least
recently used order, and written back rather than through. A buffer a caller is
holding is passed over by the eviction and never taken away, a request being
refused instead — handing the same storage to two callers would appear as
corruption somewhere else entirely. A dirty buffer is written back before its
storage is reused, since dropping it would lose a write already reported as
successful. See [`../docs/storage/BUFFER.md`](../docs/storage/BUFFER.md).

### `serial/` — the COM1 diagnostic port

An interrupt-driven driver for the 16550 compatible adapter at I/O base address
`0x03F8`, claiming IRQ4. The line parameters — signalling rate, word length,
parity and stop bits — are configurable through `SerialConfigure`, which computes
the divisor and refuses a configuration the adapter cannot express. A loopback
test at initialisation determines whether an adapter is present; if none is,
every subsequent write is discarded, so that a machine without a serial port
proceeds unimpeded rather than blocking forever upon a status flag that will
never be set.

The driver has two modes and needs both. It begins polled, because it serves the
diagnostics of the earliest initialisation, at which point no interrupt
controller exists; `SerialActivateInterrupts` promotes it once one does. The
polled path remains in use wherever no interrupt could arrive — before the line
is claimed, and whenever the interrupt flag is clear, which includes every panic.
A driver that assumed interrupts were available would queue a panic message and
halt without transmitting it.

Both directions are buffered. A writer that fills the transmit buffer waits for
room rather than losing the character; a receiver that fills the receive buffer
discards the newest and counts it, having no one to wait for. The transmitter
interrupt is enabled only while characters are queued, the condition it reports
being a level rather than an event.

This driver is the basis of the automated verification described in
[`../docs/project/TESTING.md`](../docs/project/TESTING.md): it is what makes a headless
regression test possible, which is why its polled subset was implemented in
Phase 1 rather than being deferred with the rest of Phase 4. The design is
recorded in [`../docs/devices/SERIAL.md`](../docs/devices/SERIAL.md).

### `pic/` — the 8259A interrupt controllers

Remaps the pair of cascaded 8259A controllers from their reset vectors, which
collide exactly with the architecture-defined exceptions, to vectors 32 to 47.
Masks every request line until a driver claims it, recognises a spurious request
by the absence of its bit from the in-service register, signals the
end-of-interrupt to both controllers where the cascade requires it, and — from
sub-task 6.12 — masks the pair entirely when the APIC supersedes it.

This driver differs from the others in that it is not a peripheral but a
mechanism by which other peripherals are heard. It consequently owns the
end-of-interrupt protocol on behalf of all of them, for the reasons set out in
[`../docs/design/INTERRUPTS.md`](../docs/design/INTERRUPTS.md), Section 9.4.

**It holds no handler table and routes nothing.** Sub-task 6.12 moved that to
`kernel/cpu/irq.c`, because which driver claims IR1 is a property of the machine
and not of this device: the same line is delivered by an I/O APIC upon a machine
where this pair has been retired. A device driver calls `IrqInstallHandler` and
`IrqUnmaskLine`, names a line number rather than a controller, and does not
signal completion.

### `apic/` — the controllers that supersede it

Two devices that are always named together and are not alike. `lapic.c` drives
the Local APIC, of which there is one per logical processor and which completes
every interrupt from sub-task 6.12 onward. `ioapic.c` drives the I/O APIC, of
which there are one or a few in the chipset, and whose redirection table decides
what vector each interrupt input presents and to which processor.

Both are programmed from what the firmware's ACPI tables declare, which
`kernel/acpi/acpi.c` reads. The reasoning throughout — why the 8259A is retired
rather than kept beside them, why the register pages are uncacheable, why a
redirection entry is written high half first, and why the spurious vector is
`0xFF` — is in [`../docs/devices/APIC.md`](../docs/devices/APIC.md).

From sub-task 6.13 `lapic.c` also writes the **interrupt command register**, by
which one processor sends an interrupt to another. This driver owns the register
and the bounded waits upon its delivery status; it owns none of the meanings a
vector carries, exactly as it owns none of the meanings a device request line
carries. What the vectors mean is
[`../docs/design/CONCURRENCY.md`](../docs/design/CONCURRENCY.md), Section 5.

### `pit/` — the interval timer

Programmes counter 0 of the 8253 as a rate generator and counts the interrupts it
raises upon IR0, providing the kernel's only notion of elapsed time. It is the
first device to claim a request line, and so the first demonstration that the
path from a device through the controller to a driver is sound — and, at sub-task
6.12, the device whose ticks establish that the path still holds once the I/O
APIC has taken the line over.

Mode 2 is used in preference to mode 3 because the square wave mode decrements
the count by two and therefore admits only even divisors, while nothing here has
any interest in the shape of the output waveform. The reasoning, the divisor
arithmetic and the accuracy actually obtained are recorded in
[`../docs/devices/TIME.md`](../docs/devices/TIME.md).

### `ps2/` — the 8042 controller

Not a peripheral but the controller two of them are reached through. It runs the
controller's self-test, discovers and tests each of the two device ports, owns
the single configuration byte governing both, and performs the bounded exchange
of bytes with whatever is attached.

It exists as a module of its own from sub-task 6.5, and the reason is that one
byte. The configuration byte is read, modified and written **whole**, so two
drivers each keeping their own idea of it would each write back the other's bits
as they last saw them — the mouse driver enabling its own interrupt would restore
the translation bit to whatever it was when the mouse driver first looked, and
the keyboard would then deliver scan code set 2 while decoding it as set 1. The
whole argument is in [`../docs/devices/MOUSE.md`](../docs/devices/MOUSE.md),
Section 2.

The second port's existence is discovered rather than assumed: no register
reports how many ports there are, so the port is enabled and the configuration
byte read back, a controller that has one having started its clock.

Every wait upon the controller is bounded, in accordance with convention 4 below.
A machine with no PS/2 controller decodes its ports as a constant, and an
unbounded wait upon a bit of that constant would hang the kernel during
initialisation.

### `keyboard/` — the PS/2 keyboard

Initialises the keyboard upon the controller's first port, decodes scan code
set 1 into key events carrying the character and the modifier state, and delivers
them through a circular buffer of 128 events. It does not configure the
controller and refuses to run rather than doing so: a keyboard driver that reset
the controller would silence a mouse already reporting.

Two points are easily got wrong and are recorded in
[`../docs/devices/KEYBOARD.md`](../docs/devices/KEYBOARD.md). The keyboard and the controller are
different devices reached through the same pair of ports, and they use
overlapping command numbers for unrelated purposes. And the keyboard does not
send set 1: it powers up in set 2, and set 1 is what the controller presents on
its behalf when the translation bit of the configuration byte is set — which the
controller module establishes rather than assumes, it being the controller that
translates.

Its handler reads one byte and asks which port that byte came from, both devices
delivering through the controller's single output buffer. A byte from the second
port is handed to the mouse rather than discarded: the read cannot be undone, and
a discarded byte is a third of a movement packet.

### `mouse/` — the PS/2 mouse

Initialises the mouse upon the controller's second port, interrogates it for a
wheel, decodes and frames its movement packets, accumulates the pointer position
from them, and delivers events through a circular buffer of 64.

Three properties of the packet each guard a failure that produces working, wrong
behaviour rather than an error, and all three are recorded in
[`../docs/devices/MOUSE.md`](../docs/devices/MOUSE.md). The stream is framed upon
the bit set in every packet's first byte, a driver that has lost its place
reporting plausible nonsense indefinitely rather than stopping. Each movement is
a **nine-bit** quantity whose sign lives in another byte, so the obvious
eight-bit sign extension turns −256 into zero. And the vertical sense is inverted
once, where the device is known, a mouse measuring upward as positive and a
display downward.

The position is kept here and not by the reader, because a sum is only correct if
exactly one thing performs it; the bounds it is confined to are supplied by
whoever knows the display, a mouse having no idea what it is pointing at.

## Specifications implemented

| Specification | Applied to |
| ------------- | ---------- |
| IBM Video Graphics Array technical reference | Mode 3, the 80 by 25 display; the frame buffer at `0x000B8000`; the two-byte cell of code point and attribute; the CRT controller cursor registers `0x0E` and `0x0F`, reached through ports `0x03D4` and `0x03D5`. |
| National Semiconductor PC16550D datasheet | The register map at offsets 0 to 7; the divisor latch access bit, being bit 7 of the line control register; the transmitter holding register empty flag, being bit 5 of the line status register; the loopback bit, being bit 4 of the modem control register. |
| IBM Personal Computer AT technical reference | The COM1 base address `0x03F8`, and the divisor of one yielding 115200 baud. The interrupt controllers at ports `0x20`/`0x21` and `0xA0`/`0xA1`, the slave's output attached to the master's IR2 input, IR0 being the interval timer and IR1 the keyboard. |
| Intel 8259A datasheet, sections "INITIALIZATION COMMAND WORDS (ICWS)" and "OPERATION COMMAND WORDS (OCWS)" | The four-word initialisation sequence and its side effects; OCW1 the mask register; OCW2 the non-specific end-of-interrupt; OCW3 the selection of the in-service and request registers for reading. |
| 8042 controller and PS/2 device command sets | The controller commands 0x20 and 0x60 reading and writing the configuration byte, 0xAD/0xAE and 0xA7/0xA8 enabling and disabling the two device ports, 0xAA the controller self-test answered by 0x55, 0xAB and 0xA9 the ports' tests answered by 0x00, and 0xD4 directing a byte to the second port; the configuration byte's interrupt-enable, clock-disable and translation bits, and status bit 5 naming the port a byte came from; the device commands 0xFF reset, 0xF4/0xF5 reporting, 0xF6 defaults, 0xF3 sample rate, 0xF2 identifier, 0xE8 resolution and 0xE6 linear scaling, and the answers 0xFA acknowledge and 0xFE resend. |
| PS/2 auxiliary device movement packet | The three-byte packet, its always-set framing bit, its nine-bit two's complement movements with their signs and overflow indications in the first byte, and its upward vertical sense; the sample-rate sequence 200, 100, 80 that interrogates for a wheel, and the four-byte packet a device answering 0x03 sends thereafter. |
| Intel 8254 datasheet, sections "Programming the 8254", "Mode 2: Rate Generator" and "Counter Latch Command" | The control word fields; the two-byte transfer of the count, least significant first; the periodic reload of the rate generator and the illegality of a count of one within it; the latching of a running count for reading. |
| Intel SDM, Volume 3A, Chapter 10 (Chapter 11 in a current edition) | The Local APIC: the register page at `0xFEE00000` and its uncacheable mapping; the two enables, in `IA32_APIC_BASE` bit 11 and in bit 8 of the spurious-interrupt vector register; the local vector table entry; the end-of-interrupt register; the task priority register; and the spurious vector whose low four bits are hardwired. |
| Intel 82093AA I/O APIC datasheet, Sections 3.1 and 3.2 | The indirect register pair `IOREGSEL` and `IOWIN`; the identification, version and maximum-redirection-entry registers; and the 64-bit redirection table entry with its vector, delivery mode, destination mode, polarity, trigger mode, mask and destination. |
| ACPI Specification 6.5, Sections 5.2.5.3, 5.2.6 to 5.2.8 and 5.2.12 | The Root System Description Pointer and its two checksums; the description header every table begins with; the RSDT and the XSDT; and the Multiple APIC Description Table with its processor, I/O APIC, interrupt source override and local NMI structures. |
| Intel SDM, Volume 1, Section 18.3 | The programmed input/output address space through which both devices are reached. |

Full citations are held in [`../docs/project/REFERENCES.md`](../docs/project/REFERENCES.md).

## Conventions for drivers added later

1. One subdirectory per device class; the public interface is declared in
   `kernel/include/oxys/`, not beside the implementation.
2. Every function that touches a device register cites the datasheet section
   that defines the register's behaviour.
3. Memory-mapped device registers are declared `volatile`.
4. An initialisation routine reports the absence of its device by a return value.
   A missing device is not a fault, and must never cause the kernel to block.
5. From Phase 6 onward, a driver's locking discipline is stated in its file
   header, since interrupt handlers and several processors may enter it at once.
