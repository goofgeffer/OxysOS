#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
# SPDX-License-Identifier: CC0-1.0
# ==============================================================================
# File: tools/builds.sh
#
# Purpose:
#   The whole of the build register: the record of every image this project has
#   produced, the queries over it, the human-readable view generated from it,
#   and the validation that the three agree.
#
# Usage:
#   tools/builds.sh record [options] [note...]   append one build
#   tools/builds.sh query  [options]             select from the record
#   tools/builds.sh render                       regenerate BUILDS.md's view
#   tools/builds.sh archive <n> <iso>            keep the image of a build already recorded
#   tools/builds.sh check [--deep]               validate the record, the view and the archive
#   tools/builds.sh sql <statement>              SQL over the record, if sqlite3 is here
#
#   `make build-record NOTE="…"` invokes the first of those, and
#   `make build-record ARCHIVE=1 NOTE="…"` keeps the image with it.
#
# Why the record is a delimited file and not a Markdown table:
#
#   It was a Markdown table, and a Markdown table is a view rather than a
#   record. Three properties it could not have:
#
#   **A bounded document.** Every build appended a row to the file a person
#   opens, so the document a reader meets grew without limit and the prose
#   explaining it sank further from the top with each build.
#
#   **A schema.** Every field was free text, so "passed (55 assertions)",
#   "x86_64-elf-gcc 13.2.0" and a list of environments separated by commas were
#   each one opaque string. Nothing could be counted, filtered or compared, and
#   nothing prevented the next row from spelling any of them differently.
#
#   **A check.** A record nothing validates drifts, which is the defect every
#   other script in this directory was written after.
#
#   So docs/project/builds.tsv is the record and docs/project/BUILDS.md is a view
#   generated from it. The record is tab-separated rather than JSON because
#   every tool in this directory is bash and coreutils and nothing else: `jq` is
#   not present upon this machine, parsing JSON with awk is a way of being wrong
#   slowly, and a new host dependency for a record of build numbers is a poor
#   trade. `awk -F'\t'` reads this correctly in one expression.
#
# Why not SQLite, which is what "a database" usually means:
#
#   Because git is this project's storage, and a SQLite file is a binary. It
#   would be the first binary in the repository: `git diff` would say "Binary
#   files differ" for the rest of the project's life, code review could not read
#   it, and two machines recording a build would produce a merge conflict no
#   person could resolve.
#
#   The `sql` subcommand gives the query power without the cost. It materialises
#   a throwaway database from the record, runs the statement, and deletes it —
#   so SQL is available where sqlite3 is installed and the repository still holds
#   nothing but text. Nothing here requires sqlite3; only that one subcommand
#   uses it, and it says so plainly when it is absent.
#
# The archive, and why the record alone was not enough:
#
#   A row describes an image. It is not the image, and for the first ten builds
#   nothing kept the image at all — `build/` is ignored by git and `make clean`
#   removes it, so six of the first eight were unrecoverable within a day of
#   being recorded. The record said they existed and nothing said they were gone.
#
#   `record --archive` and the `archive` subcommand keep the ISO, compressed, in
#   a directory outside the working tree; the `sha256` column then names what was
#   kept, and `check` fails when a row claims an image the archive does not hold.
#   A row whose hash is `-` claims nothing, which is the honest state for a build
#   whose image was thrown away.
#
#   OXYS_BUILD_ARCHIVE selects the directory and defaults to ~/oxys-builds. It
#   must be outside the working tree and this script refuses otherwise: at one to
#   two builds a day an archived image is about a gigabyte a year, and a
#   gigabyte of ISOs committed to git cannot be taken out again.
#
#   Keeping an image is not the same as being able to reproduce one. A build
#   recorded with `dirty=no` can be rebuilt from its commit whether or not its
#   image was kept; one recorded `dirty=yes` was compiled from a tree that was
#   never committed and is gone the moment its image is. The two columns answer
#   different questions and neither substitutes for the other.
#
# What this script does not do:
#   It does not build anything and it does not run anything. It reads the
#   artefacts already in the build directory and the serial log the `verify`
#   target leaves behind, which is what makes it safe to call after any target
#   and after a boot a person observed themselves in an environment that leaves
#   no log at all.
# ==============================================================================

set -o nounset
set -o pipefail

cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)" || exit 2

RECORD='docs/project/builds.tsv'
VIEW='docs/project/BUILDS.md'
VIEW_ROWS=20

BEGIN_MARK='<!-- BEGIN GENERATED: tools/builds.sh render -->'
END_MARK='<!-- END GENERATED -->'

# The columns, in order, and the vocabulary the constrained ones are drawn from.
#
# `number` and not `#` because a leading `#` is this file type's comment
# character, which the SPDX tag at the head of the record uses; a header whose
# first field began with one could not be told from a comment.
COLUMNS='number	date	commit	dirty	compiler	cc_version	kernel	iso	result	assertions	environments	sha256	note'
RESULTS='passed failed did-not-boot not-run other'

# ------------------------------------------------------------------------------
# Where the images themselves are kept.
#
# The record names an image; this is where the image is. They are separate
# because they have opposite storage properties: the record is small, textual
# and belongs in git forever, and an ISO is seven megabytes of binary that git
# would keep a full copy of for the rest of the repository's life. At one to two
# builds a day that is about a gigabyte a year appended to a history that cannot
# be rewritten, against a repository presently 28 MB entire.
#
# So the archive is outside the working tree, and this script refuses to put it
# inside one. That refusal is a check rather than a convention because a default
# that is merely documented is a default somebody overrides at the moment it
# matters, and the consequence here — a binary in git history — is the kind that
# cannot be undone.
# ------------------------------------------------------------------------------

ARCHIVE_ROOT="${OXYS_BUILD_ARCHIVE:-$HOME/oxys-builds}"

archive_root_is_safe() {
    local root resolved tree
    root="$ARCHIVE_ROOT"
    resolved="$(cd "$(dirname "$root")" 2>/dev/null && pwd)/$(basename "$root")"
    tree="$(git rev-parse --show-toplevel 2>/dev/null)"

    [ -n "$tree" ] || return 0

    case "$resolved/" in
    "$tree"/*)
        fail "the archive root $resolved is inside the working tree $tree."
        fail 'Set OXYS_BUILD_ARCHIVE to a path outside it; an ISO in git history cannot be removed.'
        return 1
        ;;
    esac
    return 0
}

# The name an image is kept under. The build number is what joins it to the
# record; the commit and the compiler are there so that a directory listing is
# readable without opening the register beside it.
archive_path() {
    printf '%s/oxys-%04d-%s-%s.iso.xz\n' "$ARCHIVE_ROOT" "$1" "$2" "$3"
}

fail() {
    printf 'ERROR    %s\n' "$1" >&2
}

note() {
    printf 'NOTE     %s\n' "$1"
}

usage() {
    sed -n '/^# Usage:/,/^#$/p' "$0" | sed 's/^# \{0,2\}//'
}

# ---------------------------------------------------------------------------
# Reading the record.
#
# Every reader goes through this, so the two kinds of line that are not data —
# the SPDX tag and the header — are skipped in exactly one place.
# ---------------------------------------------------------------------------
rows() {
    [ -f "$RECORD" ] || return 0
    awk -F'\t' 'NR > 0 && $0 !~ /^#/ && $1 != "number" && NF > 1' "$RECORD"
}

# A field cannot hold a tab or a newline without ceasing to be a field, and a
# field a person typed can hold anything. Both become spaces, and an empty field
# becomes the placeholder, so that every row has the same shape.
sanitise() {
    printf '%s' "$1" | tr '\t\n' '  ' | sed 's/  */ /g; s/^ //; s/ $//' \
        | awk '{ print (length($0) ? $0 : "-") }'
}

# =============================================================== record

do_record() {
    local build_dir="${BUILD_DIR:-build}"
    local environments='' result='' assertions='' note='' archive=0 sha256='-'
    local kernel iso commit dirty compiler cc_version cc_line number row

    while [ "$#" -gt 0 ]; do
        case "$1" in
        --environment)
            [ "$#" -ge 2 ] || { fail '--environment needs a name.'; return 2; }
            environments="${environments:+$environments;}$2"
            shift 2
            ;;
        --result)
            [ "$#" -ge 2 ] || { fail '--result needs a word.'; return 2; }
            result="$2"
            shift 2
            ;;
        --assertions)
            [ "$#" -ge 2 ] || { fail '--assertions needs a number.'; return 2; }
            assertions="$2"
            shift 2
            ;;
        --archive)
            archive=1
            shift
            ;;
        --build-dir)
            [ "$#" -ge 2 ] || { fail '--build-dir needs a path.'; return 2; }
            build_dir="$2"
            shift 2
            ;;
        *)
            note="${note:+$note }$1"
            shift
            ;;
        esac
    done

    [ -f "$RECORD" ] || { fail "$RECORD does not exist."; return 2; }

    # The number: one more than the greatest already recorded, read from the
    # record rather than from a counter of its own. A counter is a second thing
    # that can disagree with the record, and the record is the record.
    number=$(( $(rows | awk -F'\t' '{ print $1 }' | sort -n | tail -1 || echo 0) + 1 ))

    commit="$(git rev-parse --short HEAD 2>/dev/null || echo 'none')"

    # Whether the tree differed from that commit is a column and no longer a
    # suffix upon the commit. It was a suffix, and a suffix is a second thing
    # encoded in a field that already means something — `grep 0439d05` did not
    # find `0439d05-modified`, which is exactly backwards.
    if git diff --quiet HEAD 2>/dev/null; then
        dirty='no'
    else
        dirty='yes'
    fi

    cc_line="$(${CC:-x86_64-elf-gcc} --version 2>/dev/null | head -1)"

    case "$cc_line" in
    *clang*) compiler='clang' ;;
    *gcc*|*GCC*) compiler='gcc' ;;
    '') compiler='unknown' ;;
    *) compiler='other' ;;
    esac

    cc_version="$(printf '%s' "$cc_line" | grep -oE '[0-9]+\.[0-9]+(\.[0-9]+)?' | head -1)"
    [ -n "$cc_version" ] || cc_version='-'

    kernel="$(stat -c '%s' "$build_dir/oxys.elf" 2>/dev/null || echo '-')"
    iso="$(stat -c '%s' "$build_dir/oxys.iso" 2>/dev/null || echo '-')"

    # What the verification said, by the same two conditions the `verify` target
    # applies: the banner, and the absence of any verdict of FAILED. The count of
    # assertions is the one tools/check-docs.sh uses, written the same way on
    # purpose — two programs reporting two numbers for it would be worse than
    # neither reporting it.
    local serial="$build_dir/serial.log"

    if [ -n "$result" ]; then
        :
    elif [ ! -f "$serial" ]; then
        result='not-run'
    elif ! grep -q 'initialisation complete.' "$serial"; then
        result='did-not-boot'
    elif grep -q 'FAILED' "$serial"; then
        result='failed'
    else
        result='passed'
    fi

    if [ -z "$assertions" ]; then
        if [ -f "$serial" ] && [ "$result" = 'passed' ]; then
            assertions=$(( $(grep -o 'passed' "$serial" | wc -l) \
                         + $(grep -o 'sound' "$serial" | wc -l) ))
        else
            assertions='-'
        fi
    fi

    case " $RESULTS " in
    *" $result "*) ;;
    *) fail "'$result' is not one of: $RESULTS"; return 2 ;;
    esac

    # The image itself, where it was asked for.
    #
    # The hash is of the **uncompressed** ISO and not of the file kept on disk,
    # because it identifies the artefact rather than this script's storage of it:
    # were the compression ever changed the hash would still name the same image,
    # and a hash of the container would silently not.
    if [ "$archive" -eq 1 ]; then
        local source destination
        source="$build_dir/oxys.iso"

        archive_root_is_safe || return 1

        if [ ! -f "$source" ]; then
            fail "$source does not exist; there is no image to archive."
            return 1
        fi

        command -v xz >/dev/null || { fail 'xz is not installed.'; return 1; }
        command -v sha256sum >/dev/null || { fail 'sha256sum is not installed.'; return 1; }

        sha256="$(sha256sum "$source" | cut -d' ' -f1)"
        destination="$(archive_path "$number" "$commit" "$compiler")"

        mkdir -p "$ARCHIVE_ROOT" || return 1
        if ! xz -9 -c "$source" > "$destination"; then
            fail "could not write $destination."
            rm -f "$destination"
            return 1
        fi

        printf 'Archived %s -> %s (%s bytes compressed).\n' \
            "$source" "$destination" "$(wc -c < "$destination")"
    fi

    row="$number	$(date -u '+%Y-%m-%dT%H:%MZ')	$commit	$dirty	$compiler	$cc_version	$kernel	$iso	$result	$assertions	$(sanitise "$environments")	$sha256	$(sanitise "$note")"

    printf '%s\n' "$row" >> "$RECORD"

    do_render || return 1

    printf 'Recorded build %d.\n' "$number"
    printf '%s\n' "$row" | awk -F'\t' '{ for (i = 1; i <= NF; ++i) printf "  %s\n", $i }'
}

# =============================================================== archive

# Keep the image of a build that was recorded without one.
#
# `record --archive` is the ordinary path and archives at the moment of
# recording. This is the other one: an image still sitting in a build directory
# whose row was written before it could be kept, or before this script could
# keep anything. It exists because the first ten builds were recorded without
# it, and two of their images were still on disk when it was written.
#
# It refuses a row that already carries a hash. Re-archiving would either write
# the same bytes again or, worse, quietly replace the image a hash was computed
# from with a different one — and a record that can be made to disagree with
# itself by running a command twice is not a record.
do_archive() {
    local number="${1:-}" source="${2:-}"
    local commit compiler recorded destination hash temporary

    case "$number" in
    ''|*[!0-9]*) fail 'archive needs a build number: tools/builds.sh archive 9 build/oxys.iso'; return 2 ;;
    esac
    [ -n "$source" ] || { fail 'archive needs the path of the image.'; return 2; }
    [ -f "$source" ] || { fail "$source does not exist."; return 1; }

    archive_root_is_safe || return 1
    command -v xz >/dev/null || { fail 'xz is not installed.'; return 1; }
    command -v sha256sum >/dev/null || { fail 'sha256sum is not installed.'; return 1; }

    commit="$(rows | awk -F'\t' -v n="$number" '$1 == n { print $3 }')"
    compiler="$(rows | awk -F'\t' -v n="$number" '$1 == n { print $5 }')"
    recorded="$(rows | awk -F'\t' -v n="$number" '$1 == n { print $12 }')"

    [ -n "$commit" ] || { fail "the record has no build $number."; return 1; }

    if [ "$recorded" != '-' ]; then
        fail "build $number already records an archived image ($recorded)."
        return 1
    fi

    hash="$(sha256sum "$source" | cut -d' ' -f1)"
    destination="$(archive_path "$number" "$commit" "$compiler")"

    mkdir -p "$ARCHIVE_ROOT" || return 1
    if ! xz -9 -c "$source" > "$destination"; then
        fail "could not write $destination."
        rm -f "$destination"
        return 1
    fi

    temporary="$(mktemp)" || return 1
    awk -F'\t' -v OFS='\t' -v n="$number" -v h="$hash" \
        '!/^#/ && $1 == n { $12 = h } { print }' "$RECORD" > "$temporary"
    mv "$temporary" "$RECORD"

    do_render || return 1

    printf 'Archived build %s: %s -> %s\n' "$number" "$source" "$destination"
    printf '  sha256 %s\n  %s bytes compressed from %s\n' \
        "$hash" "$(wc -c < "$destination")" "$(wc -c < "$source")"
}

# =============================================================== query

do_query() {
    local compiler='' result='' environment='' commit='' since='' last='' format='table'

    while [ "$#" -gt 0 ]; do
        case "$1" in
        --compiler)    compiler="$2"; shift 2 ;;
        --result)      result="$2"; shift 2 ;;
        --environment) environment="$2"; shift 2 ;;
        --commit)      commit="$2"; shift 2 ;;
        --since)       since="$2"; shift 2 ;;
        --last)        last="$2"; shift 2 ;;
        --format)      format="$2"; shift 2 ;;
        *)             fail "unknown option '$1'"; return 2 ;;
        esac
    done

    local selected
    selected="$(rows | awk -F'\t' \
        -v compiler="$compiler" -v result="$result" -v environment="$environment" \
        -v commit="$commit" -v since="$since" '
        {
            if (compiler != "" && $5 != compiler) next
            if (result != "" && $9 != result) next
            if (commit != "" && $3 != commit) next
            if (since != "" && $2 < since) next

            if (environment != "") {
                found = 0
                n = split($11, parts, ";")
                for (i = 1; i <= n; ++i) {
                    split(parts[i], token, ":")
                    if (token[1] == environment) found = 1
                }
                if (!found) next
            }

            print
        }')"

    if [ -n "$last" ]; then
        selected="$(printf '%s\n' "$selected" | grep -v '^$' | tail -n "$last")"
    fi

    case "$format" in
    tsv)
        printf '%s\n' "$COLUMNS"
        printf '%s\n' "$selected" | grep -v '^$' || true
        ;;
    md)
        printf '| # | Date (UTC) | Commit | Compiler | Kernel | ISO | Result | Environments | Note |\n'
        printf '| - | ---------- | ------ | -------- | ------ | --- | ------ | ------------ | ---- |\n'
        printf '%s\n' "$selected" | grep -v '^$' | awk -F'\t' '{
            printf "| %s | %s | `%s`%s | %s %s | %s | %s | %s%s | %s | %s |\n",
                   $1, $2, $3, ($4 == "yes" ? " *(modified)*" : ""),
                   $5, $6, $7, $8, $9,
                   ($10 == "-" ? "" : " (" $10 " assertions)"),
                   $11, $13
        }' || true
        ;;
    table)
        {
            printf 'number|date|commit|dirty|compiler|version|kernel|iso|result|assn|environments\n'
            printf '%s\n' "$selected" | grep -v '^$' \
                | awk -F'\t' '{ printf "%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n",
                                $1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11 }'
        } | column -t -s'|'
        ;;
    *)
        fail "unknown format '$format'; one of table, tsv, md."
        return 2
        ;;
    esac
}

# =============================================================== render

# The generated half of BUILDS.md: an aggregate, and the most recent builds.
#
# It is bounded at VIEW_ROWS deliberately. The record holds every build; the
# document a person opens holds enough of them to see what has been happening
# lately, and stays the same size when there are ten thousand.
generated_view() {
    local total passed failed compilers environments first latest

    total="$(rows | wc -l | tr -d ' ')"
    passed="$(rows | awk -F'\t' '$9 == "passed"' | wc -l | tr -d ' ')"
    failed="$(rows | awk -F'\t' '$9 != "passed"' | wc -l | tr -d ' ')"
    compilers="$(rows | awk -F'\t' '{ print $5 }' | sort -u | paste -sd, - | sed 's/,/, /g')"
    environments="$(rows | awk -F'\t' '{ n = split($11, p, ";");
                     for (i = 1; i <= n; ++i) { split(p[i], t, ":");
                     if (t[1] != "-") print t[1] } }' | sort -u | paste -sd, - | sed 's/,/, /g')"
    first="$(rows | head -1 | awk -F'\t' '{ print $2 }')"
    latest="$(rows | tail -1 | awk -F'\t' '{ print $2 }')"

    printf '%s\n\n' "$BEGIN_MARK"
    printf '*This section is generated from [`builds.tsv`](builds.tsv) by\n'
    printf '[`../../tools/builds.sh`](../../tools/builds.sh). Do not edit it: `make lint`\n'
    printf 'regenerates it and fails if what is here differs.*\n\n'

    printf '## Summary\n\n'
    printf '| | |\n| --- | --- |\n'
    printf '| Builds recorded | %s |\n' "${total:-0}"
    printf '| Verified | %s passed, %s not |\n' "${passed:-0}" "${failed:-0}"
    printf '| Compilers | %s |\n' "${compilers:--}"
    printf '| Environments | %s |\n' "${environments:--}"
    printf '| First | %s |\n' "${first:--}"
    printf '| Latest | %s |\n' "${latest:--}"
    printf '\n'

    if [ "${total:-0}" -gt "$VIEW_ROWS" ]; then
        printf '## The last %d builds\n\n' "$VIEW_ROWS"
        printf 'The remaining %d are in [`builds.tsv`](builds.tsv), which is the record.\n\n' \
               $(( total - VIEW_ROWS ))
    else
        printf '## The builds\n\n'
    fi

    do_query --last "$VIEW_ROWS" --format md

    printf '\n%s\n' "$END_MARK"
}

do_render() {
    local temporary

    if ! grep -qF "$BEGIN_MARK" "$VIEW" || ! grep -qF "$END_MARK" "$VIEW"; then
        fail "$VIEW carries no generated section; the markers are missing."
        return 1
    fi

    temporary="$(mktemp)" || return 1

    awk -v begin="$BEGIN_MARK" 'index($0, begin) { exit } { print }' "$VIEW" > "$temporary"
    generated_view >> "$temporary"
    awk -v end="$END_MARK" 'seen { print } index($0, end) { seen = 1 }' "$VIEW" >> "$temporary"

    cat "$temporary" > "$VIEW"
    rm -f "$temporary"
}

# =============================================================== check

do_check() {
    local errors=0 temporary deep=0
    [ "${1:-}" = "--deep" ] && deep=1

    if [ ! -f "$RECORD" ]; then
        fail "$RECORD does not exist."
        return 1
    fi

    # The header is the schema, and a record whose header has drifted is a
    # record whose columns mean something other than what every reader assumes.
    if ! grep -qxF "$COLUMNS" "$RECORD"; then
        fail "$RECORD: the header line is not the schema this script reads."
        errors=$(( errors + 1 ))
    fi

    # Every row: the right number of fields, a number one greater than the last,
    # and the constrained fields drawn from their vocabularies. Each of these is
    # a way the record can become unreadable by the thing that generates the
    # view, and a view generated from an unreadable record is silently wrong.
    local report
    report="$(rows | awk -F'\t' -v results=" $RESULTS " '
        NF != 13 {
            printf "row %s: %d fields where the schema has 13.\n", $1, NF; bad = 1; next
        }
        {
            if ($1 + 0 != expected && expected != 0) {
                printf "row %s: the numbers are not consecutive; %d was expected.\n",
                       $1, expected
                bad = 1
            }
            expected = $1 + 1

            if ($2 !~ /^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}Z$/) {
                printf "row %s: \"%s\" is not a date of the form 2026-09-11T21:39Z.\n", $1, $2
                bad = 1
            }
            if ($4 != "yes" && $4 != "no") {
                printf "row %s: dirty is \"%s\" and must be yes or no.\n", $1, $4
                bad = 1
            }
            if (index(results, " " $9 " ") == 0) {
                printf "row %s: result \"%s\" is not in the vocabulary.\n", $1, $9
                bad = 1
            }
        }
        BEGIN { expected = 1 }
    ')"

    if [ -n "$report" ]; then
        printf '%s\n' "$report" | while IFS= read -r line; do
            fail "$RECORD: $line"
        done
        errors=$(( errors + 1 ))
    fi

    # And that the view is the record. This is the check the whole arrangement
    # turns upon: a generated document that nothing regenerates is a document
    # that stops being true the first time somebody edits it by hand or appends
    # a row without rendering.
    temporary="$(mktemp)" || return 1
    cp "$VIEW" "$temporary"

    if ! do_render; then
        rm -f "$temporary"
        return 1
    fi

    if ! diff -q "$temporary" "$VIEW" >/dev/null 2>&1; then
        fail "$VIEW's generated section is not what the record renders to. Run: tools/builds.sh render"
        errors=$(( errors + 1 ))
    fi

    rm -f "$temporary"


    # And that every image the record claims to have kept is still there.
    #
    # This is what turns the register from an account of what happened into an
    # account of what still exists. A row whose sha256 is "-" claims nothing and
    # is not checked; a row carrying a hash claims the image was archived, and a
    # claim nothing verifies is how six of the first eight builds came to be
    # unrecoverable without anyone noticing until they were looked for.
    #
    # Presence is checked here and the hash is not, deliberately. Verifying a
    # hash means decompressing every archived image on every `make lint`, which
    # is tenths of a second today and half a minute at five hundred builds — and
    # a check slow enough to be skipped is a check nobody runs, which is the
    # rule the whole of tools/ is built on. `check --deep` verifies the bytes;
    # run it when bit rot rather than an accidental delete is the worry.
    #
    # **The archive belongs to a machine and the record belongs to the
    # project.** The register travels in git to every clone and to the CI
    # runner; the images do not, and are not meant to — being kept out of git is
    # the whole reason they are elsewhere. A machine without the archive is
    # therefore not a machine that has lost it, and a check that could not tell
    # those apart failed every CI run from the day the first image was kept:
    # `build 1 records an archived image, but /home/runner/oxys-builds/… is not
    # there`, upon a runner that had never had it and never could. **Observed
    # 2026-09-21**, by the project owner, in a CI log.
    #
    # The line is the archive root's existence. Where the archive is, every
    # image the record claims must be in it — the accident above. Where it is
    # not, the rows are reported as unchecked rather than as passed, which is
    # the honest thing to say and is visible in the log. Deleting the whole root
    # is then indistinguishable from never having had it; that is true, and it
    # is a deliberate act rather than the accident this guards.
    local archived
    archived="$(rows | awk -F'\t' -v OFS='\t' '$12 != "-" { print $1, $2, $3, $4, $5, $6 }')"

    if [ -n "$archived" ] && [ ! -d "$ARCHIVE_ROOT" ]; then
        note "$ARCHIVE_ROOT is not upon this machine, so $(printf '%s\n' "$archived" | wc -l) row(s) claiming a kept image are not checked here."
        note 'The archive is a machine'"'"'s and the record is the project'"'"'s; OXYS_BUILD_ARCHIVE names the archive.'
    elif [ -n "$archived" ]; then
        while IFS=$'\t' read -r number _ commit _ compiler _; do
            [ -n "$number" ] || continue
            local kept
            kept="$(archive_path "$number" "$commit" "$compiler")"

            if [ ! -f "$kept" ]; then
                fail "build $number records an archived image, but $kept is not there."
                errors=$(( errors + 1 ))
                continue
            fi

            if [ "$deep" -eq 1 ]; then
                local recorded actual
                recorded="$(rows | awk -F'\t' -v n="$number" '$1 == n { print $12 }')"
                actual="$(xz -dc "$kept" 2>/dev/null | sha256sum | cut -d' ' -f1)"
                if [ "$recorded" != "$actual" ]; then
                    fail "build $number: $kept hashes to $actual, and the record says $recorded."
                    errors=$(( errors + 1 ))
                fi
            fi
        done < <(printf '%s\n' "$archived")
    fi
    [ "$errors" -eq 0 ]
}

# =============================================================== sql

do_sql() {
    local statement="${1:-}" database

    if ! command -v sqlite3 >/dev/null; then
        fail 'sqlite3 is not upon the PATH, and this subcommand is the only thing here that uses it.'
        printf 'The record is %s and is tab-separated; `tools/builds.sh query` reads it\n' "$RECORD" >&2
        printf 'without sqlite3, and awk -F"\\t" reads it without this script.\n' >&2
        return 2
    fi

    if [ -z "$statement" ]; then
        fail 'sql needs a statement.'
        return 2
    fi

    # Materialised and thrown away. Nothing binary is ever written into the
    # repository; see the note at the head of this file.
    database="$(mktemp)" || return 1
    rm -f "$database"

    # The schema is stated here and nowhere else, because the record's header is
    # the schema and this is a transcription of it for one query's lifetime.
    rows | sqlite3 "$database" \
        'CREATE TABLE builds (number INTEGER, date TEXT, "commit" TEXT, dirty TEXT,
           compiler TEXT, cc_version TEXT, kernel INTEGER, iso INTEGER,
           result TEXT, assertions TEXT, environments TEXT, sha256 TEXT, note TEXT);
         .mode tabs
         .import /dev/stdin builds' 2>/dev/null

    sqlite3 -header -column "$database" "$statement"
    local status=$?

    rm -f "$database"
    return $status
}

# =============================================================== the entry

case "${1:-}" in
record) shift; do_record "$@" ;;
query)  shift; do_query "$@" ;;
render) shift; do_render && printf 'Rendered %s from %s.\n' "$VIEW" "$RECORD" ;;
archive) shift; do_archive "$@" ;;
check)  shift; do_check "$@" ;;
sql)    shift; do_sql "$@" ;;
''|-h|--help|help) usage ;;
*)      fail "unknown subcommand '$1'"; usage >&2; exit 2 ;;
esac
