# `drivers/` — Device Drivers

**Phase**: 1, sub-task 1.7, for the early output drivers. Phase 4 in full.
**Detailed design**: [`../docs/ARCHITECTURE.md`](../docs/ARCHITECTURE.md),
Section 6, records the diagnostic policy these drivers serve.

## Purpose

This directory holds the device drivers, one subdirectory per device class. A
driver implements an interface declared in `kernel/include/oxys/`; it does not
export declarations of its own. The kernel core therefore depends upon the
interface and never upon a driver's location.

## Contents

| Path | Device | Interface | Phase |
| ---- | ------ | --------- | ----- |
| `vga/vga.c` | The VGA colour text-mode display, mode 3. | `<oxys/vga.h>` | 1 |
| `serial/serial.c` | The 16550-compatible UART at COM1, polled. | `<oxys/serial.h>` | 1 |

## Planned contents

| Path | Device | Phase, sub-task |
| ---- | ------ | --------------- |
| `keyboard/` | The PS/2 keyboard, scancode set 1. | 3.7 |
| `pit/` | The programmable interval timer. | 3.6 |
| `pic/` | The 8259A programmable interrupt controller. | 3.5 |
| `pci/` | Configuration-space enumeration. | 4.3 |
| `ata/` | The ATA PIO disk driver. | 4.4 |
| `block/` | The generic block-device layer and the buffer cache. | 4.5, 4.6 |
| `mouse/` | The PS/2 mouse. | 9.4 |
| `net/` | The Ethernet controller. | 11.1 |

## The two drivers presently implemented

### `vga/` — the text-mode display

Writes directly to the character frame buffer at physical `0x000B8000`, reached
through the higher-half mapping. It maintains the cursor position, handles the
line feed, carriage return and horizontal tabulation characters, scrolls the
display when the final row is passed, and mirrors the cursor position into the
CRT controller so that the hardware cursor is displayed correctly.

The frame buffer pointer is declared `volatile`, because the memory is examined
by the display hardware independently of the processor.

### `serial/` — the COM1 diagnostic port

Configures the adapter for 115200 baud, eight data bits, no parity and one stop
bit, and transmits by polling the transmitter holding register empty flag. A
loopback test at initialisation determines whether an adapter is present; if none
is, every subsequent write is discarded, so that a machine without a serial port
proceeds unimpeded rather than blocking forever upon a status flag that will
never be set.

This driver is the basis of the automated verification described in
[`../docs/TESTING.md`](../docs/TESTING.md): it is what makes a headless
regression test possible, which is why it was implemented in Phase 1 rather than
being deferred with the rest of Phase 4.

## Specifications implemented

| Specification | Applied to |
| ------------- | ---------- |
| IBM Video Graphics Array technical reference | Mode 3, the 80 by 25 display; the frame buffer at `0x000B8000`; the two-byte cell of code point and attribute; the CRT controller cursor registers `0x0E` and `0x0F`, reached through ports `0x03D4` and `0x03D5`. |
| National Semiconductor PC16550D datasheet | The register map at offsets 0 to 7; the divisor latch access bit, being bit 7 of the line control register; the transmitter holding register empty flag, being bit 5 of the line status register; the loopback bit, being bit 4 of the modem control register. |
| IBM Personal Computer AT technical reference | The COM1 base address `0x03F8`, and the divisor of one yielding 115200 baud. |
| Intel SDM, Volume 1, Section 18.3 | The programmed input/output address space through which both devices are reached. |

Full citations are held in [`../docs/REFERENCES.md`](../docs/REFERENCES.md).

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
