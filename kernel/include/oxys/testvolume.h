/*
 * File: kernel/include/oxys/testvolume.h
 * Purpose: Declares the fixture the storage, filesystem and virtual filesystem
 *          self-tests are conducted upon: two block devices backed by memory,
 *          and the EXT2 volume composed within them.
 * Key definitions: KernelMemoryDeviceStore, KernelMemoryDeviceSecondStore,
 *          KernelMemoryDeviceOperations, KernelMemoryDeviceReadOnlyOperations,
 *          KernelComposeVolume, KernelFileBuffer, KernelFileByteAt, and the
 *          geometry of the composed volume.
 * References:
 *   - docs/storage/EXT2.md, Section 4: the layout of the composed volume, block
 *     by block and inode by inode, and the reason each thing within it is there.
 *   - docs/storage/VFS.md, Section 10: the second volume, and why it is a copy
 *     of the first with exactly one field altered.
 *   - The EXT2 field offsets themselves are in <oxys/ext2.h> and are not
 *     restated here: a fixture that restated them would agree with a mistaken
 *     decoder as readily as with a correct one.
 *
 * Why a fixture, and why in memory.
 *
 * A self-test needs a volume whose every field it knows, and no machine can be
 * relied upon to carry one. The volume below is therefore built by this kernel,
 * byte by byte, into an array in .bss, and a block device driver is supplied
 * that reads and writes that array. Every assertion the storage and filesystem
 * tests make is made against it, so those tests run identically upon a machine
 * with no disk.
 *
 * There are two devices because a mount must be asserted to have been crossed,
 * and a crossing cannot be observed between two volumes that are the same. The
 * second is a copy of the first with one field altered — the owner of one file —
 * so that an assertion can state which volume a path reached. Two wholly
 * different volumes would leave it unclear which difference had been detected.
 *
 * The composed volume is not a substitute for a real one. It shares this
 * kernel's understanding of the format, so a misreading of the specification
 * would be composed into it and asserted against itself. That is what the
 * diagnostic probes of <oxys/verify.h> and the mke2fs corroboration recorded in
 * docs/project/TESTING.md are for.
 */

#ifndef OXYS_TESTVOLUME_H
#define OXYS_TESTVOLUME_H

#include <oxys/types.h>
#include <oxys/block.h>

/*
 * A block device backed by memory, existing only for the self-test below.
 *
 * The disk this kernel can reach is not a fit subject for a test of the layer
 * above it: the machine that `make verify` runs upon has no ATA device at all,
 * and a machine that does has data upon it that a self-test must not write to.
 * A device of known contents, of a known size, whose every transfer succeeds,
 * makes the layer testable everywhere and testable exactly — the assertions
 * below can state what a read must return rather than merely that it returned.
 */
#define KERNEL_MEMORY_DEVICE_BLOCKS 256U

/* The regular file within the subdirectory: two blocks, the second partly
 * filled, so that a read crossing a block boundary and a read ending within a
 * block are both exercised. */
#define KERNEL_VOLUME_INNER_SIZE 1500U

/*
 * The layout of the composed volume, in blocks of 1024 bytes.
 *
 * Block 0 is the boot block, block 1 holds the superblock, and the group
 * descriptor table follows it. The remainder is laid out as mke2fs would lay it
 * out: the two bitmaps, then the inode table, then the data blocks. The volume
 * is 128 blocks and the device of memory is 256 blocks of 512 bytes, so the two
 * are exactly the same length.
 */
#define KERNEL_VOLUME_BLOCK_SIZE       1024U
#define KERNEL_VOLUME_DESCRIPTOR_BLOCK 2U
#define KERNEL_VOLUME_BLOCK_BITMAP     3U
#define KERNEL_VOLUME_INODE_BITMAP     4U
#define KERNEL_VOLUME_INODE_TABLE      5U

/*
 * The inodes the volume holds. Thirty-two rather than sixteen since sub-task
 * 5.7, which creates files and directories and therefore needs inodes to create
 * them with; the table accordingly occupies four blocks rather than two, and the
 * data blocks begin two blocks later than they did.
 */
#define KERNEL_VOLUME_INODES           32U
#define KERNEL_VOLUME_USED_INODES      15U

#define KERNEL_VOLUME_GROUP_BLOCKS     127U
#define KERNEL_VOLUME_USED_BLOCKS      (KERNEL_VOLUME_LAST_BLOCK - 1U)
#define KERNEL_VOLUME_FREE_BLOCKS      (KERNEL_VOLUME_GROUP_BLOCKS - KERNEL_VOLUME_USED_BLOCKS)
#define KERNEL_VOLUME_FREE_INODES      (KERNEL_VOLUME_INODES - KERNEL_VOLUME_USED_INODES)
#define KERNEL_VOLUME_DIRECTORIES      2U

/*
 * The blocks of the composed file, chosen so that every level of the
 * indirection is exercised and every one of them lies within the 128 blocks the
 * volume holds. Blocks 7 to 18 are the twelve direct blocks; the rest are the
 * pointer blocks and the data blocks they lead to.
 */
#define KERNEL_VOLUME_FILE_INODE      11U
#define KERNEL_VOLUME_DIRECT_FIRST    9U
#define KERNEL_VOLUME_INDIRECT        21U
#define KERNEL_VOLUME_INDIRECT_DATA   22U
#define KERNEL_VOLUME_INDIRECT_LAST   23U
#define KERNEL_VOLUME_DOUBLE          24U
#define KERNEL_VOLUME_DOUBLE_LEVEL    25U
#define KERNEL_VOLUME_DOUBLE_DATA     26U
#define KERNEL_VOLUME_TRIPLE          27U
#define KERNEL_VOLUME_TRIPLE_DOUBLE   28U
#define KERNEL_VOLUME_TRIPLE_INDIRECT 29U
#define KERNEL_VOLUME_TRIPLE_DATA     30U
#define KERNEL_VOLUME_ROOT_DATA       31U
#define KERNEL_VOLUME_SUB_DATA        32U
#define KERNEL_VOLUME_INNER_DATA      33U
#define KERNEL_VOLUME_INNER_DATA_LAST 34U
#define KERNEL_VOLUME_LINK_DATA       35U
#define KERNEL_VOLUME_LAST_BLOCK      36U

/*
 * The two inodes the directories lead to besides the file: a subdirectory of the
 * root, and a regular file within it. They exist so that a path of more than one
 * component may be resolved, which is the whole of what distinguishes resolution
 * from a single lookup.
 */
#define KERNEL_VOLUME_SUB_INODE   12U
#define KERNEL_VOLUME_INNER_INODE 13U

/*
 * An inode of the table that is deliberately left empty, so that a self-test may
 * assert that an entry never filled is refused. It is named rather than reached
 * by arithmetic upon the inode before it: the inodes in use grow as the composed
 * volume acquires structure, and a test that took the next number after some
 * other inode would begin asserting the wrong thing without saying so.
 */
#define KERNEL_VOLUME_UNUSED_INODE 14U

/*
 * The two symbolic links, one of each form. A target shorter than sixty bytes is
 * held within the inode, in the fields that would otherwise be block pointers;
 * a longer one is held in a block like any other file. Both forms are composed
 * because they are read by entirely different code and a volume carrying only
 * the common one would leave half of that code unexercised.
 */
#define KERNEL_VOLUME_FAST_LINK_INODE 15U
#define KERNEL_VOLUME_SLOW_LINK_INODE 16U

/* What each names. The fast target is relative and is resolved against the
 * directory holding the link; the slow one is absolute and long enough to
 * require a block, which is what makes it slow. */
#define KERNEL_VOLUME_FAST_LINK_TARGET "sub"
#define KERNEL_VOLUME_SLOW_LINK_TARGET \
    "/sub/../sub/../sub/../sub/../sub/../sub/../sub/../sub/../sub/inner"

/* How many pointers a block of the composed volume holds: 1024 / 4. */
#define KERNEL_VOLUME_POINTERS 256U

/* The size given to the composed file, which is not what the resolver uses. */
#define KERNEL_VOLUME_FILE_SIZE 274432U

/* ------------------------------------------------------------------------------
 * The block devices.
 *
 * The driver selects between the two stores by the `context` it is registered
 * with, so one pair of routines serves both. The read-only operations vector
 * omits the writer, which is how a device that refuses to be written is
 * presented to the layers above.
 * ------------------------------------------------------------------------------ */

extern uint8_t KernelMemoryDeviceStore[KERNEL_MEMORY_DEVICE_BLOCKS * BLOCK_SIZE_DEFAULT];
extern uint8_t KernelMemoryDeviceSecondStore[KERNEL_MEMORY_DEVICE_BLOCKS * BLOCK_SIZE_DEFAULT];

extern const BlockOperations KernelMemoryDeviceOperations;
extern const BlockOperations KernelMemoryDeviceReadOnlyOperations;

/* ------------------------------------------------------------------------------
 * The composed volume.
 * ------------------------------------------------------------------------------ */

/*
 * Composes the volume into KernelMemoryDeviceStore, and the second volume into
 * KernelMemoryDeviceSecondStore. It may be called again to restore both after a
 * test has written to them, which is how each test begins from a known state
 * whatever the one before it did.
 */
void KernelComposeVolume(void);

/*
 * The byte a composed file holds at a given offset, being derived from the
 * offset itself. A read that returned the right number of bytes from the wrong
 * block would otherwise pass.
 */
uint8_t KernelFileByteAt(uint64_t offset);

/* The buffer a test reads a file into, and the two judgements made upon it. */
extern uint8_t KernelFileBuffer[KERNEL_VOLUME_INNER_SIZE + 64U];
bool KernelFileBufferMatches(uint64_t offset, uint64_t length);
bool KernelFileBufferIsZero(uint64_t length);

/* ------------------------------------------------------------------------------
 * Addressing the composed volume directly.
 *
 * These give the offset within the store at which a given field stands, so that
 * a test may alter one field and assert that the alteration is refused. The
 * offsets are those <oxys/ext2.h> names; nothing here restates them.
 * ------------------------------------------------------------------------------ */

size_t KernelVolumeBlock(uint32_t block);
size_t KernelDescriptorField(size_t offset);
size_t KernelInodeField(uint32_t number, size_t offset);
size_t KernelPointerField(uint32_t block, uint32_t entry);
size_t KernelInodeBlockField(uint32_t number, uint32_t entry);

/* Writing into the store, in the volume's byte order. */
void KernelStoreHalf(size_t offset, uint16_t value);
void KernelStoreWord(size_t offset, uint32_t value);
void KernelStoreText(size_t offset, const char *text);

/* Writing a field of the superblock, which stands at a fixed offset. */
void KernelSetVolumeHalf(size_t offset, uint16_t value);
void KernelSetVolumeWord(size_t offset, uint32_t value);

/* Recomposing the group descriptor alone, after a test has altered the counts
 * the descriptor summarises. */
void KernelComposeGroupDescriptor(void);

/* Whether two null-terminated strings are equal. The kernel has no string
 * library before Phase 7, and every test that compares a name needs this. */
bool KernelSameString(const char *left, const char *right);

#endif /* OXYS_TESTVOLUME_H */
