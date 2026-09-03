/*
 * File: kernel/test/verify_storage.c
 * Purpose: Asserts the storage stack of Phase 4: the ATA driver, the generic
 *          block-device layer above it, and the buffer cache above that.
 * Key functions: KernelVerifyAta, KernelVerifyBlock, KernelVerifyBuffer.
 * References:
   - docs/storage/DISK.md, docs/storage/BLOCK.md and docs/storage/BUFFER.md:
 *     each has a verification section pairing the assertions below with the
 *     silent failure each would catch.
 *
 * The block and buffer assertions are made against the memory-backed
 * devices of <oxys/testvolume.h>, so they hold upon a machine with no disk. The
 * ATA assertions cannot be, a driver for a device being untestable without one;
 * where no device answers, that is reported and nothing is asserted. The write
 * path is exercised only when the boot loader\'s command line asks for it.
 */

#include <oxys/kernel.h>
#include <oxys/verify.h>
#include <oxys/testvolume.h>
#include <oxys/ata.h>
#include <oxys/block.h>
#include <oxys/buffer.h>
#include <oxys/pci.h>

/*
 * The buffers the disk self-test reads into. They are of static storage duration
 * because the boot stack is 64 KiB and three sectors of it would be a
 * disproportionate share of what remains after the self-tests above.
 */
static uint8_t KernelDiskBufferA[ATA_SECTOR_SIZE * 2U];
static uint8_t KernelDiskBufferB[ATA_SECTOR_SIZE * 2U];

/* True if two regions hold the same bytes. */
static bool KernelRegionsMatch(const uint8_t *left, const uint8_t *right, size_t length)
{
    for (size_t index = 0U; index < length; ++index)
    {
        if (left[index] != right[index])
        {
            return false;
        }
    }

    return true;
}

/*
 * Asserts that the disk driver addresses the sector it was asked for and
 * transfers exactly its contents.
 *
 * The failure this guards against is the worst kind the kernel has yet had to
 * consider: a driver that reads the wrong sector returns data, and data that
 * arrived is indistinguishable from data that is correct until something tries
 * to interpret it. An address composed with a byte in the wrong register, a
 * transfer of 255 words instead of 256, a second sector written over the first —
 * each of these produces a disk that appears to work and a filesystem that
 * decays. Every assertion below is chosen to make one of those visible.
 *
 * The test reads. It writes only when the operator has asked for it upon the
 * command line, and then only to a sector whose previous contents it has read
 * and restores afterwards: a self-test that wrote to a disk unbidden would
 * destroy the data of anybody who booted this kernel upon their own machine.
 */
void KernelVerifyAta(void)
{
    const AtaDevice *const disk = AtaFirstDisk();
    bool succeeded = true;

    if (AtaDeviceCount() == 0U)
    {
        KernelWriteString("Disk self-test: no device answered; nothing to assert.\n");
        return;
    }

    if (disk == NULL)
    {
        KernelWriteString("Disk self-test: devices answered but none is a disk.\n");
        return;
    }

    /* An identification that yielded no capacity was not understood. */
    if ((disk->sector_count == 0U) || (disk->model[0] == '\0'))
    {
        KernelWriteString("  The identification data yielded no capacity or model.\n");
        succeeded = false;
    }

    /* The first sector must be readable, and must read the same way twice. */
    if (!AtaRead(disk, 0U, 1U, KernelDiskBufferA))
    {
        KernelWriteString("  The first sector could not be read: ");
        KernelWriteString(AtaLastError());
        KernelWriteString("\n");
        succeeded = false;
    }
    else if (!AtaRead(disk, 0U, 1U, KernelDiskBufferB) ||
             !KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, ATA_SECTOR_SIZE))
    {
        KernelWriteString("  The same sector read differently upon a second attempt.\n");
        succeeded = false;
    }

    /*
     * A two-sector read must place the second sector after the first, and the
     * first must be what a one-sector read of the same address returned. A
     * driver that lost synchronisation between sectors, or that overwrote the
     * first with the second, passes every other assertion here.
     */
    if (disk->sector_count >= 2U)
    {
        if (!AtaRead(disk, 0U, 2U, KernelDiskBufferB))
        {
            KernelWriteString("  A two-sector read failed.\n");
            succeeded = false;
        }
        else
        {
            if (!KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, ATA_SECTOR_SIZE))
            {
                KernelWriteString("  A two-sector read did not begin where a one-sector "
                                  "read did.\n");
                succeeded = false;
            }

            if (!AtaRead(disk, 1U, 1U, KernelDiskBufferA) ||
                !KernelRegionsMatch(KernelDiskBufferA, &KernelDiskBufferB[ATA_SECTOR_SIZE],
                                    ATA_SECTOR_SIZE))
            {
                KernelWriteString("  The second sector of a two-sector read is not the "
                                  "sector that follows.\n");
                succeeded = false;
            }
        }
    }

    /* A range beyond the capacity is refused rather than attempted. */
    if (AtaRead(disk, disk->sector_count, 1U, KernelDiskBufferA) ||
        AtaRead(disk, disk->sector_count - 1U, 2U, KernelDiskBufferA))
    {
        KernelWriteString("  A read beyond the capacity of the device was accepted.\n");
        succeeded = false;
    }

    /* A request without a buffer, and one for no sectors, are both harmless. */
    if (AtaRead(disk, 0U, 1U, NULL) || !AtaRead(disk, 0U, 0U, KernelDiskBufferA))
    {
        KernelWriteString("  A degenerate request was mishandled.\n");
        succeeded = false;
    }

    /*
     * A device larger than 28 bits can name exercises the 48-bit commands, which
     * are otherwise never reached. The register writing they require is
     * different in kind and not merely in width — each register is written
     * twice, high-order byte first — so a driver that has never issued one has
     * not been tested at all in that mode.
     */
    if (disk->supports_lba48 && (disk->sector_count > ATA_LBA28_LIMIT))
    {
        if (!AtaRead(disk, ATA_LBA28_LIMIT + 1U, 1U, KernelDiskBufferA))
        {
            KernelWriteString("  A sector beyond the 28-bit limit could not be read: ");
            KernelWriteString(AtaLastError());
            KernelWriteString("\n");
            succeeded = false;
        }
    }

    /*
     * The write path, only upon request. The sector is read, overwritten with a
     * pattern, read back, compared, and then restored from what was read; the
     * restoration is verified in its turn, since a test that damaged the disk
     * and reported success would be worse than no test.
     */
    if (KernelCommandLineHasOption("disk-write-test"))
    {
        const uint64_t target = disk->sector_count - 1U;

        KernelWriteString("  Writing to the final sector, as the command line permits.\n");

        if (!AtaRead(disk, target, 1U, KernelDiskBufferA))
        {
            KernelWriteString("  The sector to be written could not first be read.\n");
            succeeded = false;
        }
        else
        {
            for (size_t index = 0U; index < ATA_SECTOR_SIZE; ++index)
            {
                KernelDiskBufferB[index] = (uint8_t)(index ^ 0xA5U);
            }

            if (!AtaWrite(disk, target, 1U, KernelDiskBufferB))
            {
                KernelWriteString("  The pattern could not be written: ");
                KernelWriteString(AtaLastError());
                KernelWriteString("\n");
                succeeded = false;
            }
            else if (!AtaRead(disk, target, 1U, &KernelDiskBufferB[ATA_SECTOR_SIZE]))
            {
                KernelWriteString("  The pattern could not be read back.\n");
                succeeded = false;
            }
            else
            {
                for (size_t index = 0U; index < ATA_SECTOR_SIZE; ++index)
                {
                    if (KernelDiskBufferB[ATA_SECTOR_SIZE + index] != (uint8_t)(index ^ 0xA5U))
                    {
                        KernelWriteString("  The pattern read back altered.\n");
                        succeeded = false;
                        break;
                    }
                }
            }

            /* Whatever happened above, the sector is put back as it was found. */
            if (!AtaWrite(disk, target, 1U, KernelDiskBufferA) ||
                !AtaRead(disk, target, 1U, KernelDiskBufferB) ||
                !KernelRegionsMatch(KernelDiskBufferA, KernelDiskBufferB, ATA_SECTOR_SIZE))
            {
                KernelWriteString("  The sector was not restored to its previous contents.\n");
                succeeded = false;
            }
        }
    }

    if (AtaTimeoutCount() != 0U)
    {
        KernelWriteString("  A device failed to respond within the driver's patience.\n");
        succeeded = false;
    }

    /*
     * Every refusal above was provoked deliberately; an error is a failure of the
     * hardware and none was expected.
     */
    if (AtaErrorCount() != 0U)
    {
        KernelWriteString("  A device reported an error: ");
        KernelWriteString(AtaLastError());
        KernelWriteString("\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "Disk self-test passed.\n" : "Disk self-test FAILED.\n");
}

/* Two blocks of working space for the transfers the self-tests perform. */
static uint8_t KernelBlockBufferA[BLOCK_SIZE_DEFAULT * 2U];
static uint8_t KernelBlockBufferB[BLOCK_SIZE_DEFAULT * 2U];

/* Fills a region with a pattern that depends upon the seed, so that two regions
 * filled from different seeds cannot be confused for one another. */
static void KernelFillPattern(uint8_t *region, size_t length, uint8_t seed)
{
    for (size_t index = 0U; index < length; ++index)
    {
        region[index] = (uint8_t)((index * 31U) + seed);
    }
}

/* True if a region holds the pattern that seed would have produced. */
static bool KernelPatternMatches(const uint8_t *region, size_t length, uint8_t seed)
{
    for (size_t index = 0U; index < length; ++index)
    {
        if (region[index] != (uint8_t)((index * 31U) + seed))
        {
            return false;
        }
    }

    return true;
}

/*
 * Asserts that the block layer validates what it is asked before it reaches a
 * driver, and transfers what it was given when it does.
 *
 * The layer exists precisely so that the four tests every driver would otherwise
 * repeat are written once, and the consequence of that is that a defect here is
 * a defect in every device at once. The assertions are made against a device of
 * known contents rather than against a disk, for the reason given where that
 * device is defined.
 */
void KernelVerifyBlock(void)
{
    BlockDevice *device;
    BlockDevice *read_only;
    const size_t already_registered = BlockDeviceCount();
    bool succeeded = true;

    device = BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelWriteString("  A device of memory could not be registered.\n");
        KernelWriteString("Block self-test FAILED.\n");
        return;
    }

    read_only = BlockRegister("mem1", &KernelMemoryDeviceReadOnlyOperations, NULL,
                              BLOCK_SIZE_DEFAULT, KERNEL_MEMORY_DEVICE_BLOCKS, true);

    if (read_only == NULL)
    {
        KernelWriteString("  A read-only device could not be registered.\n");
        succeeded = false;
    }

    /* A name identifies a device, so a second device may not take one in use. */
    if (BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 1U,
                      false) != NULL)
    {
        KernelWriteString("  A name already registered was accepted a second time.\n");
        succeeded = false;
    }

    /*
     * A writable device without a writer, and a read-only device with one, are
     * both refused: either would be a device whose declared nature and whose
     * behaviour disagree.
     */
    if ((BlockRegister("mem2", &KernelMemoryDeviceReadOnlyOperations, NULL, BLOCK_SIZE_DEFAULT,
                       1U, false) != NULL) ||
        (BlockRegister("mem3", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 1U,
                       true) != NULL))
    {
        KernelWriteString("  A device was registered whose nature and operations disagree.\n");
        succeeded = false;
    }

    /* A degenerate geometry, and a name that cannot be held, are refused. */
    if ((BlockRegister("mem4", &KernelMemoryDeviceOperations, NULL, 0U, 1U, false) != NULL) ||
        (BlockRegister("mem5", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT, 0U,
                       false) != NULL) ||
        (BlockRegister("a-name-far-too-long-to-hold", &KernelMemoryDeviceOperations, NULL,
                       BLOCK_SIZE_DEFAULT, 1U, false) != NULL))
    {
        KernelWriteString("  A degenerate registration was accepted.\n");
        succeeded = false;
    }

    if ((BlockFindByName("mem0") != device) || (BlockFindByName("mem") != NULL) ||
        (BlockFindByName("mem00") != NULL))
    {
        KernelWriteString("  A device was found by a name that is not its own.\n");
        succeeded = false;
    }

    if (BlockDeviceCount() != (already_registered + 2U))
    {
        KernelWriteString("  The registry holds a different number of devices than "
                          "were registered.\n");
        succeeded = false;
    }

    if (BlockDeviceAt(BlockDeviceCount()) != NULL)
    {
        KernelWriteString("  A device was reported beyond the end of the registry.\n");
        succeeded = false;
    }

    /* What is written to a block must be what is read back from it. */
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x11U);

    if (!BlockWrite(device, 3U, 1U, KernelBlockBufferA) ||
        !BlockRead(device, 3U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x11U))
    {
        KernelWriteString("  A block did not read back as it was written.\n");
        succeeded = false;
    }

    /*
     * A two-block transfer must carry both blocks and must not carry a third.
     * The two halves are given different patterns so that a layer which passed
     * the same block twice, or which lost the count, cannot pass this.
     */
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x22U);
    KernelFillPattern(&KernelBlockBufferA[BLOCK_SIZE_DEFAULT], BLOCK_SIZE_DEFAULT, 0x33U);

    if (!BlockWrite(device, 8U, 2U, KernelBlockBufferA) ||
        !BlockRead(device, 8U, 2U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x22U) ||
        !KernelPatternMatches(&KernelBlockBufferB[BLOCK_SIZE_DEFAULT], BLOCK_SIZE_DEFAULT,
                              0x33U))
    {
        KernelWriteString("  A two-block transfer did not carry both blocks in order.\n");
        succeeded = false;
    }

    if (!BlockRead(device, 9U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x33U))
    {
        KernelWriteString("  The second block of a two-block write is not the block "
                          "that follows.\n");
        succeeded = false;
    }

    /* A range outside the device is refused, and so is one that would wrap. */
    if (BlockRead(device, KERNEL_MEMORY_DEVICE_BLOCKS, 1U, KernelBlockBufferB) ||
        BlockRead(device, KERNEL_MEMORY_DEVICE_BLOCKS - 1U, 2U, KernelBlockBufferB) ||
        BlockRead(device, UINT64_MAX, 2U, KernelBlockBufferB))
    {
        KernelWriteString("  A range outside the device was accepted.\n");
        succeeded = false;
    }

    /* A request without a buffer is refused; one for no blocks is harmless. */
    if (BlockRead(device, 0U, 1U, NULL) || BlockWrite(device, 0U, 1U, NULL) ||
        !BlockRead(device, 0U, 0U, NULL))
    {
        KernelWriteString("  A degenerate request was mishandled.\n");
        succeeded = false;
    }

    /* A read-only device refuses a write before the driver is reached. */
    if (BlockWrite(read_only, 0U, 1U, KernelBlockBufferA))
    {
        KernelWriteString("  A read-only device accepted a write.\n");
        succeeded = false;
    }

    if (!BlockRead(read_only, 3U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x11U))
    {
        KernelWriteString("  A read-only device did not read.\n");
        succeeded = false;
    }

    /* The accounting must reflect the blocks that actually moved. */
    if ((device->blocks_written != 3U) || (device->blocks_read != 4U))
    {
        KernelWriteString("  The accounting does not match the transfers performed.\n");
        succeeded = false;
    }

    /* A device may be withdrawn, and is then neither found nor addressable. */
    if (!BlockUnregister(read_only) || !BlockUnregister(device))
    {
        KernelWriteString("  A registered device could not be withdrawn.\n");
        succeeded = false;
    }

    if (BlockUnregister(device) || (BlockFindByName("mem0") != NULL) ||
        BlockRead(device, 0U, 1U, KernelBlockBufferB) ||
        (BlockDeviceCount() != already_registered))
    {
        KernelWriteString("  A withdrawn device was still reachable.\n");
        succeeded = false;
    }

    if (BlockTotalErrors() != 0U)
    {
        KernelWriteString("  A device reported an error where none was expected.\n");
        succeeded = false;
    }

    KernelWriteString(succeeded ? "Block self-test passed.\n" : "Block self-test FAILED.\n");
}

/* The buffers held at once by the assertion that every buffer may be held. */
static Buffer *KernelHeldBuffers[BUFFER_CAPACITY];

/*
 * Asserts that the cache returns the block that was asked for, that a block held
 * is not read again, that a modified block reaches its device, and that a buffer
 * somebody is using is never taken from them.
 *
 * A cache is a thing that lies about where data came from, and every one of its
 * failures is silent by construction. A lookup that matched the wrong device
 * returns a block; an eviction that discarded a dirty buffer reports success and
 * loses a write; a buffer handed to two callers at once corrupts whichever of
 * them writes second, at a place unrelated to the defect. The assertions below
 * are chosen so that each of those produces a failure here instead.
 *
 * They are made against the device of memory, for the reasons given where it is
 * defined: this must be assertable upon a machine with no disk, and must not
 * write to a machine that has one.
 */
void KernelVerifyBuffer(void)
{
    BlockDevice *device;
    Buffer *first;
    Buffer *second;
    uint64_t reads;
    uint64_t writes;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    bool succeeded = true;

    if (BufferCount() == 0U)
    {
        KernelWriteString("  The cache has no buffers; its storage was not allocated.\n");
        KernelWriteString("Buffer self-test FAILED.\n");
        return;
    }

    device = BlockRegister("mem0", &KernelMemoryDeviceOperations, NULL, BLOCK_SIZE_DEFAULT,
                           KERNEL_MEMORY_DEVICE_BLOCKS, false);

    if (device == NULL)
    {
        KernelWriteString("  A device of memory could not be registered.\n");
        KernelWriteString("Buffer self-test FAILED.\n");
        return;
    }

    /* Blocks 5, 6 and 7 are given contents the assertions below can name. */
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x55U);
    (void)BlockWrite(device, 5U, 1U, KernelBlockBufferA);
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x66U);
    (void)BlockWrite(device, 6U, 1U, KernelBlockBufferA);
    KernelFillPattern(KernelBlockBufferA, BLOCK_SIZE_DEFAULT, 0x77U);
    (void)BlockWrite(device, 7U, 1U, KernelBlockBufferA);

    /* A block not held is read from the device, and read correctly. */
    reads = device->blocks_read;
    first = BufferGet(device, 5U);

    if ((first == NULL) || (device->blocks_read != (reads + 1U)) ||
        !KernelPatternMatches(first->data, BLOCK_SIZE_DEFAULT, 0x55U) ||
        (first->block != 5U) || (first->device != device))
    {
        KernelWriteString("  A block was not fetched from the device correctly.\n");
        KernelWriteString("Buffer self-test FAILED.\n");
        (void)BufferInvalidateDevice(device);
        (void)BlockUnregister(device);
        return;
    }

    BufferRelease(first);

    /* The same block is then found in the cache, and the device is not touched. */
    hits = BufferHits();
    reads = device->blocks_read;
    second = BufferGet(device, 5U);

    if ((second != first) || (BufferHits() != (hits + 1U)) || (device->blocks_read != reads))
    {
        KernelWriteString("  A block already held was fetched from the device again.\n");
        succeeded = false;
    }

    BufferRelease(second);

    /* Two different blocks occupy two different buffers. */
    first = BufferGet(device, 6U);
    second = BufferGet(device, 7U);

    if ((first == NULL) || (second == NULL) || (first == second) ||
        !KernelPatternMatches(first->data, BLOCK_SIZE_DEFAULT, 0x66U) ||
        !KernelPatternMatches(second->data, BLOCK_SIZE_DEFAULT, 0x77U))
    {
        KernelWriteString("  Two blocks were confused for one another.\n");
        succeeded = false;
    }

    BufferRelease(second);

    /*
     * A modified block does not reach the device until it is written back. That
     * deferral is the whole difference between this cache and none at all, so it
     * is asserted directly: the device must still hold the old contents.
     */
    if (first != NULL)
    {
        KernelFillPattern(first->data, BLOCK_SIZE_DEFAULT, 0x88U);
        BufferMarkDirty(first);
        BufferRelease(first);
    }

    writes = device->blocks_written;

    if ((device->blocks_written != writes) ||
        !BlockRead(device, 6U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x66U))
    {
        KernelWriteString("  A modified block reached the device before it was written "
                          "back.\n");
        succeeded = false;
    }

    if (BufferDirtyCount() == 0U)
    {
        KernelWriteString("  A modified block was not recorded as dirty.\n");
        succeeded = false;
    }

    /* Synchronising writes it back, and the device then holds the new contents. */
    if (!BufferSync() || (device->blocks_written != (writes + 1U)) ||
        !BlockRead(device, 6U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x88U) ||
        (BufferDirtyCount() != 0U))
    {
        KernelWriteString("  A modified block did not reach the device upon "
                          "synchronisation.\n");
        succeeded = false;
    }

    /*
     * A dirty block evicted under pressure must be written back as it goes. The
     * failure this catches is the one that loses data: an eviction that dropped
     * the contents would report nothing and be discovered only by a later read.
     */
    first = BufferGet(device, 7U);

    if (first != NULL)
    {
        KernelFillPattern(first->data, BLOCK_SIZE_DEFAULT, 0x99U);
        BufferMarkDirty(first);
        BufferRelease(first);
    }

    evictions = BufferEvictions();

    for (uint64_t block = 16U; block < (16U + (uint64_t)BUFFER_CAPACITY); ++block)
    {
        Buffer *const transient = BufferGet(device, block);

        BufferRelease(transient);
    }

    if ((BufferEvictions() <= evictions) || !BlockRead(device, 7U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0x99U))
    {
        KernelWriteString("  A dirty block was evicted without being written back.\n");
        succeeded = false;
    }

    /* Having been evicted, the block is fetched from the device once more. */
    misses = BufferMisses();
    reads = device->blocks_read;
    first = BufferGet(device, 5U);

    if ((first == NULL) || (BufferMisses() != (misses + 1U)) ||
        (device->blocks_read != (reads + 1U)))
    {
        KernelWriteString("  An evicted block was reported as still held.\n");
        succeeded = false;
    }

    /*
     * A buffer a caller is holding is passed over by the eviction, however long
     * it has been there. The reference above is deliberately not released.
     */
    for (uint64_t block = 128U; block < (128U + (uint64_t)BUFFER_CAPACITY); ++block)
    {
        Buffer *const transient = BufferGet(device, block);

        BufferRelease(transient);
    }

    hits = BufferHits();
    second = BufferGet(device, 5U);

    if ((second != first) || (BufferHits() != (hits + 1U)) ||
        !KernelPatternMatches(first->data, BLOCK_SIZE_DEFAULT, 0x55U))
    {
        KernelWriteString("  A buffer being held by a caller was evicted beneath them.\n");
        succeeded = false;
    }

    BufferRelease(second);
    BufferRelease(first);

    /*
     * With every buffer held, a further request is refused rather than served by
     * evicting one of them. Handing out storage twice is the failure this
     * prevents, and it would appear as corruption somewhere else entirely.
     */
    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        KernelHeldBuffers[index] = BufferGet(device, (uint64_t)index);

        if (KernelHeldBuffers[index] == NULL)
        {
            KernelWriteString("  A buffer could not be held while others were.\n");
            succeeded = false;
            break;
        }
    }

    if (BufferHeldCount() != BUFFER_CAPACITY)
    {
        KernelWriteString("  The cache does not agree upon how many buffers are held.\n");
        succeeded = false;
    }

    if (BufferGet(device, (uint64_t)BUFFER_CAPACITY + 1U) != NULL)
    {
        KernelWriteString("  A buffer was issued when every one of them was held.\n");
        succeeded = false;
    }

    /* Nor may a device be discarded while its buffers are held. */
    if (BufferInvalidateDevice(device))
    {
        KernelWriteString("  A device was invalidated while its buffers were held.\n");
        succeeded = false;
    }

    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        BufferRelease(KernelHeldBuffers[index]);
        KernelHeldBuffers[index] = NULL;
    }

    if (BufferHeldCount() != 0U)
    {
        KernelWriteString("  A buffer remained held after being released.\n");
        succeeded = false;
    }

    /*
     * Invalidation writes back what is dirty and then discards everything of the
     * device, which is what makes it safe to withdraw the device afterwards.
     */
    first = BufferGet(device, 4U);

    if (first != NULL)
    {
        KernelFillPattern(first->data, BLOCK_SIZE_DEFAULT, 0xAAU);
        BufferMarkDirty(first);
        BufferRelease(first);
    }

    if (!BufferInvalidateDevice(device) || (BufferValidCount() != 0U) ||
        !BlockRead(device, 4U, 1U, KernelBlockBufferB) ||
        !KernelPatternMatches(KernelBlockBufferB, BLOCK_SIZE_DEFAULT, 0xAAU))
    {
        KernelWriteString("  Invalidation did not write back and discard the device.\n");
        succeeded = false;
    }

    misses = BufferMisses();
    first = BufferGet(device, 5U);

    if ((first == NULL) || (BufferMisses() != (misses + 1U)))
    {
        KernelWriteString("  A block survived the invalidation of its device.\n");
        succeeded = false;
    }

    BufferRelease(first);

    /*
     * Nothing beneath the cache failed. Every transfer the test performed was of
     * a block the device holds, so a failure here means the cache asked for one
     * it should not have.
     */
    if (BufferFailures() != 0U)
    {
        KernelWriteString("  A transfer beneath the cache failed.\n");
        succeeded = false;
    }

    /* The device is discarded and withdrawn in that order, as it must be. */
    (void)BufferInvalidateDevice(device);
    (void)BlockUnregister(device);

    KernelWriteString(succeeded ? "Buffer self-test passed.\n" : "Buffer self-test FAILED.\n");
}
