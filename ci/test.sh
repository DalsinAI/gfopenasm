#!/bin/sh
# Run the test corpus with a gfasm built by ci/build.sh, under an emulator
# when the target is not the host:
#
#   sh ci/test.sh TARGET BUILDDIR
#
# linux-x86_64 runs as is on x86-64; linux-arm64 and linux-ppc (big-endian)
# under qemu-aarch64-static / qemu-ppc-static; windows-x86_64 under wine,
# which also names each source by a Windows path (Z:\...).  It also checks
# that the program picks the CPU and syntax when it runs under a vasm name.
set -eu
T=${1:?usage: ci/test.sh TARGET BUILDDIR}
B=${2:?usage: ci/test.sh TARGET BUILDDIR}
top=$(cd "$(dirname "$0")/.." && pwd)
case "$B" in /*) ;; *) B=$PWD/$B;; esac
EXE= RUN= SRCPATH=
host=$(uname -m)
case "$T" in
  linux-x86_64)   [ "$host" = x86_64 ] || RUN=qemu-x86_64-static ;;
  linux-arm64)    [ "$host" = aarch64 ] || RUN=qemu-aarch64-static ;;
  linux-ppc)      RUN=qemu-ppc-static ;;
  windows-x86_64) RUN=wine; EXE=.exe; SRCPATH=win
                  export WINEDEBUG=-all WINEPREFIX="${WINEPREFIX:-$(mktemp -d)/wine}"
                  wine cmd /c exit >/dev/null 2>&1 || true ;;   # make the wine prefix
  *) echo "ci/test.sh: $T cannot run here; test it on its own system (see CONTRIBUTING.md)" >&2; exit 2 ;;
esac
echo "== $T: corpus${RUN:+ (under $RUN)}"
GFASM="$B/gfasm$EXE" GFASM_RUN="$RUN" GFASM_SRCPATH="$SRCPATH" sh "$top/tests/run.sh"

echo "== $T: run under vasm names"
N=$(mktemp -d)
rc=0
for t in "m68k mot d_exe" "ppc std p_std_data" "arm std a_std_data" "x86 std x_std_data" "z80 oldstyle o_z80_dirs" "6502 oldstyle o_6502_dirs"; do
  set -- $t
  cp "$B/gfasm$EXE" "$N/vasm$1_$2$EXE"
  args=$(sed -n 's/^[;#] gfasm-args: *//p' "$top/tests/corpus/$3.s" | head -1)
  # shellcheck disable=SC2086
  if ( cd "$top/tests/corpus" && $RUN "$N/vasm$1_$2$EXE" -quiet ${args:--Fhunk} -o "$N/$3.out" "$3.s" ) &&
     cmp -s "$N/$3.out" "$top/tests/expected/$3.out"; then
    echo "ok   vasm$1_$2$EXE $3"
  else
    echo "FAIL vasm$1_$2$EXE $3"; rc=1
  fi
done
rm -rf "$N"
exit $rc
