/* gfasm - PowerPC cpu module (32-bit, with AltiVec).  MIT License.
 *
 * Encodings from the public "PowerPC Microprocessor Family: The Programming
 * Environments for 32-bit Microprocessors" and the AltiVec Technology
 * Programming Environments Manual; ELF relocation numbers from the
 * PowerPC SVR4/EABI supplements.  vasm behaviour (option names, predefined
 * register symbols, hint bits) from the vasm manual and comparison with
 * vasm's output. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void encode_float(int ftype, long double f, uint8_t *o);

/* relocation modifiers */
#define MOD_NONE 0
#define MOD_L    1
#define MOD_H    2
#define MOD_HA   3
#define MOD_SDA  4    /* @sdarel */
#define MOD_SDA21 5

static int no_regnames, ppc_little, sdreg = 13, sd2reg = 2, opt_branch;

/* ------------------------------------------------------------ table */
/* operand codes, see enc_operand() */
typedef struct pmn {
  const char *name;
  uint32_t opc;
  const char *ops;
  int flags;
} pmn;
#define F_RC   1     /* has a "." variant (bit 31) */
#define F_OE   2     /* has an "o" variant (bit 21 0x400) */
#define F_EXT  4     /* extended mnemonic handled by code (opc = kind) */
#define F_AA   8     /* has "a" variant (absolute, bit 30) */
#define F_LK   16    /* has "l" variant (bit 31) */
#define F_HINT 32    /* +/- hints allowed */
#define F_OPTCR 64   /* first operand: optional cr field (cmp...) */
#define F_ALT  128   /* AltiVec */

#define OP(x)  ((uint32_t)(x) << 26)
#define XO(x)  ((uint32_t)(x) << 1)
#define X(o,x) (OP(o) | XO(x))

static const pmn ptab[] = {
  /* integer arithmetic */
  { "addi", OP(14), "DZS", 0 }, { "addis", OP(15), "DZU", 0 }, { "addic", OP(12), "DAS", 0 },
  { "addic.", OP(13), "DAS", 0 }, { "subfic", OP(8), "DAS", 0 }, { "mulli", OP(7), "DAS", 0 },
  { "add", X(31,266), "DAB", F_RC|F_OE }, { "addc", X(31,10), "DAB", F_RC|F_OE },
  { "adde", X(31,138), "DAB", F_RC|F_OE }, { "addme", X(31,234), "DA", F_RC|F_OE },
  { "addze", X(31,202), "DA", F_RC|F_OE }, { "subf", X(31,40), "DAB", F_RC|F_OE },
  { "subfc", X(31,8), "DAB", F_RC|F_OE }, { "subfe", X(31,136), "DAB", F_RC|F_OE },
  { "subfme", X(31,232), "DA", F_RC|F_OE }, { "subfze", X(31,200), "DA", F_RC|F_OE },
  { "neg", X(31,104), "DA", F_RC|F_OE }, { "mullw", X(31,235), "DAB", F_RC|F_OE },
  { "mulhw", X(31,75), "DAB", F_RC }, { "mulhwu", X(31,11), "DAB", F_RC },
  { "divw", X(31,491), "DAB", F_RC|F_OE }, { "divwu", X(31,459), "DAB", F_RC|F_OE },
  /* compare */
  { "cmpi", OP(11), "clAS", 0 }, { "cmpli", OP(10), "clAU", 0 },
  { "cmp", X(31,0), "clAB", 0 }, { "cmpl", X(31,32), "clAB", 0 },
  /* logical (rA <- rS) */
  { "andi.", OP(28), "ADU", 0 }, { "andis.", OP(29), "ADU", 0 }, { "ori", OP(24), "ADU", 0 },
  { "oris", OP(25), "ADU", 0 }, { "xori", OP(26), "ADU", 0 }, { "xoris", OP(27), "ADU", 0 },
  { "and", X(31,28), "ADB", F_RC }, { "or", X(31,444), "ADB", F_RC }, { "xor", X(31,316), "ADB", F_RC },
  { "nand", X(31,476), "ADB", F_RC }, { "nor", X(31,124), "ADB", F_RC }, { "eqv", X(31,284), "ADB", F_RC },
  { "andc", X(31,60), "ADB", F_RC }, { "orc", X(31,412), "ADB", F_RC },
  { "extsb", X(31,954), "AD", F_RC }, { "extsh", X(31,922), "AD", F_RC }, { "cntlzw", X(31,26), "AD", F_RC }, { "extsw", X(31,986), "AD", F_RC },
  /* rotate / shift */
  { "rlwinm", OP(21), "ADhmn", F_RC }, { "rlwnm", OP(23), "ADBmn", F_RC }, { "rlwimi", OP(20), "ADhmn", F_RC },
  { "slw", X(31,24), "ADB", F_RC }, { "srw", X(31,536), "ADB", F_RC }, { "sraw", X(31,792), "ADB", F_RC },
  { "srawi", X(31,824), "ADh", F_RC },
  /* loads / stores */
  { "lbz", OP(34), "DM", 0 }, { "lbzu", OP(35), "DM", 0 }, { "lhz", OP(40), "DM", 0 }, { "lhzu", OP(41), "DM", 0 },
  { "lha", OP(42), "DM", 0 }, { "lhau", OP(43), "DM", 0 }, { "lwz", OP(32), "DM", 0 }, { "lwzu", OP(33), "DM", 0 },
  { "stb", OP(38), "DM", 0 }, { "stbu", OP(39), "DM", 0 }, { "sth", OP(44), "DM", 0 }, { "sthu", OP(45), "DM", 0 },
  { "stw", OP(36), "DM", 0 }, { "stwu", OP(37), "DM", 0 }, { "lmw", OP(46), "DM", 0 }, { "stmw", OP(47), "DM", 0 },
  { "lbzx", X(31,87), "DZB", 0 }, { "lbzux", X(31,119), "DAB", 0 }, { "lhzx", X(31,279), "DZB", 0 },
  { "lhzux", X(31,311), "DAB", 0 }, { "lhax", X(31,343), "DZB", 0 }, { "lhaux", X(31,375), "DAB", 0 },
  { "lwzx", X(31,23), "DZB", 0 }, { "lwzux", X(31,55), "DAB", 0 }, { "stbx", X(31,215), "DZB", 0 },
  { "stbux", X(31,247), "DAB", 0 }, { "sthx", X(31,407), "DZB", 0 }, { "sthux", X(31,439), "DAB", 0 },
  { "stwx", X(31,151), "DZB", 0 }, { "stwux", X(31,183), "DAB", 0 }, { "lhbrx", X(31,790), "DZB", 0 },
  { "lwbrx", X(31,534), "DZB", 0 }, { "sthbrx", X(31,918), "DZB", 0 }, { "stwbrx", X(31,662), "DZB", 0 },
  { "lswi", X(31,597), "DZN", 0 }, { "lswx", X(31,533), "DZB", 0 }, { "stswi", X(31,725), "DZN", 0 },
  { "stswx", X(31,661), "DZB", 0 }, { "lwarx", X(31,20), "DZB", 0 }, { "stwcx.", X(31,150) | 1, "DZB", 0 },
  /* floating point loads/stores */
  { "lfs", OP(48), "FM", 0 }, { "lfsu", OP(49), "FM", 0 }, { "lfd", OP(50), "FM", 0 }, { "lfdu", OP(51), "FM", 0 },
  { "stfs", OP(52), "FM", 0 }, { "stfsu", OP(53), "FM", 0 }, { "stfd", OP(54), "FM", 0 }, { "stfdu", OP(55), "FM", 0 },
  { "lfsx", X(31,535), "FZB", 0 }, { "lfsux", X(31,567), "FAB", 0 }, { "lfdx", X(31,599), "FZB", 0 },
  { "lfdux", X(31,631), "FAB", 0 }, { "stfsx", X(31,663), "FZB", 0 }, { "stfsux", X(31,695), "FAB", 0 },
  { "stfdx", X(31,727), "FZB", 0 }, { "stfdux", X(31,759), "FAB", 0 }, { "stfiwx", X(31,983), "FZB", 0 },
  /* floating point arithmetic */
  { "fadd", X(63,21), "FGH", F_RC }, { "fadds", X(59,21), "FGH", F_RC }, { "fsub", X(63,20), "FGH", F_RC },
  { "fsubs", X(59,20), "FGH", F_RC }, { "fmul", X(63,25), "FGI", F_RC }, { "fmuls", X(59,25), "FGI", F_RC },
  { "fdiv", X(63,18), "FGH", F_RC }, { "fdivs", X(59,18), "FGH", F_RC },
  { "fmadd", X(63,29), "FGIH", F_RC }, { "fmadds", X(59,29), "FGIH", F_RC },
  { "fmsub", X(63,28), "FGIH", F_RC }, { "fmsubs", X(59,28), "FGIH", F_RC },
  { "fnmadd", X(63,31), "FGIH", F_RC }, { "fnmadds", X(59,31), "FGIH", F_RC },
  { "fnmsub", X(63,30), "FGIH", F_RC }, { "fnmsubs", X(59,30), "FGIH", F_RC },
  { "fsel", X(63,23), "FGIH", F_RC }, { "fres", X(59,24), "FH", F_RC }, { "frsqrte", X(63,26), "FH", F_RC },
  { "fsqrt", X(63,22), "FH", F_RC }, { "fsqrts", X(59,22), "FH", F_RC },
  { "frsp", X(63,12), "FH", F_RC }, { "fctiw", X(63,14), "FH", F_RC }, { "fctiwz", X(63,15), "FH", F_RC },
  { "fmr", X(63,72), "FH", F_RC }, { "fneg", X(63,40), "FH", F_RC }, { "fabs", X(63,264), "FH", F_RC },
  { "fnabs", X(63,136), "FH", F_RC }, { "fcmpu", X(63,0), "cGH", 0 }, { "fcmpo", X(63,32), "cGH", 0 },
  { "mffs", X(63,583), "F", F_RC }, { "mcrfs", X(63,64), "cs", 0 }, { "mtfsfi", X(63,134), "cj", F_RC },
  { "mtfsf", X(63,711), "kH", F_RC }, { "mtfsb0", X(63,70), "b", F_RC }, { "mtfsb1", X(63,38), "b", F_RC },
  /* branches */
  { "b", OP(18), "L", F_AA|F_LK }, { "bc", OP(16), "OiT", F_AA|F_LK|F_HINT },
  { "bclr", X(19,16), "Oi", F_LK|F_HINT }, { "bcctr", X(19,528), "Oi", F_LK|F_HINT },
  /* condition register */
  { "crand", X(19,257), "bef", 0 }, { "crandc", X(19,129), "bef", 0 }, { "creqv", X(19,289), "bef", 0 },
  { "crnand", X(19,225), "bef", 0 }, { "crnor", X(19,33), "bef", 0 }, { "cror", X(19,449), "bef", 0 },
  { "crorc", X(19,417), "bef", 0 }, { "crxor", X(19,193), "bef", 0 }, { "mcrf", X(19,0), "cs", 0 },
  { "mcrxr", X(31,512), "c", 0 }, { "mfcr", X(31,19), "D", 0 }, { "mtcrf", X(31,144), "KD", 0 },
  /* system */
  { "sc", OP(17) | 2, "", 0 }, { "rfi", X(19,50), "", 0 }, { "isync", X(19,150), "", 0 },
  { "sync", X(31,598), "o", 0 }, { "eieio", X(31,854), "", 0 }, { "tlbsync", X(31,566), "", 0 },
  { "tw", X(31,4), "tAB", 0 }, { "twi", OP(3), "tAS", 0 },
  { "mfspr", X(31,339), "DP", 0 }, { "mtspr", X(31,467), "PD", 0 }, { "mftb", X(31,371), "Dg", 0 },
  { "mfmsr", X(31,83), "D", 0 }, { "mtmsr", X(31,146), "D", 0 }, { "mfsr", X(31,595), "DR", 0 },
  { "mtsr", X(31,210), "RD", 0 }, { "mfsrin", X(31,659), "DB", 0 }, { "mtsrin", X(31,242), "DB", 0 },
  { "tlbie", X(31,306), "B", 0 }, { "tlbia", X(31,370), "", 0 }, { "eciwx", X(31,310), "DZB", 0 },
  { "ecowx", X(31,438), "DZB", 0 },
  /* cache */
  { "dcbf", X(31,86), "ZB", 0 }, { "dcbi", X(31,470), "ZB", 0 }, { "dcbst", X(31,54), "ZB", 0 },
  { "dcbt", X(31,278), "ZB", 0 }, { "dcbtst", X(31,246), "ZB", 0 }, { "dcbz", X(31,1014), "ZB", 0 },
  { "icbi", X(31,982), "ZB", 0 }, { "dcba", X(31,758), "ZB", 0 },
  /* AltiVec loads/stores and control */
  { "lvebx", X(31,7), "VZB", F_ALT }, { "lvehx", X(31,39), "VZB", F_ALT }, { "lvewx", X(31,71), "VZB", F_ALT },
  { "lvsl", X(31,6), "VZB", F_ALT }, { "lvsr", X(31,38), "VZB", F_ALT }, { "lvx", X(31,103), "VZB", F_ALT },
  { "lvxl", X(31,359), "VZB", F_ALT }, { "stvebx", X(31,135), "VZB", F_ALT }, { "stvehx", X(31,167), "VZB", F_ALT },
  { "stvewx", X(31,199), "VZB", F_ALT }, { "stvx", X(31,231), "VZB", F_ALT }, { "stvxl", X(31,487), "VZB", F_ALT },
  { "dss", X(31,822), "y", F_ALT }, { "dssall", X(31,822) | 0x02000000, "", F_ALT },
  { "dst", X(31,342), "ABy", F_ALT }, { "dstt", X(31,342) | 0x02000000, "ABy", F_ALT },
  { "dstst", X(31,374), "ABy", F_ALT }, { "dststt", X(31,374) | 0x02000000, "ABy", F_ALT },
  { "mfvscr", OP(4) | 1540, "V", F_ALT }, { "mtvscr", OP(4) | 1604, "X", F_ALT },
  { NULL, 0, NULL, 0 }
};

/* AltiVec VX/VA/VC forms: name, xo, operand string */
typedef struct vxe { const char *n; uint32_t xo; const char *ops; } vxe;
static const vxe vxtab[] = {
  { "vaddubm", 0, "VWX" }, { "vadduhm", 64, "VWX" }, { "vadduwm", 128, "VWX" }, { "vaddcuw", 384, "VWX" },
  { "vaddubs", 512, "VWX" }, { "vadduhs", 576, "VWX" }, { "vadduws", 640, "VWX" }, { "vaddsbs", 768, "VWX" },
  { "vaddshs", 832, "VWX" }, { "vaddsws", 896, "VWX" }, { "vsububm", 1024, "VWX" }, { "vsubuhm", 1088, "VWX" },
  { "vsubuwm", 1152, "VWX" }, { "vsubcuw", 1408, "VWX" }, { "vsububs", 1536, "VWX" }, { "vsubuhs", 1600, "VWX" },
  { "vsubuws", 1664, "VWX" }, { "vsubsbs", 1792, "VWX" }, { "vsubshs", 1856, "VWX" }, { "vsubsws", 1920, "VWX" },
  { "vmaxub", 2, "VWX" }, { "vmaxuh", 66, "VWX" }, { "vmaxuw", 130, "VWX" }, { "vmaxsb", 258, "VWX" },
  { "vmaxsh", 322, "VWX" }, { "vmaxsw", 386, "VWX" }, { "vminub", 514, "VWX" }, { "vminuh", 578, "VWX" },
  { "vminuw", 642, "VWX" }, { "vminsb", 770, "VWX" }, { "vminsh", 834, "VWX" }, { "vminsw", 898, "VWX" },
  { "vavgub", 1026, "VWX" }, { "vavguh", 1090, "VWX" }, { "vavguw", 1154, "VWX" }, { "vavgsb", 1282, "VWX" },
  { "vavgsh", 1346, "VWX" }, { "vavgsw", 1410, "VWX" }, { "vrlb", 4, "VWX" }, { "vrlh", 68, "VWX" },
  { "vrlw", 132, "VWX" }, { "vslb", 260, "VWX" }, { "vslh", 324, "VWX" }, { "vslw", 388, "VWX" },
  { "vsl", 452, "VWX" }, { "vsrb", 516, "VWX" }, { "vsrh", 580, "VWX" }, { "vsrw", 644, "VWX" },
  { "vsr", 708, "VWX" }, { "vsrab", 772, "VWX" }, { "vsrah", 836, "VWX" }, { "vsraw", 900, "VWX" },
  { "vand", 1028, "VWX" }, { "vandc", 1092, "VWX" }, { "vor", 1156, "VWX" }, { "vnor", 1284, "VWX" },
  { "vxor", 1220, "VWX" }, { "vslo", 1036, "VWX" }, { "vsro", 1100, "VWX" },
  { "vmuloub", 8, "VWX" }, { "vmulouh", 72, "VWX" }, { "vmulosb", 264, "VWX" }, { "vmulosh", 328, "VWX" },
  { "vmuleub", 520, "VWX" }, { "vmuleuh", 584, "VWX" }, { "vmulesb", 776, "VWX" }, { "vmulesh", 840, "VWX" },
  { "vsum4ubs", 1544, "VWX" }, { "vsum4sbs", 1800, "VWX" }, { "vsum4shs", 1608, "VWX" },
  { "vsum2sws", 1672, "VWX" }, { "vsumsws", 1928, "VWX" },
  { "vaddfp", 10, "VWX" }, { "vsubfp", 74, "VWX" }, { "vmaxfp", 1034, "VWX" }, { "vminfp", 1098, "VWX" },
  { "vrefp", 266, "VX" }, { "vrsqrtefp", 330, "VX" }, { "vexptefp", 394, "VX" }, { "vlogefp", 458, "VX" },
  { "vrfin", 522, "VX" }, { "vrfiz", 586, "VX" }, { "vrfip", 650, "VX" }, { "vrfim", 714, "VX" },
  { "vcfux", 778, "VXu" }, { "vcfsx", 842, "VXu" }, { "vctuxs", 906, "VXu" }, { "vctsxs", 970, "VXu" },
  { "vmrghb", 12, "VWX" }, { "vmrghh", 76, "VWX" }, { "vmrghw", 140, "VWX" }, { "vmrglb", 268, "VWX" },
  { "vmrglh", 332, "VWX" }, { "vmrglw", 396, "VWX" },
  { "vspltb", 524, "VXu" }, { "vsplth", 588, "VXu" }, { "vspltw", 652, "VXu" },
  { "vspltisb", 780, "Vv" }, { "vspltish", 844, "Vv" }, { "vspltisw", 908, "Vv" },
  { "vpkuhum", 14, "VWX" }, { "vpkuwum", 78, "VWX" }, { "vpkuhus", 142, "VWX" }, { "vpkuwus", 206, "VWX" },
  { "vpkshus", 270, "VWX" }, { "vpkswus", 334, "VWX" }, { "vpkshss", 398, "VWX" }, { "vpkswss", 462, "VWX" },
  { "vpkpx", 782, "VWX" }, { "vupkhsb", 526, "VX" }, { "vupkhsh", 590, "VX" }, { "vupklsb", 654, "VX" },
  { "vupklsh", 718, "VX" }, { "vupkhpx", 846, "VX" }, { "vupklpx", 974, "VX" },
  /* VC (compare, with "." variant: bit 0x400) */
  { "vcmpequb", 6, "VWX." }, { "vcmpequh", 70, "VWX." }, { "vcmpequw", 134, "VWX." }, { "vcmpeqfp", 198, "VWX." },
  { "vcmpgefp", 454, "VWX." }, { "vcmpgtub", 518, "VWX." }, { "vcmpgtuh", 582, "VWX." }, { "vcmpgtuw", 646, "VWX." },
  { "vcmpgtfp", 710, "VWX." }, { "vcmpgtsb", 774, "VWX." }, { "vcmpgtsh", 838, "VWX." }, { "vcmpgtsw", 902, "VWX." },
  { "vcmpbfp", 966, "VWX." },
  /* VA */
  { "vmhaddshs", 32, "VWXY" }, { "vmhraddshs", 33, "VWXY" }, { "vmladduhm", 34, "VWXY" },
  { "vmsumubm", 36, "VWXY" }, { "vmsummbm", 37, "VWXY" }, { "vmsumuhm", 38, "VWXY" }, { "vmsumuhs", 39, "VWXY" },
  { "vmsumshm", 40, "VWXY" }, { "vmsumshs", 41, "VWXY" }, { "vsel", 42, "VWXY" }, { "vperm", 43, "VWXY" },
  { "vsldoi", 44, "VWXw" }, { "vmaddfp", 46, "VWXY" }, { "vnmsubfp", 47, "VWYX" },
  { NULL, 0, NULL }
};

/* ------------------------------------------------------------ insn */
typedef struct pinsn {
  uint32_t opc;
  const char *ops;
  int nops;
  expr *op[6];
  int mod[6];
  expr *base[6];         /* d(rA) */
  int flags;
  int hint;              /* 0, '+', '-' */
  int kind;              /* extended mnemonic kind */
  int aa;
  ival bdisp;
} pinsn;


/* ------------------------------------------------------------ lookup */
#define PHASH 1024
typedef struct pent { const pmn *m; int rc, oe, aa, lk; struct pent *next; char *name; } pent;
static pent *phash[PHASH];

static unsigned ph(const char *s, size_t n)
{
  unsigned h = 5381;
  while (n--) h = h * 33 + tolower((unsigned char)*s++);
  return h & (PHASH - 1);
}

static void padd(const char *name, const pmn *m, int rc, int oe, int aa, int lk)
{
  pent *e = xcalloc(1, sizeof(*e));
  unsigned h = ph(name, strlen(name));
  e->name = xstrdup(name);
  e->m = m; e->rc = rc; e->oe = oe; e->aa = aa; e->lk = lk;
  e->next = phash[h];
  phash[h] = e;
}

static pent *pfind(const char *s, size_t n)
{
  pent *e;
  for (e = phash[ph(s, n)]; e; e = e->next)
    if (strlen(e->name) == n && !strnicmp_(e->name, s, n)) return e;
  return NULL;
}

static void build(void)
{
  int i;
  static pmn vx[256];
  int nvx = 0;
  for (i = 0; ptab[i].name; i++) {
    const pmn *m = &ptab[i];
    char b[40];
    padd(m->name, m, 0, 0, 0, 0);
    if (m->flags & F_RC) { snprintf(b, sizeof b, "%s.", m->name); padd(b, m, 1, 0, 0, 0); }
    if (m->flags & F_OE) {
      snprintf(b, sizeof b, "%so", m->name); padd(b, m, 0, 1, 0, 0);
      snprintf(b, sizeof b, "%so.", m->name); padd(b, m, 1, 1, 0, 0);
    }
    if (m->flags & F_LK) { snprintf(b, sizeof b, "%sl", m->name); padd(b, m, 0, 0, 0, 1); }
    if (m->flags & F_AA) {
      snprintf(b, sizeof b, "%sa", m->name); padd(b, m, 0, 0, 1, 0);
      snprintf(b, sizeof b, "%sla", m->name); padd(b, m, 0, 0, 1, 1);
    }
  }
  for (i = 0; vxtab[i].n; i++) {
    pmn *m = &vx[nvx++];
    size_t l = strlen(vxtab[i].ops);
    int isva = l == 4 && (vxtab[i].ops[3] != '.');
    m->name = vxtab[i].n;
    m->flags = F_ALT;
    if (vxtab[i].ops[l - 1] == '.') {
      char *o = xstrndup(vxtab[i].ops, l - 1);
      char b[40];
      m->ops = o;
      m->opc = OP(4) | vxtab[i].xo;
      padd(m->name, m, 0, 0, 0, 0);
      snprintf(b, sizeof b, "%s.", m->name);
      padd(b, m, 2, 0, 0, 0);   /* rc=2: VC record bit 0x400 */
      continue;
    }
    m->ops = vxtab[i].ops;
    m->opc = OP(4) | vxtab[i].xo;
    (void)isva;
    padd(m->name, m, 0, 0, 0, 0);
  }
}

/* ------------------------------------------------------------ operands */
static int parse_mod(char *s, int *mod)
{
  /* strip @l @h @ha @sdarel @sda21 suffix; lo16() hi16() ha16() wrappers */
  char *at = strrchr(s, '@');
  size_t n;
  *mod = MOD_NONE;
  if (at && (at == s || !(isalnum((unsigned char)at[-1]) || at[-1] == '_' || at[-1] == ')' || at[-1] == '$' || at[-1] == '.')))
    at = NULL;    /* '@' starts an identifier */
  if (at) {
    if (!stricmp_(at, "@l")) *mod = MOD_L;
    else if (!stricmp_(at, "@h")) *mod = MOD_H;
    else if (!stricmp_(at, "@ha")) *mod = MOD_HA;
    else if (!stricmp_(at, "@sdarel")) *mod = MOD_SDA;
    else if (!stricmp_(at, "@sda21")) *mod = MOD_SDA21;
    else return 0;
    *at = 0;
    return 1;
  }
  n = strlen(s);
  if (n > 6 && s[n - 1] == ')' && (s[4] == '(') &&
      (!strnicmp_(s, "lo16", 4) || !strnicmp_(s, "hi16", 4) || !strnicmp_(s, "ha16", 4))) {
    *mod = !strnicmp_(s, "lo16", 4) ? MOD_L : !strnicmp_(s, "hi16", 4) ? MOD_H : MOD_HA;
    memmove(s, s + 5, n - 5);
    s[n - 6] = 0;
  }
  return 1;
}

static expr *pexp(const char *s)
{
  const char *p = skip_ws(s);
  expr *e = parse_expr(&p);
  if (!e) { error("syntax error in operand \"%s\"", s); return new_num(0); }
  if (*skip_ws(p)) error("trailing garbage in operand");
  return e;
}

static int create(const pent *pe, char **ops, int nops, int hint, int forcecr)
{
  const pmn *m = pe->m;
  pinsn *ip = xcalloc(1, sizeof(*ip));
  atom *a;
  int i, k = 0;
  size_t need = strlen(m->ops);
  ip->opc = m->opc;
  if (pe->rc == 1) ip->opc |= 1;
  if (pe->rc == 2) ip->opc |= 0x400;
  if (pe->oe) ip->opc |= 0x400;
  if (pe->aa) { ip->opc |= 2; ip->aa = 1; }
  if (pe->lk) ip->opc |= 1;
  ip->ops = m->ops;
  ip->flags = m->flags;
  ip->hint = hint;
  /* optional operands: cr field of cmp (c first), L of cmp, TBR of mftb, STRM... */
  for (i = 0; i < (int)need && k <= nops; i++) {
    char c = m->ops[i];
    if (c == 'l' && i == 1 && m->ops[0] == 'c' && nops == (int)need - 1) { ip->op[i] = new_num(0); continue; }
    if (c == 'o' && k >= nops) { ip->op[i] = new_num(0); continue; }
    if (c == 'g' && k >= nops) { ip->op[i] = new_num(268); continue; }
    if (c == 'i' && k >= nops && (m->opc == X(19,16) || m->opc == X(19,528))) { ip->op[i] = new_num(0); continue; }
    /* rlwinm/rlwnm/rlwimi with a 32-bit mask instead of MB,ME */
    if (c == 'm' && nops == (int)need - 1 && k == nops - 1) {
      ip->op[i] = pexp(ops[k++]);
      ip->op[i + 1] = NULL;
      ip->flags |= 0x1000;   /* mask form */
      i++;
      continue;
    }
    if (k >= nops) { error("missing mandatory operand"); return 1; }
    {
      char *t = xstrdup(ops[k++]);
      if (c == 'M') {
        /* d(rA) */
        char *lp = strrchr(t, '(');
        size_t n = strlen(t);
        if (!lp || t[n - 1] != ')') { error("missing base register in load/store addressing mode"); free(t); return 1; }
        t[n - 1] = 0;
        *lp = 0;
        ip->base[i] = pexp(lp + 1);
        if (!*skip_ws(t)) ip->op[i] = new_num(0);
        else { if (!parse_mod(t, &ip->mod[i])) error("relocation attribute not supported"); ip->op[i] = pexp(t); }
      } else {
        if (strchr("SUT L", c) && c != ' ') {
          if (!parse_mod(t, &ip->mod[i])) error("relocation attribute not supported");
        }
        ip->op[i] = pexp(t);
      }
      free(t);
    }
  }
  if (k < nops) error("too many operands");
  ip->nops = (int)need;
  a = add_atom(AT_INSN);
  a->u.insn.insn = ip;
  return 1;
}

/* ------------------------------------------------------------ extended */
static const struct { const char *n; int bit; int t; } bcond[] = {
  { "lt", 0, 1 }, { "le", 1, 0 }, { "eq", 2, 1 }, { "ge", 0, 0 }, { "gt", 1, 1 }, { "nl", 0, 0 },
  { "ne", 2, 0 }, { "ng", 1, 0 }, { "so", 3, 1 }, { "ns", 3, 0 }, { "un", 3, 1 }, { "nu", 3, 0 },
  { NULL, 0, 0 }
};

static int do_create(const char *name, char **ops, int nops, int hint)
{
  pent *pe = pfind(name, strlen(name));
  if (!pe) return 0;
  return create(pe, ops, nops, hint, 0);
}

static char *fmt(const char *f, ...)
{
  va_list ap;
  char *s;
  int n;
  va_start(ap, f);
  n = vsnprintf(NULL, 0, f, ap);
  va_end(ap);
  s = xmalloc(n + 1);
  va_start(ap, f);
  vsnprintf(s, n + 1, f, ap);
  va_end(ap);
  return s;
}

static int rewrite(const char *base, int n, char **v, char **ops, int nops, int hint)
{
  int r, i;
  (void)ops; (void)nops;
  r = do_create(base, v, n, hint);
  for (i = 0; i < n; i++) free(v[i]);
  return r;
}

/* conditional branch family: b<cond>[lr|ctr][l][a][+-] */
static int ext_branch(const char *mn, char **ops, int nops, int hint)
{
  char m[40], *p;
  int bo, bit = -1, t = -1, kind = 0, lk = 0, aa = 0, i;
  const char *rest;
  char *v[4]; int nv = 0;
  if (strlen(mn) >= sizeof m) return 0;
  strcpy(m, mn);
  for (p = m; *p; p++) *p = tolower((unsigned char)*p);
  if (m[0] != 'b') return 0;
  rest = m + 1;
  if (!strncmp(rest, "dnz", 3) || !strncmp(rest, "dz", 2)) {
    int nz = rest[1] == 'n';
    rest += nz ? 3 : 2;
    if (*rest == 't' || *rest == 'f') {
      t = *rest == 't'; rest++;
      bo = nz ? (t ? 8 : 0) : (t ? 10 : 2);
      bit = -2;   /* BI given as operand */
    } else
      bo = nz ? 16 : 18;
  } else if (*rest == 't' || *rest == 'f') {
    t = *rest == 't'; rest++;
    bo = t ? 12 : 4;
    bit = -2;
  } else {
    for (i = 0; bcond[i].n; i++)
      if (!strncmp(rest, bcond[i].n, 2)) break;
    if (!bcond[i].n) return 0;
    rest += 2;
    bit = bcond[i].bit;
    bo = bcond[i].t ? 12 : 4;
  }
  if (!strncmp(rest, "lr", 2)) { kind = 1; rest += 2; }
  else if (!strncmp(rest, "ctr", 3)) { kind = 2; rest += 3; }
  if (*rest == 'l') { lk = 1; rest++; }
  if (*rest == 'a' && kind == 0) { aa = 1; rest++; }
  if (*rest) return 0;
  if (kind == 2 && (bo & 4) == 0 && bo != 12 && bo != 4) return 0;   /* no bdnzctr */
  /* operands */
  i = 0;
  if (bit >= 0) {
    /* optional cr field */
    int wantop = kind ? 0 : 1;
    if (nops > wantop) v[nv++] = fmt("4*(%s)+%d", ops[i++], bit);
    else v[nv++] = fmt("%d", bit);
  } else if (bit == -2) {
    if (i >= nops) { error("missing mandatory operand"); return 1; }
    v[nv++] = xstrdup(ops[i++]);
  } else
    v[nv++] = xstrdup("0");
  {
    char *bos = fmt("%d", bo);
    char *vv[4];
    int k = 0;
    vv[k++] = bos;
    vv[k++] = v[0];
    if (kind == 0) {
      if (i >= nops) { error("missing mandatory operand"); return 1; }
      vv[k++] = xstrdup(ops[i++]);
    }
    if (i < nops) error("too many operands");
    {
      const char *base = kind == 1 ? "bclr" : kind == 2 ? "bcctr" : "bc";
      char b[16];
      snprintf(b, sizeof b, "%s%s%s", base, lk ? "l" : "", aa ? "a" : "");
      return rewrite(b, k, vv, ops, nops, hint);
    }
  }
}

static int spr_num(const char *n, int *spr, int *to)
{
  static const struct { const char *n; int spr; } sprs[] = {
    { "xer", 1 }, { "lr", 8 }, { "ctr", 9 }, { "dsisr", 18 }, { "dar", 19 }, { "dec", 22 },
    { "sdr1", 25 }, { "srr0", 26 }, { "srr1", 27 }, { "ear", 282 }, { "tbl", 284 }, { "tbu", 285 },
    { "pvr", 287 }, { "vrsave", 256 }, { NULL, 0 }
  };
  int i;
  *to = -1;
  for (i = 0; sprs[i].n; i++)
    if (!stricmp_(n, sprs[i].n)) { *spr = sprs[i].spr; return 1; }
  return 0;
}

static int extended(const char *mn, char **ops, int nops, int hint)
{
  char m[40], *p;
  char *v[6];
  size_t l = strlen(mn);
  int dot;
  if (l >= sizeof m - 2) return 0;
  strcpy(m, mn);
  for (p = m; *p; p++) *p = tolower((unsigned char)*p);
  dot = m[l - 1] == '.';
#define A(i) (i < nops ? ops[i] : "0")
#define R(base, n) rewrite(base, n, v, ops, nops, hint)
#define BASE(b) (dot ? b "." : b)
  if (!strcmp(m, "li") && nops == 2) { v[0] = xstrdup(A(0)); v[1] = xstrdup("0"); v[2] = xstrdup(A(1)); return R("addi", 3); }
  if (!strcmp(m, "lis") && nops == 2) { v[0] = xstrdup(A(0)); v[1] = xstrdup("0"); v[2] = xstrdup(A(1)); return R("addis", 3); }
  if (!strcmp(m, "la") && nops == 2) {
    char *t = xstrdup(ops[1]), *lp = strrchr(t, '(');
    size_t n = strlen(t);
    if (!lp || t[n - 1] != ')') { free(t); return 0; }
    t[n - 1] = 0; *lp = 0;
    v[0] = xstrdup(A(0)); v[1] = xstrdup(lp + 1); v[2] = xstrdup(*t ? t : "0");
    free(t);
    return R("addi", 3);
  }
  if ((!strcmp(m, "subi") || !strcmp(m, "subis") || !strcmp(m, "subic") || !strcmp(m, "subic.")) && nops == 3) {
    v[0] = xstrdup(A(0)); v[1] = xstrdup(A(1)); v[2] = fmt("-(%s)", A(2));
    return R(!strcmp(m, "subi") ? "addi" : !strcmp(m, "subis") ? "addis" : !strcmp(m, "subic") ? "addic" : "addic.", 3);
  }
  if (!strncmp(m, "sub", 3) && nops == 3) {
    const char *rest = m + 3;
    char b[64];
    if (strlen(rest) > 8) return 0;
    if (*rest == 'c') { rest++; snprintf(b, sizeof b, "subfc%s", rest); }
    else snprintf(b, sizeof b, "subf%s", rest);
    if (pfind(b, strlen(b))) { v[0] = xstrdup(A(0)); v[1] = xstrdup(A(2)); v[2] = xstrdup(A(1)); return R(b, 3); }
  }
  if ((!strcmp(m, "mr") || !strcmp(m, "mr.")) && nops == 2) { v[0] = xstrdup(A(0)); v[1] = xstrdup(A(1)); v[2] = xstrdup(A(1)); return R(BASE("or"), 3); }
  if ((!strcmp(m, "not") || !strcmp(m, "not.")) && nops == 2) { v[0] = xstrdup(A(0)); v[1] = xstrdup(A(1)); v[2] = xstrdup(A(1)); return R(BASE("nor"), 3); }
  if (!strcmp(m, "nop") && nops == 0) { v[0] = xstrdup("0"); v[1] = xstrdup("0"); v[2] = xstrdup("0"); return R("ori", 3); }
  if (!strcmp(m, "cmpwi") || !strcmp(m, "cmpw") || !strcmp(m, "cmplwi") || !strcmp(m, "cmplw")) {
    const char *b = !strcmp(m, "cmpwi") ? "cmpi" : !strcmp(m, "cmpw") ? "cmp" : !strcmp(m, "cmplwi") ? "cmpli" : "cmpl";
    if (nops == 2) { v[0] = xstrdup("0"); v[1] = xstrdup("0"); v[2] = xstrdup(A(0)); v[3] = xstrdup(A(1)); }
    else if (nops == 3) { v[0] = xstrdup(A(0)); v[1] = xstrdup("0"); v[2] = xstrdup(A(1)); v[3] = xstrdup(A(2)); }
    else return 0;
    return R(b, 4);
  }
  {
    /* rotate and shift */
    const char *b = dot ? "rlwinm." : "rlwinm";
    char base[16];
    strcpy(base, m);
    if (dot) base[l - 1] = 0;
#define IS(x) (!strcmp(base, x))
    if (IS("extlwi") && nops == 4) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=xstrdup(A(3)); v[3]=xstrdup("0"); v[4]=fmt("(%s)-1",A(2)); return R(b,5); }
    if (IS("extrwi") && nops == 4) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=fmt("(%s)+(%s)",A(3),A(2)); v[3]=fmt("32-(%s)",A(2)); v[4]=xstrdup("31"); return R(b,5); }
    if (IS("inslwi") && nops == 4) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=fmt("32-(%s)",A(3)); v[3]=xstrdup(A(3)); v[4]=fmt("(%s)+(%s)-1",A(3),A(2)); return R(dot?"rlwimi.":"rlwimi",5); }
    if (IS("insrwi") && nops == 4) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=fmt("32-((%s)+(%s))",A(3),A(2)); v[3]=xstrdup(A(3)); v[4]=fmt("(%s)+(%s)-1",A(3),A(2)); return R(dot?"rlwimi.":"rlwimi",5); }
    if (IS("rotlwi") && nops == 3) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=xstrdup(A(2)); v[3]=xstrdup("0"); v[4]=xstrdup("31"); return R(b,5); }
    if (IS("rotrwi") && nops == 3) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=fmt("32-(%s)",A(2)); v[3]=xstrdup("0"); v[4]=xstrdup("31"); return R(b,5); }
    if (IS("rotlw") && nops == 3) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=xstrdup(A(2)); v[3]=xstrdup("0"); v[4]=xstrdup("31"); return R(dot?"rlwnm.":"rlwnm",5); }
    if (IS("slwi") && nops == 3) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=xstrdup(A(2)); v[3]=xstrdup("0"); v[4]=fmt("31-(%s)",A(2)); return R(b,5); }
    if (IS("srwi") && nops == 3) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=fmt("32-(%s)",A(2)); v[3]=xstrdup(A(2)); v[4]=xstrdup("31"); return R(b,5); }
    if (IS("clrlwi") && nops == 3) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=xstrdup("0"); v[3]=xstrdup(A(2)); v[4]=xstrdup("31"); return R(b,5); }
    if (IS("clrrwi") && nops == 3) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=xstrdup("0"); v[3]=xstrdup("0"); v[4]=fmt("31-(%s)",A(2)); return R(b,5); }
    if (IS("clrlslwi") && nops == 4) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=xstrdup(A(3)); v[3]=fmt("(%s)-(%s)",A(2),A(3)); v[4]=fmt("31-(%s)",A(3)); return R(b,5); }
#undef IS
  }
  /* special purpose registers */
  if ((!strncmp(m, "mt", 2) || !strncmp(m, "mf", 2)) && strlen(m) > 2) {
    int spr, to, mt = m[1] == 't';
    const char *r = m + 2;
    if (!mt && (!strcmp(r, "tbu") || !strcmp(r, "tbl")) && nops == 1) {
      v[0] = xstrdup(A(0)); v[1] = xstrdup(r[2] == 'u' ? "269" : "268");
      return R("mftb", 2);
    }
    if (spr_num(r, &spr, &to)) {
      if (nops != 1) return 0;
      if (mt) { v[0] = fmt("%d", spr); v[1] = xstrdup(A(0)); }
      else { v[0] = xstrdup(A(0)); v[1] = fmt("%d", spr); }
      return R(mt ? "mtspr" : "mfspr", 2);
    }
    if (!strcmp(r, "sprg") && nops == 2) {
      if (mt) { v[0] = fmt("272+(%s)", A(0)); v[1] = xstrdup(A(1)); }
      else { v[0] = xstrdup(A(0)); v[1] = fmt("272+(%s)", A(1)); }
      return R(mt ? "mtspr" : "mfspr", 2);
    }
    if ((!strcmp(r, "ibatu") || !strcmp(r, "ibatl") || !strcmp(r, "dbatu") || !strcmp(r, "dbatl")) && nops == 2) {
      int b = (r[0] == 'i' ? 528 : 536) + (r[4] == 'l');
      if (mt) { v[0] = fmt("%d+2*(%s)", b, A(0)); v[1] = xstrdup(A(1)); }
      else { v[0] = xstrdup(A(0)); v[1] = fmt("%d+2*(%s)", b, A(1)); }
      return R(mt ? "mtspr" : "mfspr", 2);
    }
    if (!strcmp(m, "mtcr") && nops == 1) { v[0] = xstrdup("0xff"); v[1] = xstrdup(A(0)); return R("mtcrf", 2); }
    if (!strcmp(m, "mftbu") && nops == 1) { v[0] = xstrdup(A(0)); v[1] = xstrdup("269"); return R("mftb", 2); }
  }
  if (!strcmp(m, "crset") && nops == 1) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(0)); v[2]=xstrdup(A(0)); return R("creqv",3); }
  if (!strcmp(m, "crclr") && nops == 1) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(0)); v[2]=xstrdup(A(0)); return R("crxor",3); }
  if (!strcmp(m, "crmove") && nops == 2) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=xstrdup(A(1)); return R("cror",3); }
  if (!strcmp(m, "crnot") && nops == 2) { v[0]=xstrdup(A(0)); v[1]=xstrdup(A(1)); v[2]=xstrdup(A(1)); return R("crnor",3); }
  if (!strcmp(m, "trap") && nops == 0) { v[0]=xstrdup("31"); v[1]=xstrdup("0"); v[2]=xstrdup("0"); return R("tw",3); }
  if (!strncmp(m, "tw", 2) && strlen(m) > 2) {
    static const struct { const char *n; int to; } tos[] = {
      { "lt", 16 }, { "le", 20 }, { "eq", 4 }, { "ge", 12 }, { "gt", 8 }, { "nl", 12 }, { "ne", 24 },
      { "ng", 20 }, { "llt", 2 }, { "lle", 6 }, { "lge", 5 }, { "lgt", 1 }, { "lnl", 5 }, { "lng", 6 },
      { NULL, 0 } };
    int i;
    for (i = 0; tos[i].n; i++) {
      size_t n = strlen(tos[i].n);
      if (!strncmp(m + 2, tos[i].n, n) && (m[2 + n] == 0 || (m[2 + n] == 'i' && m[3 + n] == 0)) && nops == 2) {
        int imm = m[2 + n] == 'i';
        v[0] = fmt("%d", tos[i].to); v[1] = xstrdup(A(0)); v[2] = xstrdup(A(1));
        return R(imm ? "twi" : "tw", 3);
      }
    }
  }
  if (!strcmp(m, "blr") || !strcmp(m, "blrl") || !strcmp(m, "bctr") || !strcmp(m, "bctrl")) {
    if (nops) return 0;
    v[0] = xstrdup("20"); v[1] = xstrdup("0");
    return R(!strcmp(m, "blr") ? "bclr" : !strcmp(m, "blrl") ? "bclrl" : !strcmp(m, "bctr") ? "bcctr" : "bcctrl", 2);
  }
  return ext_branch(m, ops, nops, hint);
#undef A
#undef R
#undef BASE
}

static int ppc_instruction(const char *mn, size_t ml, const char *ext,
                           char **ops, int nops, int col)
{
  char name[48];
  int hint = 0;
  pent *pe;
  (void)ext; (void)col;
  if (ml >= sizeof name) return 0;
  memcpy(name, mn, ml);
  name[ml] = 0;
  if (ml > 1 && (name[ml - 1] == '+' || name[ml - 1] == '-')) { hint = name[ml - 1]; name[--ml] = 0; }
  pe = pfind(name, ml);
  if (pe && !(pe->m->flags & F_EXT)) return create(pe, ops, nops, hint, 0);
  return extended(name, ops, nops, hint);
}

/* ------------------------------------------------------------ encoding */
static void put_insn(bbuf *b, uint32_t w)
{
  if (ppc_little) { bb_byte(b, w); bb_byte(b, w >> 8); bb_byte(b, w >> 16); bb_byte(b, w >> 24); }
  else bb_be32(b, w);
}

static ival field(expr *e, int enc, eres *r)
{
  eval_expr(e, r);
  if (r->isflt) r->v = (ival)r->f;
  (void)enc;
  return r->v;
}

static void range(int enc, ival v, ival lo, ival hi)
{
  if (enc && (v < lo || v > hi))
    error("operand out of range: %lld (allowed: %lld to %lld)", (long long)v, (long long)lo, (long long)hi);
}

static uint32_t imm16(pinsn *ip, int i, int sgn, section *s, ival pc, int enc)
{
  eres r;
  ival v = field(ip->op[i], enc, &r);
  int mod = ip->mod[i];
  int negate = 0;
  if (r.bad && ip->op[i]->op == X_NEG) {
    /* subi rD,rA,label: vasm relocates +label but stores -label */
    eval_expr(ip->op[i]->l, &r);
    negate = 1;
  }
  if (r.sec || r.ext) {
    if (enc) {
      int type = R_ABS;
      if (mod == MOD_SDA || mod == MOD_SDA21 || mod == 100) type = R_SD;
      if (mod == 100) mod = MOD_NONE;
      add_reloc(s, pc + 2, 16, type, r.sec, r.ext, r.v);
      s->rlast->mod = mod;
    }
    v = negate ? -r.v : r.v;
    if (mod == MOD_H) return ((uint32_t)v >> 16) & 0xffff;
    if (mod == MOD_HA) return (((uint32_t)v + 0x8000) >> 16) & 0xffff;
    return (uint32_t)v & 0xffff;
  }
  if (mod == MOD_L) return (uint32_t)v & 0xffff;
  if (mod == MOD_H) return ((uint32_t)v >> 16) & 0xffff;
  if (mod == MOD_HA) return (((uint32_t)v + 0x8000) >> 16) & 0xffff;
  if (sgn) range(enc, v, -32768, 32767);
  else range(enc, v, -32768, 65535);
  return (uint32_t)v & 0xffff;
}

static uint32_t reg5(pinsn *ip, int i, int enc, int max)
{
  eres r;
  ival v = field(ip->op[i], enc, &r);
  if (enc && (r.sec || r.ext || v < 0 || v > max)) error("not a valid register (0-%d)", max);
  return (uint32_t)v & max;
}

static uint32_t uimm(pinsn *ip, int i, int enc, ival lo, ival hi, ival mask)
{
  eres r;
  ival v = field(ip->op[i], enc, &r);
  if (enc && (r.sec || r.ext)) error("constant integer expression required");
  range(enc, v, lo, hi);
  return (uint32_t)(v & mask);
}

static uint32_t branch(pinsn *ip, int i, section *s, ival pc, int bits, int enc)
{
  eres r;
  ival v = field(ip->op[i], enc, &r), d;
  uint32_t mask = bits == 24 ? 0x03fffffc : 0xfffc;
  if (ip->aa) {
    if (r.sec || r.ext) { if (enc) add_reloc(s, bits == 14 ? pc + 2 : pc, bits, R_ABS, r.sec, r.ext, r.v); return (uint32_t)r.v & mask; }
    d = v;
  } else if (r.sec == s && !r.ext) d = v - pc;
  else if (!r.sec && !r.ext && (s->flags & SEC_ABS)) d = v - (s->org + pc);
  else if (!r.sec && !r.ext) d = v - pc;  /* absolute target address */
  else { if (enc) add_reloc(s, bits == 14 ? pc + 2 : pc, bits, R_PC, r.sec, r.ext, r.v); return (uint32_t)r.v & mask; }
  if (enc) {
    if (bits == 24) range(enc, d, -0x2000000, 0x1ffffff);
    else range(enc, d, -32768, 32767);
  }
  ip->bdisp = d;
  ip->opc = ip->opc;   /* hint applied by caller */
  return (uint32_t)d & mask;
}

static uint32_t encode(pinsn *ip, section *s, ival pc, int enc)
{
  uint32_t w = ip->opc;
  int i;
  ival bd = 0;
  for (i = 0; i < ip->nops; i++) {
    char c = ip->ops[i];
    switch (c) {
    case 'D': case 'F': case 'V': w |= reg5(ip, i, enc, 31) << 21; break;
    case 'A': case 'Z': case 'G': case 'W': w |= reg5(ip, i, enc, 31) << 16; break;
    case 'B': case 'H': case 'X': w |= reg5(ip, i, enc, 31) << 11; break;
    case 'I': case 'Y': w |= reg5(ip, i, enc, 31) << 6; break;
    case 'c': w |= uimm(ip, i, enc, 0, 7, 7) << 23; break;
    case 's': w |= uimm(ip, i, enc, 0, 7, 7) << 18; break;
    case 'l': w |= uimm(ip, i, enc, 0, 1, 1) << 21; break;
    case 'b': w |= uimm(ip, i, enc, 0, 31, 31) << 21; break;
    case 'e': w |= uimm(ip, i, enc, 0, 31, 31) << 16; break;
    case 'f': w |= uimm(ip, i, enc, 0, 31, 31) << 11; break;
    case 'S': w |= imm16(ip, i, 1, s, pc, enc); break;
    case 'U': w |= imm16(ip, i, 0, s, pc, enc); break;
    case 'M': {
      eres r; ival v = field(ip->base[i], enc, &r);
      if (ip->mod[i] == MOD_NONE && (v == sdreg || v == sd2reg)) ip->mod[i] = 100;   /* base-relative */
      w |= imm16(ip, i, 1, s, pc, enc);
      if (ip->mod[i] == 100) ip->mod[i] = MOD_NONE;
      if (enc && (v < 0 || v > 31)) error("not a valid register (0-31)");
      w |= ((uint32_t)v & 31) << 16;
    } break;
    case 'L': w |= branch(ip, i, s, pc, 24, enc); break;
    case 'T':
      ip->bdisp = 0;
      w |= branch(ip, i, s, pc, 14, enc);
      bd = ip->bdisp;
      break;
    case 'O': w |= uimm(ip, i, enc, 0, 31, 31) << 21; break;
    case 'i': w |= uimm(ip, i, enc, 0, 31, 31) << 16; break;
    case 'h': w |= uimm(ip, i, enc, 0, 32, 31) << 11; break;
    case 'm':
      if (ip->flags & 0x1000) {
        /* mask -> MB/ME */
        eres r;
        uint32_t mk = (uint32_t)field(ip->op[i], enc, &r), mb = 0, me = 31;
        if (mk) {
          int b;
          if ((mk & 0x80000001u) == 0x80000001u && mk != 0xffffffffu) {
            uint32_t inv = ~mk;
            for (b = 0; b < 32 && !(inv & (0x80000000u >> b)); b++) ;
            me = b - 1;
            for (b = 31; b >= 0 && !(inv & (0x80000000u >> b)); b--) ;
            mb = b + 1;
          } else {
            for (b = 0; b < 32 && !(mk & (0x80000000u >> b)); b++) ;
            mb = b;
            for (b = 31; b >= 0 && !(mk & (0x80000000u >> b)); b--) ;
            me = b;
          }
        }
        w |= (mb << 6) | (me << 1);
        i++;
      } else
        w |= uimm(ip, i, enc, 0, 31, 31) << 6;
      break;
    case 'n': w |= uimm(ip, i, enc, 0, 31, 31) << 1; break;
    case 'N': w |= uimm(ip, i, enc, 0, 32, 31) << 11; break;
    case 'o': w |= uimm(ip, i, enc, 0, 3, 3) << 21; break;
    case 'P': case 'g': {
      uint32_t v = uimm(ip, i, enc, 0, 1023, 1023);
      w |= ((v & 31) << 16) | ((v >> 5) << 11);
    } break;
    case 'R': w |= uimm(ip, i, enc, 0, 15, 15) << 16; break;
    case 'k': w |= uimm(ip, i, enc, 0, 255, 255) << 17; break;
    case 'K': w |= uimm(ip, i, enc, 0, 255, 255) << 12; break;
    case 'j': w |= uimm(ip, i, enc, 0, 15, 15) << 12; break;
    case 't': w |= uimm(ip, i, enc, 0, 31, 31) << 21; break;
    case 'y': w |= uimm(ip, i, enc, 0, 3, 3) << 21; break;
    case 'u': w |= uimm(ip, i, enc, 0, 31, 31) << 16; break;
    case 'v': w |= uimm(ip, i, enc, -16, 15, 31) << 16; break;
    case 'w': w |= uimm(ip, i, enc, 0, 15, 15) << 6; break;
    }
  }
  /* static branch prediction hint (y bit of BO) */
  if (ip->hint) {
    int setb;
    if (ip->ops[0] == 'O' && strchr(ip->ops, 'T'))
      setb = ip->hint == '+' ? bd >= 0 : bd < 0;
    else
      setb = ip->hint == '+';
    if (setb) w |= 0x00200000;
  }
  return w;
}

static ival ppc_size(atom *a, section *s, ival pc)
{
  (void)a; (void)s; (void)pc;
  return 4;
}

static void ppc_encode(atom *a, section *s, ival pc, bbuf *out)
{
  pinsn *ip = a->u.insn.insn;
  put_insn(out, encode(ip, s, pc, 1));
}

static int ppc_option(const char *o)
{
  if (!strcmp(o, "-big")) { ppc_little = 0; cpu_ppc.big_endian = 1; return 1; }
  if (!strcmp(o, "-little")) { ppc_little = 1; cpu_ppc.big_endian = 0; return 1; }
  if (!strcmp(o, "-no-regnames")) { no_regnames = 1; return 1; }
  if (!strcmp(o, "-opt-branch")) { opt_branch = 1; return 1; }
  if (!strncmp(o, "-sdreg=", 7)) { sdreg = atoi(o + 7); return 1; }
  if (!strncmp(o, "-sd2reg=", 8)) { sd2reg = atoi(o + 8); return 1; }
  if (!strncmp(o, "-m", 2)) return 1;   /* cpu model selections */
  if (!strcmp(o, "-many")) return 1;
  return 0;
}

static int ppc_directive(const char *name, const char *args)
{
  ival v;
  const char *p = skip_ws(args);
  if (!stricmp_(name, ".sdreg") || !stricmp_(name, ".sd2reg")) {
    expr *e = parse_expr(&p);
    if (e && eval_const(e, &v)) { if (name[3] == '2') sd2reg = (int)v; else sdreg = (int)v; }
    return 1;
  }
  return 0;
}

static void defreg(const char *n, int v)
{
  symbol *s = sym_get(n);
  s->type = S_EXPR;
  s->e = new_num(v);
  s->flags |= SF_DEFINED | SF_INTERNAL;
}

static void ppc_define_symbols(void)
{
  int i;
  char b[8];
  set_internal(sym_get("__VASM"), 0);
  sym_get("__VASM")->flags |= SF_INTERNAL;
  if (no_regnames) return;
  for (i = 0; i < 32; i++) {
    snprintf(b, sizeof b, "r%d", i); defreg(b, i);
    snprintf(b, sizeof b, "f%d", i); defreg(b, i);
    snprintf(b, sizeof b, "v%d", i); defreg(b, i);
  }
  for (i = 0; i < 8; i++) { snprintf(b, sizeof b, "cr%d", i); defreg(b, i); }
  defreg("sp", 1); defreg("rtoc", 2); defreg("fp", 31); defreg("vrsave", 256);
  defreg("xer", 1); defreg("lr", 8); defreg("ctr", 9); defreg("fpscr", 0);
  defreg("lt", 0); defreg("gt", 1); defreg("eq", 2); defreg("so", 3); defreg("un", 3);
}

static int ppc_init(void)
{
  build();
  return 1;
}

static int ppc_elf_reloc(reloc *r)
{
  if (r->type == R_ABS) {
    if (r->size == 32) return 1;
    if (r->size == 24) return 2;
    if (r->size == 14) return 7;
    if (r->size == 16) return r->mod == MOD_L ? 4 : r->mod == MOD_H ? 5 : r->mod == MOD_HA ? 6 : 3;
  }
  if (r->type == R_PC) {
    if (r->size == 24) return 10;
    if (r->size == 14) return 11;
    if (r->size == 32) return 26;
  }
  if (r->type == R_SD) return r->mod == MOD_SDA21 ? 109 : 32;
  return -1;
}

cpu_module cpu_ppc = {
  "ppc",
  ppc_init,
  ppc_option,
  ppc_instruction,
  ppc_directive,
  ppc_size,
  ppc_encode,
  NULL,
  ppc_define_symbols,
  NULL,
  1, 4, 0, 32, 20, ppc_elf_reloc,
  0, 0, NULL, NULL
};
