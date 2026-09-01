# The Buffer Cache

**Phase**: 4, sub-task 4.6, of [`PLAN.md`](../project/PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../drivers/block/buffer.c`](../../drivers/block/buffer.c),
[`../kernel/include/oxys/buffer.h`](../../kernel/include/oxys/buffer.h).

## 1. What the cache is for

A disk reached by programmed input/output moves every word of every sector
through a register, with the processor doing the moving; see
[`DISK.md`](DISK.md), Section 5. A filesystem reads the same few blocks
constantly — a superblock, a block group descriptor, an inode table — and reading
each of them from the medium every time is the difference between a filesystem
that works and one that is unusable.

The cache holds recently used blocks in memory and hands them out. That is the
whole of the idea, and every difficulty in it comes from the two things that
follow: something must decide which block to discard when the store is full, and
something must decide when a modified block reaches the device.

## 2. What a buffer is

A buffer holds one block of one device. Its identity is the pair — the device and
the block number — and not the block number alone: block zero of one device and
block zero of another are different blocks, and a cache that keyed upon the
number would return one for the other. The device forms part of the hash as well
as of the comparison, so that the two do not even share a bucket.

A caller obtains a buffer with `BufferGet`, reads and writes its `data`, and
returns it with `BufferRelease`. Between those two calls the buffer is **held**,
and a held buffer is never taken away.

## 3. Finding, and choosing what to discard

Sixty-four buffers of 512 bytes are allocated from the kernel heap in one
request. They are found through a hash of 128 buckets — a power of two, so the
bucket is a mask rather than a division, and larger than the number of buffers so
that the chains stay short even when every buffer holds something.

Recency is a doubly linked list through the same entries, most recently used at
one end. `BufferGet` moves the entry it returns to that end; `BufferClaim` takes
from the other. Two properties of it are worth stating.

**Every entry is in the list from the outset**, the unused ones at the least
recently used end. `BufferClaim` therefore has one rule — take the oldest — and
not two, and there is no separate free list to keep consistent with the rest.

**A held buffer is passed over, not evicted.** `BufferClaim` walks towards the
newest end until it finds an entry nobody is holding. If there is none, the
request is refused and `BufferGet` returns null. Refusing is the only safe answer:
the alternative is to hand the same storage to two callers, one of whom believes
it holds a block that has been replaced beneath it, and that defect appears as
corruption somewhere else entirely and much later.

## 4. Write-back, not write-through

A modified buffer is marked dirty and the device is not touched. The block is
written back when the buffer is evicted, when `BufferFlush` is called upon it,
when `BufferSync` is called, or when its device is invalidated — and not before.

The alternative, writing through to the device upon every modification, is
simpler and would make the cache pointless for writing: a filesystem that updates
an inode three times would perform three sector writes upon a path where a sector
write is the most expensive thing the kernel does.

The obligation that follows is absolute: **an eviction must write back before it
reuses the storage.** A cache that dropped a dirty block would lose a write that
had already been reported as successful, and the loss would be discovered only
when something read the block back and found the old contents. `BufferClaim`
therefore writes back first and abandons the claim if the write fails, rather
than proceeding and losing the data.

## 5. Invalidation, and the order of withdrawal

`BufferInvalidateDevice` writes back everything of a device and then discards it
from the cache. It exists because a device may be withdrawn from the block layer,
and a buffer written back after that would be written to whatever was registered
in its place. The order is therefore fixed, and is stated in both headers:

1. `BufferInvalidateDevice`, which flushes and discards.
2. `BlockUnregister`, which frees the slot.

Invalidation refuses to discard anything while any buffer of the device is held.
A caller holding one is about to write into storage the cache no longer
associates with any block; the check is cheap where the consequence is not.

## 6. Verification

Every failure of a cache is silent by construction — that is what a cache is for.
A lookup that matched the wrong device returns a block. An eviction that dropped a
dirty buffer reports success. A buffer handed to two callers corrupts whichever
writes second, at a place unrelated to the defect. `KernelVerifyBuffer` is written
so that each of those produces a failure at the point of the defect instead.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A block not held is read from the device, and its contents are the device's. | A lookup that returned the wrong entry, or a read that never happened. |
| The same block, asked for again, is the same buffer, is counted a hit, and does not touch the device. | A cache that caches nothing — which is otherwise indistinguishable from one that works. |
| Two different blocks occupy two different buffers with their own contents. | A hash or comparison that conflates them. |
| A modified block does **not** reach the device until it is written back; the device still holds the old contents. | Write-through masquerading as write-back, which would hide the eviction bug below. |
| Synchronising writes it back, after which the device holds the new contents and nothing is dirty. | A dirty mark that is never acted upon: data that would be lost at the next eviction. |
| A dirty block evicted under pressure is written back as it goes. | The failure that loses data outright, reported nowhere. |
| An evicted block is fetched from the device again. | An eviction that unlinked nothing, leaving a stale entry to be found later. |
| A buffer held by a caller survives 64 subsequent misses, and is still their buffer with their contents. | Storage handed out twice. |
| With every buffer held, a further request is refused. | The same, under pressure. |
| A device cannot be invalidated while its buffers are held. | Discarding the identity of storage a caller is still writing into. |
| Invalidation writes back what is dirty, discards the rest, and a later request is a miss. | A withdrawn device's blocks surviving to be written back to its successor. |
| Nothing beneath the cache failed. | The cache asking the layer for a block it should not have — an off-by-one in the pressure loops that would otherwise pass unnoticed. |

The assertions are made against the device of memory described in
[`BLOCK.md`](BLOCK.md), Section 5, for the same two reasons: the machine
`make verify` runs upon has no disk, and a machine that has one holds data a
self-test must not write to.

Observed:

```
Buffer self-test passed.
Buffer cache: 64 buffers of 512 bytes (32 KiB), 0 holding a block, 0 dirty, 0 held.
Buffer cache: hits 5, misses 197, evictions 131, write-backs 3, failures 0.
```

The cache is left empty because the self-test invalidates its device before
withdrawing it, which is the order Section 5 requires of everybody.

### 6.1 A defect this found

The first implementation linked every entry into the recency list by calling
`BufferTouch` upon it, and `BufferTouch` begins by detaching. Detaching an entry
that is not in the list wrote the neighbours of an entry that has none — nothing
— over both ends of the list, so each call emptied it and only the last entry
survived. The cache still worked in the small: the self-test's first few
assertions passed. It failed at the assertion that every buffer may be held at
once, because sixty-three of the sixty-four were unreachable.

The corrective is one flag, `listed`, and the detachment now leaves an entry that
is not in the list alone. The point worth recording is that a cache with 63 of its
64 buffers lost would have shown no symptom whatever in ordinary use beyond being
slow.

## 7. Limitations

1. **One block size.** Buffers are 512 bytes and a device of any other block size
   is not cached; it may still be addressed through the block layer directly.
2. **A fixed store.** Sixty-four buffers, allocated once. It does not grow under
   pressure, nor return memory when idle.
3. **Strict least-recently-used order.** No distinction is drawn between a block
   read once in passing and one read constantly, so a single sweep of a large
   file empties the cache of everything worth keeping.
4. **No read-ahead.** A sequential reader pays a miss for every block.
5. **No delayed write policy.** A dirty block waits for an eviction, a flush or a
   synchronisation; nothing writes it back in the background, there being no
   background to write it in until Phase 6.
6. **No concurrency safety.** Reference counts are not atomic and nothing is
   locked. There is one thread of control, and when there is not, this is the
   first thing that must change.
