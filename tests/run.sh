#!/bin/sh
# gfasm regression corpus.
#
# Every tests/corpus/*.s is assembled with the options from its
# "; gfasm-args:" line (default: -Fhunk) and the CPU and syntax from its
# "; gfasm-cpu:" line (default: m68k mot).  The output is compared byte for
# byte with tests/expected/<name>.out, which vasm 2.0f produced
# (tests/gen-expected.sh).  When vasm rejected the source, the expectation
# is tests/expected/<name>.fail and gfasm must fail too.
#
# GFASM names the gfasm to test (default ./gfasm) and GFASM_RUN an
# optional command to run it with (an emulator such as "wine" or
# "qemu-ppc-static"); GFASM_SRCPATH=win also names each source by a
# Windows path ("Z:\...", for wine).
top=$(cd "$(dirname "$0")/.." && pwd)
G="${GFASM:-$top/gfasm}"
case "$G" in /*) ;; */*) G="$PWD/$G";; esac
RUN=${GFASM_RUN:-}
out="$top/tests/out"
mkdir -p "$out"
pass=0; fail=0; failed=""
for src in "$top"/tests/corpus/*.s; do
  name=$(basename "$src" .s)
  args=$(sed -n 's/^[;#] gfasm-args: *//p' "$src" | head -1)
  cs=$(sed -n 's/^[;#] gfasm-cpu: *//p' "$src" | head -1)
  [ -z "$cs" ] && cs="m68k mot"
  set -- $cs
  cpu=$1 syn=$2
  [ -z "$args" ] && args="-Fhunk"
  exp="$top/tests/expected/$name.out"
  expfail="$top/tests/expected/$name.fail"
  ( cd "$(dirname "$src")" && $RUN "$G" -cpu=$cpu -syntax=$syn -quiet $args -o "$out/$name.out" "$name.s" ) >"$out/$name.log" 2>&1
  rc=$?
  ok=0
  if [ -f "$expfail" ]; then
    [ $rc -ne 0 ] && ok=1
  elif [ -f "$exp" ]; then
    [ $rc -eq 0 ] && cmp -s "$exp" "$out/$name.out" && ok=1
    # the same source named by a path (the object names it by its file name alone): from the
    # top by a relative path and from / by the absolute one
    if [ $ok = 1 ]; then
      set -- "tests/corpus/$name.s" "$src"
      if [ "${GFASM_SRCPATH:-}" = win ]; then
        set -- "$@" "tests\\corpus\\$name.s" "Z:$(printf '%s' "$src" | sed 's|/|\\|g')"
      fi
      for p in "$@"; do
        ( cd "$top" && case "$p" in /*) cd /;; esac && $RUN "$G" -cpu=$cpu -syntax=$syn -quiet $args -o "$out/$name.path.out" "$p" ) >>"$out/$name.log" 2>&1 &&
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
