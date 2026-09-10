#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
# SPDX-License-Identifier: CC0-1.0
# ==============================================================================
# File: tools/check-docs.sh
#
# Purpose:
#   Checks the claims the `docs/` corpus makes about itself and about the source
#   — the ones that are mechanically checkable and were, until this script,
#   checked by hand or not at all.
#
    # The trailing class is a number boundary: without it "sub-task 6.1"
    # matches "sub-task 6.15", and every early sub-task reports every later
    # one's sentences as its own.
# Usage:
#   tools/check-docs.sh
#
#   Exit status is 0 when every check passes and 1 when any fails, so that
#   `make docs-check` fails a build the way a compiler diagnostic does.
#
# Why this exists:
#   PROJECT_GUIDELINES.md, Section 2, requires every code change to be reflected
#   in the documents affected by it. That rule is enforced by whoever remembers
#   it, and this session alone produced three failures of exactly that kind: a
#   design document referenced by three source files that did not exist; a list
#   of unsynchronised structures that named five files whose headers said nothing
#   of the sort; and a count of self-test assertions that had been wrong through
#   two sub-tasks.
#
#   Every check below is one that would have caught a real defect in this
#   repository's history. None of them is a style rule.
#
# The distinction between an error and an advisory:
#   An **error** is a statement that is provably false — a link to a file that is
#   not there, a count that does not match what was counted. It fails the run.
#
#   An **advisory** is a statement that is *probably* stale — a document
#   describing in the future tense a sub-task that PLAN.md marks Implemented.
#   Some of those are correctly phrased, so an advisory is printed and does not
#   fail the run. A checker that cries wolf is a checker nobody reads.
# ==============================================================================

set -o nounset
set -o pipefail

errors=0
advisories=0

fail() {
    printf 'ERROR    %s\n' "$1"
    errors=$((errors + 1))
}

warn() {
    printf 'ADVISORY %s\n' "$1"
    advisories=$((advisories + 1))
}

section() {
    printf '\n== %s\n' "$1"
}

cd "$(git -C "$(dirname "$0")" rev-parse --show-toplevel)" || exit 2

# ---------------------------------------------------------------------------
# 1. Every relative link resolves.
#
# The defect this catches: docs/design/SMP.md was referenced by three source
# files and by two indexes for the whole of sub-task 6.14 before it existed.
# ---------------------------------------------------------------------------
section 'Links'

while IFS= read -r file; do
    directory="$(dirname "$file")"

    grep -o '](\([^)]*\.\(md\|c\|h\|asm\|ld\|txt\|sh\|cfg\|yml\)\)[^)]*)' "$file" 2>/dev/null \
        | sed 's/](//; s/[)#].*//' | sort -u | while IFS= read -r target; do
            case "$target" in http*|'') continue ;; esac
            [ -e "$directory/$target" ] || printf 'BROKEN\t%s\t%s\n' "$file" "$target"
        done
done < <(git ls-files '*.md') > /tmp/oxys-links.$$ 2>/dev/null

while IFS=$'\t' read -r _ file target; do
    [ -n "${file:-}" ] && fail "$file links to $target, which does not exist."
done < /tmp/oxys-links.$$
rm -f /tmp/oxys-links.$$

# ---------------------------------------------------------------------------
# 2. Every design document is indexed, and the count that describes them is
#    the number of them.
#
# The defect this catches: a document added without an index entry is a document
# nobody finds, and the two indexes drift apart silently.
# ---------------------------------------------------------------------------
section 'Indexes'

design_count=0
for path in docs/design/*.md; do
    name="$(basename "$path")"
    [ "$name" = 'README.md' ] && continue
    design_count=$((design_count + 1))

    grep -q "\[\`$name\`\]" docs/design/README.md \
        || fail "docs/design/$name is not indexed in docs/design/README.md."
    grep -q "design/$name" docs/README.md \
        || fail "docs/design/$name is not indexed in docs/README.md."
done

# The prose says "These <word> documents". The word must be the count.
number_word() {
    local n="$1"
    local ones=(zero one two three four five six seven eight nine ten eleven \
                twelve thirteen fourteen fifteen sixteen seventeen eighteen \
                nineteen)
    local tens=('' '' twenty thirty forty fifty sixty seventy eighty ninety)

    if [ "$n" -lt 20 ]; then
        echo "${ones[$n]}"
    elif [ $((n % 10)) -eq 0 ]; then
        echo "${tens[$((n / 10))]}"
    else
        echo "${tens[$((n / 10))]}-${ones[$((n % 10))]}"
    fi
}

expected_word="$(number_word "$design_count")"
if ! grep -qi "These $expected_word" docs/design/README.md; then
    actual="$(grep -o 'These [a-z-]*' docs/design/README.md | head -1)"
    fail "docs/design/README.md says '$actual' but indexes $design_count documents (expected 'These $expected_word')."
fi

# ---------------------------------------------------------------------------
# 3. A source file's header names its own path.
#
# The defect this catches: a file copied to start another and left carrying the
# original's path, which sends every reader of the header to the wrong file.
# PROJECT_GUIDELINES.md, Section 4, requires the header to carry "the file name
# and path".
# ---------------------------------------------------------------------------
section 'File headers'

while IFS= read -r file; do
    case "$file" in LICENSES/*) continue ;; esac

    declared="$(head -n 12 "$file" | grep -m1 -o 'File: [^ ]*' | sed 's/File: //')"

    if [ -z "$declared" ]; then
        fail "$file has no 'File:' line in its header (PROJECT_GUIDELINES.md, Section 4)."
    elif [ "$declared" != "$file" ]; then
        fail "$file declares itself as '$declared'."
    fi
done < <(git ls-files '*.c' '*.h' '*.asm')

# ---------------------------------------------------------------------------
# 4. Every directory holding source carries a README.
#
# PROJECT_GUIDELINES.md, Section 10, requires one and requires it to be updated
# in the same change that alters what it describes. This checks only that it
# exists; no script can check that it is true.
# ---------------------------------------------------------------------------
section 'Directory READMEs'

for directory in boot kernel drivers graphics libc net crypto uefi userland tools; do
    [ -d "$directory" ] || continue
    if [ -n "$(git ls-files "$directory" | head -1)" ] && [ ! -f "$directory/README.md" ]; then
        fail "$directory/ holds tracked material but has no README.md (PROJECT_GUIDELINES.md, Section 10)."
    fi
done

# ---------------------------------------------------------------------------
# 5. The unsynchronised-structure list is true.
#
# CONCURRENCY.md, Section 10, limitation 1, names each file that still holds an
# unsynchronised structure, and states that every file named carries a
# `Concurrency.` paragraph in its header. That claim was false for five files
# when it was first made, and the audit that found it was done by hand.
# ---------------------------------------------------------------------------
section 'Concurrency notes'

concurrency_list="$(sed -n '/\*\*Everything else is still unsynchronised\*\*/,/^$/p' \
                    docs/design/CONCURRENCY.md | grep -o '`[a-z0-9_/]*\.c`' | tr -d '`' | sort -u)"

if [ -z "$concurrency_list" ]; then
    fail "CONCURRENCY.md, Section 10, limitation 1: could not find the list of unsynchronised files."
else
    while IFS= read -r file; do
        [ -n "$file" ] || continue
        if [ ! -f "$file" ]; then
            fail "CONCURRENCY.md names $file, which does not exist."
        elif ! grep -q 'Concurrency\.' "$file"; then
            fail "CONCURRENCY.md names $file as unsynchronised, but its header has no 'Concurrency.' paragraph."
        fi
    done <<< "$concurrency_list"
fi

# ---------------------------------------------------------------------------
# 6. The count of self-test assertions is the number the last run produced.
#
# The defect this catches: STATUS.md said thirty-nine for two sub-tasks after
# the figure had become forty-seven. It runs only where a captured log exists,
# because the log is the evidence and an absent one is not a failure.
# ---------------------------------------------------------------------------
section 'Assertion count'

if [ -f build/serial.log ]; then
    counted=$(( $(grep -o 'passed' build/serial.log | wc -l) \
              + $(grep -o 'sound' build/serial.log | wc -l) ))
    counted_word="$(number_word "$counted")"

    if grep -qiE '[A-Za-z-]+ assertions presently report' docs/project/STATUS.md; then
        claimed="$(grep -oiE '[A-Za-z-]+ assertions presently report' docs/project/STATUS.md \
                   | head -1 | awk '{print tolower($1)}')"
        if [ "$claimed" != "$counted_word" ]; then
            fail "STATUS.md claims '$claimed' assertions; build/serial.log holds $counted ('$counted_word')."
        fi
    fi
else
    printf 'SKIPPED  no build/serial.log; run `make verify` for the assertion count check.\n'
fi


# ---------------------------------------------------------------------------
# 7. The Makefile's targets and the list in the guidelines are the same set.
#
# The defect this catches: PROJECT_GUIDELINES.md, Section 3, enumerates the
# build targets. `clang-check` was added and the list was not, so the document
# had been wrong about the build for several phases before anybody noticed — and
# it was noticed by reading, not by running anything.
#
# The check runs both ways. A target present in the Makefile and absent from the
# list is a build the guidelines do not describe; a target named in the list and
# absent from the Makefile is an instruction to run something that is not there,
# which is the worse of the two.
# ---------------------------------------------------------------------------
section 'Build targets'

makefile_targets="$(sed -n '/^\.PHONY:/,/[^\\]$/p' Makefile \
                    | sed 's/^\.PHONY://; s/\\$//' | tr ' ' '\n' \
                    | grep -E '^[a-z][a-z0-9-]*$' | sort -u)"

guideline_targets="$(grep '\*\*Build System\*\*' PROJECT_GUIDELINES.md \
                     | grep -o '`[a-z][a-z0-9-]*`' | tr -d '`' | sort -u)"

if [ -z "$makefile_targets" ] || [ -z "$guideline_targets" ]; then
    fail "Could not read the build targets from the Makefile or from PROJECT_GUIDELINES.md, Section 3."
else
    while IFS= read -r target; do
        [ -n "$target" ] || continue
        grep -qx "$target" <<< "$guideline_targets" \
            || fail "The Makefile has target '$target', which PROJECT_GUIDELINES.md, Section 3, does not name."
    done <<< "$makefile_targets"

    while IFS= read -r target; do
        [ -n "$target" ] || continue
        grep -qx "$target" <<< "$makefile_targets" \
            || fail "PROJECT_GUIDELINES.md, Section 3, names target '$target', which the Makefile does not have."
    done <<< "$guideline_targets"
fi
# ---------------------------------------------------------------------------
# 8. Advisory: a sub-task that PLAN.md marks Implemented, still written about in
#    the future tense.
#
# The defect this catches is the one that recurred most in this project's
# history: sub-task 6.14 landed, and thirty documents went on saying "sub-task
# 6.14 is what makes this contended". Some phrasings are legitimate, so these
# are advisories and a human decides.
# ---------------------------------------------------------------------------
section 'Stale forward references (advisory)'

implemented="$(grep -oE '^\| [0-9]+\.[0-9]+ \|.*\| Implemented \|' docs/project/PLAN.md \
               | grep -oE '^\| [0-9]+\.[0-9]+' | tr -d '| ' | sort -u)"

while IFS= read -r subtask; do
    [ -n "$subtask" ] || continue

    # Two patterns, and the choice between them was made by measurement.
    #
    # "sub-task N will" and "sub-task N is what makes" are claims about a future
    # that has since arrived, and every one this check found was genuinely stale.
    #
    # "until sub-task N" was tried and dropped. It reads as future tense but is
    # almost always narration of a transition that has happened — "there was one
    # stack until sub-task 6.9" is correct English about the past — and it
    # produced thirty-six hits of which none was a defect. A check with that
    # ratio teaches its reader to skip the section.
    #
    # The trailing character class is a number boundary. Without it "sub-task
    # 6.1" matches "sub-task 6.15", and every early sub-task reports every later
    # one's sentences as its own.
    hits="$(grep -rn -E "sub-task $subtask[^0-9.] *(will |is what (makes|gives|could))" \
            --include='*.md' --include='*.c' --include='*.h' --include='*.asm' . 2>/dev/null \
            | grep -v '^\./build/' | grep -v 'HISTORY\.md' | grep -v 'TESTING-RECORD\.md')"

    if [ -n "$hits" ]; then
        while IFS= read -r hit; do
            warn "sub-task $subtask is Implemented, but: ${hit%%:*}:$(echo "$hit" | cut -d: -f2)"
        done <<< "$hits"
    fi
done <<< "$implemented"

# ---------------------------------------------------------------------------

printf '\n== Summary\n'
printf 'docs-check: %d error(s), %d advisory/advisories.\n' "$errors" "$advisories"

[ "$errors" -gt 0 ] && exit 1
exit 0
