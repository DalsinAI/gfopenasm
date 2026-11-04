/* gfasm - 80x86 cpu module (i386..PentiumPro, x87, MMX, SSE/SSE2 subset),
 * AT&T ("MIT") syntax as used by vasm's x86 backend.  MIT License.
 *
 * Encodings from the public Intel Architecture Software Developer's
 * Manuals (vol. 2, instruction set reference, opcode maps) and the AMD64
 * Architecture Programmer's Manual; ELF relocation numbers from the
 * System V ABI i386 supplement.  vasm behaviour (options, directives,
 * suffix rules, operand order, optimisations) from the vasm manual's
 * 80x86 chapter and comparison with vasm's output; see docs/REFERENCES.md. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int mode_bits = 32;       /* .code16/.code32/.code64 */
static int cmode = 32;           /* mode of the instruction being encoded */
static int opt_m64;

/* ------------------------------------------------------------ registers */
enum { RC_GPR, RC_SEG, RC_CR, RC_DR, RC_TR, RC_ST, RC_MMX, RC_XMM };
typedef struct xreg { const char *n; int cls, num, size; } xreg;
static const xreg regs[] = {
  { "al", RC_GPR, 0, 8 }, { "cl", RC_GPR, 1, 8 }, { "dl", RC_GPR, 2, 8 }, { "bl", RC_GPR, 3, 8 },
  { "ah", RC_GPR, 4, 8 }, { "ch", RC_GPR, 5, 8 }, { "dh", RC_GPR, 6, 8 }, { "bh", RC_GPR, 7, 8 },
  { "ax", RC_GPR, 0, 16 }, { "cx", RC_GPR, 1, 16 }, { "dx", RC_GPR, 2, 16 }, { "bx", RC_GPR, 3, 16 },
  { "sp", RC_GPR, 4, 16 }, { "bp", RC_GPR, 5, 16 }, { "si", RC_GPR, 6, 16 }, { "di", RC_GPR, 7, 16 },
  { "eax", RC_GPR, 0, 32 }, { "ecx", RC_GPR, 1, 32 }, { "edx", RC_GPR, 2, 32 }, { "ebx", RC_GPR, 3, 32 },
  { "esp", RC_GPR, 4, 32 }, { "ebp", RC_GPR, 5, 32 }, { "esi", RC_GPR, 6, 32 }, { "edi", RC_GPR, 7, 32 },
  { "es", RC_SEG, 0, 16 }, { "cs", RC_SEG, 1, 16 }, { "ss", RC_SEG, 2, 16 }, { "ds", RC_SEG, 3, 16 },
  { "fs", RC_SEG, 4, 16 }, { "gs", RC_SEG, 5, 16 },
  { NULL, 0, 0, 0 }
};

static int find_reg(const char *s, size_t n, xreg *out)
{
  int i;
  for (i = 0; regs[i].n; i++)
    if (strlen(regs[i].n) == n && !strnicmp_(s, regs[i].n, n)) { *out = regs[i]; return 1; }
  if (n >= 3 && n <= 4 && !strnicmp_(s, "cr", 2) && isdigit((unsigned char)s[2])) {
    int v = atoi(s + 2); if (v > 15) return 0;
    out->cls = RC_CR; out->num = v; out->size = 32; return 1;
  }
  if (n >= 3 && n <= 4 && !strnicmp_(s, "dr", 2) && isdigit((unsigned char)s[2])) {
    int v = atoi(s + 2); if (v > 15) return 0;
    out->cls = RC_DR; out->num = v; out->size = 32; return 1;
  }
  if (n == 3 && !strnicmp_(s, "tr", 2) && isdigit((unsigned char)s[2])) {
    out->cls = RC_TR; out->num = s[2] - '0'; out->size = 32; return s[2] <= '7';
  }
  if (n == 3 && !strnicmp_(s, "mm", 2) && s[2] >= '0' && s[2] <= '7') {
    out->cls = RC_MMX; out->num = s[2] - '0'; out->size = 64; return 1;
  }
  if (n == 4 && !strnicmp_(s, "xmm", 3) && s[3] >= '0' && s[3] <= '7') {
    out->cls = RC_XMM; out->num = s[3] - '0'; out->size = 128; return 1;
  }
  if (n == 2 && !strnicmp_(s, "st", 2)) { out->cls = RC_ST; out->num = 0; out->size = 80; return 1; }
  return 0;
}

/* ------------------------------------------------------------ operands */
enum { K_REG, K_IMM, K_MEM };
typedef struct xopnd {
  int kind, abs;
  xreg r;
  expr *e;           /* immediate or displacement */
  int base, index, scale, asize;   /* base/index register numbers (-1 none), address size */
  int seg;           /* segment override register number, -1 */
  int hasdisp;
} xopnd;

/* operand type bits (what an operand is) */
#define T_R8    0x00000001
#define T_R16   0x00000002
#define T_R32   0x00000004
#define T_R64   0x00000008
#define T_ACC   0x00000010
#define T_CL    0x00000020
#define T_DX    0x00000040
#define T_SREG2 0x00000080
#define T_SREG3 0x00000100
#define T_CR    0x00000200
#define T_DR    0x00000400
#define T_TR    0x00000800
#define T_ST0   0x00001000
#define T_STI   0x00002000
#define T_MMX   0x00004000
#define T_XMM   0x00008000
#define T_IMM   0x00010000
#define T_MEM   0x00020000
#define T_ABS   0x00040000   /* '*' */
#define T_GPR   (T_R8 | T_R16 | T_R32 | T_R64)

static int parse_reg_at(const char **pp, xreg *r)
{
  const char *p = *pp, *q;
  if (*p != '%') return 0;
  p++;
  q = p;
  while (isalnum((unsigned char)*q)) q++;
  if (!find_reg(p, q - p, r)) { error("unknown register specified"); return -1; }
  if (r->cls == RC_ST) {
    const char *t = skip_ws(q);
    if (*t == '(') {
      t = skip_ws(t + 1);
      if (!isdigit((unsigned char)*t) || t[0] > '7') return -1;
      r->num = *t - '0';
      t = skip_ws(t + 1);
      if (*t != ')') return -1;
      q = t + 1;
      r->size = 81;      /* st(i) written explicitly */
    }
  }
  *pp = q;
  return 1;
}

static int parse_operand(const char *s, xopnd *o)
{
  const char *p = skip_ws(s);
  memset(o, 0, sizeof(*o));
  o->base = o->index = o->seg = -1;
  o->scale = 1;
  if (*p == '*') { o->abs = 1; p = skip_ws(p + 1); }
  if (*p == '$') {
    p++;
    o->kind = K_IMM;
    o->e = parse_expr(&p);
    return o->e && !*skip_ws(p) && !o->abs;
  }
  if (*p == '%') {
    xreg r;
    int k = parse_reg_at(&p, &r);
    if (k <= 0) return 0;
    p = skip_ws(p);
    if (*p == ':' && r.cls == RC_SEG) { o->seg = r.num; p = skip_ws(p + 1); }
    else {
      if (*p) return 0;
      o->kind = K_REG;
      o->r = r;
      return 1;
    }
  }
  /* memory: disp(base,index,scale) */
  o->kind = K_MEM;
  if (*p != '(') {
    o->e = parse_expr(&p);
    if (!o->e) return 0;
    o->hasdisp = 1;
    p = skip_ws(p);
  }
  if (*p == '(') {
    xreg r;
    p = skip_ws(p + 1);
    if (*p == '%') {
      if (parse_reg_at(&p, &r) <= 0 || r.cls != RC_GPR || r.size == 8) { error("base register expected"); return 0; }
      o->base = r.num; o->asize = r.size;
      p = skip_ws(p);
    }
    if (*p == ',') {
      p = skip_ws(p + 1);
      if (parse_reg_at(&p, &r) <= 0 || r.cls != RC_GPR || r.size == 8) return 0;
      o->index = r.num;
      if (o->asize && o->asize != r.size) return 0;
      o->asize = r.size;
      p = skip_ws(p);
      if (*p == ',') {
        ival sc;
        expr *se;
        p = skip_ws(p + 1);
        se = parse_expr(&p);
        if (!se || !eval_const(se, &sc)) { error("absolute scale factor required"); return 0; }
        if (sc != 1 && sc != 2 && sc != 4 && sc != 8) { error("illegal scale factor (valid: 1,2,4,8)"); return 0; }
        o->scale = (int)sc;
        p = skip_ws(p);
      }
    }
    if (*p != ')') { error("missing ')' in baseindex addressing mode"); return 0; }
    p = skip_ws(p + 1);
  } else if (!o->e)
    return 0;
  return !*p;
}


/* ------------------------------------------------------------ templates */
/* Template line:  name  opcode-bytes  [/d | /r]  [flags]  role:class ...
 *   flags: W (w bit), W3 (w bit in bit 3, short forms), D (reversible, opcode|2),
 *          V (reversible, same opcode), S (register in opcode), s=bwlq (suffixes),
 *          dl/dw (default size), P (operand-size prefix even without suffix list)
 *   roles: r modrm.reg, m modrm.rm, i first immediate, j second immediate,
 *          o register in opcode, - implicit
 * Operands are in AT&T order (source first). */
static const char *xtab_src[] = {
  /* data movement */
  "mov a0 W D s=bwl -:MOFFS -:ACC",
  "mov 88 /r W D s=bwl r:R m:RM",
  "mov b0 W3 S s=bwl i:IX o:R",
  "mov c6 /0 W s=bwl i:IANY m:RM",
  "mov 8c /r s=wl dl r:SEG m:RM",
  "mov 8e /r s=wl dl NP m:RM r:SEG",
  "mov 0f,20 /r s=l r:CR m:R32",
  "mov 0f,22 /r s=l m:R32 r:CR",
  "mov 0f,21 /r s=l r:DR m:R32",
  "mov 0f,23 /r s=l m:R32 r:DR",
  "mov 0f,24 /r s=l r:TR m:R32",
  "mov 0f,26 /r s=l m:R32 r:TR",
  "movsbw 0f,be /r m:RM8 r:R16", "movsbl 0f,be /r m:RM8 r:R32", "movswl 0f,bf /r m:RM16 r:R32",
  "movzbw 0f,b6 /r P16 m:RM8 r:R16", "movzbl 0f,b6 /r m:RM8 r:R32", "movzwl 0f,b7 /r m:RM16 r:R32",
  "movsxb 0f,be /r s=wl m:RM8 r:R", "movzxb 0f,b6 /r s=wl m:RM8 r:R",
  "xchg 90 S s=wl o:R -:ACC", "xchg 90 S s=wl -:ACC o:R",
  "xchg 86 /r W V s=bwl r:R m:RM",
  "lea 8d /r s=wl m:M r:R",
  "lds c5 /r s=wl m:M r:R", "les c4 /r s=wl m:M r:R", "lfs 0f,b4 /r s=wl m:M r:R",
  "lgs 0f,b5 /r s=wl m:M r:R", "lss 0f,b2 /r s=wl m:M r:R",
  "push 50 S s=wl o:R", "push ff /6 s=wl dl m:M", "push 6a s=wl dl i:IBS", "push 68 s=wl dl i:I",
  "push 06 s=wl dl -:ES", "push 0e s=wl dl -:CS", "push 16 s=wl dl -:SS", "push 1e s=wl dl -:DS",
  "push 0f,a0 s=wl dl -:FS", "push 0f,a8 s=wl dl -:GS",
  "pop 58 S s=wl o:R", "pop 8f /0 s=wl dl m:M",
  "pop 07 s=wl dl -:ES", "pop 17 s=wl dl -:SS", "pop 1f s=wl dl -:DS", "pop 0f,a1 s=wl dl -:FS", "pop 0f,a9 s=wl dl -:GS",
  "pusha 60 s=wl dl", "popa 61 s=wl dl", "pushf 9c s=wl dl", "popf 9d s=wl dl",
  "pushaw 60 P16", "pushal 60", "popaw 61 P16", "popal 61", "pushfw 9c P16", "pushfl 9c",
  "popfw 9d P16", "popfl 9d",
  "cbw 98", "cwde 98", "cwtl 98", "cwd 99", "cdq 99", "cltd 99", "cbtw 98", "cwtd 99",
  "bswap 0f,c8 S s=l o:R32",
  "xlat d7", "xlatb d7", "lahf 9f", "sahf 9e",
  /* arithmetic */
  "@alu add 0", "@alu or 1", "@alu adc 2", "@alu sbb 3", "@alu and 4", "@alu sub 5", "@alu xor 6", "@alu cmp 7",
  "test 84 /r W V s=bwl m:RM r:R",
  "test a8 W s=bwl i:IX -:ACC",
  "test f6 /0 W s=bwl i:IANY m:RM",
  "inc 40 S s=wl o:R", "inc fe /0 W s=bwl m:RM",
  "dec 48 S s=wl o:R", "dec fe /1 W s=bwl m:RM",
  "not f6 /2 W s=bwl m:RM", "neg f6 /3 W s=bwl m:RM", "mul f6 /4 W s=bwl m:RM",
  "imul f6 /5 W s=bwl m:RM",
  "imul 0f,af /r s=wl m:RM r:R",
  "imul 6b /r s=wl i:IBS m:RM r:R", "imul 69 /r s=wl i:I m:RM r:R",
  "imul 6b /r s=wl i:IBS mr:R", "imul 69 /r s=wl i:I mr:R",
  "div f6 /6 W s=bwl m:RM", "idiv f6 /7 W s=bwl m:RM",
  "@shift rol 0", "@shift ror 1", "@shift rcl 2", "@shift rcr 3", "@shift shl 4", "@shift sal 4",
  "@shift shr 5", "@shift sar 7",
  "shld 0f,a4 /r s=wl i:IB r:R m:RM", "shld 0f,a5 /r s=wl -:CL r:R m:RM",
  "shrd 0f,ac /r s=wl i:IB r:R m:RM", "shrd 0f,ad /r s=wl -:CL r:R m:RM",
  "shld 0f,a5 /r s=wl r:R m:RM", "shrd 0f,ad /r s=wl r:R m:RM",
  "bt 0f,a3 /r s=wl r:R m:RM", "bt 0f,ba /4 s=wl dl i:IB m:RM",
  "bts 0f,ab /r s=wl r:R m:RM", "bts 0f,ba /5 s=wl dl i:IB m:RM",
  "btr 0f,b3 /r s=wl r:R m:RM", "btr 0f,ba /6 s=wl dl i:IB m:RM",
  "btc 0f,bb /r s=wl r:R m:RM", "btc 0f,ba /7 s=wl dl i:IB m:RM",
  "bsf 0f,bc /r s=wl m:RM r:R", "bsr 0f,bd /r s=wl m:RM r:R",
  "cmpxchg 0f,b0 /r W s=bwl r:R m:RM", "xadd 0f,c0 /r W s=bwl r:R m:RM",
  "cmpxchg8b 0f,c7 /1 m:M",
  "aaa 37", "aas 3f", "daa 27", "das 2f", "aam d4,0a", "aad d5,0a", "aam d4 i:IB", "aad d5 i:IB",
  "bound 62 /r s=wl r:R m:M", "arpl 63 /r r:R16 m:RM16",
  /* control transfer (relative forms are handled in code) */
  "jmp ff /4 s=wl dl m:JRM", "call ff /2 s=wl dl m:JRM",
  "ljmp ff /5 s=wl dl m:JM", "lcall ff /3 s=wl dl m:JM",
  "ljmp ea s=wl dl j:IW i:I", "lcall 9a s=wl dl j:IW i:I",
  "jmp ea s=wl dl j:IW i:I", "call 9a s=wl dl j:IW i:I",
  "ret c3 s=wl", "ret c2 s=wl i:IW", "lret cb s=wl", "lret ca s=wl i:IW",
  "iret cf s=wl", "int cd i:IB", "int3 cc", "into ce",
  "enter c8 s=wl i:IW j:IB", "leave c9 s=wl",
  /* strings and i/o */
  "movsb a4", "movsw a5 P16", "movsl a5", "movsd a5", "cmpsb a6", "cmpsw a7 P16", "cmpsl a7", "cmpsd a7",
  "stosb aa", "stosw ab P16", "stosl ab", "stosd ab", "lodsb ac", "lodsw ad P16", "lodsl ad", "lodsd ad",
  "scasb ae", "scasw af P16", "scasl af", "scasd af", "insb 6c", "insw 6d P16", "insl 6d", "insd 6d",
  "outsb 6e", "outsw 6f P16", "outsl 6f", "outsd 6f",
  "in e4 W s=bwl i:IB -:ACC", "in ec W s=bwl -:DX -:ACC",
  "out e6 W s=bwl -:ACC i:IB", "out ee W s=bwl -:ACC -:DX",
  /* prefixes as statements */
  "rep f3", "repe f3", "repz f3", "repne f2", "repnz f2", "lock f0", "data16 66", "addr16 67",
  "data32 66", "addr32 67", "wait 9b", "fwait 9b",
  /* flags and misc */
  "nop 90", "hlt f4", "cmc f5", "clc f8", "stc f9", "cli fa", "sti fb", "cld fc", "std fd",
  "clts 0f,06", "invd 0f,08", "wbinvd 0f,09", "ud2 0f,0b", "cpuid 0f,a2", "rdtsc 0f,31",
  "rdmsr 0f,32", "wrmsr 0f,30", "rdpmc 0f,33", "rsm 0f,aa", "sysenter 0f,34", "sysexit 0f,35",
  "emms 0f,77", "femms 0f,0e", "pause f3,90", "sfence 0f,ae", "lfence 0f,ae", "mfence 0f,ae",
  "monitor 0f,01", "mwait 0f,01",
  /* system */
  "lgdt 0f,01 /2 m:M", "sgdt 0f,01 /0 m:M", "lidt 0f,01 /3 m:M", "sidt 0f,01 /1 m:M",
  "lldt 0f,00 /2 m:RM16", "sldt 0f,00 /0 s=wl dl NPM m:RM", "ltr 0f,00 /3 m:RM16", "str 0f,00 /1 s=wl dl NPM m:RM",
  "verr 0f,00 /4 m:RM16", "verw 0f,00 /5 m:RM16", "lmsw 0f,01 /6 m:RM16", "smsw 0f,01 /4 s=wl dl NPM m:RM",
  "invlpg 0f,01 /7 m:M", "lar 0f,02 /r s=wl m:RM r:R", "lsl 0f,03 /r s=wl m:RM r:R",
  "clflush 0f,ae /7 m:M", "prefetchnta 0f,18 /0 m:M", "prefetcht0 0f,18 /1 m:M",
  "prefetcht1 0f,18 /2 m:M", "prefetcht2 0f,18 /3 m:M", "movnti 0f,c3 /r s=wl NP r:R m:M",
  "fxsave 0f,ae /0 m:M", "fxrstor 0f,ae /1 m:M", "ldmxcsr 0f,ae /2 m:M", "stmxcsr 0f,ae /3 m:M",
  NULL
};

static const char *xtab_fpu[] = {
  "fld d9 /0 m:M", "fld d9,c0 S o:STI", "fldt db /5 m:M", "fst d9 /2 m:M", "fst dd,d0 S o:STI",
  "fstp d9 /3 m:M", "fstp dd,d8 S o:STI", "fstpt db /7 m:M",
  "fild df /0 m:M", "filds df /0 m:M", "fildl db /0 m:M", "fildq df /5 m:M", "fildll df /5 m:M",
  "fist df /2 m:M", "fists df /2 m:M", "fistl db /2 m:M",
  "fistp df /3 m:M", "fistps df /3 m:M", "fistpl db /3 m:M", "fistpq df /7 m:M", "fistpll df /7 m:M",
  "fisttp df /1 m:M", "fisttps df /1 m:M", "fisttpl db /1 m:M", "fisttpll dd /1 m:M", "fisttpq dd /1 m:M",
  "@fari add 0", "@fari mul 1", "@fari com 2", "@fari comp 3", "@fari sub 4", "@fari subr 5",
  "@fari div 6", "@fari divr 7",
  "fcom d8,d1", "fcomp d8,d9", "fcompp de,d9", "fucom dd,e0 S o:STI", "fucom dd,e1",
  "fucomp dd,e8 S o:STI", "fucomp dd,e9", "fucompp da,e9",
  "fcomi db,f0 S o:STI -:ST0", "fcomip df,f0 S o:STI -:ST0", "fucomi db,e8 S o:STI -:ST0",
  "fucomip df,e8 S o:STI -:ST0", "fcomi db,f0 S o:STI", "fcomip df,f0 S o:STI", "fucomi db,e8 S o:STI",
  "fucomip df,e8 S o:STI", "fcomi db,f1", "fcomip df,f1", "fucomi db,e9", "fucomip df,e9",
  "faddp de,c0 S o:STI", "fmulp de,c8 S o:STI", "fsubp de,e0 S o:STI", "fsubrp de,e8 S o:STI",
  "fdivp de,f0 S o:STI", "fdivrp de,f8 S o:STI",
  "faddp de,c0 S -:ST0 o:STI", "faddp de,c1", "fmulp de,c8 S -:ST0 o:STI", "fmulp de,c9",
  "fsubp de,e0 S -:ST0 o:STI", "fsubp de,e1", "fsubrp de,e8 S -:ST0 o:STI", "fsubrp de,e9",
  "fdivp de,f0 S -:ST0 o:STI", "fdivp de,f1", "fdivrp de,f8 S -:ST0 o:STI", "fdivrp de,f9",
  "fxch d9,c8 S o:STI", "fxch d9,c9", "ffree dd,c0 S o:STI", "ffreep df,c0 S o:STI",
  "fcmovb da,c0 S o:STI -:ST0", "fcmove da,c8 S o:STI -:ST0", "fcmovbe da,d0 S o:STI -:ST0",
  "fcmovu da,d8 S o:STI -:ST0", "fcmovnb db,c0 S o:STI -:ST0", "fcmovne db,c8 S o:STI -:ST0",
  "fcmovnbe db,d0 S o:STI -:ST0", "fcmovnu db,d8 S o:STI -:ST0",
  "fldz d9,ee", "fld1 d9,e8", "fldpi d9,eb", "fldl2t d9,e9", "fldl2e d9,ea", "fldlg2 d9,ec",
  "fldln2 d9,ed", "fchs d9,e0", "fabs d9,e1", "ftst d9,e4", "fxam d9,e5", "f2xm1 d9,f0",
  "fyl2x d9,f1", "fptan d9,f2", "fpatan d9,f3", "fxtract d9,f4", "fprem1 d9,f5", "fdecstp d9,f6",
  "fincstp d9,f7", "fprem d9,f8", "fyl2xp1 d9,f9", "fsqrt d9,fa", "fsincos d9,fb", "frndint d9,fc",
  "fscale d9,fd", "fsin d9,fe", "fcos d9,ff", "fnop d9,d0", "fninit db,e3", "finit db,e3",
  "fnclex db,e2", "fclex db,e2", "fnstsw df,e0 -:AX", "fstsw df,e0 -:AX", "fnstsw df,e0", "fstsw df,e0", "fnstsw dd /7 m:M",
  "fstsw dd /7 m:M", "fstcw d9 /7 m:M", "fnstcw d9 /7 m:M", "fldcw d9 /5 m:M", "fstenv d9 /6 m:M",
  "fnstenv d9 /6 m:M", "fldenv d9 /4 m:M", "fsave dd /6 m:M", "fnsave dd /6 m:M", "frstor dd /4 m:M",
  "fbld df /4 m:M", "fbstp df /6 m:M",
  /* MMX / SSE / SSE2 */
  "movd 0f,6e /r m:R32M r:MMX", "movd 0f,7e /r r:MMX m:R32M",
  "movd 66,0f,6e /r m:R32M r:XMM", "movd 66,0f,7e /r r:XMM m:R32M",
  "movq 0f,6f /r m:MMXM r:MMX", "movq 0f,7f /r r:MMX m:MMXM",
  "movq f3,0f,7e /r m:XMMM r:XMM", "movq 66,0f,d6 /r r:XMM m:XMMM",
  "@mmx punpcklbw 60", "@mmx punpcklwd 61", "@mmx punpckldq 62", "@mmx packsswb 63", "@mmx pcmpgtb 64",
  "@mmx pcmpgtw 65", "@mmx pcmpgtd 66", "@mmx packuswb 67", "@mmx punpckhbw 68", "@mmx punpckhwd 69",
  "@mmx punpckhdq 6a", "@mmx packssdw 6b", "@mmx pcmpeqb 74", "@mmx pcmpeqw 75", "@mmx pcmpeqd 76",
  "@mmx psrlw d1", "@mmx psrld d2", "@mmx psrlq d3", "@mmx paddq d4", "@mmx pmullw d5", "@mmx psubusb d8",
  "@mmx psubusw d9", "@mmx pminub da", "@mmx pand db", "@mmx paddusb dc", "@mmx paddusw dd",
  "@mmx pmaxub de", "@mmx pandn df", "@mmx pavgb e0", "@mmx psraw e1", "@mmx psrad e2", "@mmx pavgw e3",
  "@mmx pmulhuw e4", "@mmx pmulhw e5", "@mmx psubsb e8", "@mmx psubsw e9", "@mmx pminsw ea", "@mmx por eb",
  "@mmx paddsb ec", "@mmx paddsw ed", "@mmx pmaxsw ee", "@mmx pxor ef", "@mmx psllw f1", "@mmx pslld f2",
  "@mmx psllq f3", "@mmx pmuludq f4", "@mmx pmaddwd f5", "@mmx psadbw f6", "@mmx psubb f8", "@mmx psubw f9",
  "@mmx psubd fa", "@mmx psubq fb", "@mmx paddb fc", "@mmx paddw fd", "@mmx paddd fe",
  "psrlw 0f,71 /2 i:IB m:MMX", "psraw 0f,71 /4 i:IB m:MMX", "psllw 0f,71 /6 i:IB m:MMX",
  "psrld 0f,72 /2 i:IB m:MMX", "psrad 0f,72 /4 i:IB m:MMX", "pslld 0f,72 /6 i:IB m:MMX",
  "psrlq 0f,73 /2 i:IB m:MMX", "psllq 0f,73 /6 i:IB m:MMX",
  "psrlw 66,0f,71 /2 i:IB m:XMM", "psraw 66,0f,71 /4 i:IB m:XMM", "psllw 66,0f,71 /6 i:IB m:XMM",
  "psrld 66,0f,72 /2 i:IB m:XMM", "psrad 66,0f,72 /4 i:IB m:XMM", "pslld 66,0f,72 /6 i:IB m:XMM",
  "psrlq 66,0f,73 /2 i:IB m:XMM", "psllq 66,0f,73 /6 i:IB m:XMM",
  "pmovmskb 66,0f,d7 /r m:XMM r:R32", "pinsrw 66,0f,c4 /r i:IB m:R32M r:XMM",
  "pextrw 66,0f,c5 /r i:IB m:XMM r:R32",
  "pshufw 0f,70 /r i:IB m:MMXM r:MMX", "pshufd 66,0f,70 /r i:IB m:XMMM r:XMM",
  "pshufhw f3,0f,70 /r i:IB m:XMMM r:XMM", "pshuflw f2,0f,70 /r i:IB m:XMMM r:XMM",
  "@sse movups 10 11", "@sse movaps 28 29", "@sse movupd 66 10 11", "@sse movapd 66 28 29",
  "@sse movss f3 10 11", "@sse movsd f2 10 11", "@sse movdqa 66 6f 7f", "@sse movdqu f3 6f 7f",
  "@ps add 58", "@ps mul 59", "@ps sub 5c", "@ps min 5d", "@ps div 5e", "@ps max 5f", "@ps sqrt 51",
  "@psx rsqrt 52", "@psx rcp 53",
  "andps 0f,54 /r m:XMMM r:XMM", "andnps 0f,55 /r m:XMMM r:XMM", "orps 0f,56 /r m:XMMM r:XMM",
  "xorps 0f,57 /r m:XMMM r:XMM", "andpd 66,0f,54 /r m:XMMM r:XMM", "andnpd 66,0f,55 /r m:XMMM r:XMM",
  "orpd 66,0f,56 /r m:XMMM r:XMM", "xorpd 66,0f,57 /r m:XMMM r:XMM",
  "unpcklps 0f,14 /r m:XMMM r:XMM", "unpckhps 0f,15 /r m:XMMM r:XMM",
  "unpcklpd 66,0f,14 /r m:XMMM r:XMM", "unpckhpd 66,0f,15 /r m:XMMM r:XMM",
  "comiss 0f,2f /r m:XMMM r:XMM", "ucomiss 0f,2e /r m:XMMM r:XMM",
  "comisd 66,0f,2f /r m:XMMM r:XMM", "ucomisd 66,0f,2e /r m:XMMM r:XMM",
  "cmpps 0f,c2 /r i:IB m:XMMM r:XMM", "cmpss f3,0f,c2 /r i:IB m:XMMM r:XMM",
  "cmppd 66,0f,c2 /r i:IB m:XMMM r:XMM", "cmpsd f2,0f,c2 /r i:IB m:XMMM r:XMM",
  "shufps 0f,c6 /r i:IB m:XMMM r:XMM", "shufpd 66,0f,c6 /r i:IB m:XMMM r:XMM",
  "cvtsi2ss f3,0f,2a /r m:R32M r:XMM", "cvtsi2sd f2,0f,2a /r m:R32M r:XMM",
  "cvtss2si f3,0f,2d /r m:XMMM r:R32", "cvttss2si f3,0f,2c /r m:XMMM r:R32",
  "cvtsd2si f2,0f,2d /r m:XMMM r:R32", "cvttsd2si f2,0f,2c /r m:XMMM r:R32",
  "cvtpi2ps 0f,2a /r m:MMXM r:XMM", "cvtps2pi 0f,2d /r m:XMMM r:MMX", "cvttps2pi 0f,2c /r m:XMMM r:MMX",
  "cvtps2pd 0f,5a /r m:XMMM r:XMM", "cvtpd2ps 66,0f,5a /r m:XMMM r:XMM",
  "cvtss2sd f3,0f,5a /r m:XMMM r:XMM", "cvtsd2ss f2,0f,5a /r m:XMMM r:XMM",
  "cvtdq2ps 0f,5b /r m:XMMM r:XMM", "cvtps2dq 66,0f,5b /r m:XMMM r:XMM",
  "movhlps 0f,12 /r m:XMM r:XMM", "movlhps 0f,16 /r m:XMM r:XMM",
  "movlps 0f,12 /r m:M r:XMM", "movlps 0f,13 /r r:XMM m:M", "movhps 0f,16 /r m:M r:XMM",
  "movhps 0f,17 /r r:XMM m:M", "movmskps 0f,50 /r m:XMM r:R32", "movntps 0f,2b /r r:XMM m:M",
  "movntq 0f,e7 /r r:MMX m:M", "maskmovq 0f,f7 /r m:MMX r:MMX", "pmovmskb 0f,d7 /r m:MMX r:R32",
  "pextrw 0f,c5 /r i:IB m:MMX r:R32", "pinsrw 0f,c4 /r i:IB m:R32M r:MMX",
  "movdq2q f2,0f,d6 /r m:XMM r:MMX", "movq2dq f3,0f,d6 /r m:MMX r:XMM",
  "@cmpps eq 0", "@cmpps lt 1", "@cmpps le 2", "@cmpps unord 3", "@cmpps neq 4", "@cmpps nlt 5",
  "@cmpps nle 6", "@cmpps ord 7",
  "pfadd 0f,0f /r X=6 m:MMXM r:MMX", "pfsub 0f,0f /r X=2 m:MMXM r:MMX", "pfmul 0f,0f /r X=4 m:MMXM r:MMX",
  NULL
};

/* operand classes in templates */
enum { C_NONE, C_R, C_RM, C_M, C_ACC, C_I, C_IBS, C_IB, C_IW, C_CL, C_DX, C_AX, C_R8, C_R16, C_R32,
       C_RM8, C_RM16, C_R32M, C_SEG, C_ES, C_CS, C_SS, C_DS, C_FS, C_GS, C_CR, C_DR, C_TR, C_ST0,
       C_STI, C_MMX, C_MMXM, C_XMM, C_XMMM, C_MOFFS, C_JRM, C_JM, C_IX, C_IANY };
static const char *cnames[] = { "", "R", "RM", "M", "ACC", "I", "IBS", "IB", "IW", "CL", "DX", "AX",
  "R8", "R16", "R32", "RM8", "RM16", "R32M", "SEG", "ES", "CS", "SS", "DS", "FS", "GS", "CR", "DR",
  "TR", "ST0", "STI", "MMX", "MMXM", "XMM", "XMMM", "MOFFS", "JRM", "JM", "IX", "IANY", NULL };
enum { RO_NONE, RO_REG, RO_RM, RO_IMM, RO_OP, RO_MR };

#define F_W   1
#define F_W3  2
#define F_D   4
#define F_V   8
#define F_S   16
#define F_P16 32
#define F_DL  64
#define F_NP  128
#define F_NPM 256

typedef struct xt {
  char *name;
  uint8_t op[4]; int oplen;
  int ext;            /* -1, 0..7, 8 = /r */
  int flags, sfx, nops;
  int cls[3], role[3];
  int bugx;           /* as vasm does: memory form puts this in modrm.reg and the register in rm */
  struct xt *next;
} xt;

#define XH 2048
static xt *xhash[XH];
static unsigned xhf(const char *s, size_t n)
{
  unsigned h = 5381;
  while (n--) h = h * 33 + (unsigned char)tolower((unsigned char)*s++);
  return h & (XH - 1);
}
static xt *xfirst(const char *s, size_t n)
{
  xt *t;
  for (t = xhash[xhf(s, n)]; t; t = t->next)
    if (strlen(t->name) == n && !strnicmp_(t->name, s, n)) return t;
  return NULL;
}

static char *next_tok(char **pp)
{
  char *p = *pp, *t;
  while (*p == ' ') p++;
  if (!*p) return NULL;
  t = p;
  while (*p && *p != ' ') p++;
  if (*p) *p++ = 0;
  *pp = p;
  return t;
}

static void add_template(const char *line)
{
  xt *t = xcalloc(1, sizeof(*t)), **pp;
  char *s = xstrdup(line), *tok, *save = s;
  unsigned h;
  int k = 0;
  t->ext = -1;
  t->bugx = -1;
  tok = next_tok(&save);
  t->name = xstrdup(tok);
  tok = next_tok(&save);
  {
    char *b = tok;
    while (*b) { t->op[t->oplen++] = (uint8_t)strtoul(b, &b, 16); if (*b == ',') b++; }
  }
  while ((tok = next_tok(&save))) {
    char *c = strchr(tok, ':');
    if (tok[0] == '/') { t->ext = tok[1] == 'r' ? 8 : tok[1] - '0'; continue; }
    if (!strcmp(tok, "W")) t->flags |= F_W;
    else if (!strcmp(tok, "W3")) t->flags |= F_W3;
    else if (!strcmp(tok, "D")) t->flags |= F_D;
    else if (!strcmp(tok, "V")) t->flags |= F_V;
    else if (!strcmp(tok, "S")) t->flags |= F_S;
    else if (!strcmp(tok, "P16")) t->flags |= F_P16;
    else if (!strcmp(tok, "dl")) t->flags |= F_DL;
    else if (!strcmp(tok, "NP")) t->flags |= F_NP;
    else if (!strcmp(tok, "NPM")) t->flags |= F_NPM;
    else if (!strncmp(tok, "X=", 2)) t->bugx = atoi(tok + 2);
    else if (!strncmp(tok, "s=", 2)) {
      char *q;
      for (q = tok + 2; *q; q++) t->sfx |= *q == 'b' ? 1 : *q == 'w' ? 2 : *q == 'l' ? 4 : 8;
    } else if (c && k < 3) {
      int i;
      *c = 0;
      t->role[k] = !strcmp(tok, "r") ? RO_REG : !strcmp(tok, "m") ? RO_RM : !strcmp(tok, "mr") ? RO_MR :
                   (!strcmp(tok, "i") || !strcmp(tok, "j")) ? RO_IMM : !strcmp(tok, "o") ? RO_OP : RO_NONE;
      for (i = 1; cnames[i]; i++) if (!strcmp(cnames[i], c + 1)) break;
      if (!cnames[i]) fatal("x86 table: bad class %s", c + 1);
      t->cls[k++] = i;
    } else
      fatal("x86 table: bad token %s in %s", tok, line);
  }
  t->nops = k;
  h = xhf(t->name, strlen(t->name));
  for (pp = &xhash[h]; *pp; pp = &(*pp)->next) ;   /* keep table order */
  *pp = t;
  free(s);
}

static void addf(const char *fmt, ...)
{
  char b[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(b, sizeof b, fmt, ap);
  va_end(ap);
  add_template(b);
}

static const char *ccn[16][4] = {
  { "o" }, { "no" }, { "b", "c", "nae" }, { "ae", "nb", "nc" }, { "e", "z" }, { "ne", "nz" },
  { "be", "na" }, { "a", "nbe" }, { "s" }, { "ns" }, { "p", "pe" }, { "np", "po" }, { "l", "nge" },
  { "ge", "nl" }, { "le", "ng" }, { "g", "nle" } };

static void build_tables(void)
{
  int i, c, k;
  const char **tabs[2] = { xtab_src, xtab_fpu };
  for (k = 0; k < 2; k++)
    for (i = 0; tabs[k][i]; i++) {
      const char *l = tabs[k][i];
      char n[32], a[8], b[8], d[8];
      int v;
      if (l[0] != '@') { add_template(l); continue; }
      a[0] = b[0] = d[0] = 0;
      if (!strncmp(l, "@alu ", 5) && sscanf(l + 5, "%31s %d", n, &v) == 2) {
        addf("%s %02x /r W D s=bwl r:R m:RM", n, v * 8);
        addf("%s 83 /%d s=wl dl i:IBS m:RM", n, v);
        addf("%s %02x W s=bwl i:I -:ACC", n, v * 8 + 4);
        addf("%s 80 /%d W s=bwl i:IANY m:RM", n, v);
      } else if (!strncmp(l, "@shift ", 7) && sscanf(l + 7, "%31s %d", n, &v) == 2) {
        addf("%s d0 /%d W s=bwl m:RM", n, v);
        addf("%s c0 /%d W s=bwl i:IB m:RM", n, v);
        addf("%s d2 /%d W s=bwl -:CL m:RM", n, v);
      } else if (!strncmp(l, "@fari ", 6) && sscanf(l + 6, "%31s %d", n, &v) == 2) {
        addf("f%s d8 /%d m:M", n, v);
        if (v == 2 || v == 3) addf("f%s d8,%02x S o:STI", n, 0xc0 + 8 * v);
        else {
          addf("f%s d8,%02x S o:STI -:ST0", n, 0xc0 + 8 * v);
          addf("f%s d8,%02x S o:STI", n, 0xc0 + 8 * v);
          addf("f%s de,%02x", n, 0xc1 + 8 * v);     /* vasm: no operand = the pop form */
        }
        addf("fi%s de /%d m:M", n, v);
        addf("fi%ss de /%d m:M", n, v);
        addf("fi%sl da /%d m:M", n, v);
      } else if (!strncmp(l, "@mmx ", 5) && sscanf(l + 5, "%31s %7s", n, a) == 2) {
        addf("%s 0f,%s /r m:MMXM r:MMX", n, a);
        addf("%s 66,0f,%s /r m:XMMM r:XMM", n, a);
      } else if (!strncmp(l, "@sse ", 5)) {
        int nn = sscanf(l + 5, "%31s %7s %7s %7s", n, a, b, d);
        if (nn == 3) {
          addf("%s 0f,%s /r m:XMMM r:XMM", n, a);
          addf("%s 0f,%s /r r:XMM m:XMMM", n, b);
        } else {
          addf("%s %s,0f,%s /r m:XMMM r:XMM", n, a, b);
          addf("%s %s,0f,%s /r r:XMM m:XMMM", n, a, d);
        }
      } else if ((!strncmp(l, "@ps ", 4) || !strncmp(l, "@psx ", 5)) && sscanf(strchr(l, ' ') + 1, "%31s %7s", n, a) == 2) {
        addf("%sps 0f,%s /r m:XMMM r:XMM", n, a);
        addf("%sss f3,0f,%s /r m:XMMM r:XMM", n, a);
        if (l[3] == ' ') {
          addf("%spd 66,0f,%s /r m:XMMM r:XMM", n, a);
          addf("%ssd f2,0f,%s /r m:XMMM r:XMM", n, a);
        }
      } else if (!strncmp(l, "@cmpps ", 7) && sscanf(l + 7, "%31s %d", n, &v) == 2) {
        addf("cmp%sps 0f,c2 /r X=%d m:XMMM r:XMM", n, v);
        addf("cmp%sss f3,0f,c2 /r X=%d m:XMMM r:XMM", n, v);
        addf("cmp%spd 66,0f,c2 /r X=%d m:XMMM r:XMM", n, v);
        addf("cmp%ssd f2,0f,c2 /r X=%d m:XMMM r:XMM", n, v);
      } else
        fatal("x86 table: bad macro %s", l);
    }
  for (c = 0; c < 16; c++)
    for (k = 0; k < 4 && ccn[c][k]; k++) {
      addf("set%s 0f,%02x /0 m:RM8", ccn[c][k], 0x90 + c);
      addf("cmov%s 0f,%02x /r s=wl m:RM r:R", ccn[c][k], 0x40 + c);
    }
}

/* ------------------------------------------------------------ matching */
static int fits(ival v, int bits)
{
  if (bits == 8) return v >= -128 && v <= 255;
  if (bits == 16) return v >= -32768 && v <= 65535;
  return v >= -(ival)0x80000000LL && v <= (ival)0xffffffffLL;
}

static int imm_const(xopnd *o, ival *v)
{
  eres r;
  eval_expr(o->e, &r);
  if (r.isflt) { r.v = (ival)r.f; }
  *v = r.v;
  return r.defined && !r.sec && !r.ext && !r.bad;
}

/* lax: register size may differ from the operation size (vasm takes
 * the size from the last register operand and converts the others) */
static int cls_ok(int c, xopnd *o, int sz, int lax)
{
  int gpr = o->kind == K_REG && o->r.cls == RC_GPR;
  int mem = o->kind == K_MEM;
  int reg = o->kind == K_REG;
  ival v;
  if (o->abs && c != C_JRM && c != C_JM) return 0;
  switch (c) {
  case C_R: return gpr && (lax || o->r.size == sz);
  case C_RM: return (gpr && (lax || o->r.size == sz)) || mem;
  case C_M: return mem;
  case C_ACC: return gpr && (lax || o->r.size == sz) && o->r.num == 0;
  case C_I: if (o->kind != K_IMM) return 0; return !imm_const(o, &v) || fits(v, sz == 8 ? 8 : 32);
  case C_IX: if (o->kind != K_IMM) return 0; return !imm_const(o, &v) || fits(v, sz == 64 ? 32 : sz);
  case C_IANY: return o->kind == K_IMM;
  case C_IBS: return o->kind == K_IMM && sz != 8 && imm_const(o, &v) && v >= -128 && v <= 127;
  case C_IB: case C_IW: return o->kind == K_IMM;
  case C_CL: return gpr && o->r.size == 8 && o->r.num == 1;
  case C_DX: return gpr && o->r.size == 16 && o->r.num == 2;
  case C_AX: return gpr && o->r.size == 16 && o->r.num == 0;
  case C_R8: return gpr && o->r.size == 8;
  case C_R16: return gpr && o->r.size == 16;
  case C_R32: return gpr && o->r.size == 32;
  case C_RM8: return (gpr && o->r.size == 8) || mem;
  case C_RM16: return (gpr && o->r.size == 16) || mem;
  case C_R32M: return (gpr && o->r.size == 32) || mem;
  case C_SEG: return reg && o->r.cls == RC_SEG;
  case C_ES: case C_CS: case C_SS: case C_DS: case C_FS: case C_GS:
    return reg && o->r.cls == RC_SEG && o->r.num == c - C_ES;
  case C_CR: return reg && o->r.cls == RC_CR;
  case C_DR: return reg && o->r.cls == RC_DR;
  case C_TR: return reg && o->r.cls == RC_TR;
  case C_ST0: return reg && o->r.cls == RC_ST && o->r.num == 0;
  case C_STI: return reg && o->r.cls == RC_ST;
  case C_MMX: return reg && o->r.cls == RC_MMX;
  case C_MMXM: return (reg && o->r.cls == RC_MMX) || mem;
  case C_XMM: return reg && o->r.cls == RC_XMM;
  case C_XMMM: return (reg && o->r.cls == RC_XMM) || mem;
  case C_MOFFS: return mem && o->base < 0 && o->index < 0;
  case C_JRM: return o->abs && ((gpr && (lax || o->r.size == sz)) || mem);
  case C_JM: return o->abs && mem;
  }
  return 0;
}

/* sized classes: their register operands define the operation size */
static int sized(int c) { return c == C_R || c == C_RM || c == C_ACC || c == C_JRM || c == C_CL; }

typedef struct xmatch { xt *t; int sz; int swap; } xmatch;

static int try_match(xt *t, xopnd *ops, int nops, int sfx, int swap, xmatch *m)
{
  int i, sz = 0;
  if (t->nops != nops) return 0;
  if (sfx) { if (!(t->sfx & sfx)) return 0; sz = sfx == 1 ? 8 : sfx == 2 ? 16 : sfx == 4 ? 32 : 64; }
  else if (t->sfx) {
    for (i = 0; i < nops; i++) {
      xopnd *o = &ops[swap ? nops - 1 - i : i];
      if (sized(t->cls[i]) && o->kind == K_REG && o->r.cls == RC_GPR)
        sz = o->r.size;      /* the last one wins */
    }
    if (!sz) {
      int need = 0;
      for (i = 0; i < nops; i++)
        if (t->cls[i] == C_RM || t->cls[i] == C_I || t->cls[i] == C_IBS || t->cls[i] == C_M || t->cls[i] == C_JRM || t->cls[i] == C_JM) need = 1;
      if (need && !(t->flags & F_DL)) return 0;
      sz = cmode == 16 ? 16 : 32;
    }
    if (!(t->sfx & (sz == 8 ? 1 : sz == 16 ? 2 : sz == 32 ? 4 : 8))) return 0;
  }
  for (i = 0; i < nops; i++)
    if (!cls_ok(t->cls[i], &ops[swap ? nops - 1 - i : i], sz, t->role[i] != RO_OP)) return 0;
  m->t = t; m->sz = sz; m->swap = swap;
  return 1;
}

static int find_match(xt *t, xopnd *ops, int nops, int sfx, xmatch *m)
{
  const char *name = t->name;
  size_t nl = strlen(name);
  for (; t; t = t->next) {
    if (strlen(t->name) != nl || stricmp_(t->name, name)) continue;
    if (try_match(t, ops, nops, sfx, 0, m)) return 1;
    if ((t->flags & (F_D | F_V)) && nops == 2 && try_match(t, ops, nops, sfx, 1, m)) return 1;
  }
  return 0;
}

/* ------------------------------------------------------------ instructions */
enum { J_NONE, J_JMP, J_JCC, J_CALL, J_LOOP };
typedef struct xinsn {
  xt *first;
  int sfx, nops, mode;
  xopnd ops[3];
  int jkind, cc;
  uint8_t jop;
} xinsn;

static int is_rel(xopnd *o)
{
  return o->kind == K_MEM && !o->abs && o->base < 0 && o->index < 0 && o->seg < 0 && o->e;
}

static void put_le(bbuf *b, uval v, int n)
{
  int i;
  for (i = 0; i < n; i++) bb_byte(b, (int)(v >> (8 * i)));
}

/* value of an expression field, with relocation when enc */
static void field(bbuf *b, expr *e, int bytes, section *s, ival pc, int enc, int pcrel)
{
  eres r;
  eval_expr(e, &r);
  if (r.isflt) r.v = (ival)r.f;
  if (r.sec || r.ext) {
    if (enc) {
      if (r.bad) error("illegal relocation");
      add_reloc(s, pc + (ival)b->n, bytes * 8, pcrel ? R_PC : R_ABS, r.sec, r.ext, r.v);
    }
  } else if (enc && bytes < 4 && r.defined && !fits(r.v, bytes * 8))
    warning("operand value out of range");
  put_le(b, (uval)r.v, bytes);
}

static int disp_small(xopnd *o, ival *v)
{
  eres r;
  if (!o->hasdisp) { *v = 0; return 1; }
  eval_expr(o->e, &r);
  *v = r.v;
  return r.defined && !r.sec && !r.ext && !r.bad && r.v >= -128 && r.v <= 127;
}

static void modrm_mem(bbuf *b, int reg, xopnd *o, section *s, ival pc, int enc)
{
  ival dv;
  int small = disp_small(o, &dv);
  int asz = o->asize ? o->asize : cmode;
  if (asz == 16) {
    int rm = -1, mod;
    int bs = o->base, ix = o->index;
    if (bs >= 0 && ix >= 0) {
      if ((bs == 3 || bs == 5) && (ix == 6 || ix == 7)) rm = (bs == 5 ? 2 : 0) + (ix == 7);
      else if ((ix == 3 || ix == 5) && (bs == 6 || bs == 7)) rm = (ix == 5 ? 2 : 0) + (bs == 7);
    } else if (bs >= 0 || ix >= 0) {
      int x = bs >= 0 ? bs : ix;
      rm = x == 6 ? 4 : x == 7 ? 5 : x == 5 ? 6 : x == 3 ? 7 : -1;
    } else {
      bb_byte(b, (reg << 3) | 6);
      field(b, o->e, 2, s, pc, enc, 0);
      return;
    }
    if (rm < 0) { if (enc) error("illegal 16-bit addressing mode"); rm = 0; }
    if (!o->hasdisp && rm != 6) mod = 0;
    else if (small) mod = 1;
    else mod = 2;
    bb_byte(b, (mod << 6) | (reg << 3) | rm);
    if (mod == 1) { if (o->hasdisp) field(b, o->e, 1, s, pc, enc, 0); else bb_byte(b, 0); }
    else if (mod == 2) field(b, o->e, 4, s, pc, enc, 0);   /* vasm: 32-bit displacement */
    return;
  }
  if (o->base < 0 && o->index < 0) {
    bb_byte(b, (reg << 3) | 5);
    field(b, o->e, 4, s, pc, enc, 0);
    return;
  }
  {
    int sib = o->index >= 0 || o->base == 4;
    int mod;
    if (o->base < 0) mod = 0;
    else if (!o->hasdisp && o->base != 5) mod = 0;
    else if (small) mod = 1;
    else mod = 2;
    bb_byte(b, (mod << 6) | (reg << 3) | (sib ? 4 : o->base));
    if (sib) {
      int ss = o->scale == 1 ? 0 : o->scale == 2 ? 1 : o->scale == 4 ? 2 : 3;
      bb_byte(b, (ss << 6) | ((o->index >= 0 ? o->index : 4) << 3) | (o->base >= 0 ? o->base : 5));
    }
    if (o->base < 0) { if (o->e) field(b, o->e, 4, s, pc, enc, 0); else put_le(b, 0, 4); }
    else if (mod == 1) { if (o->hasdisp) field(b, o->e, 1, s, pc, enc, 0); else bb_byte(b, 0); }
    else if (mod == 2) field(b, o->e, 4, s, pc, enc, 0);
  }
}

static int regnum(xopnd *o) { return o->r.num & 7; }

static void emit_template(xinsn *ip, xmatch *m, section *s, ival pc, bbuf *b, int enc)
{
  xt *t = m->t;
  xopnd *op[3];
  int i, opidx = -1, regop = -1, rmop = -1;
  uint8_t code[4];
  static const uint8_t segpfx[6] = { 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65 };
  for (i = 0; i < t->nops; i++) op[i] = &ip->ops[m->swap ? t->nops - 1 - i : i];
  for (i = 0; i < t->nops; i++) {
    if (t->role[i] == RO_REG) regop = i;
    else if (t->role[i] == RO_RM) rmop = i;
    else if (t->role[i] == RO_MR) regop = rmop = i;
    else if (t->role[i] == RO_OP) opidx = i;
  }
  /* prefixes: address size, operand size, segment */
  for (i = 0; i < t->nops; i++)
    if (op[i]->kind == K_MEM && op[i]->asize && op[i]->asize != ip->mode) { bb_byte(b, 0x67); break; }
  if (!(t->flags & F_NP) && !((t->flags & F_NPM) && rmop >= 0 && op[rmop]->kind == K_MEM)) {
    int sized_t = (t->sfx & 2) && (t->sfx & ~2);
    if (cmode == 16 ? (!(t->flags & F_P16) && sized_t && m->sz == 32)
                    : ((t->flags & F_P16) || (sized_t && m->sz == 16)))
      bb_byte(b, 0x66);
  }
  for (i = 0; i < t->nops; i++)
    if (op[i]->kind == K_MEM && op[i]->seg >= 0) {
      /* vasm: an override of the default segment is dropped */
      int bp = op[i]->asize == 16 ? (op[i]->base == 5 || (op[i]->base < 0 && op[i]->index == 5))
                                  : (op[i]->base == 4 || op[i]->base == 5);
      int def = bp ? 2 : 3;
      if (op[i]->seg != def) bb_byte(b, segpfx[op[i]->seg]);
      break;
    }
  memcpy(code, t->op, t->oplen);
  if ((t->flags & F_W) && m->sz != 8) code[t->oplen - 1] |= 1;
  if ((t->flags & F_W3) && m->sz != 8) code[t->oplen - 1] |= 8;
  if ((t->flags & F_D) && m->swap) code[t->oplen - 1] |= 2;
  if ((t->flags & F_S) && opidx >= 0) code[t->oplen - 1] += (uint8_t)regnum(op[opidx]);
  bb_put(b, code, t->oplen);
  if (t->ext >= 0 && rmop >= 0) {
    int reg = t->ext == 8 ? (regop >= 0 ? regnum(op[regop]) : 0) : t->ext;
    xopnd *o = op[rmop];
    if (o->kind == K_REG) bb_byte(b, 0xc0 | (reg << 3) | regnum(o));
    else if (t->bugx >= 0) {
      size_t at = b->n;
      modrm_mem(b, t->bugx, o, s, pc, enc);
      b->d[at] = (uint8_t)((b->d[at] & 0xf8) | reg);
    } else modrm_mem(b, reg, o, s, pc, enc);
  } else if (t->ext >= 0 && t->ext < 8)
    bb_byte(b, 0xc0 | (t->ext << 3));
  for (i = 0; i < t->nops; i++) {
    xopnd *o = &ip->ops[i];
    int ti = m->swap ? t->nops - 1 - i : i;
    if (t->role[ti] != RO_IMM) continue;
    switch (t->cls[ti]) {
    case C_I: case C_IX: case C_IANY: field(b, o->e, m->sz == 8 ? 1 : m->sz == 16 ? 2 : 4, s, pc, enc, 0); break;
    case C_IW: field(b, o->e, 2, s, pc, enc, 0); break;
    default: field(b, o->e, 1, s, pc, enc, 0); break;
    }
  }
  /* "mov moffs" has its displacement after the opcode */
  if (t->cls[0] == C_MOFFS) field(b, op[0]->e, cmode == 16 ? 2 : 4, s, pc, enc, 0);
  else if (t->nops > 1 && t->cls[1] == C_MOFFS) field(b, op[1]->e, cmode == 16 ? 2 : 4, s, pc, enc, 0);
}

/* jump displacement known within this section? */
static int jdist(xinsn *ip, section *s, ival pc, ival *target, eres *r)
{
  eval_expr(ip->ops[0].e, r);
  if (r->ext || r->bad) return 0;
  if (r->sec == s) { *target = r->v; return r->defined; }
  if (!r->sec && (s->flags & SEC_ABS)) { *target = r->v - s->org; return r->defined; }
  if (!r->sec) { *target = r->v; return r->defined; }   /* vasm: absolute = section offset */
  return 0;
}

static ival jump_size(xinsn *ip, section *s, ival pc)
{
  eres r;
  ival tg = 0, d;
  int known = jdist(ip, s, pc, &tg, &r);
  int w = ip->mode == 16 ? 2 : 4;
  if (ip->jkind == J_CALL) return 1 + w;
  if (ip->jkind == J_LOOP) return 2;
  if (!known && r.defined) return ip->jkind == J_JMP ? 1 + w : 2 + w;
  if (!known) return 2;     /* forward reference: assume short first */
  /* vasm: the range test uses the displacement of the long form */
  d = tg - (pc + (ip->jkind == J_JMP ? 1 + w : 2 + w));
  if (d >= -128 && d <= 127) return 2;
  return ip->jkind == J_JMP ? 1 + w : 2 + w;
}

static void emit_jump(xinsn *ip, section *s, ival pc, bbuf *b, ival size, int enc)
{
  eres r;
  ival tg = 0;
  int known = jdist(ip, s, pc, &tg, &r);
  if (ip->jkind == J_LOOP) {
    bb_byte(b, ip->jop);
    if (!known) { if (enc) error("pc-relative jump destination out of range"); bb_byte(b, 0); return; }
    tg -= pc + size;
    if (enc && (tg < -128 || tg > 127)) error("pc-relative jump destination out of range (%lld)", (long long)tg);
    bb_byte(b, (int)tg);
    return;
  }
  if (size == 2) {
    bb_byte(b, ip->jkind == J_JMP ? 0xeb : 0x70 + ip->cc);
    if (enc && (tg - (pc + 2) < -128 || tg - (pc + 2) > 127))
      error("pc-relative jump destination out of range (%lld)", (long long)(tg - (pc + 2)));
    bb_byte(b, (int)(tg - (pc + 2)));
    return;
  }
  if (ip->jkind == J_JMP) bb_byte(b, 0xe9);
  else if (ip->jkind == J_CALL) bb_byte(b, 0xe8);
  else { bb_byte(b, 0x0f); bb_byte(b, 0x80 + ip->cc); }
  {
    int w = ip->mode == 16 ? 2 : 4;
    if (known) put_le(b, (uval)(tg - (pc + size)), w);
    else {
      if (enc) add_reloc(s, pc + (ival)b->n, w * 8, R_PC, r.sec, r.ext, r.v);
      put_le(b, (uval)(r.v - w), w);
    }
  }
}

static void x86_emit(xinsn *ip, section *s, ival pc, bbuf *b, ival size, int enc)
{
  xmatch m;
  cmode = ip->mode;
  if (ip->jkind) { emit_jump(ip, s, pc, b, size, enc); return; }
  if (!find_match(ip->first, ip->ops, ip->nops, ip->sfx, &m)) {
    if (enc) error("illegal operand types");
    return;
  }
  emit_template(ip, &m, s, pc, b, enc);
}

static ival x86_size(atom *a, section *s, ival pc)
{
  xinsn *ip = a->u.insn.insn;
  ival sz;
  if (ip->jkind && ip->jkind != J_LOOP) sz = jump_size(ip, s, pc);
  else {
    bbuf b = { 0 };
    x86_emit(ip, s, pc, &b, 0, 0);
    sz = (ival)b.n;
    free(b.d);
  }
  if (grow_only && sz < a->size) sz = a->size;
  return sz;
}

static void x86_encode(atom *a, section *s, ival pc, bbuf *out)
{
  xinsn *ip = a->u.insn.insn;
  bbuf b = { 0 };
  x86_emit(ip, s, pc, &b, a->size, 1);   /* field offsets are relative to the instruction */
  while ((ival)b.n < a->size) bb_byte(&b, 0x90);
  bb_put(out, b.d, b.n);
  free(b.d);
}

static int x86_instruction(const char *mn, size_t ml, const char *ext,
                           char **ops, int nops, int col)
{
  char name[40];
  xinsn *ip;
  size_t i;
  xt *t;
  atom *a;
  (void)ext; (void)col;
  if (ml >= sizeof name) return 0;
  for (i = 0; i < ml; i++) name[i] = (char)tolower((unsigned char)mn[i]);
  name[ml] = 0;
  ip = xcalloc(1, sizeof(*ip));
  ip->mode = mode_bits;
  if (nops > 3) { error("illegal operand types"); return 1; }
  for (i = 0; i < (size_t)nops; i++)
    if (!parse_operand(ops[i], &ip->ops[i])) { if (!errcount) error("illegal operand types"); return 1; }
  ip->nops = nops;
  /* relative branches */
  if (nops == 1 && is_rel(&ip->ops[0])) {
    int c, k;
    if (!strcmp(name, "jmp")) ip->jkind = J_JMP;
    else if (!strcmp(name, "call") || !strcmp(name, "calll")) ip->jkind = J_CALL;
    else if (!strcmp(name, "loop")) { ip->jkind = J_LOOP; ip->jop = 0xe2; }
    else if (!strcmp(name, "loope") || !strcmp(name, "loopz")) { ip->jkind = J_LOOP; ip->jop = 0xe1; }
    else if (!strcmp(name, "loopne") || !strcmp(name, "loopnz")) { ip->jkind = J_LOOP; ip->jop = 0xe0; }
    else if (!strcmp(name, "jecxz")) { ip->jkind = J_LOOP; ip->jop = 0xe3; }
    else if (!strcmp(name, "jcxz")) { ip->jkind = J_LOOP; ip->jop = 0xe3; ip->cc = 1; }
    else if (name[0] == 'j')
      for (c = 0; c < 16 && !ip->jkind; c++)
        for (k = 0; k < 4 && ccn[c][k]; k++)
          if (!strcmp(name + 1, ccn[c][k])) { ip->jkind = J_JCC; ip->cc = c; break; }
    if (ip->jkind) {
      a = add_atom(AT_INSN);
      a->u.insn.insn = ip;
      return 1;
    }
  }
  t = xfirst(name, ml);
  if (!t && ml > 1 && strchr("bwlq", name[ml - 1])) {
    t = xfirst(name, ml - 1);
    ip->sfx = name[ml - 1] == 'b' ? 1 : name[ml - 1] == 'w' ? 2 : name[ml - 1] == 'l' ? 4 : 8;
    if (t && !t->sfx) {
      xt *u;
      for (u = t; u && u->sfx == 0; u = u->next) ;
      /* suffix on a mnemonic that has no sized forms */
    }
  }
  if (!t) return 0;
  ip->first = t;
  cmode = ip->mode;
  {
    xmatch m;
    if (!find_match(t, ip->ops, nops, ip->sfx, &m)) {
      int i2, sizedreg = 0, anys = 0;
      xt *u;
      for (u = t; u; u = u->next) if (!stricmp_(u->name, t->name) && u->sfx) anys = 1;
      for (i2 = 0; i2 < nops; i2++) if (ip->ops[i2].kind == K_REG && ip->ops[i2].r.cls == RC_GPR) sizedreg = 1;
      if (anys && !ip->sfx && !sizedreg)
        error("instruction has no suffix and no register operands - size is unknown");
      else
        error("illegal operand types");
      return 1;
    }
  }
  a = add_atom(AT_INSN);
  a->u.insn.insn = ip;
  return 1;
}

static int x86_directive(const char *name, const char *args)
{
  (void)args;
  if (!stricmp_(name, ".code16")) { mode_bits = 16; return 1; }
  if (!stricmp_(name, ".code32")) { mode_bits = 32; return 1; }
  if (!stricmp_(name, ".code64")) { error("64-bit mode is not supported by gfasm yet"); return 1; }
  return 0;
}

static int x86_option(const char *o)
{
  static const char *cpus[] = { "-m8086", "-mi186", "-mi286", "-mi386", "-mi486", "-mi586", "-mi686",
    "-mpentium", "-mpentiumpro", "-mk6", "-mathlon", "-msledgehammer", NULL };
  int i;
  for (i = 0; cpus[i]; i++) if (!strcmp(o, cpus[i])) return 1;
  if (!strncmp(o, "-cpudebug=", 10)) return 1;
  if (!strcmp(o, "-m64")) { opt_m64 = 1; return 1; }
  return 0;
}

static int x86_init(void)
{
  build_tables();
  return 1;
}

static void x86_define_symbols(void)
{
  set_internal(sym_get("__VASM"), 0);
  sym_get("__VASM")->flags |= SF_INTERNAL;
  if (opt_m64) fatal("x86-64 (-m64) is not supported by gfasm yet");
}

static int x86_elf_reloc(reloc *r)
{
  if (r->type == R_ABS) return r->size == 32 ? 1 : r->size == 16 ? 20 : r->size == 8 ? 22 : -1;
  if (r->type == R_PC) return r->size == 32 ? 2 : r->size == 16 ? 21 : r->size == 8 ? 23 : -1;
  return -1;
}

cpu_module cpu_x86 = {
  "x86",
  x86_init,
  x86_option,
  x86_instruction,
  x86_directive,
  x86_size,
  x86_encode,
  NULL,
  x86_define_symbols,
  NULL,
  0, 1, 0, 32, 3, x86_elf_reloc,
  1, 0, NULL, NULL
};
