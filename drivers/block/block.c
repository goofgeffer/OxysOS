/*
 * File: drivers/block/block.c
 * Purpose: Implements the generic block-device layer: the registry of devices
 *          that transfer fixed-size blocks, and the validated read and write
 *          path through which every caller above reaches a driver.
 * Key functions: BlockRegister, BlockUnregister, BlockRead, BlockWrite,
 *          BlockDeviceAt, BlockFindByName, BlockReport.
 * References:
 *   - docs/storage/BLOCK.md: the design of this layer, and the reasons for validating
 *     here rather than in each driver.
 *   - ISO/IEC 9899:2011, Section 6.7.3: the const qualifier upon the write
 *     buffer, which this layer preserves down to the driver's operation.
 */

#include <oxys/block.h>
#include <oxys/kernel.h>

/* The registry. A slot is occupied while its registered flag is set. */
static BlockDevice BlockDevices[BLOCK_DEVICE_CAPACITY];

/* Accounting across every device, retained when a device is withdrawn. */
static uint64_t BlockReads;
static uint64_t BlockWrites;
static uint64_t BlockRefusals;
static uint64_t BlockErrors;

/*
 * Records a request the layer declined to pass to a driver. It is counted
 * separately from a failure of the hardware, for the reason given in
 * docs/storage/DISK.md, Section 7: a refusal is this layer working, and a figure that
 * added the two together would show a healthy machine accumulating errors.
 */
static bool BlockRefuse(BlockDevice *device)
{
    ++BlockRefusals;

    if (device != NULL)
    {
        ++device->refusals;
    }

    return false;
}

/* Compares two names, neither of which may be null. */
static bool BlockNamesMatch(const char *left, const char *right)
{
    size_t index = 0U;

    while ((left[index] != '\0') && (left[index] == right[index]))
    {
        ++index;
    }

    return left[index] == right[index];
}

/*
 * Copies a name into a device, returning false if it is empty or longer than the
 * field. It is truncated by no one: a name that did not fit would identify a
 * different device from the one the caller meant.
 */
static bool BlockCopyName(BlockDevice *device, const char *name)
{
    size_t length = 0U;

    while (name[length] != '\0')
    {
        if (length >= BLOCK_NAME_MAXIMUM)
        {
            return false;
        }

        device->name[length] = name[length];
        ++length;
    }

    if (length == 0U)
    {
        return false;
    }

    device->name[length] = '\0';
    return true;
}

BlockDevice *BlockRegister(const char *name, const BlockOperations *operations, void *context,
                          uint32_t block_size, uint64_t block_count, bool read_only)
{
    BlockDevice *device = NULL;

    if ((name == NULL) || (operations == NULL) || (operations->read == NULL) ||
        (block_size == 0U) || (block_count == 0U))
    {
        (void)BlockRefuse(NULL);
        return NULL;
    }

    /*
     * A device that cannot be written must not be registered with a write
     * operation the layer would never call, and one that can be written must
     * have one; the alternative is a device whose read-only flag and whose
     * behaviour disagree.
     */
    if (read_only != (operations->write == NULL))
    {
        (void)BlockRefuse(NULL);
        return NULL;
    }

    if (BlockFindByName(name) != NULL)
    {
        (void)BlockRefuse(NULL);
        return NULL;
    }

    for (size_t index = 0U; index < BLOCK_DEVICE_CAPACITY; ++index)
    {
        if (!BlockDevices[index].registered)
        {
            device = &BlockDevices[index];
            break;
        }
    }

    if (device == NULL)
    {
        (void)BlockRefuse(NULL);
        return NULL;
    }

    if (!BlockCopyName(device, name))
    {
        (void)BlockRefuse(NULL);
        return NULL;
    }

    device->operations = operations;
    device->context = context;
    device->block_size = block_size;
    device->block_count = block_count;
    device->read_only = read_only;
    device->reads = 0U;
    device->writes = 0U;
    device->blocks_read = 0U;
    device->blocks_written = 0U;
    device->errors = 0U;
    device->refusals = 0U;
    device->registered = true;

    return device;
}

bool BlockUnregister(BlockDevice *device)
{
    if ((device == NULL) || !device->registered)
    {
        return BlockRefuse(device);
    }

    device->registered = false;
    device->operations = NULL;
    device->context = NULL;
    device->name[0] = '\0';
    return true;
}

/*
 * The common part of a transfer. The two directions differ only in which
 * operation is called and in whether a read-only device may be addressed, so the
 * validation — which is the whole purpose of this layer — is written once.
 */
static bool BlockTransfer(BlockDevice *device, uint64_t block, uint32_t count, void *buffer,
                          bool writing)
{
    bool succeeded;

    if ((device == NULL) || !device->registered)
    {
        return BlockRefuse(device);
    }

    if (writing && device->read_only)
    {
        return BlockRefuse(device);
    }

    if (count == 0U)
    {
        return true;
    }

    if (buffer == NULL)
    {
        return BlockRefuse(device);
    }

    /*
     * The bound is expressed as a subtraction rather than as an addition. The
     * block number is 64 bits wide and a caller may present one near its
     * greatest value, in which case block + count would wrap and a range wholly
     * outside the device would be accepted.
     */
    if ((block >= device->block_count) || ((uint64_t)count > (device->block_count - block)))
    {
        return BlockRefuse(device);
    }

    if (writing)
    {
        succeeded = device->operations->write(device->context, block, count, buffer);
    }
    else
    {
        succeeded = device->operations->read(device->context, block, count, buffer);
    }

    if (!succeeded)
    {
        ++device->errors;
        ++BlockErrors;
        return false;
    }

    if (writing)
    {
        ++device->writes;
        ++BlockWrites;
        device->blocks_written += count;
    }
    else
    {
        ++device->reads;
        ++BlockReads;
        device->blocks_read += count;
    }

    return true;
}

bool BlockRead(BlockDevice *device, uint64_t block, uint32_t count, void *buffer)
{
    return BlockTransfer(device, block, count, buffer, false);
}

bool BlockWrite(BlockDevice *device, uint64_t block, uint32_t count, const void *buffer)
{
    /*
     * The cast discards a qualifier that the shared path cannot express in both
     * directions. The writing path passes the buffer to an operation whose own
     * parameter is const, so nothing here or below may modify it.
     */
    return BlockTransfer(device, block, count, (void *)(uintptr_t)buffer, true);
}

size_t BlockDeviceCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < BLOCK_DEVICE_CAPACITY; ++index)
    {
        if (BlockDevices[index].registered)
        {
            ++count;
        }
    }

    return count;
}

BlockDevice *BlockDeviceAt(size_t index)
{
    size_t seen = 0U;

    for (size_t position = 0U; position < BLOCK_DEVICE_CAPACITY; ++position)
    {
        if (!BlockDevices[position].registered)
        {
            continue;
        }

        if (seen == index)
        {
            return &BlockDevices[position];
        }

        ++seen;
    }

    return NULL;
}

BlockDevice *BlockFindByName(const char *name)
{
    if (name == NULL)
    {
        return NULL;
    }

    for (size_t index = 0U; index < BLOCK_DEVICE_CAPACITY; ++index)
    {
        if (BlockDevices[index].registered && BlockNamesMatch(BlockDevices[index].name, name))
        {
            return &BlockDevices[index];
        }
    }

    return NULL;
}

uint64_t BlockTotalReads(void)
{
    return BlockReads;
}

uint64_t BlockTotalWrites(void)
{
    return BlockWrites;
}

uint64_t BlockTotalRefusals(void)
{
    return BlockRefusals;
}

uint64_t BlockTotalErrors(void)
{
    return BlockErrors;
}

void BlockReport(void)
{
    const size_t count = BlockDeviceCount();

    KernelWriteString("Block layer: ");
    KernelWriteDecimal((uint64_t)count);
    KernelWriteString(" devices registered of ");
    KernelWriteDecimal((uint64_t)BLOCK_DEVICE_CAPACITY);
    KernelWriteString(".\n");

    for (size_t index = 0U; index < count; ++index)
    {
        const BlockDevice *const device = BlockDeviceAt(index);

        if (device == NULL)
        {
            break;
        }

        KernelWriteString("  ");
        KernelWriteString(device->name);
        KernelWriteString(": ");
        KernelWriteDecimal(device->block_count);
        KernelWriteString(" blocks of ");
        KernelWriteDecimal((uint64_t)device->block_size);
        KernelWriteString(" bytes (");
        KernelWriteDecimal((device->block_count / 1024U) * device->block_size);
        KernelWriteString(" KiB), ");
        KernelWriteString(device->read_only ? "read-only" : "writable");
        KernelWriteString(", blocks read ");
        KernelWriteDecimal(device->blocks_read);
        KernelWriteString(", written ");
        KernelWriteDecimal(device->blocks_written);
        KernelWriteString("\n");
    }

    KernelWriteString("Block layer: reads ");
    KernelWriteDecimal(BlockReads);
    KernelWriteString(", writes ");
    KernelWriteDecimal(BlockWrites);
    KernelWriteString(", device errors ");
    KernelWriteDecimal(BlockErrors);
    KernelWriteString(", requests refused ");
    KernelWriteDecimal(BlockRefusals);
    KernelWriteString(".\n");
}
