#!/usr/bin/env python3
"""Generate 80x86 coverage sources (oracle-filtered), like gen_ppc.py.

usage: gen_x86.py VASMX86_STD outdir
"""
import os, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

VASM, OUT = sys.argv[1], sys.argv[2]

R8 = ["%al", "%bl", "%ah", "%cl", "%dh"]
R16 = ["%ax", "%bx", "%dx", "%sp", "%di"]
R32 = ["%eax", "%ebx", "%ecx", "%esp", "%ebp", "%esi", "%edi"]
MEM = ["(%eax)", "4(%ebx)", "-4(%ebp)", "0x1234", "(%esp)", "0x12(%eax,%ebx,4)", "(,%ecx,8)",
       "%es:(%edi)", "0(%eax)", "(%ebp,%esi)", "0x80(%eax)", "-0x80(%eax)", "(%bx,%si)", "4(%bp)",
       "0x12345678(%esi)", "%fs:4", "(%eax,%esp)", "(%esp,%eax,2)", "-128(%ecx)", "127(%edx)", "(%bp)",
       "0x100(%bx,%di)", "%ss:0x10(%ebp)"]
IMM = ["$0", "$1", "$-1", "$127", "$128", "$-128", "$-129", "$255", "$256", "$0x7fff", "$0x8000",
       "$65535", "$0x12345678", "$-0x80000000", "$'A'", "$3*4"]

cands = []
def emit(m, ops):
    cands.append("\t%s\t%s" % (m, ops) if ops else "\t" + m)

two = "add or adc sbb and sub xor cmp test mov xchg".split()
for m in two:
    for s in ["", "b", "w", "l"]:
        regs = {"b": R8, "w": R16, "l": R32, "": R8[:2] + R16[:2] + R32}[s]
        for a in regs[:3]:
            for b in regs[:4]:
                emit(m + s, a + "," + b)
        for x in MEM:
            emit(m + s, x + "," + regs[0]); emit(m + s, regs[1] + "," + x)
        for i in IMM:
            emit(m + s, i + "," + regs[0]); emit(m + s, i + "," + regs[1])
        for i in IMM[:6]:
            emit(m + s, i + ",(%eax)"); emit(m + s, i + ",0x10(%ebx,%ecx,2)")
one = "inc dec not neg mul imul div idiv push pop".split()
for m in one:
    for s in ["", "b", "w", "l"]:
        for r in R8[:3] + R16[:3] + R32:
            emit(m + s, r)
        for x in MEM[:8]:
            emit(m + s, x)
for m in ["push", "pushl", "pushw"]:
    for i in IMM:
        emit(m, i)
    for sr in ["%es", "%cs", "%ss", "%ds", "%fs", "%gs"]:
        emit(m, sr)
for m in ["pop", "popl", "popw"]:
    for sr in ["%es", "%ss", "%ds", "%fs", "%gs", "%cs"]:
        emit(m, sr)
for m in "rol ror rcl rcr shl sal shr sar".split():
    for s in ["", "b", "w", "l"]:
        for r in ["%al", "%bx", "%ecx", "(%eax)", "4(%esi)"]:
            emit(m + s, r)
            for c in ["$0", "$1", "$3", "$31", "$32", "$-1", "%cl"]:
                emit(m + s, c + "," + r)
for m in ["imul", "imulw", "imull"]:
    for i in IMM:
        emit(m, i + ",%ebx,%eax"); emit(m, i + ",(%ecx),%edx"); emit(m, i + ",%eax"); emit(m, i + ",%bx,%ax")
    for x in ["%ebx,%eax", "(%eax),%ebx", "%cx,%dx", "4(%ebp),%esi"]:
        emit(m, x)
for m in ["shld", "shrd", "shldl", "shrdw"]:
    for x in ["$3,%eax,%ebx", "%cl,%eax,%ebx", "$31,%ebx,(%eax)", "%cl,%ax,%bx", "$1,%ax,(%ebx)", "%eax,%ebx"]:
        emit(m, x)
for m in "bt bts btr btc".split():
    for s in ["", "w", "l"]:
        for x in ["%eax,%ebx", "%ax,%bx", "%ecx,(%eax)", "$3,%eax", "$31,(%eax)", "$7,%bx", "$255,%ecx"]:
            emit(m + s, x)
for m in "bsf bsr lar lsl cmpxchg xadd lea lds les lfs lgs lss bound".split():
    for s in ["", "b", "w", "l"]:
        for x in ["%eax,%ebx", "(%eax),%ebx", "%ax,%bx", "4(%ebp),%cx", "%al,%bl", "%ebx,(%eax)", "%bl,(%eax)"]:
            emit(m + s, x)
for m in ["movsbw", "movsbl", "movswl", "movzbw", "movzbl", "movzwl", "movsxb", "movzxb", "movsx", "movzx"]:
    for x in ["%al,%ax", "%al,%eax", "%ah,%ebx", "%ax,%eax", "(%eax),%ebx", "(%eax),%bx", "%bl,%cx", "%cl,%esi"]:
        emit(m, x)
for m in ["mov", "movl", "movw"]:
    for x in ["%ds,%eax", "%es,%ax", "%fs,(%eax)", "%eax,%ds", "%ax,%es", "(%eax),%ss", "%cr0,%eax",
              "%eax,%cr3", "%cr4,%ebx", "%dr7,%eax", "%eax,%dr0", "%tr6,%ecx", "%ecx,%tr7", "%gs,%bx",
              "%eax,%cs", "$1,%cr0"]:
        emit(m, x)
for c in "o no b c nae ae nb nc e z ne nz be na a nbe s ns p pe np po l nge ge nl le ng g nle".split():
    for t in [".", ".+2", ".+127", ".+128", ".+129", ".-126", ".-127", ".+0x1000", ".-0x1000"]:
        emit("j" + c, t)
    for x in ["%al", "(%eax)", "%bh", "4(%ebx)"]:
        emit("set" + c, x)
    for x in ["%eax,%ebx", "(%eax),%ecx", "%ax,%bx"]:
        emit("cmov" + c, x); emit("cmov" + c + "l", x)
for m in ["jmp", "call", "loop", "loope", "loopz", "loopne", "loopnz", "jecxz", "jcxz", "calll", "jmpl"]:
    for t in [".", ".+2", ".+127", ".+128", ".+129", ".-126", ".-127", ".+0x1000", ".-0x1000", "0x1000"]:
        emit(m, t)
for m in ["jmp", "call", "jmpl", "calll", "jmpw", "callw", "ljmp", "lcall", "ljmpl", "lcallw"]:
    for x in ["*%eax", "*%ax", "*(%eax)", "*4(%ebx,%ecx,4)", "*0x1234", "$8,$0x1000", "$0x10,$0x12345678"]:
        emit(m, x)
for m in ["ret", "retl", "retw", "lret", "lretl", "iret", "iretl", "iretw", "int", "enter", "enterl", "aam", "aad", "leave", "leavel"]:
    for x in ["", "$0", "$8", "$0x10000", "$255", "$256", "$8,$0", "$0x100,$1", "$3"]:
        emit(m, x)
for m in ["in", "inb", "inw", "inl", "out", "outb", "outw", "outl"]:
    for x in ["$0x60,%al", "$0x60,%ax", "$0x60,%eax", "%dx,%al", "%dx,%ax", "%dx,%eax",
              "%al,$0x60", "%ax,$0x60", "%eax,$0x60", "%al,%dx", "%ax,%dx", "%eax,%dx", "$256,%al"]:
        emit(m, x)
for m in ["sldt", "str", "smsw", "lldt", "ltr", "verr", "verw", "lmsw", "lgdt", "sgdt", "lidt", "sidt", "invlpg",
          "sldtl", "smsww", "strw", "arpl", "bswap", "cmpxchg8b", "clflush", "prefetchnta", "prefetcht0",
          "prefetcht1", "prefetcht2", "movnti", "fxsave", "fxrstor", "ldmxcsr", "stmxcsr", "bswapl"]:
    for x in ["%eax", "%ax", "(%eax)", "4(%ebx)", "%ax,%bx", "%ax,(%eax)", "%eax,(%ebx)"]:
        emit(m, x)
zero = """pusha pushal pushaw popa popal popaw pushf pushfl pushfw popf popfl popfw cbw cwde cwtl cwd cdq cltd
cbtw cwtd xlat xlatb lahf sahf aaa aas daa das aam aad int3 into leave nop hlt cmc clc stc cli sti cld std
clts invd wbinvd ud2 cpuid rdtsc rdmsr wrmsr rdpmc rsm sysenter sysexit emms femms pause sfence lfence mfence
monitor mwait movsb movsw movsl movsd cmpsb cmpsw cmpsl cmpsd stosb stosw stosl stosd lodsb lodsw lodsl lodsd
scasb scasw scasl scasd insb insw insl insd outsb outsw outsl outsd rep repe repz repne repnz lock data16
addr16 wait fwait ret lret iret""".split()
for m in zero:
    emit(m, "")
# x87
fpu = """fld fst fstp fldt fstpt fild filds fildl fildq fildll fist fists fistl fistp fistps fistpl fistpq fistpll
fisttp fisttpl fisttpll fadd fmul fcom fcomp fsub fsubr fdiv fdivr fiadd fiadds fiaddl fimul fimuls fimull ficom
ficoms ficoml ficomp ficompl fisub fisubl fisubr fisubrl fidiv fidivl fidivr fidivrs fcompp fucom fucomp fucompp
fcomi fcomip fucomi fucomip faddp fmulp fsubp fsubrp fdivp fdivrp fxch ffree ffreep fcmovb fcmove fcmovbe
fcmovu fcmovnb fcmovne fcmovnbe fcmovnu fldz fld1 fldpi fldl2t fldl2e fldlg2 fldln2 fchs fabs ftst fxam f2xm1
fyl2x fptan fpatan fxtract fprem1 fdecstp fincstp fprem fyl2xp1 fsqrt fsincos frndint fscale fsin fcos fnop
fninit finit fnclex fclex fnstsw fstsw fstcw fnstcw fldcw fstenv fnstenv fldenv fsave fnsave frstor fbld
fbstp""".split()
for m in fpu:
    for x in ["", "%st", "%st(1)", "%st(3),%st", "%st,%st(2)", "%st(0),%st", "(%eax)", "4(%ebx)", "%ax", "%st(7)"]:
        emit(m, x)
simd = """movd movq punpcklbw punpcklwd punpckldq packsswb pcmpgtb pcmpgtw pcmpgtd packuswb punpckhbw punpckhwd
punpckhdq packssdw pcmpeqb pcmpeqw pcmpeqd psrlw psrld psrlq paddq pmullw psubusb psubusw pminub pand paddusb
paddusw pmaxub pandn pavgb psraw psrad pavgw pmulhuw pmulhw psubsb psubsw pminsw por paddsb paddsw pmaxsw pxor
psllw pslld psllq pmuludq pmaddwd psadbw psubb psubw psubd psubq paddb paddw paddd pshufw pshufd pshufhw pshuflw
movups movaps movupd movapd movss movsd movdqa movdqu addps addss addpd addsd mulps mulss subps subsd minps
maxsd divps divss sqrtps sqrtsd rsqrtps rcpss andps andnps orps xorps andpd xorpd unpcklps unpckhps unpcklpd
comiss ucomiss comisd ucomisd cmpps cmpss cmppd cmpsd shufps shufpd cvtsi2ss cvtsi2sd cvtss2si cvttss2si
cvtsd2si cvttsd2si cvtpi2ps cvtps2pi cvttps2pi cvtps2pd cvtpd2ps cvtss2sd cvtsd2ss cvtdq2ps cvtps2dq movhlps
movlhps movlps movhps movmskps movntps movntq maskmovq pmovmskb pextrw pinsrw movdq2q movq2dq cmpeqps cmpltss
cmplepd cmpunordsd cmpneqps cmpnltss cmpnlepd cmpordps pfadd pfsub pfmul""".split()
for m in simd:
    for x in ["%mm0,%mm1", "(%eax),%mm2", "%mm3,(%eax)", "%xmm0,%xmm1", "(%eax),%xmm2", "%xmm3,(%eax)",
              "%eax,%mm0", "%mm0,%eax", "%eax,%xmm1", "%xmm1,%eax", "$3,%mm1", "$3,%xmm1", "$1,%mm0,%mm1",
              "$1,%xmm0,%xmm1", "$2,(%eax),%xmm1", "$2,%eax,%mm1", "$2,%mm1,%eax", "4(%ebx,%esi,2),%xmm7"]:
        emit(m, x)

def ok(line):
    with tempfile.TemporaryDirectory() as d:
        open(os.path.join(d, "t.s"), "w").write(line + "\n")
        p = subprocess.run([VASM, "-quiet", "-Felf", "-o", os.path.join(d, "o"), "t.s"],
                           cwd=d, capture_output=True)
        return p.returncode == 0 and not p.stderr.strip()

with ThreadPoolExecutor(16) as ex:
    res = list(ex.map(ok, cands))
good = [c for c, r in zip(cands, res) if r]
os.makedirs(OUT, exist_ok=True)
for i in range(0, len(good), 2000):
    with open(os.path.join(OUT, "x86_insn%d.s" % (i // 2000)), "w") as f:
        f.write("# gfasm-args: -Felf\n# gfasm-cpu: x86 std\n# generated by tests/gen/gen_x86.py (oracle-filtered)\n")
        for c in good[i:i + 2000]:
            f.write(c + "\n")
print(len(good), "of", len(cands))
