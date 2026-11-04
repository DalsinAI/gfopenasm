#!/bin/sh
# Compare gfasm with the vasm oracle on real-world sources that are not part
# of this repository (third-party licences).  Each line of the list file is
#     <options> | <path to source>
# and the source is assembled in its own directory with those options.
#   VASM=/path/to/vasmm68k_mot tests/realworld.sh tests/realworld.list
top=$(cd "$(dirname "$0")/.." && pwd)
G="${GFASM:-$top/gfasm}"
: "${VASM:?set VASM to the vasmm68k_mot oracle binary}"
list=${1:-$top/tests/realworld.list}
tmp=$(mktemp -d)
pass=0; fail=0; skip=0
while IFS= read -r line; do
  case "$line" in ''|'#'*) continue;; esac
  line=$(printf "%s" "$line" | sed "s|@RW@|${RW:-$HOME/t5scratch/gfasm-rw}|g; s|@NDK@|${NDK:-$HOME/AmiCradle/stoves/os32/ndk/Include_I}|g; s|~/|$HOME/|g")
  opts=${line%%|*}
  src=$(echo "${line#*|}" | sed 's/^ *//; s/ *$//')
  [ -f "$src" ] || { echo "SKIP (missing) $src"; skip=$((skip+1)); continue; }
  dir=$(dirname "$src"); base=$(basename "$src")
  if ! ( cd "$dir" && "$VASM" -quiet $opts -o "$tmp/v.o" "$base" ) >/dev/null 2>&1; then
    echo "SKIP (oracle fails) $src"; skip=$((skip+1)); continue
  fi
  ( cd "$dir" && "$G" -quiet $opts -o "$tmp/g.o" "$base" ) >"$tmp/g.log" 2>&1
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
