#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
# SPDX-License-Identifier: CC0-1.0
# ==============================================================================
# File: tools/spdx.sh
#
# Purpose:
#   Checks that every tracked file carries the SPDX licence tag that
#   LICENSING.md, Section 1, assigns to its path — and, when asked, adds the tag
#   to the files that lack one.
#
# Usage:
#   tools/spdx.sh            Report what is missing or wrong. Changes nothing.
#   tools/spdx.sh --check    The same; the default, stated explicitly.
#   tools/spdx.sh --apply    Add the tag to every file missing one.
#   tools/spdx.sh --list     Print each file and the identifier it should carry.
#
#   Exit status is 0 when every file is correct and 1 when any is not, so that
#   `make spdx-check` fails a build the way a compiler diagnostic does.
#
# Why this exists:
#   LICENSING.md maps a path to a licence in a table. A table is authoritative
#   and is not machine-readable, so the mapping was true only for as long as
#   somebody remembered it — and a file added to a directory nobody thought
#   about carries no licence at all until the omission is noticed by accident.
#   That is what happened to `crypto/`, `net/` and `uefi/`, which stood without a
#   licence from the day LICENSING.md was written.
#
#   The rules below are that table, expressed once, in a form that can be run.
#   **When the table changes, this file changes with it**, and `make spdx-check`
#   is what makes a disagreement between the two visible.
#
# The one thing to be careful of:
#   A file's *first* matching rule wins, so the specific exceptions must be
#   tested before the directory rules. `boot/grub/grub.cfg` lives under `boot/`
#   and is CC0, not LGPL; `boot/README.md` likewise; and `kernel/abi/` is MIT
#   although everything else under `kernel/` is LGPL, that directory being the
#   interface a program is entitled to rather than the kernel's own corpus.
#   Reordering the rules below would relicense all three without anything saying
#   so.
# ==============================================================================

set -o errexit
set -o nounset
set -o pipefail

readonly COPYRIGHT='2026 The Oxys-OS Authors'
readonly TAG='SPDX-License-Identifier'
readonly OWNER_TAG='SPDX-FileCopyrightText'

# How many lines from the top of a file a tag may appear in and still count.
# Generous enough for a shebang and a blank line, tight enough that a tag quoted
# in the middle of a document is not mistaken for the file's own.
readonly SCAN_LINES=6

repository_root() {
    git -C "$(dirname "$0")" rev-parse --show-toplevel
}

# ------------------------------------------------------------------ the rules

# Paths that carry no tag at all, and why each is exempt.
#
#   LICENSES/       Verbatim upstream licence texts. Adding a line to one would
#                   alter a document that must be reproduced exactly.
#   tools/spdx-exceptions
#                   The exception list itself; see below.
#
# A ported third-party tool is exempt too, and is not named here because none
# exists yet. PROJECT_GUIDELINES.md, Section 2, requires a port to live in a
# directory of its own and to keep the licence it arrived under; when the first
# arrives, its directory is added to tools/spdx-exceptions rather than to this
# script, so that adding a port does not mean editing a program.
is_exempt() {
    local path="$1"

    case "$path" in
        LICENSES/*)             return 0 ;;
        tools/spdx-exceptions)  return 0 ;;
    esac

    if [ -f tools/spdx-exceptions ]; then
        local pattern
        while IFS= read -r pattern; do
            case "$pattern" in
                ''|'#'*) continue ;;
            esac

            # shellcheck disable=SC2254  # the pattern is a glob by intent.
            case "$path" in
                $pattern) return 0 ;;
            esac
        done < tools/spdx-exceptions
    fi

    return 1
}

# The identifier a path must carry, or the empty string where none applies.
#
# This is LICENSING.md, Section 1, in order. The exceptions come first for the
# reason the file header gives.
identifier_for() {
    local path="$1"

    case "$path" in
        # --- the CC0 exceptions, which override the directory rules below ---
        */README.md|README.md)   echo 'CC0-1.0'; return ;;
        docs/*)                  echo 'CC0-1.0'; return ;;
        PROJECT_GUIDELINES.md|CONTRIBUTING.md|CODE_OF_CONDUCT.md|SECURITY.md|LICENSING.md)
                                 echo 'CC0-1.0'; return ;;
        Makefile|build_*.sh|boot/grub/grub.cfg|.gitignore|.gitattributes)
                                 echo 'CC0-1.0'; return ;;
        .github/*|tools/*)       echo 'CC0-1.0'; return ;;

        # --- the interface a program is entitled to, which is permissive so
        #     that an MIT C library may include it; it must be tested before the
        #     kernel rule below, which would otherwise claim it ---
        kernel/abi/*)            echo 'MIT'; return ;;

        # --- the kernel image: what is linked into it ---
        boot/*|kernel/*|drivers/*|graphics/*|crypto/*|net/*|uefi/*|linker.ld)
                                 echo 'LGPL-3.0-or-later'; return ;;

        # --- programs that run upon it ---
        libc/*|userland/*)       echo 'MIT'; return ;;
    esac

    echo ''
}

# The comment syntax a path's tag must be written in.
#
# Printed as three fields — opener, closer, and whether the tag must follow a
# shebang — because that is the whole of what differs between them.
comment_style_for() {
    local path="$1"

    case "$path" in
        *.c|*.h|*.ld)                 echo '/* */ no'  ;;
        *.asm)                        echo '; {} no'   ;;
        *.md)                         echo '<!-- --> no' ;;
        *.sh)                         echo '# {} yes'  ;;
        *.yml|*.yaml|*.cfg|*.tsv|Makefile|.gitignore|.gitattributes)
                                      echo '# {} no'   ;;
        *)                            echo ''          ;;
    esac
}

# ------------------------------------------------------------------- the work

# The identifier a file already carries, or the empty string.
existing_identifier() {
    local path="$1"

    head -n "$SCAN_LINES" "$path" 2>/dev/null \
        | grep -m1 -o "$TAG:[[:space:]]*[A-Za-z0-9.+-]*" \
        | sed "s/$TAG:[[:space:]]*//" \
        || true
}

# Writes the two tag lines into a file, in the comment syntax its type requires.
insert_tag() {
    local path="$1" identifier="$2"
    local style opener closer after_shebang
    local temporary

    style="$(comment_style_for "$path")"

    if [ -z "$style" ]; then
        return 1
    fi

    read -r opener closer after_shebang <<< "$style"
    [ "$closer" = '{}' ] && closer=''

    temporary="$(mktemp)"

    {
        # A shebang must remain the first line, or the file stops being
        # executable by the kernel that reads it.
        if [ "$after_shebang" = 'yes' ] && head -n1 "$path" | grep -q '^#!'; then
            head -n1 "$path"
            emit_tag "$opener" "$closer" "$identifier"
            tail -n +2 "$path"
        else
            emit_tag "$opener" "$closer" "$identifier"
            cat "$path"
        fi
    } > "$temporary"

    # The mode is preserved deliberately: the build scripts are executable and
    # would stop being so if this replaced them with a fresh file.
    chmod --reference="$path" "$temporary"
    mv "$temporary" "$path"

    return 0
}

emit_tag() {
    local opener="$1" closer="$2" identifier="$3"

    if [ -n "$closer" ]; then
        printf '%s %s: %s %s\n' "$opener" "$OWNER_TAG" "$COPYRIGHT" "$closer"
        printf '%s %s: %s %s\n' "$opener" "$TAG" "$identifier" "$closer"
    else
        printf '%s %s: %s\n' "$opener" "$OWNER_TAG" "$COPYRIGHT"
        printf '%s %s: %s\n' "$opener" "$TAG" "$identifier"
    fi
}

main() {
    local mode='check'
    local missing=0 wrong=0 applied=0 unknown=0 correct=0

    case "${1:-}" in
        ''|--check) mode='check'  ;;
        --apply)    mode='apply'  ;;
        --list)     mode='list'   ;;
        --help|-h)  sed -n '2,30p' "$0"; exit 0 ;;
        *)          echo "spdx.sh: unknown argument '$1'" >&2; exit 2 ;;
    esac

    cd "$(repository_root)"

    local path expected actual
    while IFS= read -r path; do
        [ -f "$path" ] || continue
        is_exempt "$path" && continue

        expected="$(identifier_for "$path")"

        if [ -z "$expected" ]; then
            echo "UNASSIGNED  $path"
            echo "            No rule in LICENSING.md, Section 1, covers this path."
            unknown=$((unknown + 1))
            continue
        fi

        if [ "$mode" = 'list' ]; then
            printf '%-20s %s\n' "$expected" "$path"
            continue
        fi

        actual="$(existing_identifier "$path")"

        if [ "$actual" = "$expected" ]; then
            correct=$((correct + 1))
            continue
        fi

        if [ -n "$actual" ]; then
            echo "MISMATCH    $path"
            echo "            carries $actual, should carry $expected"
            wrong=$((wrong + 1))
            continue
        fi

        if [ "$mode" = 'apply' ]; then
            if insert_tag "$path" "$expected"; then
                echo "ADDED       $expected  $path"
                applied=$((applied + 1))
            else
                echo "NO SYNTAX   $path"
                echo "            This file type has no comment syntax defined here."
                unknown=$((unknown + 1))
            fi
        else
            echo "MISSING     $expected  $path"
            missing=$((missing + 1))
        fi
    done < <(git ls-files)

    [ "$mode" = 'list' ] && exit 0

    echo
    echo "SPDX: $correct correct, $missing missing, $wrong mismatched, $applied added, $unknown unassigned."

    # A mismatch is never repaired automatically. A file whose tag disagrees with
    # the table is either a file in the wrong place or a table that is wrong, and
    # neither is a thing a script should decide.
    if [ "$wrong" -gt 0 ] || [ "$unknown" -gt 0 ] || [ "$missing" -gt 0 ]; then
        exit 1
    fi

    exit 0
}

main "$@"
