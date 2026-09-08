#!/usr/bin/env bash
# ==============================================================================
# File: build_all.sh
#
# Purpose:
#   Takes a machine with nothing upon it to a verified kernel, in the order the
#   steps depend upon one another: host packages, then the cross-toolchain, then
#   the ISO, then the verification of docs/project/TESTING.md.
#
#   It builds nothing itself. Every step is delegated to the script or the make
#   target that owns it, so that this file cannot drift from them: what it adds
#   is the order and the reporting, which are the two things a newcomer to the
#   repository does not have.
#
# Principal routines:
#   Usage      - States the invocation and the environment it reads.
#   Announce   - Prints a step banner, so that a failure names its own stage.
#   Step*      - One routine per stage, each delegating to its owner.
#
# Environment:
#   SKIP_DEPS=1        Do not install host packages.
#   SKIP_TOOLCHAIN=1   Do not build the cross-toolchain.
#   SKIP_VERIFY=1      Build the ISO but do not run it under QEMU.
#   PREFIX=<dir>       Passed to build_toolchain.sh. Default ~/opt/cross.
#
# References:
#   - docs/project/TOOLCHAIN.md, Sections 1, 6 and 8.
#   - docs/project/TESTING.md: the procedure `make verify` performs.
#   - PROJECT_GUIDELINES.md, Section 2: a change is not final until it has been
#     verified, which is why the last step is the verification and not the build.
# ==============================================================================

set -euo pipefail

REPOSITORY_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${PREFIX:-$HOME/opt/cross}"

Usage() {
    cat <<'USAGE'
Usage: ./build_all.sh

Installs the host packages, builds the cross-toolchain if it is absent, builds
the ISO, and runs the verification under QEMU.

Environment:
  SKIP_DEPS=1        Do not install host packages.
  SKIP_TOOLCHAIN=1   Do not build the cross-toolchain.
  SKIP_VERIFY=1      Build the ISO but do not run it under QEMU.
  PREFIX=<dir>       Where the toolchain is installed. Default ~/opt/cross.

The first run takes twenty to forty minutes, nearly all of it the compiler.
USAGE
}

Announce() {
    echo
    echo "=============================================================================="
    echo "  $1"
    echo "=============================================================================="
}

StepDependencies() {
    if [ "${SKIP_DEPS:-0}" = "1" ]; then
        echo "SKIP_DEPS=1, host packages not installed."
        return 0
    fi
    Announce "1 of 4: host packages"
    "$REPOSITORY_ROOT/build_deps.sh"
}

StepToolchain() {
    if [ "${SKIP_TOOLCHAIN:-0}" = "1" ]; then
        echo "SKIP_TOOLCHAIN=1, the cross-toolchain is not built."
        return 0
    fi
    Announce "2 of 4: the x86_64-elf cross-toolchain"
    PREFIX="$PREFIX" "$REPOSITORY_ROOT/build_toolchain.sh"
}

StepBuild() {
    Announce "3 of 4: the kernel and the ISO"
    # The toolchain is not upon the default PATH, as TOOLCHAIN.md Section 8
    # records. It is placed there for this process alone rather than for the
    # invoking shell, a script that edited a profile being a script that changes
    # a machine in a way nothing here records.
    export PATH="$PREFIX/bin:$PATH"
    make -C "$REPOSITORY_ROOT" toolcheck
    make -C "$REPOSITORY_ROOT" iso
}

StepVerify() {
    if [ "${SKIP_VERIFY:-0}" = "1" ]; then
        echo "SKIP_VERIFY=1, the ISO was built but not executed."
        return 0
    fi
    Announce "4 of 4: verification under QEMU"
    export PATH="$PREFIX/bin:$PATH"
    make -C "$REPOSITORY_ROOT" verify
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    Usage
    exit 0
fi

StepDependencies
StepToolchain
StepBuild
StepVerify

echo
echo "BUILD COMPLETE. Add the toolchain to the PATH of your own shell to use the"
echo "make targets directly:"
echo "  export PATH=\"$PREFIX/bin:\$PATH\""
