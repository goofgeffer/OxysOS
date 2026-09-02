/*
 * File: kernel/include/oxys/ext2.h
 * Purpose: Declares the on-disk structures of an EXT2 volume that this kernel
 *          reads: the superblock, which describes the volume's geometry and
 *          features; the block group descriptor, which locates the three
 *          structures of one group; the inode, which describes one file and
 *          names the blocks holding its data; and the directory entry, which is
 *          the only place in the format where a name appears. All are declared in
 *          the processor's own order, the volume's order being decoded at the
 *          point of reading.
 * Key definitions: EXT2_SUPER_MAGIC, EXT2_SUPERBLOCK_OFFSET, Ext2Superblock,
 *          Ext2ReadSuperblock, Ext2GroupCount, Ext2GroupDescriptor,
 *          Ext2ReadGroupDescriptor, Ext2VerifyGroupDescriptors, Ext2Inode,
 *          Ext2ReadInode, Ext2InodeBlock, Ext2ReadFile, Ext2ReadSymbolicLink,
 *          Ext2InodeIsFastSymbolicLink, Ext2DirectoryEntry, Ext2DirectoryCursor,
 *          Ext2DirectoryStep, Ext2DirectoryOpen, Ext2DirectoryNext,
 *          Ext2DirectoryFind, Ext2ResolvePath, Ext2ResolvePathNoFollow,
 *          Ext2FileTypeOfMode, Ext2LastError, Ext2ReportVolume,
 *          Ext2ReportGroup, Ext2ReportInode, Ext2ReportDirectory.
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
 *   - The same, the Block Group Descriptor Table chapter and Table 3.12: the
 *     table begins upon the first block following the superblock — the third
 *     block of a 1 KiB volume, the second of any larger — a descriptor occupies
 *     32 bytes, and every block identifier within one is absolute.
 *   - The same, the Inode Table chapter and Table 3.13: an inode occupies the
 *     first 128 bytes of whatever s_inode_size states, i_block at offset 40
 *     holding fifteen block numbers of which twelve are direct, the thirteenth
 *     indirect, the fourteenth doubly indirect and the fifteenth triply so; a
 *     zero among them denotes a block that is not allocated.
 *   - The same, Locating an Inode: the group is (inode - 1) / s_inodes_per_group
 *     and the index within it (inode - 1) % s_inodes_per_group, inode numbers
 *     beginning at one and indices at zero.
 *   - The same, Defined Reserved Inodes and Defined i_mode Values: inode 2 is the
 *     root directory, and the file format occupies the high four bits of i_mode.
 *   - The same, the Directory Structure chapter and Table 4.1: a directory is a
 *     file whose data is a linked list of entries, each holding a 32-bit inode
 *     number at offset 0, a 16-bit record length at 4, a name length at 6 and a
 *     file type at 7, the name following at 8; entries are aligned upon four
 *     bytes and none spans two blocks; an inode number of zero marks an entry
 *     that is not in use; and the name length may never exceed the record length
 *     less eight.
 *   - The same, Table 4.2: the eight file types an entry may declare, which are
 *     numbered differently from the formats of i_mode and must agree with them.
 *   - The same, Table 4.3: the layout of a sample directory, against which the
 *     entries the self-test composes were checked.
 *   - Linux kernel documentation, filesystems/ext2.rst: the file type is an
 *     incompatible feature because a kernel unaware of it would read the name
 *     length as sixteen bits and believe a name longer than 256 characters.
 *   - The same, the Symbolic Links chapter: a symbolic link holds a text string
 *     interpreted as a path to another file; for a target shorter than 60 bytes
 *     the string is stored within the inode itself, in the fields that would
 *     otherwise hold the pointers to its data blocks, which avoids allocating a
 *     whole block for a string most links are shorter than.
 *   - Linux kernel documentation, the ext4 superblock, block group descriptor and
 *     inode tables, consulted as an independent statement of the same offsets.
 *     The two
 *     formats agree upon every field this kernel reads; they diverge only at
 *     descriptor offset 18, which EXT2 reserves as bg_pad and ext4 reuses as
 *     bg_flags, and which this kernel reads in neither sense.
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
 * The block group descriptor.
 *
 * The table of these begins upon the first block following the superblock —
 * block 2 upon a volume of 1024-byte blocks, block 1 upon any other — and holds
 * one descriptor for every group. Each descriptor locates the three structures
 * of its group: the block bitmap, the inode bitmap and the inode table.
 *
 * The offsets are declared here for the same reason the superblock's are: the
 * boot-time self-test composes a descriptor from these names, and a test that
 * stated them a second time would agree with a mistaken parser.
 */
#define EXT2_GROUP_DESCRIPTOR_SIZE 32U

#define EXT2_OFFSET_BG_BLOCK_BITMAP     0U
#define EXT2_OFFSET_BG_INODE_BITMAP     4U
#define EXT2_OFFSET_BG_INODE_TABLE      8U
#define EXT2_OFFSET_BG_FREE_BLOCKS      12U
#define EXT2_OFFSET_BG_FREE_INODES      14U
#define EXT2_OFFSET_BG_USED_DIRECTORIES 16U

/*
 * The inode.
 *
 * An inode describes one file — its format, its permissions, its size, its
 * times, and the blocks holding its data. It carries no name; names live in
 * directories alone.
 *
 * The structure occupies the first 128 bytes of whatever s_inode_size states.
 * A revision 1 volume may state a larger size, as mke2fs now does by default,
 * and the bytes beyond the 128th belong to extensions this kernel does not read;
 * EXT2_GOOD_OLD_INODE_SIZE is that 128 and is also the size a revision 0 volume
 * has no field in which to state.
 */
#define EXT2_OFFSET_I_MODE        0U
#define EXT2_OFFSET_I_UID         2U
#define EXT2_OFFSET_I_SIZE        4U
#define EXT2_OFFSET_I_ATIME       8U
#define EXT2_OFFSET_I_CTIME       12U
#define EXT2_OFFSET_I_MTIME       16U
#define EXT2_OFFSET_I_DTIME       20U
#define EXT2_OFFSET_I_GID         24U
#define EXT2_OFFSET_I_LINKS_COUNT 26U
#define EXT2_OFFSET_I_BLOCKS      28U
#define EXT2_OFFSET_I_FLAGS       32U
#define EXT2_OFFSET_I_OSD1        36U
#define EXT2_OFFSET_I_BLOCK       40U
#define EXT2_OFFSET_I_GENERATION  100U
#define EXT2_OFFSET_I_FILE_ACL    104U
#define EXT2_OFFSET_I_DIR_ACL     108U
#define EXT2_OFFSET_I_FADDR       112U

/*
 * The fifteen entries of i_block. The first twelve name blocks of the file
 * directly; the last three name blocks of pointers, one, two and three levels
 * deep. A zero entry at any level denotes a block that was never allocated —
 * a hole — and not the end of the file.
 */
#define EXT2_BLOCK_POINTER_COUNT   15U
#define EXT2_DIRECT_BLOCK_COUNT    12U
#define EXT2_INDIRECT_INDEX        12U
#define EXT2_DOUBLE_INDIRECT_INDEX 13U
#define EXT2_TRIPLE_INDIRECT_INDEX 14U

/* The width of a block pointer, in bytes, wherever one is stored. */
#define EXT2_BLOCK_POINTER_SIZE 4U

/* The reserved inodes. Numbers below s_first_ino belong to the filesystem. */
#define EXT2_BAD_INODE         1U
#define EXT2_ROOT_INODE        2U
#define EXT2_ACL_INDEX_INODE   3U
#define EXT2_ACL_DATA_INODE    4U
#define EXT2_BOOT_LOADER_INODE 5U
#define EXT2_UNDELETE_INODE    6U

/* The file format, held in the high four bits of i_mode. */
#define EXT2_S_IFMT   UINT16_C(0xF000)
#define EXT2_S_IFSOCK UINT16_C(0xC000)
#define EXT2_S_IFLNK  UINT16_C(0xA000)
#define EXT2_S_IFREG  UINT16_C(0x8000)
#define EXT2_S_IFBLK  UINT16_C(0x6000)
#define EXT2_S_IFDIR  UINT16_C(0x4000)
#define EXT2_S_IFCHR  UINT16_C(0x2000)
#define EXT2_S_IFIFO  UINT16_C(0x1000)

/* The overrides and the permission bits, which occupy the low twelve. */
#define EXT2_S_ISUID         UINT16_C(0x0800)
#define EXT2_S_ISGID         UINT16_C(0x0400)
#define EXT2_S_ISVTX         UINT16_C(0x0200)
#define EXT2_S_IRWXU         UINT16_C(0x01C0)
#define EXT2_S_IRWXG         UINT16_C(0x0038)
#define EXT2_S_IRWXO         UINT16_C(0x0007)
#define EXT2_PERMISSION_MASK UINT16_C(0x0FFF)

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


/*
 * One block group's descriptor, parsed into the processor's own order.
 *
 * Every block identifier here is absolute — a block number of the volume and
 * not of the group — which the specification states expressly, and which is the
 * one thing about this structure it is easy to assume wrongly.
 */
typedef struct Ext2GroupDescriptor
{
    uint32_t group; /* Which group this describes, for a report to name. */
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint16_t free_block_count;
    uint16_t free_inode_count;
    uint16_t used_directory_count;
} Ext2GroupDescriptor;

/* The first block of the group descriptor table, and the blocks it occupies. */
uint32_t Ext2GroupDescriptorBlock(const Ext2Superblock *superblock);
uint32_t Ext2GroupDescriptorBlocks(const Ext2Superblock *superblock);

/* The blocks the inode table of any one group occupies. */
uint32_t Ext2InodeTableBlocks(const Ext2Superblock *superblock);

/*
 * The first block of a group, and the blocks it holds. Every group holds
 * s_blocks_per_group blocks except the last, which holds what remains; a caller
 * that assumed otherwise would address blocks beyond the end of the volume.
 */
uint32_t Ext2GroupFirstBlock(const Ext2Superblock *superblock, uint32_t group);
uint32_t Ext2GroupBlockCount(const Ext2Superblock *superblock, uint32_t group);

/*
 * Reads one group's descriptor and validates it: the group must exist, each of
 * the three structures it locates must lie within the volume and be distinct
 * from the others, the inode table must fit within the volume, and the free
 * counts must not exceed what the group holds.
 *
 * Ext2LastError describes any refusal.
 */
bool Ext2ReadGroupDescriptor(BlockDevice *device, const Ext2Superblock *superblock,
                             uint32_t group, Ext2GroupDescriptor *descriptor);

/*
 * Reads and validates every descriptor of the table, and asserts that the free
 * blocks and free inodes the groups report sum to the totals the superblock
 * reports.
 *
 * The sum is checked only upon a volume marked cleanly unmounted: a volume that
 * was not is permitted to disagree with itself, that disagreement being what an
 * unclean unmount means, and it is already read-only.
 */
bool Ext2VerifyGroupDescriptors(BlockDevice *device, const Ext2Superblock *superblock);

/* How many descriptors have been read, and how many refused. */
uint64_t Ext2GroupsRead(void);
uint64_t Ext2GroupsRefused(void);

/* Writes a description of one group to the console. */
void Ext2ReportGroup(const Ext2GroupDescriptor *descriptor);


/*
 * One inode, parsed.
 *
 * The size is held as a single 64-bit quantity. A revision 1 volume stores the
 * high half of a regular file's size in the field a revision 0 volume calls
 * i_dir_acl, which is the format's own arrangement and not this kernel's, and
 * joining the halves here means nothing above must remember to.
 */
typedef struct Ext2Inode
{
    uint32_t number; /* The inode this is, for a report to name. */
    uint16_t mode;
    uint16_t uid;
    uint16_t gid;
    uint64_t size;
    uint32_t access_time;
    uint32_t change_time;
    uint32_t modify_time;
    uint32_t delete_time;
    uint16_t link_count;
    uint32_t sector_count; /* i_blocks: 512-byte sectors, not volume blocks. */
    uint32_t flags;
    uint32_t block[EXT2_BLOCK_POINTER_COUNT];
    uint32_t generation;
    uint32_t file_acl;
} Ext2Inode;

/*
 * Reads an inode by number and validates it.
 *
 * The inode is refused where the number is outside the volume, where the group
 * or its descriptor cannot be read, where a block pointer addresses a block the
 * volume does not hold, or where the inode is not in use — an inode with no
 * format and no links is a table entry that was never filled, and reading one is
 * how arithmetic that has strayed beyond the table announces itself.
 *
 * Ext2LastError describes any refusal.
 */
bool Ext2ReadInode(BlockDevice *device, const Ext2Superblock *superblock, uint32_t number,
                   Ext2Inode *inode);

/*
 * Resolves the index of a block within a file to the block of the volume that
 * holds it, following the indirect blocks as far as is needed.
 *
 * A resolved block of zero is a hole: a block the file never had allocated,
 * which reads as zeroes. That is distinct from a refusal, which means the index
 * is beyond what fifteen pointers can address or the volume contradicts itself.
 *
 * The index is not checked against the file's size. A caller reading a file
 * bounds itself by the size; a caller walking the blocks a file has allocated
 * does not, and the two must not be conflated here.
 */
bool Ext2InodeBlock(BlockDevice *device, const Ext2Superblock *superblock,
                    const Ext2Inode *inode, uint64_t index, uint32_t *block);

/* How many blocks of the volume the file's size spans. */
uint64_t Ext2InodeBlockCount(const Ext2Superblock *superblock, const Ext2Inode *inode);

/* The format of the file the inode describes. */
bool Ext2InodeIsDirectory(const Ext2Inode *inode);
bool Ext2InodeIsRegular(const Ext2Inode *inode);
bool Ext2InodeIsSymbolicLink(const Ext2Inode *inode);

/* How many inodes have been read, and how many refused. */
uint64_t Ext2InodesRead(void);
uint64_t Ext2InodesRefused(void);

/* Writes a description of one inode to the console. */
void Ext2ReportInode(const Ext2Inode *inode);

/*
 * The bytes of i_block a symbolic link's target occupies in place of the
 * pointers that would otherwise be there. Fifteen pointers of four bytes are
 * sixty, and a target shorter than that is stored within the inode rather than
 * being given a block of its own — an optimisation worth having, since nearly
 * every symbolic link on a system is shorter than sixty characters.
 */
#define EXT2_FAST_SYMLINK_CAPACITY (EXT2_BLOCK_POINTER_COUNT * EXT2_BLOCK_POINTER_SIZE)

/*
 * The longest target this kernel will read, and the number of links it will
 * follow in resolving one path.
 *
 * Neither is a property of the format, which bounds a target only by the size of
 * the file holding it and says nothing whatever about following one. Both are
 * bounds upon this kernel: the resolver follows a link by resolving its target,
 * which re-enters the resolver, so each is a stack frame carrying a target
 * buffer, and a link naming itself would otherwise recur until the stack was
 * gone. Eight is the depth POSIX requires an implementation to allow.
 */
#define EXT2_SYMLINK_MAXIMUM       255U
#define EXT2_SYMLINK_DEPTH_MAXIMUM 8U

/*
 * Reads bytes of a file, from an offset within it, and reports how many were
 * read.
 *
 * The read is bounded by the file's size. An offset at or beyond the end yields
 * no bytes and is not a failure — it is the end of the file, which every reader
 * must reach — so a caller distinguishes the end from an error by the count and
 * not by the return value. A read that crosses the end is shortened to it.
 *
 * A block the file never had allocated reads as zeroes rather than being
 * refused. That is what a hole means, and a sparse file is the ordinary case
 * rather than a curiosity.
 *
 * A directory is refused. Its bytes are entries and are read by traversing it;
 * a caller reading them as a stream has almost certainly mistaken what it holds.
 *
 * Upon failure the count reports the bytes transferred before it, and the
 * remainder of the buffer is indeterminate.
 */
bool Ext2ReadFile(BlockDevice *device, const Ext2Superblock *superblock,
                  const Ext2Inode *inode, uint64_t offset, void *buffer, uint64_t length,
                  uint64_t *read);

/*
 * Whether a symbolic link holds its target within the inode rather than in
 * blocks of the volume.
 *
 * The test is that the inode has no data blocks, and not that its size is below
 * sixty. The two agree upon every link a filesystem creates, the one being the
 * reason for the other; they part when the inode carries an extended attribute
 * block, which i_blocks counts and which is not data. Testing the size alone
 * would then read the target out of the pointers to a block that exists.
 */
bool Ext2InodeIsFastSymbolicLink(const Ext2Superblock *superblock, const Ext2Inode *inode);

/*
 * Reads the target of a symbolic link, terminating it.
 *
 * A target is not terminated upon the volume: its length is the file's size, as
 * for any other file. It is terminated here because what it is read for is to be
 * resolved as a path.
 *
 * Both forms are read. A target held within the inode is taken from the bytes of
 * i_block; one held in blocks is read as any other file is.
 */
bool Ext2ReadSymbolicLink(BlockDevice *device, const Ext2Superblock *superblock,
                          const Ext2Inode *inode, char *target, size_t capacity);

/* How many files have been read, how many bytes, and how many reads refused. */
uint64_t Ext2FilesRead(void);
uint64_t Ext2BytesRead(void);
uint64_t Ext2ReadsRefused(void);

/*
 * The directory entry.
 *
 * A directory is an ordinary file whose data is a sequence of these, and it is
 * the only place in the format where a name appears: an inode describes a file
 * entirely without naming it, and a name is a property of the directory that
 * holds it rather than of the file it leads to. That is why one file may bear
 * several names and why removing one of them need not remove the file.
 *
 * The entries of a block form a linked list, each stating the displacement to
 * the next rather than its own length, so that an entry may be removed by
 * lengthening the displacement of the entry before it and space may be reclaimed
 * without moving anything. The list runs to the end of the block and no further:
 * the last entry of a block states the displacement to the end of that block,
 * and the next block begins a new list.
 *
 * The offsets are declared here for the reason the superblock's are: the
 * boot-time self-test composes entries from these same names, and a test that
 * stated the offsets a second time would agree with a mistaken parser as readily
 * as with a correct one.
 */
#define EXT2_OFFSET_DE_INODE         0U
#define EXT2_OFFSET_DE_RECORD_LENGTH 4U
#define EXT2_OFFSET_DE_NAME_LENGTH   6U
#define EXT2_OFFSET_DE_FILE_TYPE     7U
#define EXT2_OFFSET_DE_NAME          8U

/* The bytes an entry occupies before its name begins. */
#define EXT2_DIRECTORY_HEADER_SIZE 8U

/* The boundary every entry begins upon, and every record length is a multiple of. */
#define EXT2_DIRECTORY_ALIGNMENT 4U

/*
 * The greatest length of a name, in bytes, and the reason the format has two
 * readings of the two bytes at offset 6.
 *
 * Revision 0 held a 16-bit name length there. Since no implementation ever
 * permitted a name beyond 255 bytes the upper byte was always zero, and it was
 * reclaimed as the file type — which is why the file type is an *incompatible*
 * feature and not a compatible one: a kernel that did not know of it would read
 * the type as the high byte of the length and believe the name to be thousands
 * of bytes long. Which reading applies is therefore not a matter of the
 * revision but of EXT2_FEATURE_INCOMPAT_FILETYPE, and this kernel decides it
 * from that flag alone.
 */
#define EXT2_NAME_MAXIMUM 255U

/*
 * The file types an entry may declare, which are not the file formats i_mode
 * holds and are not numbered in the same order as them. The two must agree, and
 * Ext2FileTypeOfMode is the translation between them.
 */
#define EXT2_FT_UNKNOWN  0U
#define EXT2_FT_REG_FILE 1U
#define EXT2_FT_DIR      2U
#define EXT2_FT_CHRDEV   3U
#define EXT2_FT_BLKDEV   4U
#define EXT2_FT_FIFO     5U
#define EXT2_FT_SOCK     6U
#define EXT2_FT_SYMLINK  7U

/* The character separating the components of a path. */
#define EXT2_PATH_SEPARATOR '/'

/*
 * The longest path this kernel will resolve. The format imposes no such limit,
 * a path being no part of the volume at all; the limit is upon the caller, and
 * exists so that a string that was never terminated is refused rather than
 * walked until it meets something that faults.
 */
#define EXT2_PATH_MAXIMUM 4096U

/*
 * One directory entry, parsed.
 *
 * The name is held terminated, which it is not upon the volume: there the length
 * is a field and the name is not terminated at all, so a name may contain
 * anything the length admits. The parser refuses a name containing the separator
 * or a null byte, both of which would make the name unaddressable by the very
 * path resolution the entry exists to serve.
 *
 * The block and the offset the entry was read from are retained. Nothing in this
 * sub-task uses them; the insertion and removal of entries in sub-task 5.7 must
 * know where an entry stands in order to alter the one before it, and an entry
 * that did not remember where it came from would have to be found a second time.
 */
typedef struct Ext2DirectoryEntry
{
    uint32_t inode;
    uint16_t record_length;
    uint16_t name_length;
    uint8_t file_type; /* EXT2_FT_UNKNOWN upon a volume that declares no types. */
    char name[EXT2_NAME_MAXIMUM + 1U];
    uint32_t block;  /* The block of the volume the entry was read from. */
    uint32_t offset; /* Its offset within that block. */
} Ext2DirectoryEntry;

/*
 * A position within a directory.
 *
 * The cursor holds the directory rather than copying it, an inode being some
 * hundred and forty bytes and the kernel stack being small. The directory must
 * therefore outlive the cursor, which every caller here satisfies by declaring
 * the two beside one another.
 */
typedef struct Ext2DirectoryCursor
{
    const Ext2Inode *directory;
    uint64_t index;  /* Which block of the directory, counted from zero. */
    uint32_t offset; /* The byte within that block the next entry begins at. */
} Ext2DirectoryCursor;

/*
 * What one step of a traversal produced.
 *
 * The three outcomes are distinguished because two of them are ordinary and one
 * is not: a directory that has ended has been read correctly and completely, and
 * a caller that could not tell that from a volume it failed to read would stop
 * at the first bad block and report the directory finished.
 */
typedef enum Ext2DirectoryStep
{
    EXT2_DIRECTORY_ENTRY_READ, /* An entry was produced. */
    EXT2_DIRECTORY_END,        /* The directory holds no further entry. */
    EXT2_DIRECTORY_FAILED      /* The directory could not be read or is malformed. */
} Ext2DirectoryStep;

/* Places a cursor before the first entry of a directory. */
void Ext2DirectoryOpen(Ext2DirectoryCursor *cursor, const Ext2Inode *directory);

/*
 * Produces the next entry of a directory and advances the cursor past it.
 *
 * Entries that name no inode are passed over rather than produced. Such an entry
 * is not corruption: it is how a name is removed, the record remaining to hold
 * the space it occupied until something else claims it, and a caller listing a
 * directory has no use for it.
 *
 * A block the directory never had allocated holds no entries and is passed over
 * likewise, which is what distinguishes a hole from a block of zeroes that would
 * otherwise be read as an entry of no length and refused.
 *
 * Ext2LastError describes any failure.
 */
Ext2DirectoryStep Ext2DirectoryNext(BlockDevice *device, const Ext2Superblock *superblock,
                                    Ext2DirectoryCursor *cursor, Ext2DirectoryEntry *entry);

/*
 * Finds the entry of a directory bearing a name, which is given by its address
 * and its length and need not be terminated.
 *
 * The length is explicit so that one component of a path may be looked up where
 * it stands, without first being copied out of the path into a buffer of its
 * own. Names are compared by their bytes and their length alone: the format
 * attributes no meaning to case, to an encoding, or to any character but the
 * separator, which cannot occur within a name.
 *
 * Returns false where the directory holds no such entry, which is an ordinary
 * outcome and not a fault of the volume; Ext2LastError distinguishes it.
 */
bool Ext2DirectoryFind(BlockDevice *device, const Ext2Superblock *superblock,
                       const Ext2Inode *directory, const char *name, size_t length,
                       Ext2DirectoryEntry *entry);

/*
 * Resolves an absolute path to the inode it names, beginning at the root
 * directory, which the format fixes as inode 2.
 *
 * Repeated separators are equivalent to one, and a trailing separator asserts
 * that what the path names is a directory. The entries "." and ".." are not
 * treated specially: every EXT2 directory holds them upon the volume, the ".."
 * of the root naming the root itself, so the ordinary lookup resolves them and
 * a kernel that interpreted them here would be second-guessing the volume.
 *
 * A symbolic link met anywhere in the path is followed, including as the last
 * component, which is the behaviour of every operation that acts upon a file
 * rather than upon the name of one. A target is resolved against the directory
 * holding the link where it is relative, and from the root where it is absolute.
 * At most EXT2_SYMLINK_DEPTH_MAXIMUM links are followed in one resolution, a
 * link naming itself being otherwise unbounded.
 */
bool Ext2ResolvePath(BlockDevice *device, const Ext2Superblock *superblock, const char *path,
                     Ext2Inode *inode);

/*
 * The same, save that a symbolic link standing as the last component is returned
 * as it stands rather than followed. Links within the path are followed as
 * before: it is the file the path names that is left unresolved, not the route
 * taken to it.
 *
 * This is the distinction between acting upon a file and acting upon its name,
 * and both are needed — the first by anything that opens a file, the second by
 * anything that must see the link itself. A trailing separator overrides it: a
 * path that asserts a directory is asking for what the link names, since a link
 * is not one.
 */
bool Ext2ResolvePathNoFollow(BlockDevice *device, const Ext2Superblock *superblock,
                             const char *path, Ext2Inode *inode);

/* The entry file type corresponding to the format held in i_mode. */
uint8_t Ext2FileTypeOfMode(uint16_t mode);

/* The name of an entry file type, for a report. */
const char *Ext2FileTypeName(uint8_t type);

/* How many entries have been read, and how many refused as malformed. */
uint64_t Ext2EntriesRead(void);
uint64_t Ext2EntriesRefused(void);

/* How many paths have been resolved, and how many lookups have failed. */
uint64_t Ext2PathsResolved(void);
uint64_t Ext2PathsRefused(void);

/* Writes a description of one entry, and of a whole directory, to the console. */
void Ext2ReportDirectoryEntry(const Ext2DirectoryEntry *entry);
void Ext2ReportDirectory(BlockDevice *device, const Ext2Superblock *superblock,
                         const Ext2Inode *directory);

#endif /* OXYS_EXT2_H */
