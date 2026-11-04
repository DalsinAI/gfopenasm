#!/bin/sh
# Build gfasm for AROS x86-64 on AROS itself, and run the test corpus there.
#
#   sh ci/aros-hosted.sh OUTDIR        ->  OUTDIR/gfasm, OUTDIR/compiler.txt
#
# It downloads the AROS x86-64 (ABI v11) release 2026.09 for Linux-hosted
# use and its SDK (with a native gcc) from the AROS project's releases,
# checks their SHA-256, boots AROS under Xvfb with a startup script that
# compiles gfasm twice with the SDK's gcc (not stripped: AROS loads
# relocatable ELF files), compares the two builds, runs
# every corpus test with the first one (from the AmigaDOS shell, with a
# 4 KB shell stack), and shuts down.  The outputs are then compared with
# tests/expected on the host.
#
# Needs: curl, unzip, Xvfb and the X11 client libraries (on Ubuntu 24.04:
# xvfb libx11-6 libxext6 libxcursor1 libxrandr2 libxxf86vm1 libxrender1
# libxi6 libgl1), on an x86-64 Linux host.
set -eu
OUT=${1:?usage: ci/aros-hosted.sh OUTDIR}
top=$(cd "$(dirname "$0")/.." && pwd)
case "$OUT" in /*) ;; *) OUT=$PWD/$OUT;; esac
URL=https://github.com/deadwood2/AROS/releases/download/ABIv11_2026.09
SYSZ=AROS-202609-linux-x86_64-system.tar.gz
SYSZ_SHA=85e1a46dd07aaa2402a69143e56ebbd89b37d4cf599e6e58579d718b1525b6bb
SDKZ=SDK-202609-any-x86_64.zip
SDKZ_SHA=8a4e5f09f027844501c1421bd3948c4e55ceb005a16b0bd3575276f40c8977e6
W=${AROS_WORK:-$(mktemp -d)}
DL=${AROS_DOWNLOADS:-$W}
mkdir -p "$OUT" "$DL"
for f in "$SYSZ:$SYSZ_SHA" "$SDKZ:$SDKZ_SHA"; do
  n=${f%%:*}; h=${f#*:}
  [ -f "$DL/$n" ] || curl -sSfL -o "$DL/$n" "$URL/$n"
  echo "$h  $DL/$n" | sha256sum -c - >/dev/null || { echo "aros-hosted.sh: $n: wrong SHA-256" >&2; exit 1; }
done
cd "$W"
rm -rf AROS-202609-linux-x86_64-system SDK-202609
tar xzf "$DL/$SYSZ"
unzip -q "$DL/$SDKZ"
S=$W/AROS-202609-linux-x86_64-system
cp -a SDK-202609/Development/. "$S/Development/"
grep -q '^memory' "$S/boot/linux/AROSBootstrap.conf" || echo 'memory 1024' >> "$S/boot/linux/AROSBootstrap.conf"

# the sources and the corpus
mkdir -p "$S/gfasm/out"
cp -a "$top/src" "$top/Makefile" "$S/gfasm/"
cp -a "$top/tests/corpus" "$S/gfasm/corpus"
SRCS=$(sed -n 's/^SRCS = //p' "$top/Makefile")
FL="-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Werror"
cat > "$S/gfasm/build.sh" <<EOF
x86_64-aros-gcc --version | head -1 >compiler.txt
x86_64-aros-gcc $FL -o gfasm.1 $SRCS -lm >out/build1.txt 2>&1; echo rc=\$? >>out/build1.txt
x86_64-aros-gcc $FL -o gfasm.2 $SRCS -lm >out/build2.txt 2>&1; echo rc=\$? >>out/build2.txt
cmp gfasm.1 gfasm.2 >out/cmp.txt 2>&1; echo rc=\$? >>out/cmp.txt
EOF
{
  echo "Execute SYS:Development/S/Package-Startup"
  echo "FailAt 1000"
  echo "Stack 2000000"
  echo "CD SYS:gfasm"
  echo "sh build.sh"
  echo "Stack 4096"
  echo "CD SYS:gfasm/corpus"
  for src in "$top"/tests/corpus/*.s; do
    name=$(basename "$src" .s)
    args=$(sed -n 's/^[;#] gfasm-args: *//p' "$src" | head -1)
    cs=$(sed -n 's/^[;#] gfasm-cpu: *//p' "$src" | head -1)
    [ -n "$cs" ] || cs="m68k mot"
    set -- $cs
    echo "SYS:gfasm/gfasm.1 -cpu=$1 -syntax=$2 -quiet ${args:--Fhunk} -o SYS:gfasm/out/$name.out $name.s >SYS:gfasm/out/$name.log"
  done
  echo "Copy SYS:gfasm/gfasm.1 SYS:gfasm/vasmm68k_mot"
  echo "SYS:gfasm/vasmm68k_mot -quiet -Fhunk -o SYS:gfasm/out/vname.out d_include.s"
  echo "Echo done >SYS:gfasm/out/done"
} > "$S/S/User-Startup"

Xvfb :${AROS_DISPLAY:-77} -screen 0 1024x768x24 >/dev/null 2>&1 &
XP=$!
sleep 2
( cd "$S" && DISPLAY=:${AROS_DISPLAY:-77} exec ./boot/linux/AROSBootstrap ) >"$W/aros.log" 2>&1 &
AP=$!
i=0
while [ ! -f "$S/gfasm/out/done" ] && [ $i -lt "${AROS_TIMEOUT:-1800}" ]; do sleep 5; i=$((i+5)); done
kill $AP $XP 2>/dev/null || true
sleep 1
[ -f "$S/gfasm/out/done" ] || { echo "aros-hosted.sh: AROS did not finish in time (log: $W/aros.log)" >&2; exit 1; }

echo "== AROS x86-64: two native builds"
cat "$S/gfasm/out/build1.txt" "$S/gfasm/out/build2.txt"
grep -q '^rc=0' "$S/gfasm/out/cmp.txt" && echo "two builds identical" || { echo "the two builds differ" >&2; exit 1; }
echo "== AROS x86-64: corpus"
pass=0; fail=0; failed=
for src in "$top"/tests/corpus/*.s; do
  name=$(basename "$src" .s)
  if [ -f "$top/tests/expected/$name.fail" ]; then
    [ -s "$S/gfasm/out/$name.out" ] && ok=0 || ok=1
  else
    cmp -s "$top/tests/expected/$name.out" "$S/gfasm/out/$name.out" && ok=1 || ok=0
  fi
  if [ $ok = 1 ]; then pass=$((pass+1)); else fail=$((fail+1)); failed="$failed $name"; fi
done
echo "corpus: $pass passed, $fail failed${failed:+ ($failed)}"
cmp -s "$S/gfasm/out/vname.out" "$top/tests/expected/d_include.out" && echo "ok   vasmm68k_mot d_include" || { echo "FAIL vasmm68k_mot d_include"; fail=$((fail+1)); }
cp "$S/gfasm/gfasm.1" "$OUT/gfasm"
cp "$S/gfasm/compiler.txt" "$OUT/compiler.txt"
[ -n "${AROS_WORK:-}" ] || rm -rf "$W"
[ $fail -eq 0 ]
