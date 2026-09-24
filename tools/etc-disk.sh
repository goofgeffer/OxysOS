#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
# SPDX-License-Identifier: CC0-1.0
# ==============================================================================
# File: tools/etc-disk.sh
#
# Purpose:
#   Makes and checks the disk that holds the persistent `/etc`: an EXT2 volume
#   labelled `oxys-etc`, which the kernel mounts over the ramdisk's `/etc` at
#   start and seeds from it. docs/storage/PERSIST.md.
#
# Usage:
#   tools/etc-disk.sh create [image]   Makes the image, refusing one that exists.
#   tools/etc-disk.sh check  [image]   Reports what e2fsck finds, altering nothing.
#   tools/etc-disk.sh repair [image]   Lets e2fsck repair what it safely can.
#   tools/etc-disk.sh list   [image]   Lists the files upon it, by debugfs.
#
#   The image is OXYS_ETC_DISK, or ~/oxys-disks/oxys-etc.img.
#
# Why outside the repository, and why `create` refuses an existing image:
#   The image holds what a person edited upon the running machine. Under
#   build/ it would be removed by `make clean`; inside the tree it would be one
#   `git add -A` from being committed; and a `create` that overwrote it would
#   discard every setting a person had made. The build archive is kept outside
#   the tree for the first of those reasons, docs/project/BUILDS.md.
#
# Why these options to mke2fs:
#   The same as the initial ramdisk's, Makefile: ext2, blocks of 1024 bytes,
#   revision 1 — the format the kernel's EXT2 driver was written for and is
#   asserted against. Four megabytes is a thousand times what `/etc` holds.
# ==============================================================================

set -euo pipefail

command=${1:-}
image=${2:-${OXYS_ETC_DISK:-$HOME/oxys-disks/oxys-etc.img}}

case "$command" in
create)
    if [ -e "$image" ]; then
        echo "etc-disk: $image exists and holds whatever was edited upon it; it is left as it is." >&2
        echo "etc-disk: remove it by hand to begin again with the shipped /etc." >&2
        exit 1
    fi
    case "$(realpath -m "$image")" in
        "$(realpath "$(dirname "$0")/..")"/*)
            echo "etc-disk: $image is inside the repository; keep it outside, where make clean and git cannot reach it." >&2
            exit 1 ;;
    esac
    mkdir -p "$(dirname "$image")"
    truncate -s 4M "$image"
    mke2fs -q -F -t ext2 -b 1024 -r 1 -L oxys-etc "$image"
    echo "etc-disk: $image is made, labelled oxys-etc and empty; the kernel seeds it at its first start."
    ;;
check)
    e2fsck -f -n "$image"
    ;;
repair)
    e2fsck -f -p "$image"
    ;;
list)
    debugfs -R "ls -l /" "$image"
    ;;
*)
    sed -n '/^# Usage:/,/^# Why outside/p' "$0" | sed '$d; s/^# \{0,1\}//'
    exit 2
    ;;
esac
