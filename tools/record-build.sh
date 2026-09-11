#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
# SPDX-License-Identifier: CC0-1.0
# ==============================================================================
# File: tools/record-build.sh
#
# Purpose:
#   Appends one numbered row to docs/project/BUILDS.md, the register of every
#   image this project has produced: what was built, from which commit, by which
#   compiler, how large it came out, and what the verification said.
#
# Usage:
#   tools/record-build.sh [--environment NAME] [--result WORD] [note...]
#   make build-record NOTE="what this build is"
#
#   --environment  Where the image was run, if anywhere. Repeatable. Free text,
#                  because the set of environments is not this script's to fix:
#                  docs/project/TESTING.md names four and a fifth will not be a
#                  change to a program.
#   --result       Overrides what the script would conclude from the serial log.
#                  Intended for a run this script cannot see the evidence of —
#                  a boot under VirtualBox, or upon real hardware.
#
#   Everything else on the command line is the note, which is the one field a
#   person supplies and the only one that says *why* the build was made.
#
# Why this exists:
#   Every other record in this repository answers a question about the source:
#   PLAN.md what is being built, HISTORY.md how it came to be, TESTING-RECORD.md
#   what was tested. None of them answers "which image was that?" — and by
#   sub-task 7.2 this project had produced some hundreds of images, of which not
#   one could be named. A defect observed under Bochs and not under QEMU, or a
#   size that grew by a hundred kibibytes between two runs, has nothing to be
#   attached to without a number.
#
#   So every build gets one, and the number is the thing a later document can
#   cite. The register is Markdown and is committed, because a database that is
#   not in the repository is a database that is on one machine.
#
# What it does not do:
#   It does not build anything and it does not run anything. It reads the tree
#   and the artefacts that are already there, which is what makes it safe to run
#   after any of `make all`, `make iso`, `make verify`, or a boot in an emulator
#   whose evidence a person is recording by hand. A script that rebuilt in order
#   to record would be recording something other than what was run.
# ==============================================================================

set -o nounset
set -o pipefail

cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)" || exit 2

# BUILD_DIR is the Makefile's variable of the same name, so that an image built
# elsewhere — `make BUILD_DIR=build-clang` produces one — is recorded from where
# it actually is rather than from where the default build would have put it.
BUILD_DIR="${BUILD_DIR:-build}"

REGISTER='docs/project/BUILDS.md'
KERNEL="$BUILD_DIR/oxys.elf"
ISO="$BUILD_DIR/oxys.iso"
SERIAL="$BUILD_DIR/serial.log"

environments=''
result=''
note=''

while [ "$#" -gt 0 ]; do
    case "$1" in
    --environment)
        [ "$#" -ge 2 ] || { printf 'ERROR: --environment needs a name.\n' >&2; exit 2; }
        if [ -z "$environments" ]; then
            environments="$2"
        else
            environments="$environments, $2"
        fi
        shift 2
        ;;
    --result)
        [ "$#" -ge 2 ] || { printf 'ERROR: --result needs a word.\n' >&2; exit 2; }
        result="$2"
        shift 2
        ;;
    *)
        if [ -z "$note" ]; then
            note="$1"
        else
            note="$note $1"
        fi
        shift
        ;;
    esac
done

[ -f "$REGISTER" ] || { printf 'ERROR: %s does not exist.\n' "$REGISTER" >&2; exit 2; }

# ---------------------------------------------------------------------------
# The number.
#
# It is one more than the greatest already in the register, read from the file
# rather than kept in a counter of its own. A counter is a second thing that can
# disagree with the table, and the table is the record.
# ---------------------------------------------------------------------------
previous="$(grep -oE '^\| [0-9]+ \|' "$REGISTER" | tr -d '| ' | sort -n | tail -1)"
number=$(( ${previous:-0} + 1 ))

# ---------------------------------------------------------------------------
# What was built, and from what.
#
# The commit carries a `-modified` suffix where the working tree differs from
# it, because an image built from an edited tree is not the image that commit
# produces and a row that said otherwise would be the worst kind of record: one
# that is wrong and looks precise.
# ---------------------------------------------------------------------------
commit="$(git rev-parse --short HEAD 2>/dev/null || echo 'none')"

if ! git diff --quiet HEAD 2>/dev/null; then
    commit="$commit-modified"
fi

compiler="$(${CC:-x86_64-elf-gcc} --version 2>/dev/null | head -1 \
            | sed 's/ (.*)//; s/  */ /g')"
[ -n "$compiler" ] || compiler='unknown'

size_of() {
    if [ -f "$1" ]; then
        stat -c '%s' "$1"
    else
        echo '—'
    fi
}

kernel_bytes="$(size_of "$KERNEL")"
iso_bytes="$(size_of "$ISO")"

# ---------------------------------------------------------------------------
# What the verification said.
#
# Read from the serial log the `verify` target leaves behind, by the same two
# conditions that target applies: the banner, and the absence of any verdict of
# FAILED. A log that is not there is recorded as such rather than as a pass.
#
# The count of assertions is the one tools/check-docs.sh uses — every occurrence
# of "passed" and of "sound" in the log — and it is written the same way here on
# purpose. Two programs reporting two different numbers for something both call
# the assertion count would be worse than neither reporting it.
# ---------------------------------------------------------------------------
if [ -n "$result" ]; then
    :
elif [ ! -f "$SERIAL" ]; then
    result='not run'
elif ! grep -q 'initialisation complete.' "$SERIAL"; then
    result='did not boot'
elif grep -q 'FAILED' "$SERIAL"; then
    result="failed ($(grep -c 'FAILED' "$SERIAL") verdict(s))"
else
    result="passed ($(( $(grep -o 'passed' "$SERIAL" | wc -l) \
                      + $(grep -o 'sound' "$SERIAL" | wc -l) )) assertions)"
fi

[ -n "$environments" ] || environments='—'
[ -n "$note" ] || note='—'

row="| $number | $(date -u '+%Y-%m-%d %H:%M') | \`$commit\` | $compiler | $kernel_bytes | $iso_bytes | $result | $environments | $note |"

printf '%s\n' "$row" >> "$REGISTER"
printf 'Recorded build %d.\n%s\n' "$number" "$row"
