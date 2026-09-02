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
 *          Ext2WriteSuperblock, Ext2WriteGroupDescriptor, Ext2WriteInode,
 *          Ext2AllocateBlock, Ext2FreeBlock, Ext2AllocateInode, Ext2FreeInode,
 *          Ext2WriteFile, Ext2TruncateFile, Ext2DirectoryInsert,
 *          Ext2DirectoryRemove, Ext2DirectoryIsEmpty, Ext2CreateFile,
 *          Ext2CreateDirectory, Ext2Link, Ext2Unlink, Ext2RemoveDirectory,
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
 *   - The same, the Block Bitmap and Inode Bitmap chapters: one bit per block or
 *     inode of the group, 1 meaning used, the first of the group being bit 0 of
 *     byte 0 and the ninth bit 0 of byte 1; the inode bitmap begins at inode 1.
 *   - The same, the rec_len note: where a record is removed the record before it
 *     is lengthened to cover it, and where the first record of a block is
 *     removed a blank record is left in its place.
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
static uint64_t Ext2BlocksAllocatedCount;
static uint64_t Ext2BlocksFreedCount;
static uint64_t Ext2InodesAllocatedCount;
static uint64_t Ext2InodesFreedCount;
static uint64_t Ext2BytesWrittenCount;
static uint64_t Ext2WritesRefusedCount;
static uint64_t Ext2NamesInsertedCount;
static uint64_t Ext2NamesRemovedCount;
static uint64_t Ext2FilesCreatedCount;
static uint64_t Ext2FilesDestroyedCount;

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
 * The same for anything that would alter a volume.
 *
 * Counted apart from every other refusal because it is the only one that
 * describes something not done to somebody's data. A rising count of these is
 * the sign of a volume being written by a kernel that should not be writing it.
 */
static bool Ext2WriteRefuse(const char *reason)
{
    Ext2Error = reason;
    ++Ext2WritesRefusedCount;
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

    /*
     * An inode bearing no names and a deletion time is a file that was
     * destroyed. Its mode and its block pointers are still what they were —
     * nothing overwrites them, and a recovery tool reads them for exactly that
     * reason — so nothing here distinguishes it from a live file but this.
     *
     * It is refused because no name leads to it, so nothing above has any lawful
     * way to reach it: an attempt to read one means a directory entry survives
     * that should not, and the blocks it names have been given to somebody else.
     * Reading it would serve another file's data under the dead file's name.
     */
    if ((parsed.link_count == 0U) && (parsed.delete_time != 0U))
    {
        return Ext2InodeRefuse("the inode names a file that was deleted");
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

/*
 * Writing.
 *
 * The disciplines this whole section is held to are stated at the head of the
 * writing declarations in oxys/ext2.h and are not repeated here. What follows is
 * their machinery.
 */

/* The encoders, the exact inverses of Ext2ReadHalf and Ext2ReadWord. */
static void Ext2WriteHalf(uint8_t *raw, size_t offset, uint16_t value)
{
    raw[offset] = (uint8_t)(value & 0xFFU);
    raw[offset + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}

static void Ext2WriteWord(uint8_t *raw, size_t offset, uint32_t value)
{
    raw[offset] = (uint8_t)(value & 0xFFU);
    raw[offset + 1U] = (uint8_t)((value >> 8) & 0xFFU);
    raw[offset + 2U] = (uint8_t)((value >> 16) & 0xFFU);
    raw[offset + 3U] = (uint8_t)((value >> 24) & 0xFFU);
}

/*
 * Whether a volume may be altered at all.
 *
 * Asked before anything else by every function that writes, so that a volume
 * this kernel judged unsafe to write is refused once, in one place, rather than
 * by each caller remembering to ask.
 */
static bool Ext2Writable(const Ext2Superblock *superblock)
{
    if (superblock->read_only)
    {
        return Ext2WriteRefuse("the volume is read-only and may not be altered");
    }

    return true;
}

/*
 * Writes a run of bytes into one block of the volume, through the buffer cache,
 * and marks the buffer dirty.
 *
 * The mirror of Ext2ReadBytes, and bounded by the same two tests. It writes only
 * the bytes it is given: the rest of the block is whatever it held, which is
 * what allows a structure to be altered without destroying the fields around it
 * that this kernel does not parse.
 */
static bool Ext2WriteBytes(BlockDevice *device, const Ext2Superblock *superblock,
                           uint32_t block, uint32_t offset, uint32_t length,
                           const uint8_t *source)
{
    uint64_t position;

    if (block >= superblock->block_count)
    {
        return Ext2WriteRefuse("a write was attempted beyond the end of the volume");
    }

    if ((offset > superblock->block_size) || (length > (superblock->block_size - offset)))
    {
        return Ext2WriteRefuse("a write was attempted beyond the end of a block");
    }

    position = ((uint64_t)block * superblock->block_size) + offset;

    while (length > 0U)
    {
        const uint64_t device_block = position / device->block_size;
        const uint32_t within = (uint32_t)(position % device->block_size);
        uint32_t put = device->block_size - within;
        Buffer *buffer;

        if (put > length)
        {
            put = length;
        }

        buffer = BufferGet(device, device_block);

        if (buffer == NULL)
        {
            return Ext2WriteRefuse("a block of the volume could not be read to be written");
        }

        for (uint32_t index = 0U; index < put; ++index)
        {
            buffer->data[within + index] = source[index];
        }

        BufferMarkDirty(buffer);
        BufferRelease(buffer);

        source += put;
        position += put;
        length -= put;
    }

    return true;
}

/* Fills a whole block of the volume with zeroes, a block being handed out with
 * whatever it last held otherwise. */
static bool Ext2ZeroBlock(BlockDevice *device, const Ext2Superblock *superblock,
                          uint32_t block)
{
    uint8_t zeroes[EXT2_MAXIMUM_BLOCK_SIZE];

    Ext2FillZero(zeroes, superblock->block_size);

    return Ext2WriteBytes(device, superblock, block, 0U, superblock->block_size, zeroes);
}

bool Ext2WriteSuperblock(BlockDevice *device, const Ext2Superblock *superblock)
{
    uint8_t raw[EXT2_SUPERBLOCK_SIZE];

    if ((device == NULL) || (superblock == NULL))
    {
        return Ext2WriteRefuse("no device or no volume to write");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    /*
     * The superblock is read before it is written, and only the fields this
     * kernel maintains are altered within it. Composing 1024 bytes from the
     * parsed structure would write zeroes over every field this kernel does not
     * parse — the journal identifiers, the hash seed, the mount options — and
     * would present the volume to the system that made it as though those fields
     * had never been set.
     */
    if (!Ext2ReadSuperblockBytes(device, raw))
    {
        return false;
    }

    Ext2WriteWord(raw, EXT2_OFFSET_FREE_BLOCKS, superblock->free_block_count);
    Ext2WriteWord(raw, EXT2_OFFSET_FREE_INODES, superblock->free_inode_count);
    Ext2WriteHalf(raw, EXT2_OFFSET_STATE, superblock->state);
    Ext2WriteWord(raw, EXT2_OFFSET_WRITE_TIME, superblock->write_time);

    return Ext2WriteBytes(device, superblock,
                          EXT2_SUPERBLOCK_OFFSET / superblock->block_size,
                          EXT2_SUPERBLOCK_OFFSET % superblock->block_size,
                          EXT2_SUPERBLOCK_SIZE, raw);
}

bool Ext2WriteGroupDescriptor(BlockDevice *device, const Ext2Superblock *superblock,
                              const Ext2GroupDescriptor *descriptor)
{
    uint8_t raw[EXT2_GROUP_DESCRIPTOR_SIZE];
    uint32_t position;

    if ((device == NULL) || (superblock == NULL) || (descriptor == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or no descriptor to write");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (descriptor->group >= superblock->group_count)
    {
        return Ext2WriteRefuse("the volume holds no group of that number");
    }

    position = descriptor->group * EXT2_GROUP_DESCRIPTOR_SIZE;

    /* Read first, for the reason the superblock is: bg_pad and bg_reserved are
     * not this kernel's to overwrite. */
    if (!Ext2ReadBytes(device, superblock,
                       Ext2GroupDescriptorBlock(superblock) +
                           (position / superblock->block_size),
                       position % superblock->block_size, EXT2_GROUP_DESCRIPTOR_SIZE, raw))
    {
        return false;
    }

    Ext2WriteWord(raw, EXT2_OFFSET_BG_BLOCK_BITMAP, descriptor->block_bitmap);
    Ext2WriteWord(raw, EXT2_OFFSET_BG_INODE_BITMAP, descriptor->inode_bitmap);
    Ext2WriteWord(raw, EXT2_OFFSET_BG_INODE_TABLE, descriptor->inode_table);
    Ext2WriteHalf(raw, EXT2_OFFSET_BG_FREE_BLOCKS, descriptor->free_block_count);
    Ext2WriteHalf(raw, EXT2_OFFSET_BG_FREE_INODES, descriptor->free_inode_count);
    Ext2WriteHalf(raw, EXT2_OFFSET_BG_USED_DIRECTORIES, descriptor->used_directory_count);

    return Ext2WriteBytes(device, superblock,
                          Ext2GroupDescriptorBlock(superblock) +
                              (position / superblock->block_size),
                          position % superblock->block_size, EXT2_GROUP_DESCRIPTOR_SIZE, raw);
}

bool Ext2WriteInode(BlockDevice *device, const Ext2Superblock *superblock,
                    const Ext2Inode *inode)
{
    uint8_t raw[EXT2_GOOD_OLD_INODE_SIZE];
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;
    uint32_t position;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or no inode to write");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if ((inode->number == 0U) || (inode->number > superblock->inode_count))
    {
        return Ext2WriteRefuse("the volume holds no inode of that number");
    }

    group = (inode->number - 1U) / superblock->inodes_per_group;
    index = (inode->number - 1U) % superblock->inodes_per_group;

    if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    position = index * superblock->inode_size;

    /* Read first: an inode of 256 bytes carries extensions beyond the 128 this
     * kernel parses, and they are not this kernel's to discard. */
    if (!Ext2ReadBytes(device, superblock,
                       descriptor.inode_table + (position / superblock->block_size),
                       position % superblock->block_size, EXT2_GOOD_OLD_INODE_SIZE, raw))
    {
        return false;
    }

    Ext2WriteHalf(raw, EXT2_OFFSET_I_MODE, inode->mode);
    Ext2WriteHalf(raw, EXT2_OFFSET_I_UID, inode->uid);
    Ext2WriteHalf(raw, EXT2_OFFSET_I_GID, inode->gid);
    Ext2WriteWord(raw, EXT2_OFFSET_I_SIZE, (uint32_t)(inode->size & 0xFFFFFFFFU));
    Ext2WriteWord(raw, EXT2_OFFSET_I_ATIME, inode->access_time);
    Ext2WriteWord(raw, EXT2_OFFSET_I_CTIME, inode->change_time);
    Ext2WriteWord(raw, EXT2_OFFSET_I_MTIME, inode->modify_time);
    Ext2WriteWord(raw, EXT2_OFFSET_I_DTIME, inode->delete_time);
    Ext2WriteHalf(raw, EXT2_OFFSET_I_LINKS_COUNT, inode->link_count);
    Ext2WriteWord(raw, EXT2_OFFSET_I_BLOCKS, inode->sector_count);
    Ext2WriteWord(raw, EXT2_OFFSET_I_FLAGS, inode->flags);
    Ext2WriteWord(raw, EXT2_OFFSET_I_GENERATION, inode->generation);
    Ext2WriteWord(raw, EXT2_OFFSET_I_FILE_ACL, inode->file_acl);

    /*
     * The high half of the size is written only for a regular file upon a
     * revision 1 volume. Upon a directory those same bytes are i_dir_acl and
     * mean something else entirely, exactly as when they are read.
     */
    if ((superblock->revision >= EXT2_DYNAMIC_REV) && ((inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG))
    {
        Ext2WriteWord(raw, EXT2_OFFSET_I_DIR_ACL, (uint32_t)(inode->size >> 32));
    }

    for (uint32_t entry = 0U; entry < EXT2_BLOCK_POINTER_COUNT; ++entry)
    {
        Ext2WriteWord(raw, EXT2_OFFSET_I_BLOCK + (entry * EXT2_BLOCK_POINTER_SIZE),
                      inode->block[entry]);
    }

    return Ext2WriteBytes(device, superblock,
                          descriptor.inode_table + (position / superblock->block_size),
                          position % superblock->block_size, EXT2_GOOD_OLD_INODE_SIZE, raw);
}

/*
 * The bitmaps.
 *
 * One bit stands for each block of a group and each inode of it, 1 meaning used
 * and 0 free. The first of the group is bit 0 of byte 0 and the ninth is bit 0
 * of byte 1: least significant bit first within a byte, which is not what a
 * diagram of a byte suggests and is what the format states.
 *
 * The bitmaps are the first structure of the volume this kernel reads that it
 * did not need in order to read a file. Nothing before now had to know which
 * blocks were in use, because nothing allocated one; the free counts were read
 * and believed. That ends here.
 */
#define EXT2_BITS_PER_BYTE 8U

/* Reads one bit of a bitmap block, the index counting from the first of the group. */
static bool Ext2BitmapTest(BlockDevice *device, const Ext2Superblock *superblock,
                           uint32_t bitmap, uint32_t index, bool *used)
{
    uint8_t byte;

    if (!Ext2ReadBytes(device, superblock, bitmap, index / EXT2_BITS_PER_BYTE, 1U, &byte))
    {
        return false;
    }

    *used = (byte & (uint8_t)(1U << (index % EXT2_BITS_PER_BYTE))) != 0U;
    return true;
}

/*
 * Sets or clears one bit, refusing to do what has already been done.
 *
 * Setting a bit already set means two owners believe they hold the same block;
 * clearing one already clear means a block is about to be freed twice, and the
 * second free is what lets it be allocated to two files at once. Both are
 * refused rather than performed, because both are corruption that spreads and
 * neither announces itself at the moment it occurs.
 */
static bool Ext2BitmapSet(BlockDevice *device, const Ext2Superblock *superblock,
                          uint32_t bitmap, uint32_t index, bool used, const char *what)
{
    const uint8_t mask = (uint8_t)(1U << (index % EXT2_BITS_PER_BYTE));
    const uint32_t offset = index / EXT2_BITS_PER_BYTE;
    uint8_t byte;

    if (!Ext2ReadBytes(device, superblock, bitmap, offset, 1U, &byte))
    {
        return false;
    }

    if (((byte & mask) != 0U) == used)
    {
        return Ext2WriteRefuse(what);
    }

    byte = used ? (uint8_t)(byte | mask) : (uint8_t)(byte & (uint8_t)~mask);

    return Ext2WriteBytes(device, superblock, bitmap, offset, 1U, &byte);
}

/* The group a block belongs to, and its index within that group's bitmap. */
static bool Ext2BlockPosition(const Ext2Superblock *superblock, uint32_t block,
                              uint32_t *group, uint32_t *index)
{
    if (!Ext2BlockExists(superblock, block))
    {
        return Ext2WriteRefuse("the block lies outside the volume");
    }

    *group = (block - superblock->first_data_block) / superblock->blocks_per_group;
    *index = (block - superblock->first_data_block) % superblock->blocks_per_group;
    return true;
}

bool Ext2BlockInUse(BlockDevice *device, const Ext2Superblock *superblock, uint32_t block,
                    bool *used)
{
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;

    if ((device == NULL) || (superblock == NULL) || (used == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or nowhere to put the answer");
    }

    if (!Ext2BlockPosition(superblock, block, &group, &index) ||
        !Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    return Ext2BitmapTest(device, superblock, descriptor.block_bitmap, index, used);
}

bool Ext2InodeInUse(BlockDevice *device, const Ext2Superblock *superblock, uint32_t number,
                    bool *used)
{
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;

    if ((device == NULL) || (superblock == NULL) || (used == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or nowhere to put the answer");
    }

    if ((number == 0U) || (number > superblock->inode_count))
    {
        return Ext2WriteRefuse("the volume holds no inode of that number");
    }

    /* Inode numbers begin at one and the bits at zero, exactly as when an inode
     * is located within its table. */
    group = (number - 1U) / superblock->inodes_per_group;
    index = (number - 1U) % superblock->inodes_per_group;

    if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    return Ext2BitmapTest(device, superblock, descriptor.inode_bitmap, index, used);
}

/*
 * Finds the first free bit of a group's bitmap, searching only the bits that
 * stand for something.
 *
 * The last group is short whenever the volume is not an exact multiple of the
 * group size, and the bits beyond its blocks are set by whatever made the
 * volume. Bounding the search by the group's true extent rather than trusting
 * those bits is what keeps this from issuing a block the volume does not hold
 * upon a volume that left them clear.
 */
static bool Ext2BitmapFindFree(BlockDevice *device, const Ext2Superblock *superblock,
                               uint32_t bitmap, uint32_t count, uint32_t *index, bool *found)
{
    *found = false;

    for (uint32_t position = 0U; position < count; ++position)
    {
        bool used;

        if (!Ext2BitmapTest(device, superblock, bitmap, position, &used))
        {
            return false;
        }

        if (!used)
        {
            *index = position;
            *found = true;
            return true;
        }
    }

    return true;
}

bool Ext2AllocateBlock(BlockDevice *device, Ext2Superblock *superblock, uint32_t near,
                       uint32_t *block)
{
    uint32_t first_group = 0U;

    if ((device == NULL) || (superblock == NULL) || (block == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or nowhere to put the block");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (superblock->free_block_count == 0U)
    {
        return Ext2WriteRefuse("the volume holds no free block");
    }

    /*
     * The hint names a block the caller would like to be near — ordinarily the
     * previous block of the same file. Beginning the search in that block's group
     * is the whole of this kernel's allocation policy: it keeps a file's blocks
     * together, which is what makes reading it sequential, and it costs one
     * division.
     */
    if ((near != 0U) && Ext2BlockExists(superblock, near))
    {
        uint32_t index;

        if (!Ext2BlockPosition(superblock, near, &first_group, &index))
        {
            return false;
        }
    }

    for (uint32_t attempt = 0U; attempt < superblock->group_count; ++attempt)
    {
        const uint32_t group = (first_group + attempt) % superblock->group_count;
        Ext2GroupDescriptor descriptor;
        uint32_t index;
        bool found;

        if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
        {
            return false;
        }

        if (descriptor.free_block_count == 0U)
        {
            continue;
        }

        if (!Ext2BitmapFindFree(device, superblock, descriptor.block_bitmap,
                                Ext2GroupBlockCount(superblock, group), &index, &found))
        {
            return false;
        }

        if (!found)
        {
            /*
             * The descriptor claimed free blocks and the bitmap holds none. The
             * volume contradicts itself, and allocating from another group would
             * leave the contradiction in place for the next caller to meet.
             */
            return Ext2WriteRefuse("a group's free count disagrees with its block bitmap");
        }

        /* The bitmap first, then the accounting; see the discipline in ext2.h. */
        if (!Ext2BitmapSet(device, superblock, descriptor.block_bitmap, index, true,
                           "a block already in use was allocated"))
        {
            return false;
        }

        descriptor.free_block_count--;
        superblock->free_block_count--;

        if (!Ext2WriteGroupDescriptor(device, superblock, &descriptor) ||
            !Ext2WriteSuperblock(device, superblock))
        {
            return false;
        }

        *block = Ext2GroupFirstBlock(superblock, group) + index;
        ++Ext2BlocksAllocatedCount;
        return true;
    }

    return Ext2WriteRefuse("no group of the volume holds a free block");
}

bool Ext2FreeBlock(BlockDevice *device, Ext2Superblock *superblock, uint32_t block)
{
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;

    if ((device == NULL) || (superblock == NULL))
    {
        return Ext2WriteRefuse("no device or no volume");
    }

    if (!Ext2Writable(superblock) || !Ext2BlockPosition(superblock, block, &group, &index) ||
        !Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    if (!Ext2BitmapSet(device, superblock, descriptor.block_bitmap, index, false,
                       "a block that was already free was freed"))
    {
        return false;
    }

    descriptor.free_block_count++;
    superblock->free_block_count++;

    if (!Ext2WriteGroupDescriptor(device, superblock, &descriptor) ||
        !Ext2WriteSuperblock(device, superblock))
    {
        return false;
    }

    ++Ext2BlocksFreedCount;
    return true;
}

bool Ext2AllocateInode(BlockDevice *device, Ext2Superblock *superblock, bool directory,
                       uint32_t *number)
{
    if ((device == NULL) || (superblock == NULL) || (number == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or nowhere to put the inode");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (superblock->free_inode_count == 0U)
    {
        return Ext2WriteRefuse("the volume holds no free inode");
    }

    for (uint32_t group = 0U; group < superblock->group_count; ++group)
    {
        Ext2GroupDescriptor descriptor;
        uint32_t index;
        uint32_t candidate;
        bool found;

        if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
        {
            return false;
        }

        if (descriptor.free_inode_count == 0U)
        {
            continue;
        }

        if (!Ext2BitmapFindFree(device, superblock, descriptor.inode_bitmap,
                                superblock->inodes_per_group, &index, &found))
        {
            return false;
        }

        if (!found)
        {
            return Ext2WriteRefuse("a group's free count disagrees with its inode bitmap");
        }

        candidate = (group * superblock->inodes_per_group) + index + 1U;

        /*
         * An inode below s_first_ino belongs to the filesystem. Such an inode is
         * ordinarily marked used in the bitmap already, so this is a second line
         * and not the first; a volume that left one clear would otherwise have
         * its root directory issued to a file.
         */
        if (candidate < superblock->first_inode)
        {
            continue;
        }

        if (!Ext2BitmapSet(device, superblock, descriptor.inode_bitmap, index, true,
                           "an inode already in use was allocated"))
        {
            return false;
        }

        descriptor.free_inode_count--;
        superblock->free_inode_count--;

        if (directory)
        {
            descriptor.used_directory_count++;
        }

        if (!Ext2WriteGroupDescriptor(device, superblock, &descriptor) ||
            !Ext2WriteSuperblock(device, superblock))
        {
            return false;
        }

        *number = candidate;
        ++Ext2InodesAllocatedCount;
        return true;
    }

    return Ext2WriteRefuse("no group of the volume holds a free inode");
}

bool Ext2FreeInode(BlockDevice *device, Ext2Superblock *superblock, uint32_t number,
                   bool directory)
{
    Ext2GroupDescriptor descriptor;
    uint32_t group;
    uint32_t index;

    if ((device == NULL) || (superblock == NULL))
    {
        return Ext2WriteRefuse("no device or no volume");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if ((number == 0U) || (number > superblock->inode_count))
    {
        return Ext2WriteRefuse("the volume holds no inode of that number");
    }

    if (number < superblock->first_inode)
    {
        return Ext2WriteRefuse("an inode belonging to the filesystem was freed");
    }

    group = (number - 1U) / superblock->inodes_per_group;
    index = (number - 1U) % superblock->inodes_per_group;

    if (!Ext2ReadGroupDescriptor(device, superblock, group, &descriptor))
    {
        return false;
    }

    if (!Ext2BitmapSet(device, superblock, descriptor.inode_bitmap, index, false,
                       "an inode that was already free was freed"))
    {
        return false;
    }

    descriptor.free_inode_count++;
    superblock->free_inode_count++;

    if (directory && (descriptor.used_directory_count > 0U))
    {
        descriptor.used_directory_count--;
    }

    if (!Ext2WriteGroupDescriptor(device, superblock, &descriptor) ||
        !Ext2WriteSuperblock(device, superblock))
    {
        return false;
    }

    ++Ext2InodesFreedCount;
    return true;
}

/*
 * Growing a file.
 *
 * The decomposition of a block index into levels of indirection is the one
 * Ext2InodeBlock performs, and it is performed a second time here rather than
 * shared, because the two walks differ at every step: one reads a pointer and
 * accepts zero as a hole, and the other must allocate a block where it finds
 * zero, zero that block if it is a block of pointers, and write the pointer back
 * into whatever holds it.
 */

/* How many 512-byte sectors one block of the volume occupies, i_blocks being
 * counted in sectors and not in blocks. */
static uint32_t Ext2SectorsPerBlock(const Ext2Superblock *superblock)
{
    return superblock->block_size / 512U;
}

/*
 * Reads one entry of a block of pointers, allocating the entry's block where it
 * is zero and writing the pointer back.
 *
 * `zero` says whether the newly allocated block is itself a block of pointers,
 * which must be zeroed: an unzeroed one is read as pointers to whatever the
 * block last held, and those are real blocks belonging to other files.
 */
static bool Ext2PointerAllocate(BlockDevice *device, Ext2Superblock *superblock,
                                Ext2Inode *inode, uint32_t table, uint64_t entry, bool zero,
                                uint32_t *block, bool *allocated)
{
    uint8_t raw[EXT2_BLOCK_POINTER_SIZE];
    const uint32_t offset = (uint32_t)(entry * EXT2_BLOCK_POINTER_SIZE);

    if (!Ext2ReadBytes(device, superblock, table, offset, EXT2_BLOCK_POINTER_SIZE, raw))
    {
        return false;
    }

    *block = Ext2ReadWord(raw, 0U);

    if (*block != 0U)
    {
        if (!Ext2BlockExists(superblock, *block))
        {
            return Ext2WriteRefuse("a block pointer addresses a block the volume does not hold");
        }

        return true;
    }

    if (!Ext2AllocateBlock(device, superblock, table, block))
    {
        return false;
    }

    if (zero && !Ext2ZeroBlock(device, superblock, *block))
    {
        return false;
    }

    inode->sector_count += Ext2SectorsPerBlock(superblock);
    *allocated = true;

    Ext2WriteWord(raw, 0U, *block);

    return Ext2WriteBytes(device, superblock, table, offset, EXT2_BLOCK_POINTER_SIZE, raw);
}

/* The same for one of the fifteen entries of i_block, which is held in memory
 * rather than upon the volume and so is assigned rather than written. */
static bool Ext2InodePointerAllocate(BlockDevice *device, Ext2Superblock *superblock,
                                     Ext2Inode *inode, uint32_t entry, bool zero,
                                     uint32_t *block, bool *allocated)
{
    if (inode->block[entry] != 0U)
    {
        *block = inode->block[entry];
        return true;
    }

    if (!Ext2AllocateBlock(device, superblock, inode->block[0], block))
    {
        return false;
    }

    if (zero && !Ext2ZeroBlock(device, superblock, *block))
    {
        return false;
    }

    inode->sector_count += Ext2SectorsPerBlock(superblock);
    inode->block[entry] = *block;
    *allocated = true;
    return true;
}

bool Ext2InodeBlockAllocate(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *inode,
                            uint64_t index, uint32_t *block, bool *allocated)
{
    uint64_t per_block;
    uint64_t remaining;
    uint32_t level;
    uint32_t table;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (block == NULL) ||
        (allocated == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no inode, or nowhere to put the block");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    *allocated = false;

    if (index < EXT2_DIRECT_BLOCK_COUNT)
    {
        return Ext2InodePointerAllocate(device, superblock, inode, (uint32_t)index, false,
                                        block, allocated);
    }

    per_block = (uint64_t)Ext2PointersPerBlock(superblock);
    remaining = index - EXT2_DIRECT_BLOCK_COUNT;

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
                return Ext2WriteRefuse("the index is beyond what fifteen pointers can address");
            }
        }
    }

    /*
     * The block of pointers named by i_block for this level, allocated and zeroed
     * where the file has not reached this far before.
     */
    if (!Ext2InodePointerAllocate(device, superblock, inode,
                                  EXT2_INDIRECT_INDEX + (level - 1U), true, &table, allocated))
    {
        return false;
    }

    while (level > 0U)
    {
        uint64_t span = 1U;

        for (uint32_t power = 1U; power < level; ++power)
        {
            span *= per_block;
        }

        /* Every block of this walk is a block of pointers save the last, which is
         * the file's own data and is not zeroed: the caller is about to write it,
         * and a caller writing only part of it zeroes the rest itself. */
        if (!Ext2PointerAllocate(device, superblock, inode, table, remaining / span,
                                 level > 1U, &table, allocated))
        {
            return false;
        }

        remaining %= span;
        --level;
    }

    *block = table;
    return true;
}

bool Ext2WriteFile(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *inode,
                   uint64_t offset, const void *buffer, uint64_t length, uint64_t *written)
{
    const uint8_t *source = (const uint8_t *)buffer;
    uint64_t remaining = length;
    uint64_t put = 0U;
    bool allocated = false;
    bool grew = false;

    if ((device == NULL) || (superblock == NULL) || (inode == NULL) || (written == NULL) ||
        ((buffer == NULL) && (length != 0U)))
    {
        return Ext2WriteRefuse("no device, no volume, no inode, nothing to write, or "
                               "nowhere to put the count");
    }

    *written = 0U;

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (Ext2InodeIsDirectory(inode))
    {
        return Ext2WriteRefuse("a directory's entries are not written as a stream of bytes");
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

        if (!Ext2InodeBlockAllocate(device, superblock, inode, index, &block, &allocated))
        {
            break;
        }

        /*
         * A block newly allocated holds whatever its previous owner left in it.
         * Where this write covers the whole of it that does not matter, every
         * byte being about to be replaced; where it does not, the remainder would
         * become another file's data appearing as this one's contents. It is
         * zeroed in that case and only that case, which is why the allocation
         * reports whether it allocated rather than the caller inferring it from
         * the offsets.
         */
        if (allocated && (take != superblock->block_size) &&
            !Ext2ZeroBlock(device, superblock, block))
        {
            break;
        }

        if (!Ext2WriteBytes(device, superblock, block, within, take, source))
        {
            break;
        }

        source += take;
        offset += take;
        remaining -= take;
        put += take;

        if (offset > inode->size)
        {
            inode->size = offset;
            grew = true;
        }
    }

    *written = put;
    Ext2BytesWrittenCount += put;

    /*
     * The inode is written back whether or not every byte was written. The blocks
     * that were allocated are allocated and the bytes that were written are upon
     * the volume; an inode left unwritten would describe a file shorter than its
     * contents and would leak every block beyond it.
     */
    if ((put > 0U) || grew)
    {
        if (!Ext2WriteInode(device, superblock, inode))
        {
            return false;
        }
    }

    return remaining == 0U;
}

/*
 * Frees the blocks of one pointer subtree at or beyond a file block index.
 *
 * `base` is the file block index the subtree's first entry stands for, and
 * `span` the indices one entry covers. The table itself is freed, and the
 * caller's pointer to it cleared, only when nothing is left in it — which is
 * what makes a truncation to zero return every block of the file and a
 * truncation to the middle of an indirect range keep the table that still holds
 * the earlier half.
 */
static bool Ext2TruncateSubtree(BlockDevice *device, Ext2Superblock *superblock,
                                Ext2Inode *inode, uint32_t *table, uint32_t level,
                                uint64_t base, uint64_t span, uint64_t first)
{
    const uint64_t per_block = (uint64_t)Ext2PointersPerBlock(superblock);
    const uint64_t entry_span = span / per_block;
    bool occupied = false;

    if (*table == 0U)
    {
        return true;
    }

    /*
     * A subtree lying wholly below the new size is retained entire, and is not
     * walked. Without this a truncation of one block from a large file would read
     * every pointer block the file has, which is the whole of its indirection for
     * the sake of one block.
     */
    if ((base + span) <= first)
    {
        return true;
    }

    for (uint64_t entry = 0U; entry < per_block; ++entry)
    {
        const uint64_t entry_base = base + (entry * entry_span);
        uint8_t raw[EXT2_BLOCK_POINTER_SIZE];
        uint32_t child;

        if (!Ext2ReadBytes(device, superblock, *table,
                           (uint32_t)(entry * EXT2_BLOCK_POINTER_SIZE),
                           EXT2_BLOCK_POINTER_SIZE, raw))
        {
            return false;
        }

        child = Ext2ReadWord(raw, 0U);

        if (child == 0U)
        {
            continue;
        }

        if (level > 1U)
        {
            if (!Ext2TruncateSubtree(device, superblock, inode, &child, level - 1U, entry_base,
                                     entry_span, first))
            {
                return false;
            }
        }
        else if (entry_base >= first)
        {
            if (!Ext2FreeBlock(device, superblock, child))
            {
                return false;
            }

            inode->sector_count -= Ext2SectorsPerBlock(superblock);
            child = 0U;
        }

        if (child == 0U)
        {
            Ext2WriteWord(raw, 0U, 0U);

            if (!Ext2WriteBytes(device, superblock, *table,
                                (uint32_t)(entry * EXT2_BLOCK_POINTER_SIZE),
                                EXT2_BLOCK_POINTER_SIZE, raw))
            {
                return false;
            }
        }
        else
        {
            occupied = true;
        }
    }

    if (!occupied)
    {
        if (!Ext2FreeBlock(device, superblock, *table))
        {
            return false;
        }

        inode->sector_count -= Ext2SectorsPerBlock(superblock);
        *table = 0U;
    }

    return true;
}

/*
 * The mechanism of truncation, without the judgement that a directory is not
 * truncated as a file is.
 *
 * A directory's blocks must be freed when it is removed, and that is the same
 * work; separating the two lets the public entry point refuse a directory — a
 * caller truncating one has almost certainly mistaken what it holds — while
 * Ext2RemoveDirectory, which knows exactly what it holds, uses the mechanism.
 */
static bool Ext2TruncateBlocks(BlockDevice *device, Ext2Superblock *superblock,
                               Ext2Inode *inode, uint64_t size)
{
    const uint64_t per_block = (uint64_t)Ext2PointersPerBlock(superblock);
    uint64_t first;

    /*
     * A size above the present one extends the file with a hole rather than with
     * allocated blocks. That is what every Unix does, and it is why truncation is
     * the cheap way to make a large sparse file: nothing is allocated and nothing
     * is written but the size.
     */
    if (size >= inode->size)
    {
        inode->size = size;
        return Ext2WriteInode(device, superblock, inode);
    }

    /* The first block index the file no longer needs. A size that ends within a
     * block keeps that block, the bytes before the new end still being in it. */
    first = (size + (uint64_t)superblock->block_size - 1U) / (uint64_t)superblock->block_size;

    for (uint32_t entry = 0U; entry < EXT2_DIRECT_BLOCK_COUNT; ++entry)
    {
        if ((entry >= first) && (inode->block[entry] != 0U))
        {
            if (!Ext2FreeBlock(device, superblock, inode->block[entry]))
            {
                return false;
            }

            inode->sector_count -= Ext2SectorsPerBlock(superblock);
            inode->block[entry] = 0U;
        }
    }

    if (!Ext2TruncateSubtree(device, superblock, inode, &inode->block[EXT2_INDIRECT_INDEX], 1U,
                             EXT2_DIRECT_BLOCK_COUNT, per_block, first) ||
        !Ext2TruncateSubtree(device, superblock, inode,
                             &inode->block[EXT2_DOUBLE_INDIRECT_INDEX], 2U,
                             EXT2_DIRECT_BLOCK_COUNT + per_block, per_block * per_block,
                             first) ||
        !Ext2TruncateSubtree(device, superblock, inode,
                             &inode->block[EXT2_TRIPLE_INDIRECT_INDEX], 3U,
                             EXT2_DIRECT_BLOCK_COUNT + per_block + (per_block * per_block),
                             per_block * per_block * per_block, first))
    {
        return false;
    }

    inode->size = size;
    return Ext2WriteInode(device, superblock, inode);
}

bool Ext2TruncateFile(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *inode,
                      uint64_t size)
{
    if ((device == NULL) || (superblock == NULL) || (inode == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, or no inode to truncate");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (Ext2InodeIsDirectory(inode))
    {
        return Ext2WriteRefuse("a directory is not truncated as a file is");
    }

    return Ext2TruncateBlocks(device, superblock, inode, size);
}


/*
 * Altering a directory.
 *
 * A directory is a file, so the blocks it occupies are allocated and freed by
 * the machinery above. What is particular to a directory is the linked list of
 * records within each of its blocks, and every operation here is an alteration
 * of that list: an insertion splits a record, and a removal joins two.
 *
 * The list is what makes both cheap. Nothing is ever moved: a name is removed by
 * lengthening the record before it, and a name is inserted into the slack that
 * such a lengthening left behind. A directory therefore does not shrink, and
 * repeated creation and removal reuses the same space rather than growing.
 */

/* Writes the eight-byte header of a record, in the volume's own order and in
 * whichever of the two readings of offset 6 the volume uses. */
static void Ext2ComposeEntryHeader(uint8_t *raw, uint32_t number, uint16_t record_length,
                                   uint16_t name_length, uint8_t file_type, bool states_type)
{
    Ext2WriteWord(raw, EXT2_OFFSET_DE_INODE, number);
    Ext2WriteHalf(raw, EXT2_OFFSET_DE_RECORD_LENGTH, record_length);

    if (states_type)
    {
        raw[EXT2_OFFSET_DE_NAME_LENGTH] = (uint8_t)name_length;
        raw[EXT2_OFFSET_DE_FILE_TYPE] = file_type;
    }
    else
    {
        Ext2WriteHalf(raw, EXT2_OFFSET_DE_NAME_LENGTH, name_length);
    }
}

/* Writes a whole record — header and name — at an offset within a block. */
static bool Ext2PutEntry(BlockDevice *device, Ext2Superblock *superblock, uint32_t block,
                         uint32_t offset, uint32_t number, uint16_t record_length,
                         const char *name, size_t length, uint8_t file_type)
{
    uint8_t raw[EXT2_DIRECTORY_HEADER_SIZE];

    Ext2ComposeEntryHeader(raw, number, record_length, (uint16_t)length, file_type,
                           Ext2VolumeStatesFileType(superblock));

    if (!Ext2WriteBytes(device, superblock, block, offset, EXT2_DIRECTORY_HEADER_SIZE, raw))
    {
        return false;
    }

    if (length == 0U)
    {
        return true;
    }

    return Ext2WriteBytes(device, superblock, block, offset + EXT2_DIRECTORY_HEADER_SIZE,
                          (uint32_t)length, (const uint8_t *)name);
}

/*
 * Finds room for a record of the given length within one block of a directory,
 * and lays the new record in it.
 *
 * Two kinds of room exist and both are used. A record naming no inode is unused
 * entirely and may be taken if it is long enough. A record in use is ordinarily
 * longer than the name it holds — because a removal lengthened it, or because it
 * is the last of its block and runs to the end — and the excess beyond what it
 * actually needs may be given away by shortening it.
 */
static bool Ext2InsertIntoBlock(BlockDevice *device, Ext2Superblock *superblock,
                                uint32_t block, uint16_t needed, uint32_t number,
                                const char *name, size_t length, uint8_t file_type,
                                bool *inserted)
{
    uint32_t offset = 0U;

    *inserted = false;

    while (offset <= (superblock->block_size - EXT2_DIRECTORY_HEADER_SIZE))
    {
        Ext2DirectoryEntry entry;
        uint16_t actual;

        if (!Ext2ReadEntryHeader(device, superblock, block, offset, &entry))
        {
            return false;
        }

        /* An unused record may be taken whole. */
        if (entry.inode == 0U)
        {
            if (entry.record_length >= needed)
            {
                *inserted = true;
                return Ext2PutEntry(device, superblock, block, offset, number,
                                    entry.record_length, name, length, file_type);
            }
        }
        else
        {
            actual = EXT2_RECORD_LENGTH(entry.name_length);

            if ((entry.record_length - actual) >= needed)
            {
                const uint16_t remainder = (uint16_t)(entry.record_length - actual);
                uint8_t raw[EXT2_DIRECTORY_HEADER_SIZE];

                /*
                 * The record in use is shortened to what it needs and the new one
                 * takes what is left. The header of the existing record is rewritten
                 * rather than composed afresh, so that its name and its file type
                 * are untouched: only its length changes.
                 */
                if (!Ext2ReadBytes(device, superblock, block, offset,
                                   EXT2_DIRECTORY_HEADER_SIZE, raw))
                {
                    return false;
                }

                Ext2WriteHalf(raw, EXT2_OFFSET_DE_RECORD_LENGTH, actual);

                if (!Ext2WriteBytes(device, superblock, block, offset,
                                    EXT2_DIRECTORY_HEADER_SIZE, raw))
                {
                    return false;
                }

                *inserted = true;
                return Ext2PutEntry(device, superblock, block, offset + actual, number,
                                    remainder, name, length, file_type);
            }
        }

        offset += entry.record_length;

        if (offset == superblock->block_size)
        {
            break;
        }
    }

    return true;
}

bool Ext2DirectoryInsert(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
                         const char *name, size_t length, uint32_t number, uint8_t file_type)
{
    Ext2DirectoryEntry existing;
    uint16_t needed;
    uint64_t blocks;
    uint32_t block;
    bool allocated;
    bool inserted = false;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, or no name to insert");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2InodeIsDirectory(directory))
    {
        return Ext2WriteRefuse("a name may be inserted only into a directory");
    }

    if ((length == 0U) || (length > EXT2_NAME_MAXIMUM))
    {
        return Ext2WriteRefuse("a name of no length, or longer than the format permits");
    }

    if ((number == 0U) || (number > superblock->inode_count))
    {
        return Ext2WriteRefuse("a name may not be given to an inode the volume does not hold");
    }

    for (size_t index = 0U; index < length; ++index)
    {
        if ((name[index] == EXT2_PATH_SEPARATOR) || (name[index] == '\0'))
        {
            return Ext2WriteRefuse("a name holding a separator or a null byte");
        }
    }

    /*
     * A name already present is refused. A directory holding one name twice names
     * two files by the same path, and which is found depends upon which record
     * the traversal reaches first — so the duplicate is not merely untidy, it
     * makes the path ambiguous.
     */
    if (Ext2DirectoryFind(device, superblock, directory, name, length, &existing))
    {
        return Ext2WriteRefuse("the directory already holds that name");
    }

    needed = EXT2_RECORD_LENGTH(length);
    blocks = directory->size / (uint64_t)superblock->block_size;

    for (uint64_t index = 0U; index < blocks; ++index)
    {
        if (!Ext2InodeBlock(device, superblock, directory, index, &block))
        {
            return false;
        }

        if (block == 0U)
        {
            continue;
        }

        if (!Ext2InsertIntoBlock(device, superblock, block, needed, number, name, length,
                                 file_type, &inserted))
        {
            return false;
        }

        if (inserted)
        {
            ++Ext2NamesInsertedCount;
            return true;
        }
    }

    /*
     * No block had room, so the directory grows by one. The new block holds a
     * single record running its whole length, which is the shape of an empty
     * block, and the insertion then finds room in it as it would in any other.
     */
    if (!Ext2InodeBlockAllocate(device, superblock, directory, blocks, &block, &allocated))
    {
        return false;
    }

    if (!Ext2ZeroBlock(device, superblock, block) ||
        !Ext2PutEntry(device, superblock, block, 0U, 0U, (uint16_t)superblock->block_size,
                      NULL, 0U, (uint8_t)EXT2_FT_UNKNOWN))
    {
        return false;
    }

    directory->size += superblock->block_size;

    if (!Ext2WriteInode(device, superblock, directory))
    {
        return false;
    }

    if (!Ext2InsertIntoBlock(device, superblock, block, needed, number, name, length,
                             file_type, &inserted))
    {
        return false;
    }

    if (!inserted)
    {
        return Ext2WriteRefuse("a name did not fit within a block of its own");
    }

    ++Ext2NamesInsertedCount;
    return true;
}

bool Ext2DirectoryRemove(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
                         const char *name, size_t length)
{
    uint64_t blocks;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, or no name to remove");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2InodeIsDirectory(directory))
    {
        return Ext2WriteRefuse("a name may be removed only from a directory");
    }

    if ((length == 0U) || (length > EXT2_NAME_MAXIMUM))
    {
        return Ext2WriteRefuse("a name of no length, or longer than the format permits");
    }

    /*
     * A directory without "." no longer knows itself, and without ".." no longer
     * knows its parent; the resolver finds both by looking, so removing either
     * makes paths through this directory fail.
     */
    if (((length == 1U) && (name[0] == '.')) ||
        ((length == 2U) && (name[0] == '.') && (name[1] == '.')))
    {
        return Ext2WriteRefuse("the entries \".\" and \"..\" may not be removed");
    }

    blocks = directory->size / (uint64_t)superblock->block_size;

    for (uint64_t index = 0U; index < blocks; ++index)
    {
        uint32_t block;
        uint32_t offset = 0U;
        uint32_t previous = 0U;
        bool have_previous = false;

        if (!Ext2InodeBlock(device, superblock, directory, index, &block))
        {
            return false;
        }

        if (block == 0U)
        {
            continue;
        }

        while (offset <= (superblock->block_size - EXT2_DIRECTORY_HEADER_SIZE))
        {
            Ext2DirectoryEntry entry;
            bool same = false;

            if (!Ext2ReadEntryHeader(device, superblock, block, offset, &entry))
            {
                return false;
            }

            if ((entry.inode != 0U) && (entry.name_length == (uint16_t)length))
            {
                if (!Ext2ReadEntryName(device, superblock, &entry))
                {
                    return false;
                }

                same = true;

                for (size_t position = 0U; position < length; ++position)
                {
                    if (entry.name[position] != name[position])
                    {
                        same = false;
                        break;
                    }
                }
            }

            if (same)
            {
                uint8_t raw[EXT2_DIRECTORY_HEADER_SIZE];

                if (have_previous)
                {
                    /*
                     * The record before it absorbs its space. Nothing is moved and
                     * nothing is zeroed: the bytes of the removed record remain
                     * where they are, unreachable, until an insertion takes them.
                     */
                    Ext2DirectoryEntry before;

                    if (!Ext2ReadEntryHeader(device, superblock, block, previous, &before) ||
                        !Ext2ReadBytes(device, superblock, block, previous,
                                       EXT2_DIRECTORY_HEADER_SIZE, raw))
                    {
                        return false;
                    }

                    Ext2WriteHalf(raw, EXT2_OFFSET_DE_RECORD_LENGTH,
                                  (uint16_t)(before.record_length + entry.record_length));

                    if (!Ext2WriteBytes(device, superblock, block, previous,
                                        EXT2_DIRECTORY_HEADER_SIZE, raw))
                    {
                        return false;
                    }
                }
                else
                {
                    /*
                     * The first record of a block has nothing before it to lengthen,
                     * so it is marked unused where it stands. This is why a traversal
                     * must pass over a record naming inode 0 rather than reading the
                     * name still lying in it.
                     */
                    if (!Ext2ReadBytes(device, superblock, block, offset,
                                       EXT2_DIRECTORY_HEADER_SIZE, raw))
                    {
                        return false;
                    }

                    Ext2WriteWord(raw, EXT2_OFFSET_DE_INODE, 0U);

                    if (!Ext2WriteBytes(device, superblock, block, offset,
                                        EXT2_DIRECTORY_HEADER_SIZE, raw))
                    {
                        return false;
                    }
                }

                ++Ext2NamesRemovedCount;
                return true;
            }

            previous = offset;
            have_previous = true;
            offset += entry.record_length;

            if (offset == superblock->block_size)
            {
                break;
            }
        }
    }

    return Ext2WriteRefuse("the directory holds no name of that name to remove");
}

bool Ext2DirectoryIsEmpty(BlockDevice *device, const Ext2Superblock *superblock,
                          const Ext2Inode *directory, bool *empty)
{
    Ext2DirectoryCursor cursor;
    Ext2DirectoryEntry entry;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (empty == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, or nowhere for the answer");
    }

    *empty = true;
    Ext2DirectoryOpen(&cursor, directory);

    for (;;)
    {
        const Ext2DirectoryStep step = Ext2DirectoryNext(device, superblock, &cursor, &entry);

        if (step == EXT2_DIRECTORY_FAILED)
        {
            return false;
        }

        if (step == EXT2_DIRECTORY_END)
        {
            return true;
        }

        /* "." and ".." are what every directory holds; anything else occupies it. */
        if (((entry.name_length == 1U) && (entry.name[0] == '.')) ||
            ((entry.name_length == 2U) && (entry.name[0] == '.') && (entry.name[1] == '.')))
        {
            continue;
        }

        *empty = false;
        return true;
    }
}

/*
 * Creating and destroying files.
 *
 * A file is an inode and the names that lead to it, and the two are kept in
 * step by i_links_count. Creation allocates the inode and gives it one name;
 * a further name raises the count; removing a name lowers it; and the file
 * itself is destroyed only when the count reaches zero, since until then some
 * path still reaches it.
 */

/* Gives a newly allocated inode the state a file begins with: no blocks, no
 * size, and the mode the caller asked for. */
static void Ext2ComposeInode(Ext2Inode *inode, uint32_t number, uint16_t mode,
                             uint16_t link_count)
{
    inode->number = number;
    inode->mode = mode;
    inode->uid = 0U;
    inode->gid = 0U;
    inode->size = 0U;
    inode->access_time = 0U;
    inode->change_time = 0U;
    inode->modify_time = 0U;
    inode->delete_time = 0U;
    inode->link_count = link_count;
    inode->sector_count = 0U;
    inode->flags = 0U;
    inode->generation = 0U;
    inode->file_acl = 0U;

    for (uint32_t entry = 0U; entry < EXT2_BLOCK_POINTER_COUNT; ++entry)
    {
        inode->block[entry] = 0U;
    }
}

bool Ext2CreateFile(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
                    const char *name, size_t length, uint16_t mode, Ext2Inode *inode)
{
    Ext2Inode created;
    uint32_t number;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL) ||
        (inode == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, no name, or nowhere to "
                               "put the inode");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    /*
     * A directory is refused here. One is not merely an inode with a different
     * mode: it must hold "." and ".." before anything may resolve a path through
     * it, and its parent's link count must account for the "..". Creating one
     * with this would leave a directory that is not a directory.
     */
    if ((mode & EXT2_S_IFMT) == EXT2_S_IFDIR)
    {
        return Ext2WriteRefuse("a directory is created by Ext2CreateDirectory");
    }

    if ((mode & EXT2_S_IFMT) == 0U)
    {
        return Ext2WriteRefuse("a file must be created with a format");
    }

    if (!Ext2AllocateInode(device, superblock, false, &number))
    {
        return false;
    }

    Ext2ComposeInode(&created, number, mode, 1U);

    if (!Ext2WriteInode(device, superblock, &created))
    {
        return false;
    }

    /*
     * The inode is written before the name is inserted, so that a machine
     * stopping between the two leaves an inode nothing names — which a check
     * reclaims — rather than a name leading to an inode that was never written.
     */
    if (!Ext2DirectoryInsert(device, superblock, directory, name, length, number,
                             Ext2FileTypeOfMode(mode)))
    {
        return false;
    }

    *inode = created;
    ++Ext2FilesCreatedCount;
    return true;
}

bool Ext2CreateDirectory(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *parent,
                         const char *name, size_t length, uint16_t mode, Ext2Inode *inode)
{
    Ext2Inode created;
    uint32_t number;
    uint32_t block;
    uint16_t dot_length;
    bool allocated;

    if ((device == NULL) || (superblock == NULL) || (parent == NULL) || (name == NULL) ||
        (inode == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no parent, no name, or nowhere to put "
                               "the inode");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2InodeIsDirectory(parent))
    {
        return Ext2WriteRefuse("a directory may be created only within a directory");
    }

    /*
     * The parent gains a link for the ".." this directory will hold, so a parent
     * already at the limit cannot take another child. Refusing before anything is
     * allocated is what keeps a half-made directory off the volume.
     */
    if (parent->link_count >= EXT2_LINK_MAXIMUM)
    {
        return Ext2WriteRefuse("the parent directory holds as many links as it may");
    }

    if (!Ext2AllocateInode(device, superblock, true, &number))
    {
        return false;
    }

    /* Two links: its own "." and the entry the parent is about to hold. */
    Ext2ComposeInode(&created, number, (uint16_t)((mode & EXT2_PERMISSION_MASK) | EXT2_S_IFDIR),
                     2U);

    if (!Ext2InodeBlockAllocate(device, superblock, &created, 0U, &block, &allocated) ||
        !Ext2ZeroBlock(device, superblock, block))
    {
        return false;
    }

    created.size = superblock->block_size;

    /*
     * The two entries every directory holds. The record for ".." runs to the end
     * of the block, which is what ends the list of that block; a directory whose
     * last record stopped after its name would have the padding beyond it read
     * as a further entry.
     */
    dot_length = EXT2_RECORD_LENGTH(1U);

    if (!Ext2PutEntry(device, superblock, block, 0U, number, dot_length, ".", 1U,
                      (uint8_t)EXT2_FT_DIR) ||
        !Ext2PutEntry(device, superblock, block, dot_length, parent->number,
                      (uint16_t)(superblock->block_size - dot_length), "..", 2U,
                      (uint8_t)EXT2_FT_DIR))
    {
        return false;
    }

    if (!Ext2WriteInode(device, superblock, &created) ||
        !Ext2DirectoryInsert(device, superblock, parent, name, length, number,
                             (uint8_t)EXT2_FT_DIR))
    {
        return false;
    }

    /* The parent's link count accounts for the ".." now standing in the child. */
    parent->link_count++;

    if (!Ext2WriteInode(device, superblock, parent))
    {
        return false;
    }

    *inode = created;
    ++Ext2FilesCreatedCount;
    return true;
}

bool Ext2Link(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
              const char *name, size_t length, Ext2Inode *target)
{
    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL) ||
        (target == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, no name, or no file");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    /*
     * A directory may bear only one name. Two paths to one directory make a cycle
     * in what the format requires to be a tree, and a resolver walking ".." from
     * within it would have no single answer to give.
     */
    if (Ext2InodeIsDirectory(target))
    {
        return Ext2WriteRefuse("a directory may not be given a second name");
    }

    if (target->link_count >= EXT2_LINK_MAXIMUM)
    {
        return Ext2WriteRefuse("the file bears as many names as it may");
    }

    if (!Ext2DirectoryInsert(device, superblock, directory, name, length, target->number,
                             Ext2FileTypeOfMode(target->mode)))
    {
        return false;
    }

    target->link_count++;
    return Ext2WriteInode(device, superblock, target);
}

bool Ext2Unlink(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *directory,
                const char *name, size_t length)
{
    Ext2DirectoryEntry entry;
    Ext2Inode target;

    if ((device == NULL) || (superblock == NULL) || (directory == NULL) || (name == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no directory, or no name");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2DirectoryFind(device, superblock, directory, name, length, &entry) ||
        !Ext2ReadInode(device, superblock, entry.inode, &target))
    {
        return false;
    }

    if (Ext2InodeIsDirectory(&target))
    {
        return Ext2WriteRefuse("a directory is removed by Ext2RemoveDirectory");
    }

    if (!Ext2DirectoryRemove(device, superblock, directory, name, length))
    {
        return false;
    }

    if (target.link_count > 0U)
    {
        target.link_count--;
    }

    /*
     * A file with names remaining is not destroyed; only its count changes. A
     * file with none is destroyed, and the blocks go before the inode: an inode
     * freed while its blocks were still marked used would leak them with nothing
     * left to say which they were.
     */
    if (target.link_count > 0U)
    {
        return Ext2WriteInode(device, superblock, &target);
    }

    /*
     * A symbolic link holding its target within the inode has no blocks to free,
     * and the words of i_block are its target rather than pointers; truncating it
     * would read them as blocks of the volume.
     */
    if (!Ext2InodeIsFastSymbolicLink(superblock, &target) &&
        !Ext2TruncateBlocks(device, superblock, &target, 0U))
    {
        return false;
    }

    target.size = 0U;
    target.delete_time = 1U;

    if (!Ext2WriteInode(device, superblock, &target) ||
        !Ext2FreeInode(device, superblock, target.number, false))
    {
        return false;
    }

    ++Ext2FilesDestroyedCount;
    return true;
}

bool Ext2RemoveDirectory(BlockDevice *device, Ext2Superblock *superblock, Ext2Inode *parent,
                         const char *name, size_t length)
{
    Ext2DirectoryEntry entry;
    Ext2Inode target;
    bool empty = false;

    if ((device == NULL) || (superblock == NULL) || (parent == NULL) || (name == NULL))
    {
        return Ext2WriteRefuse("no device, no volume, no parent, or no name");
    }

    if (!Ext2Writable(superblock))
    {
        return false;
    }

    if (!Ext2DirectoryFind(device, superblock, parent, name, length, &entry) ||
        !Ext2ReadInode(device, superblock, entry.inode, &target))
    {
        return false;
    }

    if (!Ext2InodeIsDirectory(&target))
    {
        return Ext2WriteRefuse("what the name leads to is not a directory");
    }

    if (target.number == EXT2_ROOT_INODE)
    {
        return Ext2WriteRefuse("the root directory may not be removed");
    }

    if (!Ext2DirectoryIsEmpty(device, superblock, &target, &empty))
    {
        return false;
    }

    /*
     * A directory holding anything is not removed. Removing it would leave every
     * file within it reachable by no path — an inode with a link count above zero
     * that nothing names, which is exactly the state a check has to repair by
     * hand.
     */
    if (!empty)
    {
        return Ext2WriteRefuse("the directory is not empty");
    }

    if (!Ext2DirectoryRemove(device, superblock, parent, name, length) ||
        !Ext2TruncateBlocks(device, superblock, &target, 0U))
    {
        return false;
    }

    target.size = 0U;
    target.link_count = 0U;
    target.delete_time = 1U;

    if (!Ext2WriteInode(device, superblock, &target) ||
        !Ext2FreeInode(device, superblock, target.number, true))
    {
        return false;
    }

    /* The link the child's ".." held in the parent goes with it. */
    if (parent->link_count > 0U)
    {
        parent->link_count--;
    }

    if (!Ext2WriteInode(device, superblock, parent))
    {
        return false;
    }

    ++Ext2FilesDestroyedCount;
    return true;
}

uint64_t Ext2NamesInserted(void)
{
    return Ext2NamesInsertedCount;
}

uint64_t Ext2NamesRemoved(void)
{
    return Ext2NamesRemovedCount;
}

uint64_t Ext2FilesCreated(void)
{
    return Ext2FilesCreatedCount;
}

uint64_t Ext2FilesDestroyed(void)
{
    return Ext2FilesDestroyedCount;
}

uint64_t Ext2BlocksAllocated(void)
{
    return Ext2BlocksAllocatedCount;
}

uint64_t Ext2BlocksFreed(void)
{
    return Ext2BlocksFreedCount;
}

uint64_t Ext2InodesAllocated(void)
{
    return Ext2InodesAllocatedCount;
}

uint64_t Ext2InodesFreed(void)
{
    return Ext2InodesFreedCount;
}

uint64_t Ext2BytesWritten(void)
{
    return Ext2BytesWrittenCount;
}

uint64_t Ext2WritesRefused(void)
{
    return Ext2WritesRefusedCount;
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
