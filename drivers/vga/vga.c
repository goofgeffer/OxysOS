/*
 * File: drivers/vga/vga.c
 * Purpose: Implements the VGA colour text-mode output driver, which writes
 *          directly to the legacy character frame buffer and manages the cursor
 *          position, scrolling and colour attributes.
 * Key functions: VgaInitialise, VgaSetColour, VgaClear, VgaPutCharacter,
 *          VgaWriteString, VgaCursorPosition, VgaScroll, VgaUpdateHardwareCursor.
 * References:
 *   - IBM Video Graphics Array technical reference: mode 3 provides an 80 by 25
 *     character display whose frame buffer begins at physical address 0x000B8000
 *     and whose cells comprise a code-point byte followed by an attribute byte.
 *   - IBM VGA technical reference, CRT controller registers: the cursor location
 *     is held in registers 0x0E (high byte) and 0x0F (low byte), addressed
 *     through the index port 0x03D4 and the data port 0x03D5 when the adapter is
 *     configured for colour operation.
 *   - Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3A,
 *     Section 4.5: the frame buffer is reachable at PhysicalToVirtual(0xB8000)
 *     because the boot-time paging hierarchy maps the first gibibyte of physical
 *     memory into the higher half.
 *   - ANSI X3.4-1986 (ISO/IEC 646), the control characters: BS (0x08) moves the
 *     active position one character position backward, LF (0x0A) moves it one
 *     line down and CR (0x0D) moves it to the first position of the line. None of
 *     the three erases anything.
 */

#include <oxys/vga.h>
#include <oxys/kernel.h>
#include <oxys/io.h>

/* The index and data ports of the CRT controller in its colour configuration. */
#define VGA_CRTC_INDEX_PORT UINT16_C(0x03D4)
#define VGA_CRTC_DATA_PORT  UINT16_C(0x03D5)

/* The CRT controller registers holding the two halves of the cursor location. */
#define VGA_CRTC_CURSOR_LOCATION_HIGH UINT8_C(0x0E)
#define VGA_CRTC_CURSOR_LOCATION_LOW  UINT8_C(0x0F)

/*
 * A pointer to the character frame buffer. It is declared volatile because the
 * memory is examined by the display hardware independently of the processor, and
 * the compiler must therefore not elide or reorder stores to it.
 *
 * The translation is expressed as explicit arithmetic upon the two macro
 * constants rather than as a call to PhysicalToVirtual, because ISO/IEC
 * 9899:2011, Section 6.7.9, paragraph 4, requires the initialiser of an object
 * of static storage duration to be a constant expression, which a function call
 * is not, irrespective of the function being declared inline.
 */
static volatile uint16_t *const VgaBuffer =
    (volatile uint16_t *)(uintptr_t)(VGA_TEXT_BUFFER_PHYSICAL + KERNEL_VIRTUAL_BASE);

/* The column at which the next character will be written. */
static size_t VgaCursorColumn;

/* The row at which the next character will be written. */
static size_t VgaCursorRow;

/*
 * The attribute byte applied to characters written subsequently, held in the
 * upper half of a cell value so that it may be combined with a code point by a
 * single bitwise disjunction.
 */
static uint16_t VgaCurrentAttribute;

/*
 * Composes an attribute byte from a foreground and a background colour index.
 * The foreground occupies bits 0 to 3 and the background bits 4 to 7.
 */
static uint8_t VgaComposeAttribute(VgaColour foreground, VgaColour background)
{
    return (uint8_t)((uint8_t)foreground | ((uint8_t)background << 4));
}

/*
 * Composes a complete cell value from a code point and an attribute byte.
 */
static uint16_t VgaComposeCell(char character, uint8_t attribute)
{
    return (uint16_t)((uint16_t)(unsigned char)character | ((uint16_t)attribute << 8));
}

/*
 * Writes the current cursor position to the CRT controller so that the hardware
 * cursor is displayed at the position at which the next character will appear.
 */
static void VgaUpdateHardwareCursor(void)
{
    const uint16_t position = (uint16_t)((VgaCursorRow * VGA_WIDTH) + VgaCursorColumn);

    PortWriteByte(VGA_CRTC_INDEX_PORT, VGA_CRTC_CURSOR_LOCATION_LOW);
    PortWriteByte(VGA_CRTC_DATA_PORT, (uint8_t)(position & 0x00FFU));
    PortWriteByte(VGA_CRTC_INDEX_PORT, VGA_CRTC_CURSOR_LOCATION_HIGH);
    PortWriteByte(VGA_CRTC_DATA_PORT, (uint8_t)((position >> 8) & 0x00FFU));
}

/*
 * Moves the contents of the display upward by one row, discarding the first row
 * and filling the final row with spaces in the current colour pair.
 */
static void VgaScroll(void)
{
    const uint16_t blank_cell = VgaComposeCell(' ', (uint8_t)(VgaCurrentAttribute >> 8));

    for (size_t row = 1U; row < VGA_HEIGHT; ++row)
    {
        for (size_t column = 0U; column < VGA_WIDTH; ++column)
        {
            VgaBuffer[((row - 1U) * VGA_WIDTH) + column] =
                VgaBuffer[(row * VGA_WIDTH) + column];
        }
    }

    for (size_t column = 0U; column < VGA_WIDTH; ++column)
    {
        VgaBuffer[((VGA_HEIGHT - 1U) * VGA_WIDTH) + column] = blank_cell;
    }
}

/*
 * Advances the cursor to the beginning of the following row, scrolling the
 * display if the cursor would otherwise pass the final row.
 */
static void VgaAdvanceRow(void)
{
    VgaCursorColumn = 0U;

    if (VgaCursorRow + 1U < VGA_HEIGHT)
    {
        ++VgaCursorRow;
    }
    else
    {
        VgaScroll();
    }
}

void VgaSetColour(VgaColour foreground, VgaColour background)
{
    VgaCurrentAttribute = (uint16_t)((uint16_t)VgaComposeAttribute(foreground, background) << 8);
}

void VgaClear(void)
{
    const uint16_t blank_cell = VgaComposeCell(' ', (uint8_t)(VgaCurrentAttribute >> 8));

    for (size_t index = 0U; index < (size_t)VGA_WIDTH * (size_t)VGA_HEIGHT; ++index)
    {
        VgaBuffer[index] = blank_cell;
    }

    VgaCursorColumn = 0U;
    VgaCursorRow = 0U;
    VgaUpdateHardwareCursor();
}

void VgaInitialise(void)
{
    VgaSetColour(VGA_COLOUR_LIGHT_GREY, VGA_COLOUR_BLACK);
    VgaClear();
}

void VgaPutCharacter(char character)
{
    switch (character)
    {
    case '\n':
        VgaAdvanceRow();
        break;

    case '\r':
        VgaCursorColumn = 0U;
        break;

    case '\b':
        /*
         * ISO/IEC 646 and ANSI X3.4 define the backspace as a movement of the
         * active position one character backward, and no more: it does not erase
         * the character it moves over. A caller that means to erase writes the
         * three-character sequence "\b \b", which erases upon a serial terminal
         * equally.
         *
         * The cursor stops at the first column rather than wrapping to the end of
         * the preceding row. Nothing here records whether a row ended because the
         * text wrapped or because a line feed was written, so a wrap would let a
         * backspace destroy output that the user never typed. The distinction
         * belongs to the line discipline of Phase 8, which knows where each line
         * of input began.
         */
        if (VgaCursorColumn > 0U)
        {
            --VgaCursorColumn;
        }
        break;

    case '\t':
        /* A horizontal tabulation advances to the next multiple of eight columns. */
        do
        {
            VgaPutCharacter(' ');
        } while ((VgaCursorColumn % 8U) != 0U);
        break;

    default:
        VgaBuffer[(VgaCursorRow * VGA_WIDTH) + VgaCursorColumn] =
            VgaComposeCell(character, (uint8_t)(VgaCurrentAttribute >> 8));

        ++VgaCursorColumn;
        if (VgaCursorColumn >= VGA_WIDTH)
        {
            VgaAdvanceRow();
        }
        break;
    }

    VgaUpdateHardwareCursor();
}

void VgaCursorPosition(size_t *row, size_t *column)
{
    if (row != NULL)
    {
        *row = VgaCursorRow;
    }

    if (column != NULL)
    {
        *column = VgaCursorColumn;
    }
}

void VgaWriteString(const char *string)
{
    if (string == NULL)
    {
        return;
    }

    for (size_t index = 0U; string[index] != '\0'; ++index)
    {
        VgaPutCharacter(string[index]);
    }
}
