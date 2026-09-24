<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# The Buffer Cache

**Phase**: sub-task 4.6 of [`../project/PLAN.md`](../project/PLAN.md).
**Source**: [`../../kernel/block/buffer.c`](../../kernel/block/buffer.c),
[`../../kernel/include/oxys/block/buffer.h`](../../kernel/include/oxys/block/buffer.h).
**Specifications**: none; this is the kernel's own interface.

Recently used blocks, held in memory and handed out. A filesystem reads the same
few blocks constantly (the superblock, a group descriptor, an inode table), and
a disk driven by programmed I/O moves every word through the processor
([`DISK.md`](DISK.md)); without the cache the filesystem is unusable. The hard
parts are deciding which block to discard and when a modified block reaches the
device.

## 1. A buffer

A buffer holds one block of one device. Its identity is the pair (device, block
number): block zero of two devices are different blocks. The device is part of
the hash as well as the comparison, so the two do not share a bucket.

`BufferGet` returns a buffer; the caller reads and writes its `data` and returns
it with `BufferRelease`. Between the two the buffer is **held**, and a held
buffer is never taken away.

## 2. Lookup and eviction

Sixty-four 512-byte buffers are allocated from the heap in one request. They are
found through a hash of 128 buckets: a power of two, so the bucket is a mask, and
more buckets than buffers, so chains stay short.

Recency is a doubly linked list through the same entries. `BufferGet` moves the
entry it returns to the newest end; `BufferClaim` takes from the oldest.

- **Every entry is in the list from the start**, unused ones at the oldest end.
  `BufferClaim` has one rule, take the oldest, and there is no separate free
  list to keep consistent.
- **A held buffer is passed over, never evicted.** `BufferClaim` walks towards
  the newest end for an entry nobody holds. If there is none, `BufferGet`
  returns null. The alternative is the same storage handed to two callers, one
  of whom holds a block replaced beneath it: corruption that appears somewhere
  else, much later.
- **An entry records whether it is in the list** (`listed`), and detaching an
  unlisted entry does nothing. Detaching one that is not listed would write its
  empty neighbours over both ends of the list and lose every other entry, which
  in use shows only as slowness.

## 3. Write-back

A modified buffer is marked dirty and the device is not touched. The block is
written back when the buffer is evicted, flushed (`BufferFlush`), synchronised
(`BufferSync`) or invalidated, and not before. Writing through on every
modification would make a filesystem that updates an inode three times pay three
sector writes, the most expensive thing the kernel does.

**An eviction writes back before it reuses the storage.** Dropping a dirty block
loses a write already reported as successful, discovered only when the block is
read again. If the write-back fails, `BufferClaim` abandons the claim.

## 4. Invalidation

`BufferInvalidateDevice` writes back every dirty block of a device and discards
all of its blocks. It is the first step of withdrawing a device
([`BLOCK.md`](BLOCK.md)): a buffer written back after withdrawal would reach the
device registered in its place.

Invalidation is refused while any buffer of the device is held: the holder is
about to write into storage that would no longer belong to any block.

## Verification

`KernelVerifyBuffer` in [`../../kernel/test/storage/`](../../kernel/test/storage/)
runs against the memory device of [`BLOCK.md`](BLOCK.md). Every failure of a
cache is silent by construction, so each assertion makes one fail at the point
of the defect.

| Property asserted | The failure it would catch |
| ----------------- | -------------------------- |
| A block not held is read from the device with the device's contents. | The wrong entry returned, or no read made. |
| The same block asked for again is the same buffer, counted a hit, with no device access. | A cache that caches nothing. |
| Two blocks occupy two buffers with their own contents. | A hash or comparison that conflates them. |
| A modified block does not reach the device until written back. | Write-through posing as write-back, hiding the eviction bug below. |
| Synchronising writes it back and leaves nothing dirty. | A dirty mark never acted on. |
| A dirty block evicted under pressure is written back as it goes. | A write lost outright and reported nowhere. |
| An evicted block is read from the device again. | An eviction that unlinked nothing. |
| A held buffer survives 64 later misses with its contents. | Storage handed out twice. |
| With every buffer held, a further request is refused. | The same, under pressure. |
| A device with a held buffer cannot be invalidated. | Discarding storage a caller is writing into. |
| Invalidation writes back the dirty, discards the rest, and the next request misses. | A withdrawn device's blocks written to its successor. |
| Nothing beneath the cache failed. | An off-by-one in the pressure loops asking for a block that should not be asked for. |

The log after the test, which leaves the cache empty because it invalidates its
device before withdrawing it:

```
Buffer self-test passed.
Buffer cache: 64 buffers of 512 bytes (32 KiB), 0 holding a block, 0 dirty, 0 held.
Buffer cache: hits 5, misses 197, evictions 131, write-backs 3, failures 0.
```

## Limitations

1. One block size, 512 bytes.
2. A fixed store of 64 buffers; it neither grows under pressure nor shrinks when
   idle.
3. Strict least-recently-used order: one sweep of a large file evicts everything
   worth keeping.
4. No read-ahead; a sequential reader misses on every block.
5. No background write-back; a dirty block waits for an eviction, flush or sync.
6. No lock, and reference counts are not atomic. Before user threads leave the
   bootstrap processor, one lock must cover a whole lookup and its outcome (the
   search, any eviction, the reference taken): two processors that both miss on
   one block and both claim an entry leave it cached twice. A miss waiting on a
   device is the first place the kernel will want a lock it can sleep on
   ([`../design/CONCURRENCY.md`](../design/CONCURRENCY.md)).
