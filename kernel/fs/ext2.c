/*
 * File: kernel/fs/ext2.c
 * Purpose: Implements the reading and validation of an EXT2 superblock: the
 *          decoding of the volume's fields from their stored order into the
 *          processor's, the derivation of the geometry, and the judgement of
 *          whether the volume may be read, may be written, or may not be
 *          addressed at all.
 * Key functions: Ext2ReadSuperblock, Ext2GroupCount, Ext2ReadGroupDescriptor,
 *          Ext2VerifyGroupDescriptors, Ext2ReadInode, Ext2InodeBlock,
 *          Ext2ReadFile, Ext2ReadSymbolicLink, Ext2InodeIsFastSymbolicLink,
 *          Ext2DirectoryOpen, Ext2DirectoryNext, Ext2DirectoryFind,
 *          Ext2ResolvePath, Ext2ResolvePathNoFollow, Ext2FileTypeOfMode,
 *          Ext2LastError, Ext2ReportVolume, Ext2ReportGroup, Ext2ReportInode,
 *          Ext2ReportDirectory.
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
 *   - The same, Locating an Inode: the group holding an inode is
 *     (inode - 1) / s_inodes_per_group and its index within that group's table is
 *     (inode - 1) % s_inodes_per_group, inode numbers beginning at one and
 *     indices at zero. The worked values of Table 3.20 were used to check the
 *     arithmetic.
 *   - The same, Table 3.13 and the description of i_block: an inode is 128 bytes
 *     of defined structure; i_block at offset 40 holds fifteen block numbers, the
 *     first twelve direct, the thirteenth indirect, the fourteenth doubly
 *     indirect and the fifteenth triply indirect; a zero entry denotes a block
 *     that is not allocated rather than the end of the file.
 *   - The same, i_size: upon a revision 1 volume the high 32 bits of a regular
 *     file's size are held in the field otherwise called i_dir_acl, at offset
 *     108.
 *   - The same, the Directory Structure chapter and Table 4.1: a directory is a
 *     file whose data is a linked list of entries; the inode number lies at
 *     offset 0 and is zero where the entry is not in use, the record length at
 *     4, the name length at 6 and the file type at 7, the name following at 8.
 *     A record length must be at least the length of its record, must be a
 *     multiple of four, and no entry may span two blocks; the name length may
 *     never exceed the record length less eight.
 *   - The same, Table 4.2: the eight values a file type may take, which are not
 *     numbered as the formats of i_mode are and must agree with them.
 *   - Linux kernel documentation, filesystems/ext2.rst: the file type is an
 *     incompatible feature because a kernel unaware of it would read the name
 *     length as sixteen bits; which of the two readings applies is therefore a
 *     property of the feature flag and not of the revision.
 *   - The same, the Symbolic Links chapter: a symbolic link holds a text string
 *     interpreted as a path to another file; for a target shorter than 60 bytes
 *     the string is stored within the inode itself, in the fields that would
 *     otherwise hold the pointers to its data blocks, which avoids allocating a
 *     whole block for a string most links are shorter than.
 *   - Linux kernel documentation, the ext4 superblock and block group descriptor
 *     tables, consulted as an independent statement of the same offsets.
 */

#include <oxys/ext2.h>
#include <oxys/buffer.h>
#include <oxys/kernel.h>

/* The greatest value of s_log_block_size this kernel will accept. */
#define EXT2_MAXIMUM_LOG_BLOCK_SIZE 2U

/* How many entries of a directory a report writes out before it summarises. */
#define EXT2_REPORTED_ENTRIES 16U

/* A description of the most recent refusal, and the accounting. */
static const char *Ext2Error = "none";
static uint64_t Ext2Read;
static uint64_t Ext2Refused;
static uint64_t Ext2GroupsReadCount;
static uint64_t Ext2GroupsRefusedCount;
static uint64_t Ext2InodesReadCount;
static uint64_t Ext2InodesRefusedCount;
static uint64_t Ext2EntriesReadCount;
static uint64_t Ext2EntriesRefusedCount;
static uint64_t Ext2PathsResolvedCount;
static uint64_t Ext2PathsRefusedCount;
static uint64_t Ext2FilesReadCount;
static uint64_t Ext2BytesReadCount;
static uint64_t Ext2ReadsRefusedCount;

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

/* The same again for an inode, counted apart from both. */
static bool Ext2InodeRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2InodesRefusedCount;
    return false;
}

/* The same for a directory entry whose record contradicts the format. */
static bool Ext2EntryRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2EntriesRefusedCount;
    return false;
}

/*
 * The same for a lookup or a path that resolved to nothing.
 *
 * This is counted apart from the entry refusals above because the two are not
 * the same kind of event and only one of them is a fault. A path that names no
 * file is an ordinary answer to an ordinary question, and it will be the common
 * case once a shell is asking; an entry that contradicts the format is a volume
 * that cannot be trusted. A single counter would report their sum and so would
 * report neither.
 */
static bool Ext2PathRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2PathsRefusedCount;
    return false;
}

/* The same for a read of a file's contents, counted apart from the rest. */
static bool Ext2ReadRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2ReadsRefusedCount;
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

/*
 * The number of block pointers one block holds. Every level of the indirection
 * branches by this, and the file block index is decomposed in terms of it.
 */
static uint32_t Ext2PointersPerBlock(const Ext2Superblock *superblock)
{
    return superblock->block_size / EXT2_BLOCK_POINTER_SIZE;
}

/*
 * Reads one entry of a block of pointers.
 *
 * A table block of zero is a hole occupying the whole subtree beneath it: the
 * pointer block was never allocated, so none of the blocks it would have named
 * exist, and every entry of it reads as zero. Returning zero rather than
 * refusing is what makes a sparse file readable, and it is the reason this is
 * one function rather than a check repeated at each of the three levels.
 */
static bool Ext2ReadPointer(BlockDevice *device, const Ext2Superblock *superblock,
                            uint32_t table, uint64_t entry, uint32_t *block)
{
    uint8_t raw[EXT2_BLOCK_POINTER_SIZE];

    if (table == 0U)
    {
        *block = 0U;
        return true;
    }

    if (!Ext2BlockExists(superblock, table))
    {
        return Ext2InodeRefuse("a block of pointers lies outside the volume");
    }

    if (!Ext2ReadBytes(device, superblock, table,
                       (uint32_t)(entry * EXT2_BLOCK_POINTER_SIZE), EXT2_BLOCK_POINTER_SIZE,
                       raw))
    {
        return false;
    }

    *block = Ext2ReadWord(raw, 0U);

    if ((*block != 0U) && !Ext2BlockExists(superblock, *block))
    {
        return Ext2InodeRefuse("a block pointer addresses a block the volume does not hold");
    }

    return true;
}

bool Ext2InodeBlock(BlockDevice *device, const Ext2Superblock *superblock,
                    const Ext2Inode *inode, uint64_t index, uint32_t *block)
{
    uint64_t per_block;
    uint64_t remaining;
    uint32_t level;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (block == NULL))
    {
        return Ext2InodeRefuse("no device, no volume, no inode, or nowhere to put the block");
    }

    if (index < EXT2_DIRECT_BLOCK_COUNT)
    {
        *block = inode->block[index];
        return true;
    }

    per_block = (uint64_t)Ext2PointersPerBlock(superblock);
    remaining = index - EXT2_DIRECT_BLOCK_COUNT;

    /*
     * The three indirect entries address per_block, per_block squared and
     * per_block cubed blocks in turn. The index is reduced by each range it lies
     * beyond, so that what remains is the offset within the range it lies in;
     * the level is then the number of pointer blocks that must be walked.
     */
    if (remaining < per_block)
    {
        level = 1U;
    }
    else
    {
        remaining -= per_block;

        if (remaining < (per_block * per_block))
        {
            level = 2U;
        }
        else
        {
            remaining -= per_block * per_block;

            if (remaining < (per_block * per_block * per_block))
            {
                level = 3U;
            }
            else
            {
                return Ext2InodeRefuse("the block index is beyond what an inode can address");
            }
        }
    }

    /*
     * The walk begins at the entry of i_block for the level and descends,
     * dividing the offset by the span of one entry at each step. The span of an
     * entry at the deepest level is one block, so the last step indexes directly.
     */
    *block = inode->block[EXT2_INDIRECT_INDEX + (level - 1U)];

    while (level > 0U)
    {
        uint64_t span = 1U;

        for (uint32_t power = 1U; power < level; ++power)
        {
            span *= per_block;
        }

        if (!Ext2ReadPointer(device, superblock, *block, remaining / span, block))
        {
            return false;
        }

        remaining %= span;
        --level;
    }

    return true;
}

uint64_t Ext2InodeBlockCount(const Ext2Superblock *superblock, const Ext2Inode *inode)
{
    if ((superblock == NULL) || (inode == NULL))
    {
        return 0U;
    }

    return (inode->size + (uint64_t)superblock->block_size - 1U) /
           (uint64_t)superblock->block_size;
}

bool Ext2InodeIsDirectory(const Ext2Inode *inode)
{
    return (inode != NULL) && ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
}

bool Ext2InodeIsRegular(const Ext2Inode *inode)
{
    return (inode != NULL) && ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG);
}

bool Ext2InodeIsSymbolicLink(const Ext2Inode *inode)
{
    return (inode != NULL) && ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFLNK);
}

bool Ext2ReadInode(BlockDevice *device, const Ext2Superblock *superblock, uint32_t number,
                   Ext2Inode *inode)
{
    uint8_t raw[EXT2_GOOD_OLD_INODE_SIZE];
    Ext2GroupDescriptor descriptor;
    Ext2Inode parsed;
    uint32_t group;
    uint32_t index;
    uint32_t position;
    bool holds_target;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL))
    {
        return Ext2InodeRefuse("no device, no volume, or nowhere to put the inode");
    }

    /*
     * Inode numbers begin at one, and the volume holds s_inodes_count of them.
     * Zero is not an inode at all: a directory entry bearing it names nothing,
     * which is how a deleted entry is recorded.
     */
    if ((number == 0U) || (number > superblock->inode_count))
    {
        return Ext2InodeRefuse("the volume holds no inode of that number");
    }

    group = (number - 1U) / superblock->inodes_per_group;
    index = (number - 1U) % superblock->inodes_per_group;

    if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    /*
     * The inode lies at index * s_inode_size within the group's table. The
     * superblock has already been made to state an inode size that is a power of
     * two no larger than a block, so a whole number of inodes occupies a block
     * and the 128 bytes read below never straddle two.
     */
    position = index * superblock->inode_size;

    if (!Ext2ReadBytes(device, superblock,
                       descriptor.inode_table + (position / superblock->block_size),
                       position % superblock->block_size, EXT2_GOOD_OLD_INODE_SIZE, raw))
    {
        return false;
    }

    parsed.number = number;
    parsed.mode = Ext2ReadHalf(raw, EXT2_OFFSET_I_MODE);
    parsed.uid = Ext2ReadHalf(raw, EXT2_OFFSET_I_UID);
    parsed.gid = Ext2ReadHalf(raw, EXT2_OFFSET_I_GID);
    parsed.access_time = Ext2ReadWord(raw, EXT2_OFFSET_I_ATIME);
    parsed.change_time = Ext2ReadWord(raw, EXT2_OFFSET_I_CTIME);
    parsed.modify_time = Ext2ReadWord(raw, EXT2_OFFSET_I_MTIME);
    parsed.delete_time = Ext2ReadWord(raw, EXT2_OFFSET_I_DTIME);
    parsed.link_count = Ext2ReadHalf(raw, EXT2_OFFSET_I_LINKS_COUNT);
    parsed.sector_count = Ext2ReadWord(raw, EXT2_OFFSET_I_BLOCKS);
    parsed.flags = Ext2ReadWord(raw, EXT2_OFFSET_I_FLAGS);
    parsed.generation = Ext2ReadWord(raw, EXT2_OFFSET_I_GENERATION);
    parsed.file_acl = Ext2ReadWord(raw, EXT2_OFFSET_I_FILE_ACL);
    parsed.size = (uint64_t)Ext2ReadWord(raw, EXT2_OFFSET_I_SIZE);

    /*
     * A revision 1 volume keeps the high half of a regular file's size in the
     * field a revision 0 volume calls i_dir_acl. It is a size only for a regular
     * file: upon a directory the same bytes mean something else entirely, and a
     * kernel that joined them regardless would give a directory a size of some
     * gigabytes and read it until it fell off the volume.
     */
    if ((superblock->revision >= EXT2_DYNAMIC_REV) &&
        ((parsed.mode & EXT2_S_IFMT) == EXT2_S_IFREG))
    {
        parsed.size |= (uint64_t)Ext2ReadWord(raw, EXT2_OFFSET_I_DIR_ACL) << 32;
    }

    /*
     * The fifteen words of i_block are block pointers for every file but one: a
     * symbolic link whose target is shorter than sixty bytes holds that target
     * in them instead, which is why it needs no block at all.
     *
     * Whether this inode is such a link is decided before the words are
     * examined, because the two readings are incompatible. Read as pointers, the
     * text "sub" is the word 0x00627573 — a block number some millions beyond the
     * end of any volume this kernel composes — and validating it as one refuses
     * the inode outright. Every fast symbolic link upon every real volume would
     * be unreadable, and the diagnosis would name a block pointer that is not a
     * block pointer.
     *
     * The words are decoded either way. Ext2ReadSymbolicLink recovers the bytes
     * of the target from them, in the order the volume stores them. The decision
     * rests upon the mode, the sector count and the extended attribute block,
     * every one of which has been parsed above.
     */
    holds_target = Ext2InodeIsFastSymbolicLink(superblock, &parsed);

    for (uint32_t entry = 0U; entry < EXT2_BLOCK_POINTER_COUNT; ++entry)
    {
        parsed.block[entry] =
            Ext2ReadWord(raw, EXT2_OFFSET_I_BLOCK + (entry * EXT2_BLOCK_POINTER_SIZE));

        if (!holds_target && (parsed.block[entry] != 0U) &&
            !Ext2BlockExists(superblock, parsed.block[entry]))
        {
            return Ext2InodeRefuse("a block pointer of the inode lies outside the volume");
        }
    }

    /*
     * An inode with no format and no links is a table entry that was never
     * filled. Refusing it is how arithmetic that has strayed beyond the inode
     * table announces itself: the bytes past the table are zeroes upon a fresh
     * volume, and a kernel that accepted them would report a file of no type and
     * no blocks rather than the mistake that produced it.
     */
    if ((parsed.mode == 0U) && (parsed.link_count == 0U))
    {
        return Ext2InodeRefuse("the inode is not in use");
    }

    *inode = parsed;
    ++Ext2InodesReadCount;
    return true;
}

uint64_t Ext2InodesRead(void)
{
    return Ext2InodesReadCount;
}

uint64_t Ext2InodesRefused(void)
{
    return Ext2InodesRefusedCount;
}

void Ext2ReportInode(const Ext2Inode *inode)
{
    if (inode == NULL)
    {
        return;
    }

    KernelWriteString("EXT2 inode ");
    KernelWriteDecimal((uint64_t)inode->number);
    KernelWriteString(": mode ");
    KernelWriteHexadecimal((uint64_t)inode->mode);
    KernelWriteString(" (");

    if (Ext2InodeIsDirectory(inode))
    {
        KernelWriteString("directory");
    }
    else if (Ext2InodeIsRegular(inode))
    {
        KernelWriteString("regular file");
    }
    else if (Ext2InodeIsSymbolicLink(inode))
    {
        KernelWriteString("symbolic link");
    }
    else
    {
        KernelWriteString("other");
    }

    KernelWriteString("), ");
    KernelWriteDecimal(inode->size);
    KernelWriteString(" bytes, ");
    KernelWriteDecimal((uint64_t)inode->link_count);
    KernelWriteString(" links, ");
    KernelWriteDecimal((uint64_t)inode->sector_count);
    KernelWriteString(" sectors, first block ");
    KernelWriteDecimal((uint64_t)inode->block[0]);
    KernelWriteString(".\n");
}

/*
 * File reading.
 *
 * Everything to this point locates things: a superblock, a descriptor, an inode,
 * a block of a file, a name within a directory. This is the first that produces
 * the contents of a file, and it is the shortest piece of work in the chapter
 * precisely because the locating was done properly — the whole of it is the
 * arithmetic of a byte range against a block size, and one call per block to
 * machinery that already exists.
 */

/* Fills a run of bytes with zeroes. There is no C library until Phase 7. */
static void Ext2FillZero(uint8_t *destination, uint32_t length)
{
    for (uint32_t index = 0U; index < length; ++index)
    {
        destination[index] = 0U;
    }
}

bool Ext2ReadFile(BlockDevice *device, const Ext2Superblock *superblock,
                  const Ext2Inode *inode, uint64_t offset, void *buffer, uint64_t length,
                  uint64_t *read)
{
    uint8_t *destination = (uint8_t *)buffer;
    uint64_t remaining;
    uint64_t taken = 0U;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (read == NULL) ||
        ((buffer == NULL) && (length != 0U)))
    {
        return Ext2ReadRefuse("no device, no volume, no inode, nowhere to read into, or "
                              "nowhere to put the count");
    }

    *read = 0U;

    /*
     * A directory's bytes are entries, and are read by traversing it. A caller
     * reading them as a stream has mistaken what it holds, and would receive
     * record lengths and inode numbers as though they were text.
     */
    if (Ext2InodeIsDirectory(inode))
    {
        return Ext2ReadRefuse("a directory is traversed and not read as a stream of bytes");
    }

    /*
     * The end of the file is not a failure. A reader arrives at it by reading,
     * and a kernel that reported it as an error would oblige every caller to
     * treat the ordinary conclusion of its work as a fault; the count reports it
     * instead. An offset beyond the end is the same answer for the same reason.
     */
    if (offset >= inode->size)
    {
        return true;
    }

    remaining = inode->size - offset;

    if (remaining > length)
    {
        remaining = length;
    }

    while (remaining > 0U)
    {
        const uint64_t index = offset / (uint64_t)superblock->block_size;
        const uint32_t within = (uint32_t)(offset % (uint64_t)superblock->block_size);
        uint32_t take = superblock->block_size - within;
        uint32_t block;

        if ((uint64_t)take > remaining)
        {
            take = (uint32_t)remaining;
        }

        if (!Ext2InodeBlock(device, superblock, inode, index, &block))
        {
            *read = taken;
            return false;
        }

        /*
         * A hole reads as zeroes. The block was never allocated, so there is
         * nothing upon the volume to read and nothing is read: the file's
         * contents at that offset are zeroes by definition, not by accident, and
         * a reader cannot tell a hole from a block that was written with zeroes,
         * which is exactly the point of one.
         */
        if (block == 0U)
        {
            Ext2FillZero(destination, take);
        }
        else if (!Ext2ReadBytes(device, superblock, block, within, take, destination))
        {
            *read = taken;
            return false;
        }

        destination += take;
        offset += take;
        remaining -= take;
        taken += take;
    }

    *read = taken;
    Ext2BytesReadCount += taken;
    ++Ext2FilesReadCount;
    return true;
}

bool Ext2InodeIsFastSymbolicLink(const Ext2Superblock *superblock, const Ext2Inode *inode)
{
    uint32_t attribute_sectors;

    if ((superblock == NULL) || !Ext2InodeIsSymbolicLink(inode))
    {
        return false;
    }

    /*
     * i_blocks counts 512-byte sectors, and an extended attribute block is among
     * them although it is not data. Subtracting it leaves the sectors the file's
     * own contents occupy, and a symbolic link with none of those holds its
     * target within the inode.
     */
    attribute_sectors = (inode->file_acl != 0U) ? (superblock->block_size / 512U) : 0U;

    return inode->sector_count == attribute_sectors;
}

bool Ext2ReadSymbolicLink(BlockDevice *device, const Ext2Superblock *superblock,
                          const Ext2Inode *inode, char *target, size_t capacity)
{
    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (target == NULL) ||
        (capacity == 0U))
    {
        return Ext2ReadRefuse("no device, no volume, no inode, or nowhere to put the target");
    }

    if (!Ext2InodeIsSymbolicLink(inode))
    {
        return Ext2ReadRefuse("the inode is not a symbolic link");
    }

    /*
     * A link with no target names nothing. The format does not forbid it; it
     * cannot be resolved, and saying so here is better than resolving the empty
     * path to whatever it happens to reach.
     */
    if (inode->size == 0U)
    {
        return Ext2ReadRefuse("a symbolic link bearing no target");
    }

    if (inode->size >= (uint64_t)capacity)
    {
        return Ext2ReadRefuse("a symbolic link's target is longer than this kernel will read");
    }

    if (Ext2InodeIsFastSymbolicLink(superblock, inode))
    {
        if (inode->size > EXT2_FAST_SYMLINK_CAPACITY)
        {
            return Ext2ReadRefuse("a symbolic link holds no blocks and no room for its target");
        }

        /*
         * The target occupies the sixty bytes of i_block, and i_block was decoded
         * into fifteen words when the inode was read. The bytes are recovered
         * from those words in the order the volume stores them, least significant
         * first — the same order the decoding assumed, applied in reverse.
         */
        for (uint32_t index = 0U; index < (uint32_t)inode->size; ++index)
        {
            const uint32_t word = inode->block[index / EXT2_BLOCK_POINTER_SIZE];
            const uint32_t shift = (index % EXT2_BLOCK_POINTER_SIZE) * 8U;

            target[index] = (char)((word >> shift) & 0xFFU);
        }
    }
    else
    {
        uint64_t read = 0U;

        if (!Ext2ReadFile(device, superblock, inode, 0U, target, inode->size, &read))
        {
            return false;
        }

        if (read != inode->size)
        {
            return Ext2ReadRefuse("a symbolic link's target was read short");
        }
    }

    target[inode->size] = '\0';

    /*
     * A target holding a null byte would be a path shorter than the file says it
     * is, and everything below this point treats it as a terminated string. The
     * format permits the byte; this kernel cannot resolve what it produces.
     */
    for (uint32_t index = 0U; index < (uint32_t)inode->size; ++index)
    {
        if (target[index] == '\0')
        {
            return Ext2ReadRefuse("a symbolic link's target holds a null byte");
        }
    }

    return true;
}

uint64_t Ext2FilesRead(void)
{
    return Ext2FilesReadCount;
}

uint64_t Ext2BytesRead(void)
{
    return Ext2BytesReadCount;
}

uint64_t Ext2ReadsRefused(void)
{
    return Ext2ReadsRefusedCount;
}

/*
 * The directory.
 *
 * Everything above reads a file by its inode number; nothing above knows an
 * inode number, because a user names a file. A directory is what stands between
 * the two, and it is an ordinary file whose data happens to be a sequence of
 * entries rather than anything the format treats specially — which is why the
 * traversal below rests entirely upon Ext2InodeBlock and adds nothing to it but
 * an interpretation of the bytes.
 */

/*
 * The entry file type corresponding to a format held in i_mode.
 *
 * The two numberings are unrelated: i_mode holds the historical Unix values in
 * its high four bits, and the entry's file type is a small integer assigned in
 * an order of its own. A directory is 0x4000 in the one and 2 in the other, a
 * regular file 0x8000 and 1, and a socket 0xC000 and 6. Nothing about either
 * numbering derives from the other, so the correspondence must be written out.
 */
uint8_t Ext2FileTypeOfMode(uint16_t mode)
{
    switch (mode & EXT2_S_IFMT)
    {
    case EXT2_S_IFREG:
        return (uint8_t)EXT2_FT_REG_FILE;
    case EXT2_S_IFDIR:
        return (uint8_t)EXT2_FT_DIR;
    case EXT2_S_IFCHR:
        return (uint8_t)EXT2_FT_CHRDEV;
    case EXT2_S_IFBLK:
        return (uint8_t)EXT2_FT_BLKDEV;
    case EXT2_S_IFIFO:
        return (uint8_t)EXT2_FT_FIFO;
    case EXT2_S_IFSOCK:
        return (uint8_t)EXT2_FT_SOCK;
    case EXT2_S_IFLNK:
        return (uint8_t)EXT2_FT_SYMLINK;
    default:
        return (uint8_t)EXT2_FT_UNKNOWN;
    }
}

const char *Ext2FileTypeName(uint8_t type)
{
    switch (type)
    {
    case EXT2_FT_REG_FILE:
        return "regular file";
    case EXT2_FT_DIR:
        return "directory";
    case EXT2_FT_CHRDEV:
        return "character device";
    case EXT2_FT_BLKDEV:
        return "block device";
    case EXT2_FT_FIFO:
        return "fifo";
    case EXT2_FT_SOCK:
        return "socket";
    case EXT2_FT_SYMLINK:
        return "symbolic link";
    default:
        return "of no stated type";
    }
}

/*
 * Whether the volume states a file type in its directory entries.
 *
 * The two bytes at offset 6 are either a name length of eight bits followed by a
 * file type, or a name length of sixteen bits. Which of the two a volume holds
 * is stated by the incompatible feature flag and by nothing else — not by the
 * revision, a revision 1 volume being free to omit the feature. Reading the
 * wrong one of the two is not a subtle error: a volume without the feature,
 * read as though it had it, gives every entry a file type equal to the high byte
 * of its name length, which is zero, and so declares every file to be of no
 * stated type. Read the other way about, a name of three bytes becomes a name of
 * 3 + 256 * EXT2_FT_DIR bytes and the entry is refused.
 */
static bool Ext2VolumeStatesFileType(const Ext2Superblock *superblock)
{
    return (superblock->feature_incompatible & EXT2_FEATURE_INCOMPAT_FILETYPE) != 0U;
}

/*
 * Whether a directory's data may be traversed at all.
 *
 * A directory occupies whole blocks: the record length of the last entry of a
 * block runs to the end of that block, so a size that is not a multiple of the
 * block size describes a directory whose final block ends in the middle of an
 * entry. A directory of no size is refused likewise — every directory holds at
 * least its own entry and its parent's — and both refusals catch an inode that
 * is not really a directory long before its bytes are interpreted as entries.
 */
static bool Ext2DirectoryTraversable(const Ext2Superblock *superblock,
                                     const Ext2Inode *directory)
{
    if (!Ext2InodeIsDirectory(directory))
    {
        return Ext2EntryRefuse("the inode is not a directory");
    }

    if (directory->size == 0U)
    {
        return Ext2EntryRefuse("a directory of no size holds not even its own entry");
    }

    if ((directory->size % (uint64_t)superblock->block_size) != 0U)
    {
        return Ext2EntryRefuse("a directory's size is not a whole number of blocks");
    }

    return true;
}

/*
 * Reads and validates the eight-byte header of the entry standing at an offset
 * within a block, leaving the name unread.
 *
 * Every rule the specification states about a record is applied here, because
 * every one of them is what keeps the traversal from walking off the block or
 * looping upon itself: a record length below the header cannot be advanced past,
 * one that is not a multiple of four leaves the next entry unaligned, and one
 * that reaches beyond the block contradicts the rule that no entry spans two.
 */
static bool Ext2ReadEntryHeader(BlockDevice *device, const Ext2Superblock *superblock,
                                uint32_t block, uint32_t offset, Ext2DirectoryEntry *entry)
{
    uint8_t raw[EXT2_DIRECTORY_HEADER_SIZE];

    if (!Ext2ReadBytes(device, superblock, block, offset, EXT2_DIRECTORY_HEADER_SIZE, raw))
    {
        return false;
    }

    entry->inode = Ext2ReadWord(raw, EXT2_OFFSET_DE_INODE);
    entry->record_length = Ext2ReadHalf(raw, EXT2_OFFSET_DE_RECORD_LENGTH);

    if (Ext2VolumeStatesFileType(superblock))
    {
        entry->name_length = (uint16_t)raw[EXT2_OFFSET_DE_NAME_LENGTH];
        entry->file_type = raw[EXT2_OFFSET_DE_FILE_TYPE];
    }
    else
    {
        entry->name_length = Ext2ReadHalf(raw, EXT2_OFFSET_DE_NAME_LENGTH);
        entry->file_type = (uint8_t)EXT2_FT_UNKNOWN;
    }

    entry->block = block;
    entry->offset = offset;

    if (entry->record_length < EXT2_DIRECTORY_HEADER_SIZE)
    {
        return Ext2EntryRefuse("a directory entry is shorter than its own header");
    }

    if ((entry->record_length % EXT2_DIRECTORY_ALIGNMENT) != 0U)
    {
        return Ext2EntryRefuse("a directory entry is not a multiple of four bytes long");
    }

    if (entry->record_length > (superblock->block_size - offset))
    {
        return Ext2EntryRefuse("a directory entry reaches beyond the block that holds it");
    }

    if (entry->name_length > (entry->record_length - EXT2_DIRECTORY_HEADER_SIZE))
    {
        return Ext2EntryRefuse("a directory entry's name does not fit within it");
    }

    if (entry->name_length > EXT2_NAME_MAXIMUM)
    {
        return Ext2EntryRefuse("a directory entry's name is longer than the format permits");
    }

    /*
     * An entry in use names an inode of this volume and bears a name. An entry
     * naming inode zero is not in use and is not held to either rule: it is the
     * record left where a name was removed, and its name length is ordinarily
     * zero but need not be.
     */
    if (entry->inode != 0U)
    {
        if (entry->inode > superblock->inode_count)
        {
            return Ext2EntryRefuse("a directory entry names an inode the volume does not hold");
        }

        if (entry->name_length == 0U)
        {
            return Ext2EntryRefuse("a directory entry in use bears no name");
        }
    }

    return true;
}

/*
 * Reads the name of an entry whose header has been read and validated.
 *
 * The name is read directly into the entry rather than through a buffer of its
 * own: a name may be 255 bytes, the entry has room for it already, and the
 * kernel stack is not large enough to hold a second copy without reason. A
 * character type may alias any object, so reading bytes into the storage of a
 * char array is defined and not a pun.
 *
 * The name is then held to the two rules the resolver depends upon. Neither is
 * stated by the specification, which describes a name as bytes and attributes no
 * meaning to any of them; both are enforced because a name containing the
 * separator would be reachable by no path, and a name containing a null byte
 * would compare equal to its own prefix once terminated. A volume bearing such a
 * name is not one this kernel can address correctly, and saying so is better
 * than resolving a path to the wrong file.
 */
static bool Ext2ReadEntryName(BlockDevice *device, const Ext2Superblock *superblock,
                              Ext2DirectoryEntry *entry)
{
    if (!Ext2ReadBytes(device, superblock, entry->block,
                       entry->offset + EXT2_DIRECTORY_HEADER_SIZE, entry->name_length,
                       (uint8_t *)entry->name))
    {
        return false;
    }

    entry->name[entry->name_length] = '\0';

    for (uint16_t index = 0U; index < entry->name_length; ++index)
    {
        if ((entry->name[index] == EXT2_PATH_SEPARATOR) || (entry->name[index] == '\0'))
        {
            return Ext2EntryRefuse("a directory entry's name holds a separator or a null byte");
        }
    }

    return true;
}

void Ext2DirectoryOpen(Ext2DirectoryCursor *cursor, const Ext2Inode *directory)
{
    if (cursor == NULL)
    {
        return;
    }

    cursor->directory = directory;
    cursor->index = 0U;
    cursor->offset = 0U;
}

Ext2DirectoryStep Ext2DirectoryNext(BlockDevice *device, const Ext2Superblock *superblock,
                                    Ext2DirectoryCursor *cursor, Ext2DirectoryEntry *entry)
{
    uint64_t blocks;

    if ((device == NULL) || (superblock == NULL) || (cursor == NULL) || (entry == NULL) ||
        (cursor->directory == NULL))
    {
        (void)Ext2EntryRefuse("no device, no volume, no cursor, or nowhere to put the entry");
        return EXT2_DIRECTORY_FAILED;
    }

    if (!Ext2DirectoryTraversable(superblock, cursor->directory))
    {
        return EXT2_DIRECTORY_FAILED;
    }

    blocks = cursor->directory->size / (uint64_t)superblock->block_size;

    while (cursor->index < blocks)
    {
        uint32_t block;

        if (!Ext2InodeBlock(device, superblock, cursor->directory, cursor->index, &block))
        {
            return EXT2_DIRECTORY_FAILED;
        }

        /*
         * A block the directory never had allocated holds no entries. Reading it
         * would yield zeroes, and a record length of zero cannot be advanced
         * past; passing over the block is both the correct reading of a hole and
         * the only one that terminates.
         */
        if (block == 0U)
        {
            ++cursor->index;
            cursor->offset = 0U;
            continue;
        }

        /*
         * The records of a block run to its end, so a remainder too small to
         * hold a header is a block that does not account for itself. It is
         * refused rather than passed over: something wrote a record length that
         * stops short, and the entries beyond it are unreachable.
         */
        if (cursor->offset > (superblock->block_size - EXT2_DIRECTORY_HEADER_SIZE))
        {
            (void)Ext2EntryRefuse("a directory block ends in too little space for an entry");
            return EXT2_DIRECTORY_FAILED;
        }

        if (!Ext2ReadEntryHeader(device, superblock, block, cursor->offset, entry))
        {
            return EXT2_DIRECTORY_FAILED;
        }

        cursor->offset += entry->record_length;

        if (cursor->offset == superblock->block_size)
        {
            ++cursor->index;
            cursor->offset = 0U;
        }

        /* An entry naming no inode holds space and not a name. */
        if (entry->inode == 0U)
        {
            continue;
        }

        if (!Ext2ReadEntryName(device, superblock, entry))
        {
            return EXT2_DIRECTORY_FAILED;
        }

        ++Ext2EntriesReadCount;
        return EXT2_DIRECTORY_ENTRY_READ;
    }

    return EXT2_DIRECTORY_END;
}

bool Ext2DirectoryFind(BlockDevice *device, const Ext2Superblock *superblock,
                       const Ext2Inode *directory, const char *name, size_t length,
                       Ext2DirectoryEntry *entry)
{
    Ext2DirectoryCursor cursor;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL) ||
        (entry == NULL))
    {
        return Ext2PathRefuse("no device, no volume, no directory, or no name to look for");
    }

    if (length == 0U)
    {
        return Ext2PathRefuse("a name of no length names nothing");
    }

    if (length > EXT2_NAME_MAXIMUM)
    {
        return Ext2PathRefuse("a name longer than the format permits can be upon no volume");
    }

    Ext2DirectoryOpen(&cursor, directory);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, entry);
        bool same;

        if (step == EXT2_DIRECTORY_FAILED)
        {
            return false;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            return Ext2PathRefuse("the directory holds no entry of that name");
        }

        if (entry->name_length != (uint16_t)length)
        {
            continue;
        }

        same = true;

        for (size_t index = 0U; index < length; ++index)
        {
            if (entry->name[index] != name[index])
            {
                same = false;
                break;
            }
        }

        if (same)
        {
            return true;
        }
    }
}

/*
 * Whether a path ends in a separator, which is an assertion by the caller that
 * what it names is a directory.
 *
 * It is determined before the walk because it governs the treatment of the last
 * component: a link is not a directory, so a path ending in a separator is
 * asking for what the link names whether or not the caller asked links to be
 * followed.
 */
static bool Ext2PathAssertsDirectory(const char *path, size_t length)
{
    return (length > 0U) && (path[length - 1U] == EXT2_PATH_SEPARATOR);
}

/* The length of a path, refusing one that is not terminated within the bound. */
static bool Ext2PathLength(const char *path, size_t *length)
{
    size_t position = 0U;

    while (path[position] != '\0')
    {
        ++position;

        if (position > EXT2_PATH_MAXIMUM)
        {
            return Ext2PathRefuse("the path is longer than this kernel will resolve");
        }
    }

    *length = position;
    return true;
}

/*
 * Resolves a path against a starting directory, following symbolic links.
 *
 * The depth is the number of links already followed. A link is followed by
 * resolving its target, which re-enters this function, so the depth is what
 * bounds a link that names itself — directly, or around a cycle of several. The
 * format offers no protection against such a link and cannot: it is a valid
 * file whose contents happen to be its own name.
 *
 * A link is followed by resolving its target to an inode and continuing the
 * original path from that point, rather than by splicing the target into the
 * path and starting again. The two are equivalent, and this one needs no buffer
 * to hold the spliced path — which matters, every level of the recursion already
 * carrying a target of its own.
 */
static bool Ext2ResolveFrom(BlockDevice *device, const Ext2Superblock *superblock,
                            const Ext2Inode *start, const char *path, bool follow_last,
                            uint32_t depth, Ext2Inode *inode)
{
    Ext2DirectoryEntry entry;
    Ext2Inode current;
    Ext2Inode next;
    size_t length;
    size_t position = 0U;
    bool asserts_directory;

    if (depth > EXT2_SYMLINK_DEPTH_MAXIMUM)
    {
        return Ext2PathRefuse("too many symbolic links were followed to resolve one path");
    }

    if (!Ext2PathLength(path, &length))
    {
        return false;
    }

    if (length == 0U)
    {
        return Ext2PathRefuse("an empty path names nothing");
    }

    asserts_directory = Ext2PathAssertsDirectory(path, length);

    /*
     * An absolute target begins again at the root; a relative one continues from
     * the directory it was found in. This is the whole of the difference between
     * the two, and it is why a link must be resolved against the directory
     * holding it rather than against the root or the working directory.
     */
    if (path[0] == EXT2_PATH_SEPARATOR)
    {
        if (!Ext2ReadInode(device, superblock, EXT2_ROOT_INODE, &current))
        {
            return false;
        }
    }
    else if (start != NULL)
    {
        current = *start;
    }
    else
    {
        return Ext2PathRefuse("the path is not absolute");
    }

    if (!Ext2InodeIsDirectory(&current))
    {
        return Ext2PathRefuse("a path is resolved against something that is not a directory");
    }

    for (;;)
    {
        size_t start_of_component;
        size_t component_length;
        bool last;

        /* Consecutive separators are one separator, and a path may end in them. */
        while (path[position] == EXT2_PATH_SEPARATOR)
        {
            ++position;
        }

        if (path[position] == '\0')
        {
            break;
        }

        start_of_component = position;

        while ((path[position] != '\0') && (path[position] != EXT2_PATH_SEPARATOR))
        {
            ++position;
        }

        component_length = position - start_of_component;

        /*
         * Whether this is the last component, which governs whether a link
         * standing here is followed. A separator after it does not make it any
         * less the last: "/link/" names the last component still, and asserts
         * that it is a directory.
         */
        last = true;

        for (size_t look = position; path[look] != '\0'; ++look)
        {
            if (path[look] != EXT2_PATH_SEPARATOR)
            {
                last = false;
                break;
            }
        }

        /*
         * Only a directory holds names. Refusing here rather than within the
         * lookup distinguishes the two failures a caller cares about: a path
         * whose components do not exist, and a path that treats a file as though
         * it were a directory.
         */
        if (!Ext2InodeIsDirectory(&current))
        {
            return Ext2PathRefuse("a component of the path is not a directory");
        }

        if (!Ext2DirectoryFind(device, superblock, &current, &path[start_of_component],
                               component_length, &entry))
        {
            return false;
        }

        if (!Ext2ReadInode(device, superblock, entry.inode, &next))
        {
            return false;
        }

        /*
         * The specification requires the file type of an entry to match the
         * format of the inode it names. The two are written at different times
         * by different code, and a volume upon which they disagree is one whose
         * directories and inodes no longer describe the same filesystem; the
         * check costs nothing here, the inode having just been read.
         *
         * A volume that states no file type declares EXT2_FT_UNKNOWN for every
         * entry, and there is nothing to check.
         */
        if ((entry.file_type != (uint8_t)EXT2_FT_UNKNOWN) &&
            (entry.file_type != Ext2FileTypeOfMode(next.mode)))
        {
            return Ext2PathRefuse("a directory entry's file type contradicts its inode");
        }

        /*
         * A link within the path must be followed for the rest of the path to
         * mean anything. A link as the last component is followed only if the
         * caller asked for the file rather than for its name — or if the path
         * asserts a directory, a link not being one.
         */
        if (Ext2InodeIsSymbolicLink(&next) && (!last || follow_last || asserts_directory))
        {
            char target[EXT2_SYMLINK_MAXIMUM + 1U];
            Ext2Inode resolved;

            if (!Ext2ReadSymbolicLink(device, superblock, &next, target, sizeof target))
            {
                return false;
            }

            if (!Ext2ResolveFrom(device, superblock, &current, target, true, depth + 1U,
                                 &resolved))
            {
                return false;
            }

            next = resolved;
        }

        current = next;
    }

    /*
     * A path written with a trailing separator asserts that what it names is a
     * directory. The assertion is the caller's and is honoured: "/etc/" names a
     * directory or it names nothing.
     */
    if (asserts_directory && !Ext2InodeIsDirectory(&current))
    {
        return Ext2PathRefuse("the path ends in a separator but does not name a directory");
    }

    *inode = current;
    return true;
}

bool Ext2ResolvePath(BlockDevice *device, const Ext2Superblock *superblock, const char *path,
                     Ext2Inode *inode)
{
    if ((device == NULL) || (superblock == NULL) || (path == NULL) || (inode == NULL))
    {
        return Ext2PathRefuse("no device, no volume, no path, or nowhere to put the inode");
    }

    /*
     * Only an absolute path is resolved here. A relative one is resolved against
     * a working directory, which is a property of a process and not of a volume,
     * and there are no processes until Phase 6. Ext2ResolveFrom accepts a
     * relative path because a symbolic link's target may be one, and is resolved
     * against the directory holding the link.
     */
    if (path[0] != EXT2_PATH_SEPARATOR)
    {
        return Ext2PathRefuse("the path is not absolute");
    }

    if (!Ext2ResolveFrom(device, superblock, NULL, path, true, 0U, inode))
    {
        return false;
    }

    ++Ext2PathsResolvedCount;
    return true;
}

bool Ext2ResolvePathNoFollow(BlockDevice *device, const Ext2Superblock *superblock,
                             const char *path, Ext2Inode *inode)
{
    if ((device == NULL) || (superblock == NULL) || (path == NULL) || (inode == NULL))
    {
        return Ext2PathRefuse("no device, no volume, no path, or nowhere to put the inode");
    }

    if (path[0] != EXT2_PATH_SEPARATOR)
    {
        return Ext2PathRefuse("the path is not absolute");
    }

    if (!Ext2ResolveFrom(device, superblock, NULL, path, false, 0U, inode))
    {
        return false;
    }

    ++Ext2PathsResolvedCount;
    return true;
}

uint64_t Ext2EntriesRead(void)
{
    return Ext2EntriesReadCount;
}

uint64_t Ext2EntriesRefused(void)
{
    return Ext2EntriesRefusedCount;
}

uint64_t Ext2PathsResolved(void)
{
    return Ext2PathsResolvedCount;
}

uint64_t Ext2PathsRefused(void)
{
    return Ext2PathsRefusedCount;
}

void Ext2ReportDirectoryEntry(const Ext2DirectoryEntry *entry)
{
    if (entry == NULL)
    {
        return;
    }

    KernelWriteString("EXT2 entry: inode ");
    KernelWriteDecimal((uint64_t)entry->inode);
    KernelWriteString(", ");
    KernelWriteString(Ext2FileTypeName(entry->file_type));
    KernelWriteString(", ");
    KernelWriteDecimal((uint64_t)entry->record_length);
    KernelWriteString(" bytes at block ");
    KernelWriteDecimal((uint64_t)entry->block);
    KernelWriteString(" offset ");
    KernelWriteDecimal((uint64_t)entry->offset);
    KernelWriteString(": ");
    KernelWriteString(entry->name);
    KernelWriteString("\n");
}

void Ext2ReportDirectory(BlockDevice *device, const Ext2Superblock *superblock,
                         const Ext2Inode *directory)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;
    uint64_t counted = 0U;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL))
    {
        return;
    }

    Ext2DirectoryOpen(&cursor, directory);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, &entry);

        if (step == EXT2_DIRECTORY_FAILED)
        {
            KernelWriteString("EXT2 directory ");
            KernelWriteDecimal((uint64_t)directory->number);
            KernelWriteString(" could not be read: ");
            KernelWriteString(Ext2LastError());
            KernelWriteString("\n");
            return;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            break;
        }

        if (counted < EXT2_REPORTED_ENTRIES)
        {
            Ext2ReportDirectoryEntry(&entry);
        }

        ++counted;
    }

    KernelWriteString("EXT2 directory ");
    KernelWriteDecimal((uint64_t)directory->number);
    KernelWriteString(" holds ");
    KernelWriteDecimal(counted);
    KernelWriteString(" entries.\n");
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
