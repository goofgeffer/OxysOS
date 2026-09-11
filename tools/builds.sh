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
#   tools/builds.sh check                        validate the record and the view
#   tools/builds.sh sql <statement>              SQL over the record, if sqlite3 is here
#
#   `make build-record NOTE="…"` invokes the first of those.
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
COLUMNS='number	date	commit	dirty	compiler	cc_version	kernel	iso	result	assertions	environments	note'
RESULTS='passed failed did-not-boot not-run other'

fail() {
    printf 'ERROR    %s\n' "$1" >&2
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
    local environments='' result='' assertions='' note=''
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

    row="$number	$(date -u '+%Y-%m-%dT%H:%MZ')	$commit	$dirty	$compiler	$cc_version	$kernel	$iso	$result	$assertions	$(sanitise "$environments")	$(sanitise "$note")"

    printf '%s\n' "$row" >> "$RECORD"

    do_render || return 1

    printf 'Recorded build %d.\n' "$number"
    printf '%s\n' "$row" | awk -F'\t' '{ for (i = 1; i <= NF; ++i) printf "  %s\n", $i }'
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
                   $11, $12
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
    local errors=0 temporary

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
        NF != 12 {
            printf "row %s: %d fields where the schema has 12.\n", $1, NF; bad = 1; next
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
           result TEXT, assertions TEXT, environments TEXT, note TEXT);
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
check)  shift; do_check ;;
sql)    shift; do_sql "$@" ;;
''|-h|--help|help) usage ;;
*)      fail "unknown subcommand '$1'"; usage >&2; exit 2 ;;
esac
