#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
# SPDX-License-Identifier: CC0-1.0
# ==============================================================================
# File: tools/face.sh
#
# Purpose:
#   Renders the desktop's face, fonts/inter/face.h, from
#   fonts/inter/Inter-Medium.ttf: the ninety-five printable ASCII glyphs in the
#   cells of text scales one to four (5 by 8, 10 by 16, 15 by 24 and 20 by 32
#   pixels), one byte of coverage to a pixel, and each glyph's proportional
#   advance and offset at each scale.
#
# Usage:
#   tools/face.sh
#
#   Run it by hand when the font or the metrics below change, and commit the
#   header it writes. It is not a Makefile rule, so that no build depends on
#   ImageMagick; art/README.md makes the same choice for the mark.
#
# The metrics:
#   A cell is five units wide and eight high, near the proportion of a monospaced
#   face, so text laid out on a grid reads as words and not as spaced letters.
#   The baseline stands three-quarters of the way down and the size is 0.82 of
#   the height, which puts a capital about 0.6 of the height high and leaves
#   room for the descenders. Each glyph is centred by its ink, and the few whose
#   ink is wider than the cell (W, M, m, w, @) are narrowed to fit it rather than
#   cut off.
#
# Why the output is OFL-1.1:
#   A rendering of the font into another format is a Modified Version under the
#   SIL Open Font License, so the header carries the font's licence and notice
#   (LICENSING.md, Section 4).
# ==============================================================================

set -euo pipefail

font="fonts/inter/Inter-Medium.ttf"
output="fonts/inter/face.h"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

command -v convert >/dev/null || { echo "face.sh: ImageMagick's convert is required" >&2; exit 1; }
[ -f "$font" ] || { echo "face.sh: $font is missing" >&2; exit 1; }

# The text ImageMagick draws, escaped: `%` begins a property escape, `\` an
# escape, and a leading `@` names a file to read the text from.
annotation() {
    case "$1" in
        '%')  printf '%%%%' ;;
        '\')  printf '\\\\' ;;
        '@')  printf '\\@' ;;
        *)    printf '%s' "$1" ;;
    esac
}

# The pen's advance past a run of text, in pixels and fractions of one, as
# FreeType reports it.
advance_of() {
    convert -debug annotate xc: -font "$font" -pointsize "$2" -annotate 0 "$1" null: 2>&1 |
        sed -n 's/.*origin: \([0-9.-]*\),.*/\1/p' | head -n 1
}

# One glyph in a cell of `width` by `height`, as rows of decimal coverage; its
# advance, in sixteenths of a pixel, and the offset of its cell from the pen, in
# pixels, are appended to `metrics`.
render() {
    local character="$1" width="$2" height="$3" metrics="$4" size baseline text box ink left
    local narrow advance first

    size="$(awk -v h="$height" 'BEGIN { printf "%.2f", h * 0.82 }')"
    baseline=$((height * 3 / 4))
    text="$(annotation "$character")"

    if [ "$character" = ' ' ]; then
        # A lone space reports no metrics, so its advance is what it adds
        # between two letters.
        awk -v a="$(advance_of 'a a' "$size")" -v b="$(advance_of 'aa' "$size")" \
            'BEGIN { printf "%d 0\n", ((a - b) * 16) + 0.5 }' >> "$metrics"
        head -c $((width * height)) /dev/zero | od -An -v -tu1 -w"$width"
        return
    fi

    # The ink's extent, on a canvas wide enough for any glyph, the pen a
    # height in from the left.
    box="$(convert -size "$((height * 3))x$height" xc:black -font "$font" -pointsize "$size" \
               -fill white -annotate "+$height+$baseline" "$text" -format '%@' info:)"
    ink="${box%%x*}"
    left="${box#*+}"
    left="${left%%+*}"
    narrow="$ink"
    [ "$ink" -gt "$width" ] && narrow="$width"

    convert -size "$((height * 3))x$height" xc:black -font "$font" -pointsize "$size" \
        -fill white -annotate "+$height+$baseline" "$text" \
        -crop "${ink}x$height+$left+0" +repage -resize "${narrow}x$height!" \
        -background black -gravity center -extent "${width}x$height" -depth 8 "gray:$work/glyph"

    # Proportionally, the pen advances by the font's own advance, less what a
    # glyph too wide for the cell was narrowed by; the cell stands so that its
    # first inked column falls where the font puts the ink.
    advance="$(awk -v a="$(advance_of "$text" "$size")" -v n=$((ink - narrow)) \
        'BEGIN { printf "%d", ((a - n) * 16) + 0.5 }')"
    first="$(od -An -v -tu1 -w"$width" "$work/glyph" |
        awk '{ for (i = 1; i <= NF; ++i) if ($i > 0 && (m == "" || i - 1 < m)) m = i - 1 }
             END { print (m == "") ? 0 : m }')"
    echo "$advance $(( (left - height) - first ))" >> "$metrics"

    od -An -v -tu1 -w"$width" "$work/glyph"
}

{
    cat <<'HEADER'
/* SPDX-FileCopyrightText: 2016 The Inter Project Authors (https://github.com/rsms/inter) */
/* SPDX-License-Identifier: OFL-1.1 */
/*
 * File: fonts/inter/face.h
 * Purpose: Inter 4.1 Medium rendered into cells of 5 by 8 units at scales one
 *          to four, one byte of coverage to a pixel, for the desktop's face.
 * Key definitions: FACE_FIRST_CODE, FACE_GLYPHS, FACE_SIZES, FaceCell1 to
 *          FaceCell4, FaceAdvance, FaceOffset.
 * References:
 *   - tools/face.sh: generated by it; do not edit by hand.
 *   - LICENSING.md, Section 4: the font, its source and its licence.
 *   - docs/design/CONSOLE.md: how graphics/face.c draws these.
 *
 * Licensed under the SIL Open Font License, Version 1.1, whose text is
 * LICENSES/OFL-1.1.txt. It is included only by graphics/face.c, which
 * provides the fixed-width types: the kernel has no <stdint.h> of its own.
 */

#ifndef OXYS_FONTS_INTER_FACE_H
#define OXYS_FONTS_INTER_FACE_H

#define FACE_FIRST_CODE 0x20U
#define FACE_GLYPHS     95U
#define FACE_SIZES      4U
HEADER

    for scale in 1 2 3 4; do
        width=$((scale * 5))
        height=$((scale * 8))
        printf '\nstatic const uint8_t FaceCell%d[FACE_GLYPHS][%d] = {\n' "$scale" $((width * height))
        for code in $(seq 32 126); do
            character="$(printf "\\$(printf '%03o' "$code")")"
            [ "$code" -eq 32 ] && character=' '
            printf '    { /* 0x%02X */\n' "$code"
            render "$character" "$width" "$height" "$work/metrics$scale" |
                sed 's/^ *//; s/  */, /g; s/^/        /; s/$/,/'
            printf '    },\n'
        done
        printf '};\n'
    done

    # A table of one column of the metrics, a row of glyphs to each scale.
    metric_table() {
        local type="$1" name="$2" column="$3" comment="$4"
        printf '\n/* %s */\nstatic const %s %s[FACE_SIZES][FACE_GLYPHS] = {\n' "$comment" "$type" "$name"
        for scale in 1 2 3 4; do
            printf '    {\n'
            awk -v c="$column" '{ printf "%s%d,", (NR % 16 == 1) ? "        " : " ", $c; if (NR % 16 == 0) print "" }
                                END { if (NR % 16 != 0) print "" }' "$work/metrics$scale"
            printf '    },\n'
        done
        printf '};\n'
    }

    metric_table uint16_t FaceAdvance 1 \
        'How far the pen moves past each glyph in proportional text, in sixteenths of a pixel.'
    metric_table int8_t FaceOffset 2 \
        "Where each glyph's cell stands relative to the pen in proportional text."

    printf '\n#endif /* OXYS_FONTS_INTER_FACE_H */\n'
} > "$output"

echo "face.sh: wrote $output"
