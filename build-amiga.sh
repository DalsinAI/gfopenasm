#!/bin/sh
# For the kitchen's stove_build (STOVE= and OUT= set, target .): gfasm for
# AmigaOS 3.x with the stove's compiler, and its release archive.
#
#   OUT/gfasm            the program (ci/build.sh amigaos-m68k)
#   OUT/compiler.txt     the compiler's own version line
#   OUT/BUILD-INFO.txt   version, commit, stove and compiler
#   OUT/release/gfopenasm-VERSION-amigaos-m68k.lha   (ci/package.sh; when lha is there)
#
# Before the cross build, gfasm is built for the kitchen's own machine and
# runs the test corpus, so a build of a tree that fails its tests stops here.
# The version is the one in src/gfasm.h.  Nothing is published.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${OUT:-$HERE/build/os3}
STOVE=${STOVE:-$HOME/AmiCradle/stoves/os32-gcc16}
case "$OUT" in /*) ;; *) OUT=$PWD/$OUT;; esac
V=$(sed -n 's/^#define GFASM_VERSION "\(.*\)"/\1/p' "$HERE/src/gfasm.h")
[ -n "$V" ] || { echo "build-amiga.sh: no GFASM_VERSION in src/gfasm.h" >&2; exit 1; }
CC=${AMIGA_CC:-$STOVE/prefix/bin/m68k-amigaos-gcc}
STRIP=${AMIGA_STRIP:-$STOVE/prefix/bin/m68k-amigaos-strip}
[ -x "$CC" ] || { echo "build-amiga.sh: no compiler at $CC (STOVE=$STOVE)" >&2; exit 1; }
mkdir -p "$OUT"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT

# the same tree on this machine first: the corpus, byte for byte
if [ "${HOST_TEST:-yes}" = yes ]; then
    CC=${HOST_CC:-cc} STRIP=true sh "$HERE/ci/build.sh" linux-x86_64 "$TMP/host" >/dev/null
    GFASM="$TMP/host/gfasm" sh "$HERE/tests/run.sh" > "$TMP/corpus.log" 2>&1 ||
        { tail -20 "$TMP/corpus.log" >&2; echo "build-amiga.sh: the test corpus fails on this machine" >&2; exit 1; }
    echo "build-amiga.sh: corpus: $(tail -1 "$TMP/corpus.log")"
fi

CC=$CC STRIP=$STRIP sh "$HERE/ci/build.sh" amigaos-m68k "$OUT"
commit=$(git -C "$HERE" rev-parse HEAD 2>/dev/null || echo unknown)
{
    echo "gfopenasm $V, AmigaOS 3.x (amigaos-m68k)"
    echo "commit:   $commit"
    echo "stove:    $(basename "$STOVE")"
    echo "compiler: $(cat "$OUT/compiler.txt")"
} > "$OUT/BUILD-INFO.txt"
cat "$OUT/BUILD-INFO.txt"

if command -v lha >/dev/null 2>&1; then
    epoch=$(git -C "$HERE" log -1 --format=%ct 2>/dev/null || echo 0)
    SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$epoch} sh "$HERE/ci/package.sh" "$V" amigaos-m68k "$OUT" "$OUT/release"
else
    echo "build-amiga.sh: no lha here: OUT/gfasm is built, the archive is not"
fi
