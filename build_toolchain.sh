#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 The Oxys-OS Authors
# SPDX-License-Identifier: CC0-1.0
# ==============================================================================
# File: build_toolchain.sh
#
# Purpose:
#   Builds the x86_64-elf cross-toolchain that docs/project/TOOLCHAIN.md,
#   Section 1, requires: GNU binutils and GCC, configured for a target that
#   presumes nothing beyond the freestanding environment of ISO/IEC 9899:2011,
#   Section 4, paragraph 6.
#
#   No package index carries x86_64-elf-gcc, so it is built from source. The
#   versions are pinned rather than left to float, for the reason TOOLCHAIN.md
#   Section 10.1 gives: a compiler that differs from the one another contributor
#   runs reports the difference between two compilers as a defect in the kernel.
#
# Principal routines:
#   Usage            - States the invocation and the environment it reads.
#   AlreadyPresent   - Reports whether the toolchain stands at PREFIX already.
#   FetchAndUnpack   - Retrieves and unpacks one source archive.
#   BuildBinutils    - Configures, builds and installs the assembler and linker.
#   BuildGcc         - Configures, builds and installs the compiler and libgcc.
#
# Environment:
#   PREFIX             Installation directory. Default ~/opt/cross.
#   BINUTILS_VERSION   Default 2.42.
#   GCC_VERSION        Default 13.2.0.
#   SRC_DIR            Working directory for sources. Default ~/src/oxys-toolchain.
#   JOBS               Parallel make jobs. Default: the processor count.
#   FORCE=1            Rebuild even if the toolchain is already present.
#
# References:
#   - docs/project/TOOLCHAIN.md, Sections 1, 2 and 10.1.
#   - GNU GCC Installation Instructions: --without-headers is required when no
#     C library exists for the target, and all-target-libgcc is built separately
#     from all-gcc for the same reason.
# ==============================================================================

set -euo pipefail

PREFIX="${PREFIX:-$HOME/opt/cross}"
BINUTILS_VERSION="${BINUTILS_VERSION:-2.42}"
GCC_VERSION="${GCC_VERSION:-13.2.0}"
SRC_DIR="${SRC_DIR:-$HOME/src/oxys-toolchain}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
TARGET="x86_64-elf"

Usage() {
    cat <<'USAGE'
Usage: ./build_toolchain.sh

Builds GNU binutils and GCC for the x86_64-elf target and installs them into
PREFIX. Exits at once, reporting the versions found, if they are already there.

Environment:
  PREFIX=<dir>            Installation directory. Default ~/opt/cross.
  BINUTILS_VERSION=<v>    Default 2.42.
  GCC_VERSION=<v>         Default 13.2.0.
  SRC_DIR=<dir>           Where sources are unpacked. Default ~/src/oxys-toolchain.
  JOBS=<n>                Parallel make jobs. Default: the processor count.
  FORCE=1                 Rebuild even if the toolchain is already present.

This takes twenty to forty minutes on a machine that has not built it before.
The build prerequisites are installed by ./build_deps.sh.
USAGE
}

AlreadyPresent() {
    [ -x "$PREFIX/bin/$TARGET-gcc" ] && [ -x "$PREFIX/bin/$TARGET-ld" ]
}

FetchAndUnpack() {
    local url="$1"
    local archive="$2"
    local directory="$3"

    if [ -d "$directory" ]; then
        echo "Present already, not fetched again: $directory"
        return 0
    fi

    if [ ! -f "$archive" ]; then
        echo "Fetching $archive"
        curl -fL --retry 3 -o "$archive" "$url"
    fi

    echo "Unpacking $archive"
    tar -xf "$archive"
}

BuildBinutils() {
    cd "$SRC_DIR"
    FetchAndUnpack \
        "https://ftpmirror.gnu.org/binutils/binutils-$BINUTILS_VERSION.tar.xz" \
        "binutils-$BINUTILS_VERSION.tar.xz" \
        "binutils-$BINUTILS_VERSION"

    rm -rf build-binutils
    mkdir build-binutils
    cd build-binutils

    # --with-sysroot is given so that the linker has a sysroot to be empty
    # rather than falling back upon the host's, and --disable-nls keeps the
    # diagnostics in one language, which is the one the documentation quotes.
    "../binutils-$BINUTILS_VERSION/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --with-sysroot \
        --disable-nls \
        --disable-werror

    make -j"$JOBS"
    make install
}

BuildGcc() {
    cd "$SRC_DIR"
    FetchAndUnpack \
        "https://ftpmirror.gnu.org/gcc/gcc-$GCC_VERSION/gcc-$GCC_VERSION.tar.xz" \
        "gcc-$GCC_VERSION.tar.xz" \
        "gcc-$GCC_VERSION"

    # GMP, MPFR and MPC are required by GCC itself. The script that accompanies
    # the source retrieves the versions that source was tested against, which is
    # a stronger guarantee than whatever the distribution happens to carry.
    (cd "gcc-$GCC_VERSION" && ./contrib/download_prerequisites)

    rm -rf build-gcc
    mkdir build-gcc
    cd build-gcc

    # --without-headers states that no C library exists for this target, which
    # is true and will remain true until Phase 7. Only the C front end is built:
    # nothing in this project is written in another language GCC compiles.
    "../gcc-$GCC_VERSION/configure" \
        --target="$TARGET" \
        --prefix="$PREFIX" \
        --disable-nls \
        --enable-languages=c \
        --without-headers \
        --disable-werror

    make -j"$JOBS" all-gcc all-target-libgcc
    make install-gcc install-target-libgcc
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    Usage
    exit 0
fi

if AlreadyPresent && [ "${FORCE:-0}" != "1" ]; then
    echo "The toolchain stands at $PREFIX already:"
    "$PREFIX/bin/$TARGET-gcc" --version | head -1
    "$PREFIX/bin/$TARGET-ld" --version | head -1
    echo "Set FORCE=1 to build it again."
    exit 0
fi

echo "Building the $TARGET toolchain."
echo "  binutils : $BINUTILS_VERSION"
echo "  gcc      : $GCC_VERSION"
echo "  prefix   : $PREFIX"
echo "  jobs     : $JOBS"
echo "This takes twenty to forty minutes."

mkdir -p "$SRC_DIR" "$PREFIX"
BuildBinutils
BuildGcc

echo
echo "TOOLCHAIN BUILT. Put it upon the PATH, as TOOLCHAIN.md Section 8 states:"
echo "  export PATH=\"$PREFIX/bin:\$PATH\""
"$PREFIX/bin/$TARGET-gcc" --version | head -1
