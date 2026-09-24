<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The PS/2 Keyboard

**Phase**: sub-task 3.7 of [`../project/PLAN.md`](../project/PLAN.md); the
controller was separated from it in sub-task 6.5.
**Source**: [`../../drivers/keyboard/keyboard.c`](../../drivers/keyboard/keyboard.c),
[`../../kernel/include/oxys/dev/keyboard.h`](../../kernel/include/oxys/dev/keyboard.h);
the controller in [`../../drivers/ps2/ps2.c`](../../drivers/ps2/ps2.c).
**Specifications**: IBM PC/AT Technical Reference (the 8042 and scan code set 1);
the 8042 command set; the PS/2 keyboard command set; Intel SDM, Volume 2B, `STI`.

The keyboard on the first port of the 8042 controller: initialisation, the
decoding of scan code set 1 into key events with modifiers, and the buffer the
events wait in.

## 1. Two devices

| Device | Reached by | Governs |
| ------ | ---------- | ------- |
| The 8042 controller | Ports `0x60` (data) and `0x64` (status/command) | Its two ports, the interrupt, scancode translation. |
| The keyboard | Bytes written to `0x60`, forwarded by the controller | Scanning, its scancode set, its lamps. |

The two use overlapping numbers for different things (`0xAA` is the controller's
self-test command and the keyboard's "self-test passed"), so a byte sent to the
wrong one does something, just not the intended thing.

The controller has its own module, `ps2.c`, shared with the mouse on the second
port. The configuration byte governs both ports and is written whole, so two
drivers each keeping their own copy would each write back the other's bits as
they last saw them ([`MOUSE.md`](MOUSE.md)).

## 2. The status register

| Bit | Name | Set means |
| --- | ---- | --------- |
| 0 | Output buffer full | A byte waits **for the processor**; only now is a read of `0x60` valid. |
| 1 | Input buffer full | The controller has not taken the last byte **from the processor**; write only when clear. |

The names are from the controller's side. A write while bit 1 is set overwrites
an unconsumed byte; a read while bit 0 is clear returns a stale byte decoded as a
keystroke nobody made.

**Every wait is bounded** by an iteration count, and expiry is reported as "no
device". Many machines have no 8042, and port `0x64` then reads as a constant; an
unbounded wait would hang initialisation with no message. The bound is a count,
not a time, because the timer does not advance while interrupts are masked during
initialisation.

## 3. Scan code set 1

A PS/2 keyboard sends **set 2**. The processor sees set 1 only because the 8042
translates, under bit 6 of its configuration byte (the AT's compromise with the
original PC's keyboard). Firmware usually leaves translation on; a driver merely
assuming set 1 would type plausible gibberish on machines where it is off, since
both sets use the same numbers for different keys. `Ps2Initialise` therefore sets
bit 6 explicitly.

| Form | Encoding |
| ---- | -------- |
| Make (press) | The key's code, `0x01`–`0x58`. |
| Break (release) | The make code with bit 7 set. |
| A key added after the 84-key layout | Prefix `0xE0`, then make or break. |

Removing bit 7 gives the key; its value gives `pressed`. The prefix is recorded
and applied to the next code; it is not a key.

## 4. Events

A `KeyEvent` carries the scancode, the character under the current modifiers, the
modifiers, pressed or released, and whether extended. Releases and characterless
keys are kept, because the window system and the terminal need keys, not just
text; discarding them here would be irreversible. `KeyboardReadCharacter` serves
a consumer that wants text, skipping releases.

| Modifier | Behaviour |
| -------- | --------- |
| Shift, Control, Alt | Set while held, cleared on release. Left and right are not distinguished. |
| Caps Lock | Toggled on press only; toggling on release too would appear to do nothing. |

- **Shift and Caps Lock combine by exclusive or for letters only.** Shift with
  Caps Lock gives lower case; Caps Lock does not turn `1` into `!`.
- **An extended key yields no character.** Extended `0x1C` is keypad Enter and
  ordinary `0x1C` is the main Enter; the tables are indexed by number, so
  consulting them would give the wrong key's character.

## 5. The buffer

128 events with two free-running indices, masked on use: their difference is the
occupancy, with no empty/full ambiguity, correct across wrap. The capacity must be
a power of two for the mask, enforced by `_Static_assert` (a capacity of 100
would silently address only 64 entries).

- **An overrun drops the newest event** and counts it. The oldest are the start
  of the line being typed, which matters more than its end.
- **No lock between producer and consumer.** The handler alone advances the write
  index, and writes the event before advancing it; the consumer alone advances
  the read index.

## 6. Initialisation

The controller (`Ps2Initialise`, once, before both device drivers):

1. Disable both ports (`0xAD`, `0xA7`), so nothing arrives mid-configuration.
2. Drain the output buffer: firmware may have left a keystroke or a command reply.
3. Read the configuration byte (`0x20`); clear both interrupt enables and the
   first port's clock-disable; set translation; write it (`0x60`).
4. Controller self-test (`0xAA`), expecting `0x55`.
5. **Write the configuration byte again.** Some controllers reset on self-test and
   lose step 3.
6. Detect and test the second port ([`MOUSE.md`](MOUSE.md)).
7. Test the first port (`0xAB`), expecting `0x00`.

The keyboard (`KeyboardInitialise`):

8. Enable the first port (`0xAE`).
9. Reset the keyboard (`0xFF`): expect `0xFA`, then `0xAA` (read but not required;
   some emulators omit it).
10. Enable scanning (`0xF4`).
11. Drain again.
12. Set the first port's interrupt enable.
13. Register the handler, **then** unmask IR1; the other order loses a keystroke
    arriving in between as an unclaimed request.

The keyboard driver never reconfigures the controller, and refuses to run rather
than do so: it would silence a mouse already reporting.

**The handler reads exactly one byte**, since the controller raises one request
per byte; draining in a loop leaves later requests with nothing to read, counted
as spurious. It asks **which port** the byte came from and hands a mouse byte to
the mouse decoder: the read cannot be undone, and a lost byte desynchronises
every later mouse packet.

## 7. From keystroke to program

Key events reach programs through the terminal
([`../design/SHELL.md`](../design/SHELL.md)), which turns them into bytes and
control sequences. Where there is no root filesystem, the kernel's echo loop
(`KernelEchoLoop`) prints what is typed instead. Its wait is `STI` immediately
followed by `HLT`: `STI` takes effect after the next instruction, so no interrupt
can arrive between them and leave the processor halted with nothing to wake it.

## Verification

`KernelVerifyKeyboard` in [`../../kernel/test/dev/devices.c`](../../kernel/test/dev/devices.c)
drives `KeyboardProcessScancode` with exactly the bytes the hardware would send;
the decoding of set 1 does not depend on the route a byte took.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| An unshifted key yields its lower-case character. | A misindexed table. |
| A release is a release of the same key. | Bit 7 not removed. |
| A modifier sets its flag and produces no event. | Modifiers arriving as characters. |
| Shifted letters give capitals; shifted digits, punctuation. | A shifted table that only changes case. |
| Caps Lock latches over a full keystroke and capitalises letters. | A latch toggled on release too. |
| Caps Lock does not alter a digit. | Caps Lock implemented as a second Shift. |
| Shift with Caps Lock gives lower case. | Inclusive instead of exclusive or. |
| An extended modifier sets its flag without an event; an extended key is marked and has no character. | The prefix treated as a key; the ordinary twin's character. |
| Reading characters skips releases. | Every keystroke twice. |
| An overrun is counted exactly and accepted events survive; the buffer holds exactly its capacity. | Corruption instead of refusal; occupancy off by one. |

The interrupt path from a real key is exercised by typing at the shell, or by the
QEMU monitor's `sendkey` ([`../project/TESTING.md`](../project/TESTING.md)). The
log reports the controller present, set 1 by translation, and IR1 on vector 33.

## Limitations

1. The lamps are not driven; `0xED` needs an acknowledged exchange.
2. Num Lock is not tracked; the keypad always gives digits.
3. Keypad Enter and keypad `/` give no character.
4. The controller's "fake shift" sequences (`E0 2A` … `E0 AA` around some extended
   keys) are decoded as extended keys with no character. Text consumers are
   unaffected; a consumer counting key events sees phantom keys.
5. The consumer side has no lock; one consumer is assumed until user threads leave
   the bootstrap processor.
