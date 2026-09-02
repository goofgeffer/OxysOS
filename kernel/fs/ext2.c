/*
 * File: kernel/fs/ext2.c
 * Purpose: Implements the reading and validation of an EXT2 superblock: the
 *          decoding of the volume's fields from their stored order into the
 *          processor's, the derivation of the geometry, and the judgement of
 *          whether the volume may be read, may be written, or may not be
 *          addressed at all.
 * Key functions: Ext2ReadSuperblock, Ext2GroupCount, Ext2ReadGroupDescriptor,
 *          Ext2VerifyGroupDescriptors, Ext2LastError, Ext2ReportVolume,
 *          Ext2ReportGroup.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout", the
 *     Superblock chapter and its field table: the superblock lies 1024 bytes
 *     from the start of the volume and occupies 1024 bytes; s_magic at offset 56
 *     holds 0xEF53; the block size is 1024 shifted left by s_log_block_size at
 *     offset 24; s_first_data_block at offset 20 is 1 upon a volume of 1024-byte
 *     blocks and 0 otherwise, the superblock occupying the first kibibyte in
 *     either case.
 *   - The same, Revision Levels: revision 0 has no field for the inode size or
 *     the first usable inode, which are 128 and 11 respectively; revision 1
 *     states both, at offsets 88 and 84.
 *   - The same, the feature flags at offsets 92, 96 and 100: a volume declaring
 *     an incompatible feature the implementation lacks may not be read, and one
 *     declaring a read-only compatible feature it lacks may be read and not
 *     written. That distinction is the entire purpose of the two fields.
 *   - The same, the Block Group Descriptor Table chapter: the table starts upon
 *     the first block following the superblock, which is the third block of a
 *     1 KiB volume and the second of any larger one; a descriptor is 32 bytes;
 *     bg_block_bitmap, bg_inode_bitmap and bg_inode_table at offsets 0, 4 and 8
 *     are absolute block identifiers, and bg_free_blocks_count,
 *     bg_free_inodes_count and bg_used_dirs_count at 12, 14 and 16 are halves.
 *   - The same, Inode Table: there is one inode table per group and it holds
 *     s_inodes_per_group inodes, so its length follows from the inode size.
 *   - Linux kernel documentation, the ext4 superblock and block group descriptor
 *     tables, consulted as an independent statement of the same offsets.
 */

#include <oxys/ext2.h>
#include <oxys/buffer.h>
#include <oxys/kernel.h>

/* The greatest value of s_log_block_size this kernel will accept. */
#define EXT2_MAXIMUM_LOG_BLOCK_SIZE 2U

/* A description of the most recent refusal, and the accounting. */
static const char *Ext2Error = "none";
static uint64_t Ext2Read;
static uint64_t Ext2Refused;
static uint64_t Ext2GroupsReadCount;
static uint64_t Ext2GroupsRefusedCount;

/* Records a refusal, so that a report may say why and not merely that. */
static bool Ext2Refuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2Refused;
    return false;
}

/*
 * The same for a group descriptor, counted apart from the volumes.
 *
 * A volume refused and a descriptor refused are different events with different
 * causes, and a single counter reporting their sum would say that something was
 * wrong without saying what kind of thing.
 */
static bool Ext2GroupRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2GroupsRefusedCount;
    return false;
}

/*
 * The decoders.
 *
 * Every quantity upon an EXT2 volume is stored least significant byte first,
 * whatever the machine that wrote it and whatever the machine that reads it. The
 * decoding is therefore written out rather than obtained by laying a structure
 * over the bytes: a structure would be correct only upon a little-endian
 * processor and would additionally require the packing of a structure, which is
 * a compiler extension this project does not admit. Written out, the byte order
 * of the volume is stated in the code that depends upon it.
 */
static uint16_t Ext2ReadHalf(const uint8_t *raw, size_t offset)
{
    return (uint16_t)((uint16_t)raw[offset] | ((uint16_t)raw[offset + 1U] << 8));
}

static uint32_t Ext2ReadWord(const uint8_t *raw, size_t offset)
{
    return (uint32_t)raw[offset] | ((uint32_t)raw[offset + 1U] << 8) |
           ((uint32_t)raw[offset + 2U] << 16) | ((uint32_t)raw[offset + 3U] << 24);
}

/*
 * Copies a fixed-length character field, which is padded with zero bytes rather
 * than terminated, into a buffer that is terminated. A field that is entirely
 * full has no terminator upon the volume at all, which is why the destination is
 * one character longer than the field.
 */
static void Ext2ReadText(const uint8_t *raw, size_t offset, size_t length, char *destination)
{
    size_t index = 0U;

    while ((index < length) && (raw[offset + index] != 0U))
    {
        destination[index] = (char)raw[offset + index];
        ++index;
    }

    destination[index] = '\0';
}

/* The number of groups needed to hold a quantity divided into fixed parts. */
static uint32_t Ext2DivideRoundingUp(uint32_t quantity, uint32_t divisor)
{
    return (quantity + (divisor - 1U)) / divisor;
}

/*
 * Reads a run of bytes from within one block of the volume, through the buffer
 * cache.
 *
 * A filesystem block is some whole number of the device's blocks, so a run
 * within one may span several of them; the loop copies from each in turn. The
 * caller asks for the bytes it needs and no more — a group descriptor is 32
 * bytes and a block pointer is four — because the cache is holding the block
 * regardless, and copying a whole 4 KiB block onto the kernel stack to take four
 * bytes out of it would be both wasteful and a stack the kernel cannot spare.
 */
static bool Ext2ReadBytes(BlockDevice *device, const Ext2Superblock *superblock,
                          uint32_t block, uint32_t offset, uint32_t length,
                          uint8_t *destination)
{
    uint64_t position;

    if (block >= superblock->block_count)
    {
        return Ext2Refuse("a read was attempted beyond the end of the volume");
    }

    if ((offset > superblock->block_size) || (length > (superblock->block_size - offset)))
    {
        return Ext2Refuse("a read was attempted beyond the end of a block");
    }

    position = ((uint64_t)block * superblock->block_size) + offset;

    while (length > 0U)
    {
        const uint64_t device_block = position / device->block_size;
        const uint32_t within = (uint32_t)(position % device->block_size);
        uint32_t take = device->block_size - within;
        Buffer *buffer;

        if (take > length)
        {
            take = length;
        }

        buffer = BufferGet(device, device_block);

        if (buffer == NULL)
        {
            return Ext2Refuse("a block of the volume could not be read");
        }

        for (uint32_t index = 0U; index < take; ++index)
        {
            destination[index] = buffer->data[within + index];
        }

        BufferRelease(buffer);

        destination += take;
        position += take;
        length -= take;
    }

    return true;
}

/*
 * Whether a block identifier read from the volume addresses a block that exists.
 *
 * Nothing of a filesystem lies before the first data block, so an identifier
 * below it is as wrong as one beyond the end, and both are checked wherever an
 * identifier is read rather than where it is used: a block number used unchecked
 * addresses somebody else's data with no symptom whatever.
 */
static bool Ext2BlockExists(const Ext2Superblock *superblock, uint32_t block)
{
    return (block >= superblock->first_data_block) && (block < superblock->block_count);
}

/*
 * Reads the 1024 bytes of the superblock through the buffer cache.
 *
 * The superblock begins 1024 bytes into the volume, which is two blocks of a
 * 512-byte device: the first kibibyte is left for a boot sector and belongs to
 * nobody else. Reading it through the cache rather than through the block layer
 * directly is deliberate — this is the first caller above the cache, and the
 * blocks it reads are the ones every later part of the filesystem will read
 * most often.
 */
static bool Ext2ReadSuperblockBytes(BlockDevice *device, uint8_t *raw)
{
    const uint64_t first = EXT2_SUPERBLOCK_OFFSET / device->block_size;
    const uint32_t count = EXT2_SUPERBLOCK_SIZE / device->block_size;

    for (uint32_t index = 0U; index < count; ++index)
    {
        Buffer *const buffer = BufferGet(device, first + index);

        if (buffer == NULL)
        {
            return false;
        }

        for (uint32_t position = 0U; position < device->block_size; ++position)
        {
            raw[(index * device->block_size) + position] = buffer->data[position];
        }

        BufferRelease(buffer);
    }

    return true;
}

bool Ext2ReadSuperblock(BlockDevice *device, Ext2Superblock *superblock)
{
    uint8_t raw[EXT2_SUPERBLOCK_SIZE];
    Ext2Superblock volume;
    uint32_t log_block_size;
    uint32_t groups_by_block;
    uint32_t groups_by_inode;

    if ((device == NULL) || (superblock == NULL))
    {
        return Ext2Refuse("no device or nowhere to put the superblock");
    }

    /*
     * The superblock must be a whole number of the device's own blocks, and the
     * device must be long enough to hold it. Neither is a property of EXT2; both
     * are properties of the device it was found upon.
     */
    if ((device->block_size == 0U) || (EXT2_SUPERBLOCK_OFFSET % device->block_size != 0U) ||
        (EXT2_SUPERBLOCK_SIZE % device->block_size != 0U))
    {
        return Ext2Refuse("the device's blocks do not divide the superblock");
    }

    if (device->block_count <
        ((EXT2_SUPERBLOCK_OFFSET + EXT2_SUPERBLOCK_SIZE) / device->block_size))
    {
        return Ext2Refuse("the device is too short to hold a superblock");
    }

    if (!Ext2ReadSuperblockBytes(device, raw))
    {
        return Ext2Refuse("the superblock could not be read from the device");
    }

    volume.magic = Ext2ReadHalf(raw, EXT2_OFFSET_MAGIC);

    if (volume.magic != EXT2_SUPER_MAGIC)
    {
        return Ext2Refuse("the volume bears no EXT2 magic number");
    }

    volume.revision = Ext2ReadWord(raw, EXT2_OFFSET_REVISION);

    /*
     * A revision beyond the one this kernel knows may place fields where it does
     * not expect them, so it is refused rather than read hopefully. The two that
     * exist are handled; a third would be a format this code has never seen.
     */
    if (volume.revision > EXT2_DYNAMIC_REV)
    {
        return Ext2Refuse("the volume is of a revision this kernel does not know");
    }

    volume.inode_count = Ext2ReadWord(raw, EXT2_OFFSET_INODE_COUNT);
    volume.block_count = Ext2ReadWord(raw, EXT2_OFFSET_BLOCK_COUNT);
    volume.reserved_block_count = Ext2ReadWord(raw, EXT2_OFFSET_RESERVED_BLOCKS);
    volume.free_block_count = Ext2ReadWord(raw, EXT2_OFFSET_FREE_BLOCKS);
    volume.free_inode_count = Ext2ReadWord(raw, EXT2_OFFSET_FREE_INODES);
    volume.first_data_block = Ext2ReadWord(raw, EXT2_OFFSET_FIRST_DATA_BLOCK);
    log_block_size = Ext2ReadWord(raw, EXT2_OFFSET_LOG_BLOCK_SIZE);
    volume.blocks_per_group = Ext2ReadWord(raw, EXT2_OFFSET_BLOCKS_PER_GROUP);
    volume.fragments_per_group = Ext2ReadWord(raw, EXT2_OFFSET_FRAGS_PER_GROUP);
    volume.inodes_per_group = Ext2ReadWord(raw, EXT2_OFFSET_INODES_PER_GROUP);
    volume.mount_time = Ext2ReadWord(raw, EXT2_OFFSET_MOUNT_TIME);
    volume.write_time = Ext2ReadWord(raw, EXT2_OFFSET_WRITE_TIME);
    volume.mount_count = Ext2ReadHalf(raw, EXT2_OFFSET_MOUNT_COUNT);
    volume.maximum_mount_count = Ext2ReadHalf(raw, EXT2_OFFSET_MAX_MOUNT_COUNT);
    volume.state = Ext2ReadHalf(raw, EXT2_OFFSET_STATE);
    volume.errors = Ext2ReadHalf(raw, EXT2_OFFSET_ERRORS);
    volume.minor_revision = Ext2ReadHalf(raw, EXT2_OFFSET_MINOR_REVISION);
    volume.last_check = Ext2ReadWord(raw, EXT2_OFFSET_LAST_CHECK);
    volume.check_interval = Ext2ReadWord(raw, EXT2_OFFSET_CHECK_INTERVAL);
    volume.creator_os = Ext2ReadWord(raw, EXT2_OFFSET_CREATOR_OS);
    volume.default_uid = Ext2ReadHalf(raw, EXT2_OFFSET_DEFAULT_UID);
    volume.default_gid = Ext2ReadHalf(raw, EXT2_OFFSET_DEFAULT_GID);

    /*
     * A block size is 1024 shifted left by the stored exponent. The limit is not
     * arbitrary: a filesystem block must be addressable as a whole number of the
     * device's blocks and must fit within the buffers this kernel holds, and
     * nothing above has yet any use for a larger one.
     */
    if (log_block_size > EXT2_MAXIMUM_LOG_BLOCK_SIZE)
    {
        return Ext2Refuse("the volume's block size is beyond this kernel");
    }

    volume.block_size = 1024U << log_block_size;
    volume.fragment_size = 1024U << Ext2ReadWord(raw, EXT2_OFFSET_LOG_FRAGMENT_SIZE);

    if (volume.block_size < device->block_size)
    {
        return Ext2Refuse("the volume's blocks are smaller than the device's");
    }

    volume.sectors_per_block = volume.block_size / device->block_size;

    /*
     * The superblock occupies the second kibibyte of the volume whatever the
     * block size, so it is within block 1 of a volume of 1024-byte blocks and
     * within block 0 of any other. The specification states the value of
     * s_first_data_block in those terms, and a volume that disagrees with itself
     * here would send every later calculation to the wrong block.
     */
    if (volume.first_data_block != ((volume.block_size == 1024U) ? 1U : 0U))
    {
        return Ext2Refuse("the first data block contradicts the block size");
    }

    if ((volume.block_count == 0U) || (volume.inode_count == 0U) ||
        (volume.blocks_per_group == 0U) || (volume.inodes_per_group == 0U))
    {
        return Ext2Refuse("the volume's geometry is degenerate");
    }

    if (volume.first_data_block >= volume.block_count)
    {
        return Ext2Refuse("the first data block lies beyond the volume");
    }

    if (volume.blocks_per_group > (volume.block_size * 8U))
    {
        /*
         * A group's blocks are recorded in a bitmap occupying one block, so a
         * group can hold no more blocks than that bitmap has bits.
         */
        return Ext2Refuse("a group holds more blocks than its bitmap can record");
    }

    if (volume.inodes_per_group > (volume.block_size * 8U))
    {
        return Ext2Refuse("a group holds more inodes than its bitmap can record");
    }

    if ((volume.free_block_count > volume.block_count) ||
        (volume.free_inode_count > volume.inode_count))
    {
        return Ext2Refuse("the volume reports more free than it holds");
    }

    /* The revision 0 volume states neither of these and is fixed at both. */
    if (volume.revision == EXT2_GOOD_OLD_REV)
    {
        volume.first_inode = EXT2_GOOD_OLD_FIRST_INODE;
        volume.inode_size = (uint16_t)EXT2_GOOD_OLD_INODE_SIZE;
        volume.block_group_number = 0U;
        volume.feature_compatible = 0U;
        volume.feature_incompatible = 0U;
        volume.feature_read_only = 0U;
        volume.uuid[0] = 0U;
        volume.volume_name[0] = '\0';
        volume.last_mounted[0] = '\0';

        for (size_t index = 0U; index < EXT2_UUID_LENGTH; ++index)
        {
            volume.uuid[index] = 0U;
        }
    }
    else
    {
        volume.first_inode = Ext2ReadWord(raw, EXT2_OFFSET_FIRST_INODE);
        volume.inode_size = Ext2ReadHalf(raw, EXT2_OFFSET_INODE_SIZE);
        volume.block_group_number = Ext2ReadHalf(raw, EXT2_OFFSET_BLOCK_GROUP_NUMBER);
        volume.feature_compatible = Ext2ReadWord(raw, EXT2_OFFSET_FEATURE_COMPAT);
        volume.feature_incompatible = Ext2ReadWord(raw, EXT2_OFFSET_FEATURE_INCOMPAT);
        volume.feature_read_only = Ext2ReadWord(raw, EXT2_OFFSET_FEATURE_RO_COMPAT);

        for (size_t index = 0U; index < EXT2_UUID_LENGTH; ++index)
        {
            volume.uuid[index] = raw[EXT2_OFFSET_UUID + index];
        }

        Ext2ReadText(raw, EXT2_OFFSET_VOLUME_NAME, EXT2_VOLUME_NAME_LENGTH, volume.volume_name);
        Ext2ReadText(raw, EXT2_OFFSET_LAST_MOUNTED, EXT2_LAST_MOUNTED_LENGTH,
                     volume.last_mounted);
    }

    /*
     * An inode must be a power of two in size, no smaller than the size a
     * revision 0 volume fixes and no larger than a block, so that a whole number
     * of them occupies a block and none straddles two.
     */
    if ((volume.inode_size < EXT2_GOOD_OLD_INODE_SIZE) ||
        (volume.inode_size > volume.block_size) ||
        ((volume.inode_size & (volume.inode_size - 1U)) != 0U))
    {
        return Ext2Refuse("the inode size is not a power of two within a block");
    }

    if ((volume.first_inode < EXT2_GOOD_OLD_FIRST_INODE) ||
        (volume.first_inode > volume.inode_count))
    {
        return Ext2Refuse("the first usable inode is outside the volume");
    }

    /*
     * The number of groups is derivable twice, from two independent fields, and
     * the two must agree. A superblock whose block count and inode count imply
     * different numbers of groups is corrupt in a way that would otherwise be
     * discovered only when a descriptor was read from beyond the table.
     */
    groups_by_block = Ext2DivideRoundingUp(volume.block_count - volume.first_data_block,
                                           volume.blocks_per_group);
    groups_by_inode = Ext2DivideRoundingUp(volume.inode_count, volume.inodes_per_group);

    if (groups_by_block != groups_by_inode)
    {
        return Ext2Refuse("the block count and the inode count imply different group counts");
    }

    volume.group_count = groups_by_block;

    /*
     * An incompatible feature is one whose absence from an implementation makes
     * the volume unreadable; a read-only compatible feature is one whose absence
     * makes it unwritable. The two fields exist precisely so that an
     * implementation of this age can be told what it must not attempt.
     */
    if ((volume.feature_incompatible & ~(uint32_t)EXT2_FEATURES_INCOMPAT_SUPPORTED) != 0U)
    {
        return Ext2Refuse("the volume requires a feature this kernel does not implement");
    }

    volume.read_only =
        ((volume.feature_read_only & ~(uint32_t)EXT2_FEATURES_RO_COMPAT_SUPPORTED) != 0U) ||
        (volume.state != EXT2_VALID_FS);

    *superblock = volume;
    ++Ext2Read;
    return true;
}

uint32_t Ext2GroupCount(const Ext2Superblock *superblock)
{
    return (superblock != NULL) ? superblock->group_count : 0U;
}

uint32_t Ext2GroupDescriptorBlock(const Ext2Superblock *superblock)
{
    /*
     * The table begins upon the block after the one the superblock lies within.
     * The superblock always occupies the second kibibyte of the volume, so that
     * block is block 1 of a volume of 1024-byte blocks and block 0 of any other,
     * which is exactly what s_first_data_block holds.
     */
    return superblock->first_data_block + 1U;
}

uint32_t Ext2GroupDescriptorBlocks(const Ext2Superblock *superblock)
{
    const uint32_t per_block = superblock->block_size / EXT2_GROUP_DESCRIPTOR_SIZE;

    return Ext2DivideRoundingUp(superblock->group_count, per_block);
}

uint32_t Ext2InodeTableBlocks(const Ext2Superblock *superblock)
{
    const uint32_t per_block = superblock->block_size / superblock->inode_size;

    return Ext2DivideRoundingUp(superblock->inodes_per_group, per_block);
}

uint32_t Ext2GroupFirstBlock(const Ext2Superblock *superblock, uint32_t group)
{
    return superblock->first_data_block + (group * superblock->blocks_per_group);
}

uint32_t Ext2GroupBlockCount(const Ext2Superblock *superblock, uint32_t group)
{
    const uint32_t first = Ext2GroupFirstBlock(superblock, group);
    const uint32_t remaining = superblock->block_count - first;

    /*
     * Every group holds s_blocks_per_group blocks except the last, which holds
     * whatever remains. The division that fixes the group count rounds upward,
     * so the last group is short whenever the volume is not an exact multiple of
     * the group size, which is the usual case rather than the exception.
     */
    return (remaining < superblock->blocks_per_group) ? remaining
                                                      : superblock->blocks_per_group;
}

bool Ext2ReadGroupDescriptor(BlockDevice *device, const Ext2Superblock *superblock,
                             uint32_t group, Ext2GroupDescriptor *descriptor)
{
    uint8_t raw[EXT2_GROUP_DESCRIPTOR_SIZE];
    Ext2GroupDescriptor parsed;
    uint32_t table;
    uint32_t position;
    uint32_t inode_table_blocks;
    uint32_t group_blocks;

    if ((device == NULL) || (superblock == NULL) || (descriptor == NULL))
    {
        return Ext2GroupRefuse("no device, no volume, or nowhere to put the descriptor");
    }

    if (group >= superblock->group_count)
    {
        return Ext2GroupRefuse("the volume holds no such group");
    }

    /*
     * The table may occupy several blocks, so the descriptor is located by its
     * position within the table and not within one block of it.
     */
    table = Ext2GroupDescriptorBlock(superblock);
    position = group * EXT2_GROUP_DESCRIPTOR_SIZE;

    if ((table + Ext2GroupDescriptorBlocks(superblock)) > superblock->block_count)
    {
        return Ext2GroupRefuse("the descriptor table does not fit within the volume");
    }

    if (!Ext2ReadBytes(device, superblock, table + (position / superblock->block_size),
                       position % superblock->block_size, EXT2_GROUP_DESCRIPTOR_SIZE, raw))
    {
        /* Ext2ReadBytes has already recorded the reason and counted the
         * refusal; counting it twice would say two things went wrong. */
        return false;
    }

    parsed.group = group;
    parsed.block_bitmap = Ext2ReadWord(raw, EXT2_OFFSET_BG_BLOCK_BITMAP);
    parsed.inode_bitmap = Ext2ReadWord(raw, EXT2_OFFSET_BG_INODE_BITMAP);
    parsed.inode_table = Ext2ReadWord(raw, EXT2_OFFSET_BG_INODE_TABLE);
    parsed.free_block_count = Ext2ReadHalf(raw, EXT2_OFFSET_BG_FREE_BLOCKS);
    parsed.free_inode_count = Ext2ReadHalf(raw, EXT2_OFFSET_BG_FREE_INODES);
    parsed.used_directory_count = Ext2ReadHalf(raw, EXT2_OFFSET_BG_USED_DIRECTORIES);

    if (!Ext2BlockExists(superblock, parsed.block_bitmap) ||
        !Ext2BlockExists(superblock, parsed.inode_bitmap) ||
        !Ext2BlockExists(superblock, parsed.inode_table))
    {
        return Ext2GroupRefuse("a structure of the group lies outside the volume");
    }

    /*
     * The three structures are each at least one block long, so no two of them
     * can begin upon the same block. A descriptor read four bytes adrift yields
     * two identical pointers far more often than it yields three plausible ones,
     * which is why this is asserted rather than assumed.
     */
    if ((parsed.block_bitmap == parsed.inode_bitmap) ||
        (parsed.block_bitmap == parsed.inode_table) ||
        (parsed.inode_bitmap == parsed.inode_table))
    {
        return Ext2GroupRefuse("two structures of the group begin upon the same block");
    }

    inode_table_blocks = Ext2InodeTableBlocks(superblock);

    if ((superblock->block_count - parsed.inode_table) < inode_table_blocks)
    {
        return Ext2GroupRefuse("the inode table runs past the end of the volume");
    }

    group_blocks = Ext2GroupBlockCount(superblock, group);

    if (((uint32_t)parsed.free_block_count > group_blocks) ||
        ((uint32_t)parsed.free_inode_count > superblock->inodes_per_group))
    {
        return Ext2GroupRefuse("the group reports more free than it holds");
    }

    /*
     * A directory occupies an inode that is in use, so the directories cannot
     * outnumber the inodes of the group that are not free.
     */
    if ((uint32_t)parsed.used_directory_count >
        (superblock->inodes_per_group - (uint32_t)parsed.free_inode_count))
    {
        return Ext2GroupRefuse("the group holds more directories than used inodes");
    }

    *descriptor = parsed;
    ++Ext2GroupsReadCount;
    return true;
}

bool Ext2VerifyGroupDescriptors(BlockDevice *device, const Ext2Superblock *superblock)
{
    uint64_t free_blocks = 0U;
    uint64_t free_inodes = 0U;

    if ((device == NULL) || (superblock == NULL))
    {
        return Ext2GroupRefuse("no device or no volume");
    }

    for (uint32_t group = 0U; group < superblock->group_count; ++group)
    {
        Ext2GroupDescriptor descriptor;

        if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
        {
            return false;
        }

        free_blocks += (uint64_t)descriptor.free_block_count;
        free_inodes += (uint64_t)descriptor.free_inode_count;
    }

    /*
     * The groups account for every free block and every free inode of the
     * volume, so their sums must equal the totals the superblock states. This is
     * the strongest statement that can be made about the table as a whole
     * without reading the bitmaps: a table read at the wrong offset, or one
     * descriptor short, yields descriptors that are individually plausible and a
     * sum that is not.
     *
     * It holds only of a volume that was cleanly unmounted. A volume that was
     * not is permitted to disagree with itself — that disagreement is what the
     * state means — and Ext2ReadSuperblock has already made it read-only.
     */
    if (superblock->state == EXT2_VALID_FS)
    {
        if ((free_blocks != (uint64_t)superblock->free_block_count) ||
            (free_inodes != (uint64_t)superblock->free_inode_count))
        {
            return Ext2GroupRefuse("the groups do not account for the volume's free space");
        }
    }

    return true;
}

uint64_t Ext2GroupsRead(void)
{
    return Ext2GroupsReadCount;
}

uint64_t Ext2GroupsRefused(void)
{
    return Ext2GroupsRefusedCount;
}

void Ext2ReportGroup(const Ext2GroupDescriptor *descriptor)
{
    if (descriptor == NULL)
    {
        return;
    }

    KernelWriteString("EXT2 group ");
    KernelWriteDecimal((uint64_t)descriptor->group);
    KernelWriteString(": block bitmap at ");
    KernelWriteDecimal((uint64_t)descriptor->block_bitmap);
    KernelWriteString(", inode bitmap at ");
    KernelWriteDecimal((uint64_t)descriptor->inode_bitmap);
    KernelWriteString(", inode table at ");
    KernelWriteDecimal((uint64_t)descriptor->inode_table);
    KernelWriteString("; ");
    KernelWriteDecimal((uint64_t)descriptor->free_block_count);
    KernelWriteString(" free blocks, ");
    KernelWriteDecimal((uint64_t)descriptor->free_inode_count);
    KernelWriteString(" free inodes, ");
    KernelWriteDecimal((uint64_t)descriptor->used_directory_count);
    KernelWriteString(" directories.\n");
}

const char *Ext2LastError(void)
{
    return Ext2Error;
}

uint64_t Ext2VolumesRead(void)
{
    return Ext2Read;
}

uint64_t Ext2VolumesRefused(void)
{
    return Ext2Refused;
}

void Ext2ReportVolume(const Ext2Superblock *superblock, const char *name)
{
    if (superblock == NULL)
    {
        return;
    }

    KernelWriteString("EXT2 volume upon ");
    KernelWriteString((name != NULL) ? name : "an unnamed device");
    KernelWriteString(": revision ");
    KernelWriteDecimal((uint64_t)superblock->revision);
    KernelWriteString(".");
    KernelWriteDecimal((uint64_t)superblock->minor_revision);

    if (superblock->volume_name[0] != '\0')
    {
        KernelWriteString(", labelled ");
        KernelWriteString(superblock->volume_name);
    }

    KernelWriteString(superblock->read_only ? ", read-only.\n" : ", writable.\n");

    KernelWriteString("EXT2 volume: ");
    KernelWriteDecimal((uint64_t)superblock->block_count);
    KernelWriteString(" blocks of ");
    KernelWriteDecimal((uint64_t)superblock->block_size);
    KernelWriteString(" bytes (");
    KernelWriteDecimal(((uint64_t)superblock->block_count * superblock->block_size) / 1024U);
    KernelWriteString(" KiB), ");
    KernelWriteDecimal((uint64_t)superblock->free_block_count);
    KernelWriteString(" free; ");
    KernelWriteDecimal((uint64_t)superblock->inode_count);
    KernelWriteString(" inodes of ");
    KernelWriteDecimal((uint64_t)superblock->inode_size);
    KernelWriteString(" bytes, ");
    KernelWriteDecimal((uint64_t)superblock->free_inode_count);
    KernelWriteString(" free.\n");

    KernelWriteString("EXT2 volume: ");
    KernelWriteDecimal((uint64_t)superblock->group_count);
    KernelWriteString(" groups of ");
    KernelWriteDecimal((uint64_t)superblock->blocks_per_group);
    KernelWriteString(" blocks and ");
    KernelWriteDecimal((uint64_t)superblock->inodes_per_group);
    KernelWriteString(" inodes, first data block ");
    KernelWriteDecimal((uint64_t)superblock->first_data_block);
    KernelWriteString(", first usable inode ");
    KernelWriteDecimal((uint64_t)superblock->first_inode);
    KernelWriteString(".\n");

    KernelWriteString("EXT2 volume: features compatible ");
    KernelWriteHexadecimal((uint64_t)superblock->feature_compatible);
    KernelWriteString(", incompatible ");
    KernelWriteHexadecimal((uint64_t)superblock->feature_incompatible);
    KernelWriteString(", read-only ");
    KernelWriteHexadecimal((uint64_t)superblock->feature_read_only);
    KernelWriteString(", state ");
    KernelWriteString((superblock->state == EXT2_VALID_FS) ? "clean" : "not cleanly unmounted");
    KernelWriteString(".\n");
}
