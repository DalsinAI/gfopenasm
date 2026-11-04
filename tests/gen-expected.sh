#!/bin/sh
# Regenerate tests/expected/* with the vasm oracle (black box only).
#   VASM=/path/to/vasmm68k_mot tests/gen-expected.sh [name...]
top=$(cd "$(dirname "$0")/.." && pwd)
: "${VASM:?set VASM to the vasmm68k_mot oracle binary}"
mkdir -p "$top/tests/expected"
if [ $# -gt 0 ]; then list=""; for n in "$@"; do list="$list $top/tests/corpus/$n.s"; done
else list="$top/tests/corpus/*.s"; fi
for src in $list; do
  name=$(basename "$src" .s)
  args=$(sed -n 's/^; gfasm-args: *//p' "$src" | head -1)
  [ -z "$args" ] && args="-Fhunk"
  rm -f "$top/tests/expected/$name.out" "$top/tests/expected/$name.fail"
  if ( cd "$(dirname "$src")" && "$VASM" -quiet $args -o "$top/tests/expected/$name.out" "$name.s" ) >/dev/null 2>&1; then
    :
  else
    rm -f "$top/tests/expected/$name.out"
    echo "oracle failed" > "$top/tests/expected/$name.fail"
  fi
done
