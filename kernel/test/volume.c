/*
 * File: kernel/test/volume.c
 * Purpose: Builds the fixture the storage, filesystem and virtual filesystem
 *          self-tests are conducted upon: two block devices backed by arrays in
 *          .bss, and the EXT2 volume composed within them byte by byte.
 * Key functions: KernelComposeVolume, KernelFileByteAt, KernelVolumeBlock,
 *          KernelInodeField, KernelDescriptorField, KernelPointerField,
 *          KernelInodeBlockField, KernelStoreHalf, KernelStoreWord,
 *          KernelStoreText, KernelSetVolumeHalf, KernelSetVolumeWord,
 *          KernelComposeGroupDescriptor, KernelFileBufferMatches,
 *          KernelFileBufferIsZero, KernelSameString.
 * References:
 *   - docs/storage/EXT2.md, Section 4: the layout composed below, block by block
 *     and inode by inode, and the reason each thing within it is there.
 *   - docs/storage/VFS.md, Section 10: the second volume, and why it differs
 *     from the first in exactly one field.
 *   - The Second Extended File System, Dave Poirier: the format. Every field
 *     offset written below is named in <oxys/ext2.h>; none is restated here,
 *     because a fixture that restated an offset would agree with a mistaken
 *     decoder as readily as with a correct one.
 *
 * Why the volume is composed rather than read.
 *
 * A self-test needs a volume whose every field it knows, and no machine can be
 * relied upon to carry one. This file therefore writes a complete EXT2 volume
 * into an array and supplies a block device that reads and writes it, so that
 * every assertion the storage and filesystem tests make holds upon a machine
 * with no disk at all.
 *
 * The composition is not evidence that this kernel reads the format correctly.
 * It shares the kernel\'s understanding of it, so a misreading of the
 * specification would be composed into the volume and then asserted against
 * itself. That is what the diagnostic probes of <oxys/verify.h> and the mke2fs
 * corroboration of docs/project/TESTING.md are for, and it is how the defect in
 * the recorded deletion time was found.
 *
 * The composition is idempotent. It may be called again to restore both volumes
 * after a test has written to them, which is how each test begins from a known
 * state whatever the one before it did.
 */

#include <oxys/kernel.h>
#include <oxys/testvolume.h>
#include <oxys/block.h>
#include <oxys/ext2.h>

uint8_t KernelMemoryDeviceStore[KERNEL_MEMORY_DEVICE_BLOCKS * BLOCK_SIZE_DEFAULT];

/*
 * A second store, and with it a second device.
 *
 * The filesystem layer of sub-task 5.8 joins several volumes into one tree, and
 * a layer that composes a tree from one volume has not been shown to compose one
 * at all: crossing a mount point, and returning across it by "..", are exactly
 * the properties that do not arise until a second volume exists. The second
 * store is what supplies one. It is filled by copying the first and then
 * altering one field, so that the two volumes are identical in every respect but
 * the one an assertion can name — which is what makes an assertion able to say
 * *which* volume a path reached, and not merely that it reached one.
 */
uint8_t KernelMemoryDeviceSecondStore[KERNEL_MEMORY_DEVICE_BLOCKS * BLOCK_SIZE_DEFAULT];

/*
 * The store a device addresses. The driver's context selects it: a null context
 * is the first store, which every device registered before sub-task 5.8 passes
 * and which therefore needs no alteration.
 */
static uint8_t *KernelMemoryDeviceStoreOf(void *context)
{
    return (context == NULL) ? KernelMemoryDeviceStore : (uint8_t *)context;
}

static bool KernelMemoryDeviceRead(void *context, uint64_t block, uint32_t count, void *buffer)
{
    uint8_t *const destination = (uint8_t *)buffer;
    const uint8_t *const store = KernelMemoryDeviceStoreOf(context);
    const size_t offset = (size_t)block * BLOCK_SIZE_DEFAULT;

    for (size_t index = 0U; index < ((size_t)count * BLOCK_SIZE_DEFAULT); ++index)
    {
        destination[index] = store[offset + index];
    }

    return true;
}

static bool KernelMemoryDeviceWrite(void *context, uint64_t block, uint32_t count,
                                    const void *buffer)
{
    const uint8_t *const source = (const uint8_t *)buffer;
    uint8_t *const store = KernelMemoryDeviceStoreOf(context);
    const size_t offset = (size_t)block * BLOCK_SIZE_DEFAULT;

    for (size_t index = 0U; index < ((size_t)count * BLOCK_SIZE_DEFAULT); ++index)
    {
        store[offset + index] = source[index];
    }

    return true;
}

const BlockOperations KernelMemoryDeviceOperations = { KernelMemoryDeviceRead,
                                                              KernelMemoryDeviceWrite };

/* The same device without a writer, for the read-only assertions. */
const BlockOperations KernelMemoryDeviceReadOnlyOperations = { KernelMemoryDeviceRead,
                                                                      NULL };

/*
 * Reports the root directory of a volume and the blocks it occupies.
 *
 * The root is inode 2 upon every EXT2 volume, so it is the one inode that can be
 * named without reading a directory first, and it is therefore the corroboration
 * available at every boot: a volume this kernel did not compose, whose root
 * inode and block list may be compared against what made the volume.
 *
 * A directory of any size would fill the log, so the list is a prefix and not
 * the whole of it. The prefix is followed by the block standing at the first
 * index of each indirect range the directory is long enough to reach, named by
 * its index. Those are the resolutions worth reporting: a prefix alone shows
 * only that the direct pointers were copied out of the inode, whereas the block
 * at index 12, at 12 + pointers-per-block, and at 12 + pointers + pointers
 * squared can each be reached only by following one, two or three blocks of
 * pointers, and each can be compared against what made the volume.
 */

/*
 * The byte a composed file holds at an offset within itself.
 *
 * The value depends upon the offset, so a read that returned the right number of
 * bytes from the wrong place fails: a constant fill, or a pattern repeating
 * every block, would be returned identically by a reader that resolved the wrong
 * block, and the test would pass upon a defect it exists to catch.
 */
uint8_t KernelFileByteAt(uint64_t offset)
{
    return (uint8_t)(((offset * 31U) + 7U) & 0xFFU);
}

/*
 * The buffer a file is read into or written from.
 *
 * It is static rather than automatic because it is larger than a page and the
 * kernel stack, though 64 KiB, is shared with every interrupt taken while this
 * runs. Nothing here is concurrent, so one buffer suffices.
 */
uint8_t KernelFileBuffer[KERNEL_VOLUME_INNER_SIZE + 64U];

/*
 * The composition of an EXT2 superblock within the device of memory, so that
 * the parser may be asserted against a volume whose every field is known.
 *
 * No machine this kernel is verified upon carries an EXT2 volume, and one that
 * did would carry somebody's data. The superblock is therefore built here, field
 * by field, from the offsets the format defines — the same names the parser
 * reads, so that a mistaken offset cannot agree with itself.
 */
void KernelStoreHalf(size_t offset, uint16_t value)
{
    uint8_t *const field = &KernelMemoryDeviceStore[offset];

    field[0] = (uint8_t)(value & 0xFFU);
    field[1] = (uint8_t)((value >> 8) & 0xFFU);
}

void KernelStoreWord(size_t offset, uint32_t value)
{
    uint8_t *const field = &KernelMemoryDeviceStore[offset];

    field[0] = (uint8_t)(value & 0xFFU);
    field[1] = (uint8_t)((value >> 8) & 0xFFU);
    field[2] = (uint8_t)((value >> 16) & 0xFFU);
    field[3] = (uint8_t)((value >> 24) & 0xFFU);
}

/* The same, addressed by a field's offset within the superblock. */
void KernelSetVolumeHalf(size_t offset, uint16_t value)
{
    KernelStoreHalf(EXT2_SUPERBLOCK_OFFSET + offset, value);
}

void KernelSetVolumeWord(size_t offset, uint32_t value)
{
    KernelStoreWord(EXT2_SUPERBLOCK_OFFSET + offset, value);
}

/*
 * The free counts, which must agree with the bitmaps composed below and with one
 * another. Until sub-task 5.6 the bitmaps were zeroes and the counts were
 * whatever the composition said, nothing having read a bitmap; an allocator
 * reads them, so they now describe the same volume or they describe none.
 *
 * The group holds 127 blocks — 128 less the boot block, the volume's first data
 * block being 1 — of which blocks 1 to 33 are the metadata and the composed
 * files. Sixteen inodes, of which every one but 14 is in use; 14 is the inode
 * the self-test of sub-task 5.3 requires to be empty, and is now also the only
 * one an allocation can be given.
 */

/* The byte at which a block of the composed volume begins. */
size_t KernelVolumeBlock(uint32_t block)
{
    return (size_t)block * KERNEL_VOLUME_BLOCK_SIZE;
}

/* A field of the group descriptor of group 0, which lies first in the table. */
size_t KernelDescriptorField(size_t offset)
{
    return KernelVolumeBlock(KERNEL_VOLUME_DESCRIPTOR_BLOCK) + offset;
}









/* A field of an inode, the table beginning at KERNEL_VOLUME_INODE_TABLE. */
size_t KernelInodeField(uint32_t number, size_t offset)
{
    return KernelVolumeBlock(KERNEL_VOLUME_INODE_TABLE) +
           ((size_t)(number - 1U) * EXT2_GOOD_OLD_INODE_SIZE) + offset;
}

/* One entry of a block of pointers. */
size_t KernelPointerField(uint32_t block, uint32_t entry)
{
    return KernelVolumeBlock(block) + ((size_t)entry * EXT2_BLOCK_POINTER_SIZE);
}

/* One of the fifteen block pointers of an inode. */
size_t KernelInodeBlockField(uint32_t number, uint32_t entry)
{
    return KernelInodeField(number, EXT2_OFFSET_I_BLOCK + ((size_t)entry *
                                                           EXT2_BLOCK_POINTER_SIZE));
}

/*
 * Composes an inode table and the blocks of pointers one of its inodes leads to.
 *
 * Inode 2 is the root directory, as the format reserves it, with a single direct
 * block. Inode 11 is a regular file whose fifteen pointers reach every level of
 * the indirection: twelve direct blocks, an indirect block whose first and last
 * entries are used and whose second is a hole, a doubly indirect block, and a
 * triply indirect block. The holes are deliberate — a sparse file is the usual
 * case and not a curiosity, and a resolver that mistook a hole for the end of
 * the file would be wrong upon most of the files a system holds.
 */
static void KernelComposeInodes(void)
{
    for (size_t index = KernelVolumeBlock(KERNEL_VOLUME_INODE_TABLE);
         index < KernelVolumeBlock(KERNEL_VOLUME_LAST_BLOCK); ++index)
    {
        KernelMemoryDeviceStore[index] = 0U;
    }

    /* The root directory: one block, three links — its own entry, the entry it
     * holds for itself, and the parent entry of the one subdirectory. */
    KernelStoreHalf(KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFDIR | 0x01EDU));
    KernelStoreWord(KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_SIZE),
                    KERNEL_VOLUME_BLOCK_SIZE);
    KernelStoreHalf(KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_LINKS_COUNT), 3U);
    KernelStoreWord(KernelInodeField(EXT2_ROOT_INODE, EXT2_OFFSET_I_BLOCKS), 2U);
    KernelStoreWord(KernelInodeBlockField(EXT2_ROOT_INODE, 0U), KERNEL_VOLUME_ROOT_DATA);

    /* The subdirectory: one block, two links — its own entry and the root's. */
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_SUB_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFDIR | 0x01EDU));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_SUB_INODE, EXT2_OFFSET_I_SIZE),
                    KERNEL_VOLUME_BLOCK_SIZE);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_SUB_INODE, EXT2_OFFSET_I_LINKS_COUNT), 2U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_SUB_INODE, EXT2_OFFSET_I_BLOCKS), 2U);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_SUB_INODE, 0U), KERNEL_VOLUME_SUB_DATA);

    /* The regular file within the subdirectory. */
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_INNER_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFREG | 0x01A4U));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_INNER_INODE, EXT2_OFFSET_I_SIZE),
                    KERNEL_VOLUME_INNER_SIZE);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_INNER_INODE, EXT2_OFFSET_I_LINKS_COUNT), 1U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_INNER_INODE, EXT2_OFFSET_I_BLOCKS), 4U);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_INNER_INODE, 0U),
                    KERNEL_VOLUME_INNER_DATA);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_INNER_INODE, 1U),
                    KERNEL_VOLUME_INNER_DATA_LAST);

    /*
     * The two symbolic links. The fast one declares no sectors, which is what
     * says its target is within the inode; the slow one declares the two sectors
     * of its single block, which is what says the target is not.
     */
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFLNK | 0x01FFU));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_SIZE),
                    (uint32_t)(sizeof(KERNEL_VOLUME_FAST_LINK_TARGET) - 1U));
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_LINKS_COUNT),
                    1U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_BLOCKS), 0U);

    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_SLOW_LINK_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFLNK | 0x01FFU));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_SLOW_LINK_INODE, EXT2_OFFSET_I_SIZE),
                    (uint32_t)(sizeof(KERNEL_VOLUME_SLOW_LINK_TARGET) - 1U));
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_SLOW_LINK_INODE, EXT2_OFFSET_I_LINKS_COUNT),
                    1U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_SLOW_LINK_INODE, EXT2_OFFSET_I_BLOCKS), 2U);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_SLOW_LINK_INODE, 0U),
                    KERNEL_VOLUME_LINK_DATA);

    /* The file. */
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_MODE),
                    (uint16_t)(EXT2_S_IFREG | 0x01A4U));
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_SIZE),
                    KERNEL_VOLUME_FILE_SIZE);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_LINKS_COUNT), 1U);
    KernelStoreWord(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_BLOCKS), 32U);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_UID), 1000U);
    KernelStoreHalf(KernelInodeField(KERNEL_VOLUME_FILE_INODE, EXT2_OFFSET_I_GID), 1001U);

    for (uint32_t entry = 0U; entry < EXT2_DIRECT_BLOCK_COUNT; ++entry)
    {
        KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, entry),
                        KERNEL_VOLUME_DIRECT_FIRST + entry);
    }

    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, EXT2_INDIRECT_INDEX),
                    KERNEL_VOLUME_INDIRECT);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, EXT2_DOUBLE_INDIRECT_INDEX),
                    KERNEL_VOLUME_DOUBLE);
    KernelStoreWord(KernelInodeBlockField(KERNEL_VOLUME_FILE_INODE, EXT2_TRIPLE_INDIRECT_INDEX),
                    KERNEL_VOLUME_TRIPLE);

    /* The indirect block: its first and last entries used, its second a hole. */
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_INDIRECT, 0U), KERNEL_VOLUME_INDIRECT_DATA);
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_INDIRECT, KERNEL_VOLUME_POINTERS - 1U),
                    KERNEL_VOLUME_INDIRECT_LAST);

    /* The doubly indirect block, and the indirect block beneath it. */
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_DOUBLE, 0U), KERNEL_VOLUME_DOUBLE_LEVEL);
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_DOUBLE_LEVEL, 5U),
                    KERNEL_VOLUME_DOUBLE_DATA);

    /* The triply indirect block, and the two levels beneath it. */
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_TRIPLE, 0U), KERNEL_VOLUME_TRIPLE_DOUBLE);
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_TRIPLE_DOUBLE, 0U),
                    KERNEL_VOLUME_TRIPLE_INDIRECT);
    KernelStoreWord(KernelPointerField(KERNEL_VOLUME_TRIPLE_INDIRECT, 3U),
                    KERNEL_VOLUME_TRIPLE_DATA);
}

/* Writes a terminated string into the device's storage, without its terminator,
 * a target upon the volume being bounded by the file's size and not terminated. */
void KernelStoreText(size_t offset, const char *text)
{
    for (size_t index = 0U; text[index] != '\0'; ++index)
    {
        KernelMemoryDeviceStore[offset + index] = (uint8_t)text[index];
    }
}

/*
 * Composes the contents of the files: the two blocks of the regular file within
 * the subdirectory, one block of the sparse file so that a block holding data
 * may be contrasted with the hole beside it, and the target of the symbolic link
 * too long to be held within its inode.
 *
 * The fast link's target is stored in the fifteen words of i_block, which is
 * where the pointers would otherwise be. It is written here as the bytes the
 * volume holds, in the volume's own order, so that the parser recovers it by the
 * decoding it applies to every other field rather than by agreement with this.
 */
static void KernelComposeFiles(void)
{
    const size_t indirect = KernelVolumeBlock(KERNEL_VOLUME_INDIRECT_DATA);

    /*
     * The file occupies two blocks, which are addressed through the pointers the
     * inode holds rather than by relying upon their being adjacent in the store.
     */
    for (uint64_t offset = 0U; offset < KERNEL_VOLUME_INNER_SIZE; ++offset)
    {
        const uint32_t block = (offset < KERNEL_VOLUME_BLOCK_SIZE)
                                   ? KERNEL_VOLUME_INNER_DATA
                                   : KERNEL_VOLUME_INNER_DATA_LAST;
        const size_t within = (size_t)(offset % KERNEL_VOLUME_BLOCK_SIZE);

        KernelMemoryDeviceStore[KernelVolumeBlock(block) + within] = KernelFileByteAt(offset);
    }

    /*
     * Block 12 of the sparse file, which is the first block its indirect block
     * names. Block 13 is a hole and is deliberately left as it is: the two lie
     * beside one another so that a reader returning zeroes for both, or data for
     * both, is caught.
     */
    for (uint32_t offset = 0U; offset < KERNEL_VOLUME_BLOCK_SIZE; ++offset)
    {
        KernelMemoryDeviceStore[indirect + offset] =
            KernelFileByteAt((uint64_t)EXT2_DIRECT_BLOCK_COUNT * KERNEL_VOLUME_BLOCK_SIZE +
                             offset);
    }

    KernelStoreText(KernelVolumeBlock(KERNEL_VOLUME_LINK_DATA), KERNEL_VOLUME_SLOW_LINK_TARGET);
    KernelStoreText(KernelInodeField(KERNEL_VOLUME_FAST_LINK_INODE, EXT2_OFFSET_I_BLOCK),
                    KERNEL_VOLUME_FAST_LINK_TARGET);
}

/*
 * Composes the directory data of the volume: the root directory and the one
 * subdirectory beneath it.
 *
 * The entries are written from the same offset names the parser reads, for the
 * reason the superblock's fields are, and the block is laid out as Table 4.3 of
 * the specification lays out its sample: entries aligned upon four bytes, an
 * unused record left where a name was removed, and a final record whose length
 * runs to the end of the block rather than stopping after its name.
 *
 * Both of those last two are deliberate and neither is a curiosity. A traversal
 * that mistook the unused record for a name would report a file that does not
 * exist; one that stopped at the end of the last name rather than at the end of
 * the block would read the padding as a further entry.
 */
static size_t KernelComposeEntry(size_t position, uint32_t inode, uint16_t record_length,
                                 uint8_t file_type, const char *name)
{
    size_t length = 0U;

    while (name[length] != '\0')
    {
        ++length;
    }

    KernelStoreWord(position + EXT2_OFFSET_DE_INODE, inode);
    KernelStoreHalf(position + EXT2_OFFSET_DE_RECORD_LENGTH, record_length);
    KernelMemoryDeviceStore[position + EXT2_OFFSET_DE_NAME_LENGTH] = (uint8_t)length;
    KernelMemoryDeviceStore[position + EXT2_OFFSET_DE_FILE_TYPE] = file_type;

    for (size_t index = 0U; index < length; ++index)
    {
        KernelMemoryDeviceStore[position + EXT2_OFFSET_DE_NAME + index] = (uint8_t)name[index];
    }

    return position + record_length;
}

static void KernelComposeDirectories(void)
{
    const size_t root = KernelVolumeBlock(KERNEL_VOLUME_ROOT_DATA);
    const size_t sub = KernelVolumeBlock(KERNEL_VOLUME_SUB_DATA);
    size_t position;

    /*
     * The root directory. Its entries "." and ".." are ordinary entries upon the
     * volume and are composed as such: the resolver is meant to find them by
     * looking, not by knowing what they mean, and the ".." of the root names the
     * root itself because the root has no parent.
     */
    position = KernelComposeEntry(root, EXT2_ROOT_INODE, 12U, (uint8_t)EXT2_FT_DIR, ".");
    position = KernelComposeEntry(position, EXT2_ROOT_INODE, 12U, (uint8_t)EXT2_FT_DIR, "..");
    position = KernelComposeEntry(position, KERNEL_VOLUME_FILE_INODE, 16U,
                                  (uint8_t)EXT2_FT_REG_FILE, "file");

    /*
     * The record of a name that was removed. It retains the bytes of the name it
     * held, which is what a volume looks like when the first entry of a block is
     * removed: the record is marked unused by having its inode number set to
     * zero and nothing else about it is touched. A traversal that read the name
     * rather than the inode number would report a file that was deleted.
     */
    position = KernelComposeEntry(position, 0U, 16U, (uint8_t)EXT2_FT_UNKNOWN, "removed");

    position = KernelComposeEntry(position, KERNEL_VOLUME_SUB_INODE, 12U,
                                  (uint8_t)EXT2_FT_DIR, "sub");
    position = KernelComposeEntry(position, KERNEL_VOLUME_FAST_LINK_INODE, 20U,
                                  (uint8_t)EXT2_FT_SYMLINK, "link-fast");
    position = KernelComposeEntry(position, KERNEL_VOLUME_SLOW_LINK_INODE, 20U,
                                  (uint8_t)EXT2_FT_SYMLINK, "link-slow");

    /* The last record of the block runs to the end of it. */
    (void)KernelComposeEntry(position, 0U,
                             (uint16_t)((root + KERNEL_VOLUME_BLOCK_SIZE) - position),
                             (uint8_t)EXT2_FT_UNKNOWN, "");

    /* The subdirectory, whose ".." names the root. */
    position = KernelComposeEntry(sub, KERNEL_VOLUME_SUB_INODE, 12U, (uint8_t)EXT2_FT_DIR, ".");
    position = KernelComposeEntry(position, EXT2_ROOT_INODE, 12U, (uint8_t)EXT2_FT_DIR, "..");
    position = KernelComposeEntry(position, KERNEL_VOLUME_INNER_INODE, 16U,
                                  (uint8_t)EXT2_FT_REG_FILE, "inner");
    (void)KernelComposeEntry(position, 0U,
                             (uint16_t)((sub + KERNEL_VOLUME_BLOCK_SIZE) - position),
                             (uint8_t)EXT2_FT_UNKNOWN, "");
}

/*
 * Composes the two bitmaps.
 *
 * One bit stands for each block of the group and each inode of it, 1 meaning
 * used, the first of the group being bit 0 of byte 0 and the ninth bit 0 of
 * byte 1. The order is written out here as the parser writes it out, and for the
 * same reason: it is the format's and not the composer's opinion of it.
 *
 * Every block from the first data block to the last the composition uses is
 * marked, and every inode but 14. The counts stated in the superblock and the
 * descriptor are derived from the same constants, so a bitmap and a count that
 * disagreed would be a mistake in one place rather than a difference between two.
 */
static void KernelSetBitmapBit(uint32_t block, uint32_t index)
{
    KernelMemoryDeviceStore[KernelVolumeBlock(block) + (index / 8U)] |=
        (uint8_t)(1U << (index % 8U));
}

static void KernelComposeBitmaps(void)
{
    for (uint32_t offset = 0U; offset < KERNEL_VOLUME_BLOCK_SIZE; ++offset)
    {
        KernelMemoryDeviceStore[KernelVolumeBlock(KERNEL_VOLUME_BLOCK_BITMAP) + offset] = 0U;
        KernelMemoryDeviceStore[KernelVolumeBlock(KERNEL_VOLUME_INODE_BITMAP) + offset] = 0U;
    }

    /*
     * The blocks in use. Block 1 is the first data block, so it is bit 0; the
     * subtraction is the same one the allocator performs, and getting it wrong
     * in either place marks a block that is not the one meant.
     */
    for (uint32_t block = 1U; block < KERNEL_VOLUME_LAST_BLOCK; ++block)
    {
        KernelSetBitmapBit(KERNEL_VOLUME_BLOCK_BITMAP, block - 1U);
    }

    /*
     * The inodes in use: 1 to 16 but for 14, which is left free deliberately.
     * Everything above 16 is free and is what an allocation is given.
     */
    for (uint32_t number = 1U; number <= 16U; ++number)
    {
        if (number != KERNEL_VOLUME_UNUSED_INODE)
        {
            KernelSetBitmapBit(KERNEL_VOLUME_INODE_BITMAP, number - 1U);
        }
    }
}

/*
 * Composes the descriptor of the volume's single group.
 *
 * The free counts must agree with the superblock's, there being one group to
 * account for the whole volume; the verification of the table asserts exactly
 * that, so a self-test composing them inconsistently would fail upon its own
 * arithmetic rather than upon the parser's.
 */
void KernelComposeGroupDescriptor(void)
{
    for (size_t index = 0U; index < KERNEL_VOLUME_BLOCK_SIZE; ++index)
    {
        KernelMemoryDeviceStore[KernelVolumeBlock(KERNEL_VOLUME_DESCRIPTOR_BLOCK) + index] = 0U;
    }

    KernelStoreWord(KernelDescriptorField(EXT2_OFFSET_BG_BLOCK_BITMAP),
                    KERNEL_VOLUME_BLOCK_BITMAP);
    KernelStoreWord(KernelDescriptorField(EXT2_OFFSET_BG_INODE_BITMAP),
                    KERNEL_VOLUME_INODE_BITMAP);
    KernelStoreWord(KernelDescriptorField(EXT2_OFFSET_BG_INODE_TABLE),
                    KERNEL_VOLUME_INODE_TABLE);
    KernelStoreHalf(KernelDescriptorField(EXT2_OFFSET_BG_FREE_BLOCKS),
                    (uint16_t)KERNEL_VOLUME_FREE_BLOCKS);
    KernelStoreHalf(KernelDescriptorField(EXT2_OFFSET_BG_FREE_INODES),
                    (uint16_t)KERNEL_VOLUME_FREE_INODES);
    KernelStoreHalf(KernelDescriptorField(EXT2_OFFSET_BG_USED_DIRECTORIES),
                    (uint16_t)KERNEL_VOLUME_DIRECTORIES);
}

/*
 * Composes a volume that every rule of the parser accepts: revision 1, blocks of
 * 1024 bytes, one group, and the two features this kernel implements.
 */
void KernelComposeVolume(void)
{
    static const char label[] = "oxys-test";

    for (size_t index = 0U; index < EXT2_SUPERBLOCK_SIZE; ++index)
    {
        KernelMemoryDeviceStore[EXT2_SUPERBLOCK_OFFSET + index] = 0U;
    }

    KernelSetVolumeWord(EXT2_OFFSET_INODE_COUNT, KERNEL_VOLUME_INODES);
    KernelSetVolumeWord(EXT2_OFFSET_BLOCK_COUNT, 128U);
    KernelSetVolumeWord(EXT2_OFFSET_RESERVED_BLOCKS, 6U);
    KernelSetVolumeWord(EXT2_OFFSET_FREE_BLOCKS, KERNEL_VOLUME_FREE_BLOCKS);
    KernelSetVolumeWord(EXT2_OFFSET_FREE_INODES, KERNEL_VOLUME_FREE_INODES);
    KernelSetVolumeWord(EXT2_OFFSET_FIRST_DATA_BLOCK, 1U);
    KernelSetVolumeWord(EXT2_OFFSET_LOG_BLOCK_SIZE, 0U);
    KernelSetVolumeWord(EXT2_OFFSET_LOG_FRAGMENT_SIZE, 0U);
    KernelSetVolumeWord(EXT2_OFFSET_BLOCKS_PER_GROUP, 8192U);
    KernelSetVolumeWord(EXT2_OFFSET_FRAGS_PER_GROUP, 8192U);
    KernelSetVolumeWord(EXT2_OFFSET_INODES_PER_GROUP, KERNEL_VOLUME_INODES);
    KernelSetVolumeHalf(EXT2_OFFSET_MAGIC, EXT2_SUPER_MAGIC);
    KernelSetVolumeHalf(EXT2_OFFSET_STATE, (uint16_t)EXT2_VALID_FS);
    KernelSetVolumeHalf(EXT2_OFFSET_ERRORS, 1U);
    KernelSetVolumeWord(EXT2_OFFSET_REVISION, EXT2_DYNAMIC_REV);
    KernelSetVolumeWord(EXT2_OFFSET_FIRST_INODE, EXT2_GOOD_OLD_FIRST_INODE);
    KernelSetVolumeHalf(EXT2_OFFSET_INODE_SIZE, (uint16_t)EXT2_GOOD_OLD_INODE_SIZE);
    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_INCOMPAT, EXT2_FEATURE_INCOMPAT_FILETYPE);
    KernelSetVolumeWord(EXT2_OFFSET_FEATURE_RO_COMPAT, EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER);

    for (size_t index = 0U; label[index] != '\0'; ++index)
    {
        KernelMemoryDeviceStore[EXT2_SUPERBLOCK_OFFSET + EXT2_OFFSET_VOLUME_NAME + index] =
            (uint8_t)label[index];
    }

    KernelComposeGroupDescriptor();
    KernelComposeBitmaps();
    KernelComposeInodes();
    KernelComposeDirectories();
    KernelComposeFiles();
}

/*
 * Whether two terminated strings hold the same characters. There is no C library
 * until Phase 7, and this is the only place in the self-test that needs one.
 */
bool KernelSameString(const char *left, const char *right)
{
    size_t index = 0U;

    while ((left[index] != '\0') && (left[index] == right[index]))
    {
        ++index;
    }

    return left[index] == right[index];
}

/* Whether a run of the buffer holds the bytes the composed file holds at an
 * offset within itself. */
bool KernelFileBufferMatches(uint64_t offset, uint64_t length)
{
    for (uint64_t index = 0U; index < length; ++index)
    {
        if (KernelFileBuffer[index] != KernelFileByteAt(offset + index))
        {
            return false;
        }
    }

    return true;
}

/* Whether a run of the buffer is entirely zero, which is what a hole reads as. */
bool KernelFileBufferIsZero(uint64_t length)
{
    for (uint64_t index = 0U; index < length; ++index)
    {
        if (KernelFileBuffer[index] != 0U)
        {
            return false;
        }
    }

    return true;
}
