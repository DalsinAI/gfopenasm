#!/bin/sh
# build-amiga.sh, the kitchen's build: with a stand-in for the stove's
# compiler (this machine's cc under the stove's names) it must run the
# corpus, build OUT/gfasm, say which compiler and commit it was, and stop by
# name when the stove has no compiler or the version line is gone.
# MIT, Copyright (c) 2026 AmiCradle Team.
set -eu
top=$(cd "$(dirname "$0")/../.." && pwd)
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
pass=0; fail=0
ok()  { pass=$((pass+1)); echo "ok   $1"; }
bad() { fail=$((fail+1)); echo "FAIL $1"; }
V=$(sed -n 's/^#define GFASM_VERSION "\(.*\)"/\1/p' "$top/src/gfasm.h")

# a stove whose m68k-amigaos-gcc is this machine's cc without the Amiga-only options
mkdir -p "$W/os32-gcc16-test/prefix/bin"
cat > "$W/os32-gcc16-test/prefix/bin/m68k-amigaos-gcc" <<'CC'
#!/bin/sh
[ "${1:-}" = --version ] && { echo "m68k-amigaos-gcc (stand-in) 16.2.0"; exit 0; }
for a in "$@"; do case "$a" in -noixemul|-m68000) ;; *) set -- "$@" "$a";; esac; shift; done
exec cc "$@"
CC
printf '#!/bin/sh\nexit 0\n' > "$W/os32-gcc16-test/prefix/bin/m68k-amigaos-strip"
chmod +x "$W/os32-gcc16-test/prefix/bin/"*

if STOVE="$W/os32-gcc16-test" OUT="$W/out" sh "$top/build-amiga.sh" > "$W/log" 2>&1; then ok "builds"; else bad "builds"; tail -5 "$W/log"; fi
grep -q "^build-amiga.sh: corpus: " "$W/log" && ok "runs the corpus first" || bad "runs the corpus first"
[ -x "$W/out/gfasm" ] && ok "OUT/gfasm is there" || bad "OUT/gfasm is there"
grep -qx "gfopenasm $V, AmigaOS 3.x (amigaos-m68k)" "$W/out/BUILD-INFO.txt" && ok "BUILD-INFO names the version in src/gfasm.h" || bad "BUILD-INFO names the version in src/gfasm.h"
grep -qx "compiler: m68k-amigaos-gcc (stand-in) 16.2.0" "$W/out/BUILD-INFO.txt" && ok "BUILD-INFO names the compiler" || bad "BUILD-INFO names the compiler"
grep -qx "stove:    os32-gcc16-test" "$W/out/BUILD-INFO.txt" && ok "BUILD-INFO names the stove" || bad "BUILD-INFO names the stove"
if command -v lha >/dev/null 2>&1; then
    [ -s "$W/out/release/gfopenasm-$V-amigaos-m68k.lha" ] && ok "the archive has the release's name" || bad "the archive has the release's name"
else
    grep -q "no lha here" "$W/log" && ok "says so when lha is missing" || bad "says so when lha is missing"
fi

if STOVE="$W/nostove" OUT="$W/out2" HOST_TEST=no sh "$top/build-amiga.sh" > "$W/log2" 2>&1; then bad "stops with no compiler"; else ok "stops with no compiler"; fi
grep -q "no compiler at $W/nostove/prefix/bin/m68k-amigaos-gcc" "$W/log2" && ok "and says where it looked" || bad "and says where it looked"

# a tree whose tests fail must not be built
mkdir "$W/tree" && ( cd "$top" && tar -cf - --exclude=.git . ) | tar -xf - -C "$W/tree"
f=$(ls "$W/tree/tests/expected/"*.out | head -1); printf 'x' >> "$f"
if STOVE="$W/os32-gcc16-test" OUT="$W/out3" sh "$W/tree/build-amiga.sh" > "$W/log3" 2>&1; then bad "stops when the corpus fails"; else ok "stops when the corpus fails"; fi
[ ! -e "$W/out3/gfasm" ] && ok "and builds nothing then" || bad "and builds nothing then"

echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
