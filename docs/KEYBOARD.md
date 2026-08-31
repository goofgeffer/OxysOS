# The PS/2 Keyboard

**Corresponding phase**: Phase 3, sub-task 3.7.

**Specifications**: IBM Personal Computer AT technical reference, the 8042
keyboard controller and scan code set 1; the 8042 controller command set; the
PS/2 device command set.

## 1. Two devices, not one

The word "keyboard" names two devices that are programmed quite differently, and
conflating them is the first way this subsystem is got wrong.

| Device | Reached by | Governs |
| ------ | ---------- | ------- |
| The 8042 controller | Ports `0x60` and `0x64` | The ports themselves, the interrupt, the translation of scancodes, and which of its two device ports are enabled. |
| The keyboard | Bytes written to port `0x60` and passed through by the controller | Its own scanning, its scancode set, and its lamps. |

A command to the controller is written to port `0x64`. A command to the keyboard
is written to port `0x60`, whence the controller forwards it. The two use
overlapping numbers for entirely different purposes — `0xAA` is the controller's
self-test and also the keyboard's report that *its* self-test passed — so a byte
sent to the wrong port does something, and not the thing intended.

## 2. The status register, and the rule that governs every access

Port `0x64` read yields the status register. Two of its bits govern every
exchange:

| Bit | Name | Meaning when set |
| --- | ---- | ---------------- |
| 0 | Output buffer full | The controller holds a byte **for the processor**. A read of port `0x60` is valid only now. |
| 1 | Input buffer full | The controller still holds a byte **from the processor**. A write to `0x60` or `0x64` is valid only when this is *clear*. |

The names are stated from the controller's point of view, not the processor's,
which is a reliable source of confusion: the *output* buffer is what the
processor reads.

Writing while bit 1 is set overwrites a byte the controller has not yet consumed.
Reading while bit 0 is clear yields whatever the port last held, which will be
interpreted as a scancode and appear as a keystroke nobody made.

### 2.1 Every wait is bounded

`drivers/README.md` records the convention that a missing device must never cause
the kernel to block. It is nowhere more necessary than here. A machine with no
PS/2 controller — which is to say a large proportion of modern machines, where
the ports are emulated by the firmware or absent altogether — decodes port `0x64`
as a constant. An unbounded wait upon a bit of that constant would never end, and
the kernel would hang during initialisation with no message, on hardware the
developer very likely does not have in front of him.

Every wait in `drivers/keyboard/keyboard.c` is therefore bounded by iteration
count, and its expiry is reported as the absence of the device. `KeyboardInitialise`
returns false, claims no request line, and the kernel proceeds.

The bound is an iteration count rather than a duration measured by the interval
timer of sub-task 3.6, because initialisation runs with the interrupt flag clear
and the tick counter is therefore not advancing.

## 3. Scan code set 1, and where it actually comes from

`PLAN.md`, sub-task 3.7, specifies scan code set 1. The keyboard does not send
it.

A PS/2 keyboard powers up in **set 2**. Set 1 is what the processor sees only
because the 8042 translates on the keyboard's behalf, that translation being
governed by bit 6 of the controller configuration byte. The arrangement is
historical: the original Personal Computer's keyboard sent set 1, the AT's sent
set 2, and rather than break the software the AT's controller was given the
ability to present the old codes.

The firmware ordinarily leaves the translation enabled. A driver that merely
assumed set 1 would therefore work upon most machines — and fail upon the rest by
delivering plausible characters that were simply the wrong ones, since the two
sets use the same range of numbers for different keys. There would be no
diagnostic; the machine would type gibberish.

`KeyboardInitialise` accordingly **sets bit 6 explicitly** and keeps it set. The
alternative, clearing it and decoding set 2 directly, is entirely defensible, is
what a driver that must also serve a USB-attached keyboard would want, and is not
what this sub-task specifies.

### 3.1 The encoding

| Form | Encoding |
| ---- | -------- |
| A depression ("make") | The key's own code, in the range `0x01` to `0x58`. |
| A release ("break") | The same code with bit 7 set, that is, the make code plus `0x80`. |
| A key added after the original 84-key layout | The prefix `0xE0`, then the make or break code as above. |

The break code being the make code with one bit set is what allows the decoder to
treat the two identically: bit 7 is removed to obtain the key, and its former
value becomes the `pressed` field of the event.

The prefix is not itself a key. It is recorded, and the following code is
interpreted in its light.

## 4. What the decoder produces

A `KeyEvent`, declared in `kernel/include/oxys/keyboard.h`, carrying the
scancode, the character it yields under the modifiers in force, those modifiers,
whether it was a depression or a release, and whether it was extended.

Both depressions and releases are recorded, and the scancode is kept alongside
the character. A consumer that wants text ignores the releases and reads the
character, which `KeyboardReadCharacter` does on its behalf. A consumer that
wants *keys* — the window system of Phase 9, or a shell implementing a keyboard
interrupt — needs the releases, and needs the codes of the keys that produce no
character at all. Discarding either at this level would be irreversible.

### 4.1 The modifiers

| Modifier | Behaviour |
| -------- | --------- |
| Shift, control, alternate | Follow the key: set while held, cleared upon release. |
| Capitals lock | A latch: toggled by each depression, and unaffected by the release. |

The latch is toggled upon the depression alone. Toggling upon the release as well
would return it to where it began, and the key would appear to do nothing —
which is the commonest way for this to be got wrong, and one that a cursory test
does not reveal, since the state is correct again by the time anybody looks.

Left and right shift are not distinguished, nor left and right control; both set
the same flag. Nothing yet needs the distinction.

### 4.2 How shift and capitals lock combine

They do not combine in the same way for every key, and treating the lock as a
second shift is wrong.

- For a **letter**, the two combine as an *exclusive* disjunction. Shift with the
  lock engaged yields a lower-case letter, which is what every keyboard has ever
  done.
- For **every other key**, the lock is disregarded entirely. Capitals lock does
  not turn the digit 1 into an exclamation mark.

The self-test asserts both, the second being the assertion that fails if the lock
has been implemented as a second shift.

### 4.3 Extended keys yield no character

An extended code shares its number with an ordinary key: extended `0x1C` is the
keypad's enter and ordinary `0x1C` the main one. The character tables are indexed
by the number alone, so consulting them for an extended code would yield the
character of the wrong key.

An extended event therefore carries no character. The two extended keys that
genuinely produce one — the keypad's enter and solidus — are left to a later
phase rather than given a table of their own for two entries.

## 5. The circular buffer

A fixed array of 128 events with two free-running indices, one advanced only by
the producer and one only by the consumer.

### 5.1 Why the indices are not wrapped

They increase without bound and are masked when used to subscript the array.
The usual alternative, wrapping each index to the capacity, makes equal indices
mean either an empty buffer or a full one, and requires a further datum to say
which. Here the difference of the two indices *is* the occupancy, and unsigned
arithmetic keeps that true across the wrap of the indices themselves.

The capacity is a power of two so that the masking is a bitwise operation rather
than a division, the operation being performed inside an interrupt handler.

### 5.2 Why an overrun discards the newest event

A full buffer refuses the new event; it does not overwrite the oldest.

The oldest events are the characters typed first. For a line of input the
beginning matters more than the end, and a consumer that had read half a line
would find the half it had not yet read silently rewritten by a later burst. The
discard is counted, so that the loss is visible in the report rather than merely
suffered.

### 5.3 Why no lock is required

There is one producer, the interrupt handler, and one consumer. The producer
advances the write index alone; the consumer advances the read index alone; each
reads the other's index without modifying it. The event is written *before* the
write index is advanced, so a consumer that observes the advance is guaranteed a
complete event beneath it.

From sub-task 6.9, with several consumers possible, the consumer's side requires
the spinlock governing this device. The producer's side will not: there is one
keyboard, and therefore one producer.

## 6. The initialisation sequence

1. Disable both device ports (`0xAD`, `0xA7`), so that nothing arrives while the
   controller is being reconfigured and no byte read below belongs to a keystroke
   rather than to the exchange in progress.
2. Drain the output buffer. The firmware has been using the keyboard and may have
   left a keystroke or the tail of a command exchange behind; such a byte would
   be decoded as a scancode.
3. Read the configuration byte (`0x20`), clear both ports' interrupt enables and
   the first port's clock-disable bit, set the translation bit, and write it back
   (`0x60`).
4. Run the controller self-test (`0xAA`); expect `0x55`.
5. **Write the configuration byte again.** The self-test resets the controller
   upon some implementations, discarding what was written at step 3. Upon an
   implementation that does not, this is merely redundant. The failure it prevents
   is a keyboard that works upon the developer's machine and not upon the user's.
6. Test the first port (`0xAB`); expect `0x00`.
7. Enable the first port (`0xAE`).
8. Reset the keyboard (`0xFF`); expect the acknowledgement `0xFA`, then `0xAA`
   reporting its self-test. The second byte is read but not insisted upon, some
   emulated keyboards omitting it.
9. Enable scanning (`0xF4`).
10. Drain again; nothing left by the reset is a keystroke.
11. Set the first port's interrupt enable in the configuration byte.
12. Register the handler, **then** unmask IR1.

The order of step 12 matters. Were the line unmasked first, a keystroke arriving
between the two would be recorded by the interrupt controller as an unclaimed
request and lost.

### 6.1 The handler reads exactly one byte

The controller raises its request once for each byte it has to offer. A handler
that drained the buffer in a loop would consume bytes whose requests had not yet
been delivered, and those requests would then arrive to find nothing to read.
They would be counted as spurious or unclaimed, and the accounting of
`docs/INTERRUPTS.md`, Section 9, would cease to mean anything.

## 7. Verification

A keyboard cannot be made to produce a keystroke by the kernel that drives it, so
the verification is in two parts.

### 7.1 The decoder, driven directly

`KeyboardProcessScancode` is exposed and the self-test drives it with codes of
the kernel's own choosing — exactly the bytes the hardware would deliver. This is
not a concession to testability: the decoding of set 1 is not a property of the
8042, and a scancode arriving by any other route decodes identically.

| Assertion | The failure it detects |
| --------- | ---------------------- |
| An unshifted key yields its lower-case character | The table is misindexed. |
| A release is decoded as a release, bearing the same key | Bit 7 is not being removed, or is being lost. |
| A modifier produces no event of its own, and sets its flag | Modifiers reaching consumers of text as spurious characters. |
| A shifted letter yields its capital, and a shifted digit its punctuation | A shifted table that is a mere case conversion. |
| Capitals lock latches upon a full keystroke and capitalises a letter | A latch toggled upon release as well, which appears to do nothing. |
| Capitals lock does **not** alter a digit | The lock implemented as a second shift. |
| Shift with the lock engaged yields lower case | The two combined as a disjunction rather than an exclusive one. |
| An extended modifier sets its flag and produces no event | The prefix being treated as a key. |
| An extended non-modifier is marked extended and bears no character | An extended key decoded as its ordinary twin. |
| Reading a character skips releases | Every keystroke appearing twice. |
| An overrun is counted exactly, and the events accepted survive intact | A buffer that corrupts rather than refuses, which is far worse than one that loses keystrokes. |
| The buffer holds exactly its stated capacity | An off-by-one in the occupancy arithmetic. |

### 7.2 The interrupt path, driven by a real keystroke

The above establishes the decoder and leaves the path from the physical key to
it — the controller raising IR1, the interrupt controller routing it, the handler
reading the data port — asserted only as configured state.

That path is exercised by the echo loop `KernelKeyboardEcho`, which the kernel
enters at the completion of Phase 3 in place of halting, and which prints every
character typed. It was driven from the QEMU monitor:

```sh
( sleep 6; for k in h e l l o spc o x y s; do echo "sendkey $k"; sleep 0.15; done; \
  sleep 1; echo quit ) \
  | qemu-system-x86_64 -machine q35 -cpu qemu64 -smp cores=2 -m 512M \
      -cdrom build/oxys.iso -display none -monitor stdio -serial file:/tmp/kbtest.log
```

The captured serial output ends `hello oxys`. This is the only assertion in the
project so far that exercises a device end to end, from a physical event to a
character, and it is the reason the echo loop exists.

### 7.3 The loop halts the processor, and the order of two instructions matters

The loop executes `STI` followed immediately by `HLT`. Intel SDM, Volume 2B,
"STI", provides that the instruction's effect is delayed by one instruction, so
the `HLT` is executed before any interrupt can be taken.

Reversing the two, or placing anything between them, opens a window in which a
keystroke is serviced and the processor then halts with nothing left to wake it.
The machine would appear to work and would freeze upon a keystroke that happened
to fall in the window — which is to say, rarely, and irreproducibly.

## 8. Observed state

At the completion of the self-test under QEMU:

| Quantity | Value |
| -------- | ----- |
| Controller | Present; self-test and port test passed |
| Scan code set | 1, by controller translation |
| Request line | IR1, vector 33, unmasked |
| Scancodes decoded by the self-test | 161 |
| Events produced | 140 |
| Events discarded by the deliberate overrun | 8 |

## 9. Limitations

1. The lamps are not driven. Capitals lock changes the decoding but not the light,
   the `0xED` command requiring an acknowledgement exchange that is unattractive
   to perform from within an interrupt handler. It belongs with the shell of
   Phase 8, which is the first thing that will care.
2. Number lock is not tracked, so the keypad always yields digits. The cursor
   movements it selects require the latch and a second table.
3. Only the first device port is used. The mouse of sub-task 9.4 occupies the
   second, which is presently disabled.
4. The two extended keys that produce characters, the keypad's enter and solidus,
   produce none.
5. There is no notion of a keyboard interrupt, a line discipline, or echo control.
   Those are properties of a terminal rather than of a keyboard and belong to the
   shell of Phase 8.
6. The consumer's side of the buffer is unsynchronised; from sub-task 6.9 it
   requires the spinlock governing this device.
7. The "fake shift" sequences are not suppressed. The controller emits `E0 2A`
   before, and `E0 AA` after, several extended keys — the keypad's solidus, and
   the cursor keys while number lock is engaged — in order that software unaware
   of those keys should see a plausible shifted keystroke. This driver declines
   to treat an extended `0x2A` as a shift, which is correct, but then decodes it
   as an ordinary key and emits a `KeyEvent` bearing that scancode, `extended`
   set and no character. A consumer of characters is unaffected, since the event
   carries none; a consumer that counts key events, such as the window system of
   sub-task 9.7, would see phantom keys. Suppressing them requires the driver to
   recognise the sequence as a whole rather than one code at a time.
