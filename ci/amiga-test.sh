#!/bin/sh
# Run the test corpus on AmigaOS (3.x or 4), MorphOS or AROS, from the
# AmigaDOS shell, and check the results on the host:
#
#   sh ci/amiga-test.sh make DIR ROOT   # DIR: corpus + run.script for the Amiga
#   (copy DIR to the Amiga as ROOT, with gfasm in it, and there:
#      Stack 4096
#      Execute ROOT/run.script
#    then copy ROOT/out back to DIR/out)
#   sh ci/amiga-test.sh check DIR       # compare DIR/out with tests/expected
#
# ROOT is an Amiga path ending in ':' or '/', e.g. "RAM:gfasm/" or
# "Work:gfasm/".  The script also runs gfasm under a vasm name.
set -eu
cmd=${1:?usage: ci/amiga-test.sh make DIR ROOT | check DIR}
D=${2:?usage: ci/amiga-test.sh make DIR ROOT | check DIR}
top=$(cd "$(dirname "$0")/.." && pwd)
case "$cmd" in
make)
  R=${3:?usage: ci/amiga-test.sh make DIR ROOT}
  mkdir -p "$D/out"
  cp -R "$top/tests/corpus" "$D/corpus"
  {
    echo "FailAt 1000"
    echo "CD ${R}corpus"
    for src in "$top"/tests/corpus/*.s; do
      name=$(basename "$src" .s)
      args=$(sed -n 's/^[;#] gfasm-args: *//p' "$src" | head -1)
      cs=$(sed -n 's/^[;#] gfasm-cpu: *//p' "$src" | head -1)
      [ -n "$cs" ] || cs="m68k mot"
      set -- $cs
      echo "${R}gfasm -cpu=$1 -syntax=$2 -quiet ${args:--Fhunk} -o ${R}out/$name.out $name.s >${R}out/$name.log"
      echo "Echo \"$name rc \$RC\""
    done
    echo "Copy ${R}gfasm ${R}vasmm68k_mot"
    echo "${R}vasmm68k_mot -quiet -Fhunk -o ${R}out/vname.out d_include.s"
    echo "Echo \"done\""
  } > "$D/run.script"
  echo "made $D (run.script assembles into ${R}out)"
  ;;
check)
  pass=0; fail=0; failed=
  for src in "$top"/tests/corpus/*.s; do
    name=$(basename "$src" .s)
    if [ -f "$top/tests/expected/$name.fail" ]; then
      [ -s "$D/out/$name.out" ] && ok=0 || ok=1
    else
      cmp -s "$top/tests/expected/$name.out" "$D/out/$name.out" && ok=1 || ok=0
    fi
    if [ $ok = 1 ]; then pass=$((pass+1)); else fail=$((fail+1)); failed="$failed $name"; fi
  done
  cmp -s "$D/out/vname.out" "$top/tests/expected/d_include.out" || { fail=$((fail+1)); failed="$failed (vasm name)"; }
  echo "corpus: $pass passed, $fail failed${failed:+ ($failed)}"
  [ $fail -eq 0 ]
  ;;
*) echo "usage: ci/amiga-test.sh make DIR ROOT | check DIR" >&2; exit 2 ;;
esac
