<!-- SPDX-FileCopyrightText: 2026 The Oxys-OS Authors -->
<!-- SPDX-License-Identifier: CC0-1.0 -->
# `fonts/` — Third-Party Fonts

**Phase**: 9 of [`../docs/project/PLAN.md`](../docs/project/PLAN.md), sub-task 9.9.
**Detailed design**: [`../docs/design/CONSOLE.md`](../docs/design/CONSOLE.md), Section 4.
**Licence**: each font under its own, recorded in
[`../LICENSING.md`](../LICENSING.md), Section 4; this README is `CC0-1.0`.

## Purpose

Typefaces written elsewhere, kept with their upstream licence and notice, as
`PROJECT_GUIDELINES.md`, Section 2, permits. Nothing here is compiled as
source: the kernel includes a coverage table generated from a font.

## Contents

| Path | Description |
| ---- | ----------- |
| [`inter/Inter-Medium.ttf`](inter/Inter-Medium.ttf) | Inter 4.1, Medium weight, from the release archive `Inter-4.1.zip` of <https://github.com/rsms/inter> (SHA-256 of the archive `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`). The source `tools/face.sh` renders. Its tag is in `Inter-Medium.ttf.license`. |
| [`inter/face.h`](inter/face.h) | The desktop's face: the font rendered by [`../tools/face.sh`](../tools/face.sh) into cells of five by eight units at scales one to four, one byte of coverage to a pixel, with each glyph's proportional advance and offset. Generated; regenerate it rather than editing it. Included only by `graphics/face.c`. |

## Licence

Inter is Copyright 2016 The Inter Project Authors and is licensed under the SIL
Open Font License 1.1, whose text is
[`../LICENSES/OFL-1.1.txt`](../LICENSES/OFL-1.1.txt). The licence reserves no
font name. It permits the font, and a version of it converted to another
format, to be bundled with software under any licence, which is how `face.h` is
compiled into the `LGPL-3.0-or-later` kernel; the converted version stays under
the OFL, and `face.h` carries that licence and the notice.
