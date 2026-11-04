#!/usr/bin/env python3
"""Line-by-line oracle comparison for instruction coverage files.

usage: linecmp.py GFASM VASM file.s [extra args...]

Lines above a line '; --' are a prologue that is kept for every test.
Each following non-empty, non-comment line is assembled on its own with
both assemblers and the outputs (bytes or failure) are compared.  For hunk
objects only the first code hunk's contents are shown in diffs.
"""
import os, subprocess, sys, tempfile, struct
from concurrent.futures import ThreadPoolExecutor

def code_of(b):
    if b is None or len(b) < 8 or b[:4] != b"\0\0\x03\xe7":
        return b
    i = 0
    while i + 8 <= len(b):
        (w,) = struct.unpack(">I", b[i:i + 4])
        if (w & 0x3fffffff) in (0x3e9, 0x3ea):
            (n,) = struct.unpack(">I", b[i + 4:i + 8])
            return b[i + 8:i + 8 + 4 * n]
        i += 4
    return b

def run(exe, args, d, out):
    p = subprocess.run([exe, "-quiet", "-Fbin"] + args + ["-o", out, "t.s"],
                       cwd=d, capture_output=True, text=True)
    if p.returncode != 0 or not os.path.exists(os.path.join(d, out)):
        return None, (p.stdout + p.stderr).strip().splitlines()[:3]
    return open(os.path.join(d, out), "rb").read(), None

def one(job):
    g, v, pro, l, args = job
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "t.s"), "w").write("\n".join(pro + [l]) + "\n")
        gb, ge = run(g, args, d, "g.o")
        vb, ve = run(v, args, d, "v.o")
    if gb == vb:
        return None
    def show(b, e):
        if b is None:
            return "FAIL:" + " ".join(x for x in (e or []) if x.strip())[:80]
        return code_of(b).hex()
    return "DIFF %-36s vasm=%s gfasm=%s" % (l.strip(), show(vb, ve), show(gb, ge))

def main():
    g, v, f = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2]), sys.argv[3]
    args = sys.argv[4:]
    lines = open(f).read().splitlines()
    pro, body = [], lines
    for i, l in enumerate(lines):
        if l.strip() == "; --":
            pro, body = lines[:i], lines[i + 1:]
            break
    jobs = [(g, v, pro, l, args) for l in body
            if l.strip() and not l.strip().startswith(";") and not l.strip().startswith("*")]
    with ThreadPoolExecutor(16) as ex:
        res = list(ex.map(one, jobs))
    bad = [r for r in res if r]
    for r in bad:
        print(r)
    print("%d/%d lines match" % (len(jobs) - len(bad), len(jobs)))

main()
