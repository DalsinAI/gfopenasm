#!/bin/sh
# Regenerate tests/expected/* with the vasm oracle (black box only).
#   [ORACLES=dir-with-vasm<cpu>_<syntax>] tests/gen-expected.sh [name...]
top=$(cd "$(dirname "$0")/.." && pwd)
: "${ORACLES:=$HOME/AmiCradle/stoves/vasm-oracles/2.0f/bin}"
mkdir -p "$top/tests/expected"
if [ $# -gt 0 ]; then list=""; for n in "$@"; do list="$list $top/tests/corpus/$n.s"; done
else list=$(ls "$top"/tests/corpus/*.s); fi
for src in $list; do
  name=$(basename "$src" .s)
  args=$(sed -n 's/^[;#] gfasm-args: *//p' "$src" | head -1)
  cs=$(sed -n 's/^[;#] gfasm-cpu: *//p' "$src" | head -1)
  [ -z "$cs" ] && cs="m68k mot"
  set -- $cs
  [ -z "$args" ] && args="-Fhunk"
  rm -f "$top/tests/expected/$name.out" "$top/tests/expected/$name.fail"
  VASM="$ORACLES/vasm$1_$2"
  [ "$1" = m68k ] && [ "$2" = mot ] && [ -n "${VASM68K:-}" ] && VASM=$VASM68K
  if ( cd "$(dirname "$src")" && "$VASM" -quiet $args -o "$top/tests/expected/$name.out" "$name.s" ) >/dev/null 2>&1; then
    :
  else
    rm -f "$top/tests/expected/$name.out"
    echo "oracle failed" > "$top/tests/expected/$name.fail"
  fi
done
