#!/bin/sh
# Build gfasm for one target from this source tree, with a cross compiler
# where needed, the same way for every release:
#
#   sh ci/build.sh TARGET OUTDIR      ->  OUTDIR/gfasm (gfasm.exe for Windows)
#
# TARGET           compiler (override with CC=...)    notes
# linux-x86_64     gcc                                static
# linux-arm64      aarch64-linux-gnu-gcc              static
# linux-ppc        powerpc-linux-gnu-gcc              static; big-endian test build, not released
# windows-x86_64   x86_64-w64-mingw32-gcc             static, stripped by the linker, no PE time stamp
# amigaos-m68k     m68k-amigaos-gcc                   AmigaOS 2.04+/3.x, 68000+, libnix
# amigaos4-ppc     ppc-amigaos-gcc                    AmigaOS 4.1, newlib
#
# AROS x86-64 is built on AROS itself, with the SDK's gcc: see ci/aros-hosted.sh.
#
# The cross compilers must be on PATH (or given as CC, with STRIP for the
# matching strip).  SOURCE_DATE_EPOCH is not needed: nothing in the build
# depends on the date or the build directory, so two builds of the same tree
# with the same compiler give the same bytes.
set -eu
T=${1:?usage: ci/build.sh TARGET OUTDIR}
OUT=${2:?usage: ci/build.sh TARGET OUTDIR}
top=$(cd "$(dirname "$0")/.." && pwd)
SRCS=$(sed -n 's/^SRCS = //p' "$top/Makefile")
EXE=
LIBS=-lm
EXTRA=
SFLAGS=
case "$T" in
  linux-x86_64)   P=;                    EXTRA="-static" ;;
  linux-arm64)    P=aarch64-linux-gnu-;  EXTRA="-static" ;;
  linux-ppc)      P=powerpc-linux-gnu-;  EXTRA="-static" ;;
  windows-x86_64) P=x86_64-w64-mingw32-; EXTRA="-static -s -Wl,--no-insert-timestamp"; EXE=.exe; STRIP=true ;;
  amigaos-m68k)   P=m68k-amigaos-;       EXTRA="-noixemul -m68000" ;;
  amigaos4-ppc)   P=ppc-amigaos-;        EXTRA=; SFLAGS="-R .comment" ;;
  *) echo "ci/build.sh: unknown target $T" >&2; exit 2 ;;
esac
CC=${CC:-${P}gcc}
STRIP=${STRIP:-${P}strip}   # Windows: the linker strips (strip would add a PE time stamp)
CFLAGS="-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror -ffile-prefix-map=$top=."
mkdir -p "$OUT"
cd "$top"
# shellcheck disable=SC2086
$CC $CFLAGS $EXTRA -o "$OUT/gfasm$EXE" $SRCS $LIBS
$STRIP $SFLAGS "$OUT/gfasm$EXE"
$CC --version | head -1 > "$OUT/compiler.txt"
echo "$T: $OUT/gfasm$EXE ($(wc -c < "$OUT/gfasm$EXE") bytes, $(cat "$OUT/compiler.txt"))"
