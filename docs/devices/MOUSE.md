<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The PS/2 Mouse and the Pointer

**Phase**: sub-task 6.5 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../drivers/ps2/ps2.c`](../../drivers/ps2/ps2.c),
[`../../drivers/mouse/mouse.c`](../../drivers/mouse/mouse.c),
[`../../graphics/cursor.c`](../../graphics/cursor.c); declared by
`<oxys/dev/ps2.h>`, `<oxys/dev/mouse.h>`, `<oxys/gfx/cursor.h>`.
**Specifications**: IBM PS/2 auxiliary device; the 8042 command set; the PS/2
device command set, the standard movement packet and its wheel extension.

The mouse on the 8042's second port, the controller both PS/2 devices share, and
the pointer drawn from the mouse's position. How the pointer is composited is
[`../design/COMPOSITOR.md`](../design/COMPOSITOR.md).

## 1. Three modules

| Module | Owns | Knows nothing of |
| ------ | ---- | ---------------- |
| `ps2.c` | The 8042: its ports, self-test, two device ports, and the one configuration byte for both. | What is attached. |
| `mouse.c` | The mouse: its commands, the packet, framing, and the accumulated position. | The display. |
| `cursor.c` | The pointer's shape and its rendering as a compositor layer. | Where the position came from. |

## 2. The controller has one owner

The configuration byte serves both ports:

| Bit | Set means | Whose |
| --- | --------- | ----- |
| 0 | Port 1 may raise IR1 | Keyboard |
| 1 | Port 2 may raise IR12 | Mouse |
| 4 | Port 1's clock is stopped | Controller |
| 5 | Port 2's clock is stopped | Controller |
| 6 | Set 2 is translated to set 1 | Controller, for the keyboard |

It is read, modified and written whole. Two drivers each holding a copy would
write back each other's bits as last seen: the mouse enabling bit 1 would restore
bit 6 to an old value, and the keyboard would then decode set 2 as set 1,
producing mostly plausible, sometimes wrong characters. So `Ps2Initialise` runs
once before either driver ([`KEYBOARD.md`](KEYBOARD.md) lists its steps), sets
translation itself, and each driver asks it for its port.

**The second port is detected, not assumed.** No register says how many ports
exist. The second port is enabled and the configuration byte read back: its clock
bit (5) clears only if the port exists. This is done before either port's test,
since testing a missing port reports a failure the machine does not have.

## 3. The packet

Three bytes, or four with a wheel:

| Byte | Bits | Meaning |
| ---- | ---- | ------- |
| 0 | 0, 1, 2 | Left, right, middle buttons |
| 0 | 3 | **Always 1** |
| 0 | 4, 5 | Signs of X and Y |
| 0 | 6, 7 | Overflow of X and Y |
| 1 | all | X magnitude |
| 2 | all | Y magnitude |
| 3 | 0–3 | Wheel, two's complement (wheel devices only) |

- **A movement is nine bits.** Magnitude `0xFF` with the sign set is −1;
  magnitude `0x00` with the sign set is **−256**. Treating the magnitude as a
  signed char gets −1 right and turns −256 into 0, losing exactly the fastest
  movements. The driver subtracts 256 when the sign is set.
- **Y is positive upward** on the device and downward on the screen. The negation
  is made once, in `MouseCompletePacket`; forgetting it gives a pointer that moves
  backwards vertically, which looks like a broken mouse.
- **An overflowed movement is discarded** and counted, since its magnitude is the
  low bits of a larger number. The buttons in the same byte are kept.

## 4. Framing

Nothing in the byte stream marks where a packet starts. A decoder that loses its
place keeps working, reading buttons from magnitudes and movements from button
bytes, indefinitely and plausibly. Bit 3 of the first byte is always set, so a
byte without it is refused while a packet is awaited; bytes are discarded singly
until one could start a packet, and the discards are counted. `MouseFlush`
abandons a partial packet with the buffered events; keeping the fragment would
append the next byte to a packet whose remainder was thrown away.

## 5. One output buffer for two devices

The controller holds one byte at a time for both ports. Status bit 5 says which
port it came from and must be read **before** the data port, which clears it. A
byte waiting when a handler runs is often the other device's (a packet arriving
during a keystroke), so each handler reads the byte, checks its port, and hands
it to the other driver if it is not its own. The read cannot be undone; dropping
the byte would desynchronise every later mouse packet.

## 6. The position

A mouse reports movement; the position is their running sum, kept in the driver.
A sum is correct only if exactly one party performs it: a consumer that missed an
event would be displaced for ever. An event therefore carries the position as
well as the movement.

The bounds belong to whoever knows the display and are set by `MouseSetBounds`.
Until then they are one pixel square, so the position is the origin: a driver
defaulting to a plausible screen size would report a position it invented.

## 7. The pointer

The shape is two bitmaps, opacity and interior, giving three states per pixel:
transparent, outline, interior. A white arrow in a black outline is visible on any
background. It was drawn for this project (`PROJECT_GUIDELINES.md`, Section 2).

The shape is rendered once to a surface and composited as a layer, so nothing is
saved or restored beneath it ([`../design/COMPOSITOR.md`](../design/COMPOSITOR.md)).
`CursorMoveTo` returns at once when the position has not changed: a stationary
mouse still sends about a hundred packets a second, and each would otherwise mark
the display changed.

## Verification

`KernelVerifyMouse` and `KernelVerifyCursor` in
[`../../kernel/test/dev/mouse.c`](../../kernel/test/dev/mouse.c). Neither needs a
mouse: `MouseProcessByte` is driven with composed packets, as the keyboard
decoder is. What only a real device can show (it answered, the line is claimed,
the port is usable) is checked against the driver's report and skipped when no
mouse exists, which is legitimate; reaching the check shows detection did not
block.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A plain packet yields its movement, position and buttons. | Bytes read in the wrong order. |
| Vertical movement is inverted. | A pointer moving backwards vertically. |
| `0xFF` with sign is −1; `0x00` with sign is −256. | Eight-bit sign extension. |
| The position stops at each of the four edges. | A hot spot one pixel outside the display, where it cannot be drawn. |
| A button transition is reported once, named. | Consumers tracking previous state themselves. |
| An overflowed movement is dropped, its buttons kept. | The pointer jumping under fast movement. |
| A byte without bit 3 is refused and counted, and the stream recovers. | Indefinite plausible nonsense. |
| A flush abandons a partial packet; a full buffer drops the newest and counts it. | Mis-framing introduced by the flush; events overwritten. |
| Every interior pixel is opaque; the hot spot is opaque; the shape is neither empty nor full. | A hole in the shape; an unaimable pointer; a mask read in the wrong bit order. |
| The rendered surface matches the shape: coverage from opacity, colour from interior. | A solid rectangle; a pointer drawn inside out. |
| Showing twice leaves it shown; a move goes where sent; a move to the same place is not counted. | The display redrawn a hundred times a second for a hand at rest. |

On QEMU at 1280 by 800 with no mouse moved, the log reports the controller with
both ports usable, configuration `0x40` (the interrupt bits are set after the
report), a wheel mouse with four-byte packets, and counts of packets, mis-framed
bytes and discards that are the self-test's own.

**Headless capture**: a key injected through the QEMU monitor before the
controller is initialised lands after the initial drain and is read as a command
reply, so the machine reports no controller at all. Let GRUB's timeout elapse, or
wait for the boot to finish, before sending keys.

## Limitations

1. Buttons 4 and 5 are not decoded; the device is not put into the mode where the
   fourth byte carries them.
2. The wheel movement is carried in events and used by nothing.
3. No acceleration; scaling is linear at the device.
4. A USB mouse works only through firmware PS/2 emulation.
