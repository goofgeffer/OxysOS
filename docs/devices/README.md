# `docs/devices/` — The Hardware the Kernel Drives

One document per device. Each describes what the hardware is, what its
specification actually says — quoted and cited, never recalled — and why the
driver treats it as it does; each ends with the limitations of the driver as it
stands.

| Document | Device | Driver | Phase |
| -------- | ------ | ------ | ----- |
| [`TIME.md`](TIME.md) | Counter 0 of the 8253 programmable interval timer, and the system tick derived from it. | [`../../drivers/pit/`](../../drivers/pit/) | 3.6 |
| [`DISPLAY.md`](DISPLAY.md) | The VGA text-mode display: its register configuration, cursor, attributes and control characters — and, since sub-task 6.2, the graphics mode that displaces it. | [`../../drivers/vga/`](../../drivers/vga/) | 1.8, 4.2 |
| [`SERIAL.md`](SERIAL.md) | The 16550 serial adapter at COM1, the channel every automated test reads. | [`../../drivers/serial/`](../../drivers/serial/) | 1.9, 4.1 |
| [`KEYBOARD.md`](KEYBOARD.md) | The PS/2 keyboard upon the controller's first port, and the controller it is reached through. | [`../../drivers/ps2/`](../../drivers/ps2/), [`../../drivers/keyboard/`](../../drivers/keyboard/) | 3.7 |
| [`MOUSE.md`](MOUSE.md) | The PS/2 mouse upon the controller's second port, and the pointer drawn from it. | [`../../drivers/mouse/`](../../drivers/mouse/), [`../../graphics/cursor.c`](../../graphics/cursor.c) | 6.5 |
| [`PCI.md`](PCI.md) | The PCI bus: how a machine is asked what it contains. | [`../../drivers/pci/`](../../drivers/pci/) | 4.3 |

The storage devices are documented apart, in [`../storage/`](../storage/), because
the disk is the bottom of a stack rather than a device on its own. The framebuffer
is documented apart likewise, in
[`../design/GRAPHICS.md`](../design/GRAPHICS.md), because nothing programs it:
the boot loader sets the mode and hands over an address, and there is no
conversation with hardware to describe.

[`MOUSE.md`](MOUSE.md) is the one document here that crosses that line, and says
where the line is: the mouse is a device in the sense these documents mean, while
the pointer drawn from it is arithmetic upon memory and would be identical if the
position came from somewhere else. The two are documented together because the
division between them is the design.

## What these documents have in common

Each contains a table pairing every property its device's boot-time self-test
asserts with the silent failure that assertion exists to catch. A device driver's
characteristic defect is one the machine cannot notice — a cursor that does not
move, a sector that is not the one asked for, an interrupt that is never heard —
and the self-tests are written against that class of failure specifically.
