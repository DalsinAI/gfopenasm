#!/usr/bin/env python3
"""Find real-world sources that vasm can assemble, for tests/realworld.sh.

usage: rw_discover.py VASM NDK_INCLUDE_I dir... > list

For every *.s/*.asm/*.i below the given directories, a few option sets are
tried with vasm.  The first one that works is printed as
a realworld.sh list line.
"""
import os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

VASM, NDK = sys.argv[1], sys.argv[2]
roots = sys.argv[3:]
SETS = ["-Fhunk", "-Fhunk -m68020", "-Fhunk -m68040", "-Fhunk -m68020 -m68881",
        "-Fhunkexe -nosym", "-Fbin"]

def incs(path, root):
    d = os.path.dirname(path)
    out = ["-I" + d, "-I" + root]
    for sub in ("include", "inc", "includes", "Include", "common", "framework", "src"):
        p = os.path.join(root, sub)
        if os.path.isdir(p):
            out.append("-I" + p)
    out += ["-I" + NDK, "-I" + os.path.join(NDK, "lvo")]
    return out

def try_file(job):
    path, root = job
    with tempfile.TemporaryDirectory() as t:
        for s in SETS:
            args = s.split() + incs(path, root)
            try:
                p = subprocess.run([VASM, "-quiet"] + args + ["-o", os.path.join(t, "o"),
                                   os.path.basename(path)], cwd=os.path.dirname(path),
                                   capture_output=True, timeout=60)
            except subprocess.TimeoutExpired:
                return None
            if p.returncode == 0:
                return "%s | %s" % (" ".join(args), path)
    return None

jobs = []
for r in roots:
    r = os.path.abspath(r)
    for top in sorted(os.listdir(r)):
        root = os.path.join(r, top)
        if not os.path.isdir(root):
            continue
        for dp, dn, fn in os.walk(root):
            dn[:] = [x for x in dn if not x.startswith(".")]
            for f in sorted(fn):
                if f.lower().endswith((".s", ".asm")):
                    jobs.append((os.path.join(dp, f), root))
with ThreadPoolExecutor(16) as ex:
    for res in ex.map(try_file, jobs):
        if res and " " not in res.split(" | ", 1)[1]:
            print(res)
