#!/bin/sh
# Compare gfasm with vasm on real-world sources that are not part of this
# repository (they have their own licences; tests/fetch-realworld.sh clones
# them).  Each line of the list file is
#     <options> | <path to source>
# and the source is assembled in its own directory with those options
# (or, for "<dir>//<relative path>", in <dir>).  @RW@ is the clone directory
# ($RW, default tests/rw) and @NDK@ the NDK assembler includes ($NDK).
#   GFASM=names/vasmppc_std VASM=/path/to/vasmppc_std sh tests/realworld.sh tests/realworld-ppc.list
top=$(cd "$(dirname "$0")/.." && pwd)
G="${GFASM:-$top/gfasm}"
case "$G" in /*) ;; */*) G="$PWD/$G";; esac
RUN=${GFASM_RUN:-}   # e.g. wine, qemu-ppc-static
: "${VASM:?set VASM to the vasm binary to compare with}"
list=${1:?usage: realworld.sh tests/realworld-<cpu>.list}
case "$VASM" in /*) ;; */*) VASM="$PWD/$VASM";; esac
RW=${RW:-$top/tests/rw}
tmp=$(mktemp -d)
pass=0; fail=0; skip=0
while IFS= read -r line; do
  case "$line" in ''|'#'*) continue;; esac
  line=$(printf "%s" "$line" | sed "s|@RW@|$RW|g; s|@NDK@|${NDK:-@NDK@}|g")
  opts=${line%%|*}
  src=$(echo "${line#*|}" | sed 's/^ *//; s/ *$//')
  case "$src" in
  *//*) dir=${src%%//*}; base=${src#*//}; src="$dir/$base";;   # dir//file: assemble from dir
  *) dir=$(dirname "$src"); base=$(basename "$src");;
  esac
  [ -f "$src" ] || { echo "SKIP (missing) $src"; skip=$((skip+1)); continue; }
  if ! ( cd "$dir" && "$VASM" -quiet $opts -o "$tmp/v.o" "$base" ) >/dev/null 2>&1; then
    echo "SKIP (vasm fails) $src"; skip=$((skip+1)); continue
  fi
  ( cd "$dir" && $RUN "$G" -quiet $opts -o "$tmp/g.o" "$base" ) >"$tmp/g.log" 2>&1
  if cmp -s "$tmp/v.o" "$tmp/g.o"; then
    echo "PASS $src"; pass=$((pass+1))
  else
    echo "FAIL $src"; head -5 "$tmp/g.log" | sed 's/^/    /'; fail=$((fail+1))
  fi
  rm -f "$tmp/v.o" "$tmp/g.o"
done < "$list"
rm -rf "$tmp"
echo "realworld: $pass passed, $fail failed, $skip skipped"
[ $fail -eq 0 ]
