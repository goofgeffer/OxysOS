/*
 * File: kernel/include/oxys/vga.h
 * Purpose: Declares the interface of the VGA colour text-mode output driver.
 * Key definitions: VgaColour, VgaInitialise, VgaClear, VgaPutCharacter,
 *          VgaWriteString, VgaSetColour, VgaCursorPosition.
 * References:
 *   - IBM Video Graphics Array technical reference: mode 3 presents an 80 by 25
 *     character display whose frame buffer begins at physical address 0x000B8000.
 *   - Each character cell occupies two bytes: the code point in the first byte
 *     and the attribute in the second, the attribute comprising a four-bit
 *     foreground index in bits 0 to 3 and a background index in bits 4 to 6,
 *     with bit 7 controlling blinking or intensity according to configuration.
 *   - ANSI X3.4-1986 (ISO/IEC 646), the control characters: the treatment of the
 *     backspace, the line feed and the carriage return by VgaPutCharacter.
 */

#ifndef OXYS_VGA_H
#define OXYS_VGA_H

#include <oxys/types.h>

/* The dimensions of the standard VGA colour text mode (mode 3). */
#define VGA_WIDTH  80
#define VGA_HEIGHT 25

/* The physical address of the colour text-mode frame buffer. */
#define VGA_TEXT_BUFFER_PHYSICAL UINT64_C(0x000B8000)

/*
 * The sixteen colour indices of the standard VGA text-mode palette. Indices 0 to
 * 7 are valid as background colours; all sixteen are valid as foreground colours.
 */
typedef enum VgaColour
{
    VGA_COLOUR_BLACK          = 0,
    VGA_COLOUR_BLUE           = 1,
    VGA_COLOUR_GREEN          = 2,
    VGA_COLOUR_CYAN           = 3,
    VGA_COLOUR_RED            = 4,
    VGA_COLOUR_MAGENTA        = 5,
    VGA_COLOUR_BROWN          = 6,
    VGA_COLOUR_LIGHT_GREY     = 7,
    VGA_COLOUR_DARK_GREY      = 8,
    VGA_COLOUR_LIGHT_BLUE     = 9,
    VGA_COLOUR_LIGHT_GREEN    = 10,
    VGA_COLOUR_LIGHT_CYAN     = 11,
    VGA_COLOUR_LIGHT_RED      = 12,
    VGA_COLOUR_LIGHT_MAGENTA  = 13,
    VGA_COLOUR_YELLOW         = 14,
    VGA_COLOUR_WHITE          = 15
} VgaColour;

/*
 * Prepares the driver for use, selecting the default colour pair and clearing
 * the display. This must be called before any other function declared here.
 */
void VgaInitialise(void);

/*
 * Selects the foreground and background colours applied to characters written
 * subsequently.
 */
void VgaSetColour(VgaColour foreground, VgaColour background);

/*
 * Fills the entire display with space characters in the current colour pair and
 * returns the cursor to the origin.
 */
void VgaClear(void);

/*
 * Writes a single character at the cursor position and advances the cursor. The
 * line feed character advances to the beginning of the following row; the
 * carriage return returns to the beginning of the current row; the horizontal
 * tabulation advances to the next multiple of eight columns. The backspace moves
 * the cursor one column to the left without erasing, as ANSI X3.4-1986 defines
 * it, and has no effect in the first column; a caller that means to erase writes
 * "\b \b". The display is scrolled upward by one row when the cursor passes the
 * final row.
 */
void VgaPutCharacter(char character);

/*
 * Reports the position at which the next character will be written. Either
 * pointer may be null, in which case that component is not reported. It exists
 * so that the boot-time self-test may assert the cursor movements above, which
 * are otherwise observable only by a person looking at the display.
 */
void VgaCursorPosition(size_t *row, size_t *column);

/*
 * Writes a null-terminated string by repeated application of VgaPutCharacter.
 */
void VgaWriteString(const char *string);

#endif /* OXYS_VGA_H */
