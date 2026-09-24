/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/icon.c
 * Purpose: Asserts the icon format of sub-task 9.6 — the parser, upon bytes
 *          composed in memory, and then the file the ramdisk actually ships,
 *          read through the filesystem and put through the same parser.
 * Key functions: KernelVerifyIcon.
 * References:
 *   - docs/design/SESSION.md: what icons are for, and every
 *     assertion here paired with the silent failure it would catch.
 *   - libc/include/icon.h: the format and the seam.
 *   - art/README.md: the command that made the shipped file.
 *
 * Why the shipped file is read here and not only the parser driven.
 *
 *   A parser that reads an icon correctly and a system whose icons are what
 *   its launcher expects are two different properties, and the second is the
 *   one that fails in silence: a picture converted at the wrong size, or
 *   converted with its transparency flattened, parses perfectly and draws a
 *   black square. So the file is read, and what is asserted of it is what a
 *   person would have looked for — its extent, that something in it is
 *   transparent, that something is not, and that something is partly.
 *
 *   It is `config-check`'s argument, made from the kernel because an icon needs
 *   no privilege to judge: the bytes are a file the VFS can read, and the
 *   parser is ordinary C.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/fs/vfs.h>

#include <icon.h>
#include <string.h>

/* The file the ramdisk ships and `/etc/session.conf` names. */
#define VERIFY_ICON_PATH "/share/icons/terminal.oxi"

/* What that file is, and what the launcher was sized for. A conversion that
 * produced some other extent would draw a picture in the wrong place, at the
 * wrong size, or not at all. */
#define VERIFY_ICON_EXTENT 48U

static bool VerifyIconSucceeded;

/* One icon and one buffer, reused: the icon is sixteen kilobytes and a second of
 * either would be a second thing to keep in step for no assertion's sake. */
static OxysIcon VerifyIconStore;
static uint8_t VerifyIconBytes[ICON_BYTES_MAXIMUM + 1U];

static void VerifyIconRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyIconSucceeded = false;
    }
}

/*
 * Composes a valid icon of the extent given into the buffer, with a pattern a
 * test can recognise: the pixel at (x, y) is 0x00xxyy00 and the last position
 * of every row is nothing. Returns the length.
 */
static size_t VerifyIconCompose(uint32_t width, uint32_t height)
{
    size_t at = ICON_HEADER_BYTES;

    VerifyIconBytes[0] = (uint8_t)'O';
    VerifyIconBytes[1] = (uint8_t)'X';
    VerifyIconBytes[2] = (uint8_t)'I';
    VerifyIconBytes[3] = (uint8_t)'C';
    VerifyIconBytes[4] = (uint8_t)ICON_VERSION;
    VerifyIconBytes[5] = (uint8_t)width;
    VerifyIconBytes[6] = (uint8_t)height;
    VerifyIconBytes[7] = 0U;

    for (uint32_t y = 0U; y < height; ++y)
    {
        for (uint32_t x = 0U; x < width; ++x)
        {
            const uint32_t value =
                (x + 1U == width) ? ICON_NOTHING : (((uint32_t)x << 16) | ((uint32_t)y << 8));

            VerifyIconBytes[at++] = (uint8_t)(value & 0xFFU);
            VerifyIconBytes[at++] = (uint8_t)((value >> 8) & 0xFFU);
            VerifyIconBytes[at++] = (uint8_t)((value >> 16) & 0xFFU);
            VerifyIconBytes[at++] = (uint8_t)((value >> 24) & 0xFFU);
        }
    }

    return at;
}

/* ------------------------------------------------------------- the parser */

static void VerifyIconFormat(void)
{
    const size_t length = VerifyIconCompose(4U, 3U);

    VerifyIconRequire(OxysIconParse(&VerifyIconStore, VerifyIconBytes, length),
                      "an icon composed to the format was refused");
    VerifyIconRequire(OxysIconWidth(&VerifyIconStore) == 4U,
                      "the width was not the width the header carried");
    VerifyIconRequire(OxysIconHeight(&VerifyIconStore) == 3U,
                      "the height was not the height the header carried");

    /*
     * The pixels are read little-endian and row by row. A parser that read them
     * in the other order, or that walked the rows as columns, produces a
     * picture that is wrong in a way only a person looking would catch.
     */
    VerifyIconRequire(OxysIconAt(&VerifyIconStore, 0U, 0U) == 0U,
                      "the first pixel was not what was written there");
    VerifyIconRequire(OxysIconAt(&VerifyIconStore, 2U, 1U) == UINT32_C(0x00020100),
                      "a pixel was not read from the position it was written at");
    VerifyIconRequire(OxysIconAt(&VerifyIconStore, 3U, 2U) == ICON_NOTHING,
                      "a position written as nothing did not come back as nothing");

    /* Outside is nothing, so that a caller drawing a square larger than the
     * icon needs no bounds of its own. */
    VerifyIconRequire(OxysIconAt(&VerifyIconStore, 4U, 0U) == ICON_NOTHING,
                      "a position beyond the width was not nothing");
    VerifyIconRequire(OxysIconAt(&VerifyIconStore, 0U, 3U) == ICON_NOTHING,
                      "a position beyond the height was not nothing");
    VerifyIconRequire(OxysIconAt(NULL, 0U, 0U) == ICON_NOTHING,
                      "a position of no icon at all was not nothing");
}

static void VerifyIconRefusals(void)
{
    size_t length;

    VerifyIconRequire(!OxysIconParse(&VerifyIconStore, NULL, 0U),
                      "no bytes at all were accepted as an icon");
    VerifyIconRequire(!OxysIconParse(NULL, VerifyIconBytes, ICON_HEADER_BYTES),
                      "an icon was parsed into nowhere");

    length = VerifyIconCompose(2U, 2U);

    VerifyIconRequire(!OxysIconParse(&VerifyIconStore, VerifyIconBytes, ICON_HEADER_BYTES - 1U),
                      "a file shorter than the header was accepted");

    /*
     * **A refusal leaves the icon empty.** A caller that drew a refused icon
     * would draw whatever the last one left behind, which is the failure that
     * looks like the wrong picture rather than like no picture.
     */
    VerifyIconRequire(OxysIconWidth(&VerifyIconStore) == 0U,
                      "a refused icon kept the extent of the one before it");

    VerifyIconBytes[0] = (uint8_t)'N';
    VerifyIconRequire(!OxysIconParse(&VerifyIconStore, VerifyIconBytes, length),
                      "a file that is not an icon at all was accepted");

    length = VerifyIconCompose(2U, 2U);
    VerifyIconBytes[4] = (uint8_t)(ICON_VERSION + 1U);
    VerifyIconRequire(!OxysIconParse(&VerifyIconStore, VerifyIconBytes, length),
                      "a version this library does not know was accepted");

    length = VerifyIconCompose(2U, 2U);
    VerifyIconBytes[5] = 0U;
    VerifyIconRequire(!OxysIconParse(&VerifyIconStore, VerifyIconBytes, length),
                      "an icon of no width was accepted");

    length = VerifyIconCompose(2U, 2U);
    VerifyIconBytes[6] = (uint8_t)(ICON_EXTENT_MAXIMUM + 1U);
    VerifyIconRequire(!OxysIconParse(&VerifyIconStore, VerifyIconBytes, length),
                      "an icon taller than the library holds was accepted");

    /*
     * The length must be exactly the pixels the extent calls for. Without this
     * a file truncated in the middle becomes a picture of whatever followed it
     * in memory, which is the worst of the three failures here: it draws, and
     * what it draws is somebody else's bytes.
     */
    length = VerifyIconCompose(2U, 2U);
    VerifyIconRequire(!OxysIconParse(&VerifyIconStore, VerifyIconBytes, length - 1U),
                      "a file one byte short of its own extent was accepted");
    VerifyIconRequire(!OxysIconParse(&VerifyIconStore, VerifyIconBytes, length + 1U),
                      "a file one byte longer than its own extent was accepted");

    /* The largest the library holds is held, and not refused for being large. */
    length = VerifyIconCompose(ICON_EXTENT_MAXIMUM, ICON_EXTENT_MAXIMUM);
    VerifyIconRequire(OxysIconParse(&VerifyIconStore, VerifyIconBytes, length),
                      "an icon of the largest extent the library holds was refused");
}

/* Writes one pixel of the icon the buffer holds, of the width given. */
static void VerifyIconSet(uint32_t width, uint32_t x, uint32_t y, uint32_t value)
{
    const size_t at = ICON_HEADER_BYTES + (((size_t)y * width + x) * 4U);

    VerifyIconBytes[at] = (uint8_t)(value & 0xFFU);
    VerifyIconBytes[at + 1U] = (uint8_t)((value >> 8) & 0xFFU);
    VerifyIconBytes[at + 2U] = (uint8_t)((value >> 16) & 0xFFU);
    VerifyIconBytes[at + 3U] = (uint8_t)((value >> 24) & 0xFFU);
}

/* ------------------------------------------------------- the composition */

static void VerifyIconComposition(void)
{
    size_t length;

    /*
     * A two-by-two of red, nothing, blue half transparent, and green — each
     * fitted one to one. The half is what version 2 exists for: a reader that
     * took it for none or for all would draw the edge of every picture as the
     * staircase the format was changed to be rid of.
     */
    length = VerifyIconCompose(2U, 2U);
    VerifyIconSet(2U, 0U, 0U, UINT32_C(0x00FF0000));
    VerifyIconSet(2U, 1U, 0U, ICON_NOTHING);
    VerifyIconSet(2U, 0U, 1U, UINT32_C(0x800000FF));
    VerifyIconSet(2U, 1U, 1U, UINT32_C(0x0000FF00));
    VerifyIconRequire(OxysIconParse(&VerifyIconStore, VerifyIconBytes, length),
                      "an icon with a pixel half transparent was refused");
    VerifyIconRequire(OxysIconCompose(&VerifyIconStore, 0U, 0U, 2U, UINT32_C(0x123456)) ==
                          UINT32_C(0x00FF0000),
                      "an opaque pixel drawn one to one was not its own colour");
    VerifyIconRequire(OxysIconCompose(&VerifyIconStore, 1U, 0U, 2U, UINT32_C(0x123456)) ==
                          UINT32_C(0x00123456),
                      "a position of nothing was not the paper behind it");
    VerifyIconRequire(OxysIconCompose(&VerifyIconStore, 0U, 1U, 2U, 0U) == UINT32_C(0x0000007F),
                      "a pixel half transparent was not half its colour upon black");

    /* Enlarged, the pixel beneath is repeated: (3, 3) of four is (1, 1). */
    VerifyIconRequire(OxysIconCompose(&VerifyIconStore, 3U, 3U, 4U, 0U) == UINT32_C(0x0000FF00),
                      "an enlarged icon did not repeat the pixel beneath the position");
    VerifyIconRequire(OxysIconCompose(&VerifyIconStore, 4U, 0U, 4U, UINT32_C(0x123456)) ==
                          UINT32_C(0x00123456),
                      "a position outside the square was not the paper");

    /*
     * **Reduced, the colours are weighted by opacity.** Two white pixels and
     * two of nothing, reduced to one upon white paper, must be white. Were the
     * colours averaged alone, the black that ICON_NOTHING carries would come
     * through as grey — a dark fringe about every picture reduced to the slot,
     * which looks like a fault in the drawing and not in the arithmetic.
     */
    length = VerifyIconCompose(2U, 2U);
    VerifyIconSet(2U, 0U, 0U, UINT32_C(0x00FFFFFF));
    VerifyIconSet(2U, 1U, 0U, ICON_NOTHING);
    VerifyIconSet(2U, 0U, 1U, ICON_NOTHING);
    VerifyIconSet(2U, 1U, 1U, UINT32_C(0x00FFFFFF));
    (void)OxysIconParse(&VerifyIconStore, VerifyIconBytes, length);
    VerifyIconRequire(OxysIconCompose(&VerifyIconStore, 0U, 0U, 1U, UINT32_C(0xFFFFFF)) ==
                          UINT32_C(0x00FFFFFF),
                      "a reduction darkened white upon white with the black of nothing");

    /* And an opaque reduction is the average: two white and two black is the
     * grey between. */
    VerifyIconSet(2U, 1U, 0U, 0U);
    VerifyIconSet(2U, 0U, 1U, 0U);
    (void)OxysIconParse(&VerifyIconStore, VerifyIconBytes, length);
    VerifyIconRequire(OxysIconCompose(&VerifyIconStore, 0U, 0U, 1U, 0U) == UINT32_C(0x00808080),
                      "a reduction was not the average of the pixels it covers");

    /* An icon wider than it is tall is centred, not stretched: three by one in
     * a square of three has the paper above and below it. */
    length = VerifyIconCompose(3U, 1U);
    VerifyIconSet(3U, 0U, 0U, UINT32_C(0x00ABCDEF));
    (void)OxysIconParse(&VerifyIconStore, VerifyIconBytes, length);
    VerifyIconRequire((OxysIconCompose(&VerifyIconStore, 0U, 0U, 3U, 0U) == 0U) &&
                          (OxysIconCompose(&VerifyIconStore, 0U, 1U, 3U, 0U) ==
                           UINT32_C(0x00ABCDEF)) &&
                          (OxysIconCompose(&VerifyIconStore, 0U, 2U, 3U, 0U) == 0U),
                      "an icon that is not square was not centred in its square");

    /* Version 1 is still read: its two transparencies mean what they did. */
    length = VerifyIconCompose(2U, 2U);
    VerifyIconBytes[4] = (uint8_t)ICON_VERSION_OLDEST;
    VerifyIconRequire(OxysIconParse(&VerifyIconStore, VerifyIconBytes, length),
                      "an icon of version 1 was no longer read");
    VerifyIconBytes[4] = 0U;
    VerifyIconRequire(!OxysIconParse(&VerifyIconStore, VerifyIconBytes, length),
                      "an icon of version 0 was accepted");
}

/* --------------------------------------------------- the file that ships */

static void VerifyIconShipped(void)
{
    VfsAttributes attributes;
    uint64_t read = 0U;
    int descriptor;
    bool transparent = false;
    bool opaque = false;
    bool partial = false;

    if (!VfsStat(VERIFY_ICON_PATH, &attributes))
    {
        VerifyIconRequire(false, "the icon the launcher names is not upon the ramdisk");

        return;
    }

    if (attributes.size > (uint64_t)sizeof VerifyIconBytes)
    {
        VerifyIconRequire(false, "the shipped icon is larger than any icon may be");

        return;
    }

    descriptor = VfsOpen(VERIFY_ICON_PATH, VFS_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        VerifyIconRequire(false, "the shipped icon could not be opened");

        return;
    }

    if (!VfsRead(descriptor, VerifyIconBytes, attributes.size, &read))
    {
        (void)VfsClose(descriptor);
        VerifyIconRequire(false, "the shipped icon could not be read");

        return;
    }

    (void)VfsClose(descriptor);

    VerifyIconRequire(read == attributes.size, "the shipped icon read short of its size");
    VerifyIconRequire(OxysIconParse(&VerifyIconStore, VerifyIconBytes, (size_t)read),
                      "the shipped icon is not an icon this library understands");
    VerifyIconRequire(OxysIconWidth(&VerifyIconStore) == VERIFY_ICON_EXTENT,
                      "the shipped icon is not the width the launcher's slot was sized for");
    VerifyIconRequire(OxysIconHeight(&VerifyIconStore) == VERIFY_ICON_EXTENT,
                      "the shipped icon is not the height the launcher's slot was sized for");

    for (uint32_t y = 0U; y < OxysIconHeight(&VerifyIconStore); ++y)
    {
        for (uint32_t x = 0U; x < OxysIconWidth(&VerifyIconStore); ++x)
        {
            const uint32_t pixel = OxysIconAt(&VerifyIconStore, x, y);
            const uint32_t transparency = ICON_TRANSPARENCY(pixel);

            if (transparency == ICON_TRANSPARENT)
            {
                transparent = true;
            }
            else if (transparency == 0U)
            {
                opaque = true;
            }
            else
            {
                partial = true;
            }
        }
    }

    /*
     * The first two are what a conversion gone wrong produces, and neither is
     * a thing the parser can see: an icon converted with its transparency
     * flattened is a square where a picture should be, and an icon converted
     * to nothing at all is a picture that never appears and reports no fault
     * of any kind. The third is the conversion of version 1 run again — the
     * alpha thresholded at half, every pixel of the edge made all or nothing —
     * which parses and draws, and draws the staircase version 2 was made to
     * be rid of.
     */
    VerifyIconRequire(transparent, "no part of the shipped icon is transparent, so it would "
                                   "draw as a square upon whatever is behind it");
    VerifyIconRequire(opaque, "no part of the shipped icon is opaque, so it would draw as a "
                              "ghost of itself or not at all");
    VerifyIconRequire(partial, "no pixel of the shipped icon is partly transparent, so its "
                               "edge was converted as a staircase");
}

void KernelVerifyIcon(void)
{
    VerifyIconSucceeded = true;

    KernelWriteString("Icons: asserting the format upon bytes in memory, then the file the "
                      "ramdisk ships.\n");

    VerifyIconFormat();
    VerifyIconRefusals();
    VerifyIconComposition();
    VerifyIconShipped();

    KernelWriteString(VerifyIconSucceeded
                          ? "Icon self-test passed: the format, every refusal of it, the "
                            "composition over the paper, and the icon `/etc/session.conf` "
                            "names — read off the ramdisk, of the "
                            "extent the launcher draws, with a transparent, an opaque and a "
                            "partly transparent pixel.\n"
                          : "Icon self-test FAILED.\n");
}
