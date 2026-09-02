# `docs/storage/` — From a Medium to a Caller

Four documents describing one stack, bottom upwards. They are grouped apart from
[`../devices/`](../devices/) because each exists to serve the one above it, and
because the whole stack is what the filesystem of Phase 5 will be written against.

| Document | Subject | Implementation | Phase |
| -------- | ------- | -------------- | ----- |
| [`DISK.md`](DISK.md) | The ATA disk in programmed input/output mode: the task file, the two addressing modes, the identification of what answered, and the cache flush that makes a write durable. | [`../../drivers/ata/ata.c`](../../drivers/ata/ata.c) | 4.4 |
| [`BLOCK.md`](BLOCK.md) | The generic block-device layer: what a driver supplies to register a device, and what the layer refuses before any driver is reached. | [`../../drivers/block/block.c`](../../drivers/block/block.c) | 4.5 |
| [`BUFFER.md`](BUFFER.md) | The buffer cache: how a block is found, what is discarded when the store is full, and when a modified block reaches its device. | [`../../drivers/block/buffer.c`](../../drivers/block/buffer.c) | 4.6 |
| [`EXT2.md`](EXT2.md) | The EXT2 volume: the superblock, the block group descriptor table, the inode with the direct and indirect pointers that name a file's blocks, and the directory entries that turn a name into an inode number, and the reading of a file's contents. | [`../../kernel/fs/ext2.c`](../../kernel/fs/ext2.c) | 5.1 to 5.5 |

## The two rules that run through all three

**A failure here is silent.** A disk driver that reads the wrong sector returns
data, and data that arrived cannot be distinguished from data that is correct
until something interprets it. Every self-test in this stack asserts a
*relationship* — that the second block of a two-block read is the block that
follows, that a block already held is not read again, that a dirty buffer
evicted under pressure reached its device — rather than merely that an operation
succeeded.

**This is the part of the kernel that can destroy something.** The disk
self-test reads unconditionally and writes only when the operator has asked for
it at the GRUB menu, restoring what it wrote; the block and buffer layers are
asserted against a device made of memory, so that the verification needs no disk
and touches nobody's data. See [`DISK.md`](DISK.md), Section 6.
