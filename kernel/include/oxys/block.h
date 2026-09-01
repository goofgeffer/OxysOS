/*
 * File: kernel/include/oxys/block.h
 * Purpose: Declares the generic block-device layer: the description of a device
 *          that transfers fixed-size blocks, the operations a driver supplies to
 *          register one, and the validated read and write path through which
 *          every caller above reaches a disk.
 * Key definitions: BlockOperations, BlockDevice, BlockRegister, BlockUnregister,
 *          BlockRead, BlockWrite, BlockDeviceCount, BlockDeviceAt,
 *          BlockFindByName, BlockReport.
 * References:
 *   - docs/storage/BLOCK.md: the design of this layer and the reasons for its shape.
 *   - AT Attachment with Packet Interface: the device this layer is first
 *     implemented over transfers 512-byte sectors addressed by logical block
 *     number, which is the interface generalised here.
 */

#ifndef OXYS_BLOCK_H
#define OXYS_BLOCK_H

#include <oxys/types.h>

/* The number of devices that may be registered at once. */
#define BLOCK_DEVICE_CAPACITY 8U

/* The greatest length of a device name, excluding its terminator. */
#define BLOCK_NAME_MAXIMUM 15U

/* The block size every device registered so far uses, and the only size the
 * buffer cache of sub-task 4.6 is prepared to hold. */
#define BLOCK_SIZE_DEFAULT 512U

typedef struct BlockDevice BlockDevice;

/*
 * The operations a driver supplies. Each transfers whole blocks and returns
 * false upon any failure, the layer above being responsible for saying what was
 * asked and the driver for saying what went wrong.
 *
 * Neither is called with a null buffer, a count of zero, or a range outside the
 * device: the layer refuses those before the driver is reached, so that every
 * driver need not repeat the same four tests.
 */
typedef struct BlockOperations
{
    bool (*read)(void *context, uint64_t block, uint32_t count, void *buffer);
    bool (*write)(void *context, uint64_t block, uint32_t count, const void *buffer);
} BlockOperations;

/*
 * A registered device. The structure is exposed so that a caller may read a
 * device's geometry and accounting; it is not to be composed by hand, the layer
 * owning the storage of every device.
 */
struct BlockDevice
{
    char name[BLOCK_NAME_MAXIMUM + 1U];
    const BlockOperations *operations;
    void *context; /* The driver's own description of the device. */
    uint32_t block_size;
    uint64_t block_count;
    bool read_only;
    bool registered;

    /* Accounting, per device. */
    uint64_t reads;
    uint64_t writes;
    uint64_t blocks_read;
    uint64_t blocks_written;
    uint64_t errors;
    uint64_t refusals;
};

/*
 * Registers a device and returns it, or null if the table is full, the name is
 * unusable or already taken, the operations are incomplete, or the geometry is
 * degenerate.
 *
 * The name is how everything above identifies the device; it is copied, not
 * retained by reference.
 */
BlockDevice *BlockRegister(const char *name, const BlockOperations *operations, void *context,
                          uint32_t block_size, uint64_t block_count, bool read_only);

/*
 * Withdraws a device. The caller must first have flushed and discarded anything
 * held elsewhere upon it: this layer does not know what caches stand above it,
 * and a buffer written back to a device that has been withdrawn would be written
 * to whatever is registered in its place.
 *
 * Returns false if the device was not registered.
 */
bool BlockUnregister(BlockDevice *device);

/*
 * Reads or writes count blocks beginning at the stated block. The buffer holds
 * count times the device's block size in bytes.
 *
 * Returns false, without reaching the driver, if the device is not registered,
 * the buffer is absent, the range lies outside the device, or a write is asked
 * of a read-only device; and false, having reached it, if the driver failed. A
 * count of zero succeeds and does nothing.
 */
bool BlockRead(BlockDevice *device, uint64_t block, uint32_t count, void *buffer);
bool BlockWrite(BlockDevice *device, uint64_t block, uint32_t count, const void *buffer);

/* The number of devices presently registered. */
size_t BlockDeviceCount(void);

/*
 * The registered device at an index below BlockDeviceCount, or null beyond it.
 * The order is the order of registration among the slots presently occupied.
 */
BlockDevice *BlockDeviceAt(size_t index);

/* The registered device of that name, or null. */
BlockDevice *BlockFindByName(const char *name);

/* Accounting across every device, including those since withdrawn. */
uint64_t BlockTotalReads(void);
uint64_t BlockTotalWrites(void);
uint64_t BlockTotalRefusals(void);
uint64_t BlockTotalErrors(void);

/* Writes every registered device, and the accounting, to the console. */
void BlockReport(void);

#endif /* OXYS_BLOCK_H */
