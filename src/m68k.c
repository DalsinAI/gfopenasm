/* gfasm - Motorola 680x0 / 68881/2 / 68851 cpu module.  MIT License.
 *
 * Instruction encodings were written from the Motorola "M68000 Family
 * Programmer's Reference Manual" (M68000PM/AD), the MC68881/MC68882 and
 * MC68851 user's manuals.  Optimisation behaviour follows the vasm manual
 * (chapter "m68k cpu module") and comparison with vasm's output.
 */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

void encode_float(int ftype, long double f, uint8_t *o);

/* ------------------------------------------------------------ cpu set */
#define C000  0x0001
#define C010  0x0002
#define C020  0x0004
#define C030  0x0008
#define C040  0x0010
#define C060  0x0020
#define CCPU32 0x0040
#define CFPU  0x0080   /* 68881/2 */
#define CMMU  0x0100   /* 68851 */
#define C080  0x0200

#define C000UP (C000|C010|C020|C030|C040|C060|CCPU32|C080)
#define C010UP (C010|C020|C030|C040|C060|CCPU32|C080)
#define C020UP (C020|C030|C040|C060|C080)
#define C020CPU32 (C020UP|CCPU32)

static unsigned cpu_type = C000;
static int fpu_id = 1;
static int fpu_enabled;      /* 68881/2 or 040/060 internal */
static int mmu_851;
static int fpu_explicit;

/* options */
static int no_opt, opt_allbra, opt_brajmp, opt_clr, opt_div, opt_fconst = 1,
  opt_jbra, opt_lsl, opt_movem, opt_mul, opt_nmoveq, opt_pea, opt_speed,
  opt_size, opt_st, opt_sc, opt_sd, opt_showopt, opt_showcrit, opt_nodpc,
  opt_kick1, opt_guess_ext, opt_elfregs, opt_extsd, opt_gas, opt_sgs,
  opt_regsymredef, opt_rangewarnings, opt_conv_brackets, opt_phx, opt_dpac;
static int opt_pcrel = 1;          /* opt a */
static int opt_og = 1;             /* generic vasm opts */
static int sdreg = 4;
static int near_mode;              /* small data active */
static expr *basereg_expr[8];

/* ------------------------------------------------------------ registers */
enum { RK_NONE, RK_D, RK_A, RK_PC, RK_ZPC, RK_ZD, RK_ZA, RK_FP, RK_FPC,
       RK_SR, RK_CCR, RK_USP, RK_CTRL, RK_MMU, RK_CACHE };

typedef struct regname { const char *n; int kind; int num; } regname;
static const regname regnames[] = {
  { "pc", RK_PC, 0 }, { "zpc", RK_ZPC, 0 }, { "sp", RK_A, 7 },
  { "sr", RK_SR, 0 }, { "ccr", RK_CCR, 0 }, { "usp", RK_USP, 0x800 },
  { "fpcr", RK_FPC, 4 }, { "fpsr", RK_FPC, 2 }, { "fpiar", RK_FPC, 1 },
  { "sfc", RK_CTRL, 0x000 }, { "dfc", RK_CTRL, 0x001 }, { "cacr", RK_CTRL, 0x002 },
  { "itt0", RK_CTRL, 0x004 }, { "itt1", RK_CTRL, 0x005 },
  { "dtt0", RK_CTRL, 0x006 }, { "dtt1", RK_CTRL, 0x007 }, { "buscr", RK_CTRL, 0x008 },
  { "iacr0", RK_CTRL, 0x004 }, { "iacr1", RK_CTRL, 0x005 },
  { "dacr0", RK_CTRL, 0x006 }, { "dacr1", RK_CTRL, 0x007 },
  { "vbr", RK_CTRL, 0x801 }, { "caar", RK_CTRL, 0x802 }, { "msp", RK_CTRL, 0x803 },
  { "isp", RK_CTRL, 0x804 }, { "mmusr", RK_CTRL, 0x805 }, { "urp", RK_CTRL, 0x806 },
  { "srp", RK_CTRL, 0x807 }, { "pcr", RK_CTRL, 0x808 }, { "tc", RK_CTRL, 0x003 },
  /* mmu-only registers (pmove); numbers are internal ids */
  { "crp", RK_MMU, 3 }, { "drp", RK_MMU, 1 }, { "cal", RK_MMU, 4 }, { "val", RK_MMU, 5 },
  { "scc", RK_MMU, 6 }, { "ac", RK_MMU, 7 }, { "psr", RK_MMU, 0x18 }, { "pcsr", RK_MMU, 0x19 },
  { "tt0", RK_MMU, 0x22 }, { "tt1", RK_MMU, 0x23 },
  { "nc", RK_CACHE, 0 }, { "dc", RK_CACHE, 1 }, { "ic", RK_CACHE, 2 }, { "bc", RK_CACHE, 3 },
  { NULL, 0, 0 }
};

static int parse_regname(const char **pp, int *num)
{
  const char *p = *pp;
  const char *s;
  size_t l;
  int i;
  if (opt_elfregs) { if (*p != '%') return RK_NONE; p++; }
  s = p;
  if (!isalpha((unsigned char)*p)) return RK_NONE;
  while (isalnum((unsigned char)*p) || *p == '_') p++;
  l = p - s;
  if (l == 2 && (tolower((unsigned char)s[0]) == 'd' || tolower((unsigned char)s[0]) == 'a') &&
      s[1] >= '0' && s[1] <= '7') {
    *num = s[1] - '0';
    *pp = p;
    return tolower((unsigned char)s[0]) == 'd' ? RK_D : RK_A;
  }
  if (l == 3 && tolower((unsigned char)s[0]) == 'z' &&
      (tolower((unsigned char)s[1]) == 'd' || tolower((unsigned char)s[1]) == 'a') &&
      s[2] >= '0' && s[2] <= '7') {
    *num = s[2] - '0';
    *pp = p;
    return tolower((unsigned char)s[1]) == 'd' ? RK_ZD : RK_ZA;
  }
  if (l == 3 && tolower((unsigned char)s[0]) == 'f' && tolower((unsigned char)s[1]) == 'p' &&
      s[2] >= '0' && s[2] <= '7') {
    *num = s[2] - '0';
    *pp = p;
    return RK_FP;
  }
  if (l == 4 && !strnicmp_(s, "bad", 3) && s[3] >= '0' && s[3] <= '7') {
    *num = 0x30 + s[3] - '0'; *pp = p; return RK_MMU;
  }
  if (l == 4 && !strnicmp_(s, "bac", 3) && s[3] >= '0' && s[3] <= '7') {
    *num = 0x38 + s[3] - '0'; *pp = p; return RK_MMU;
  }
  for (i = 0; regnames[i].n; i++)
    if (strlen(regnames[i].n) == l && !strnicmp_(s, regnames[i].n, l)) {
      *num = regnames[i].num;
      *pp = p;
      return regnames[i].kind;
    }
  if (!opt_elfregs) {
    char *n = xstrndup(s, l);
    symbol *sy = sym_lookup(n);
    free(n);
    if (sy && sy->type == S_REG) {
      *num = (int)sy->val & 7;
      *pp = p;
      return sy->val >= 8 ? RK_A : RK_D;
    }
    if (sy && sy->type == S_FREG) {
      *num = (int)sy->val & 7;
      *pp = p;
      return RK_FP;
    }
  }
  return RK_NONE;
}

static int m68k_parse_reg(const char **pp)
{
  int n, k;
  *pp = skip_ws(*pp);
  k = parse_regname(pp, &n);
  if (k == RK_D) return n;
  if (k == RK_A) return 8 + n;
  if (k == RK_FP) return n;
  return -1;
}

/* register list: bit0=d0..bit15=a7; fp: bit0=fp0..; fpcr list: bits 8.. */
static int parse_reglist(const char **pp, uint32_t *mask, int *isfp)
{
  const char *p = *pp;
  uint32_t m = 0;
  int fp = -1;
  for (;;) {
    int n, k, n2, k2;
    const char *q = p;
    k = parse_regname(&q, &n);
    if (k == RK_NONE) {
      const char *s = q;
      char *name;
      symbol *sy;
      while (isalnum((unsigned char)*q) || *q == '_' || *q == '.') q++;
      if (q == s) return 0;
      name = xstrndup(s, q - s);
      sy = sym_lookup(name);
      free(name);
      if (!sy || (sy->type != S_REGLIST && sy->type != S_FREGLIST)) return 0;
      m |= (uint32_t)sy->val;
      fp = sy->type == S_FREGLIST ? ((sy->val & 0xff00) ? 2 : 1) : 0;
      p = q;
      goto next;
    }
    if (k == RK_D || k == RK_A) {
      int r = (k == RK_A ? 8 : 0) + n;
      if (fp >= 1) return 0;
      fp = 0;
      if (*q == '-') {
        const char *q2 = q + 1;
        k2 = parse_regname(&q2, &n2);
        if (k2 == RK_D || k2 == RK_A) {
          int r2 = (k2 == RK_A ? 8 : 0) + n2;
          int i;
          if (r2 < r) { int t = r; r = r2; r2 = t; }
          for (i = r; i <= r2; i++) m |= 1u << i;
          q = q2;
        } else if (isdigit((unsigned char)q[1])) {
          int r2 = (k == RK_A ? 8 : 0) + (q[1] - '0');
          int i;
          for (i = r; i <= r2; i++) m |= 1u << i;
          q += 2;
        } else return 0;
      } else
        m |= 1u << r;
    } else if (k == RK_FP) {
      if (fp == 0 || fp == 2) return 0;
      fp = 1;
      if (*q == '-') {
        const char *q2 = q + 1;
        k2 = parse_regname(&q2, &n2);
        if (k2 == RK_FP) {
          int i;
          if (n2 < n) { int t = n; n = n2; n2 = t; }
          for (i = n; i <= n2; i++) m |= 1u << i;
          q = q2;
        } else if (isdigit((unsigned char)q[1])) {
          int i;
          for (i = n; i <= q[1] - '0'; i++) m |= 1u << i;
          q += 2;
        } else return 0;
      } else
        m |= 1u << n;
    } else if (k == RK_FPC) {
      if (fp == 0 || fp == 1) return 0;
      fp = 2;
      m |= (uint32_t)n << 8;
    } else
      return 0;
    p = q;
  next:
    if (*p == '/') { p++; continue; }
    break;
  }
  *pp = p;
  *mask = m;
  *isfp = fp;
  return 1;
}

int m68k_parse_reglist_str(const char *s, uint32_t *mask, int fp)
{
  const char *p = skip_ws(s);
  int isfp;
  (void)fp;
  if (!parse_reglist(&p, mask, &isfp)) return 0;
  p = skip_ws(p);
  return *p == 0;
}

/* ------------------------------------------------------------ operands */
enum {
  M_NONE, M_DN, M_AN, M_IND, M_POST, M_PRE, M_MEM, M_ABS, M_IMM,
  M_REGLIST, M_FP, M_FPCR, M_SR, M_CCR, M_USP, M_CTRL, M_MMU, M_PAIR,
  M_INDPAIR, M_CACHE
};

#define BASE_NONE (-1)
#define BASE_PC   16
#define BASE_ZPC  17

typedef struct operand {
  int mode;
  int reg;
  int base;          /* M_MEM: 0..7 = An, BASE_PC, BASE_ZPC, BASE_NONE */
  int zbase;         /* ZAn */
  expr *bd, *od;
  int bdsz, odsz;    /* forced size 0/1/2/4 */
  int xn, xsz, scale, xsupp;
  int memind;        /* 0, 1=pre-indexed, 2=post-indexed, 3=no index */
  int hasod;
  uint32_t mask;
  int fpmask;        /* reglist type: 0 int, 1 fp, 2 fpcr */
  int reg2;
  int bf; int bfo_reg, bfw_reg; expr *bfo, *bfw;
  int kf; int kf_reg; expr *kfe;
  int isreglist1;
  int rawhex;        /* #$..., #%..., #@... : raw bit pattern for FP sizes */
  /* resolution results (per pass) */
  int form;
  int extlen;
  ival disp;
} operand;

static int parse_index(const char **pp, operand *o)
{
  const char *p = *pp;
  int n, k = parse_regname(&p, &n);
  if (k != RK_D && k != RK_A && k != RK_ZD && k != RK_ZA) return 0;
  o->xn = ((k == RK_A || k == RK_ZA) ? 8 : 0) + n;
  o->xsupp = (k == RK_ZD || k == RK_ZA);
  o->xsz = 2;
  o->scale = 1;
  if (*p == '.') {
    int c = tolower((unsigned char)p[1]);
    if (c == 'w') o->xsz = 2;
    else if (c == 'l') o->xsz = 4;
    else return 0;
    p += 2;
  }
  if (*p == '*') {
    p++;
    if (*p == '1') o->scale = 1;
    else if (*p == '2') o->scale = 2;
    else if (*p == '4') o->scale = 4;
    else if (*p == '8') o->scale = 8;
    else { error("illegal scale factor"); return 0; }
    p++;
  }
  *pp = p;
  return 1;
}

static int parse_dsize(const char **pp, int *sz)
{
  const char *p = *pp;
  if (*p == '.' && p[1] && !isalnum((unsigned char)p[2]) && p[2] != '_') {
    int c = tolower((unsigned char)p[1]);
    if (c == 'w') { *sz = 2; *pp = p + 2; return 1; }
    if (c == 'l') { *sz = 4; *pp = p + 2; return 1; }
    if (c == 'b' || c == 's') { *sz = 1; *pp = p + 2; return 1; }
  }
  return 0;
}

static int parse_paren(const char **pp, operand *o, int inbracket)
{
  const char *p = *pp;
  int nregs = 0;
  char close = inbracket ? ']' : ')';
  for (;;) {
    const char *q;
    int n, k;
    p = skip_ws(p);
    if (*p == '[' && !inbracket) {
      p++;
      o->memind = 3;
      if (!parse_paren(&p, o, 1)) return 0;
      nregs++;
      if (o->xn >= 0) o->memind = 1;
      goto sep;
    }
    q = p;
    k = parse_regname(&q, &n);
    if (k == RK_A || k == RK_PC || k == RK_ZPC || k == RK_ZA || k == RK_D || k == RK_ZD) {
      int isidx = (*q == '.' || *q == '*');
      if (!isidx && o->base == BASE_NONE && !o->zbase &&
          (k == RK_A || k == RK_PC || k == RK_ZPC || k == RK_ZA) &&
          !(inbracket == 0 && o->memind)) {
        if (k == RK_A) o->base = n;
        else if (k == RK_PC) o->base = BASE_PC;
        else if (k == RK_ZPC) o->base = BASE_ZPC;
        else { o->zbase = 1; o->base = BASE_NONE; o->reg = n; }
        p = q;
      } else {
        if (o->xn >= 0) return 0;
        if (!parse_index(&p, o)) return 0;
        if (o->memind && !inbracket) o->memind = 2;
      }
      nregs++;
    } else {
      expr *e;
      int sz = 0;
      e = parse_expr(&p);
      if (!e) return 0;
      parse_dsize(&p, &sz);
      if (o->memind && !inbracket) { o->od = e; o->odsz = sz; o->hasod = 1; }
      else { if (o->bd) return 0; o->bd = e; o->bdsz = sz; }
    }
  sep:
    p = skip_ws(p);
    if (*p == ',') { p++; continue; }
    if (*p == close) { p++; break; }
    return 0;
  }
  *pp = p;
  return nregs > 0 || inbracket;
}

static int parse_bitfield(const char **pp, operand *o)
{
  const char *p;
  int n, k;
  const char *q = skip_ws(*pp + 1);
  o->bf = 1;
  k = parse_regname(&q, &n);
  if (k == RK_D) { o->bfo_reg = n; p = q; }
  else { o->bfo_reg = -1; if (*q == '#') q++; p = q; o->bfo = parse_expr(&p); if (!o->bfo) return 0; }
  p = skip_ws(p);
  if (*p != ':') return 0;
  q = skip_ws(p + 1);
  k = parse_regname(&q, &n);
  if (k == RK_D) { o->bfw_reg = n; p = q; }
  else { o->bfw_reg = -1; if (*q == '#') q++; p = q; o->bfw = parse_expr(&p); if (!o->bfw) return 0; }
  p = skip_ws(p);
  if (*p != '}') return 0;
  *pp = p + 1;
  return 1;
}

static int parse_kfactor(const char **pp, operand *o)
{
  const char *p = skip_ws(*pp + 1);
  int n, k;
  const char *q = p;
  o->kf = 1;
  k = parse_regname(&q, &n);
  if (k == RK_D) { o->kf_reg = n; p = q; }
  else {
    o->kf_reg = -1;
    if (*p == '#') p++;
    o->kfe = parse_expr(&p);
    if (!o->kfe) return 0;
  }
  p = skip_ws(p);
  if (*p != '}') return 0;
  *pp = p + 1;
  return 1;
}

static void op_clear(operand *o)
{
  memset(o, 0, sizeof(*o));
  o->xn = -1;
  o->base = BASE_NONE;
  o->bfo_reg = o->bfw_reg = -1;
}

static int parse_suffix_brace(const char **pr, operand *o)
{
  const char *r = *pr;
  if (*r == '{') {
    const char *t = skip_ws(r + 1);
    int n;
    if (*t == '#') { if (!parse_kfactor(&r, o)) return 0; }
    else {
      const char *t2 = t;
      int k = parse_regname(&t2, &n);
      if (k == RK_D && *skip_ws(t2) == '}') { if (!parse_kfactor(&r, o)) return 0; }
      else if (!parse_bitfield(&r, o)) return 0;
    }
  }
  *pr = r;
  return 1;
}

static int parse_operand(const char *s, operand *o)
{
  const char *p = skip_ws(s);
  const char *q;
  int n, k;
  op_clear(o);
  if (!*p) return 0;
  if (*p == '#' || (opt_sgs && *p == '&')) {
    p++;
    o->mode = M_IMM;
    o->rawhex = (*p == '$' || *p == '%' || (*p == '@' && p[1] >= '0' && p[1] <= '7'));
    o->bd = parse_expr(&p);
    if (!o->bd) return 0;
    p = skip_ws(p);
    return *p == 0;
  }
  q = p;
  k = parse_regname(&q, &n);
  if (k != RK_NONE) {
    if ((k == RK_D || k == RK_A || k == RK_FP || k == RK_FPC) && (*q == '/' || *q == '-')) {
      uint32_t m;
      int fp;
      const char *r = p;
      if (parse_reglist(&r, &m, &fp) && !*skip_ws(r)) {
        o->mode = fp == 2 ? M_FPCR : M_REGLIST;
        o->fpmask = fp;
        o->mask = fp == 2 ? m >> 8 : m;
        return 1;
      }
    }
    if (k == RK_D || k == RK_A) {
      if (*q == ':') {
        const char *r = q + 1;
        int n2, k2 = parse_regname(&r, &n2);
        if (k2 == RK_D || k2 == RK_A) {
          o->mode = M_PAIR;
          o->reg = (k == RK_A ? 8 : 0) + n;
          o->reg2 = (k2 == RK_A ? 8 : 0) + n2;
          return !*skip_ws(r);
        }
        return 0;
      }
      if (k == RK_D && *q == '{') {
        o->mode = M_DN;
        o->reg = n;
        p = q;
        if (!parse_bitfield(&p, o)) return 0;
        return !*skip_ws(p);
      }
      if (!*skip_ws(q)) {
        o->mode = k == RK_D ? M_DN : M_AN;
        o->reg = n;
        o->mask = 1u << ((k == RK_A ? 8 : 0) + n);
        o->isreglist1 = 1;
        return 1;
      }
    } else if (k == RK_FP) {
      if (*q == ':') {
        const char *r = q + 1;
        int n2, k2 = parse_regname(&r, &n2);
        if (k2 == RK_FP) {
          o->mode = M_PAIR;
          o->reg = n;
          o->reg2 = n2;
          o->fpmask = 1;
          return !*skip_ws(r);
        }
        return 0;
      }
      if (!*skip_ws(q)) { o->mode = M_FP; o->reg = n; o->mask = 1u << n; o->fpmask = 1; return 1; }
    } else if (k == RK_FPC) {
      if (!*skip_ws(q)) { o->mode = M_FPCR; o->mask = n; o->reg = n; o->fpmask = 2; return 1; }
    } else if (!*skip_ws(q)) {
      switch (k) {
      case RK_SR: o->mode = M_SR; return 1;
      case RK_CCR: o->mode = M_CCR; return 1;
      case RK_USP: o->mode = M_USP; o->reg = n; return 1;
      case RK_CTRL: o->mode = M_CTRL; o->reg = n; return 1;
      case RK_MMU: o->mode = M_MMU; o->reg = n; return 1;
      case RK_CACHE: o->mode = M_CACHE; o->reg = n; return 1;
      default: break;
      }
    }
  }
  /* register list symbol */
  {
    const char *r = p;
    while (isalnum((unsigned char)*r) || *r == '_' || *r == '.') r++;
    if (r > p) {
      char *nm = xstrndup(p, r - p);
      symbol *sy = sym_lookup(nm);
      free(nm);
      if (sy && (sy->type == S_REGLIST || sy->type == S_FREGLIST)) {
        uint32_t m;
        int fp;
        const char *t = p;
        if (parse_reglist(&t, &m, &fp) && !*skip_ws(t)) {
          o->mode = fp == 2 ? M_FPCR : M_REGLIST;
          o->fpmask = fp;
          o->mask = fp == 2 ? m >> 8 : m;
          return 1;
        }
      }
    }
  }
  if (*p == '-' && p[1] == '(') {
    const char *r = skip_ws(p + 2);
    k = parse_regname(&r, &n);
    r = skip_ws(r);
    if (k == RK_A && *r == ')' && !*skip_ws(r + 1)) {
      o->mode = M_PRE;
      o->reg = n;
      return 1;
    }
  }
  if (*p == '(') {
    const char *r = skip_ws(p + 1);
    int n1, k1 = parse_regname(&r, &n1);
    r = skip_ws(r);
    if ((k1 == RK_D || k1 == RK_A) && *r == ')' && r[1] == ':' && r[2] == '(') {
      const char *t = skip_ws(r + 3);
      int n2, k2 = parse_regname(&t, &n2);
      t = skip_ws(t);
      if ((k2 == RK_D || k2 == RK_A) && *t == ')' && !*skip_ws(t + 1)) {
        o->mode = M_INDPAIR;
        o->reg = (k1 == RK_A ? 8 : 0) + n1;
        o->reg2 = (k2 == RK_A ? 8 : 0) + n2;
        return 1;
      }
    }
  }
  if (*p == '(') {
    const char *r = p + 1;
    operand t;
    op_clear(&t);
    diag_suppress++;
    if (parse_paren(&r, &t, 0)) {
      diag_suppress--;
      *o = t;
      r = skip_ws(r);
      if (*r == '+' && o->base >= 0 && o->base < 8 && !o->bd && o->xn < 0 && !o->memind) {
        o->mode = M_POST;
        o->reg = o->base;
        r++;
      } else if (o->base >= 0 && o->base < 8 && !o->bd && o->xn < 0 && !o->memind && !o->zbase) {
        o->mode = M_IND;
        o->reg = o->base;
      } else
        o->mode = M_MEM;
      r = skip_ws(r);
      if (!parse_suffix_brace(&r, o)) return 0;
      return !*skip_ws(r);
    }
    diag_suppress--;
    op_clear(o);
  }
  {
    const char *r = p;
    expr *e = parse_expr(&r);
    int sz = 0;
    if (!e) return 0;
    parse_dsize(&r, &sz);
    r = skip_ws(r);
    if (*r == '(') {
      const char *t = r + 1;
      o->bd = e;
      o->bdsz = sz;
      if (!parse_paren(&t, o, 0)) return 0;
      o->mode = M_MEM;
      r = skip_ws(t);
    } else {
      o->mode = M_ABS;
      o->bd = e;
      o->bdsz = sz;
    }
    if (!parse_suffix_brace(&r, o)) return 0;
    return !*skip_ws(r);
  }
}

/* ------------------------------------------------------- instructions */
#define MAXOPS 4

struct mnem;
typedef struct insn {
  const struct mnem *m;
  int size;            /* 0,'b','w','l','s','d','x','p' */
  int nops;
  operand op[MAXOPS];
  unsigned cpu;        /* cpu set at parse time */
  int fpuid;
  int noopt;           /* optimisations disabled at parse time */
  int optflags;
  ival lastsize;
  int lastform;        /* branch form memory for grow_only */
  int sdreg, near;
  expr *basereg[8];
  atom *self;
} insn;

typedef struct ictx {
  insn *ip;
  section *sec;
  ival pc;
  bbuf *out;
  int enc;             /* 1 = encoding pass */
  int len;             /* bytes emitted so far */
  int jmppc;           /* jmp/jsr (d,pc): keep 16-bit form, no range check */
} ictx;

typedef int (*handler)(ictx *c);
typedef struct mnem {
  const char *name;
  uint16_t opc;
  uint16_t opc2;
  uint8_t sizes;
  char defsize;
  unsigned cpus;
  handler h;
  int arg;
} mnem;

#define SZ_B 1
#define SZ_W 2
#define SZ_L 4
#define SZ_S 8
#define SZ_D 16
#define SZ_X 32
#define SZ_P 64
#define SZ_U 128   /* unsized allowed */
#define SZ_BWL (SZ_B|SZ_W|SZ_L)
#define SZ_F (SZ_B|SZ_W|SZ_L|SZ_S|SZ_D|SZ_X|SZ_P)

/* ea classes */
#define A_DN   0x001
#define A_AN   0x002
#define A_IND  0x004
#define A_POST 0x008
#define A_PRE  0x010
#define A_D16  0x020
#define A_IDX  0x040
#define A_ABSW 0x080
#define A_ABSL 0x100
#define A_PCD  0x200
#define A_PCI  0x400
#define A_IMM  0x800
#define EA_ALL  0xfff
#define EA_DATA (EA_ALL & ~A_AN)
#define EA_MEM  (EA_DATA & ~A_DN)
#define EA_CTRL (A_IND|A_D16|A_IDX|A_ABSW|A_ABSL|A_PCD|A_PCI)
#define EA_ALT  (EA_ALL & ~(A_PCD|A_PCI|A_IMM))
#define EA_DALT (EA_DATA & EA_ALT)
#define EA_MALT (EA_MEM & EA_ALT)
#define EA_CALT (EA_CTRL & EA_ALT)

enum { F_NONE, F_DN, F_AN, F_IND, F_POST, F_PRE, F_D16, F_BRIEF, F_FULL,
       F_ABSW, F_ABSL, F_PCD16, F_PCBRIEF, F_PCFULL, F_IMM };

static unsigned form_class(int f)
{
  switch (f) {
  case F_DN: return A_DN;
  case F_AN: return A_AN;
  case F_IND: return A_IND;
  case F_POST: return A_POST;
  case F_PRE: return A_PRE;
  case F_D16: return A_D16;
  case F_BRIEF: case F_FULL: return A_IDX;
  case F_ABSW: return A_ABSW;
  case F_ABSL: return A_ABSL;
  case F_PCD16: return A_PCD;
  case F_PCBRIEF: case F_PCFULL: return A_PCI;
  case F_IMM: return A_IMM;
  }
  return 0;
}

#define IS020(c) ((c)->ip->cpu & (C020UP|CCPU32))
#define OPT(c) (!(c)->ip->noopt)

static int fits8(ival v) { return v >= -128 && v <= 127; }
static int fits16(ival v) { return v >= -32768 && v <= 32767; }

static int imm_len(int sz)
{
  switch (sz) {
  case 'b': case 'w': return 2;
  case 'l': case 's': return 4;
  case 'd': return 8;
  case 'x': case 'p': return 12;
  }
  return 2;
}

/* ---- emission */
static void emit16(ictx *c, uint32_t v)
{
  if (c->enc) bb_be16(c->out, v & 0xffff);
  c->len += 2;
}
static void emit32(ictx *c, uint32_t v)
{
  if (c->enc) bb_be32(c->out, v);
  c->len += 4;
}

static int is_abs(const eres *r) { return !r->sec && !r->ext && !r->bad; }

static void eval_op(ictx *c, expr *e, eres *r)
{
  (void)c;
  if (!e) { memset(r, 0, sizeof(*r)); r->defined = 1; return; }
  eval_expr(e, r);
  if (r->isflt) { r->v = (ival)r->f; }
}

/* emit a value of 'bytes' (1 -> low byte of a word, 2, 4) at the
 * current position.  kind: 0 abs, 1 pc-relative to 'base' (section offset) */
static void emit_val(ictx *c, const eres *r0, int bytes, int kind, ival base,
                     ival lo, ival hi, const char *rangemsg)
{
  eres r = *r0;
  ival off = c->pc + c->len;
  int wbytes = bytes == 1 ? 2 : bytes;
  ival v = r.v;
  if (c->enc) {
    if (r.bad) { error("illegal relocation"); v = 0; }
    else if (kind == 1) {
      if (r.sec == c->sec && !r.ext) v = r.v - base;
      else if (!r.sec && !r.ext) {
        if (c->sec->flags & SEC_ABS) v = r.v - (c->sec->org + base);
        else v = r.v;   /* absolute displacement encoded directly */
      } else {
        ival P = off + (bytes == 1 ? 1 : 0);
        v = r.v + (P - base);
        add_reloc(c->sec, P, bytes * 8, R_PC, r.sec, r.ext, v);
      }
    } else if (r.sec || r.ext) {
      add_reloc(c->sec, off + (bytes == 1 ? 1 : 0), bytes * 8, R_ABS, r.sec, r.ext, r.v);
    }
    if (is_abs(&r) || (kind == 1 && r.sec == c->sec)) {
      if (v < lo || v > hi) {
        if (opt_rangewarnings) warning("%s", rangemsg);
        else error("%s", rangemsg);
      }
    }
    if (bytes == 1) v &= 0xff;
    if (wbytes == 2) bb_be16(c->out, (uint32_t)v & 0xffff);
    else bb_be32(c->out, (uint32_t)v);
  }
  c->len += wbytes;
}

/* ------------------------------------------------- ea resolution */
static int base_adjust(ictx *c, int an, eres *r)
{
  /* basereg: subtract the base expression */
  expr *b = (an >= 0 && an < 8) ? c->ip->basereg[an] : NULL;
  if (b) {
    eres br;
    eval_expr(b, &br);
    if (br.sec && br.sec == r->sec) { r->v -= br.v; r->sec = NULL; return 1; }
    if (!br.sec && !br.ext && !r->sec && !r->ext) { r->v -= br.v; return 1; }
  }
  return 0;
}

/* pc-relative displacement for target r at extension address 'ea' */
static int pc_disp(ictx *c, const eres *r, ival ea, ival *d)
{
  if (r->sec && r->sec == c->sec && !r->ext) { *d = r->v - ea; return 1; }
  if (is_abs(r)) {
    if (c->sec->flags & SEC_ABS) { *d = r->v - (c->sec->org + ea); return 1; }
    *d = r->v;
    return 1;
  }
  *d = 0;
  return 0;  /* needs relocation */
}

static int bdsize_for(ictx *c, expr *e, int forced, int is_pc, ival extaddr, int an, int *bdlen)
{
  eres r;
  ival d;
  (void)c;
  if (forced == 4) { *bdlen = 4; return 3; }
  if (forced == 2) { *bdlen = 2; return 2; }
  if (!e) { *bdlen = 0; return 1; }
  eval_op(c, e, &r);
  if (is_pc) {
    if (!pc_disp(c, &r, extaddr, &d)) { *bdlen = 4; return 3; }
  } else {
    base_adjust(c, an, &r);
    if (!is_abs(&r)) { *bdlen = 4; return 3; }
    d = r.v;
  }
  if (!forced && d == 0 && !(is_pc && r.sec)) { *bdlen = 0; return 1; }
  if (fits16(d)) { *bdlen = 2; return 2; }
  *bdlen = 4;
  return 3;
}

static int ea_resolve(ictx *c, operand *o, unsigned allowed, int isz, int extoff)
{
  eres r;
  ival d;
  ival extaddr = c->pc + extoff;
  o->extlen = 0;
  o->form = F_NONE;
  switch (o->mode) {
  case M_DN: o->form = F_DN; break;
  case M_AN: o->form = F_AN; break;
  case M_IND: o->form = F_IND; break;
  case M_POST: o->form = F_POST; break;
  case M_PRE: o->form = F_PRE; break;
  case M_IMM: o->form = F_IMM; o->extlen = imm_len(isz); break;
  case M_ABS:
    eval_op(c, o->bd, &r);
    if (o->bdsz == 2 || o->bdsz == 1) { o->form = F_ABSW; o->extlen = 2; break; }
    if (o->bdsz == 4) { o->form = F_ABSL; o->extlen = 4; break; }
    if (OPT(c) && opt_pcrel && (allowed & A_PCD) && r.sec && r.sec == c->sec && !r.ext) {
      d = r.v - extaddr;
      if (fits16(d)) { o->form = F_PCD16; o->extlen = 2; o->disp = d; break; }
    }
    if (is_abs(&r) && r.defined && fits16(r.v) && OPT(c)) { o->form = F_ABSW; o->extlen = 2; break; }
    o->form = F_ABSL;
    o->extlen = 4;
    break;
  case M_MEM: {
    int is_pc = o->base == BASE_PC || o->base == BASE_ZPC;
    int simple = !o->memind && o->base != BASE_ZPC && !o->zbase && o->base != BASE_NONE &&
                 !(o->xn >= 0 && o->xsupp);
    if (simple && o->xn < 0) {
      int reloc = 0;
      eval_op(c, o->bd, &r);
      if (is_pc) {
        if (!pc_disp(c, &r, extaddr, &d)) reloc = 1;
        if (is_abs(&r) && !(c->sec->flags & SEC_ABS) && opt_nodpc) {
          d = r.v - extaddr;
        }
      } else {
        base_adjust(c, o->base, &r);
        if (!is_abs(&r)) reloc = 1;
        d = r.v;
      }
      o->disp = d;
      if (is_pc && c->jmppc && !o->bdsz) { o->form = F_PCD16; o->extlen = 2; break; }
      if (o->bdsz == 4 && IS020(c)) { o->form = is_pc ? F_PCFULL : F_FULL; o->extlen = 6; break; }
      if (!is_pc && !o->bdsz && !reloc && d == 0 && OPT(c) && (allowed & A_IND) && o->bd) {
        o->form = F_IND; o->reg = o->base; break;
      }
      if (reloc || fits16(d) || o->bdsz == 2 || !IS020(c) || !OPT(c)) {
        o->form = is_pc ? F_PCD16 : F_D16; o->extlen = 2; break;
      }
      o->form = is_pc ? F_PCFULL : F_FULL; o->extlen = 6;
      break;
    }
    if (simple) {
      int reloc = 0;
      eval_op(c, o->bd, &r);
      if (is_pc) { if (!pc_disp(c, &r, extaddr, &d)) reloc = 1; }
      else { base_adjust(c, o->base, &r); if (!is_abs(&r)) reloc = 1; d = r.v; }
      o->disp = d;
      if (!reloc && (o->bdsz == 0 || o->bdsz == 1) && fits8(d) && (o->scale == 1 || IS020(c)) &&
          !(o->bdsz == 0 && !OPT(c) && 0)) {
        o->form = is_pc ? F_PCBRIEF : F_BRIEF; o->extlen = 2; break;
      }
      if (!IS020(c)) { o->form = is_pc ? F_PCBRIEF : F_BRIEF; o->extlen = 2; break; }
    }
    /* full format */
    {
      int bdl, odl = 0;
      bdsize_for(c, o->bd, o->bdsz, is_pc, extaddr, o->base, &bdl);
      if (o->memind && o->hasod) {
        if (o->odsz == 4) odl = 4;
        else if (o->odsz == 2) odl = 2;
        else {
          eres orr;
          eval_op(c, o->od, &orr);
          if (!is_abs(&orr)) odl = 4;
          else if (orr.v == 0) odl = 0;
          else if (fits16(orr.v)) odl = 2;
          else odl = 4;
        }
      }
      o->form = is_pc ? F_PCFULL : F_FULL;
      o->extlen = 2 + bdl + odl;
    }
    break;
  }
  default:
    return 0;
  }
  return (form_class(o->form) & allowed) != 0;
}

static int ea_bits(const operand *o)
{
  switch (o->form) {
  case F_DN: return o->reg & 7;
  case F_AN: return 010 | (o->reg & 7);
  case F_IND: return 020 | (o->reg & 7);
  case F_POST: return 030 | (o->reg & 7);
  case F_PRE: return 040 | (o->reg & 7);
  case F_D16: return 050 | (o->base & 7);
  case F_BRIEF: return 060 | (o->base & 7);
  case F_FULL: return 060 | (o->base >= 0 && o->base < 8 ? o->base : (o->zbase ? o->reg & 7 : 0));
  case F_ABSW: return 070;
  case F_ABSL: return 071;
  case F_PCD16: return 072;
  case F_PCBRIEF: case F_PCFULL: return 073;
  case F_IMM: return 074;
  }
  return 0;
}

static int scale_bits(int s) { return s == 8 ? 3 : s == 4 ? 2 : s == 2 ? 1 : 0; }

static int imm_raw;
static void emit_imm(ictx *c, expr *e, int isz)
{
  eres r;
  if (isz == 's' || isz == 'd' || isz == 'x' || isz == 'p') {
    uint8_t buf[12];
    int n = imm_len(isz), i;
    eval_expr(e, &r);
    if (c->enc) {
      if (!is_abs(&r)) error("immediate operand has illegal type or size");
      if (imm_raw && !r.isflt) {
        uint64_t u = (uint64_t)r.v;
        if (r.v >= -0x80000000LL && r.v <= 0xffffffffLL) u = (uint32_t)r.v;
        memset(buf, 0, sizeof(buf));
        for (i = 0; i < 8 && i < n; i++) buf[n - 1 - i] = (uint8_t)(u >> (8 * i));
      } else
        encode_float(isz, r.isflt ? r.f : (long double)r.v, buf);
      for (i = 0; i < n; i += 2) bb_be16(c->out, (buf[i] << 8) | buf[i + 1]);
    }
    c->len += n;
    return;
  }
  eval_expr(e, &r);
  if (r.isflt && (isz == 'l' || isz == 'w' || isz == 'b')) {
    uint8_t buf[4];
    encode_float('s', r.f, buf);
    if (isz == 'l') {
      if (c->enc) { bb_be16(c->out, (buf[0] << 8) | buf[1]); bb_be16(c->out, (buf[2] << 8) | buf[3]); }
      c->len += 4;
    } else {
      if (c->enc) bb_be16(c->out, (buf[0] << 8) | buf[1]);
      c->len += 2;
    }
    return;
  }
  eval_op(c, e, &r);
  if (isz == 'b') emit_val(c, &r, 1, 0, 0, -128, 255, "immediate operand out of range");
  else if (isz == 'l') emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "immediate operand out of range");
  else emit_val(c, &r, 2, 0, 0, -32768, 65535, "immediate operand out of range");
}

static void ea_emit(ictx *c, operand *o, int isz)
{
  eres r;
  ival ea = c->pc + c->len;
  switch (o->form) {
  case F_IMM:
    imm_raw = o->rawhex;
    emit_imm(c, o->bd, isz);
    imm_raw = 0;
    return;
  case F_ABSW:
    eval_op(c, o->bd, &r);
    if (c->enc && (r.sec || r.ext) && !r.bad) {
      if (r.sec) { error("need 32 bits to reference a program label"); }
    }
    emit_val(c, &r, 2, 0, 0, -32768, 32767, "absolute short address out of range");
    return;
  case F_ABSL:
    eval_op(c, o->bd, &r);
    emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "absolute address out of range");
    return;
  case F_D16:
    eval_op(c, o->bd, &r);
    base_adjust(c, o->base, &r);
    emit_val(c, &r, 2, 0, 0, -32768, 32767, "displacement out of range");
    return;
  case F_PCD16:
    eval_op(c, o->bd, &r);
    if (is_abs(&r) && !(c->sec->flags & SEC_ABS) && !opt_nodpc) {
      if (c->jmppc) { emit16(c, (uint32_t)r.v & 0xffff); return; }
      emit_val(c, &r, 2, 0, 0, -32768, 32767, "displacement out of range");
      return;
    }
    emit_val(c, &r, 2, 1, ea, -32768, 32767, "displacement out of range");
    return;
  case F_BRIEF: case F_PCBRIEF: {
    uint32_t w = ((o->xn & 15) << 12) | (o->xsz == 4 ? 0x800 : 0) | (scale_bits(o->scale) << 9);
    ival d;
    eval_op(c, o->bd, &r);
    if (o->form == F_PCBRIEF) {
      if (!pc_disp(c, &r, ea, &d)) { if (c->enc) error("illegal relocation"); d = 0; }
    } else {
      base_adjust(c, o->base, &r);
      if (!is_abs(&r)) { if (c->enc) error("illegal relocation"); }
      d = r.v;
    }
    if (c->enc && !fits8(d)) error("displacement out of range");
    if (c->enc && o->scale != 1 && !IS020(c)) error("illegal scale factor");
    emit16(c, w | (d & 0xff));
    return;
  }
  case F_FULL: case F_PCFULL: {
    int is_pc = o->form == F_PCFULL;
    int bdl, bdcode, odl = 0, odcode = 1, iis = 0;
    uint32_t w;
    bdcode = bdsize_for(c, o->bd, o->bdsz, is_pc && o->base != BASE_ZPC ? 1 : is_pc, ea, o->base, &bdl);
    if (o->memind && o->hasod) {
      if (o->odsz == 4) { odl = 4; odcode = 3; }
      else if (o->odsz == 2) { odl = 2; odcode = 2; }
      else {
        eres orr;
        eval_op(c, o->od, &orr);
        if (!is_abs(&orr)) { odl = 4; odcode = 3; }
        else if (orr.v == 0) { odl = 0; odcode = 1; }
        else if (fits16(orr.v)) { odl = 2; odcode = 2; }
        else { odl = 4; odcode = 3; }
      }
    }
    if (!o->memind) iis = 0;
    else if (o->memind == 2) iis = 4 | odcode;
    else iis = odcode;  /* pre-indexed or no index */
    w = 0x100 | (bdcode << 4) | iis;
    if (o->xn >= 0 && !o->xsupp)
      w |= ((o->xn & 15) << 12) | (o->xsz == 4 ? 0x800 : 0) | (scale_bits(o->scale) << 9);
    else {
      w |= 0x40;   /* index suppress */
      if (o->xn >= 0) w |= ((o->xn & 15) << 12) | (o->xsz == 4 ? 0x800 : 0) | (scale_bits(o->scale) << 9);
    }
    if (o->memind == 2 && (o->xn < 0 || o->xsupp)) {
      /* post-indexed without index makes no sense: treat as no index */
      w = (w & ~7u) | odcode;
    }
    if (o->base == BASE_NONE || o->base == BASE_ZPC || o->zbase) w |= 0x80;
    if (c->enc && !IS020(c)) error("addressing mode not supported on selected cpu");
    emit16(c, w);
    if (bdl) {
      eval_op(c, o->bd, &r);
      if (is_pc && o->base != BASE_ZPC) {
        if (is_abs(&r) && !(c->sec->flags & SEC_ABS) && !opt_nodpc)
          emit_val(c, &r, bdl, 0, 0, bdl == 2 ? -32768 : -0x80000000LL, bdl == 2 ? 32767 : 0xffffffffLL, "displacement out of range");
        else
          emit_val(c, &r, bdl, 1, ea, bdl == 2 ? -32768 : -0x80000000LL, bdl == 2 ? 32767 : 0xffffffffLL, "displacement out of range");
      } else {
        base_adjust(c, o->base, &r);
        emit_val(c, &r, bdl, 0, 0, bdl == 2 ? -32768 : -0x80000000LL, bdl == 2 ? 32767 : 0xffffffffLL, "displacement out of range");
      }
    }
    if (odl) {
      eval_op(c, o->od, &r);
      emit_val(c, &r, odl, 0, 0, odl == 2 ? -32768 : -0x80000000LL, odl == 2 ? 32767 : 0xffffffffLL, "displacement out of range");
    }
    return;
  }
  default:
    return;
  }
}

/* ------------------------------------------------------- handler utils */
static int opsize(ictx *c)
{
  int s = c->ip->size;
  if (!s) s = c->ip->m->defsize;
  return s;
}
static int sz2(int s) { return s == 'b' ? 0 : s == 'w' ? 1 : 2; }       /* bits 7-6 */
static int movesz(int s) { return s == 'b' ? 1 : s == 'w' ? 3 : 2; }   /* bits 13-12 */

static operand *OP(ictx *c, int i) { return &c->ip->op[i]; }

static int nops_is(ictx *c, int n)
{
  if (c->ip->nops != n) {
    if (c->enc) error(c->ip->nops < n ? "missing operand" : "too many operands");
    return 0;
  }
  return 1;
}

static int ea_chk(ictx *c, int i, unsigned allowed, int isz, int extoff)
{
  operand *o = OP(c, i);
  if (!ea_resolve(c, o, allowed, isz, extoff)) {
    if (c->enc) error("illegal addressing mode");
    return 0;
  }
  if (o->form == F_AN && isz == 'b' && (allowed & A_AN)) {
    if (c->enc) error("illegal addressing mode");
    return 0;
  }
  return 1;
}

static int is_dn(operand *o) { return o->mode == M_DN && !o->bf; }
static int is_an(operand *o) { return o->mode == M_AN; }
static int is_imm(operand *o) { return o->mode == M_IMM; }

static int imm_value(ictx *c, operand *o, ival *v)
{
  eres r;
  if (!o->bd) return 0;
  eval_op(c, o->bd, &r);
  if (!is_abs(&r) || !r.defined) return 0;
  if (r.isflt) return 0;
  *v = r.v;
  return 1;
}

/* value sign-extended per operation size */
static ival sext(ival v, int s)
{
  if (s == 'b') return (int8_t)v;
  if (s == 'w') return (int16_t)v;
  return (int32_t)v;
}

static void cpu_check(ictx *c, unsigned need)
{
  if (c->enc && !(c->ip->cpu & need))
    error("instruction not supported on selected architecture");
}

/* emit "opcode + ea extensions" for single-ea instructions */
static int emit_op_ea(ictx *c, uint32_t opc, int i, int isz)
{
  emit16(c, opc | ea_bits(OP(c, i)));
  ea_emit(c, OP(c, i), isz);
  return 1;
}

/* ----------------------------------------------------------- handlers */
static int h_none(ictx *c)
{
  const mnem *m = c->ip->m;
  if (c->ip->nops) { if (c->enc) error("too many operands"); }
  cpu_check(c, m->cpus);
  emit16(c, m->opc);
  if (m->opc2) emit16(c, m->opc2);
  return 1;
}

static int h_moveq(ictx *c)
{
  ival v = 0;
  eres r;
  if (!nops_is(c, 2)) return 0;
  if (!is_imm(OP(c, 0)) || !is_dn(OP(c, 1))) { if (c->enc) error("illegal addressing mode"); }
  eval_op(c, OP(c, 0)->bd, &r);
  v = r.v;
  if (c->enc && is_abs(&r)) {
    if (v > 127 && v <= 255) warning("using signed operand as unsigned: %lld (valid: -128..127), %lld to fix",
                                     (long long)v, (long long)(v - 256));
    else if (v < -128 || v > 255) error("immediate operand out of range");
  }
  if (c->enc && !is_abs(&r)) error("immediate operand has illegal type or size");
  emit16(c, 0x7000 | (OP(c, 1)->reg << 9) | (v & 0xff));
  return 1;
}

/* forward decls */
static int h_lea(ictx *c);
static int h_movea(ictx *c);
static void emit_moveq(ictx *c, int dn, ival v) { emit16(c, 0x7000 | (dn << 9) | (v & 0xff)); }

static int h_move(ictx *c)
{
  int s = opsize(c);
  operand *src, *dst;
  if (!nops_is(c, 2)) return 0;
  src = OP(c, 0); dst = OP(c, 1);
  /* special registers */
  if (src->mode == M_SR) {
    if (!ea_chk(c, 1, EA_DALT, 'w', 2)) return 0;
    return emit_op_ea(c, 0x40C0, 1, 'w');
  }
  if (src->mode == M_CCR) {
    cpu_check(c, C010UP);
    if (!ea_chk(c, 1, EA_DALT, 'w', 2)) return 0;
    return emit_op_ea(c, 0x42C0, 1, 'w');
  }
  if (dst->mode == M_SR) {
    if (!ea_chk(c, 0, EA_DATA, 'w', 2)) return 0;
    return emit_op_ea(c, 0x46C0, 0, 'w');
  }
  if (dst->mode == M_CCR) {
    if (!ea_chk(c, 0, EA_DATA, 'w', 2)) return 0;
    return emit_op_ea(c, 0x44C0, 0, 'w');
  }
  if (src->mode == M_USP && is_an(dst)) { emit16(c, 0x4E68 | dst->reg); return 1; }
  if (dst->mode == M_USP && is_an(src)) { emit16(c, 0x4E60 | src->reg); return 1; }
  if (is_an(dst)) {
    /* movea */
    ival v;
    if (s == 'b') { if (c->enc) error("illegal opcode extension"); s = 'w'; }
    if (OPT(c) && is_imm(src) && imm_value(c, src, &v)) {
      v = sext(v, s);
      if (v == 0) { emit16(c, 0x91C8 | (dst->reg << 9) | dst->reg); return 1; } /* suba.l An,An */
      if (s == 'l' && fits16(v)) s = 'w';
    } else if (OPT(c) && is_imm(src) && s == 'l') {
      /* movea.l #label,An -> lea label,An */
      eres r;
      eval_op(c, src->bd, &r);
      if (!r.bad && (r.sec || r.ext) && !r.isflt) {
        operand save = *src;
        int ok;
        src->mode = M_ABS;
        src->bdsz = 0;
        if (!ea_chk(c, 0, EA_CTRL, 'l', 2)) { *src = save; return 0; }
        ok = emit_op_ea(c, 0x41C0 | (dst->reg << 9), 0, 'l');
        *src = save;
        return ok;
      }
    }
    if (!ea_chk(c, 0, EA_ALL, s, 2)) return 0;
    return emit_op_ea(c, (movesz(s) << 12) | (dst->reg << 9) | 0x40, 0, s);
  }
  if (OPT(c) && is_imm(src) && c->ip->m->h != h_movea) {
    ival v;
    if (imm_value(c, src, &v)) {
      ival sv = sext(v, s);
      if (s == 'l' && is_dn(dst)) {
        uint32_t u = (uint32_t)v;
        int n = dst->reg;
        if (fits8(sv)) { emit_moveq(c, n, sv); return 1; }
        if (u >= 128 && u <= 254 && !(u & 1)) {
          emit_moveq(c, n, u >> 1);
          emit16(c, 0xD040 | (n << 9) | n);
          return 1;
        }
        if ((u & 0xffff) == 0 && (u >> 16) >= 1 && (u >> 16) <= 0x7f) {
          emit_moveq(c, n, u >> 16);
          emit16(c, 0x4840 | n);
          return 1;
        }
        if ((u & 0xffff) == 0xffff && (u >> 16) >= 0xff80 && (u >> 16) <= 0xfffe) {
          emit_moveq(c, n, (int8_t)(u >> 16));
          emit16(c, 0x4840 | n);
          return 1;
        }
      }
      if (sv == 0 && !is_an(dst) && ((c->ip->cpu & ~C000) || opt_clr)) {
        if (ea_resolve(c, dst, EA_DALT, s, 2)) {
          return emit_op_ea(c, 0x4200 | (sz2(s) << 6), 1, s);
        }
      }
    }
  }
  if (!ea_chk(c, 0, EA_ALL, s, 2)) return 0;
  if (!ea_chk(c, 1, EA_DALT, s, 2 + OP(c, 0)->extlen)) return 0;
  {
    int d = ea_bits(dst);
    emit16(c, (movesz(s) << 12) | ((d & 7) << 9) | ((d >> 3) << 6) | ea_bits(src));
    ea_emit(c, src, s);
    ea_emit(c, dst, s);
  }
  return 1;
}

static int h_movea(ictx *c)
{
  if (!nops_is(c, 2)) return 0;
  return h_move(c);
}

static int h_addq(ictx *c);

/* ADD / SUB */
static int h_addsub(ictx *c)
{
  int s = opsize(c);
  int issub = c->ip->m->arg;
  uint32_t base = issub ? 0x9000 : 0xD000;
  operand *src, *dst;
  ival v;
  if (!nops_is(c, 2)) return 0;
  src = OP(c, 0); dst = OP(c, 1);
  if (is_an(dst)) {
    if (s == 'b') { if (c->enc) error("illegal opcode extension"); s = 'w'; }
    if (OPT(c) && is_imm(src) && imm_value(c, src, &v)) {
      if (v == 0) return 1;     /* deleted */
      if (v >= 1 && v <= 8) { emit16(c, (issub ? 0x5100 : 0x5000) | ((v & 7) << 9) | (sz2(s) << 6) | 010 | dst->reg); return 1; }
      if (!issub && v >= -8 && v <= -1) { emit16(c, 0x5100 | ((-v & 7) << 9) | (sz2(s) << 6) | 010 | dst->reg); return 1; }
      if (issub ? (v >= -32767 && v <= 32768) : fits16(v)) {
        /* adda #x,An -> lea (x,An),An */
        emit16(c, 0x41E8 | (dst->reg << 9) | dst->reg);
        emit16(c, (uint32_t)(issub ? -v : v) & 0xffff);
        return 1;
      }
    }
    if (!ea_chk(c, 0, EA_ALL, s, 2)) return 0;
    return emit_op_ea(c, base | (dst->reg << 9) | (s == 'l' ? 0x1C0 : 0xC0), 0, s);
  }
  if (is_imm(src)) {
    int explicit_i = (c->ip->m->opc & 0xF000) == 0;
    if (OPT(c) && imm_value(c, src, &v)) {
      if (v >= 1 && v <= 8) {
        if (!ea_chk(c, 1, EA_ALT, s, 2)) return 0;
        return emit_op_ea(c, (issub ? 0x5100 : 0x5000) | ((v & 7) << 9) | (sz2(s) << 6), 1, s);
      }
    }
    if (!is_dn(dst) || explicit_i) {
      /* addi / subi */
      if (!ea_chk(c, 0, A_IMM, s, 2)) return 0;
      if (!ea_chk(c, 1, EA_DALT, s, 2 + imm_len(s))) return 0;
      emit16(c, (issub ? 0x0400 : 0x0600) | (sz2(s) << 6) | ea_bits(dst));
      ea_emit(c, src, s);
      ea_emit(c, dst, s);
      return 1;
    }
  }
  if (is_dn(dst)) {
    if (!ea_chk(c, 0, EA_ALL, s, 2)) return 0;
    return emit_op_ea(c, base | (dst->reg << 9) | (sz2(s) << 6), 0, s);
  }
  if (is_dn(src)) {
    if (!ea_chk(c, 1, EA_MALT, s, 2)) return 0;
    return emit_op_ea(c, base | (src->reg << 9) | ((4 + sz2(s)) << 6), 1, s);
  }
  if (c->enc) error("illegal addressing mode");
  return 0;
}

static int h_adda(ictx *c)
{
  if (!nops_is(c, 2)) return 0;
  if (!is_an(OP(c, 1))) { if (c->enc) error("address register required"); return 0; }
  return h_addsub(c);
}

static int h_addi(ictx *c)
{
  if (!nops_is(c, 2)) return 0;
  if (!is_imm(OP(c, 0))) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (is_an(OP(c, 1))) { if (c->enc) error("illegal addressing mode"); return 0; }
  return h_addsub(c);
}

static int h_addq(ictx *c)
{
  int s = opsize(c);
  ival v = 0;
  eres r;
  if (!nops_is(c, 2)) return 0;
  if (!is_imm(OP(c, 0))) { if (c->enc) error("illegal addressing mode"); return 0; }
  eval_op(c, OP(c, 0)->bd, &r);
  v = r.v;
  if (c->enc && (!is_abs(&r) || v < 1 || v > 8)) error("immediate operand out of range");
  if (!ea_chk(c, 1, EA_ALT, s, 2)) return 0;
  return emit_op_ea(c, (c->ip->m->arg ? 0x5100 : 0x5000) | ((v & 7) << 9) | (sz2(s) << 6), 1, s);
}

/* ADDX/SUBX/ABCD/SBCD */
static int h_addx(ictx *c)
{
  int s = opsize(c);
  operand *a, *b;
  uint32_t opc = c->ip->m->opc;
  if (!nops_is(c, 2)) return 0;
  a = OP(c, 0); b = OP(c, 1);
  if (c->ip->m->sizes != SZ_B) opc |= sz2(s) << 6;
  if (is_dn(a) && is_dn(b)) { emit16(c, opc | (b->reg << 9) | a->reg); return 1; }
  if (a->mode == M_PRE && b->mode == M_PRE) { emit16(c, opc | 8 | (b->reg << 9) | a->reg); return 1; }
  if (c->enc) error("illegal addressing mode");
  return 0;
}

/* immediate to ccr/sr helper */
static int imm_to_sr(ictx *c, uint32_t opc, int sr)
{
  emit16(c, opc | (sr ? 0x7C : 0x3C));
  emit_imm(c, OP(c, 0)->bd, sr ? 'w' : 'b');
  return 1;
}

/* AND / OR / EOR and their immediate forms; arg: 0=and 1=or 2=eor */
static int h_logic(ictx *c)
{
  int s = opsize(c);
  int kind = c->ip->m->arg;
  int immonly = c->ip->m->opc2;    /* andi/ori/eori mnemonics */
  static const uint32_t regop[3] = { 0xC000, 0x8000, 0xB000 };
  static const uint32_t immop[3] = { 0x0200, 0x0000, 0x0A00 };
  operand *src, *dst;
  if (!nops_is(c, 2)) return 0;
  src = OP(c, 0); dst = OP(c, 1);
  if (dst->mode == M_CCR || dst->mode == M_SR) {
    if (!is_imm(src)) { if (c->enc) error("illegal addressing mode"); return 0; }
    return imm_to_sr(c, immop[kind], dst->mode == M_SR);
  }
  if (is_imm(src)) {
    ival v;
    if (OPT(c) && imm_value(c, src, &v)) {
      ival mask = s == 'b' ? 0xff : s == 'w' ? 0xffff : 0xffffffffLL;
      ival mv = kind == 2 ? (v & mask) : (s == 'l' ? (v & mask) : v);
      if (kind == 0 && mv == 0 && ((c->ip->cpu & ~C000) || opt_clr)) {
        if (!ea_chk(c, 1, EA_DALT, s, 2)) return 0;
        return emit_op_ea(c, 0x4200 | (sz2(s) << 6), 1, s);
      }
      if ((kind == 0 && mv == mask) || (kind != 0 && mv == 0)) {
        if (!ea_chk(c, 1, EA_DALT, s, 2)) return 0;
        return emit_op_ea(c, 0x4A00 | (sz2(s) << 6), 1, s);
      }
      if (kind == 2 && mv == mask) {
        if (!ea_chk(c, 1, EA_DALT, s, 2)) return 0;
        return emit_op_ea(c, 0x4600 | (sz2(s) << 6), 1, s);
      }
    }
    if (kind != 2 && !immonly && is_dn(dst)) {
      if (!ea_chk(c, 0, EA_DATA, s, 2)) return 0;
      return emit_op_ea(c, regop[kind] | (dst->reg << 9) | (sz2(s) << 6), 0, s);
    }
    if (!ea_chk(c, 0, A_IMM, s, 2)) return 0;
    if (!ea_chk(c, 1, EA_DALT, s, 2 + imm_len(s))) return 0;
    emit16(c, immop[kind] | (sz2(s) << 6) | ea_bits(dst));
    ea_emit(c, src, s);
    ea_emit(c, dst, s);
    return 1;
  }
  if (immonly) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (kind != 2 && is_dn(dst)) {
    if (!ea_chk(c, 0, EA_DATA, s, 2)) return 0;
    return emit_op_ea(c, regop[kind] | (dst->reg << 9) | (sz2(s) << 6), 0, s);
  }
  if (is_dn(src)) {
    if (!ea_chk(c, 1, kind == 2 ? EA_DALT : EA_MALT, s, 2)) return 0;
    return emit_op_ea(c, regop[kind] | (src->reg << 9) | ((4 + sz2(s)) << 6), 1, s);
  }
  if (c->enc) error("illegal addressing mode");
  return 0;
}

/* CMP family; arg: 0 cmp, 1 cmpa, 2 cmpi, 3 cmpm */
static int h_cmp(ictx *c)
{
  int s = opsize(c);
  int kind = c->ip->m->arg;
  operand *src, *dst;
  ival v;
  if (!nops_is(c, 2)) return 0;
  src = OP(c, 0); dst = OP(c, 1);
  if (src->mode == M_POST && dst->mode == M_POST && (kind == 0 || kind == 3)) {
    emit16(c, 0xB108 | (dst->reg << 9) | (sz2(s) << 6) | src->reg);
    return 1;
  }
  if (kind == 3) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (is_an(dst) && kind != 2 && is_imm(src) && OPT(c) && (c->ip->cpu & (C020UP | CCPU32)) &&
      imm_value(c, src, &v) && sext(v, s) == 0 && s != 'b') {
    emit16(c, 0x4A80 | 010 | dst->reg);
    return 1;
  }
  if (is_an(dst) && kind != 2) {
    if (s == 'b') { if (c->enc) error("illegal opcode extension"); s = 'w'; }
    if (OPT(c) && is_imm(src) && s == 'l' && imm_value(c, src, &v) && fits16(sext(v, s))) s = 'w';
    if (!ea_chk(c, 0, EA_ALL, s, 2)) return 0;
    return emit_op_ea(c, 0xB0C0 | (dst->reg << 9) | (s == 'l' ? 0x100 : 0), 0, s);
  }
  if (kind == 1) { if (c->enc) error("address register required"); return 0; }
  if (is_imm(src)) {
    unsigned dmask = (c->ip->cpu & (C020UP | CCPU32)) ? (EA_DATA & ~A_IMM) : EA_DALT;
    if (OPT(c) && imm_value(c, src, &v) && sext(v, s) == 0) {
      unsigned tmask = (c->ip->cpu & (C020UP | CCPU32)) ? (EA_ALL & ~A_IMM) : EA_DALT;
      if (ea_resolve(c, dst, tmask, s, 2) && !(dst->form == F_AN && s == 'b'))
        return emit_op_ea(c, 0x4A00 | (sz2(s) << 6), 1, s);
    }
    if (kind == 2 || !is_dn(dst)) {
      if (!ea_chk(c, 0, A_IMM, s, 2)) return 0;
      if (!ea_chk(c, 1, dmask, s, 2 + imm_len(s))) return 0;
      emit16(c, 0x0C00 | (sz2(s) << 6) | ea_bits(dst));
      ea_emit(c, src, s);
      ea_emit(c, dst, s);
      return 1;
    }
  }
  if (kind == 2) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (!is_dn(dst)) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (!ea_chk(c, 0, EA_ALL, s, 2)) return 0;
  return emit_op_ea(c, 0xB000 | (dst->reg << 9) | (sz2(s) << 6), 0, s);
}

/* CLR NEG NEGX NOT TST */
static int h_unary(ictx *c)
{
  int s = opsize(c);
  uint32_t opc = c->ip->m->opc;
  unsigned allowed = EA_DALT;
  if (!nops_is(c, 1)) return 0;
  if (opc == 0x4A00 && (c->ip->cpu & (C020UP | CCPU32))) allowed = EA_ALL;
  if (opc == 0x4200 && OPT(c) && s == 'l' && is_dn(OP(c, 0))) {
    emit_moveq(c, OP(c, 0)->reg, 0);
    return 1;
  }
  if (!ea_chk(c, 0, allowed, s, 2)) return 0;
  return emit_op_ea(c, opc | (sz2(s) << 6), 0, s);
}

/* single ea, fixed opcode: NBCD TAS PEA JMP JSR etc; m->arg = ea mask */
static int h_ea(ictx *c)
{
  int s = opsize(c);
  if (!nops_is(c, 1)) return 0;
  cpu_check(c, c->ip->m->cpus);
  if (!ea_chk(c, 0, (unsigned)c->ip->m->arg, s, 2)) return 0;
  return emit_op_ea(c, c->ip->m->opc, 0, s);
}

/* shifts: arg = type (0 as,1 ls,2 rox,3 ro) | dir<<2 */
static int h_shift(ictx *c)
{
  int s = opsize(c);
  int type = c->ip->m->arg & 3, dir = (c->ip->m->arg >> 2) & 1;
  operand *a = OP(c, 0), *b;
  if (c->ip->nops == 1) {
    if (is_dn(a)) {
      if (OPT(c) && dir == 1 && !(c->ip->cpu & (C040 | C060)) &&
          (type == 0 || (type == 1 && opt_lsl))) {
        /* vasm encodes this as ADD Dn,D0 */
        emit16(c, 0xD000 | (sz2(s) << 6) | a->reg);
        return 1;
      }
      emit16(c, 0xE000 | (1 << 9) | (dir << 8) | (sz2(s) << 6) | (type << 3) | a->reg);
      return 1;
    }
    if (c->ip->size && s != 'w') { if (c->enc) error("illegal opcode extension"); }
    if (!ea_chk(c, 0, EA_MALT, 'w', 2)) return 0;
    return emit_op_ea(c, 0xE0C0 | (type << 9) | (dir << 8), 0, 'w');
  }
  if (!nops_is(c, 2)) return 0;
  b = OP(c, 1);
  if (!is_dn(b)) { if (c->enc) error("data register required"); return 0; }
  if (is_dn(a)) {
    emit16(c, 0xE020 | (a->reg << 9) | (dir << 8) | (sz2(s) << 6) | (type << 3) | b->reg);
    return 1;
  }
  if (is_imm(a)) {
    ival v = 0;
    eres r;
    eval_op(c, a->bd, &r);
    v = r.v;
    if (OPT(c) && is_abs(&r) && v == 1 && dir == 1 && !(c->ip->cpu & (C040 | C060)) &&
        (type == 0 || (type == 1 && opt_lsl))) {
      /* asl #1,Dn -> add Dn,Dn */
      emit16(c, 0xD000 | (b->reg << 9) | (sz2(s) << 6) | b->reg);
      return 1;
    }
    if (c->enc && (!is_abs(&r) || v < 1 || v > 8)) error("immediate operand out of range");
    emit16(c, 0xE000 | ((v & 7) << 9) | (dir << 8) | (sz2(s) << 6) | (type << 3) | b->reg);
    return 1;
  }
  if (c->enc) error("illegal addressing mode");
  return 0;
}

/* BTST BCHG BCLR BSET; arg = type */
static int h_bitop(ictx *c)
{
  int type = c->ip->m->arg;
  operand *a, *b;
  unsigned dmask;
  int s;
  if (!nops_is(c, 2)) return 0;
  a = OP(c, 0); b = OP(c, 1);
  s = is_dn(b) ? 'l' : 'b';
  if (is_dn(a)) {
    dmask = type == 0 ? EA_DATA : EA_DALT;
    if (!ea_chk(c, 1, dmask, s, 2)) return 0;
    return emit_op_ea(c, 0x0100 | (a->reg << 9) | (type << 6), 1, 'b');
  }
  if (is_imm(a)) {
    dmask = type == 0 ? (EA_DATA & ~A_IMM) : EA_DALT;
    if (!ea_chk(c, 1, dmask, s, 4)) return 0;
    emit16(c, 0x0800 | (type << 6) | ea_bits(b));
    {
      eres r;
      eval_op(c, a->bd, &r);
      if (c->enc && is_abs(&r)) {
        ival lim = is_dn(b) ? 31 : 7;
        if (r.v < 0 || r.v > lim) {
          /* vasm masks the bit number; warn */
        }
      }
      emit_val(c, &r, 1, 0, 0, -128, 255, "immediate operand out of range");
    }
    ea_emit(c, b, 'b');
    return 1;
  }
  if (c->enc) error("illegal addressing mode");
  return 0;
}

/* ------------------------------------------------------------ branches */
static int targets_next(ictx *c, expr *e);
/* target evaluation: returns 1 if displacement known (same section) */
static int branch_disp(ictx *c, expr *e, ival base, ival *d, eres *r)
{
  eval_op(c, e, r);
  if (r->sec && r->sec == c->sec && !r->ext) { *d = r->v - base; return 1; }
  if (is_abs(r) && (c->sec->flags & SEC_ABS)) { *d = r->v - (c->sec->org + base); return 1; }
  *d = 0;
  return 0;
}

/* adjust a forward displacement for a size change of this instruction */
static ival fwd_adj(ictx *c, eres *r, ival d, int newsize)
{
  ival tgt = r->v - ((c->sec->flags & SEC_ABS) ? c->sec->org : 0);
  ival old = c->ip->lastsize;
  if (old > 0 && tgt > c->pc) return d + (newsize - old);
  return d;
}

static void emit_jmp_abs(ictx *c, uint32_t opc, expr *e)
{
  eres r;
  emit16(c, opc);
  eval_op(c, e, &r);
  emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "address out of range");
}

/* Bcc/BRA/BSR. m->arg = condition (0 = bra, 1 = bsr) */
static int h_bcc(ictx *c)
{
  int cc = c->ip->m->arg;
  int s = c->ip->size;
  operand *o;
  eres r;
  ival d;
  int known;
  int form;   /* 1 byte, 2 word, 4 long, 8 = translated to jmp */
  if (!nops_is(c, 1)) return 0;
  o = OP(c, 0);
  if (o->mode != M_ABS) { if (c->enc) error("illegal addressing mode"); return 0; }
  known = branch_disp(c, o->bd, c->pc + 2, &d, &r);
  if (s == 's' || s == 'b') form = 1;
  else if (s == 'w') form = 2;
  else if (s == 'l') form = 4;
  else form = 0;
  if (form && (opt_allbra && OPT(c)) && known) form = 0;
  if (!form && OPT(c) && known && cc != 1 && r.sec == c->sec && targets_next(c, o->bd)) {
    return 1;   /* branch to the following instruction is deleted */
  }
  if (!form) {
    if (!OPT(c)) form = 2;
    else if (known) {
      ival d8 = fwd_adj(c, &r, d, 2), d16 = fwd_adj(c, &r, d, 4);
      if (d8 != 0 && fits8(d8) && d8 != -1 && !(grow_only && c->ip->lastsize > 2) &&
          !(cc == 1 && d16 == 2)) form = 1;
      else if (fits16(d16) && !(grow_only && c->ip->lastsize > 4)) form = 2;
      else if (IS020(c)) form = 4;
      else form = 8;
    } else {
      if (r.ext && cc <= 1 && opt_brajmp) form = 8;
      else if (!r.sec && !r.ext && !(c->sec->flags & SEC_ABS)) form = IS020(c) ? 4 : 8;
      else if (r.sec && r.sec != c->sec && opt_brajmp) form = 8;
      else form = 2;
    }
  }
  if (form == 4 && !IS020(c)) {
    if (c->enc) warning("deprecated B<cc>.L on 68000 transformed into B<cc>.W");
    form = 2;
  }
  if (form == 1) {
    if (c->enc) {
      if (!known) error("branch destination out of range");
      else if (d == 0) warning("short-branch to following instruction turned into a nop");
      else if (!fits8(d)) error("branch destination out of range");
    }
    if (known && d == 0) { emit16(c, 0x4DD6); return 1; }
    emit16(c, 0x6000 | (cc << 8) | (d & 0xff));
    return 1;
  }
  if (form == 2) {
    emit16(c, 0x6000 | (cc << 8));
    emit_val(c, &r, 2, 1, c->pc + 2, -32768, 32767, "branch destination out of range");
    return 1;
  }
  if (form == 4) {
    cpu_check(c, C020UP | CCPU32);
    emit16(c, 0x60FF | (cc << 8));
    emit_val(c, &r, 4, 1, c->pc + 2, -0x80000000LL, 0x7fffffffLL, "branch destination out of range");
    return 1;
  }
  /* translate into jmp/jsr */
  if (cc == 0) emit_jmp_abs(c, 0x4EF9, o->bd);
  else if (cc == 1) emit_jmp_abs(c, 0x4EB9, o->bd);
  else {
    emit16(c, 0x6000 | ((cc ^ 1) << 8) | 6);
    emit_jmp_abs(c, 0x4EF9, o->bd);
  }
  return 1;
}

static int h_dbcc(ictx *c)
{
  eres r;
  ival d;
  if (!nops_is(c, 2)) return 0;
  if (!is_dn(OP(c, 0)) || OP(c, 1)->mode != M_ABS) { if (c->enc) error("illegal addressing mode"); return 0; }
  emit16(c, 0x50C8 | (c->ip->m->arg << 8) | OP(c, 0)->reg);
  branch_disp(c, OP(c, 1)->bd, c->pc + 2, &d, &r);
  emit_val(c, &r, 2, 1, c->pc + 2, -32768, 32767, "branch destination out of range");
  return 1;
}

static int h_scc(ictx *c)
{
  if (!nops_is(c, 1)) return 0;
  if (!ea_chk(c, 0, EA_DALT, 'b', 2)) return 0;
  return emit_op_ea(c, 0x50C0 | (c->ip->m->arg << 8), 0, 'b');
}

static int h_trapcc(ictx *c)
{
  int s = c->ip->size;
  cpu_check(c, C020UP | CCPU32);
  if (c->ip->nops == 0) { emit16(c, 0x50FC | (c->ip->m->arg << 8)); return 1; }
  if (!nops_is(c, 1) || !is_imm(OP(c, 0))) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (!s) {
    ival v;
    s = (imm_value(c, OP(c, 0), &v) && fits16(v)) ? 'w' : 'l';
    if (!OPT(c)) s = 'w';
  }
  emit16(c, 0x50F8 | (c->ip->m->arg << 8) | (s == 'l' ? 3 : 2));
  emit_imm(c, OP(c, 0)->bd, s == 'l' ? 'l' : 'w');
  return 1;
}

/* JMP/JSR; arg 0 jmp, 1 jsr */
static int h_jmp(ictx *c)
{
  int isjsr = c->ip->m->arg;
  operand *o;
  if (!nops_is(c, 1)) return 0;
  o = OP(c, 0);
  if (OPT(c) && o->mode == M_ABS && !o->bdsz) {
    eres r;
    ival d;
    int known = branch_disp(c, o->bd, c->pc + 2, &d, &r) && r.sec == c->sec;
    if (known && !isjsr && targets_next(c, o->bd))
      return 1;
    if (known && !opt_phx) {
      ival d8 = fwd_adj(c, &r, d, 2), d16 = fwd_adj(c, &r, d, 4);
      if (d8 != 0 && fits8(d8) && d8 != -1 && !(grow_only && c->ip->lastsize > 2)) {
        emit16(c, 0x6000 | (isjsr << 8) | (d & 0xff));
        return 1;
      }
      if (fits16(d16)) {
        emit16(c, 0x6000 | (isjsr << 8));
        emit_val(c, &r, 2, 1, c->pc + 2, -32768, 32767, "branch destination out of range");
        return 1;
      }
    }
    if (!known && r.ext && (opt_jbra || opt_gas) && IS020(c) && !opt_kick1) {
      emit16(c, 0x60FF | (isjsr << 8));
      emit_val(c, &r, 4, 1, c->pc + 2, -0x80000000LL, 0x7fffffffLL, "branch destination out of range");
      return 1;
    }
    if (!known && r.ext && opt_sc) {
      emit16(c, (isjsr ? 0x4EBA : 0x4EFA));
      emit_val(c, &r, 2, 1, c->pc + 2, -32768, 32767, "branch destination out of range");
      return 1;
    }
  }
  c->jmppc = 1;
  if (!ea_chk(c, 0, EA_CTRL, 'l', 2)) return 0;
  return emit_op_ea(c, isjsr ? 0x4E80 : 0x4EC0, 0, 'l');
}

/* ---------------------------------------------------------- lea / pea */
static int h_lea(ictx *c)
{
  operand *src, *dst;
  if (!nops_is(c, 2)) return 0;
  src = OP(c, 0); dst = OP(c, 1);
  if (!is_an(dst)) { if (c->enc) error("address register required"); return 0; }
  if (OPT(c)) {
    if (src->mode == M_IND && src->reg == dst->reg) return 1;          /* lea (An),An */
    if (src->mode == M_MEM && src->base == dst->reg && src->xn < 0 && !src->memind && !src->bdsz) {
      ival v;
      if (src->bd && eval_const(src->bd, &v) && !c->ip->basereg[src->base]) {
        if (v == 0) return 1;
        if (v >= 1 && v <= 8) { emit16(c, 0x5088 | ((v & 7) << 9) | dst->reg); return 1; }
        if (v >= -8 && v <= -1) { emit16(c, 0x5188 | ((-v & 7) << 9) | dst->reg); return 1; }
        if (!fits16(v) && !IS020(c)) {
          /* adda.l #v,An */
          emit16(c, 0xD1FC | (dst->reg << 9));
          emit32(c, (uint32_t)v);
          return 1;
        }
      }
    }
    if (src->mode == M_MEM && src->base >= 0 && src->base < 8 && src->base != dst->reg &&
        src->xn < 0 && !src->memind && !src->bdsz && !IS020(c)) {
      ival v;
      if (src->bd && eval_const(src->bd, &v) && !fits16(v) && !c->ip->basereg[src->base]) {
        emit16(c, 0x2048 | (dst->reg << 9) | src->base);   /* movea.l Am,An */
        emit16(c, 0xD1FC | (dst->reg << 9));
        emit32(c, (uint32_t)v);
        return 1;
      }
    }
    if (src->mode == M_ABS && !src->bdsz) {
      ival v;
      if (eval_const(src->bd, &v) && v == 0) {
        emit16(c, 0x91C8 | (dst->reg << 9) | dst->reg);
        return 1;
      }
    }
  }
  if (!ea_chk(c, 0, EA_CTRL, 'l', 2)) return 0;
  return emit_op_ea(c, 0x41C0 | (dst->reg << 9), 0, 'l');
}

/* ---------------------------------------------------------- misc */
static int h_exg(ictx *c)
{
  operand *a, *b;
  if (!nops_is(c, 2)) return 0;
  a = OP(c, 0); b = OP(c, 1);
  if (is_dn(a) && is_dn(b)) { emit16(c, 0xC140 | (a->reg << 9) | b->reg); return 1; }
  if (is_an(a) && is_an(b)) { emit16(c, 0xC148 | (a->reg << 9) | b->reg); return 1; }
  if (is_dn(a) && is_an(b)) { emit16(c, 0xC188 | (a->reg << 9) | b->reg); return 1; }
  if (is_an(a) && is_dn(b)) { emit16(c, 0xC188 | (b->reg << 9) | a->reg); return 1; }
  if (c->enc) error("illegal addressing mode");
  return 0;
}

static int h_ext(ictx *c)
{
  int s = opsize(c);
  if (!nops_is(c, 1)) return 0;
  if (!is_dn(OP(c, 0))) { if (c->enc) error("data register required"); return 0; }
  if (c->ip->m->arg) { cpu_check(c, C020UP | CCPU32); emit16(c, 0x49C0 | OP(c, 0)->reg); return 1; }
  emit16(c, (s == 'l' ? 0x48C0 : 0x4880) | OP(c, 0)->reg);
  return 1;
}

/* single register: SWAP Dn (arg 0), UNLK An (arg 1) */
static int h_reg(ictx *c)
{
  operand *o;
  if (!nops_is(c, 1)) return 0;
  o = OP(c, 0);
  if (c->ip->m->arg == 0 && !is_dn(o)) { if (c->enc) error("data register required"); return 0; }
  if (c->ip->m->arg == 1 && !is_an(o)) { if (c->enc) error("address register required"); return 0; }
  emit16(c, c->ip->m->opc | o->reg);
  return 1;
}

static int h_link(ictx *c)
{
  int s = c->ip->size;
  eres r;
  if (!nops_is(c, 2)) return 0;
  if (!is_an(OP(c, 0)) || !is_imm(OP(c, 1))) { if (c->enc) error("illegal addressing mode"); return 0; }
  eval_op(c, OP(c, 1)->bd, &r);
  if (OPT(c) && is_abs(&r) && r.defined) {
    if (s == 'l' && fits16(r.v)) s = 'w';
    else if (s != 'l' && !fits16(r.v) && IS020(c)) s = 'l';
  }
  if (s == 'l') {
    cpu_check(c, C020UP | CCPU32);
    emit16(c, 0x4808 | OP(c, 0)->reg);
    emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "immediate operand out of range");
  } else {
    emit16(c, 0x4E50 | OP(c, 0)->reg);
    emit_val(c, &r, 2, 0, 0, -32768, 65535, "immediate operand out of range");
  }
  return 1;
}

/* #imm with word/byte operand embedded: STOP, RTD, TRAP, BKPT */
static int h_immop(ictx *c)
{
  const mnem *m = c->ip->m;
  eres r;
  if (!nops_is(c, 1)) return 0;
  if (!is_imm(OP(c, 0))) { if (c->enc) error("illegal addressing mode"); return 0; }
  cpu_check(c, m->cpus);
  eval_op(c, OP(c, 0)->bd, &r);
  if (m->arg == 4 || m->arg == 8) {
    /* value inside opcode: trap #0-15 (4 bits) / bkpt #0-7 (3 bits) */
    ival lim = m->arg == 4 ? 15 : 7;
    if (c->enc && (!is_abs(&r) || r.v < 0 || r.v > lim)) error("immediate operand out of range");
    emit16(c, m->opc | (r.v & lim));
    return 1;
  }
  emit16(c, m->opc);
  if (m->opc2) emit16(c, m->opc2);
  emit_val(c, &r, 2, 0, 0, -32768, 65535, "immediate operand out of range");
  return 1;
}

static int h_chk(ictx *c)
{
  int s = opsize(c);
  if (!nops_is(c, 2)) return 0;
  if (!is_dn(OP(c, 1))) { if (c->enc) error("data register required"); return 0; }
  if (s == 'l') cpu_check(c, C020UP | CCPU32);
  if (!ea_chk(c, 0, EA_DATA, s, 2)) return 0;
  return emit_op_ea(c, (s == 'l' ? 0x4100 : 0x4180) | (OP(c, 1)->reg << 9), 0, s);
}

/* MULU/MULS/DIVU/DIVS(L); arg: bit0 signed, bit1 div, bit2 'l' form (divul) */
static int h_muldiv(ictx *c)
{
  int s = opsize(c);
  int sgn = c->ip->m->arg & 1, isdiv = (c->ip->m->arg >> 1) & 1, isl = (c->ip->m->arg >> 2) & 1;
  operand *src, *dst;
  if (!nops_is(c, 2)) return 0;
  src = OP(c, 0); dst = OP(c, 1);
  if (isl) s = 'l';
  if (s == 'w' && !isl) {
    if (!is_dn(dst)) { if (c->enc) error("data register required"); return 0; }
    if (!ea_chk(c, 0, EA_DATA, 'w', 2)) return 0;
    return emit_op_ea(c, (isdiv ? 0x80C0 : 0xC0C0) | (sgn ? 0x100 : 0) | (dst->reg << 9), 0, 'w');
  }
  cpu_check(c, C020UP | CCPU32);
  {
    int dl, dh = -1, big = 0;
    uint32_t ext;
    if (is_dn(dst)) dl = dst->reg;
    else if (dst->mode == M_PAIR && dst->reg < 8 && dst->reg2 < 8) { dh = dst->reg; dl = dst->reg2; big = !isl; }
    else { if (c->enc) error("data register required"); return 0; }
    if (dh < 0) dh = isdiv ? dl : 0;
    if (!isdiv && dh >= 0 && dst->mode != M_PAIR) dh = 0;
    ext = (dl << 12) | (sgn ? 0x800 : 0) | (big ? 0x400 : 0) | (dh & 7);
    if (!isdiv && dst->mode != M_PAIR) ext = (dl << 12) | (sgn ? 0x800 : 0) | dl;
    if (!ea_chk(c, 0, EA_DATA, 'l', 4)) return 0;
    emit16(c, (isdiv ? 0x4C40 : 0x4C00) | ea_bits(src));
    emit16(c, ext);
    ea_emit(c, src, 'l');
  }
  return 1;
}

/* MOVEM */
static uint32_t rev16(uint32_t m)
{
  uint32_t r = 0;
  int i;
  for (i = 0; i < 16; i++) if (m & (1u << i)) r |= 1u << (15 - i);
  return r;
}

static int h_movem(ictx *c)
{
  int s = opsize(c);
  operand *a, *b;
  int tomem;
  operand *ea;
  uint32_t mask;
  ival imask;
  if (!nops_is(c, 2)) return 0;
  a = OP(c, 0); b = OP(c, 1);
  if ((a->mode == M_REGLIST && !a->fpmask) || a->isreglist1) { tomem = 1; mask = a->mask; ea = b; }
  else if (a->mode == M_IMM && imm_value(c, a, &imask) && b->mode != M_REGLIST && !b->isreglist1) { tomem = 1; mask = (uint32_t)imask; ea = b; }
  else if ((b->mode == M_REGLIST && !b->fpmask) || b->isreglist1) { tomem = 0; mask = b->mask; ea = a; }
  else if (b->mode == M_IMM && imm_value(c, b, &imask)) { tomem = 0; mask = (uint32_t)imask; ea = a; }
  else { if (c->enc) error("invalid register list"); return 0; }
  if (s == 'b') { if (c->enc) error("illegal opcode extension"); s = 'w'; }
  if (OPT(c)) {
    if (mask == 0) return 1;
    if (!tomem && (mask & 0xff) == 0 && a->mode == M_POST) {
      uint32_t m2 = mask & (mask - 1);
      if (m2 && !(m2 & (m2 - 1))) {
        /* movem (An)+,Am/An -> two movea */
        int r1 = 0, r2;
        while (!(mask & (1u << r1))) r1++;
        r2 = r1 + 1;
        while (!(mask & (1u << r2))) r2++;
        emit16(c, (movesz(s) << 12) | ((r1 - 8) << 9) | 0x40 | 030 | a->reg);
        emit16(c, (movesz(s) << 12) | ((r2 - 8) << 9) | 0x40 | 030 | a->reg);
        return 1;
      }
    }
    if (!(mask & (mask - 1))) {
      int r = 0;
      while (!(mask & (1u << r))) r++;
      if (!tomem && r >= 8 && (s == 'l' || 1)) {
        /* movem <ea>,An -> movea <ea>,An */
        operand save = *b;
        int ok;
        op_clear(b);
        b->mode = M_AN; b->reg = r - 8;
        c->ip->op[1] = *b;
        if (!ea_chk(c, 0, EA_CTRL | A_POST, s, 2)) { c->ip->op[1] = save; return 0; }
        ok = emit_op_ea(c, (movesz(s) << 12) | ((r - 8) << 9) | 0x40, 0, s);
        c->ip->op[1] = save;
        return ok;
      }
      if (opt_movem) {
        int d;
        if (!tomem) {
          if (!ea_chk(c, 0, EA_CTRL | A_POST, s, 2)) return 0;
          emit16(c, (movesz(s) << 12) | (r << 9) | ea_bits(a));
          ea_emit(c, a, s);
          return 1;
        }
        if (!ea_chk(c, 1, EA_CALT | A_PRE, s, 2)) return 0;
        d = ea_bits(b);
        emit16(c, (movesz(s) << 12) | ((d & 7) << 9) | ((d >> 3) << 6) | (r & 7) | (r >= 8 ? 010 : 0));
        ea_emit(c, b, s);
        return 1;
      }
    }
  }
  if (tomem) {
    if (!ea_chk(c, 1, EA_CALT | A_PRE, s, 4)) return 0;
    if (ea->form == F_PRE) mask = rev16(mask);
    emit16(c, 0x4880 | (s == 'l' ? 0x40 : 0) | ea_bits(ea));
  } else {
    if (!ea_chk(c, 0, EA_CTRL | A_POST, s, 4)) return 0;
    emit16(c, 0x4C80 | (s == 'l' ? 0x40 : 0) | ea_bits(ea));
  }
  emit16(c, mask & 0xffff);
  ea_emit(c, ea, s);
  return 1;
}

static int h_movep(ictx *c)
{
  int s = opsize(c);
  operand *a, *b, *m;
  int dn, tomem;
  eres r;
  if (!nops_is(c, 2)) return 0;
  a = OP(c, 0); b = OP(c, 1);
  if (is_dn(a)) { dn = a->reg; m = b; tomem = 1; }
  else if (is_dn(b)) { dn = b->reg; m = a; tomem = 0; }
  else { if (c->enc) error("illegal addressing mode"); return 0; }
  if (m->mode == M_IND) {
    emit16(c, 0x0108 | (dn << 9) | (tomem ? 0x80 : 0) | (s == 'l' ? 0x40 : 0) | m->reg);
    emit16(c, 0);
    return 1;
  }
  if (m->mode != M_MEM || m->base < 0 || m->base > 7 || m->xn >= 0 || m->memind) {
    if (c->enc) error("illegal addressing mode");
    return 0;
  }
  emit16(c, 0x0108 | (dn << 9) | (tomem ? 0x80 : 0) | (s == 'l' ? 0x40 : 0) | m->base);
  eval_op(c, m->bd, &r);
  base_adjust(c, m->base, &r);
  emit_val(c, &r, 2, 0, 0, -32768, 32767, "displacement out of range");
  return 1;
}

static int h_movec(ictx *c)
{
  operand *a, *b;
  int ctrl, rn, dir;
  if (!nops_is(c, 2)) return 0;
  cpu_check(c, C010UP);
  a = OP(c, 0); b = OP(c, 1);
  if ((a->mode == M_CTRL || a->mode == M_USP) && (is_dn(b) || is_an(b))) {
    ctrl = a->reg; rn = (is_an(b) ? 8 : 0) + b->reg; dir = 0;
  } else if ((b->mode == M_CTRL || b->mode == M_USP) && (is_dn(a) || is_an(a))) {
    ctrl = b->reg; rn = (is_an(a) ? 8 : 0) + a->reg; dir = 1;
  } else if (a->mode == M_MMU || b->mode == M_MMU) {
    if (c->enc) error("illegal control register");
    return 0;
  } else { if (c->enc) error("illegal addressing mode"); return 0; }
  emit16(c, 0x4E7A | dir);
  emit16(c, (rn << 12) | (ctrl & 0xfff));
  return 1;
}

static int h_moves(ictx *c)
{
  int s = opsize(c);
  operand *a, *b;
  if (!nops_is(c, 2)) return 0;
  cpu_check(c, C010UP);
  a = OP(c, 0); b = OP(c, 1);
  if (is_dn(a) || is_an(a)) {
    if (!ea_chk(c, 1, EA_MALT, s, 4)) return 0;
    emit16(c, 0x0E00 | (sz2(s) << 6) | ea_bits(b));
    emit16(c, (((is_an(a) ? 8 : 0) + a->reg) << 12) | 0x800);
    ea_emit(c, b, s);
    return 1;
  }
  if (is_dn(b) || is_an(b)) {
    if (!ea_chk(c, 0, EA_MALT, s, 4)) return 0;
    emit16(c, 0x0E00 | (sz2(s) << 6) | ea_bits(a));
    emit16(c, ((is_an(b) ? 8 : 0) + b->reg) << 12);
    ea_emit(c, a, s);
    return 1;
  }
  if (c->enc) error("illegal addressing mode");
  return 0;
}

static int h_cas(ictx *c)
{
  int s = opsize(c);
  if (!nops_is(c, 3)) return 0;
  cpu_check(c, C020UP);
  if (!is_dn(OP(c, 0)) || !is_dn(OP(c, 1))) { if (c->enc) error("data register required"); return 0; }
  if (!ea_chk(c, 2, EA_MALT, s, 4)) return 0;
  emit16(c, 0x08C0 | ((sz2(s) + 1) << 9) | ea_bits(OP(c, 2)));
  emit16(c, (OP(c, 1)->reg << 6) | OP(c, 0)->reg);
  ea_emit(c, OP(c, 2), s);
  return 1;
}

static int h_cas2(ictx *c)
{
  int s = opsize(c);
  operand *dc, *du, *rn;
  if (!nops_is(c, 3)) return 0;
  cpu_check(c, C020UP);
  dc = OP(c, 0); du = OP(c, 1); rn = OP(c, 2);
  if (dc->mode != M_PAIR || du->mode != M_PAIR || rn->mode != M_INDPAIR) {
    if (c->enc) error("illegal addressing mode");
    return 0;
  }
  emit16(c, s == 'l' ? 0x0EFC : 0x0CFC);
  emit16(c, (rn->reg << 12) | ((du->reg & 7) << 6) | (dc->reg & 7));
  emit16(c, (rn->reg2 << 12) | ((du->reg2 & 7) << 6) | (dc->reg2 & 7));
  return 1;
}

/* CMP2 / CHK2: arg 0 cmp2, 1 chk2 */
static int h_cmp2(ictx *c)
{
  int s = opsize(c);
  operand *r;
  if (!nops_is(c, 2)) return 0;
  cpu_check(c, C020UP | CCPU32);
  r = OP(c, 1);
  if (!is_dn(r) && !is_an(r)) { if (c->enc) error("register expected"); return 0; }
  if (!ea_chk(c, 0, EA_CTRL, s, 4)) return 0;
  emit16(c, (sz2(s) << 9) | 0x00C0 | ea_bits(OP(c, 0)));
  emit16(c, (((is_an(r) ? 8 : 0) + r->reg) << 12) | (c->ip->m->arg ? 0x800 : 0));
  ea_emit(c, OP(c, 0), s);
  return 1;
}

static int h_pack(ictx *c)
{
  operand *a, *b;
  eres r;
  if (!nops_is(c, 3)) return 0;
  cpu_check(c, C020UP);
  a = OP(c, 0); b = OP(c, 1);
  if (!is_imm(OP(c, 2))) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (is_dn(a) && is_dn(b)) emit16(c, c->ip->m->opc | (b->reg << 9) | a->reg);
  else if (a->mode == M_PRE && b->mode == M_PRE) emit16(c, c->ip->m->opc | 8 | (b->reg << 9) | a->reg);
  else { if (c->enc) error("illegal addressing mode"); return 0; }
  eval_op(c, OP(c, 2)->bd, &r);
  emit_val(c, &r, 2, 0, 0, -32768, 65535, "immediate operand out of range");
  return 1;
}

/* bitfield: arg bit0 = has data register (1 src dn ea / 2 ea dn) */
static int h_bf(ictx *c)
{
  int kind = c->ip->m->arg;   /* 0 ea only, 1 ea,Dn, 2 Dn,ea (bfins) */
  operand *ea, *dn = NULL;
  uint32_t ext = 0;
  ival v;
  unsigned allowed;
  cpu_check(c, C020UP);
  if (kind == 0) { if (!nops_is(c, 1)) return 0; ea = OP(c, 0); }
  else if (kind == 1) { if (!nops_is(c, 2)) return 0; ea = OP(c, 0); dn = OP(c, 1); }
  else { if (!nops_is(c, 2)) return 0; dn = OP(c, 0); ea = OP(c, 1); }
  if (!ea->bf) { if (c->enc) error("missing bitfield specification"); return 0; }
  if (dn && !(dn->mode == M_DN)) { if (c->enc) error("data register required"); return 0; }
  if (dn) ext |= dn->reg << 12;
  if (ea->bfo_reg >= 0) ext |= 0x800 | (ea->bfo_reg << 6);
  else { if (!eval_const(ea->bfo, &v)) v = 0; if (c->enc && (v < 0 || v > 31)) error("illegal bitfield width/offset"); ext |= (v & 31) << 6; }
  if (ea->bfw_reg >= 0) ext |= 0x20 | ea->bfw_reg;
  else { if (!eval_const(ea->bfw, &v)) v = 32; if (c->enc && (v < 1 || v > 32)) error("illegal bitfield width/offset"); ext |= v & 31; }
  allowed = (c->ip->m->opc == 0xE8C0 || c->ip->m->opc == 0xE9C0 || c->ip->m->opc == 0xEBC0 || c->ip->m->opc == 0xEDC0)
            ? (EA_CTRL | A_DN) : (EA_CALT | A_DN);
  {
    int i = kind == 2 ? 1 : 0;
    if (!ea_chk(c, i, allowed, 'l', 4)) return 0;
  }
  emit16(c, c->ip->m->opc | ea_bits(ea));
  emit16(c, ext);
  ea_emit(c, ea, 'l');
  return 1;
}

static int h_callm(ictx *c)
{
  eres r;
  if (!nops_is(c, 2)) return 0;
  cpu_check(c, C020);
  if (!is_imm(OP(c, 0))) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (!ea_chk(c, 1, EA_CTRL, 'l', 4)) return 0;
  emit16(c, 0x06C0 | ea_bits(OP(c, 1)));
  eval_op(c, OP(c, 0)->bd, &r);
  emit_val(c, &r, 2, 0, 0, 0, 255, "immediate operand out of range");
  ea_emit(c, OP(c, 1), 'l');
  return 1;
}

static int h_rtm(ictx *c)
{
  operand *o;
  if (!nops_is(c, 1)) return 0;
  cpu_check(c, C020);
  o = OP(c, 0);
  if (!is_dn(o) && !is_an(o)) { if (c->enc) error("register expected"); return 0; }
  emit16(c, 0x06C0 | (is_an(o) ? 8 : 0) | o->reg);
  return 1;
}

/* ------------------------------------------------- 040/060 specials */
static int h_move16(ictx *c)
{
  operand *a, *b;
  eres r;
  if (!nops_is(c, 2)) return 0;
  cpu_check(c, C040 | C060);
  a = OP(c, 0); b = OP(c, 1);
  if (a->mode == M_POST && b->mode == M_POST) {
    emit16(c, 0xF620 | a->reg);
    emit16(c, 0x8000 | (b->reg << 12));
    return 1;
  }
  if ((a->mode == M_POST || a->mode == M_IND) && b->mode == M_ABS) {
    emit16(c, (a->mode == M_POST ? 0xF600 : 0xF610) | a->reg);
    eval_op(c, b->bd, &r);
    emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "address out of range");
    return 1;
  }
  if (a->mode == M_ABS && (b->mode == M_POST || b->mode == M_IND)) {
    emit16(c, (b->mode == M_POST ? 0xF608 : 0xF618) | b->reg);
    eval_op(c, a->bd, &r);
    emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "address out of range");
    return 1;
  }
  if (c->enc) error("illegal addressing mode");
  return 0;
}

/* CINVx / CPUSHx; m->arg = scope (1 line, 2 page, 3 all) */
static int h_cache(ictx *c)
{
  int scope = c->ip->m->arg;
  operand *a;
  cpu_check(c, C040 | C060);
  if (c->ip->nops < 1) { if (c->enc) error("missing operand"); return 0; }
  a = OP(c, 0);
  if (a->mode != M_CACHE) { if (c->enc) error("cache specifier expected"); return 0; }
  if (scope == 3) {
    if (c->ip->nops != 1) { if (c->enc) error("too many operands"); }
    emit16(c, c->ip->m->opc | (a->reg << 6) | (scope << 3));
    return 1;
  }
  if (!nops_is(c, 2)) return 0;
  if (OP(c, 1)->mode != M_IND) { if (c->enc) error("illegal addressing mode"); return 0; }
  emit16(c, c->ip->m->opc | (a->reg << 6) | (scope << 3) | OP(c, 1)->reg);
  return 1;
}

/* (An) only: PFLUSH040 etc / PLPA */
static int h_an_ind(ictx *c)
{
  if (!nops_is(c, 1)) return 0;
  cpu_check(c, c->ip->m->cpus);
  if (OP(c, 0)->mode != M_IND) { if (c->enc) error("illegal addressing mode"); return 0; }
  emit16(c, c->ip->m->opc | OP(c, 0)->reg);
  return 1;
}

static int h_lpstop(ictx *c)
{
  eres r;
  if (!nops_is(c, 1)) return 0;
  cpu_check(c, C060);
  if (!is_imm(OP(c, 0))) { if (c->enc) error("illegal addressing mode"); return 0; }
  emit16(c, 0xF800);
  emit16(c, 0x01C0);
  eval_op(c, OP(c, 0)->bd, &r);
  emit_val(c, &r, 2, 0, 0, -32768, 65535, "immediate operand out of range");
  return 1;
}

/* ------------------------------------------------------------------ FPU */
static int fpu_fmt(int s)
{
  switch (s) {
  case 'l': return 0;
  case 's': return 1;
  case 'x': return 2;
  case 'p': return 3;
  case 'w': return 4;
  case 'd': return 5;
  case 'b': return 6;
  }
  return 2;
}

static uint32_t fpw1(ictx *c, uint32_t type)
{
  return 0xF000 | ((c->ip->fpuid & 7) << 9) | type;
}

static void fpu_check(ictx *c)
{
  if (c->enc && !(c->ip->cpu & CFPU))
    error("instruction not supported on selected architecture");
}

/* lowest precision for a float immediate (opt-fconst) */
static int fconst_size(ictx *c, operand *o, int s)
{
  eres r;
  long double f;
  if (!is_imm(o)) return s;
  eval_expr(o->bd, &r);
  if (!is_abs(&r) || !r.defined) return s;
  if (!r.isflt) {
    if (o->rawhex && (s == 's' || s == 'd' || s == 'x' || s == 'p')) return s;
    if (!OPT(c) || !opt_fconst) return s ? s : 'w';
    if (s == 's' || s == 'd' || s == 'x' || !s) return fits16(r.v) ? 'w' : 'l';
    return s;
  }
  if (!OPT(c) || !opt_fconst) return s;
  if (s == 'p' || s == 'l' || s == 'w' || s == 'b') return s;
  f = r.f;
  if ((long double)(float)f == f) return 's';
  if ((long double)(double)f == f) return s == 's' ? 's' : 'd';
  return s;
}

/* fdiv #2^n -> fmul #2^-n (opt-fconst) */
static int fdiv_pow2(ictx *c, operand *o, int opmode, long double *recip)
{
  eres r;
  long double f;
  int e;
  if (!is_imm(o) || !OPT(c) || !opt_fconst || o->rawhex) return opmode;
  if (c->ip->size != 's' && c->ip->size != 'd' && c->ip->size != 'x' && !expr_is_float(o->bd)) return opmode;
  if (opmode != 0x20 && opmode != 0x24 && opmode != 0x60 && opmode != 0x64) return opmode;
  eval_expr(o->bd, &r);
  if (!is_abs(&r) || !r.defined) return opmode;
  f = r.isflt ? r.f : (long double)r.v;
  if (f == 0) return opmode;
  if (frexpl(f < 0 ? -f : f, &e) != 0.5L) return opmode;
  *recip = 1.0L / f;
  return opmode + 3;
}

static int is_fpn(operand *o) { return o->mode == M_FP; }

/* general arithmetic: arg = opmode, opc2 = 1 for monadic, 2 for ftst, 3 fsincos */
static int h_fgen(ictx *c)
{
  int opmode = c->ip->m->arg;
  int kind = c->ip->m->opc2;
  int s = c->ip->size;
  operand *src, *dst = NULL;
  int dreg, creg = 0;
  fpu_check(c);
  cpu_check(c, c->ip->m->cpus);
  if (c->ip->nops < 1 || c->ip->nops > 2) { if (c->enc) error("missing operand"); return 0; }
  src = OP(c, 0);
  if (c->ip->nops == 2) dst = OP(c, 1);
  if (kind == 2) {          /* ftst */
    if (dst) { if (c->enc) error("too many operands"); }
    dreg = 0;
  } else if (kind == 3) {   /* fsincos src,fpc:fps */
    if (!dst || dst->mode != M_PAIR || !dst->fpmask) { if (c->enc) error("illegal addressing mode"); return 0; }
    creg = dst->reg; dreg = dst->reg2;
  } else if (!dst) {
    if (kind != 1 || !is_fpn(src)) { if (c->enc) error("missing operand"); return 0; }
    dreg = src->reg;
  } else {
    if (!is_fpn(dst)) { if (c->enc) error("FP register required"); return 0; }
    dreg = dst->reg;
  }
  if (is_fpn(src)) {
    emit16(c, fpw1(c, 0));
    emit16(c, (src->reg << 10) | (dreg << 7) | opmode | creg);
    return 1;
  }
  {
    long double recip;
    int om2 = fdiv_pow2(c, src, opmode, &recip);
    if (om2 != opmode) {
      expr *save = src->bd;
      int ok;
      src->bd = new_flt(recip);
      s = fconst_size(c, src, 'x');
      ok = ea_chk(c, 0, A_IMM, s, 4);
      if (ok) {
        emit16(c, fpw1(c, ea_bits(src)));
        emit16(c, 0x4000 | (fpu_fmt(s) << 10) | (dreg << 7) | om2 | creg);
        ea_emit(c, src, s);
      }
      src->bd = save;
      return ok;
    }
  }
  if (is_imm(src)) s = fconst_size(c, src, s);
  if (!s) s = 'w';
  {
    unsigned allowed = EA_DATA;
    if (s != 'b' && s != 'w' && s != 'l' && s != 's') allowed &= ~A_DN;
    if (!ea_chk(c, 0, allowed, s, 4)) return 0;
  }
  emit16(c, fpw1(c, ea_bits(src)));
  emit16(c, 0x4000 | (fpu_fmt(s) << 10) | (dreg << 7) | opmode | creg);
  ea_emit(c, src, s);
  return 1;
}

static int h_fmove(ictx *c)
{
  int s = c->ip->size;
  operand *a, *b;
  fpu_check(c);
  if (!nops_is(c, 2)) return 0;
  a = OP(c, 0); b = OP(c, 1);
  if (a->mode == M_FPCR || b->mode == M_FPCR) {
    int toreg = b->mode == M_FPCR;
    operand *ea = toreg ? a : b;
    operand *cr = toreg ? b : a;
    unsigned allowed = toreg ? EA_ALL : EA_ALT;
    if (cr->mask != 1) allowed &= ~A_AN;
    if (cr->mask & (cr->mask - 1)) { if (c->enc) error("illegal addressing mode"); return 0; }
    if (!ea_chk(c, toreg ? 0 : 1, allowed, 'l', 4)) return 0;
    emit16(c, fpw1(c, ea_bits(ea)));
    emit16(c, (toreg ? 0x8000 : 0xA000) | (cr->mask << 10));
    ea_emit(c, ea, 'l');
    return 1;
  }
  if (is_fpn(b)) {
    /* fmove ea,fpn / fpm,fpn: general opmode 0 */
    if (is_fpn(a)) {
      emit16(c, fpw1(c, 0));
      emit16(c, (a->reg << 10) | (b->reg << 7) | c->ip->m->arg);
      return 1;
    }
    if (is_imm(a)) s = fconst_size(c, a, s);
    if (!s) s = 'w';
    {
      unsigned allowed = EA_DATA;
      if (s != 'b' && s != 'w' && s != 'l' && s != 's') allowed &= ~A_DN;
      if (!ea_chk(c, 0, allowed, s, 4)) return 0;
    }
    emit16(c, fpw1(c, ea_bits(a)));
    emit16(c, 0x4000 | (fpu_fmt(s) << 10) | (b->reg << 7) | c->ip->m->arg);
    ea_emit(c, a, s);
    return 1;
  }
  if (is_fpn(a) && c->ip->m->arg == 0) {
    /* fmove fpn,ea */
    uint32_t w2;
    unsigned allowed = EA_DALT;
    if (!s) s = 'x';
    if (s != 'b' && s != 'w' && s != 'l' && s != 's') allowed &= ~A_DN;
    if (!ea_chk(c, 1, allowed, s, 4)) return 0;
    w2 = 0x6000 | (fpu_fmt(s) << 10) | (a->reg << 7);
    if (s == 'p') {
      if (b->kf && b->kf_reg >= 0) w2 = 0x6000 | (7 << 10) | (a->reg << 7) | (b->kf_reg << 4);
      else if (b->kf) {
        ival k = 0;
        eval_const(b->kfe, &k);
        if (c->enc && (k < -64 || k > 63)) error("value from -64 to 63 required for k-factor");
        w2 |= k & 0x7f;
      }
    }
    emit16(c, fpw1(c, ea_bits(b)));
    emit16(c, w2);
    ea_emit(c, b, s);
    return 1;
  }
  if (c->enc) error("illegal addressing mode");
  return 0;
}

static int h_fmovecr(ictx *c)
{
  ival v = 0;
  fpu_check(c);
  if (!nops_is(c, 2)) return 0;
  if (!is_imm(OP(c, 0)) || !is_fpn(OP(c, 1))) { if (c->enc) error("illegal addressing mode"); return 0; }
  eval_const(OP(c, 0)->bd, &v);
  if (c->enc && (v < 0 || v > 127)) error("immediate operand out of range");
  emit16(c, fpw1(c, 0));
  emit16(c, 0x5C00 | (OP(c, 1)->reg << 7) | (v & 0x7f));
  return 1;
}

static uint32_t rev8(uint32_t m)
{
  uint32_t r = 0;
  int i;
  for (i = 0; i < 8; i++) if (m & (1u << i)) r |= 1u << (7 - i);
  return r;
}

static int h_fmovem(ictx *c)
{
  int s = c->ip->size;
  operand *a, *b;
  fpu_check(c);
  if (!nops_is(c, 2)) return 0;
  a = OP(c, 0); b = OP(c, 1);
  /* control registers */
  if (a->mode == M_FPCR || b->mode == M_FPCR) {
    int toreg = b->mode == M_FPCR;
    operand *ea = toreg ? a : b;
    operand *cr = toreg ? b : a;
    unsigned allowed;
    int single = !(cr->mask & (cr->mask - 1));
    if (s && s != 'l' && c->enc) error("illegal opcode extension");
    if (toreg) allowed = single ? EA_ALL : (EA_ALL & ~(A_AN | A_DN));
    else allowed = single ? EA_ALT : EA_MALT;
    if (single && cr->mask != 1) allowed &= ~A_AN;
    if (!single && toreg) allowed |= A_IMM;
    if (!ea_chk(c, toreg ? 0 : 1, allowed, 'l', 4)) return 0;
    emit16(c, fpw1(c, ea_bits(ea)));
    emit16(c, (toreg ? 0x8000 : 0xA000) | (cr->mask << 10));
    if (ea->form == F_IMM) {
      /* one long per register */
      int i, n = 0;
      for (i = 0; i < 3; i++) if (cr->mask & (1 << i)) n++;
      (void)n;
      ea_emit(c, ea, 'l');
    } else
      ea_emit(c, ea, 'l');
    return 1;
  }
  {
    int tomem;
    operand *ea, *rl;
    uint32_t w2;
    if ((a->mode == M_REGLIST && a->fpmask == 1) || is_fpn(a) || (is_dn(a) && b->mode != M_REGLIST && !is_fpn(b))) {
      tomem = 1; rl = a; ea = b;
    } else { tomem = 0; rl = b; ea = a; }
    if (rl->mode == M_REGLIST && rl->mask == 0 && OPT(c)) return 1;
    if (!ea_chk(c, tomem ? 1 : 0, tomem ? (EA_CALT | A_PRE) : (EA_CTRL | A_POST), 'x', 4)) return 0;
    if (is_dn(rl)) {
      /* dynamic list */
      int mode = ea->form == F_PRE ? 1 : 3;
      w2 = 0xC000 | (tomem ? 0x2000 : 0) | (mode << 11) | (rl->reg << 4);
    } else {
      uint32_t m = rl->mask & 0xff;
      int mode = ea->form == F_PRE ? 0 : 2;
      if (mode == 2) m = rev8(m);
      w2 = 0xC000 | (tomem ? 0x2000 : 0) | (mode << 11) | m;
    }
    emit16(c, fpw1(c, ea_bits(ea)));
    emit16(c, w2);
    ea_emit(c, ea, 'x');
    return 1;
  }
}

/* FBcc / PBcc: arg = cc, opc = base */
static int h_fbcc(ictx *c)
{
  int s = c->ip->size;
  eres r;
  ival d;
  int known;
  int isf = c->ip->m->opc2;
  uint32_t base = isf ? fpw1(c, 0x80) : 0xF080;
  if (isf) fpu_check(c); else cpu_check(c, CMMU);
  if (!nops_is(c, 1)) return 0;
  if (OP(c, 0)->mode != M_ABS) { if (c->enc) error("illegal addressing mode"); return 0; }
  known = branch_disp(c, OP(c, 0)->bd, c->pc + 2, &d, &r);
  if (!s || (opt_allbra && OPT(c))) {
    if (!OPT(c)) s = 'w';
    else if (known && fits16(fwd_adj(c, &r, d, 4)) && !(grow_only && c->ip->lastsize > 4)) s = 'w';
    else if (known) s = 'l';
    else s = 'w';
  }
  if (s == 'l') {
    emit16(c, base | 0x40 | c->ip->m->arg);
    emit_val(c, &r, 4, 1, c->pc + 2, -0x80000000LL, 0x7fffffffLL, "branch destination out of range");
  } else {
    emit16(c, base | c->ip->m->arg);
    emit_val(c, &r, 2, 1, c->pc + 2, -32768, 32767, "branch destination out of range");
  }
  return 1;
}

/* FDBcc / PDBcc */
static int h_fdbcc(ictx *c)
{
  eres r;
  ival d;
  int isf = c->ip->m->opc2;
  if (isf) fpu_check(c); else cpu_check(c, CMMU);
  if (!nops_is(c, 2)) return 0;
  if (!is_dn(OP(c, 0)) || OP(c, 1)->mode != M_ABS) { if (c->enc) error("illegal addressing mode"); return 0; }
  emit16(c, (isf ? fpw1(c, 0x48) : 0xF048) | OP(c, 0)->reg);
  emit16(c, c->ip->m->arg);
  branch_disp(c, OP(c, 1)->bd, c->pc + 4, &d, &r);
  emit_val(c, &r, 2, 1, c->pc + 4, -32768, 32767, "branch destination out of range");
  return 1;
}

/* FScc / PScc */
static int h_fscc(ictx *c)
{
  int isf = c->ip->m->opc2;
  if (isf) fpu_check(c); else cpu_check(c, CMMU);
  if (!nops_is(c, 1)) return 0;
  if (!ea_chk(c, 0, EA_DALT, 'b', 4)) return 0;
  emit16(c, (isf ? fpw1(c, 0x40) : 0xF040) | ea_bits(OP(c, 0)));
  emit16(c, c->ip->m->arg);
  ea_emit(c, OP(c, 0), 'b');
  return 1;
}

/* FTRAPcc / PTRAPcc */
static int h_ftrapcc(ictx *c)
{
  int s = c->ip->size;
  int isf = c->ip->m->opc2;
  uint32_t base = isf ? fpw1(c, 0x78) : 0xF078;
  if (isf) fpu_check(c); else cpu_check(c, CMMU);
  if (c->ip->nops == 0) {
    emit16(c, base | 4);
    emit16(c, c->ip->m->arg);
    return 1;
  }
  if (!nops_is(c, 1) || !is_imm(OP(c, 0))) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (!s) {
    ival v;
    s = (imm_value(c, OP(c, 0), &v) && fits16(v)) ? 'w' : 'l';
  }
  emit16(c, base | (s == 'l' ? 3 : 2));
  emit16(c, c->ip->m->arg);
  emit_imm(c, OP(c, 0)->bd, s == 'l' ? 'l' : 'w');
  return 1;
}

/* FSAVE / FRESTORE / PSAVE / PRESTORE: arg 0 save, 1 restore */
static int h_fsave(ictx *c)
{
  int isf = c->ip->m->opc2;
  unsigned allowed = c->ip->m->arg ? (EA_CTRL | A_POST) : (EA_CALT | A_PRE);
  if (isf) fpu_check(c); else cpu_check(c, CMMU);
  if (!nops_is(c, 1)) return 0;
  if (!ea_chk(c, 0, allowed, 'l', 2)) return 0;
  emit16(c, (isf ? fpw1(c, c->ip->m->arg ? 0x140 : 0x100) : c->ip->m->opc) | ea_bits(OP(c, 0)));
  ea_emit(c, OP(c, 0), 'l');
  return 1;
}

static int h_fnop(ictx *c)
{
  fpu_check(c);
  emit16(c, fpw1(c, 0x80));
  emit16(c, 0);
  return 1;
}

/* ------------------------------------------------------------------ MMU */
static int mmu_fc(ictx *c, operand *o, int *fc)
{
  ival v;
  if (o->mode == M_CTRL && o->reg == 0) { *fc = 0; return 1; }
  if (o->mode == M_CTRL && o->reg == 1) { *fc = 1; return 1; }
  if (is_dn(o)) { *fc = 8 | o->reg; return 1; }
  if (is_imm(o) && eval_const(o->bd, &v)) { *fc = 0x10 | (v & 15); return 1; }
  if (c->enc) error("illegal function code");
  return 0;
}

static int mmu_ok(ictx *c)
{
  return (c->ip->cpu & (C030 | CMMU | C040 | C060)) != 0;
}

/* pmove register code: returns word2 base (without rw), -1 if bad */
static int pmove_reg(ictx *c, operand *o, int *size)
{
  int r;
  *size = 'l';
  if (o->mode == M_CTRL) {
    if (o->reg == 0x003) { *size = 'l'; return 0x4000; }            /* tc */
    if (o->reg == 0x807) { *size = 'd'; return 0x4800; }            /* srp */
    if (o->reg == 0x805) { *size = 'w'; return 0x6000; }            /* mmusr */
    return -1;
  }
  if (o->mode != M_MMU) return -1;
  r = o->reg;
  switch (r) {
  case 3: *size = 'd'; return 0x4C00;     /* crp */
  case 1: *size = 'd'; return 0x4400;     /* drp */
  case 4: *size = 'b'; return 0x5000;     /* cal */
  case 5: *size = 'b'; return 0x5400;     /* val */
  case 6: *size = 'b'; return 0x5800;     /* scc */
  case 7: *size = 'w'; return 0x5C00;     /* ac */
  case 0x18: *size = 'w'; return 0x6000;  /* psr */
  case 0x19: *size = 'w'; return 0x6400;  /* pcsr */
  case 0x22: *size = 'l'; return 0x0800;  /* tt0 */
  case 0x23: *size = 'l'; return 0x0C00;  /* tt1 */
  }
  if (r >= 0x30 && r < 0x38) { *size = 'w'; return 0x7000 | ((r - 0x30) << 2); }
  if (r >= 0x38 && r < 0x40) { *size = 'w'; return 0x7400 | ((r - 0x38) << 2); }
  (void)c;
  return -1;
}

static int h_pmove(ictx *c)
{
  operand *a, *b;
  int w, size, toreg;
  if (!mmu_ok(c) && c->enc) error("instruction not supported on selected architecture");
  if (!nops_is(c, 2)) return 0;
  a = OP(c, 0); b = OP(c, 1);
  w = pmove_reg(c, b, &size);
  toreg = 1;
  if (w < 0) { w = pmove_reg(c, a, &size); toreg = 0; }
  if (w < 0) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (c->ip->m->arg) w |= 0x100;     /* pmovefd */
  {
    int i = toreg ? 0 : 1;
    unsigned allowed = toreg ? EA_ALL : EA_ALT;
    if (size == 'd' || size == 'q') allowed &= ~(A_DN | A_AN);
    if (!ea_chk(c, i, allowed, size == 'd' ? 'd' : size, 4)) return 0;
    emit16(c, 0xF000 | ea_bits(OP(c, i)));
    emit16(c, w | (toreg ? 0 : 0x200));
    if (OP(c, i)->form == F_IMM && size == 'd') {
      eres r;
      eval_op(c, OP(c, i)->bd, &r);
      emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "immediate operand out of range");
      emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "immediate operand out of range");
    } else
      ea_emit(c, OP(c, i), size);
  }
  return 1;
}

static int h_pflusha(ictx *c)
{
  if (c->ip->nops) { if (c->enc) error("too many operands"); }
  if (c->ip->cpu & (C040 | C060)) { emit16(c, 0xF518); return 1; }
  if (!mmu_ok(c) && c->enc) error("instruction not supported on selected architecture");
  emit16(c, 0xF000);
  emit16(c, 0x2400);
  return 1;
}

/* pflush: 040 form (An) or 030/851 form fc,#mask[,ea]; arg 1 = pflushs */
static int h_pflush(ictx *c)
{
  int fc;
  ival mask;
  if (c->ip->nops == 1 && OP(c, 0)->mode == M_IND && !c->ip->m->arg) {
    cpu_check(c, C040 | C060);
    emit16(c, 0xF508 | OP(c, 0)->reg);
    return 1;
  }
  if (c->ip->nops < 2 || c->ip->nops > 3) { if (c->enc) error("missing operand"); return 0; }
  if (!mmu_ok(c) && c->enc) error("instruction not supported on selected architecture");
  if (!mmu_fc(c, OP(c, 0), &fc)) return 0;
  if (!is_imm(OP(c, 1)) || !eval_const(OP(c, 1)->bd, &mask)) { if (c->enc) error("illegal addressing mode"); return 0; }
  if (c->ip->nops == 3) {
    if (!ea_chk(c, 2, EA_CALT, 'l', 4)) return 0;
    emit16(c, 0xF000 | ea_bits(OP(c, 2)));
    emit16(c, 0x3000 | ((c->ip->m->arg ? 7 : 6) << 10) | ((mask & 15) << 5) | fc);
    ea_emit(c, OP(c, 2), 'l');
    return 1;
  }
  emit16(c, 0xF000);
  emit16(c, 0x3000 | ((c->ip->m->arg ? 5 : 4) << 10) | ((mask & 15) << 5) | fc);
  return 1;
}

static int h_pflushr(ictx *c)
{
  cpu_check(c, CMMU);
  if (!nops_is(c, 1)) return 0;
  if (!ea_chk(c, 0, EA_ALL & ~(A_DN | A_AN), 'd', 4)) return 0;
  emit16(c, 0xF000 | ea_bits(OP(c, 0)));
  emit16(c, 0xA000);
  if (OP(c, 0)->form == F_IMM) {
    eres r;
    eval_op(c, OP(c, 0)->bd, &r);
    emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "immediate operand out of range");
    emit_val(c, &r, 4, 0, 0, -0x80000000LL, 0xffffffffLL, "immediate operand out of range");
  } else
    ea_emit(c, OP(c, 0), 'd');
  return 1;
}

/* ploadr/ploadw: arg 1 = read */
static int h_pload(ictx *c)
{
  int fc;
  if (!nops_is(c, 2)) return 0;
  if (!mmu_ok(c) && c->enc) error("instruction not supported on selected architecture");
  if (!mmu_fc(c, OP(c, 0), &fc)) return 0;
  if (!ea_chk(c, 1, EA_CALT, 'l', 4)) return 0;
  emit16(c, 0xF000 | ea_bits(OP(c, 1)));
  emit16(c, 0x2000 | (c->ip->m->arg ? 0x200 : 0) | fc);
  ea_emit(c, OP(c, 1), 'l');
  return 1;
}

/* ptestr/ptestw: arg 1 = read */
static int h_ptest(ictx *c)
{
  int fc;
  ival level;
  uint32_t w;
  if (c->ip->nops == 1 && OP(c, 0)->mode == M_IND) {
    cpu_check(c, C040 | C060);
    emit16(c, (c->ip->m->arg ? 0xF568 : 0xF548) | OP(c, 0)->reg);
    return 1;
  }
  if (c->ip->nops < 3 || c->ip->nops > 4) { if (c->enc) error("missing operand"); return 0; }
  if (!mmu_ok(c) && c->enc) error("instruction not supported on selected architecture");
  if (!mmu_fc(c, OP(c, 0), &fc)) return 0;
  if (!is_imm(OP(c, 2)) || !eval_const(OP(c, 2)->bd, &level)) { if (c->enc) error("illegal addressing mode"); return 0; }
  w = 0x8000 | ((level & 7) << 10) | (c->ip->m->arg ? 0x200 : 0) | fc;
  if (c->ip->nops == 4) {
    if (!is_an(OP(c, 3))) { if (c->enc) error("address register required"); return 0; }
    w |= 0x100 | (OP(c, 3)->reg << 5);
  }
  if (!ea_chk(c, 1, EA_CALT, 'l', 4)) return 0;
  emit16(c, 0xF000 | ea_bits(OP(c, 1)));
  emit16(c, w);
  ea_emit(c, OP(c, 1), 'l');
  return 1;
}

static int h_pvalid(ictx *c)
{
  operand *a;
  uint32_t w;
  cpu_check(c, CMMU);
  if (!nops_is(c, 2)) return 0;
  a = OP(c, 0);
  if (a->mode == M_MMU && a->reg == 5) w = 0x2800;
  else if (is_an(a)) w = 0x2C00 | a->reg;
  else { if (c->enc) error("illegal addressing mode"); return 0; }
  if (!ea_chk(c, 1, EA_CALT, 'l', 4)) return 0;
  emit16(c, 0xF000 | ea_bits(OP(c, 1)));
  emit16(c, w);
  ea_emit(c, OP(c, 1), 'l');
  return 1;
}

/* ------------------------------------------------------------ the table */
#define UP C000UP
static const mnem base_table[] = {
  /* name      opc     opc2  sizes            def  cpus           handler   arg */
  { "abcd",   0xC100, 0, SZ_B|SZ_U,          'b', UP,            h_addx, 0 },
  { "sbcd",   0x8100, 0, SZ_B|SZ_U,          'b', UP,            h_addx, 0 },
  { "addx",   0xD100, 0, SZ_BWL|SZ_U,        'w', UP,            h_addx, 0 },
  { "subx",   0x9100, 0, SZ_BWL|SZ_U,        'w', UP,            h_addx, 0 },
  { "add",    0xD000, 0, SZ_BWL|SZ_U,        'w', UP,            h_addsub, 0 },
  { "sub",    0x9000, 0, SZ_BWL|SZ_U,        'w', UP,            h_addsub, 1 },
  { "adda",   0xD000, 0, SZ_W|SZ_L|SZ_U,     'w', UP,            h_adda, 0 },
  { "suba",   0x9000, 0, SZ_W|SZ_L|SZ_U,     'w', UP,            h_adda, 1 },
  { "addi",   0x0600, 0, SZ_BWL|SZ_U,        'w', UP,            h_addi, 0 },
  { "subi",   0x0400, 0, SZ_BWL|SZ_U,        'w', UP,            h_addi, 1 },
  { "addq",   0x5000, 0, SZ_BWL|SZ_U,        'w', UP,            h_addq, 0 },
  { "subq",   0x5100, 0, SZ_BWL|SZ_U,        'w', UP,            h_addq, 1 },
  { "and",    0xC000, 0, SZ_BWL|SZ_U,        'w', UP,            h_logic, 0 },
  { "or",     0x8000, 0, SZ_BWL|SZ_U,        'w', UP,            h_logic, 1 },
  { "eor",    0xB000, 0, SZ_BWL|SZ_U,        'w', UP,            h_logic, 2 },
  { "andi",   0x0200, 1, SZ_BWL|SZ_U,        'w', UP,            h_logic, 0 },
  { "ori",    0x0000, 1, SZ_BWL|SZ_U,        'w', UP,            h_logic, 1 },
  { "eori",   0x0A00, 1, SZ_BWL|SZ_U,        'w', UP,            h_logic, 2 },
  { "cmp",    0xB000, 0, SZ_BWL|SZ_U,        'w', UP,            h_cmp, 0 },
  { "cmpa",   0xB0C0, 0, SZ_W|SZ_L|SZ_U,     'w', UP,            h_cmp, 1 },
  { "cmpi",   0x0C00, 0, SZ_BWL|SZ_U,        'w', UP,            h_cmp, 2 },
  { "cmpm",   0xB108, 0, SZ_BWL|SZ_U,        'w', UP,            h_cmp, 3 },
  { "clr",    0x4200, 0, SZ_BWL|SZ_U,        'w', UP,            h_unary, 0 },
  { "neg",    0x4400, 0, SZ_BWL|SZ_U,        'w', UP,            h_unary, 0 },
  { "negx",   0x4000, 0, SZ_BWL|SZ_U,        'w', UP,            h_unary, 0 },
  { "not",    0x4600, 0, SZ_BWL|SZ_U,        'w', UP,            h_unary, 0 },
  { "tst",    0x4A00, 0, SZ_BWL|SZ_U,        'w', UP,            h_unary, 0 },
  { "move",   0x0000, 0, SZ_BWL|SZ_U,        'w', UP,            h_move, 0 },
  { "movea",  0x0040, 0, SZ_BWL|SZ_U,        'w', UP,            h_movea, 0 },
  { "moveq",  0x7000, 0, SZ_L|SZ_U,          'l', UP,            h_moveq, 0 },
  { "movem",  0x4880, 0, SZ_W|SZ_L|SZ_U,     'w', UP,            h_movem, 0 },
  { "movep",  0x0108, 0, SZ_W|SZ_L|SZ_U,     'w', UP,            h_movep, 0 },
  { "movec",  0x4E7A, 0, SZ_L|SZ_U,          'l', C010UP,        h_movec, 0 },
  { "moves",  0x0E00, 0, SZ_BWL|SZ_U,        'w', C010UP,        h_moves, 0 },
  { "lea",    0x41C0, 0, SZ_L|SZ_U,          'l', UP,            h_lea, 0 },
  { "pea",    0x4840, 0, SZ_L|SZ_U,          'l', UP,            h_ea, EA_CTRL },
  { "jmp",    0x4EC0, 0, SZ_U,               0,   UP,            h_jmp, 0 },
  { "jsr",    0x4E80, 0, SZ_U,               0,   UP,            h_jmp, 1 },
  { "nbcd",   0x4800, 0, SZ_B|SZ_U,          'b', UP,            h_ea, EA_DALT },
  { "tas",    0x4AC0, 0, SZ_B|SZ_U,          'b', UP,            h_ea, EA_DALT },
  { "asl",    0, 0, SZ_BWL|SZ_U,             'w', UP,            h_shift, 0|4 },
  { "asr",    0, 0, SZ_BWL|SZ_U,             'w', UP,            h_shift, 0 },
  { "lsl",    0, 0, SZ_BWL|SZ_U,             'w', UP,            h_shift, 1|4 },
  { "lsr",    0, 0, SZ_BWL|SZ_U,             'w', UP,            h_shift, 1 },
  { "roxl",   0, 0, SZ_BWL|SZ_U,             'w', UP,            h_shift, 2|4 },
  { "roxr",   0, 0, SZ_BWL|SZ_U,             'w', UP,            h_shift, 2 },
  { "rol",    0, 0, SZ_BWL|SZ_U,             'w', UP,            h_shift, 3|4 },
  { "ror",    0, 0, SZ_BWL|SZ_U,             'w', UP,            h_shift, 3 },
  { "btst",   0, 0, SZ_B|SZ_L|SZ_U,          0,   UP,            h_bitop, 0 },
  { "bchg",   0, 0, SZ_B|SZ_L|SZ_U,          0,   UP,            h_bitop, 1 },
  { "bclr",   0, 0, SZ_B|SZ_L|SZ_U,          0,   UP,            h_bitop, 2 },
  { "bset",   0, 0, SZ_B|SZ_L|SZ_U,          0,   UP,            h_bitop, 3 },
  { "bra",    0, 0, SZ_B|SZ_S|SZ_W|SZ_L|SZ_U, 0,  UP,            h_bcc, 0 },
  { "bsr",    0, 0, SZ_B|SZ_S|SZ_W|SZ_L|SZ_U, 0,  UP,            h_bcc, 1 },
  { "dbra",   0, 0, SZ_W|SZ_U,               0,   UP,            h_dbcc, 1 },
  { "exg",    0xC100, 0, SZ_L|SZ_U,          'l', UP,            h_exg, 0 },
  { "ext",    0x4880, 0, SZ_W|SZ_L|SZ_U,     'w', UP,            h_ext, 0 },
  { "extb",   0x49C0, 0, SZ_L|SZ_U,          'l', C020CPU32,     h_ext, 1 },
  { "swap",   0x4840, 0, SZ_W|SZ_U,          0,   UP,            h_reg, 0 },
  { "unlk",   0x4E58, 0, SZ_U,               0,   UP,            h_reg, 1 },
  { "link",   0x4E50, 0, SZ_W|SZ_L|SZ_U,     0,   UP,            h_link, 0 },
  { "nop",    0x4E71, 0, SZ_U,               0,   UP,            h_none, 0 },
  { "rts",    0x4E75, 0, SZ_U,               0,   UP,            h_none, 0 },
  { "rte",    0x4E73, 0, SZ_U,               0,   UP,            h_none, 0 },
  { "rtr",    0x4E77, 0, SZ_U,               0,   UP,            h_none, 0 },
  { "reset",  0x4E70, 0, SZ_U,               0,   UP,            h_none, 0 },
  { "trapv",  0x4E76, 0, SZ_U,               0,   UP,            h_none, 0 },
  { "illegal",0x4AFC, 0, SZ_U,               0,   UP,            h_none, 0 },
  { "stop",   0x4E72, 0, SZ_U,               0,   UP,            h_immop, 0 },
  { "rtd",    0x4E74, 0, SZ_U,               0,   C010UP,        h_immop, 0 },
  { "trap",   0x4E40, 0, SZ_U,               0,   UP,            h_immop, 4 },
  { "bkpt",   0x4848, 0, SZ_U,               0,   C010UP,        h_immop, 8 },
  { "chk",    0x4180, 0, SZ_W|SZ_L|SZ_U,     'w', UP,            h_chk, 0 },
  { "mulu",   0, 0, SZ_W|SZ_L|SZ_U,          'w', UP,            h_muldiv, 0 },
  { "muls",   0, 0, SZ_W|SZ_L|SZ_U,          'w', UP,            h_muldiv, 1 },
  { "divu",   0, 0, SZ_W|SZ_L|SZ_U,          'w', UP,            h_muldiv, 2 },
  { "divs",   0, 0, SZ_W|SZ_L|SZ_U,          'w', UP,            h_muldiv, 3 },
  { "divul",  0, 0, SZ_L|SZ_U,               'l', C020CPU32,     h_muldiv, 6 },
  { "divsl",  0, 0, SZ_L|SZ_U,               'l', C020CPU32,     h_muldiv, 7 },
  { "cas",    0x08C0, 0, SZ_BWL|SZ_U,        'w', C020UP,        h_cas, 0 },
  { "cas2",   0x0CFC, 0, SZ_W|SZ_L|SZ_U,     'w', C020UP,        h_cas2, 0 },
  { "cmp2",   0x00C0, 0, SZ_BWL|SZ_U,        'w', C020CPU32,     h_cmp2, 0 },
  { "chk2",   0x00C0, 0, SZ_BWL|SZ_U,        'w', C020CPU32,     h_cmp2, 1 },
  { "pack",   0x8140, 0, SZ_U,               0,   C020UP,        h_pack, 0 },
  { "unpk",   0x8180, 0, SZ_U,               0,   C020UP,        h_pack, 0 },
  { "bftst",  0xE8C0, 0, SZ_U,               0,   C020UP,        h_bf, 0 },
  { "bfextu", 0xE9C0, 0, SZ_U,               0,   C020UP,        h_bf, 1 },
  { "bfchg",  0xEAC0, 0, SZ_U,               0,   C020UP,        h_bf, 0 },
  { "bfexts", 0xEBC0, 0, SZ_U,               0,   C020UP,        h_bf, 1 },
  { "bfclr",  0xECC0, 0, SZ_U,               0,   C020UP,        h_bf, 0 },
  { "bfffo",  0xEDC0, 0, SZ_U,               0,   C020UP,        h_bf, 1 },
  { "bfset",  0xEEC0, 0, SZ_U,               0,   C020UP,        h_bf, 0 },
  { "bfins",  0xEFC0, 0, SZ_U,               0,   C020UP,        h_bf, 2 },
  { "callm",  0x06C0, 0, SZ_U,               0,   C020,          h_callm, 0 },
  { "rtm",    0x06C0, 0, SZ_U,               0,   C020,          h_rtm, 0 },
  { "move16", 0xF600, 0, SZ_U,               0,   C040|C060,     h_move16, 0 },
  { "cinvl",  0xF400, 0, SZ_U,               0,   C040|C060,     h_cache, 1 },
  { "cinvp",  0xF400, 0, SZ_U,               0,   C040|C060,     h_cache, 2 },
  { "cinva",  0xF400, 0, SZ_U,               0,   C040|C060,     h_cache, 3 },
  { "cpushl", 0xF420, 0, SZ_U,               0,   C040|C060,     h_cache, 1 },
  { "cpushp", 0xF420, 0, SZ_U,               0,   C040|C060,     h_cache, 2 },
  { "cpusha", 0xF420, 0, SZ_U,               0,   C040|C060,     h_cache, 3 },
  { "pflushn",0xF500, 0, SZ_U,               0,   C040|C060,     h_an_ind, 0 },
  { "pflushan",0xF510,0, SZ_U,               0,   C040|C060,     h_none, 0 },
  { "plpar",  0xF5C8, 0, SZ_U,               0,   C060,          h_an_ind, 0 },
  { "plpaw",  0xF588, 0, SZ_U,               0,   C060,          h_an_ind, 0 },
  { "lpstop", 0xF800, 0, SZ_U,               0,   C060,          h_lpstop, 0 },
  /* MMU */
  { "pmove",  0xF000, 0, SZ_BWL|SZ_D|SZ_U, 0, C030|CMMU|C040|C060, h_pmove, 0 },
  { "pmovefd",0xF000, 0, SZ_BWL|SZ_D|SZ_U,   0,   C030,          h_pmove, 1 },
  { "pflusha",0xF000, 0, SZ_U,               0,   C030|CMMU|C040|C060, h_pflusha, 0 },
  { "pflush", 0xF000, 0, SZ_U,               0,   C030|CMMU|C040|C060, h_pflush, 0 },
  { "pflushs",0xF000, 0, SZ_U,               0,   CMMU,          h_pflush, 1 },
  { "pflushr",0xF000, 0, SZ_U,               0,   CMMU,          h_pflushr, 0 },
  { "ploadr", 0xF000, 0, SZ_U,               0,   C030|CMMU,     h_pload, 1 },
  { "ploadw", 0xF000, 0, SZ_U,               0,   C030|CMMU,     h_pload, 0 },
  { "ptestr", 0xF000, 0, SZ_U,               0,   C030|CMMU|C040, h_ptest, 1 },
  { "ptestw", 0xF000, 0, SZ_U,               0,   C030|CMMU|C040, h_ptest, 0 },
  { "pvalid", 0xF000, 0, SZ_L|SZ_U,          0,   CMMU,          h_pvalid, 0 },
  { "psave",  0xF100, 0, SZ_U,               0,   CMMU,          h_fsave, 0 },
  { "prestore",0xF140,0, SZ_U,               0,   CMMU,          h_fsave, 1 },
  /* FPU */
  { "fmove",  0xF200, 0, SZ_F|SZ_U,          0,   CFPU,          h_fmove, 0 },
  { "fsmove", 0xF200, 0, SZ_F|SZ_U,          0,   C040|C060,     h_fmove, 0x40 },
  { "fdmove", 0xF200, 0, SZ_F|SZ_U,          0,   C040|C060,     h_fmove, 0x44 },
  { "fmovem", 0xF200, 0, SZ_X|SZ_L|SZ_U,     0,   CFPU,          h_fmovem, 0 },
  { "fmovecr",0xF200, 0, SZ_X|SZ_U,          0,   CFPU,          h_fmovecr, 0 },
  { "fnop",   0xF280, 0, SZ_U,               0,   CFPU,          h_fnop, 0 },
  { "fsave",  0xF100, 1, SZ_U,               0,   CFPU,          h_fsave, 0 },
  { "frestore",0xF140,1, SZ_U,               0,   CFPU,          h_fsave, 1 },
  { NULL, 0, 0, 0, 0, 0, NULL, 0 }
};

typedef struct fgen { const char *n; int opmode; int kind; unsigned cpus; } fgen;
static const fgen fgen_table[] = {
  { "fint", 0x01, 1, CFPU }, { "fsinh", 0x02, 1, CFPU }, { "fintrz", 0x03, 1, CFPU },
  { "fsqrt", 0x04, 1, CFPU }, { "flognp1", 0x06, 1, CFPU }, { "fetoxm1", 0x08, 1, CFPU },
  { "ftanh", 0x09, 1, CFPU }, { "fatan", 0x0a, 1, CFPU }, { "fasin", 0x0c, 1, CFPU },
  { "fatanh", 0x0d, 1, CFPU }, { "fsin", 0x0e, 1, CFPU }, { "ftan", 0x0f, 1, CFPU },
  { "fetox", 0x10, 1, CFPU }, { "ftwotox", 0x11, 1, CFPU }, { "ftentox", 0x12, 1, CFPU },
  { "flogn", 0x14, 1, CFPU }, { "flog10", 0x15, 1, CFPU }, { "flog2", 0x16, 1, CFPU },
  { "fabs", 0x18, 1, CFPU }, { "fcosh", 0x19, 1, CFPU }, { "fneg", 0x1a, 1, CFPU },
  { "facos", 0x1c, 1, CFPU }, { "fcos", 0x1d, 1, CFPU }, { "fgetexp", 0x1e, 1, CFPU },
  { "fgetman", 0x1f, 1, CFPU }, { "fdiv", 0x20, 0, CFPU }, { "fmod", 0x21, 0, CFPU },
  { "fadd", 0x22, 0, CFPU }, { "fmul", 0x23, 0, CFPU }, { "fsgldiv", 0x24, 0, CFPU },
  { "frem", 0x25, 0, CFPU }, { "fscale", 0x26, 0, CFPU }, { "fsglmul", 0x27, 0, CFPU },
  { "fsub", 0x28, 0, CFPU }, { "fsincos", 0x30, 3, CFPU }, { "fcmp", 0x38, 0, CFPU },
  { "ftst", 0x3a, 2, CFPU },
  { "fssqrt", 0x41, 1, C040|C060 }, { "fdsqrt", 0x45, 1, C040|C060 },
  { "fsabs", 0x58, 1, C040|C060 }, { "fdabs", 0x5c, 1, C040|C060 },
  { "fsneg", 0x5a, 1, C040|C060 }, { "fdneg", 0x5e, 1, C040|C060 },
  { "fsdiv", 0x60, 0, C040|C060 }, { "fddiv", 0x64, 0, C040|C060 },
  { "fsadd", 0x62, 0, C040|C060 }, { "fdadd", 0x66, 0, C040|C060 },
  { "fsmul", 0x63, 0, C040|C060 }, { "fdmul", 0x67, 0, C040|C060 },
  { "fssub", 0x68, 0, C040|C060 }, { "fdsub", 0x6c, 0, C040|C060 },
  { NULL, 0, 0, 0 }
};

typedef struct ccname { const char *n; int cc; } ccname;
static const ccname icc[] = {
  { "t", 0 }, { "f", 1 }, { "hi", 2 }, { "ls", 3 }, { "cc", 4 }, { "hs", 4 },
  { "cs", 5 }, { "lo", 5 }, { "ne", 6 }, { "eq", 7 }, { "vc", 8 }, { "vs", 9 },
  { "pl", 10 }, { "mi", 11 }, { "ge", 12 }, { "lt", 13 }, { "gt", 14 }, { "le", 15 },
  { NULL, 0 }
};
static const ccname fcc[] = {
  { "f", 0 }, { "eq", 1 }, { "ogt", 2 }, { "oge", 3 }, { "olt", 4 }, { "ole", 5 },
  { "ogl", 6 }, { "or", 7 }, { "un", 8 }, { "ueq", 9 }, { "ugt", 10 }, { "uge", 11 },
  { "ult", 12 }, { "ule", 13 }, { "ne", 14 }, { "t", 15 }, { "sf", 16 }, { "seq", 17 },
  { "gt", 18 }, { "ge", 19 }, { "lt", 20 }, { "le", 21 }, { "gl", 22 }, { "gle", 23 },
  { "ngle", 24 }, { "ngl", 25 }, { "nle", 26 }, { "nlt", 27 }, { "nge", 28 }, { "ngt", 29 },
  { "sne", 30 }, { "st", 31 }, { NULL, 0 }
};
static const ccname pcc[] = {
  { "bs", 0 }, { "bc", 1 }, { "ls", 2 }, { "lc", 3 }, { "ss", 4 }, { "sc", 5 },
  { "as", 6 }, { "ac", 7 }, { "ws", 8 }, { "wc", 9 }, { "is", 10 }, { "ic", 11 },
  { "gs", 12 }, { "gc", 13 }, { "cs", 14 }, { "cc", 15 }, { NULL, 0 }
};

/* hash of all mnemonics */
#define MHASH 2048
typedef struct mentry { mnem m; struct mentry *next; } mentry;
static mentry *mtab[MHASH];

static unsigned mhash(const char *s, size_t n)
{
  unsigned h = 5381;
  while (n--) h = h * 33 + tolower((unsigned char)*s++);
  return h & (MHASH - 1);
}

static void add_mnem(const mnem *m)
{
  mentry *e = xcalloc(1, sizeof(*e));
  unsigned h = mhash(m->name, strlen(m->name));
  e->m = *m;
  e->next = mtab[h];
  mtab[h] = e;
}

static void add_gen(const char *name, uint16_t opc, uint16_t opc2, uint8_t sizes, char def,
                    unsigned cpus, handler h, int arg)
{
  mnem m;
  m.name = xstrdup(name);
  m.opc = opc; m.opc2 = opc2; m.sizes = sizes; m.defsize = def;
  m.cpus = cpus; m.h = h; m.arg = arg;
  add_mnem(&m);
}

static const mnem *find_mnem(const char *s, size_t n)
{
  mentry *e;
  for (e = mtab[mhash(s, n)]; e; e = e->next)
    if (strlen(e->m.name) == n && !strnicmp_(e->m.name, s, n)) return &e->m;
  return NULL;
}

static void build_table(void)
{
  int i;
  char buf[32];
  for (i = 0; base_table[i].name; i++) add_mnem(&base_table[i]);
  for (i = 0; icc[i].n; i++) {
    if (icc[i].cc >= 2) {
      snprintf(buf, sizeof(buf), "b%s", icc[i].n);
      add_gen(buf, 0, 0, SZ_B|SZ_S|SZ_W|SZ_L|SZ_U, 0, UP, h_bcc, icc[i].cc);
    }
    snprintf(buf, sizeof(buf), "db%s", icc[i].n);
    add_gen(buf, 0, 0, SZ_W|SZ_U, 0, UP, h_dbcc, icc[i].cc);
    snprintf(buf, sizeof(buf), "s%s", icc[i].n);
    add_gen(buf, 0, 0, SZ_B|SZ_U, 'b', UP, h_scc, icc[i].cc);
    snprintf(buf, sizeof(buf), "trap%s", icc[i].n);
    add_gen(buf, 0, 0, SZ_W|SZ_L|SZ_U, 0, C020CPU32, h_trapcc, icc[i].cc);
  }
  for (i = 0; fgen_table[i].n; i++)
    add_gen(fgen_table[i].n, 0xF200, fgen_table[i].kind, SZ_F|SZ_U, 0, fgen_table[i].cpus | CFPU,
            h_fgen, fgen_table[i].opmode);
  for (i = 0; fcc[i].n; i++) {
    snprintf(buf, sizeof(buf), "fb%s", fcc[i].n);
    add_gen(buf, 0xF080, 1, SZ_W|SZ_L|SZ_U, 0, CFPU, h_fbcc, fcc[i].cc);
    snprintf(buf, sizeof(buf), "fdb%s", fcc[i].n);
    add_gen(buf, 0xF048, 1, SZ_W|SZ_U, 0, CFPU, h_fdbcc, fcc[i].cc);
    snprintf(buf, sizeof(buf), "fs%s", fcc[i].n);
    add_gen(buf, 0xF040, 1, SZ_B|SZ_U, 0, CFPU, h_fscc, fcc[i].cc);
    snprintf(buf, sizeof(buf), "ftrap%s", fcc[i].n);
    add_gen(buf, 0xF078, 1, SZ_W|SZ_L|SZ_U, 0, CFPU, h_ftrapcc, fcc[i].cc);
  }
  add_gen("fbra", 0xF080, 1, SZ_W|SZ_L|SZ_U, 0, CFPU, h_fbcc, 15);
  add_gen("fdbra", 0xF048, 1, SZ_W|SZ_U, 0, CFPU, h_fdbcc, 0);
  for (i = 0; pcc[i].n; i++) {
    snprintf(buf, sizeof(buf), "pb%s", pcc[i].n);
    add_gen(buf, 0xF080, 0, SZ_W|SZ_L|SZ_U, 0, CMMU, h_fbcc, pcc[i].cc);
    snprintf(buf, sizeof(buf), "pdb%s", pcc[i].n);
    add_gen(buf, 0xF048, 0, SZ_W|SZ_U, 0, CMMU, h_fdbcc, pcc[i].cc);
    snprintf(buf, sizeof(buf), "ps%s", pcc[i].n);
    add_gen(buf, 0xF040, 0, SZ_B|SZ_U, 0, CMMU, h_fscc, pcc[i].cc);
    snprintf(buf, sizeof(buf), "ptrap%s", pcc[i].n);
    add_gen(buf, 0xF078, 0, SZ_W|SZ_L|SZ_U, 0, CMMU, h_ftrapcc, pcc[i].cc);
  }
}

/* ------------------------------------------------------------- module glue */
static int size_bit(int c)
{
  switch (tolower(c)) {
  case 'b': return SZ_B;
  case 'w': return SZ_W;
  case 'l': return SZ_L;
  case 's': return SZ_S;
  case 'd': return SZ_D;
  case 'x': return SZ_X;
  case 'p': return SZ_P;
  }
  return 0;
}

static unsigned effective_cpu(void)
{
  unsigned c = cpu_type;
  if (fpu_enabled) c |= CFPU;
  if (mmu_851) c |= CMMU;
  return c;
}

static int m68k_instruction(const char *mn, size_t ml, const char *ext,
                            char **ops, int nops, int col)
{
  const mnem *m = find_mnem(mn, ml);
  insn *ip;
  atom *a;
  int i;
  (void)col;
  if (!m) {
    /* jbra/jbsr/jbcc and gas-style aliases */
    if (ml > 2 && tolower((unsigned char)mn[0]) == 'j' && tolower((unsigned char)mn[1]) == 'b') {
      m = find_mnem(mn + 1, ml - 1);
      if (!m || m->h != h_bcc) m = NULL;
    }
    if (!m) return 0;
  }
  ip = xcalloc(1, sizeof(*ip));
  ip->m = m;
  ip->cpu = effective_cpu();
  ip->fpuid = fpu_id;
  ip->noopt = no_opt;
  ip->sdreg = sdreg;
  ip->near = near_mode;
  for (i = 0; i < 8; i++) ip->basereg[i] = basereg_expr[i];
  if (ext) {
    int b;
    if (strlen(ext) != 1 || !(b = size_bit(ext[0]))) {
      error("illegal opcode extension");
    } else if (!(m->sizes & b)) {
      if (!(m->sizes & ~SZ_U)) {
        /* unsized instruction: ignore extension */
        if (!opt_guess_ext) warning("extension for unsized instruction ignored");
      } else if (opt_guess_ext && !(m->sizes & (m->sizes - 1) & ~SZ_U)) {
        ip->size = 0;
      } else
        error("illegal opcode extension");
    } else
      ip->size = tolower((unsigned char)ext[0]);
  }
  if (nops > MAXOPS) { error("too many operands"); nops = MAXOPS; }
  ip->nops = nops;
  for (i = 0; i < nops; i++) {
    if (!parse_operand(ops[i], &ip->op[i])) {
      error("syntax error in operand %d: \"%s\"", i + 1, ops[i]);
      return 1;
    }
  }
  if (!(m->cpus & ip->cpu) && !(m->cpus & CFPU && ip->cpu & CFPU)) {
    error("instruction not supported on selected architecture");
  }
  a = add_atom(AT_INSN);
  a->u.insn.insn = ip;
  ip->self = a;
  return 1;
}

/* is the expression a label placed directly behind this instruction? */
static int targets_next(ictx *c, expr *e)
{
  atom *a;
  if (!e || e->op != X_SYM || e->sym->type != S_LABEL || !c->ip->self) return 0;
  for (a = c->ip->self->next; a && a->type == AT_LABEL; a = a->next)
    if (a->u.label.sym == e->sym) return 1;
  return 0;
}

static ival m68k_size(atom *a, section *s, ival pc)
{
  ictx c;
  insn *ip = a->u.insn.insn;
  memset(&c, 0, sizeof(c));
  c.ip = ip;
  c.sec = s;
  c.pc = pc;
  c.enc = 0;
  ip->m->h(&c);
  ip->lastsize = c.len;
  return c.len;
}

static void m68k_encode(atom *a, section *s, ival pc, bbuf *out)
{
  ictx c;
  insn *ip = a->u.insn.insn;
  memset(&c, 0, sizeof(c));
  c.ip = ip;
  c.sec = s;
  c.pc = pc;
  c.enc = 1;
  c.out = out;
  ip->m->h(&c);
}

/* ------------------------------------------------------------- options */
static int set_cpu(const char *n)
{
  if (!stricmp_(n, "68000") || !stricmp_(n, "68008")) cpu_type = C000;
  else if (!stricmp_(n, "68010")) cpu_type = C010;
  else if (!stricmp_(n, "68020")) cpu_type = C020;
  else if (!stricmp_(n, "68030")) cpu_type = C030;
  else if (!stricmp_(n, "68040")) { cpu_type = C040; fpu_enabled = 1; }
  else if (!stricmp_(n, "68060")) { cpu_type = C060; fpu_enabled = 1; }
  else if (!stricmp_(n, "68020up")) { cpu_type = C020 | C030 | C040 | C060; fpu_enabled = 1; }
  else if (!stricmp_(n, "68080")) { cpu_type = C080 | C040 | C060; fpu_enabled = 1; }
  else if (!stricmp_(n, "cpu32")) cpu_type = CCPU32;
  else if (!stricmp_(n, "68881") || !stricmp_(n, "68882")) fpu_enabled = fpu_explicit = 1;
  else if (!stricmp_(n, "68851")) mmu_851 = 1;
  else return 0;
  return 1;
}

static int m68k_option(const char *o)
{
  if (o[0] != '-') return 0;
  o++;
  if (o[0] == 'm' && set_cpu(o + 1)) return 1;
  if (!strcmp(o, "no-fpu")) { fpu_enabled = 0; return 1; }
  if (!strcmp(o, "no-opt")) { no_opt = 1; return 1; }
  if (!strcmp(o, "opt-allbra")) { opt_allbra = 1; return 1; }
  if (!strcmp(o, "opt-brajmp")) { opt_brajmp = 1; return 1; }
  if (!strcmp(o, "opt-clr")) { opt_clr = 1; return 1; }
  if (!strcmp(o, "opt-div")) { opt_div = 1; return 1; }
  if (!strcmp(o, "opt-fconst")) { opt_fconst = 1; return 1; }
  if (!strcmp(o, "opt-jbra")) { opt_jbra = 1; return 1; }
  if (!strcmp(o, "opt-lsl")) { opt_lsl = 1; return 1; }
  if (!strcmp(o, "opt-movem")) { opt_movem = 1; return 1; }
  if (!strcmp(o, "opt-mul")) { opt_mul = 1; return 1; }
  if (!strcmp(o, "opt-nmoveq")) { opt_nmoveq = 1; return 1; }
  if (!strcmp(o, "opt-pea")) { opt_pea = 1; return 1; }
  if (!strcmp(o, "opt-speed")) { opt_speed = 1; return 1; }
  if (!strcmp(o, "opt-size")) { opt_size = 1; return 1; }
  if (!strcmp(o, "opt-st")) { opt_st = 1; return 1; }
  if (!strcmp(o, "sc")) { opt_sc = 1; return 1; }
  if (!strcmp(o, "sd")) { opt_sd = 1; return 1; }
  if (!strcmp(o, "showopt")) { opt_showopt = 1; return 1; }
  if (!strcmp(o, "showcrit")) { opt_showcrit = 1; return 1; }
  if (!strcmp(o, "nodpc")) { opt_nodpc = 1; return 1; }
  if (!strcmp(o, "kick1hunks")) { opt_kick1 = 1; return 0; /* also for output */ }
  if (!strcmp(o, "guess-ext")) { opt_guess_ext = 1; return 1; }
  if (!strcmp(o, "elfregs")) { opt_elfregs = 1; return 1; }
  if (!strcmp(o, "extsd")) { opt_extsd = 1; return 1; }
  if (!strcmp(o, "gas")) { opt_gas = 1; if (cpu_type == C000) cpu_type = C020; fpu_enabled = 1; opt_jbra = 1; return 1; }
  if (!strcmp(o, "sgs")) { opt_sgs = 1; return 1; }
  if (!strcmp(o, "regsymredef")) { opt_regsymredef = 1; return 1; }
  if (!strcmp(o, "rangewarnings")) { opt_rangewarnings = 1; return 1; }
  if (!strcmp(o, "conv-brackets")) { opt_conv_brackets = 1; return 1; }
  if (!strncmp(o, "sdreg=", 6)) { sdreg = atoi(o + 6); return 1; }
  if (!strcmp(o, "devpac")) {
    opt_dpac = 1; opt_guess_ext = 1; opt_nodpc = 1; no_opt = 0; opt_pcrel = 0; opt_og = 0;
    expr_unsigned_shift = 1;
    return 0;  /* also seen by syntax module */
  }
  if (!strcmp(o, "phxass")) { opt_phx = 1; opt_allbra = 1; opt_sd = 1; opt_guess_ext = 1; return 0; }
  return 0;
}

/* devpac style "opt" directive */
static void do_opt(const char *args)
{
  char **v = NULL;
  int n = 0, i;
  const char *p = args;
  while (*p) {
    const char *s = p;
    while (*p && *p != ',') p++;
    v = xrealloc(v, (n + 1) * sizeof(char *));
    v[n++] = xstrndup(s, p - s);
    if (*p == ',') p++;
  }
  for (i = 0; i < n; i++) {
    char *o = v[i];
    size_t l = strlen(o);
    int on = 1;
    while (l && isspace((unsigned char)o[l - 1])) o[--l] = 0;
    if (l && (o[l - 1] == '+' || o[l - 1] == '-')) { on = o[l - 1] == '+'; o[--l] = 0; }
    if (!stricmp_(o, "a") || !stricmp_(o, "autopc")) opt_pcrel = on;
    else if (!stricmp_(o, "noautopc")) opt_pcrel = 0;
    else if (!stricmp_(o, "o")) { no_opt = !on; if (!opt_dpac) opt_pcrel = on; }
    else if (!stricmp_(o, "ob")) opt_jbra = on;
    else if (!stricmp_(o, "oc")) opt_clr = on;
    else if (!stricmp_(o, "od")) opt_div = on;
    else if (!stricmp_(o, "of")) opt_fconst = on;
    else if (!stricmp_(o, "og")) opt_og = on;
    else if (!stricmp_(o, "oj")) opt_brajmp = on;
    else if (!stricmp_(o, "ol")) opt_lsl = on;
    else if (!stricmp_(o, "om")) opt_movem = on;
    else if (!stricmp_(o, "op")) opt_pea = on;
    else if (!stricmp_(o, "oq")) opt_nmoveq = on;
    else if (!stricmp_(o, "os")) opt_speed = on;
    else if (!stricmp_(o, "ot")) opt_st = on;
    else if (!stricmp_(o, "ox")) opt_mul = on;
    else if (!stricmp_(o, "oz")) opt_size = on;
    else if (!strnicmp_(o, "p=", 2)) {
      char *q = o + 2;
      while (*q) {
        char *e = strchr(q, '/');
        if (e) *e = 0;
        if (!set_cpu(q)) error("unknown cpu type");
        if (!e) break;
        q = e + 1;
      }
    }
    /* others (c, d, l, s, t, w, x, ow, o1..o12, debug, ...) are accepted */
  }
  for (i = 0; i < n; i++) free(v[i]);
  free(v);
}

static int m68k_directive(const char *name, const char *args)
{
  const char *p = skip_ws(args);
  if (!stricmp_(name, "machine")) {
    char *a = xstrdup(p);
    char *q = a;
    while (*q && !isspace((unsigned char)*q)) q++;
    *q = 0;
    if (!strnicmp_(a, "mc", 2)) memmove(a, a + 2, strlen(a) - 1);
    if (!set_cpu(a)) error("unknown cpu type");
    free(a);
    return 1;
  }
  if (!stricmp_(name, "mc68000")) { set_cpu("68000"); return 1; }
  if (!stricmp_(name, "mc68010")) { set_cpu("68010"); return 1; }
  if (!stricmp_(name, "mc68020")) { set_cpu("68020"); return 1; }
  if (!stricmp_(name, "mc68030")) { set_cpu("68030"); return 1; }
  if (!stricmp_(name, "mc68040")) { set_cpu("68040"); return 1; }
  if (!stricmp_(name, "mc68060")) { set_cpu("68060"); return 1; }
  if (!stricmp_(name, "ac68080")) { set_cpu("68080"); return 1; }
  if (!stricmp_(name, "cpu32")) { set_cpu("cpu32"); return 1; }
  if (!stricmp_(name, "mc68881")) { set_cpu("68881"); return 1; }
  if (!stricmp_(name, "mc68882")) { set_cpu("68882"); return 1; }
  if (!stricmp_(name, "mc68851")) { set_cpu("68851"); return 1; }
  if (!stricmp_(name, "fpu")) {
    ival v = 1;
    expr *e = *p ? parse_expr(&p) : NULL;
    if (e) eval_const(e, &v);
    if (v) { fpu_enabled = 1; fpu_id = (int)v; } else fpu_enabled = 0;
    return 1;
  }
  if (!stricmp_(name, "opt")) { do_opt(p); return 1; }
  if (!stricmp_(name, "near")) {
    int r;
    if (!*p) { near_mode = 1; return 1; }
    if (!strnicmp_(p, "code", 4)) { opt_sc = 1; return 1; }
    r = m68k_parse_reg(&p);
    if (r >= 8) { sdreg = r - 8; near_mode = 1; }
    else error("not a valid small data register");
    return 1;
  }
  if (!stricmp_(name, "far")) { near_mode = 0; opt_sc = 0; return 1; }
  if (!stricmp_(name, "basereg")) {
    /* basereg expr,An */
    const char *q = p;
    expr *e = parse_expr(&q);
    int r;
    q = skip_ws(q);
    if (!e || *q != ',') { error("syntax error"); return 1; }
    q++;
    r = m68k_parse_reg(&q);
    if (r < 8 || r > 14) { error("address register required"); return 1; }
    if (basereg_expr[r - 8]) error("basereg A%d already in use", r - 8);
    basereg_expr[r - 8] = e;
    return 1;
  }
  if (!stricmp_(name, "endb")) {
    int r = m68k_parse_reg(&p);
    if (r < 8 || r > 15) { error("address register required"); return 1; }
    if (!basereg_expr[r - 8]) error("basereg A%d is already free", r - 8);
    basereg_expr[r - 8] = NULL;
    return 1;
  }
  if (!stricmp_(name, "initnear")) {
    /* lea _LinkerDB,a4 */
    symbol *s = sym_get("_LinkerDB");
    insn *ip;
    atom *a;
    if (s->type == S_NONE) s->type = S_IMPORT;
    ip = xcalloc(1, sizeof(*ip));
    ip->m = find_mnem("lea", 3);
    ip->cpu = effective_cpu();
    ip->noopt = 1;
    ip->nops = 2;
    op_clear(&ip->op[0]);
    ip->op[0].mode = M_ABS;
    ip->op[0].bd = new_sym(s);
    ip->op[0].bdsz = 4;
    op_clear(&ip->op[1]);
    ip->op[1].mode = M_AN;
    ip->op[1].reg = sdreg;
    a = add_atom(AT_INSN);
    a->u.insn.insn = ip;
    return 1;
  }
  if (!stricmp_(name, ".sdreg")) {
    int r = m68k_parse_reg(&p);
    if (r >= 8) { sdreg = r - 8; near_mode = 1; }
    return 1;
  }
  return 0;
}

static int m68k_init(void)
{
  build_table();
  return 1;
}

static void m68k_define_symbols(void)
{
  /* bit values as vasm sets them: FPU/MMU bits only when the
   * 68881/2 or 68851 was selected explicitly */
  ival v = 0;
  unsigned c = cpu_type;
  if (c & C080) v = 1 << 20;
  else {
    if (c & C000) v |= 1;
    if (c & C010) v |= 2;
    if (c & C020) v |= 4;
    if (c & C030) v |= 8;
    if (c & C040) v |= 16;
    if (c & C060) v |= 32;
    if (c & CCPU32) v |= 256;
  }
  if (fpu_explicit) v |= 64;
  if (mmu_851) v |= 128;
  set_internal(sym_get("__VASM"), v);
  sym_get("__VASM")->flags |= SF_INTERNAL;
}

cpu_module cpu_m68k = {
  "m68k",
  m68k_init,
  m68k_option,
  m68k_instruction,
  m68k_directive,
  m68k_size,
  m68k_encode,
  m68k_parse_reg,
  m68k_define_symbols,
  NULL,
  1, 2, 0, 32, 4, NULL,
  0, 0, NULL, NULL
};
