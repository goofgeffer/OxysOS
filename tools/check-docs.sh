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
# 2. Every document is indexed in the README.md of its group.
#
# The defect this catches: a document added without an index entry is a document
# nobody finds. docs/README.md indexes the groups; each group's README.md
# indexes its documents, so a document is listed in exactly one index.
# ---------------------------------------------------------------------------
section 'Indexes'

for group in docs/project docs/design docs/devices docs/storage; do
    for path in "$group"/*.md; do
        name="$(basename "$path")"
        [ "$name" = 'README.md' ] && continue
        grep -q "]($name)" "$group/README.md" \
            || fail "$path is not indexed in $group/README.md."
    done
    grep -q "](${group#docs/}/README.md)" docs/README.md \
        || fail "$group/README.md is not indexed in docs/README.md."
done

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
# CONCURRENCY.md names each file that still holds an
# unsynchronised structure, and states that every file named carries a
# `Concurrency.` paragraph in its header. That claim was false for five files
# when it was first made, and the audit that found it was done by hand.
# ---------------------------------------------------------------------------
section 'Concurrency notes'

concurrency_list="$(sed -n '/^## Unsynchronised structures/,/^## Verification/p' \
                    docs/design/CONCURRENCY.md | grep '^| `' \
                    | grep -o '`[a-z0-9_/]*\.c`' | tr -d '`' | sort -u)"

if [ -z "$concurrency_list" ]; then
    fail "CONCURRENCY.md: could not find the list of unsynchronised files."
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
    claimed="$(grep -oE '^\| Self-test assertions \| [0-9]+ \|' docs/project/STATUS.md \
               | grep -oE '[0-9]+' | head -1)"

    if [ -z "$claimed" ]; then
        fail "STATUS.md has no '| Self-test assertions | <n> |' row."
    elif [ "$claimed" != "$counted" ]; then
        fail "STATUS.md claims $claimed assertions; build/serial.log holds $counted."
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
# 8. The architecture boundary is crossed only where it is recorded.
#
# `kernel/arch/x86_64/` holds what could not survive a change of processor, and
# `kernel/arch/README.md` claims the rest of `kernel/` is the part that could.
# Nothing enforced that claim when the directory was created, and it was already
# untrue on the day it was written: five files in the portable core included
# `<oxys/arch/...>` headers.
#
# The defect this catches is a boundary that exists only in prose. A directory
# division nothing checks is a claim that decays from the moment it is made, and
# this project has the evidence: `drivers/` held the block layer for three
# phases while its own README said it held device drivers, and that was found by
# a person reading, which is the thing this script exists to stop relying upon.
#
# It is a **ratchet and not a prohibition**. The five crossings that exist are
# listed below with the reason each is there, so the list is the debt written
# down; the check fails when a file not on it crosses the boundary, and equally
# when a file on it stops crossing and the entry is not removed. Both directions
# matter, for the same reason the build-target check above runs both ways: a
# stale allowlist entry is an exemption nobody needs, which is how an allowlist
# becomes a place to hide things.
#
# The scope is the portable core alone. `kernel/kernel.c` and `kernel/init/` are
# exempt because they initialise every subsystem in dependency order and must
# therefore name every subsystem; `kernel/test/` is exempt because a test of an
# architecture subsystem is an architecture test; `drivers/` and `graphics/` are
# exempt because neither claims to be portable.
# ---------------------------------------------------------------------------
section 'Architecture boundary'

# path<TAB>header<TAB>why it is permitted
architecture_crossings=$(cat <<'CROSSINGS'
kernel/acpi/acpi.c	arch/mm/paging.h	Maps each firmware table for the duration of its parse and unmaps it afterwards.
kernel/exec/elf.c	arch/mm/addrspace.h	Places an image's segments into an address space.
kernel/exec/elf.c	arch/mm/paging.h	Reaches a target space's pages through the direct physical map.
kernel/exec/elf.c	arch/syscall/syscall.h	Validates a caller-supplied path against the user limit.
kernel/fs/vfs/pipe.c	arch/cpu/percpu.h	The masked section a reader or writer tests its condition and sleeps within, as the wait channel requires.
kernel/mm/vmm.c	arch/mm/paging.h	Asks whether a range it is about to hand out is already mapped.
kernel/mm/table.c	arch/cpu/percpu.h	Refuses growth off the bootstrap processor, the heap being unsynchronised.
kernel/proc/process.c	arch/cpu/gdt.h	The selectors a thread descends to privilege level 3 with.
kernel/proc/process.c	arch/cpu/percpu.h	Records the current thread in the executing processor's area.
kernel/proc/process.c	arch/cpu/spinlock.h	Guards the process and thread tables.
kernel/proc/process.c	arch/cpu/tss.h	Writes rsp0 when a thread becomes current.
kernel/proc/process.c	arch/mm/addrspace.h	Gives a process an address space of its own, and clones one on fork.
kernel/proc/process.c	arch/mm/paging.h	Maps a thread's kernel stack and the guard page beneath it.
kernel/proc/sched.c	arch/cpu/percpu.h	The run queue each processor holds is in its own area.
kernel/proc/signal.c	arch/cpu/percpu.h	The masked section a pending set is read and written within, against the tick handler that sends from it.
kernel/proc/sched.c	arch/cpu/spinlock.h	Guards each run queue.
kernel/proc/sched.c	arch/interrupt/interrupts.h	Registers the local timer handler that ends a quantum.
CROSSINGS
)

architecture_scope() {
    git ls-files 'kernel/mm/*' 'kernel/proc/*' 'kernel/fs/*' 'kernel/block/*' \
                 'kernel/exec/*' 'kernel/acpi/*' 'kernel/handoff/*' 2>/dev/null \
        | grep -E '\.(c|h)$'
}

# Observed: what the source actually does today.
observed_crossings="$(
    while IFS= read -r file; do
        [ -f "$file" ] || continue
        grep -oE '<oxys/arch/[a-z0-9_/]+\.h>' "$file" 2>/dev/null \
            | sed 's|<oxys/||; s|>||' | sort -u \
            | while IFS= read -r header; do printf '%s\t%s\n' "$file" "$header"; done
    done < <(architecture_scope) | sort -u
)"

# Permitted: what the list above records, stripped of its reasons.
permitted_crossings="$(printf '%s\n' "$architecture_crossings" \
    | awk -F'\t' 'NF>=2 {printf "%s\t%s\n", $1, $2}' | sort -u)"

while IFS= read -r crossing; do
    [ -n "$crossing" ] || continue
    grep -qxF "$crossing" <<< "$permitted_crossings" || fail \
        "$(cut -f1 <<< "$crossing") includes <oxys/$(cut -f2 <<< "$crossing")>, crossing the architecture boundary. Either it does not belong in the portable core, or the crossing belongs in the list in tools/check-docs.sh, Section 8, with its reason."
done <<< "$observed_crossings"

while IFS= read -r crossing; do
    [ -n "$crossing" ] || continue
    grep -qxF "$crossing" <<< "$observed_crossings" || fail \
        "tools/check-docs.sh, Section 8, permits $(cut -f1 <<< "$crossing") to include <oxys/$(cut -f2 <<< "$crossing")>, which it no longer does. Remove the entry: an exemption nobody needs is where things hide."
done <<< "$permitted_crossings"

printf 'CHECKED  %d recorded crossing(s) of the architecture boundary.\n' \
    "$(grep -c . <<< "$permitted_crossings")"

# ---------------------------------------------------------------------------
# 9. Advisory: a sub-task that PLAN.md marks Implemented, still written about in
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
# 10. The build register's record and the view generated from it agree.
#
# docs/project/builds.tsv is the record and docs/project/BUILDS.md holds a view
# rendered from it. tools/builds.sh check validates the schema, the
# consecutiveness of the numbers, the constrained vocabularies, and that the
# rendered section is what the record renders to.
#
# The defect this catches: a generated document that nothing regenerates. It was
# a hand-appended Markdown table for three builds, and there was nothing to stop
# a row being spelt differently from the one above it, a number being reused, or
# the table and the summary above it disagreeing — each of which reads as a fact
# and is not one. It is invoked here rather than being a `make` target of its own
# so that `make lint` covers it and PROJECT_GUIDELINES.md, Section 3, needs no
# further amendment.
# ---------------------------------------------------------------------------
section 'Build register'

if [ -x tools/builds.sh ]; then
    if ! tools/builds.sh check; then
        fail 'The build register does not validate; see the report above.'
    fi
else
    fail 'tools/builds.sh is missing or not executable.'
fi

# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 11. A reference to a numbered section names a section that exists.
#
# The defect this catches: a document rewritten and renumbered, leaving the
# citations of its old sections pointing at text that is not there, or worse,
# at different text under the same number. Only a document whose name is unique
# in the repository can be resolved, and the records of the past (HISTORY.md,
# TESTING-RECORD.md, the release notes) cite the documents as they then were.
# ---------------------------------------------------------------------------
section 'Section references'

git grep -hoE '[A-Za-z0-9_-]+\.md`?(\]\([^)]*\))?,? Sections? [0-9]+[A-Z]?(\.[0-9]+)*' \
        -- ':!docs/project/HISTORY.md' ':!docs/project/TESTING-RECORD.md' \
           ':!docs/project/RELEASE-*.md' \
    | sed -E 's/^([A-Za-z0-9_-]+\.md).* Sections? /\1 /' | sort -u \
    | while read -r name number; do
        matches="$(git ls-files "*/$name" "$name")"
        [ "$(printf '%s\n' "$matches" | grep -c .)" -eq 1 ] || continue
        escaped="$(printf '%s' "$number" | sed 's/\./\\./g')"
        grep -qE "^#+ $escaped(\.| )" "$matches" \
            || printf '%s\t%s\n' "$matches" "$number"
    done > /tmp/oxys-sections.$$

while IFS=$'\t' read -r file number; do
    [ -n "${file:-}" ] && fail "A reference cites $file, Section $number, which has no such heading."
done < /tmp/oxys-sections.$$
rm -f /tmp/oxys-sections.$$


printf '\n== Summary\n'
printf 'docs-check: %d error(s), %d advisory/advisories.\n' "$errors" "$advisories"

[ "$errors" -gt 0 ] && exit 1
exit 0
