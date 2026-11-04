#!/usr/bin/env python3
"""Generate ARM/THUMB coverage sources (oracle-filtered), like gen_ppc.py.

usage: gen_arm.py VASMARM_STD outdir
"""
import os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

VASM, OUT = sys.argv[1], sys.argv[2]
CONDS = ["eq", "ne", "cs", "cc", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"]
FEWC = ["eq", "lo", "gt", "al"]

R3 = ["r0,r1,r2", "r15,r14,r13", "a1,v1,ip", "sp,lr,pc", "r0,r1,#1", "r0,r1,#0xff000000",
      "r2,r3,#-1", "r4,r5,#0x101", "r0,r1,#0x3fc00", "r0,r1,#1,2", "r0,r1,#255,30", "r0,r1,#0x10001",
      "r0,r1,#-0x101", "r0,r1,#0x12345678", "r0,r1,#-256", "r0,r1,#0xff1", "r0,r1,#0x7f80001",
      "r0,r1,r2,lsl #0", "r0,r1,r2,lsl #31", "r0,r1,r2,lsr #1", "r0,r1,r2,lsr #32", "r0,r1,r2,asr #32",
      "r0,r1,r2,asl #3", "r0,r1,r2,ror #7", "r0,r1,r2,ror #0", "r0,r1,r2,rrx", "r0,r1,r2,lsl r3",
      "r0,r1,r2,ror r15", "r0,r1,r2,LSR #0", "r0,r1,#256,2", "r0,r1,#1,31", "r0,r1,#0x80000001"]
R2 = ["r0,r1", "r0,#1", "r0,#-1", "r0,#0x101", "r0,#0xff1", "r0,#0x80000080", "r0,#0x40000101",
      "r0,#0x7f80001", "r0,#0xfffffefe", "r0,#0xff00", "r0,#1,30", "r0,r1,lsl #2", "r0,r1,lsr r2",
      "r0,r1,rrx", "r0,r1,asr #32", "pc,lr", "r0,#0x12345678", "r0,#0xf000000f", "r0,#0x102000",
      "r0,#0x3000003", "r9,#0x10200", "r0,#-0x100", "r0,# 1", "r0,r1,lsl #32", "r0,#'A'",
      "r0,#0xffffff00", "r0,#0x8000"]
MUL = ["r0,r1,r2", "r0,r0,r1", "r0,r1,r0", "r15,r1,r2", "r0,r1,r2,r3", "r9,r10,r11,r12", "r0,r0,r1,r2"]
MULL = ["r0,r1,r2,r3", "r0,r0,r1,r2", "r1,r0,r2,r3", "r4,r5,r4,r6", "r12,r11,r10,r9"]
LS = ["r0,[r1]", "r0,[r1,#4]", "r0,[r1,#-4]", "r0,[r1,#4095]", "r0,[r1,#-4095]", "r0,[r1,#255]",
      "r0,[r1,#256]", "r0,[r1,#4]!", "r0,[r1,#-0]", "r0,[r1,#0]", "r0,[r1],#4", "r0,[r1],#-4",
      "r0,[r1],#0", "r0,[r1,r2]", "r0,[r1,-r2]", "r0,[r1,+r2]!", "r0,[r1,r2,lsl #2]",
      "r0,[r1,-r2,asr #32]", "r0,[r1,r2,rrx]", "r0,[r1,r2,ror #0]", "r0,[r1],r2", "r0,[r1],-r2",
      "r0,[r1],r2,lsl #3", "r0,[r1,r2]!", "pc,[sp],#4", "r0,[pc,#8]", "r0,[r1]!", "r0,[ r1 , # 4 ]",
      "r0,=0x12345678", "r0,=1", "r0,=0xffffffff", "r0,=0x1234", "r0,=-256", "r0,$", "r0,$+8",
      "r0,$-4096", "r0,$+4100", "r0,[r1,#-255]", "r0,[r1,#4096]", "r15,[r15,#-8]"]
LSM = ["r0,{r1}", "r0!,{r1-r3}", "sp!,{r4-r11,lr}", "r0,{r0-r15}", "r1,{r2,r5,pc}^", "r13!,{r0,r2,r4}^",
       "r0,{r3-r1}", "r0,{}", "r0!,{r0}"]
CDP = ["p1,2,c3,c4,c5,6", "p15,15,c15,c15,c15,7", "p1,2,c3,c4,c5", "p1,16,c3,c4,c5", "p0,0,c0,c0,c0,0",
       "p1,#2,c3,c4,c5,#6", "p1,2,c3,c4,c5,8"]
MCR = ["p15,0,r0,c1,c0,0", "p15,7,r15,c15,c15,7", "p1,2,r3,c4,c5", "p1,8,r3,c4,c5", "p14,1,sp,c2,c3,4"]
LDC = ["p1,c2,[r3]", "p1,c2,[r3,#8]", "p1,c2,[r3,#-8]!", "p1,c2,[r3],#1020", "p1,c2,[r3,#1024]",
       "p1,c2,[r3,#6]", "p15,c15,[r15]", "p2,c3,[r4],#-8", "p1,c2,[r3,#-0]"]
ADR = ["r0,$", "r1,$+8", "r2,$-0x100", "r3,$+0x1000", "r4,$+0x10001", "r5,$-0x3c00", "r6,$+0x3fc"]
BR = ["$", "$+8", "$-4", "$+0x1fffffc", "$+0x2000000", "$-0x2000000", "$+2", "$+0x400"]

arm = []
def add(mn, ops, conds=CONDS, sfx=("",)):
    for s in sfx:
        for o in ops:
            arm.append("\t%s%s\t%s" % (mn, s, o) if o else "\t%s%s" % (mn, s))
        for c in conds:
            for o in ops[:3]:
                arm.append("\t%s%s%s\t%s" % (mn, c, s, o) if o else "\t%s%s%s" % (mn, c, s))

for m in "and eor sub rsb add adc sbc rsc orr bic".split():
    add(m, R3, FEWC, ("", "s"))
for m in "mov mvn".split():
    add(m, R2, FEWC, ("", "s"))
for m in "tst teq cmp cmn".split():
    add(m, R2, FEWC, ("", "s", "p"))
add("mul", MUL, FEWC, ("", "s")); add("mla", MUL, FEWC, ("", "s"))
for m in "umull umlal smull smlal".split():
    add(m, MULL, FEWC, ("", "s"))
add("ldr", LS, FEWC, ("", "b", "t", "bt", "h", "sh", "sb"))
add("str", LS, FEWC, ("", "b", "t", "bt", "h", "sh"))
for m in ("ldm", "stm"):
    add(m, LSM, FEWC, ("ia", "ib", "da", "db", "fd", "ed", "fa", "ea", ""))
add("swp", ["r0,r1,[r2]", "r0,r0,[r1]", "pc,r1,[r2]", "r0,r1,r2"], FEWC, ("", "b"))
add("b", BR, CONDS); add("bl", BR, CONDS)
add("bx", ["r0", "lr", "pc", "#1"], CONDS)
add("swi", ["0", "0x123456", "0xffffff", "0x1000000", "#1", "-1"], FEWC)
add("svc", ["1"], ["eq"])
add("mrs", ["r0,cpsr", "r1,spsr", "r0,CPSR", "r0,cpsr_c", "r0,cpsr_all"], FEWC)
add("msr", ["cpsr,r0", "cpsr_c,r1", "spsr_fsxc,r2", "cpsr_f,#0xf0000000", "spsr_x,#1", "cpsr_cxsf,#0x101",
            "cpsr_s,r0", "cpsr_fc,r3", "CPSR_cf,r0", "cpsr_flg,r0"], FEWC)
add("cdp", CDP, FEWC); add("mcr", MCR, FEWC); add("mrc", MCR, FEWC)
add("ldc", LDC, FEWC, ("", "l")); add("stc", LDC, FEWC, ("", "l"))
add("adr", ADR, FEWC); add("adrl", ADR, ["eq"])
add("nop", [""], FEWC)
# oddities: trailing garbage, qualifiers, old/new suffix order
for l in ["addseq r0,r1,r2", "addeqsx r0,r1,r2", "movfoo r0,r1", "cmp.b r0,r1", "add.w r0,r1,r2",
          "ldrbeq r0,[r1]", "ldreqx r0,[r1]", "mulseq r0,r1,r2", "movs.b r0,r1", "ldrsheq r0,[r1]",
          "stmeqfd sp!,{r0}", "ldmeqia r0,{r1}", "ADD R0,R1,R2", "MOVEQS R0,#1", "bls $", "blls $",
          "bllo $", "blo $", "bleq $", "bxls r0", "teqp r0,r1", "blal $", "bhs $"]:
    arm.append("\t" + l)

THM = """lsl lsr asr add sub mov cmp and eor adc sbc ror tst neg cmn orr mul bic mvn bx str strb ldr ldrb
strh ldsb ldrh ldsh push pop ldmia stmia swi b bl adr nop lsls movs ldrsb beq bne bcs bcc bhs blo bmi
bpl bvs bvc bhi bls bge blt bgt ble bal""".split()
TOPS = ["", "r0,r1,#2", "r0,r1,#0", "r7,r6,#31", "r0,r1,#32", "r0,r1", "r7,r6", "r0,r1,r2", "r7,r6,r5",
        "r0,r1,#7", "r0,r1,#8", "r0,#255", "r0,#256", "r7,#0", "r8,r1", "r0,r8", "r8,r9", "r15,r14",
        "r0,pc,#8", "r0,pc,#1020", "r0,pc,#2", "r0,sp,#1020", "r1,sp,#4", "sp,#508", "sp,#4", "sp,#-4",
        "sp,#2", "r0,[r1,r2]", "r0,[r1,#4]", "r0,[r1,#124]", "r0,[r1,#128]", "r0,[r1,#31]", "r0,[r1,#62]",
        "r0,[r1,#2]", "r0,[r1,#1]", "r0,[sp,#8]", "r0,[sp,#1020]", "r0,[pc,#8]", "r0,[pc,#1020]",
        "r0,[r1]", "r0,[r8,#4]", "r0,[r1,r8]", "{r0-r7,lr}", "{r0,pc}", "{r1}", "{lr}", "{pc}", "{r8}",
        "r0!,{r1,r2}", "r7!,{r0-r7}", "r0,{r1}", "255", "0", "256", "$", "$+4", "$-2", "$+256", "$-256",
        "$+258", "$+300", "$+2046", "$+2048", "$-2048", "$+0x3ffffe", "$+0x400000", "$+3", "r1", "r8",
        "lr", "pc", "r0,$+8", "r0,$+1020", "r0,$+2", "r0,$-4"]
thumb = []
for m in THM:
    for o in TOPS:
        if m == "swi" and "$" in o:
            continue
        thumb.append("\t%s\t%s" % (m, o) if o else "\t" + m)

def ok(job):
    pro, line = job
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "t.s"), "w").write(pro + line + "\n")
        p = subprocess.run([VASM, "-quiet", "-Felf", "-o", os.path.join(d, "o"), "t.s"],
                           cwd=d, capture_output=True)
        return p.returncode == 0 and not p.stderr.strip()

def run(pro, cands):
    with ThreadPoolExecutor(16) as ex:
        res = list(ex.map(ok, [(pro, c) for c in cands]))
    return [c for c, r in zip(cands, res) if r]

os.makedirs(OUT, exist_ok=True)
good = run("", arm)
lit = [c for c in good if "=" in c]
good = [c for c in good if "=" not in c]
with open(os.path.join(OUT, "arm_lit0.s"), "w") as f:
    f.write("; gfasm-args: -Felf\n; gfasm-cpu: arm std\n; generated by tests/gen/gen_arm.py (oracle-filtered)\n; --\n")
    for c in lit:
        f.write(c + "\n\t.ltorg\n")
for i in range(0, len(good), 1500):
    with open(os.path.join(OUT, "arm_insn%d.s" % (i // 1500)), "w") as f:
        f.write("; gfasm-args: -Felf\n; gfasm-cpu: arm std\n; generated by tests/gen/gen_arm.py (oracle-filtered)\n; --\n")
        for c in good[i:i + 1500]:
            f.write(c + "\n")
tgood = run("\t.thumb\n", thumb)
with open(os.path.join(OUT, "arm_thumb0.s"), "w") as f:
    f.write("; gfasm-args: -Felf\n; gfasm-cpu: arm std\n; generated by tests/gen/gen_arm.py (oracle-filtered)\n\t.thumb\n; --\n")
    for c in tgood:
        f.write("\t.align 2\n" + c + "\n")
print(len(good), "of", len(arm), "ARM;", len(tgood), "of", len(thumb), "THUMB")
