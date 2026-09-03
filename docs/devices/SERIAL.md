# The Serial Adapter

**Phase**: 4, sub-task 4.1, of [`PLAN.md`](../project/PLAN.md). The polled subset upon which
this driver was built belongs to Phase 1, sub-task 1.9.

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6. Every assertion of
hardware behaviour below carries a citation, and every specification named is
registered in [`REFERENCES.md`](../project/REFERENCES.md).

**Implementation**: [`../drivers/serial/serial.c`](../../drivers/serial/serial.c),
[`../kernel/include/oxys/serial.h`](../../kernel/include/oxys/serial.h).

## 1. What the adapter is, and why it matters more than it appears to

The serial adapter of the IBM Personal Computer AT is a 16550 compatible
universal asynchronous receiver/transmitter decoded at I/O base address `0x03F8`
and raising IRQ4. It is, by a wide margin, the least interesting device in the
machine, and it is nevertheless the most important one this project has: it is
the only channel by which the kernel can report anything to a machine that is not
the one it is running upon.

That is why it was implemented in Phase 1 rather than being deferred with the
rest of Phase 4. `make verify`, described in [`TESTING.md`](../project/TESTING.md), boots
the kernel with no display attached and asserts upon what arrives at COM1. Every
regression test the project has depends upon this device.

## 2. The register set

Per the PC16550D datasheet, Table 2 ("Register Addresses"), eight registers are
decoded at consecutive offsets from the base address. Two of them are overlaid.

| Offset | Read | Write |
| ------ | ---- | ----- |
| 0 | Receiver buffer | Transmitter holding |
| 1 | Interrupt enable | Interrupt enable |
| 2 | Interrupt identification | First-in-first-out control |
| 3 | Line control | Line control |
| 4 | Modem control | Modem control |
| 5 | Line status | — |
| 6 | Modem status | — |
| 7 | Scratch | Scratch |

While bit 7 of the line control register — the divisor latch access bit — is set,
offsets 0 and 1 address the two halves of the baud rate divisor instead. Those
are the two registers the driver uses most, which is why the bit is set only for
the two writes that need it and cleared in the same routine
(`SerialApplyLineParameters`). A driver that left it set would find its output
disappearing into the divisor latch.

## 3. The line parameters

The signalling rate is expressed by a divisor rather than directly. Per the
PC16550D datasheet, Section "Programmable Baud Generator", the divisor is the
reference oscillator frequency divided by sixteen times the desired rate; the
IBM PC/AT technical reference gives that oscillator as 1.8432 MHz, so

    divisor = 1843200 / (16 × baud) = 115200 / baud

and 115200 baud is therefore both the rate at a divisor of one and the greatest
rate the adapter can produce.

The division is truncating, so the rate obtained is `115200 / divisor` and not in
general the rate requested. The driver retains both and `SerialReport` prints
both, for the same reason the interval timer of [`TIME.md`](TIME.md) reports the
frequency it realised: an error of a known size that does not announce itself is
worse than a coarse figure that does. At the rates in common use the two agree
exactly — 115200 at divisor 1, 9600 at divisor 12, 38400 at divisor 3.

The remaining parameters occupy the line control register. The word length is
bits 1 and 0, biased so that `00` denotes five bits and `11` eight. Bit 2 selects
two stop bits rather than one; the adapter transmits one and a half rather than
two when the word length is five bits, which is a property of the hardware and
not a separate selection the driver can make.

Parity is not a plain enumeration. Bit 3 enables parity at all, bit 4 selects
even rather than odd, and bit 5 — stick parity — replaces the computed parity
with the complement of bit 4. The mark and space schemes are obtained from that
third bit and not from a code of their own, which is why `SerialComposeLineControl`
is a `switch` and not an array subscript.

`SerialConfigure` refuses a configuration it cannot express — a rate of zero, a
rate above 115200, a word length outside five to eight — and leaves the
parameters in force untouched when it does. It flushes the transmit buffer first:
anything queued was composed for the parameters standing when it was written, and
would reach the receiver as noise were the line altered beneath it.

## 4. Two modes, and why both are necessary

The driver serves the diagnostics of the earliest initialisation. At the moment
`SerialInitialise` is called there is no interrupt descriptor table, no interrupt
controller and no handler, so the driver begins by polling: it waits for bit 5 of
the line status register — the transmitter holding register empty flag — and
writes one character.

`SerialActivateInterrupts`, called after `PicInitialise`, promotes it. From that
point a character is placed in a buffer and the caller returns; the adapter
raises IRQ4 when it can take more, and the handler carries them.

The polled path is not thereby retired. It remains the path taken whenever

- the request line has not been claimed, which is the whole of early
  initialisation;
- the interrupt flag is clear, which is the state of the machine throughout
  initialisation and within a panic; or
- the transmit buffer is full and the flag is clear, so that no handler could
  ever empty it.

`SerialBufferedPathIsUsable` is the single place that decision is made, and it
reads RFLAGS through `ReadRflags` to make it. A driver that assumed interrupts
were available would, in a panic, queue the panic message and halt without ever
transmitting it — losing precisely the output the machine exists to produce at
that moment.

### 4.1 Ordering across the two modes

A character carried by polling while others sit in the buffer would overtake
them, and the diagnostic output would be scrambled in a way that looked like
memory corruption. `SerialEmit` therefore drains the buffer by polling before
transmitting directly. `KernelHalt` flushes for the same reason before clearing
the interrupt flag for the last time.

## 5. Buffering

Both directions are single-producer, single-consumer queues. For the transmit
buffer the producer is the writing code and the consumer the handler; for the
receive buffer the reverse. Neither requires a lock upon a machine of one
processor, and both acquire one in sub-task 6.13.

The indices are free-running and masked when used, the discipline described in
[`KEYBOARD.md`](KEYBOARD.md), Section 5.1: their difference is the occupancy
directly, and the arithmetic remains correct across the wrap of the index itself.
A `_Static_assert` upon each capacity enforces the power of two that the mask
depends upon.

The two capacities differ in kind, not merely in size.

| Buffer | Capacity | Full behaviour |
| ------ | -------- | -------------- |
| Transmit | 4096 characters | The writer waits for room. |
| Receive | 256 characters | The newest character is discarded and counted. |

A writer waits because a diagnostic channel that discards its output is worse
than a slow one: the boot reporting produces some kilobytes in bursts far faster
than 115200 baud can carry them, and losing the middle of a page-fault dump would
defeat the purpose of having the channel. A receiver discards because it has no
one to wait for — the character has already arrived, and nothing the kernel does
can persuade the far end to send it again. The count is reported, so an overrun
is visible rather than silent.

## 6. Servicing the adapter

The handler reads the interrupt identification register repeatedly until bit 0
reports no interrupt pending. The adapter presents its sources one at a time in
priority order, per the PC16550D datasheet, Table 5 ("Interrupt Control
Functions"), so a single pass would leave the others asserted; and because the
request reaches the interrupt controller as a level, an unserviced source would
raise IRQ4 again the instant the end-of-interrupt was signalled.

| Identification | Source | What services it |
| -------------- | ------ | ---------------- |
| `0110` | Receiver line status | Read the line status register. |
| `0100` | Received data available | Empty the receiver into the buffer. |
| `1100` | Character timeout | The same. |
| `0010` | Transmitter holding register empty | Write up to sixteen characters. |
| `0000` | Modem status | Read the modem status register. |

Each of those actions is what *resets* the corresponding interrupt, per the same
table; they are required and not merely diagnostic. The modem status interrupt is
never enabled, and its arm exists only so that a spurious one is dismissed rather
than repeated without end.

The character timeout is serviced identically to received data available. It
reports characters that have been waiting without the receiver trigger level of
fourteen being reached, and the remedy in both cases is to empty the receiver.
Without it, a trigger level above one would strand the last few characters of
every burst — which, for a terminal session, is every line the user types.

### 6.1 The transmitter interrupt is enabled only while there is something to send

This is the principal hazard of driving a 16550 by interrupt, and the reason the
interrupt enable register is shadowed in `SerialInterruptEnableShadow` rather
than written blindly.

The transmitter interrupt reports a *level*, not an event. An adapter with
nothing to send holds its transmitter holding register empty permanently. A
driver that enabled the interrupt once and left it enabled would therefore be
presented with an interrupt that no service could dismiss: the handler would
write nothing, return, and be entered again immediately, for ever. The machine
would make no further progress and would report nothing at all, because the
channel by which it reports is the device that has seized it.

`SerialStartTransmission` sets the bit when a character is queued;
`SerialTransmitAvailable` clears it upon draining the last one. Setting the bit
while the transmitter is already empty is what begins a transmission — the
adapter raises the request at once — so no separate kick is needed.

There is a benign race: the handler may drain the buffer and clear the bit
between a writer's enqueue and its call to `SerialStartTransmission`. The result
is one extra interrupt, which finds the buffer empty and clears the bit again. It
costs nothing and needs no lock.

### 6.2 The transmitter buffer holds sixteen characters

Per the PC16550D datasheet, Section "FIFO Interrupt Mode Operation", the
transmitter first-in-first-out buffer is sixteen characters deep, and the adapter
reports it empty when it has room for a full complement rather than for one
character. `SerialTransmitAvailable` therefore writes up to sixteen characters
per interrupt. A seventeenth would be discarded by the adapter without notice,
which is the sort of defect that manifests as one character missing from every
sixteenth position of a long dump.

## 7. OUT2

Bit 3 of the modem control register controls the auxiliary output OUT2, which the
PC16550D datasheet describes as "an auxiliary user-designated output" and says
nothing further about. The IBM PC/AT is what gives it meaning: on that board the
adapter's interrupt output reaches IRQ4 through a buffer that OUT2 enables.

An adapter whose OUT2 is clear can therefore be configured for interrupts,
report them in its identification register, and never be heard by the interrupt
controller. `SerialInitialise` asserts it, and the fact is recorded here because
nothing in the UART's own datasheet would ever lead a reader to it.

It is also why `SerialLoopbackTest` is conducted by polling. Local loopback
disconnects OUT2 from its pin — the datasheet's Section "MODEM Control Register"
records that the four modem control outputs are internally connected to the four
inputs in that mode — so a loopback test that waited for an interrupt might wait
for one that could not arrive.

## 8. Verification

### 8.1 The boot-time self-test

`KernelVerifySerial`, in `kernel/kernel.c`, asserts the following. Each is a
failure that would otherwise be silent.

| Property | The silent failure it guards |
| -------- | ---------------------------- |
| The request line is claimed and unmasked | A driver that works entirely by its polled path, which would appear perfect. |
| A sequence returns unaltered and in order through local loopback | Reordered or dropped characters within the first-in-first-out buffers. |
| An impossible configuration is refused | A divisor of zero, at which the adapter transmits at a rate nothing is listening at. |
| 9600 baud yields divisor 12, and the default divisor 1 | A rate mis-computed by a factor of sixteen, indistinguishable from an absent adapter. |
| A written string raises the adapter's interrupt | As the first row: the count of interrupts is what distinguishes the two paths. |
| The transmitter interrupt is withdrawn once idle | The interrupt storm of Section 6.1, which stops the machine while reporting nothing. |
| No line error occurred | Framing or parity errors from a divisor that is nearly right. |

The interrupt flag is clear throughout initialisation, so the test sets it for
the duration of the interrupt-path assertion and clears it again, as the
interrupt controller and interval timer tests do. Nothing is written to the
console between the two calls to `SerialConfigure`, because the alternative rate
stands upon the adapter between them and anything transmitted would reach a
listening terminal as noise.

### 8.2 The receive path, driven from outside the machine

The self-test cannot establish the receive path: it needs a character actually
arriving from beyond the machine. The echo loop that the kernel enters at the
completion of initialisation therefore drains the serial receive buffer as well
as the keyboard's, and the path was driven from the host:

```sh
( sleep 9; printf 'serial-in-works'; sleep 4 ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor none -serial stdio
```

The characters appeared upon the captured output after the echo loop's banner,
having traversed the adapter, IRQ4, the interrupt controller, the handler, the
receive buffer and `SerialReadCharacter`.

### 8.3 Observed state

| Quantity | Value |
| -------- | ----- |
| Base address | `0x03F8` |
| Request line | IR4, vector 36 |
| Divisor | 1 |
| Rate requested and realised | 115200 baud |
| Line parameters | 8 data bits, no parity, 1 stop bit |
| Receiver trigger level | 14 characters |

## 9. Limitations

1. Only COM1 is driven. The constants for COM2 exist and the driver is written
   in terms of a base address, but a single adapter's state is held in objects of
   static storage duration, so a second adapter would require that state to
   become a structure. Nothing needs a second adapter yet.
2. The adapter is not discovered; its base address is assumed. Sub-task 4.3
   provides PCI enumeration, and a serial adapter upon a PCI bridge would be
   found there rather than at the address the PC/AT decoded.
3. The 16550 is assumed rather than identified. Bits 7 and 6 of the interrupt
   identification register report whether the first-in-first-out buffers are
   present and usable, and are not read; upon an 8250 or a 16450 the driver would
   write sixteen characters where one was possible and lose fifteen of them. No
   machine the project targets carries one.
4. Nothing here is safe against concurrent access. The buffers tolerate one
   producer and one consumer, which is what a single processor with interrupts
   provides; the spinlock of sub-task 6.13 is required before a second processor
   writes to the console.
5. The modem control signals are asserted and then ignored. Hardware flow control
   does not exist, so a receiver that cannot keep pace has no means of saying so.
6. There is no line discipline. Received characters are delivered exactly as they
   arrived, with no editing, echo or canonical mode; that belongs to the terminal
   of Phase 8.
