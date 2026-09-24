<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Serial Adapter

**Phase**: sub-task 4.1 of [`../project/PLAN.md`](../project/PLAN.md); the polled
subset is sub-task 1.7.
**Source**: [`../../drivers/serial/serial.c`](../../drivers/serial/serial.c),
[`../../kernel/include/oxys/dev/serial.h`](../../kernel/include/oxys/dev/serial.h).
**Specifications**: National Semiconductor PC16550D data sheet (Table 2 register
addresses; Table 5 interrupt control; the baud generator; FIFO interrupt mode;
the modem control register); IBM PC/AT Technical Reference (base `0x03F8`,
IRQ4, 1.8432 MHz, OUT2).

The 16550-compatible UART at `0x03F8`, IRQ4 (COM1). It is the only channel by
which the kernel reports to another machine: `make verify` boots with no display
and asserts on what arrives here, so every regression test depends on it. That
is why its polled subset came in Phase 1.

## 1. Registers

| Offset | Read | Write |
| ------ | ---- | ----- |
| 0 | Receiver buffer | Transmitter holding |
| 1 | Interrupt enable | Interrupt enable |
| 2 | Interrupt identification | FIFO control |
| 3 | Line control | Line control |
| 4 | Modem control | Modem control |
| 5 | Line status | — |
| 6 | Modem status | — |
| 7 | Scratch | Scratch |

While line control bit 7 (the divisor latch access bit) is set, offsets 0 and 1
are the divisor instead. `SerialApplyLineParameters` sets it only for the two
divisor writes and clears it in the same routine; left set, output disappears
into the divisor latch.

## 2. Line parameters

```
divisor = 1843200 / (16 × baud) = 115200 / baud
```

115,200 baud is divisor 1 and the maximum. The division truncates, so the driver
keeps both the requested and the realised rate and `SerialReport` prints both:
an error of known size that does not announce itself is worse than a coarse
figure that does. The common rates are exact (9,600 is divisor 12; 38,400 is 3).

Line control: bits 1–0 are the word length (`00` = 5 bits … `11` = 8); bit 2
selects two stop bits (one and a half at 5 bits, by the hardware); bit 3 enables
parity, bit 4 selects even, and bit 5 (stick parity) replaces the computed parity
with the complement of bit 4, which is how mark and space parity are made. Parity
is therefore composed by a `switch` in `SerialComposeLineControl`, not an array.

`SerialConfigure` refuses a rate of zero, above 115,200, or a word length outside
5–8, leaving the current parameters in force. It flushes the transmit buffer
first: queued characters were composed for the old parameters and would arrive
as noise.

## 3. Polled and interrupt-driven

At `SerialInitialise` there is no IDT and no handler, so the driver polls: it
waits for line status bit 5 (transmitter holding register empty) and writes a
character. `SerialActivateInterrupts`, after the interrupt controllers are set up,
promotes it: characters are queued and IRQ4 carries them.

The polled path stays in use whenever the line is not yet claimed, the interrupt
flag is clear (all of initialisation, and every panic), or the buffer is full with
interrupts off. `SerialBufferedPathIsUsable` decides, reading RFLAGS. A driver
that assumed interrupts would queue a panic message and halt without sending it.

**Order across the two paths**: `SerialEmit` drains the buffer by polling before
sending directly, and `KernelHalt` flushes before clearing the interrupt flag for
the last time. Otherwise a polled character overtakes queued ones, and the log
looks like memory corruption.

## 4. Buffers

Both are single-producer, single-consumer rings with free-running indices masked
on use; their difference is the occupancy, correct across wrap. A
`_Static_assert` holds each capacity to a power of two.

| Buffer | Capacity | When full |
| ------ | -------- | --------- |
| Transmit | 4,096 | The writer waits. A diagnostic channel that drops the middle of a fault dump defeats itself. |
| Receive | 256 | The newest character is dropped and counted. It has already arrived and cannot be asked for again. |

Application processors also write diagnostics, so the transmit side has more than
one producer. The lock is `KernelWriteString`'s, around a whole call, because what
must not interleave is a line across all three output devices; a lock inside this
driver would keep the buffer consistent and still interleave lines.

## 5. The interrupt handler

The handler reads the interrupt identification register until bit 0 reports
nothing pending. Sources are presented one at a time by priority (Table 5), and
the request is a level, so a source left unserviced raises IRQ4 again at once.

| Identification | Source | Serviced by (which also resets it) |
| -------------- | ------ | ---------------------------------- |
| `0110` | Receiver line status | Reading line status. |
| `0100` | Received data available | Emptying the receiver. |
| `1100` | Character timeout | Emptying the receiver. |
| `0010` | Transmitter holding register empty | Writing up to 16 characters. |
| `0000` | Modem status | Reading modem status (never enabled; handled so a spurious one is dismissed). |

The **character timeout** reports characters waiting below the receive trigger
level of 14; without it, the end of every typed line would be stranded.

**The transmit interrupt is enabled only while there is something to send.** It
reports a level: an idle transmitter is permanently empty, so an interrupt left
enabled could never be dismissed, and the machine would spin in the handler,
silently, because the reporting channel is the device that seized it.
`SerialStartTransmission` sets the enable (shadowed in
`SerialInterruptEnableShadow`) when a character is queued, which raises the
request at once; `SerialTransmitAvailable` clears it when the buffer empties. A
race between the two costs one extra interrupt that finds nothing to do.

**Sixteen characters per interrupt.** The transmit FIFO is 16 deep and reports
empty when it can take a full complement; a seventeenth is silently dropped.

## 6. OUT2

Modem control bit 3, OUT2, is "an auxiliary user-designated output" in the data
sheet. On the PC/AT it gates the UART's interrupt to IRQ4: with OUT2 clear, the
UART raises interrupts that the interrupt controller never hears.
`SerialInitialise` sets it. Loopback mode disconnects the modem outputs, so
`SerialLoopbackTest` polls rather than waiting for an interrupt that cannot come.

## 7. Believing a status bit

A status bit read once, without being seen to change, is a guess about timing,
and emulators disagree about timing. Three rules follow, each of which VirtualBox
or Bochs broke when it was missing:

- **The presence probe waits for data ready** after writing its loopback byte.
  Reading immediately returns whatever the receiver held; QEMU delivers at once,
  VirtualBox does not, and the kernel concluded there was no adapter.
- **Loopback is entered only when the transmitter is idle**, waited for on
  **TEMT** (line status bit 6: holding and shift registers both empty), not THRE
  (bit 5, which is set while the shift register still holds a byte). Otherwise
  the last character of the log is looped back as if the test had sent it
  (`SerialWaitForTransmitterIdle`).
- **The receiver is emptied before each test character is sent**, so the data
  ready observed afterwards belongs to that character; and a surplus is detected
  as a surplus non-zero **byte** (the pattern has no zero), not a surplus flag.

## Verification

`KernelVerifySerial` in [`../../kernel/test/dev/devices.c`](../../kernel/test/dev/devices.c).
The interrupt flag is set only for the interrupt-path assertion; nothing is
written between the two `SerialConfigure` calls, since the alternative rate would
reach a listening terminal as noise.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| IRQ4 is claimed and unmasked. | A driver running entirely on its polled path, looking perfect. |
| A sequence returns unaltered and in order through loopback. | Reordered or dropped characters in the FIFOs. |
| An impossible configuration is refused. | A divisor of zero. |
| 9,600 baud gives divisor 12; the default is divisor 1. | A rate off by sixteen, indistinguishable from no adapter. |
| A written string raises the UART's interrupt. | The polled path taken silently. |
| The transmit interrupt is withdrawn once idle. | The interrupt storm of Section 5. |
| No line error occurred. | Framing or parity errors from a nearly-right divisor. |

The receive path needs a character from outside. The shell reads the terminal,
which includes COM1, so typing over the serial line exercises it
([`../project/TESTING.md`](../project/TESTING.md)).

The log reports: base `0x03F8`, IR4 on vector 36, divisor 1, 115,200 baud
requested and realised, 8N1, receive trigger 14.

## Limitations

1. Only COM1. The driver's state is static, so a second adapter needs it made a
   structure.
2. The base address is assumed, not discovered; a UART on a PCI card is not
   found.
3. The 16550 is assumed; the FIFO bits of the identification register are not
   read. On an 8250 or 16450, fifteen of every sixteen characters would be lost.
4. The receive buffer has one consumer and no lock.
5. No hardware flow control; the modem control outputs are set and ignored.
6. No line discipline here; echo, editing and control characters belong to the
   terminal above ([`../design/SHELL.md`](../design/SHELL.md)).
