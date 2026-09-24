<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Block Layer

**Phase**: sub-task 4.5 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../kernel/block/block.c`](../../kernel/block/block.c),
[`../../kernel/include/oxys/block/block.h`](../../kernel/include/oxys/block/block.h).
**Specifications**: none; this is the kernel's own interface.

The interface between the disk drivers and everything that reads a medium. A
driver registers a device with two operations and a geometry; a caller names a
device and a block number and knows nothing else about it. Every request is
judged here, once, before any driver is reached.

## 1. A device

A registered device holds a name, a read and a write operation, a driver
context, a block size, a block count, a read-only flag and its own accounting.

- **The name** identifies the device to everything above. It is copied, since a
  driver may compose it on its stack, and it must be unique: a name shared by
  two devices sends a caller to whichever is found first.
- **The context** is the whole coupling. The ATA driver registers each disk with
  its `AtaDevice` as context and its operations cast it back; the layer carries
  the pointer and never looks inside. The adaptor therefore lives in the driver
  (`drivers/ata/transfer.c`), and the layer knows nothing of any disk type.

## 2. What the layer refuses

A driver is never called with a null buffer, a count of zero, a range outside
the device, or a write to a read-only device. Writing these tests once here,
rather than in every driver, is most of the reason the layer exists.

- **A range is bounded by subtraction.** `block + count > block_count` is wrong:
  the block number is 64 bits, and a value near its maximum makes the sum wrap,
  accepting a range wholly outside the device. The layer checks that `block` is
  inside the device and then that `count` does not exceed `block_count - block`.
- **Registration is refused when the declared nature and the operations
  disagree.** A writable device must supply a write operation; a read-only one
  must not. Otherwise the disagreement surfaces only at the first write.
- **A refusal is counted apart from a device error.** A refusal is the layer
  working; a single figure would show a healthy machine accumulating errors
  until the number is ignored ([`DISK.md`](DISK.md)).

## 3. Withdrawal

`BlockUnregister` frees a device's slot. The layer does not know which caches
stand above it, and a buffer written back after withdrawal would reach whatever
device took the slot. The caller therefore withdraws in a fixed order, stated in
both headers:

1. `BufferInvalidateDevice` ([`BUFFER.md`](BUFFER.md)): write back and discard.
2. `BlockUnregister`.

## Verification

`KernelVerifyBlock` in [`../../kernel/test/storage/`](../../kernel/test/storage/)
registers a device made of memory (32 blocks of 512 bytes), asserts the layer
against it, and withdraws it. A real disk is unsuitable: the `make verify`
machine has none, and a machine that has one holds data a test must not write.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A name already registered is refused. | Two devices answering to one name. |
| A writable device without a writer, and a read-only device with one, are refused. | A nature and behaviour that disagree, found at the first write. |
| A zero block size, a zero block count and an over-long name are refused. | A geometry that makes every bound meaningless; a name truncated into another's. |
| A device is found by its exact name, not by a prefix or extension. | A comparison that stops at the shorter string. |
| A block written reads back from the same block. | The context or block number mistranslated on the way to the driver. |
| A two-block transfer carries both blocks, in order. | A dropped count, or the same block passed twice. |
| A range beyond the device is refused, including one that would wrap. | The addition bug of Section 2. |
| A request without a buffer is refused; one for zero blocks succeeds and does nothing. | A null dereference on a filesystem's path. |
| A read-only device refuses a write and still reads. | A decorative read-only flag. |
| The accounting counts blocks moved, not requests. | Every multi-block transfer misreported. |
| A withdrawn device is not found, not addressable, and not withdrawn twice. | A stale pointer still reaching a driver. |

The log after the test, on a machine with one disk:

```
Block self-test passed.
Block layer: 1 devices registered of 8.
  ata0: 536870912 blocks of 512 bytes (268435456 KiB), writable, blocks read 0, written 0
Block layer: reads 4, writes 2, device errors 0, requests refused 14.
```

The fourteen refusals are the ones the test provokes.

## Limitations

1. At most eight devices.
2. The buffer cache holds 512-byte blocks only; a device of another block size
   is reachable through this layer but not cached.
3. No partitions: a device is the whole medium. This is why `/home` cannot yet
   persist ([`PERSIST.md`](PERSIST.md)).
4. No ordering or barriers; requests are issued as they arrive.
5. No lock. User threads run only on the bootstrap processor, and application
   processors reach no block device; widening that affinity requires a lock on
   the device table ([`../design/CONCURRENCY.md`](../design/CONCURRENCY.md)).
6. Synchronous only: a request returns when it completes.
