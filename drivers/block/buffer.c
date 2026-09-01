/*
 * File: drivers/block/buffer.c
 * Purpose: Implements the buffer cache above the generic block layer: the hash
 *          by which a held block is found, the list by which the least recently
 *          used is chosen for eviction, the reference count that protects a
 *          buffer a caller is still using, and the write-back discipline that
 *          decides when a modified block reaches its device.
 * Key functions: BufferInitialise, BufferGet, BufferRelease, BufferMarkDirty,
 *          BufferFlush, BufferSync, BufferInvalidateDevice, BufferReport.
 * References:
 *   - docs/BUFFER.md: the design, the policies and what each self-test asserts.
 *   - docs/BLOCK.md, Section 4: the order — flush, invalidate, withdraw — that a
 *     device's removal requires, and why this cache must be told of it.
 *   - ISO/IEC 9899:2011, Section 6.3.2.3: the conversion of a pointer to an
 *     integer type, used to derive a hash from a device's address.
 */

#include <oxys/buffer.h>
#include <oxys/heap.h>
#include <oxys/kernel.h>

/*
 * The number of hash buckets. It is a power of two so that the bucket is a mask
 * rather than a division, and larger than the number of buffers so that the
 * chains stay short even when every buffer is in use.
 */
#define BUFFER_BUCKET_COUNT 128U

_Static_assert((BUFFER_BUCKET_COUNT & (BUFFER_BUCKET_COUNT - 1U)) == 0U,
               "the bucket count must be a power of two for the mask to be a modulus");

/* The value of an index that refers to no buffer. */
#define BUFFER_NONE SIZE_MAX

/*
 * The cache's own record of a buffer, holding the links that the caller has no
 * business seeing. The buffers themselves are held apart, so that the structure
 * a caller is given contains nothing it might reasonably alter.
 */
typedef struct BufferEntry
{
    size_t hash_next;    /* The next entry in the same bucket. */
    size_t older;        /* Towards the least recently used. */
    size_t newer;        /* Towards the most recently used. */
    bool linked;         /* True while the entry stands in a hash bucket. */
    bool listed;         /* True while the entry stands in the recency list. */
} BufferEntry;

static Buffer BufferTable[BUFFER_CAPACITY];
static BufferEntry BufferEntries[BUFFER_CAPACITY];
static size_t BufferBuckets[BUFFER_BUCKET_COUNT];

/* The ends of the recency list: the most and least recently used entries. */
static size_t BufferNewest = BUFFER_NONE;
static size_t BufferOldest = BUFFER_NONE;

/* The single allocation from which every buffer's storage is taken. */
static uint8_t *BufferStorage;
static bool BufferReady;

/* Accounting. */
static uint64_t BufferHitCount;
static uint64_t BufferMissCount;
static uint64_t BufferEvictionCount;
static uint64_t BufferWriteBackCount;
static uint64_t BufferFailureCount;

/* The bucket a block of a device belongs in. */
static size_t BufferBucketOf(const BlockDevice *device, uint64_t block)
{
    /*
     * The device is part of the key and not merely of the comparison: block zero
     * of one device and block zero of another are different blocks, and a hash
     * taken from the block number alone would place them in one bucket and rely
     * upon the comparison to separate them. Including the address distributes
     * them instead.
     */
    const uint64_t address = (uint64_t)(uintptr_t)device;

    return (size_t)((block ^ (address >> 4)) & (uint64_t)(BUFFER_BUCKET_COUNT - 1U));
}

/* Places an entry at the head of its bucket. */
static void BufferLink(size_t index)
{
    const size_t bucket = BufferBucketOf(BufferTable[index].device, BufferTable[index].block);

    BufferEntries[index].hash_next = BufferBuckets[bucket];
    BufferBuckets[bucket] = index;
    BufferEntries[index].linked = true;
}

/* Removes an entry from its bucket. */
static void BufferUnlink(size_t index)
{
    size_t bucket;
    size_t current;

    if (!BufferEntries[index].linked)
    {
        return;
    }

    bucket = BufferBucketOf(BufferTable[index].device, BufferTable[index].block);
    current = BufferBuckets[bucket];

    if (current == index)
    {
        BufferBuckets[bucket] = BufferEntries[index].hash_next;
    }
    else
    {
        while ((current != BUFFER_NONE) && (BufferEntries[current].hash_next != index))
        {
            current = BufferEntries[current].hash_next;
        }

        if (current != BUFFER_NONE)
        {
            BufferEntries[current].hash_next = BufferEntries[index].hash_next;
        }
    }

    BufferEntries[index].hash_next = BUFFER_NONE;
    BufferEntries[index].linked = false;
}

/*
 * Removes an entry from the recency list.
 *
 * An entry that is not in the list is left alone. Without that test the ends of
 * the list are overwritten with the neighbours of an entry that has none, which
 * empties the list and leaves every entry unreachable — and an unreachable entry
 * is one the cache can neither find nor reuse.
 */
static void BufferDetach(size_t index)
{
    const size_t older = BufferEntries[index].older;
    const size_t newer = BufferEntries[index].newer;

    if (!BufferEntries[index].listed)
    {
        return;
    }

    if (newer != BUFFER_NONE)
    {
        BufferEntries[newer].older = older;
    }
    else
    {
        BufferNewest = older;
    }

    if (older != BUFFER_NONE)
    {
        BufferEntries[older].newer = newer;
    }
    else
    {
        BufferOldest = newer;
    }

    BufferEntries[index].older = BUFFER_NONE;
    BufferEntries[index].newer = BUFFER_NONE;
    BufferEntries[index].listed = false;
}

/* Places an entry at the most recently used end of the list. */
static void BufferTouch(size_t index)
{
    BufferDetach(index);

    BufferEntries[index].older = BufferNewest;
    BufferEntries[index].newer = BUFFER_NONE;

    if (BufferNewest != BUFFER_NONE)
    {
        BufferEntries[BufferNewest].newer = index;
    }

    BufferNewest = index;
    BufferEntries[index].listed = true;

    if (BufferOldest == BUFFER_NONE)
    {
        BufferOldest = index;
    }
}

/* Finds the entry holding a block, or BUFFER_NONE. */
static size_t BufferFind(const BlockDevice *device, uint64_t block)
{
    size_t index = BufferBuckets[BufferBucketOf(device, block)];

    while (index != BUFFER_NONE)
    {
        if (BufferTable[index].valid && (BufferTable[index].device == device) &&
            (BufferTable[index].block == block))
        {
            return index;
        }

        index = BufferEntries[index].hash_next;
    }

    return BUFFER_NONE;
}

/* Writes an entry back to its device if it is dirty. */
static bool BufferWriteBack(size_t index)
{
    Buffer *const buffer = &BufferTable[index];

    if (!buffer->valid || !buffer->dirty)
    {
        return true;
    }

    if (!BlockWrite(buffer->device, buffer->block, 1U, buffer->data))
    {
        ++BufferFailureCount;
        return false;
    }

    buffer->dirty = false;
    ++BufferWriteBackCount;
    return true;
}

/*
 * Chooses an entry to hold a new block: an unused one if there is one, otherwise
 * the least recently used entry that no caller holds.
 *
 * A buffer with references outstanding is passed over rather than evicted. The
 * alternative is to hand the same storage to two callers at once, one of whom
 * believes it holds a block that has been replaced beneath it, which is a defect
 * that would appear as corruption at some unrelated place much later.
 */
static size_t BufferClaim(void)
{
    size_t index = BufferOldest;

    while (index != BUFFER_NONE)
    {
        if (BufferTable[index].references == 0U)
        {
            break;
        }

        index = BufferEntries[index].newer;
    }

    if (index == BUFFER_NONE)
    {
        return BUFFER_NONE;
    }

    if (BufferTable[index].valid)
    {
        /*
         * The contents are written back before the storage is reused. A cache
         * that discarded a dirty block would lose a write that had already been
         * reported as successful, and the loss would be discovered only when
         * something read the block back and found the old contents.
         */
        if (!BufferWriteBack(index))
        {
            return BUFFER_NONE;
        }

        BufferUnlink(index);
        ++BufferEvictionCount;
    }

    BufferTable[index].valid = false;
    BufferTable[index].dirty = false;
    BufferTable[index].device = NULL;
    BufferTable[index].block = 0U;
    return index;
}

bool BufferInitialise(void)
{
    BufferStorage = (uint8_t *)KernelAllocate((size_t)BUFFER_CAPACITY * BUFFER_BLOCK_SIZE);

    if (BufferStorage == NULL)
    {
        BufferReady = false;
        return false;
    }

    for (size_t index = 0U; index < BUFFER_BUCKET_COUNT; ++index)
    {
        BufferBuckets[index] = BUFFER_NONE;
    }

    BufferNewest = BUFFER_NONE;
    BufferOldest = BUFFER_NONE;

    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        BufferTable[index].device = NULL;
        BufferTable[index].block = 0U;
        BufferTable[index].data = &BufferStorage[index * BUFFER_BLOCK_SIZE];
        BufferTable[index].references = 0U;
        BufferTable[index].valid = false;
        BufferTable[index].dirty = false;

        BufferEntries[index].hash_next = BUFFER_NONE;
        BufferEntries[index].older = BUFFER_NONE;
        BufferEntries[index].newer = BUFFER_NONE;
        BufferEntries[index].linked = false;
        BufferEntries[index].listed = false;

        /*
         * Every entry stands in the recency list from the outset, the unused
         * ones at its least recently used end, so that BufferClaim has one rule
         * and not two.
         */
        BufferTouch(index);
    }

    /* The list was built newest-first above; the unused entries are all equal. */
    BufferHitCount = 0U;
    BufferMissCount = 0U;
    BufferEvictionCount = 0U;
    BufferWriteBackCount = 0U;
    BufferFailureCount = 0U;
    BufferReady = true;
    return true;
}

Buffer *BufferGet(BlockDevice *device, uint64_t block)
{
    size_t index;

    if (!BufferReady || (device == NULL))
    {
        return NULL;
    }

    if (device->block_size != BUFFER_BLOCK_SIZE)
    {
        /*
         * The cache holds blocks of one size. A device of another may still be
         * addressed through the block layer directly; it simply is not cached.
         */
        return NULL;
    }

    index = BufferFind(device, block);

    if (index != BUFFER_NONE)
    {
        ++BufferHitCount;
        BufferTouch(index);
        ++BufferTable[index].references;
        return &BufferTable[index];
    }

    ++BufferMissCount;

    index = BufferClaim();

    if (index == BUFFER_NONE)
    {
        return NULL;
    }

    BufferTable[index].device = device;
    BufferTable[index].block = block;

    if (!BlockRead(device, block, 1U, BufferTable[index].data))
    {
        /*
         * The entry keeps its storage and is returned to the unused state. It
         * must not be left holding the identity of a block it does not hold the
         * contents of, which a later lookup would find and believe.
         */
        BufferTable[index].device = NULL;
        BufferTable[index].block = 0U;
        ++BufferFailureCount;
        return NULL;
    }

    BufferTable[index].valid = true;
    BufferTable[index].dirty = false;
    BufferTable[index].references = 1U;
    BufferLink(index);
    BufferTouch(index);

    return &BufferTable[index];
}

void BufferRelease(Buffer *buffer)
{
    if ((buffer == NULL) || (buffer->references == 0U))
    {
        return;
    }

    --buffer->references;
}

void BufferMarkDirty(Buffer *buffer)
{
    if ((buffer == NULL) || !buffer->valid)
    {
        return;
    }

    buffer->dirty = true;
}

bool BufferFlush(Buffer *buffer)
{
    /*
     * The index is recovered from the address, every buffer a caller can hold
     * being an element of the table. A pointer from anywhere else is refused
     * rather than converted into an index that would name some other buffer.
     */
    if ((buffer < &BufferTable[0]) || (buffer >= &BufferTable[BUFFER_CAPACITY]))
    {
        return false;
    }

    return BufferWriteBack((size_t)(buffer - BufferTable));
}

bool BufferSync(void)
{
    bool succeeded = true;

    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        if (!BufferWriteBack(index))
        {
            succeeded = false;
        }
    }

    return succeeded;
}

bool BufferInvalidateDevice(BlockDevice *device)
{
    bool succeeded = true;

    if (device == NULL)
    {
        return false;
    }

    /*
     * Nothing is discarded while anything is held. A caller holding a buffer of
     * the device is about to write into storage the cache no longer associates
     * with any block, and the check is cheap where the consequence is not.
     */
    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        if ((BufferTable[index].device == device) && (BufferTable[index].references != 0U))
        {
            return false;
        }
    }

    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        if (BufferTable[index].device != device)
        {
            continue;
        }

        if (!BufferWriteBack(index))
        {
            succeeded = false;
            continue;
        }

        BufferUnlink(index);
        BufferTable[index].valid = false;
        BufferTable[index].dirty = false;
        BufferTable[index].device = NULL;
        BufferTable[index].block = 0U;

        /* The entry becomes the next to be claimed, holding nothing. */
        BufferDetach(index);
        BufferEntries[index].newer = BufferOldest;
        BufferEntries[index].older = BUFFER_NONE;

        if (BufferOldest != BUFFER_NONE)
        {
            BufferEntries[BufferOldest].older = index;
        }

        BufferOldest = index;
        BufferEntries[index].listed = true;

        if (BufferNewest == BUFFER_NONE)
        {
            BufferNewest = index;
        }
    }

    return succeeded;
}

size_t BufferCount(void)
{
    return BufferReady ? BUFFER_CAPACITY : 0U;
}

size_t BufferValidCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        if (BufferTable[index].valid)
        {
            ++count;
        }
    }

    return count;
}

size_t BufferDirtyCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        if (BufferTable[index].valid && BufferTable[index].dirty)
        {
            ++count;
        }
    }

    return count;
}

size_t BufferHeldCount(void)
{
    size_t count = 0U;

    for (size_t index = 0U; index < BUFFER_CAPACITY; ++index)
    {
        if (BufferTable[index].references != 0U)
        {
            ++count;
        }
    }

    return count;
}

uint64_t BufferHits(void)
{
    return BufferHitCount;
}

uint64_t BufferMisses(void)
{
    return BufferMissCount;
}

uint64_t BufferEvictions(void)
{
    return BufferEvictionCount;
}

uint64_t BufferWriteBacks(void)
{
    return BufferWriteBackCount;
}

uint64_t BufferFailures(void)
{
    return BufferFailureCount;
}

void BufferReport(void)
{
    if (!BufferReady)
    {
        KernelWriteString("Buffer cache: unavailable; its storage could not be allocated.\n");
        return;
    }

    KernelWriteString("Buffer cache: ");
    KernelWriteDecimal((uint64_t)BUFFER_CAPACITY);
    KernelWriteString(" buffers of ");
    KernelWriteDecimal((uint64_t)BUFFER_BLOCK_SIZE);
    KernelWriteString(" bytes (");
    KernelWriteDecimal(((uint64_t)BUFFER_CAPACITY * BUFFER_BLOCK_SIZE) / 1024U);
    KernelWriteString(" KiB), ");
    KernelWriteDecimal((uint64_t)BufferValidCount());
    KernelWriteString(" holding a block, ");
    KernelWriteDecimal((uint64_t)BufferDirtyCount());
    KernelWriteString(" dirty, ");
    KernelWriteDecimal((uint64_t)BufferHeldCount());
    KernelWriteString(" held.\n");

    KernelWriteString("Buffer cache: hits ");
    KernelWriteDecimal(BufferHitCount);
    KernelWriteString(", misses ");
    KernelWriteDecimal(BufferMissCount);
    KernelWriteString(", evictions ");
    KernelWriteDecimal(BufferEvictionCount);
    KernelWriteString(", write-backs ");
    KernelWriteDecimal(BufferWriteBackCount);
    KernelWriteString(", failures ");
    KernelWriteDecimal(BufferFailureCount);
    KernelWriteString(".\n");
}
