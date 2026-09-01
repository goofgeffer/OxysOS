# The Block Layer

**Phase**: 4, sub-task 4.5, of [`PLAN.md`](PLAN.md).

**Authority**: `PROJECT_GUIDELINES.md`, Sections 2, 3 and 6.

**Implementation**: [`../drivers/block/block.c`](../drivers/block/block.c),
[`../kernel/include/oxys/block.h`](../kernel/include/oxys/block.h).

## 1. What the layer is for

The disk driver of sub-task 4.4 is the only way this kernel can reach a medium,
and everything that will want a medium — the buffer cache of sub-task 4.6, the
filesystem of Phase 5, the loader of Phase 7 — would otherwise be written against
it. Each of them would then have to be rewritten for the second kind of disk, and
each would have to repeat the same four tests upon every request.

The layer is the answer to both. A driver registers a device by supplying two
operations and a geometry; a caller addresses a device by a name and a block
number and knows nothing else about it. What separates them is not a convenience:
it is the place where a request is judged before any hardware is touched.

## 2. What a device is

A registered device holds a name, the two operations, a context that means
something to the driver and nothing to the layer, a block size, a block count, a
read-only flag, and its own accounting.

The name is how everything above identifies the device, and it is copied rather
than retained by reference — a driver's name might be composed on its stack. It
must be unique among the registered devices: a name that identified two devices
would send a caller to whichever was found first.

The context is the whole of the coupling. The ATA driver registers each disk with
its own `AtaDevice` as the context, and its two operations cast it back; the layer
carries the pointer and never looks inside it. That is why the adaptor lives in
`drivers/ata/ata.c` and not here — a driver knows the layer it presents itself
through, and the layer knows nothing of ATA.

## 3. What the layer refuses

Every request is judged before a driver is reached. A driver is never called with
a null buffer, a count of zero, a range outside the device, or a write to a
read-only device; those four tests are written here once rather than in each
driver, which is most of the reason the layer exists.

Two of the refusals are worth stating in full.

**A range is bounded by subtraction, not by addition.** The obvious test —
`block + count > block_count` — is wrong. The block number is 64 bits wide, and a
caller presenting one near its greatest value makes the sum wrap, so a range
wholly outside the device passes the test. The layer asks instead whether `count`
exceeds `block_count - block`, having first established that `block` is within
the device. The self-test asserts the wrapping case specifically, because it is
the one an ordinary test would never produce.

**Registration is refused where a device's declared nature and its operations
disagree.** A writable device must supply a write operation and a read-only one
must not. Without that rule a device could be registered read-only *and* with a
writer the layer would never call, or writable and without one, and the failure
would appear only when something eventually tried to write.

A refused request is counted apart from a failure of the hardware, for the reason
given in [`DISK.md`](DISK.md), Section 7: a refusal is the layer working, and a
figure that added the two together would show a healthy machine accumulating
errors until an operator learned to ignore the number.

## 4. Withdrawal

`BlockUnregister` frees a slot. The caller must first have flushed and discarded
anything held elsewhere upon the device: this layer does not know what caches
stand above it, and a buffer written back to a withdrawn device would be written
to whatever was registered in its place. The buffer cache of sub-task 4.6
therefore exposes `BufferInvalidateDevice`, and the order — flush, invalidate,
withdraw — is the caller's responsibility and is stated in both headers.

## 5. Verification

The disk is not a fit subject for testing this layer. The machine `make verify`
runs upon presents no ATA device at all, and a machine that does has data upon it
that a self-test must not write to. Both objections vanish if the device under
test is made of memory.

`KernelVerifyBlock` registers such a device — 32 blocks of 512 bytes over a static
array — asserts the layer against it, and withdraws it again. The device is
therefore present only during the test, and the report that follows shows only
the real disks.

| Property asserted | The silent failure it would catch |
| ----------------- | --------------------------------- |
| A name already registered is refused a second time. | Two devices answering to one name, so that a caller reaches whichever was found first. |
| A writable device without a writer, and a read-only device with one, are both refused. | A device whose declared nature and behaviour disagree, discovered only at the first write. |
| A zero block size, a zero block count, and a name too long to hold are refused. | A geometry that makes every subsequent bound calculation meaningless; a name silently truncated into another device's. |
| A device is found by its own name and not by a prefix or an extension of it. | A comparison that stops at the shorter of the two strings. |
| What is written to a block reads back from that block. | The context or the block number mistranslated on the way to the driver. |
| A two-block transfer carries both blocks in order, and the second is what a read of the following block returns. | A count dropped, or the same block passed twice. |
| A range beyond the device is refused, including one whose block number and count would wrap. | The addition bug of Section 3, which accepts a range wholly outside the device. |
| A request without a buffer is refused; one for no blocks succeeds and does nothing. | A null dereference in the path a filesystem will one day take. |
| A read-only device refuses a write but still reads. | A read-only flag that is decorative. |
| The accounting matches the blocks that actually moved. | Statistics counted per request rather than per block, which would misreport every multi-block transfer. |
| A withdrawn device is neither found, nor addressable, nor withdrawn twice. | A stale pointer that still reaches a driver. |

Observed upon a machine with a disk:

```
Block self-test passed.
Block layer: 1 devices registered of 8.
  ata0: 536870912 blocks of 512 bytes (268435456 KiB), writable, blocks read 0, written 0
Block layer: reads 4, writes 2, device errors 0, requests refused 14.
```

The fourteen refusals are the ones the self-test provoked deliberately.

## 6. Limitations

1. **A fixed registry.** Eight devices, and no more.
2. **One block size in practice.** The layer carries the block size of each
   device, but the buffer cache above it holds blocks of one size; a device of
   another size may be registered and addressed directly, not cached.
3. **No partitions.** A device is the whole medium. Partition tables belong with
   the filesystem work of Phase 5.
4. **No ordering or barriers.** Requests are issued as they arrive, and a driver
   that reordered them would not be contradicted here.
5. **No concurrency safety.** There is no lock, and there is no second thread of
   control to need one yet.
6. **Synchronous only.** A request returns when it has completed. Asynchronous
   submission needs a scheduler to return to.
