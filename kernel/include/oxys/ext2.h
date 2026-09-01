/*
 * File: kernel/include/oxys/ext2.h
 * Purpose: Declares the EXT2 superblock: the parsed, host-order description of a
 *          volume's geometry and features, the reading and validation of it from
 *          a block device, and the judgement of whether this kernel may address
 *          the volume at all.
 * Key definitions: EXT2_SUPER_MAGIC, EXT2_SUPERBLOCK_OFFSET, Ext2Superblock,
 *          Ext2ReadSuperblock, Ext2GroupCount, Ext2LastError, Ext2ReportVolume.
 * References:
 *   - Poirier, D., "The Second Extended File System: Internal Layout", the
 *     Superblock chapter: the superblock is located 1024 bytes from the start of
 *     the volume and occupies 1024 bytes; the magic number is 0xEF53; the block
 *     size is 1024 shifted left by s_log_block_size.
 *   - The same, the Superblock field table: the offset and width of every field
 *     read here, and the fields present only in a volume of revision 1.
 *   - The same, Reserved Inodes: inodes 1 to 10 are reserved, so the first
 *     inode available to a file is 11 upon a volume of revision 0, which has no
 *     field in which to state it.
 *   - Linux kernel documentation, the ext4 superblock table, consulted as an
 *     independent statement of the same offsets, the two formats agreeing upon
 *     every field this kernel reads.
 */

#ifndef OXYS_EXT2_H
#define OXYS_EXT2_H

#include <oxys/types.h>
#include <oxys/block.h>

/* Where the superblock lies, and how much of the volume it occupies. */
#define EXT2_SUPERBLOCK_OFFSET 1024U
#define EXT2_SUPERBLOCK_SIZE   1024U

/* The value of s_magic upon every EXT2 volume. */
#define EXT2_SUPER_MAGIC UINT16_C(0xEF53)

/* The revision levels. */
#define EXT2_GOOD_OLD_REV 0U
#define EXT2_DYNAMIC_REV  1U

/* What a volume of revision 0 has no field in which to state. */
#define EXT2_GOOD_OLD_INODE_SIZE 128U
#define EXT2_GOOD_OLD_FIRST_INODE 11U

/* The states of s_state. */
#define EXT2_VALID_FS 1U
#define EXT2_ERROR_FS 2U

/* The greatest block size this kernel will address. */
#define EXT2_MAXIMUM_BLOCK_SIZE 4096U

/* The incompatible features. A volume declaring one this kernel does not
 * implement may not be read at all: that is what makes them incompatible. */
#define EXT2_FEATURE_INCOMPAT_COMPRESSION UINT32_C(0x0001)
#define EXT2_FEATURE_INCOMPAT_FILETYPE    UINT32_C(0x0002)
#define EXT2_FEATURE_INCOMPAT_RECOVER     UINT32_C(0x0004)
#define EXT2_FEATURE_INCOMPAT_JOURNAL_DEV UINT32_C(0x0008)
#define EXT2_FEATURE_INCOMPAT_META_BG     UINT32_C(0x0010)

/* The read-only compatible features. A volume declaring one this kernel does
 * not implement may be read but not written. */
#define EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER UINT32_C(0x0001)
#define EXT2_FEATURE_RO_COMPAT_LARGE_FILE   UINT32_C(0x0002)
#define EXT2_FEATURE_RO_COMPAT_BTREE_DIR    UINT32_C(0x0004)

/* Those this kernel implements, and therefore tolerates. */
#define EXT2_FEATURES_INCOMPAT_SUPPORTED EXT2_FEATURE_INCOMPAT_FILETYPE
#define EXT2_FEATURES_RO_COMPAT_SUPPORTED \
    (EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | EXT2_FEATURE_RO_COMPAT_LARGE_FILE)

/*
 * The offsets of the superblock's fields, as the specification numbers them.
 *
 * They are declared here rather than beside the parser because they are the
 * format and not the parser's opinion of it: the boot-time self-test composes a
 * superblock from these same names, and a test that stated the offsets a second
 * time would agree with a mistaken parser as readily as with a correct one.
 */
#define EXT2_OFFSET_INODE_COUNT        0U
#define EXT2_OFFSET_BLOCK_COUNT        4U
#define EXT2_OFFSET_RESERVED_BLOCKS    8U
#define EXT2_OFFSET_FREE_BLOCKS        12U
#define EXT2_OFFSET_FREE_INODES        16U
#define EXT2_OFFSET_FIRST_DATA_BLOCK   20U
#define EXT2_OFFSET_LOG_BLOCK_SIZE     24U
#define EXT2_OFFSET_LOG_FRAGMENT_SIZE  28U
#define EXT2_OFFSET_BLOCKS_PER_GROUP   32U
#define EXT2_OFFSET_FRAGS_PER_GROUP    36U
#define EXT2_OFFSET_INODES_PER_GROUP   40U
#define EXT2_OFFSET_MOUNT_TIME         44U
#define EXT2_OFFSET_WRITE_TIME         48U
#define EXT2_OFFSET_MOUNT_COUNT        52U
#define EXT2_OFFSET_MAX_MOUNT_COUNT    54U
#define EXT2_OFFSET_MAGIC              56U
#define EXT2_OFFSET_STATE              58U
#define EXT2_OFFSET_ERRORS             60U
#define EXT2_OFFSET_MINOR_REVISION     62U
#define EXT2_OFFSET_LAST_CHECK         64U
#define EXT2_OFFSET_CHECK_INTERVAL     68U
#define EXT2_OFFSET_CREATOR_OS         72U
#define EXT2_OFFSET_REVISION           76U
#define EXT2_OFFSET_DEFAULT_UID        80U
#define EXT2_OFFSET_DEFAULT_GID        82U
#define EXT2_OFFSET_FIRST_INODE        84U
#define EXT2_OFFSET_INODE_SIZE         88U
#define EXT2_OFFSET_BLOCK_GROUP_NUMBER 90U
#define EXT2_OFFSET_FEATURE_COMPAT     92U
#define EXT2_OFFSET_FEATURE_INCOMPAT   96U
#define EXT2_OFFSET_FEATURE_RO_COMPAT  100U
#define EXT2_OFFSET_UUID               104U
#define EXT2_OFFSET_VOLUME_NAME        120U
#define EXT2_OFFSET_LAST_MOUNTED       136U

/* The lengths of the fields that are not scalars. */
#define EXT2_UUID_LENGTH         16U
#define EXT2_VOLUME_NAME_LENGTH  16U
#define EXT2_LAST_MOUNTED_LENGTH 64U

/*
 * A superblock, parsed.
 *
 * The fields are held in the processor's own types and order, not in the
 * volume's. Every quantity upon an EXT2 volume is stored little-endian whatever
 * the machine, so the decoding is explicit and performed once here rather than
 * being a property of how a structure happens to be laid out; see docs/storage/EXT2.md,
 * Section 3.
 */
typedef struct Ext2Superblock
{
    /* Present upon every revision. */
    uint32_t inode_count;
    uint32_t block_count;
    uint32_t reserved_block_count;
    uint32_t free_block_count;
    uint32_t free_inode_count;
    uint32_t first_data_block;
    uint32_t block_size; /* Derived: 1024 << s_log_block_size. */
    uint32_t fragment_size;
    uint32_t blocks_per_group;
    uint32_t fragments_per_group;
    uint32_t inodes_per_group;
    uint32_t mount_time;
    uint32_t write_time;
    uint16_t mount_count;
    uint16_t maximum_mount_count;
    uint16_t magic;
    uint16_t state;
    uint16_t errors;
    uint16_t minor_revision;
    uint32_t last_check;
    uint32_t check_interval;
    uint32_t creator_os;
    uint32_t revision;
    uint16_t default_uid;
    uint16_t default_gid;

    /* Present upon revision 1, and given their revision 0 values otherwise. */
    uint32_t first_inode;
    uint16_t inode_size;
    uint16_t block_group_number;
    uint32_t feature_compatible;
    uint32_t feature_incompatible;
    uint32_t feature_read_only;
    uint8_t uuid[16];
    char volume_name[17];  /* Sixteen characters, terminated here. */
    char last_mounted[65]; /* Sixty-four characters, terminated here. */

    /* Derived, and asserted against one another when the volume is read. */
    uint32_t group_count;
    uint32_t sectors_per_block; /* Block size divided by the device's own. */

    /*
     * True where the volume declares a read-only compatible feature this kernel
     * does not implement, or is not marked as cleanly unmounted. It may be read
     * and must not be written.
     */
    bool read_only;
} Ext2Superblock;

/*
 * Reads the superblock of a volume and validates it, filling the structure only
 * upon success.
 *
 * The volume is refused where the magic number is absent, the revision is beyond
 * this kernel, the geometry is impossible or self-contradictory, or an
 * incompatible feature is declared that this kernel does not implement. It is
 * accepted read-only where a read-only compatible feature is declared that this
 * kernel does not implement, or where the volume was not cleanly unmounted.
 *
 * Ext2LastError describes any refusal.
 */
bool Ext2ReadSuperblock(BlockDevice *device, Ext2Superblock *superblock);

/*
 * The number of block groups the volume holds. It is derived twice when the
 * superblock is read — from the block count and from the inode count — and the
 * volume is refused if the two disagree.
 */
uint32_t Ext2GroupCount(const Ext2Superblock *superblock);

/* A description of the most recent refusal, never null. */
const char *Ext2LastError(void);

/* How many volumes have been read, and how many refused. */
uint64_t Ext2VolumesRead(void);
uint64_t Ext2VolumesRefused(void);

/* Writes a description of a volume to the console. */
void Ext2ReportVolume(const Ext2Superblock *superblock, const char *name);

#endif /* OXYS_EXT2_H */
