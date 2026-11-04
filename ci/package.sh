#!/bin/sh
# Package a gfasm built by ci/build.sh as a release archive:
#
#   sh ci/package.sh VERSION TARGET BUILDDIR OUTDIR
#
# Linux: gfopenasm-VERSION-TARGET.tar.xz; Windows: .zip; AmigaOS, AROS: .lha
# (needs lha, e.g. from jlha-utils).  Each holds gfasm, LICENSE, README.md
# and INSTALL.txt in a folder of the same name.  File times are set to the
# time of the last commit, so the archive is the same for the same build.
# COMPILER_DESC overrides the compiler's own version line in INSTALL.txt.
set -eu
V=${1:?usage: ci/package.sh VERSION TARGET BUILDDIR OUTDIR}
T=${2:?}; B=${3:?}; OUT=${4:?}
top=$(cd "$(dirname "$0")/.." && pwd)
case "$OUT" in /*) ;; *) OUT=$PWD/$OUT;; esac
case "$B" in /*) ;; *) B=$PWD/$B;; esac
EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$top" log -1 --format=%ct)}
CCDESC=${COMPILER_DESC:-$(cat "$B/compiler.txt")}   # how to name the compiler in INSTALL.txt
D=gfopenasm-$V-$T
W=$(mktemp -d)
mkdir -p "$W/$D" "$OUT"
names="vasmm68k_mot vasmppc_std vasmppc_mot vasmarm_std vasmx86_std vasmz80_oldstyle vasm6502_oldstyle"
case "$T" in
windows-*)
  cp "$B/gfasm.exe" "$W/$D/"
  # Windows readers want CRLF in the text files
  sed 's/$/\r/' "$top/LICENSE" > "$W/$D/LICENSE.txt"
  sed 's/$/\r/' "$top/README.md" > "$W/$D/README.md"
  {
    echo "gfopenasm $V for Windows x86-64"
    echo
    echo "Copy gfasm.exe to a folder on your PATH. To use it under a vasm name,"
    echo "copy it (or make a hard link) as vasmm68k_mot.exe, vasmppc_std.exe, ...:"
    echo
    for n in $names; do echo "  mklink /H $n.exe gfasm.exe"; done
    echo
    echo "Built from https://github.com/DalsinAI/gfopenasm tag v$V with"
    echo "$CCDESC."
  } | sed 's/$/\r/' > "$W/$D/INSTALL.txt"
  ;;
amigaos-*|amigaos4-*|aros-*)
  cp "$B/gfasm" "$W/$D/gfasm"
  cp "$top/LICENSE" "$W/$D/LICENSE"
  cp "$top/README.md" "$W/$D/README.md"
  case "$T" in
    amigaos-m68k) sys="AmigaOS 2.04 or later (3.x), any 68000 or better" ;;
    amigaos4-ppc) sys="AmigaOS 4.1" ;;
    aros-x86_64)  sys="AROS x86-64 (ABI v11)" ;;
  esac
  {
    echo "gfopenasm $V for $sys"
    echo
    echo "Copy gfasm to C: (or another directory in your path). To use it"
    echo "under a vasm name, make a link or a copy:"
    echo
    for n in $names; do echo "  MakeLink C:$n C:gfasm HARD"; done
    echo
    echo "gfasm sets its own stack size, so there is no need for a Stack"
    echo "command before it."
    echo
    echo "Built from https://github.com/DalsinAI/gfopenasm tag v$V with"
    echo "$CCDESC."
  } > "$W/$D/INSTALL.txt"
  ;;
linux-*)
  cp "$B/gfasm" "$W/$D/gfasm"
  cp "$top/LICENSE" "$top/README.md" "$W/$D/"
  {
    echo "gfopenasm $V for Linux ($T), statically linked: it runs on any"
    echo "distribution."
    echo
    echo "Copy gfasm to a directory on your PATH, for example /usr/local/bin. To"
    echo "use it under a vasm name, make symbolic links:"
    echo
    for n in $names; do echo "  ln -s gfasm /usr/local/bin/$n"; done
    echo
    echo "Built from https://github.com/DalsinAI/gfopenasm tag v$V with"
    echo "$CCDESC."
  } > "$W/$D/INSTALL.txt"
  ;;
*) echo "ci/package.sh: unknown target $T" >&2; exit 2 ;;
esac
chmod 755 "$W/$D"/gfasm* ; chmod 644 "$W/$D"/*.txt "$W/$D"/*.md "$W/$D"/LICENSE* 2>/dev/null || true
find "$W/$D" -exec touch -h -d "@$EPOCH" {} +
cd "$W"
case "$T" in
windows-*)
  rm -f "$OUT/$D.zip"
  find "$D" | LC_ALL=C sort | TZ=UTC zip -q -X -D "$OUT/$D.zip" -@
  echo "$OUT/$D.zip" ;;
amigaos-*|amigaos4-*|aros-*)
  rm -f "$OUT/$D.lha"
  # lha takes the files in the order given; level 1 headers, as on the Amiga
  TZ=UTC lha -aq1 "$OUT/$D.lha" $(find "$D" -type f | LC_ALL=C sort) >/dev/null
  echo "$OUT/$D.lha" ;;
linux-*)
  tar --sort=name --mtime="@$EPOCH" --owner=0 --group=0 --numeric-owner -cf - "$D" | xz -9 > "$OUT/$D.tar.xz"
  echo "$OUT/$D.tar.xz" ;;
esac
rm -rf "$W"
