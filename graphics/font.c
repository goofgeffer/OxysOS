/*
 * File: graphics/font.c
 * Purpose: Holds the bitmap font and draws a glyph of it upon a surface. Eight
 *          columns by eight rows to the character, covering the printable
 *          ASCII range, with one replacement glyph for every code outside it.
 * Key functions: FontGlyph, FontGlyphRow, FontDrawGlyph, FontDrawGlyphOpaque,
 *          FontDrawGlyphScaled, FontCovers.
 * References:
 *   - ANSI X3.4-1986: the code points 0x20 to 0x7E this font covers, and their
 *     names, which are the comments beside each glyph below.
 *   - PROJECT_GUIDELINES.md, Section 2: all source must be original. The glyphs
 *     below were drawn for this project and are not a transcription of any other
 *     font; see the note upon that immediately below.
 *   - docs/design/GRAPHICS.md, Section 18: the design of the face and the
 *     metrics it is drawn to.
 *
 * Where this font came from.
 *
 * It was drawn for this project, pixel by pixel. That is not a boast; it is a
 * constraint, and it is why the face is a plain one. `PROJECT_GUIDELINES.md`,
 * Section 2, prohibits transcribing anybody else's source, and a font is
 * exactly the kind of asset that is easy to lift and hard to notice having
 * lifted — the IBM code page 437 face is in a hundred repositories and would
 * have been quicker to copy than to draw.
 *
 * The alternative considered and rejected was to read the font the firmware
 * loaded into plane 2 of the VGA character generator, which would have been
 * original code operating upon the machine's own data. It was rejected for two
 * reasons: the boot loader has already set a graphics mode by the time this
 * kernel runs, so the planes may no longer hold it; and Phase 12 boots under
 * UEFI, where there is no VGA character generator at all. A font compiled into
 * the image works in both cases and in every case after them.
 *
 * How to read and change it.
 *
 * Each glyph is eight bytes, one to a row, the most significant bit leftmost —
 * so a row of pixels reads left to right exactly as the picture beside it is
 * drawn. The pictures are the reason this file is legible, and they are
 * comments: nothing checks that a picture agrees with the bytes beside it.
 * Change one and you must change the other. The self-test asserts the bytes,
 * not the pictures, and no test can tell you that a letter looks wrong.
 *
 * The metrics: ink occupies columns 0 to 5, leaving two columns of spacing;
 * capitals and digits occupy rows 0 to 6 with the baseline at row 6; lowercase
 * occupies rows 2 to 6; and the descenders of g, j, p, q and y reach row 7.
 */

#include <oxys/font.h>
#include <oxys/graphics.h>
#include <oxys/kernel.h>

/*
 * The glyphs, indexed by code point less FONT_FIRST_CODE.
 *
 * The table is const and belongs in .rodata: it is read by the drawing routine
 * and by nothing else, and a font that could be written to by accident would
 * corrupt every character drawn after the accident rather than reporting it.
 */
static const uint8_t FontGlyphs[FONT_GLYPH_COUNT][FONT_HEIGHT] = {

    /* 0x20  space */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */

    /* 0x21  exclamation mark */
    { 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x20, 0x00 },
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |        | */
    /* |  @     | */
    /* |        | */

    /* 0x22  quotation mark */
    { 0x50, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* | @ @    | */
    /* | @ @    | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */

    /* 0x23  number sign */
    { 0x50, 0x50, 0xFC, 0x50, 0xFC, 0x50, 0x50, 0x00 },
    /* | @ @    | */
    /* | @ @    | */
    /* |@@@@@@  | */
    /* | @ @    | */
    /* |@@@@@@  | */
    /* | @ @    | */
    /* | @ @    | */
    /* |        | */

    /* 0x24  dollar sign */
    { 0x20, 0x78, 0xA0, 0x70, 0x28, 0xF0, 0x20, 0x00 },
    /* |  @     | */
    /* | @@@@   | */
    /* |@ @     | */
    /* | @@@    | */
    /* |  @ @   | */
    /* |@@@@    | */
    /* |  @     | */
    /* |        | */

    /* 0x25  percent sign */
    { 0xC4, 0xC8, 0x10, 0x20, 0x4C, 0x8C, 0x00, 0x00 },
    /* |@@   @  | */
    /* |@@  @   | */
    /* |   @    | */
    /* |  @     | */
    /* | @  @@  | */
    /* |@   @@  | */
    /* |        | */
    /* |        | */

    /* 0x26  ampersand */
    { 0x60, 0x90, 0xA0, 0x40, 0xA8, 0x90, 0x68, 0x00 },
    /* | @@     | */
    /* |@  @    | */
    /* |@ @     | */
    /* | @      | */
    /* |@ @ @   | */
    /* |@  @    | */
    /* | @@ @   | */
    /* |        | */

    /* 0x27  apostrophe */
    { 0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* |  @     | */
    /* |  @     | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */

    /* 0x28  left parenthesis */
    { 0x10, 0x20, 0x20, 0x20, 0x20, 0x20, 0x10, 0x00 },
    /* |   @    | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |   @    | */
    /* |        | */

    /* 0x29  right parenthesis */
    { 0x20, 0x10, 0x10, 0x10, 0x10, 0x10, 0x20, 0x00 },
    /* |  @     | */
    /* |   @    | */
    /* |   @    | */
    /* |   @    | */
    /* |   @    | */
    /* |   @    | */
    /* |  @     | */
    /* |        | */

    /* 0x2A  asterisk */
    { 0x00, 0xA8, 0x70, 0xF8, 0x70, 0xA8, 0x00, 0x00 },
    /* |        | */
    /* |@ @ @   | */
    /* | @@@    | */
    /* |@@@@@   | */
    /* | @@@    | */
    /* |@ @ @   | */
    /* |        | */
    /* |        | */

    /* 0x2B  plus sign */
    { 0x00, 0x20, 0x20, 0xF8, 0x20, 0x20, 0x00, 0x00 },
    /* |        | */
    /* |  @     | */
    /* |  @     | */
    /* |@@@@@   | */
    /* |  @     | */
    /* |  @     | */
    /* |        | */
    /* |        | */

    /* 0x2C  comma */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x20, 0x40 },
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |  @@    | */
    /* |  @     | */
    /* | @      | */

    /* 0x2D  hyphen-minus */
    { 0x00, 0x00, 0x00, 0xF8, 0x00, 0x00, 0x00, 0x00 },
    /* |        | */
    /* |        | */
    /* |        | */
    /* |@@@@@   | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */

    /* 0x2E  full stop */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x30, 0x30, 0x00 },
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |  @@    | */
    /* |  @@    | */
    /* |        | */

    /* 0x2F  solidus */
    { 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x00, 0x00 },
    /* |     @  | */
    /* |    @   | */
    /* |   @    | */
    /* |  @     | */
    /* | @      | */
    /* |@       | */
    /* |        | */
    /* |        | */

    /* 0x30  '0' */
    { 0x78, 0x84, 0x8C, 0x94, 0xA4, 0xC4, 0x78, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |@   @@  | */
    /* |@  @ @  | */
    /* |@ @  @  | */
    /* |@@   @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x31  '1' */
    { 0x20, 0x60, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 },
    /* |  @     | */
    /* | @@     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* | @@@    | */
    /* |        | */

    /* 0x32  '2' */
    { 0x78, 0x84, 0x04, 0x18, 0x60, 0x80, 0xFC, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |     @  | */
    /* |   @@   | */
    /* | @@     | */
    /* |@       | */
    /* |@@@@@@  | */
    /* |        | */

    /* 0x33  '3' */
    { 0x78, 0x84, 0x04, 0x38, 0x04, 0x84, 0x78, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |     @  | */
    /* |  @@@   | */
    /* |     @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x34  '4' */
    { 0x18, 0x28, 0x48, 0x88, 0xFC, 0x08, 0x08, 0x00 },
    /* |   @@   | */
    /* |  @ @   | */
    /* | @  @   | */
    /* |@   @   | */
    /* |@@@@@@  | */
    /* |    @   | */
    /* |    @   | */
    /* |        | */

    /* 0x35  '5' */
    { 0xFC, 0x80, 0xF8, 0x04, 0x04, 0x84, 0x78, 0x00 },
    /* |@@@@@@  | */
    /* |@       | */
    /* |@@@@@   | */
    /* |     @  | */
    /* |     @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x36  '6' */
    { 0x38, 0x40, 0x80, 0xF8, 0x84, 0x84, 0x78, 0x00 },
    /* |  @@@   | */
    /* | @      | */
    /* |@       | */
    /* |@@@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x37  '7' */
    { 0xFC, 0x04, 0x08, 0x10, 0x20, 0x20, 0x20, 0x00 },
    /* |@@@@@@  | */
    /* |     @  | */
    /* |    @   | */
    /* |   @    | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |        | */

    /* 0x38  '8' */
    { 0x78, 0x84, 0x84, 0x78, 0x84, 0x84, 0x78, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x39  '9' */
    { 0x78, 0x84, 0x84, 0x7C, 0x04, 0x08, 0x70, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@@  | */
    /* |     @  | */
    /* |    @   | */
    /* | @@@    | */
    /* |        | */

    /* 0x3A  colon */
    { 0x00, 0x30, 0x30, 0x00, 0x30, 0x30, 0x00, 0x00 },
    /* |        | */
    /* |  @@    | */
    /* |  @@    | */
    /* |        | */
    /* |  @@    | */
    /* |  @@    | */
    /* |        | */
    /* |        | */

    /* 0x3B  semicolon */
    { 0x00, 0x30, 0x30, 0x00, 0x30, 0x20, 0x40, 0x00 },
    /* |        | */
    /* |  @@    | */
    /* |  @@    | */
    /* |        | */
    /* |  @@    | */
    /* |  @     | */
    /* | @      | */
    /* |        | */

    /* 0x3C  less-than sign */
    { 0x08, 0x10, 0x20, 0x40, 0x20, 0x10, 0x08, 0x00 },
    /* |    @   | */
    /* |   @    | */
    /* |  @     | */
    /* | @      | */
    /* |  @     | */
    /* |   @    | */
    /* |    @   | */
    /* |        | */

    /* 0x3D  equals sign */
    { 0x00, 0x00, 0xF8, 0x00, 0xF8, 0x00, 0x00, 0x00 },
    /* |        | */
    /* |        | */
    /* |@@@@@   | */
    /* |        | */
    /* |@@@@@   | */
    /* |        | */
    /* |        | */
    /* |        | */

    /* 0x3E  greater-than sign */
    { 0x40, 0x20, 0x10, 0x08, 0x10, 0x20, 0x40, 0x00 },
    /* | @      | */
    /* |  @     | */
    /* |   @    | */
    /* |    @   | */
    /* |   @    | */
    /* |  @     | */
    /* | @      | */
    /* |        | */

    /* 0x3F  question mark */
    { 0x78, 0x84, 0x04, 0x18, 0x20, 0x00, 0x20, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |     @  | */
    /* |   @@   | */
    /* |  @     | */
    /* |        | */
    /* |  @     | */
    /* |        | */

    /* 0x40  commercial at */
    { 0x78, 0x84, 0xBC, 0xA4, 0xB8, 0x80, 0x78, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |@ @@@@  | */
    /* |@ @  @  | */
    /* |@ @@@   | */
    /* |@       | */
    /* | @@@@   | */
    /* |        | */

    /* 0x41  'A' */
    { 0x30, 0x48, 0x84, 0x84, 0xFC, 0x84, 0x84, 0x00 },
    /* |  @@    | */
    /* | @  @   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@@@@@@  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |        | */

    /* 0x42  'B' */
    { 0xF8, 0x84, 0x84, 0xF8, 0x84, 0x84, 0xF8, 0x00 },
    /* |@@@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@@@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@@@@@   | */
    /* |        | */

    /* 0x43  'C' */
    { 0x78, 0x84, 0x80, 0x80, 0x80, 0x84, 0x78, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |@       | */
    /* |@       | */
    /* |@       | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x44  'D' */
    { 0xF0, 0x88, 0x84, 0x84, 0x84, 0x88, 0xF0, 0x00 },
    /* |@@@@    | */
    /* |@   @   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@   @   | */
    /* |@@@@    | */
    /* |        | */

    /* 0x45  'E' */
    { 0xFC, 0x80, 0x80, 0xF8, 0x80, 0x80, 0xFC, 0x00 },
    /* |@@@@@@  | */
    /* |@       | */
    /* |@       | */
    /* |@@@@@   | */
    /* |@       | */
    /* |@       | */
    /* |@@@@@@  | */
    /* |        | */

    /* 0x46  'F' */
    { 0xFC, 0x80, 0x80, 0xF8, 0x80, 0x80, 0x80, 0x00 },
    /* |@@@@@@  | */
    /* |@       | */
    /* |@       | */
    /* |@@@@@   | */
    /* |@       | */
    /* |@       | */
    /* |@       | */
    /* |        | */

    /* 0x47  'G' */
    { 0x78, 0x84, 0x80, 0x9C, 0x84, 0x84, 0x78, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |@       | */
    /* |@  @@@  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x48  'H' */
    { 0x84, 0x84, 0x84, 0xFC, 0x84, 0x84, 0x84, 0x00 },
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@@@@@@  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |        | */

    /* 0x49  'I' */
    { 0x70, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 },
    /* | @@@    | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* | @@@    | */
    /* |        | */

    /* 0x4A  'J' */
    { 0x1C, 0x04, 0x04, 0x04, 0x84, 0x84, 0x78, 0x00 },
    /* |   @@@  | */
    /* |     @  | */
    /* |     @  | */
    /* |     @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x4B  'K' */
    { 0x84, 0x88, 0x90, 0xE0, 0x90, 0x88, 0x84, 0x00 },
    /* |@    @  | */
    /* |@   @   | */
    /* |@  @    | */
    /* |@@@     | */
    /* |@  @    | */
    /* |@   @   | */
    /* |@    @  | */
    /* |        | */

    /* 0x4C  'L' */
    { 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0xFC, 0x00 },
    /* |@       | */
    /* |@       | */
    /* |@       | */
    /* |@       | */
    /* |@       | */
    /* |@       | */
    /* |@@@@@@  | */
    /* |        | */

    /* 0x4D  'M' */
    { 0x84, 0xCC, 0xB4, 0xB4, 0x84, 0x84, 0x84, 0x00 },
    /* |@    @  | */
    /* |@@  @@  | */
    /* |@ @@ @  | */
    /* |@ @@ @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |        | */

    /* 0x4E  'N' */
    { 0x84, 0xC4, 0xA4, 0x94, 0x8C, 0x84, 0x84, 0x00 },
    /* |@    @  | */
    /* |@@   @  | */
    /* |@ @  @  | */
    /* |@  @ @  | */
    /* |@   @@  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |        | */

    /* 0x4F  'O' */
    { 0x78, 0x84, 0x84, 0x84, 0x84, 0x84, 0x78, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x50  'P' */
    { 0xF8, 0x84, 0x84, 0xF8, 0x80, 0x80, 0x80, 0x00 },
    /* |@@@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@@@@@   | */
    /* |@       | */
    /* |@       | */
    /* |@       | */
    /* |        | */

    /* 0x51  'Q' */
    { 0x78, 0x84, 0x84, 0x84, 0xB4, 0x94, 0x6C, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@ @@ @  | */
    /* |@  @ @  | */
    /* | @@ @@  | */
    /* |        | */

    /* 0x52  'R' */
    { 0xF8, 0x84, 0x84, 0xF8, 0x90, 0x88, 0x84, 0x00 },
    /* |@@@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@@@@@   | */
    /* |@  @    | */
    /* |@   @   | */
    /* |@    @  | */
    /* |        | */

    /* 0x53  'S' */
    { 0x78, 0x84, 0x80, 0x78, 0x04, 0x84, 0x78, 0x00 },
    /* | @@@@   | */
    /* |@    @  | */
    /* |@       | */
    /* | @@@@   | */
    /* |     @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x54  'T' */
    { 0xF8, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00 },
    /* |@@@@@   | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |        | */

    /* 0x55  'U' */
    { 0x84, 0x84, 0x84, 0x84, 0x84, 0x84, 0x78, 0x00 },
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x56  'V' */
    { 0x84, 0x84, 0x84, 0x84, 0x48, 0x48, 0x30, 0x00 },
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @  @   | */
    /* | @  @   | */
    /* |  @@    | */
    /* |        | */

    /* 0x57  'W' */
    { 0x84, 0x84, 0x84, 0xB4, 0xB4, 0xCC, 0x84, 0x00 },
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@ @@ @  | */
    /* |@ @@ @  | */
    /* |@@  @@  | */
    /* |@    @  | */
    /* |        | */

    /* 0x58  'X' */
    { 0x84, 0x48, 0x30, 0x30, 0x30, 0x48, 0x84, 0x00 },
    /* |@    @  | */
    /* | @  @   | */
    /* |  @@    | */
    /* |  @@    | */
    /* |  @@    | */
    /* | @  @   | */
    /* |@    @  | */
    /* |        | */

    /* 0x59  'Y' */
    { 0x84, 0x48, 0x30, 0x20, 0x20, 0x20, 0x20, 0x00 },
    /* |@    @  | */
    /* | @  @   | */
    /* |  @@    | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |        | */

    /* 0x5A  'Z' */
    { 0xFC, 0x04, 0x08, 0x30, 0x40, 0x80, 0xFC, 0x00 },
    /* |@@@@@@  | */
    /* |     @  | */
    /* |    @   | */
    /* |  @@    | */
    /* | @      | */
    /* |@       | */
    /* |@@@@@@  | */
    /* |        | */

    /* 0x5B  left square bracket */
    { 0x38, 0x20, 0x20, 0x20, 0x20, 0x20, 0x38, 0x00 },
    /* |  @@@   | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @@@   | */
    /* |        | */

    /* 0x5C  reverse solidus */
    { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x00, 0x00 },
    /* |@       | */
    /* | @      | */
    /* |  @     | */
    /* |   @    | */
    /* |    @   | */
    /* |     @  | */
    /* |        | */
    /* |        | */

    /* 0x5D  right square bracket */
    { 0x38, 0x08, 0x08, 0x08, 0x08, 0x08, 0x38, 0x00 },
    /* |  @@@   | */
    /* |    @   | */
    /* |    @   | */
    /* |    @   | */
    /* |    @   | */
    /* |    @   | */
    /* |  @@@   | */
    /* |        | */

    /* 0x5E  circumflex accent */
    { 0x20, 0x50, 0x88, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* |  @     | */
    /* | @ @    | */
    /* |@   @   | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */

    /* 0x5F  low line */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC },
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |@@@@@@  | */

    /* 0x60  grave accent */
    { 0x20, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
    /* |  @     | */
    /* |   @    | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */

    /* 0x61  'a' */
    { 0x00, 0x00, 0x78, 0x04, 0x7C, 0x84, 0x7C, 0x00 },
    /* |        | */
    /* |        | */
    /* | @@@@   | */
    /* |     @  | */
    /* | @@@@@  | */
    /* |@    @  | */
    /* | @@@@@  | */
    /* |        | */

    /* 0x62  'b' */
    { 0x80, 0x80, 0xF8, 0x84, 0x84, 0x84, 0xF8, 0x00 },
    /* |@       | */
    /* |@       | */
    /* |@@@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@@@@@   | */
    /* |        | */

    /* 0x63  'c' */
    { 0x00, 0x00, 0x78, 0x84, 0x80, 0x84, 0x78, 0x00 },
    /* |        | */
    /* |        | */
    /* | @@@@   | */
    /* |@    @  | */
    /* |@       | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x64  'd' */
    { 0x04, 0x04, 0x7C, 0x84, 0x84, 0x84, 0x7C, 0x00 },
    /* |     @  | */
    /* |     @  | */
    /* | @@@@@  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@@  | */
    /* |        | */

    /* 0x65  'e' */
    { 0x00, 0x00, 0x78, 0x84, 0xFC, 0x80, 0x78, 0x00 },
    /* |        | */
    /* |        | */
    /* | @@@@   | */
    /* |@    @  | */
    /* |@@@@@@  | */
    /* |@       | */
    /* | @@@@   | */
    /* |        | */

    /* 0x66  'f' */
    { 0x18, 0x20, 0xF8, 0x20, 0x20, 0x20, 0x20, 0x00 },
    /* |   @@   | */
    /* |  @     | */
    /* |@@@@@   | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |        | */

    /* 0x67  'g' */
    { 0x00, 0x00, 0x7C, 0x84, 0x84, 0x7C, 0x04, 0x78 },
    /* |        | */
    /* |        | */
    /* | @@@@@  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@@  | */
    /* |     @  | */
    /* | @@@@   | */

    /* 0x68  'h' */
    { 0x80, 0x80, 0xF8, 0x84, 0x84, 0x84, 0x84, 0x00 },
    /* |@       | */
    /* |@       | */
    /* |@@@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |        | */

    /* 0x69  'i' */
    { 0x20, 0x00, 0x60, 0x20, 0x20, 0x20, 0x70, 0x00 },
    /* |  @     | */
    /* |        | */
    /* | @@     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* | @@@    | */
    /* |        | */

    /* 0x6A  'j' */
    { 0x08, 0x00, 0x18, 0x08, 0x08, 0x08, 0x88, 0x70 },
    /* |    @   | */
    /* |        | */
    /* |   @@   | */
    /* |    @   | */
    /* |    @   | */
    /* |    @   | */
    /* |@   @   | */
    /* | @@@    | */

    /* 0x6B  'k' */
    { 0x80, 0x80, 0x88, 0x90, 0xE0, 0x90, 0x88, 0x00 },
    /* |@       | */
    /* |@       | */
    /* |@   @   | */
    /* |@  @    | */
    /* |@@@     | */
    /* |@  @    | */
    /* |@   @   | */
    /* |        | */

    /* 0x6C  'l' */
    { 0x60, 0x20, 0x20, 0x20, 0x20, 0x20, 0x70, 0x00 },
    /* | @@     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* | @@@    | */
    /* |        | */

    /* 0x6D  'm' */
    { 0x00, 0x00, 0xD8, 0xA4, 0xA4, 0xA4, 0xA4, 0x00 },
    /* |        | */
    /* |        | */
    /* |@@ @@   | */
    /* |@ @  @  | */
    /* |@ @  @  | */
    /* |@ @  @  | */
    /* |@ @  @  | */
    /* |        | */

    /* 0x6E  'n' */
    { 0x00, 0x00, 0xF8, 0x84, 0x84, 0x84, 0x84, 0x00 },
    /* |        | */
    /* |        | */
    /* |@@@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |        | */

    /* 0x6F  'o' */
    { 0x00, 0x00, 0x78, 0x84, 0x84, 0x84, 0x78, 0x00 },
    /* |        | */
    /* |        | */
    /* | @@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@   | */
    /* |        | */

    /* 0x70  'p' */
    { 0x00, 0x00, 0xF8, 0x84, 0x84, 0xF8, 0x80, 0x80 },
    /* |        | */
    /* |        | */
    /* |@@@@@   | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@@@@@   | */
    /* |@       | */
    /* |@       | */

    /* 0x71  'q' */
    { 0x00, 0x00, 0x7C, 0x84, 0x84, 0x7C, 0x04, 0x04 },
    /* |        | */
    /* |        | */
    /* | @@@@@  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@@  | */
    /* |     @  | */
    /* |     @  | */

    /* 0x72  'r' */
    { 0x00, 0x00, 0xB8, 0xC0, 0x80, 0x80, 0x80, 0x00 },
    /* |        | */
    /* |        | */
    /* |@ @@@   | */
    /* |@@      | */
    /* |@       | */
    /* |@       | */
    /* |@       | */
    /* |        | */

    /* 0x73  's' */
    { 0x00, 0x00, 0x7C, 0x80, 0x78, 0x04, 0xF8, 0x00 },
    /* |        | */
    /* |        | */
    /* | @@@@@  | */
    /* |@       | */
    /* | @@@@   | */
    /* |     @  | */
    /* |@@@@@   | */
    /* |        | */

    /* 0x74  't' */
    { 0x20, 0x20, 0xF8, 0x20, 0x20, 0x28, 0x10, 0x00 },
    /* |  @     | */
    /* |  @     | */
    /* |@@@@@   | */
    /* |  @     | */
    /* |  @     | */
    /* |  @ @   | */
    /* |   @    | */
    /* |        | */

    /* 0x75  'u' */
    { 0x00, 0x00, 0x84, 0x84, 0x84, 0x8C, 0x74, 0x00 },
    /* |        | */
    /* |        | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@   @@  | */
    /* | @@@ @  | */
    /* |        | */

    /* 0x76  'v' */
    { 0x00, 0x00, 0x84, 0x84, 0x84, 0x48, 0x30, 0x00 },
    /* |        | */
    /* |        | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @  @   | */
    /* |  @@    | */
    /* |        | */

    /* 0x77  'w' */
    { 0x00, 0x00, 0x84, 0xA4, 0xA4, 0xA4, 0x58, 0x00 },
    /* |        | */
    /* |        | */
    /* |@    @  | */
    /* |@ @  @  | */
    /* |@ @  @  | */
    /* |@ @  @  | */
    /* | @ @@   | */
    /* |        | */

    /* 0x78  'x' */
    { 0x00, 0x00, 0x84, 0x48, 0x30, 0x48, 0x84, 0x00 },
    /* |        | */
    /* |        | */
    /* |@    @  | */
    /* | @  @   | */
    /* |  @@    | */
    /* | @  @   | */
    /* |@    @  | */
    /* |        | */

    /* 0x79  'y' */
    { 0x00, 0x00, 0x84, 0x84, 0x84, 0x7C, 0x04, 0x78 },
    /* |        | */
    /* |        | */
    /* |@    @  | */
    /* |@    @  | */
    /* |@    @  | */
    /* | @@@@@  | */
    /* |     @  | */
    /* | @@@@   | */

    /* 0x7A  'z' */
    { 0x00, 0x00, 0xFC, 0x08, 0x30, 0x40, 0xFC, 0x00 },
    /* |        | */
    /* |        | */
    /* |@@@@@@  | */
    /* |    @   | */
    /* |  @@    | */
    /* | @      | */
    /* |@@@@@@  | */
    /* |        | */

    /* 0x7B  left curly bracket */
    { 0x18, 0x20, 0x20, 0x40, 0x20, 0x20, 0x18, 0x00 },
    /* |   @@   | */
    /* |  @     | */
    /* |  @     | */
    /* | @      | */
    /* |  @     | */
    /* |  @     | */
    /* |   @@   | */
    /* |        | */

    /* 0x7C  vertical line */
    { 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00 },
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |  @     | */
    /* |        | */

    /* 0x7D  right curly bracket */
    { 0x30, 0x08, 0x08, 0x04, 0x08, 0x08, 0x30, 0x00 },
    /* |  @@    | */
    /* |    @   | */
    /* |    @   | */
    /* |     @  | */
    /* |    @   | */
    /* |    @   | */
    /* |  @@    | */
    /* |        | */

    /* 0x7E  tilde */
    { 0x00, 0x00, 0x64, 0x98, 0x00, 0x00, 0x00, 0x00 },
    /* |        | */
    /* |        | */
    /* | @@  @  | */
    /* |@  @@   | */
    /* |        | */
    /* |        | */
    /* |        | */
    /* |        | */

};

/*
 * The glyph drawn for every code the font does not cover: a hollow box.
 *
 * A hollow box is chosen rather than a blank so that an unmapped code is
 * visibly present. A font that drew nothing for a code it did not know would
 * make a string of them indistinguishable from a run of spaces, and the fault
 * would look like missing output rather than like an unmapped character.
 */
static const uint8_t FontReplacement[FONT_HEIGHT] = {
    0x00, 0x7C, 0x44, 0x44, 0x44, 0x44, 0x7C, 0x00
    /* |        | */
    /* | @@@@@  | */
    /* | @   @  | */
    /* | @   @  | */
    /* | @   @  | */
    /* | @   @  | */
    /* | @@@@@  | */
    /* |        | */
};

bool FontCovers(uint8_t code)
{
    return (code >= FONT_FIRST_CODE) && (code <= FONT_LAST_CODE);
}

const uint8_t *FontGlyph(uint8_t code)
{
    if (!FontCovers(code))
    {
        return FontReplacement;
    }

    return FontGlyphs[code - FONT_FIRST_CODE];
}

uint8_t FontGlyphRow(uint8_t code, uint8_t row)
{
    if (row >= FONT_HEIGHT)
    {
        return 0U;
    }

    return FontGlyph(code)[row];
}

void FontDrawGlyph(GraphicsSurface *surface, int32_t x, int32_t y, uint8_t code,
                   uint32_t colour)
{
    const uint8_t *glyph = FontGlyph(code);

    /*
     * Only the set pixels are written; the cell's background is the caller's
     * business and is normally a filled rectangle drawn before this.
     *
     * Drawing the background here as well would be one pass instead of two, and
     * would make it impossible to draw a character over anything — which is
     * what a cursor does, and what sub-task 6.6 will want for a glyph drawn upon
     * a surface that already holds an image.
     *
     * Every pixel goes through GraphicsPutPixel, so every pixel is clipped. A
     * glyph at the edge of a surface is therefore cut off rather than wrapped or
     * refused, and a glyph wholly outside it costs sixty-four rejected writes
     * and touches nothing. That is the slow way to draw text and it is the
     * correct one; the fast way needs a clipped span, which is sub-task 6.6's
     * problem when there is something to measure.
     */
    for (uint8_t row = 0U; row < FONT_HEIGHT; ++row)
    {
        const uint8_t bits = glyph[row];

        if (bits == 0U)
        {
            continue;
        }

        for (uint8_t column = 0U; column < FONT_WIDTH; ++column)
        {
            if ((bits & (uint8_t)(0x80U >> column)) != 0U)
            {
                GraphicsPutPixel(surface, x + (int32_t)column, y + (int32_t)row, colour);
            }
        }
    }
}

void FontDrawGlyphOpaque(GraphicsSurface *surface, int32_t x, int32_t y, uint8_t code,
                         uint32_t ink, uint32_t paper)
{
    const uint8_t *glyph = FontGlyph(code);

    /*
     * The whole cell in one call, and nothing here tests a pixel.
     *
     * A glyph is precisely what GraphicsPatternBlock draws — eight columns wide,
     * one byte to a row, most significant bit leftmost, which is how this table
     * is stored — so the table is handed over as it stands, with no copying and
     * no transformation. The clip is applied once to the cell rather than once
     * to each of its sixty-four pixels, which is the whole of the difference
     * between this and FontDrawGlyph.
     *
     * A row of zero is not skipped as it is in FontDrawGlyph. There it is worth
     * skipping, the glyph leaving what is beneath it untouched; here every pixel
     * of the cell must be written whether the glyph lights it or not, and an
     * empty row is a run of eight paper pixels like any other.
     */
    GraphicsPatternBlock(surface, x, y, glyph, (int32_t)FONT_HEIGHT, ink, paper);
}

void FontDrawGlyphScaled(GraphicsSurface *surface, int32_t x, int32_t y, uint8_t code,
                         uint32_t ink, uint32_t paper, int32_t scale)
{
    const uint8_t *glyph;

    if (scale <= 0)
    {
        return;
    }

    if (scale == 1)
    {
        FontDrawGlyphOpaque(surface, x, y, code, ink, paper);
        return;
    }

    glyph = FontGlyph(code);

    /*
     * A rectangle to the pixel, which is as slow as it sounds and is the right
     * arrangement here.
     *
     * The fast paths above exist because the console draws thousands of
     * characters a second. This draws the title of a fault screen: some twenty
     * characters, once, upon a machine that is about to halt. Optimising it
     * would be optimising the one drawing in this kernel whose cost nobody will
     * ever measure, and the fill it calls is clipped, which is what matters when
     * the caller is a fault handler.
     */
    for (int32_t row = 0; row < (int32_t)FONT_HEIGHT; ++row)
    {
        const uint8_t bits = glyph[row];

        for (int32_t column = 0; column < (int32_t)FONT_WIDTH; ++column)
        {
            const uint8_t bit = (uint8_t)(0x80U >> (uint32_t)column);
            const GraphicsRectangle square = { x + (column * scale), y + (row * scale),
                                               scale, scale };

            GraphicsFillRectangle(surface, square, ((bits & bit) != 0U) ? ink : paper);
        }
    }
}
