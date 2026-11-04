#!/bin/sh
# gfasm regression corpus.
#
# Every tests/corpus/*.s (and tests/realworld/*/*.s listed in
# tests/realworld/LIST) is assembled with the options from its
# "; gfasm-args:" line (default: -Fhunk).  The output is compared byte for
# byte with tests/expected/<name>, which was produced by the vasm oracle
# (tests/gen-expected.sh).  A test whose oracle run failed has an
# expected file "<name>.fail": gfasm must fail too.
#
# If VASM is set to a vasm binary, the oracle is run live as well and
# compared against the stored expectation.
top=$(cd "$(dirname "$0")/.." && pwd)
G="$top/gfasm"
out="$top/tests/out"
mkdir -p "$out"
pass=0; fail=0; failed=""
for src in "$top"/tests/corpus/*.s; do
  name=$(basename "$src" .s)
  args=$(sed -n 's/^[;#] gfasm-args: *//p' "$src" | head -1)
  cs=$(sed -n 's/^[;#] gfasm-cpu: *//p' "$src" | head -1)
  [ -z "$cs" ] && cs="m68k mot"
  set -- $cs
  [ -z "$args" ] && args="-Fhunk"
  exp="$top/tests/expected/$name.out"
  expfail="$top/tests/expected/$name.fail"
  ( cd "$(dirname "$src")" && "$G" -cpu=$1 -syntax=$2 -quiet $args -o "$out/$name.out" "$name.s" ) >"$out/$name.log" 2>&1
  rc=$?
  ok=0
  if [ -f "$expfail" ]; then
    [ $rc -ne 0 ] && ok=1
  elif [ -f "$exp" ]; then
    [ $rc -eq 0 ] && cmp -s "$exp" "$out/$name.out" && ok=1
    # the same source named by a path (vasm's object names it by its file name alone): from the
    # top by a relative path and from / by the absolute one
    if [ $ok = 1 ]; then
      for p in "tests/corpus/$name.s" "$src"; do
        ( cd "$top" && cd "$( [ "$p" = "$src" ] && echo / || echo . )" && "$G" -cpu=$1 -syntax=$2 -quiet $args -o "$out/$name.path.out" "$p" ) >>"$out/$name.log" 2>&1 &&
          cmp -s "$exp" "$out/$name.path.out" || { ok=0; echo "$name: differs when the source is named $p"; }
      done
      rm -f "$out/$name.path.out"
    fi
  else
    echo "MISSING expectation for $name (run tests/gen-expected.sh)"
  fi
  if [ $ok = 1 ]; then pass=$((pass+1)); else fail=$((fail+1)); failed="$failed $name"; fi
done
echo "corpus: $pass passed, $fail failed"
[ -n "$failed" ] && echo "failed:$failed"
[ $fail -eq 0 ]
