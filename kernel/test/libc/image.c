/* SPDX-FileCopyrightText: 2026 The Oxys-OS Authors */
/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * File: kernel/test/libc/image.c
 * Purpose: Asserts the image format of 2026-09-23 — the parser and the scaler,
 *          upon bytes composed in memory, and then the background the ramdisk
 *          ships, read through the filesystem, parsed, and scaled to cover a
 *          screen.
 * Key functions: KernelVerifyImage.
 * References:
 *   - docs/design/SESSION.md: what the background is for, and every
 *     assertion here paired with the silent failure it would catch.
 *   - libc/include/image.h: the format, the scaler, and the seam.
 *   - art/README.md: the command that made the shipped file.
 *
 * Why the shipped file is read here as well as the format driven.
 *
 *   For the icon's reason: a parser that works and a background that is what
 *   the session expects are two properties, and the second fails in silence —
 *   the session draws the mark instead, says so upon a standard error nobody
 *   reads, and the desktop looks like a desktop.
 */

#include <oxys/kernel.h>
#include <oxys/test/verify.h>
#include <oxys/fs/vfs.h>

#include <image.h>
#include <string.h>

/* The file `/etc/session.conf` names, and the extent of the drawing in it. */
#define VERIFY_IMAGE_PATH   "/share/backgrounds/background.oxim"
#define VERIFY_IMAGE_WIDTH  2048U
#define VERIFY_IMAGE_HEIGHT 1448U

/* Room for the shipped file, and one byte more to tell a file that fills the
 * buffer from one cut off at it. */
#define VERIFY_IMAGE_CAPACITY (256U * 1024U)

static bool VerifyImageSucceeded;

static uint8_t VerifyImageBytes[VERIFY_IMAGE_CAPACITY + 1U];
static OxysImage VerifyImageStore;
static OxysImageScaler VerifyImageScaler;
static uint32_t VerifyImageRow[IMAGE_EXTENT_MAXIMUM];

static void VerifyImageRequire(bool condition, const char *statement)
{
    if (!condition)
    {
        KernelWriteString("  ");
        KernelWriteString(statement);
        KernelWriteString("\n");
        VerifyImageSucceeded = false;
    }
}

/* ---------------------------------------------------------- composition */

static size_t VerifyImageAt;

static void VerifyImageHeader(uint32_t width, uint32_t height)
{
    memcpy(VerifyImageBytes, IMAGE_MAGIC, 4U);
    VerifyImageBytes[4] = (uint8_t)IMAGE_VERSION;
    VerifyImageBytes[5] = 0U;
    VerifyImageBytes[6] = 0U;
    VerifyImageBytes[7] = 0U;
    VerifyImageBytes[8] = (uint8_t)(width & 0xFFU);
    VerifyImageBytes[9] = (uint8_t)(width >> 8);
    VerifyImageBytes[10] = (uint8_t)(height & 0xFFU);
    VerifyImageBytes[11] = (uint8_t)(height >> 8);
    VerifyImageAt = IMAGE_HEADER_BYTES;
}

static void VerifyImageRun(uint32_t count, uint32_t pixel)
{
    VerifyImageBytes[VerifyImageAt++] = (uint8_t)(count & 0xFFU);
    VerifyImageBytes[VerifyImageAt++] = (uint8_t)(count >> 8);
    VerifyImageBytes[VerifyImageAt++] = (uint8_t)(pixel & 0xFFU);
    VerifyImageBytes[VerifyImageAt++] = (uint8_t)((pixel >> 8) & 0xFFU);
    VerifyImageBytes[VerifyImageAt++] = (uint8_t)((pixel >> 16) & 0xFFU);
    VerifyImageBytes[VerifyImageAt++] = (uint8_t)((pixel >> 24) & 0xFFU);
}

/*
 * Four by two: the left half of each row white and the right half black in the
 * top row, the other way round in the bottom one — so that a reduction to one
 * pixel is grey, a crop is visible, and rows read in the wrong order are too.
 */
static void VerifyImageComposeSample(void)
{
    VerifyImageHeader(4U, 2U);
    VerifyImageRun(2U, UINT32_C(0x00FFFFFF));
    VerifyImageRun(2U, 0U);
    VerifyImageRun(2U, 0U);
    VerifyImageRun(2U, UINT32_C(0x00FFFFFF));
}

/* ------------------------------------------------------------- the parser */

static void VerifyImageFormat(void)
{
    VerifyImageComposeSample();
    VerifyImageRequire(OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt) &&
                           (OxysImageWidth(&VerifyImageStore) == 4U) &&
                           (OxysImageHeight(&VerifyImageStore) == 2U),
                       "an image composed to the format was refused, or misread");

    /* The refusals, each of which draws something if accepted. */
    VerifyImageRequire(!OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt - 1U),
                       "an image one byte short was accepted");
    VerifyImageRequire(OxysImageWidth(&VerifyImageStore) == 0U,
                       "a refused image kept the extent of the one before it");
    VerifyImageRequire(!OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt + 1U),
                       "an image with a byte after its last row was accepted");

    VerifyImageBytes[4] = (uint8_t)(IMAGE_VERSION + 1U);
    VerifyImageRequire(!OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt),
                       "a version this library does not know was accepted");

    VerifyImageComposeSample();
    VerifyImageBytes[6] = 1U;
    VerifyImageRequire(!OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt),
                       "a reserved byte that was not zero was accepted");

    /*
     * **A run may not cross the end of its row.** Three and then three, in a
     * row of four: the second run is refused, though a full row of four after
     * it makes the file exactly the length two rows of runs would be. A parser
     * that let the run cross would accept this whole file, and the scaler,
     * trusting it, would decode six pixels into a row of four — past the end
     * of its buffer upon an image as wide as the bound.
     */
    VerifyImageHeader(4U, 2U);
    VerifyImageRun(3U, 0U);
    VerifyImageRun(3U, 0U);
    VerifyImageRun(4U, 0U);
    VerifyImageRequire(!OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt),
                       "a run crossing the end of its row was accepted");

    VerifyImageHeader(4U, 1U);
    VerifyImageRun(0U, 0U);
    VerifyImageRun(4U, 0U);
    VerifyImageRequire(!OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt),
                       "a run of nothing was accepted");

    VerifyImageHeader(4U, 1U);
    VerifyImageRun(4U, UINT32_C(0x80FFFFFF));
    VerifyImageRequire(!OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt),
                       "a pixel with a transparency was accepted in a format that has none");

    VerifyImageHeader(0U, 1U);
    VerifyImageRequire(!OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt),
                       "an image of no width was accepted");
}

/* ------------------------------------------------------------- the scaler */

static void VerifyImageScaling(void)
{
    VerifyImageComposeSample();
    (void)OxysImageParse(&VerifyImageStore, VerifyImageBytes, VerifyImageAt);

    /* Reduced to one pixel, it is the average of all eight: grey. */
    VerifyImageRequire(OxysImageScalerBegin(&VerifyImageScaler, &VerifyImageStore, 2U, 1U) &&
                           OxysImageScalerRow(&VerifyImageScaler, VerifyImageRow),
                       "the scaler could not begin or produce a row");

    /*
     * Two by one covers four by two by taking the whole width and the whole
     * height: each screen pixel is a two-by-two block — white and black above
     * and below — so both are grey. A scaler that took one image pixel instead
     * of averaging would give white and black.
     */
    VerifyImageRequire((VerifyImageRow[0] == UINT32_C(0x00808080)) &&
                           (VerifyImageRow[1] == UINT32_C(0x00808080)) &&
                           !OxysImageScalerRow(&VerifyImageScaler, VerifyImageRow),
                       "a reduction was not the average of the pixels it covers, or ran on");

    /*
     * **Cover, not stretch.** Four by two into a square of two covers the
     * height and cuts the width to its middle two columns: the top row is then
     * white and black, the bottom black and white. Stretched, each would be
     * grey; letterboxed, there would be bars.
     */
    (void)OxysImageScalerBegin(&VerifyImageScaler, &VerifyImageStore, 2U, 2U);
    (void)OxysImageScalerRow(&VerifyImageScaler, VerifyImageRow);
    VerifyImageRequire((VerifyImageRow[0] == UINT32_C(0x00FFFFFF)) && (VerifyImageRow[1] == 0U),
                       "a square covered by a wide image was not its middle, or rows were "
                       "read out of order");
    (void)OxysImageScalerRow(&VerifyImageScaler, VerifyImageRow);
    VerifyImageRequire((VerifyImageRow[0] == 0U) && (VerifyImageRow[1] == UINT32_C(0x00FFFFFF)),
                       "the second row of a covered square was not the image's second row");

    /* Enlarged, the pixel beneath is repeated: eight by four from four by two
     * is each pixel doubled. */
    (void)OxysImageScalerBegin(&VerifyImageScaler, &VerifyImageStore, 8U, 4U);
    (void)OxysImageScalerRow(&VerifyImageScaler, VerifyImageRow);
    VerifyImageRequire((VerifyImageRow[3] == UINT32_C(0x00FFFFFF)) && (VerifyImageRow[4] == 0U),
                       "an enlargement did not repeat the pixel beneath");

    VerifyImageRequire(!OxysImageScalerBegin(&VerifyImageScaler, &VerifyImageStore, 0U, 4U) &&
                           !OxysImageScalerBegin(&VerifyImageScaler, &VerifyImageStore,
                                                 IMAGE_EXTENT_MAXIMUM + 1U, 4U),
                       "a screen of no width, or wider than the bound, was begun");
}

/* --------------------------------------------------- the file that ships */

static void VerifyImageShipped(void)
{
    VfsAttributes attributes;
    uint64_t read = 0U;
    uint32_t rows = 0U;
    uint32_t first = 0U;
    bool varied = false;
    int descriptor;

    if (!VfsStat(VERIFY_IMAGE_PATH, &attributes))
    {
        VerifyImageRequire(false, "the background the session names is not upon the ramdisk");

        return;
    }

    if (attributes.size > VERIFY_IMAGE_CAPACITY)
    {
        VerifyImageRequire(false, "the shipped background is larger than this test can hold");

        return;
    }

    descriptor = VfsOpen(VERIFY_IMAGE_PATH, VFS_OPEN_READ, 0U);

    if (descriptor < 0)
    {
        VerifyImageRequire(false, "the shipped background could not be opened");

        return;
    }

    if (!VfsRead(descriptor, VerifyImageBytes, attributes.size, &read))
    {
        (void)VfsClose(descriptor);
        VerifyImageRequire(false, "the shipped background could not be read");

        return;
    }

    (void)VfsClose(descriptor);

    VerifyImageRequire(OxysImageParse(&VerifyImageStore, VerifyImageBytes, (size_t)read),
                       "the shipped background is not an image this library understands");
    VerifyImageRequire((OxysImageWidth(&VerifyImageStore) == VERIFY_IMAGE_WIDTH) &&
                           (OxysImageHeight(&VerifyImageStore) == VERIFY_IMAGE_HEIGHT),
                       "the shipped background is not the extent of the drawing");

    /*
     * Scaled to cover the screen the session is most often given, every row is
     * produced and the picture is more than one colour. A background converted
     * with its channels in the wrong order still varies; one converted from an
     * empty or wholly transparent source does not, and draws a flat field the
     * colour of whatever the conversion made of nothing.
     */
    if (OxysImageScalerBegin(&VerifyImageScaler, &VerifyImageStore, 1280U, 800U))
    {
        while (OxysImageScalerRow(&VerifyImageScaler, VerifyImageRow))
        {
            if (rows == 0U)
            {
                first = VerifyImageRow[0];
            }

            for (uint32_t column = 0U; column < 1280U; column += 16U)
            {
                varied = varied || (VerifyImageRow[column] != first);
            }

            ++rows;
        }
    }

    VerifyImageRequire(rows == 800U, "the shipped background did not scale to every row of a "
                                     "screen");
    VerifyImageRequire(varied, "the shipped background is one colour");
}

void KernelVerifyImage(void)
{
    VerifyImageSucceeded = true;

    KernelWriteString("Images: asserting the format and the scaler upon bytes in memory, then "
                      "the background the ramdisk ships.\n");

    VerifyImageFormat();
    VerifyImageScaling();
    VerifyImageShipped();

    KernelWriteString(VerifyImageSucceeded
                          ? "Image self-test passed: the format and every refusal of it, a "
                            "reduction averaged, a cover cut to its middle, an enlargement "
                            "repeated, and the background `/etc/session.conf` names — read "
                            "off the ramdisk, the drawing's extent, and scaled to a screen.\n"
                          : "Image self-test FAILED.\n");
}
