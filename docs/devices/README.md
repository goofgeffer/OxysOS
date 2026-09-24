<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `docs/devices/` — The Hardware the Kernel Drives

One document per device: what the hardware is, what its specification says (cited,
never recalled), why the driver treats it as it does, what the self-test asserts,
and what the driver does not yet do.

| Document | Device | Phase |
| -------- | ------ | ----- |
| [`SERIAL.md`](SERIAL.md) | The 16550 UART at COM1, which every automated test reads. | 1.7, 4.1 |
| [`DISPLAY.md`](DISPLAY.md) | The VGA text console. | 1.7, 4.2 |
| [`TIME.md`](TIME.md) | The 8254 interval timer, the real-time clock, `time` and `alarm`. | 3.6, 9.7 |
| [`KEYBOARD.md`](KEYBOARD.md) | The PS/2 keyboard, and the 8042 controller's initialisation. | 3.7 |
| [`PCI.md`](PCI.md) | The PCI configuration space: what the machine contains. | 4.3 |
| [`MOUSE.md`](MOUSE.md) | The PS/2 mouse, the shared 8042 controller, and the pointer. | 6.5 |
| [`ACPI.md`](ACPI.md) | The MADT: which interrupt controllers exist and how ISA lines reach them. | 6.12 |
| [`APIC.md`](APIC.md) | The Local APIC and I/O APIC. | 6.12–6.15 |

Elsewhere: disks and SD cards are the bottom of the storage stack
([`../storage/`](../storage/README.md)); the framebuffer is set up by the boot
loader, not programmed, and is [`../design/FRAMEBUFFER.md`](../design/FRAMEBUFFER.md).
`ACPI.md` describes tables rather than a device, and is here because `APIC.md`
cannot be read without it.
