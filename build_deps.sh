#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
# SPDX-License-Identifier: CC0-1.0
# ==============================================================================
# File: build_deps.sh
#
# Purpose:
#   Installs the host packages the build requires, so that the list exists in
#   one executable place rather than in the prose of docs/project/TOOLCHAIN.md,
#   in the workflow of .github/workflows/ci.yml, and in the memory of whoever
#   configured the machine this project is presently developed upon. Three
#   copies of a list do not agree with each other for long.
#
#   It does not build the cross-compiler. No package index carries
#   x86_64-elf-gcc; build_toolchain.sh builds it from source.
#
# Principal routines:
#   Usage           - States the invocation and the environment it reads.
#   RequireApt      - Establishes that this is a distribution this script can
#                     serve, and names the packages plainly if it is not.
#   InstallPackages - Installs the two lists below.
#
# Environment:
#   DRY_RUN=1       Report what would be installed and install nothing.
#
# References:
#   - docs/project/TOOLCHAIN.md: the tools the build requires, and
#     Section 10.1, which is why the compiler is not among these packages.
#   - apt-get(8).
# ==============================================================================

set -euo pipefail

# The tools the Makefile invokes. `grub-common` carries grub-file, which the
# `all` target uses to assert that the image is Multiboot2 compliant; the ISO is
# written by xorriso, which grub-mkrescue drives, and mtools, which it requires
# for the El Torito image it embeds.
BUILD_PACKAGES=(
    make
    nasm
    grub-common
    grub-pc-bin
    xorriso
    mtools
    qemu-system-x86
    curl
)

# What building binutils and GCC from source requires. These are needed by
# build_toolchain.sh alone; a machine that already has the cross-compiler does
# not need them again.
TOOLCHAIN_PACKAGES=(
    build-essential
    bison
    flex
    texinfo
    libgmp-dev
    libmpc-dev
    libmpfr-dev
    libisl-dev
)

# Optional. The second compiler of TOOLCHAIN.md builds nothing and is
# required by no target but clang-check, so its absence is reported rather than
# treated as a fault.
OPTIONAL_PACKAGES=(
    clang
)

Usage() {
    cat <<'USAGE'
Usage: ./build_deps.sh

Installs the host packages required to build the kernel, produce the ISO and
run the verification, together with those required to build the cross-compiler
from source.

Environment:
  DRY_RUN=1   Report what would be installed and install nothing.

The cross-compiler itself is not a package. Run ./build_toolchain.sh for it.
USAGE
}

RequireApt() {
    if command -v apt-get >/dev/null 2>&1; then
        return 0
    fi

    echo "This script installs packages with apt-get, which was not found." >&2
    echo "The following are required; install their equivalents by hand:" >&2
    printf '  %s\n' "${BUILD_PACKAGES[@]}" "${TOOLCHAIN_PACKAGES[@]}" >&2
    printf '  %s (optional)\n' "${OPTIONAL_PACKAGES[@]}" >&2
    return 1
}

InstallPackages() {
    local packages=("$@")
    local sudo_command=()

    if [ "${EUID:-$(id -u)}" -ne 0 ]; then
        sudo_command=(sudo)
    fi

    if [ "${DRY_RUN:-0}" = "1" ]; then
        echo "DRY_RUN: would install:"
        printf '  %s\n' "${packages[@]}"
        return 0
    fi

    "${sudo_command[@]}" apt-get update
    "${sudo_command[@]}" apt-get install --no-install-recommends -y "${packages[@]}"
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    Usage
    exit 0
fi

RequireApt

echo "Installing the build and verification tools, and the prerequisites of a"
echo "compiler build. The cross-compiler is not among them; see build_toolchain.sh."
InstallPackages "${BUILD_PACKAGES[@]}" "${TOOLCHAIN_PACKAGES[@]}"

echo
echo "Installing the optional second compiler."
InstallPackages "${OPTIONAL_PACKAGES[@]}" || {
    echo "The optional package could not be installed. Nothing but the"
    echo "clang-check target requires it, and the build is unaffected."
}

echo
echo "DEPENDENCIES INSTALLED. Run ./build_toolchain.sh next, or 'make toolcheck'"
echo "to see what is now present."
