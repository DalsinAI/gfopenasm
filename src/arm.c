/* gfasm - ARM cpu module (ARMv2..ARMv4T, ARM and THUMB instruction sets).
 * MIT License.
 *
 * Encodings from the public ARM Architecture Reference Manual (ARMv4/v4T,
 * ARM and Thumb instruction sets) and the ARM7TDMI data sheet; ELF
 * relocation numbers from the public "ELF for the ARM Architecture" ABI
 * document.  vasm behaviour (options, directives, optimisations, messages)
 * from the vasm manual's ARM chapter and comparison with vasm's output;
 * see docs/REFERENCES.md. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int arm_big, thumb_mode, opt_adr, opt_ldrpc;
static int arch = 5;            /* 2, 3, 4 (3m), 4t ... ; we only gate a few */
static int arch_m = 1, arch_t = 1;

/* ------------------------------------------------------------ helpers */
static const char *condnames[] = { "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                   "hi", "ls", "ge", "lt", "gt", "le", "al", "nv",
                                   "hs", "lo", NULL };
static int cond_of(const char *s)
{
  int i;
  for (i = 0; condnames[i]; i++)
    if (tolower((unsigned char)s[0]) == condnames[i][0] && tolower((unsigned char)s[1]) == condnames[i][1])
      return i == 16 ? 2 : i == 17 ? 3 : i;
  return -1;
}

static int reg_of(const char *s, size_t n)
{
  static const struct { const char *n; int r; } rn[] = {
    { "a1", 0 }, { "a2", 1 }, { "a3", 2 }, { "a4", 3 }, { "v1", 4 }, { "v2", 5 }, { "v3", 6 },
    { "v4", 7 }, { "v5", 8 }, { "v6", 9 }, { "v7", 10 }, { "v8", 11 }, { "sb", 9 }, { "sl", 10 },
    { "fp", 11 }, { "ip", 12 }, { "sp", 13 }, { "lr", 14 }, { "pc", 15 }, { NULL, 0 } };
  int i;
  if (n >= 2 && n <= 3 && (s[0] == 'r' || s[0] == 'R') && isdigit((unsigned char)s[1])) {
    int v = s[1] - '0';
    if (n == 3) { if (!isdigit((unsigned char)s[2]) || v == 0) return -1; v = v * 10 + s[2] - '0'; }
    return v <= 15 ? v : -1;
  }
  for (i = 0; rn[i].n; i++)
    if (n == 2 && !strnicmp_(s, rn[i].n, 2)) return rn[i].r;
  return -1;
}

/* a whole operand string that is just a register */
static int is_reg(const char *s)
{
  const char *e;
  s = skip_ws(s);
  e = s;
  while (isalnum((unsigned char)*e) || *e == '_') e++;
  if (*skip_ws(e)) return -1;
  return reg_of(s, e - s);
}

/* register at *pp (advances) */
static int get_reg(const char **pp)
{
  const char *s = skip_ws(*pp), *e = s;
  int r;
  while (isalnum((unsigned char)*e) || *e == '_') e++;
  r = reg_of(s, e - s);
  if (r >= 0) *pp = e;
  return r;
}

/* cN / pN */
static int get_cpreg(const char **pp, int c)
{
  const char *s = skip_ws(*pp), *e;
  int v;
  if (tolower((unsigned char)*s) != c || !isdigit((unsigned char)s[1])) return -1;
  e = s + 1;
  v = 0;
  while (isdigit((unsigned char)*e)) v = v * 10 + (*e++ - '0');
  if (isalnum((unsigned char)*e) || *e == '_' || v > 15) return -1;
  *pp = e;
  return v;
}

static expr *pexp(const char **pp)
{
  const char *p = skip_ws(*pp);
  expr *e = parse_expr(&p);
  if (!e) return NULL;
  *pp = p;
  return e;
}

static uint32_t rol32(uint32_t v, int n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

/* 8-bit rotated immediate; returns 12-bit field or -1 */
static int rotimm(uint32_t v)
{
  int r;
  for (r = 0; r < 16; r++) {
    uint32_t x = rol32(v, 2 * r);
    if (x <= 0xff) return (r << 8) | (int)x;
  }
  return -1;
}

/* split into two rotated immediates (rem first, chunk second) */
static int split_imm(uint32_t v, uint32_t *a, uint32_t *b)
{
  int k;
  for (k = 0; k < 16; k++) {
    uint32_t w = rol32(0xff, 32 - 2 * k);    /* 0xff ror 2k */
    uint32_t c = v & w, r = v & ~w;
    if (!c) continue;
    if (rotimm(r) >= 0) { *a = r; *b = c; return 1; }
  }
  return 0;
}

/* ------------------------------------------------------------ table */
enum { T_DP, T_MUL, T_MULL, T_LDR, T_STR, T_LDM, T_STM, T_SWP, T_B, T_BL, T_BX, T_SWI,
       T_MRS, T_MSR, T_CDP, T_LDC, T_STC, T_MCR, T_MRC, T_ADR, T_ADRL, T_NOP };

typedef struct amn { const char *name; int type; int opc; const char *sfx; } amn;
static const amn atab[] = {
  { "and", T_DP, 0, "s" }, { "eor", T_DP, 1, "s" }, { "sub", T_DP, 2, "s" }, { "rsb", T_DP, 3, "s" },
  { "add", T_DP, 4, "s" }, { "adc", T_DP, 5, "s" }, { "sbc", T_DP, 6, "s" }, { "rsc", T_DP, 7, "s" },
  { "tst", T_DP, 8, "s p" }, { "teq", T_DP, 9, "s p" }, { "cmp", T_DP, 10, "s p" }, { "cmn", T_DP, 11, "s p" },
  { "orr", T_DP, 12, "s" }, { "mov", T_DP, 13, "s" }, { "bic", T_DP, 14, "s" }, { "mvn", T_DP, 15, "s" },
  { "mul", T_MUL, 0, "s" }, { "mla", T_MUL, 1, "s" },
  { "umull", T_MULL, 0, "s" }, { "umlal", T_MULL, 1, "s" }, { "smull", T_MULL, 2, "s" }, { "smlal", T_MULL, 3, "s" },
  { "ldr", T_LDR, 1, "b t bt h sh sb" }, { "str", T_STR, 0, "b t bt h" },
  { "ldm", T_LDM, 1, "ia ib da db fd ed fa ea" }, { "stm", T_STM, 0, "ia ib da db fd ed fa ea" },
  { "swp", T_SWP, 0, "b" }, { "b", T_B, 0, "" }, { "bl", T_BL, 0, "" }, { "bx", T_BX, 0, "" },
  { "swi", T_SWI, 0, "" }, { "svc", T_SWI, 0, "" }, { "mrs", T_MRS, 0, "" }, { "msr", T_MSR, 0, "" },
  { "cdp", T_CDP, 0, "" }, { "ldc", T_LDC, 1, "l" }, { "stc", T_STC, 0, "l" },
  { "mcr", T_MCR, 0, "" }, { "mrc", T_MRC, 1, "" }, { "adr", T_ADR, 0, "l" },
  { "nop", T_NOP, 0, "" },
  { NULL, 0, 0, NULL }
};

static const amn adrl_mn = { "adrl", T_ADRL, 0, "" };

static int sfx_ok(const char *list, const char *s, size_t n)
{
  const char *p = list;
  if (!n) return 1;
  while (*p) {
    const char *q = p;
    while (*q && *q != ' ') q++;
    if ((size_t)(q - p) == n && !strnicmp_(p, s, n)) return 1;
    p = *q ? q + 1 : q;
  }
  return 0;
}

/* parse "base[cond][suffix]" the way vasm does */
static const amn *parse_mnem(const char *m, size_t ml, int *cond, char *sfx)
{
  int i;
  const amn *best = NULL;
  size_t bestl = 0;
  for (i = 0; atab[i].name; i++) {
    size_t bl = strlen(atab[i].name);
    const char *r;
    size_t rl;
    if (bl > ml || strnicmp_(m, atab[i].name, bl)) continue;
    if (bl > bestl) { best = &atab[i]; bestl = bl; }
    r = m + bl; rl = ml - bl;
    if (rl >= 2 && cond_of(r) >= 0 && sfx_ok(atab[i].sfx, r + 2, rl - 2)) {
      *cond = cond_of(r);
      memcpy(sfx, r + 2, rl - 2); sfx[rl - 2] = 0;
      return &atab[i];
    }
    if (sfx_ok(atab[i].sfx, r, rl)) {
      *cond = 14;
      memcpy(sfx, r, rl); sfx[rl] = 0;
      return &atab[i];
    }
  }
  if (!best) return NULL;
  if (best->type == T_B || best->type == T_BL || best->type == T_BX) return NULL;
  /* fallback: trailing garbage is ignored */
  *cond = 14;
  sfx[0] = 0;
  if (ml - bestl >= 2 && cond_of(m + bestl) >= 0) *cond = cond_of(m + bestl);
  return best;
}

/* ------------------------------------------------------------ insn */
/* addressing for load/store */
typedef struct amode {
  int kind;        /* 0 none, 1 [Rn...], 2 label, 3 =literal */
  int rn, pre, wb, sign;   /* sign: 1 '+', -1 '-' */
  int hasoff;      /* 0 none, 1 imm, 2 reg */
  expr *off;
  int rm, shtype, shimm_given;
  expr *shamt;
  int explicit_sign;
} amode;

typedef struct op2 { int isimm; expr *imm, *rot; int rm, sht, shreg, rrx; expr *shamt; } op2;

typedef struct ainsn {
  const amn *m;
  int thumb;
  int tid;         /* thumb instruction kind, -1 = literal pool */
  int cond;
  char sfx[8];
  int r[5];
  op2 o2;
  expr *e, *e2, *e3;
  amode am;
  uint32_t rlist;
  int hat, wb, psr, fields, lflag;
  int pool_idx;    /* literal pool entry index */
  struct pool *pool;
  int litmode;     /* 0 pool, 1 mov, 2 mvn, 3 adr (as vasm: Rd is lost) */
  int big;         /* size decision: 2 instructions */
} ainsn;

/* ------------------------------------------------------------ literal pools */
typedef struct pent_ { expr *e; ival v; section *sec; symbol *ext; int isconst; } pentry;
typedef struct pool {
  section *sec;
  atom *atom;
  pentry *ent; int n, cap;
  struct pool *next;
  int explicit_;
} pool;
static pool *pools;
static pool *curpool_for(section *s);

static pool *new_pool(section *s, int expl)
{
  pool *p = xcalloc(1, sizeof(*p));
  p->sec = s;
  p->explicit_ = expl;
  p->next = pools;
  pools = p;
  return p;
}

/* pending pool per section: the one that will be emitted next */
typedef struct secpool { section *s; pool *p; struct secpool *next; } secpool;
static secpool *secpools;
static pool *curpool_for(section *s)
{
  secpool *sp;
  for (sp = secpools; sp; sp = sp->next) if (sp->s == s) return sp->p;
  sp = xcalloc(1, sizeof(*sp));
  sp->s = s;
  sp->p = new_pool(s, 0);
  sp->next = secpools;
  secpools = sp;
  return sp->p;
}

static int pool_add(pool *p, expr *e)
{
  eres r;
  int i;
  eval_expr(e, &r);
  for (i = 0; i < p->n; i++) {
    pentry *q = &p->ent[i];
    if (q->sec == r.sec && q->ext == r.ext && q->v == r.v && (r.sec || r.ext || q->isconst)) return i;
  }
  if (p->n >= p->cap) { p->cap = p->cap * 2 + 8; p->ent = xrealloc(p->ent, p->cap * sizeof(pentry)); }
  p->ent[p->n].e = e;
  p->ent[p->n].v = r.v;
  p->ent[p->n].sec = r.sec;
  p->ent[p->n].ext = r.ext;
  p->ent[p->n].isconst = !r.sec && !r.ext;
  return p->n++;
}

/* ------------------------------------------------------------ errors */
#define ILLOPS() do { error("illegal operand types"); return 1; } while (0)

static int arm26(void) { return arch <= 3; }

/* "#expr" or "expr" (hash optional) */
static expr *imm_op(const char *s, int need_hash, int *neglit)
{
  const char *p = skip_ws(s);
  expr *e;
  if (*p == '#') p++;
  else if (need_hash) return NULL;
  p = skip_ws(p);
  if (neglit) *neglit = *p == '-';
  e = pexp(&p);
  if (!e || *skip_ws(p)) return NULL;
  return e;
}

/* shift specifier "lsl #n", "lsl rs", "rrx" */
static int parse_shift(const char *s, op2 *o)
{
  static const char *sn[] = { "lsl", "lsr", "asr", "ror", "asl", NULL };
  const char *p = skip_ws(s);
  int i;
  if (!strnicmp_(p, "rrx", 3) && !*skip_ws(p + 3)) { o->rrx = 1; o->sht = 3; return 1; }
  for (i = 0; sn[i]; i++)
    if (!strnicmp_(p, sn[i], 3)) break;
  if (!sn[i]) return 0;
  o->sht = i == 4 ? 0 : i;
  p = skip_ws(p + 3);
  if (*p == '#') {
    p++;
    if (is_ws(*p)) { error("number or identifier expected"); return 0; }
    o->shamt = pexp(&p);
    if (!o->shamt || *skip_ws(p)) return 0;
    return 1;
  }
  o->shreg = is_reg(p);
  if (o->shreg < 0) return 0;
  o->shreg |= 0x100;
  return 1;
}

/* operand 2 from ops[k..]; returns 1 on success */
static int parse_op2(char **ops, int nops, int k, op2 *o)
{
  memset(o, 0, sizeof(*o));
  if (k >= nops || nops > k + 2) return 0;
  if (*skip_ws(ops[k]) == '#') {
    o->isimm = 1;
    o->imm = imm_op(ops[k], 1, NULL);
    if (!o->imm) return 0;
    if (k + 1 < nops) { o->rot = imm_op(ops[k + 1], 0, NULL); if (!o->rot) return 0; }
    return 1;
  }
  o->rm = is_reg(ops[k]);
  if (o->rm < 0) return 0;
  if (k + 1 < nops) return parse_shift(ops[k + 1], o);
  return 1;
}

/* register list "{r0-r3,lr}" with optional "^" */
static int parse_rlist(const char *s, uint32_t *list, int *hat)
{
  const char *p = skip_ws(s);
  uint32_t l = 0;
  *hat = 0;
  if (*p != '{') return 0;
  p++;
  for (;;) {
    int a, b;
    p = skip_ws(p);
    if (*p == '}') break;
    a = get_reg(&p);
    if (a < 0) return 0;
    b = a;
    p = skip_ws(p);
    if (*p == '-') { p++; b = get_reg(&p); if (b < 0) return 0; }
    if (a > b) { int t = a; a = b; b = t; }
    for (; a <= b; a++) l |= 1u << a;
    p = skip_ws(p);
    if (*p == ',') { p++; continue; }
    if (*p != '}') return 0;
  }
  p = skip_ws(p + 1);
  if (*p == '^') { *hat = 1; p = skip_ws(p + 1); }
  if (*p) return 0;
  *list = l;
  return 1;
}

/* load/store addressing; returns number of operands used, 0 on error */
static int parse_amode(char **ops, int nops, int k, amode *a, int allow_shift)
{
  const char *p;
  memset(a, 0, sizeof(*a));
  a->sign = 1;
  if (k >= nops) return 0;
  p = skip_ws(ops[k]);
  if (*p == '=') {
    p++;
    a->kind = 3;
    a->off = pexp(&p);
    return a->off && !*skip_ws(p) && nops == k + 1;
  }
  if (*p != '[') {
    a->kind = 2;
    a->off = pexp(&p);
    return a->off && !*skip_ws(p) && nops == k + 1;
  }
  a->kind = 1;
  p++;
  a->rn = get_reg(&p);
  if (a->rn < 0) return 0;
  p = skip_ws(p);
  if (*p == ']') {
    p = skip_ws(p + 1);
    if (*p == '!') { a->pre = 1; a->wb = 1; p = skip_ws(p + 1); if (*p) return 0; return nops == k + 1; }
    if (*p) return 0;
    if (nops == k + 1) { a->pre = 1; return 1; }
    /* post-indexed */
    p = skip_ws(ops[k + 1]);
    if (*p == '#') {
      p = skip_ws(p + 1);
      if (*p == '-') a->explicit_sign = -1;
      a->hasoff = 1;
      a->off = pexp(&p);
      if (!a->off || *skip_ws(p) || nops != k + 2) return 0;
      return 2;
    }
    if (*p == '-' || *p == '+') { a->sign = *p == '-' ? -1 : 1; p++; }
    a->rm = is_reg(p);
    if (a->rm < 0) return 0;
    a->hasoff = 2;
    if (nops == k + 3) {
      op2 o;
      memset(&o, 0, sizeof(o));
      if (!allow_shift) { error("signed/halfword ldr/str doesn't support shifts"); return 0; }
      if (!parse_shift(ops[k + 2], &o) || o.shreg) return 0;
      a->shtype = o.rrx ? 3 : o.sht; a->shamt = o.shamt; a->shimm_given = 1;
      if (o.rrx) a->shamt = NULL;
      return 3;
    }
    return nops == k + 2 ? 2 : 0;
  }
  if (*p != ',') return 0;
  p = skip_ws(p + 1);
  a->pre = 1;
  {
    /* find closing bracket of this operand */
    const char *cl = strrchr(p, ']');
    char *in, *sh = NULL;
    if (!cl) return 0;
    in = xstrndup(p, cl - p);
    p = skip_ws(cl + 1);
    if (*p == '!') { a->wb = 1; p = skip_ws(p + 1); }
    if (*p || nops != k + 1) { free(in); return 0; }
    {
      char *c = strchr(in, ',');
      if (c) { *c = 0; sh = c + 1; }
    }
    p = skip_ws(in);
    if (*p == '#') {
      if (sh) { free(in); return 0; }
      p = skip_ws(p + 1);
      if (*p == '-') a->explicit_sign = -1;
      a->hasoff = 1;
      a->off = pexp(&p);
      if (!a->off || *skip_ws(p)) { free(in); return 0; }
      free(in);
      return 1;
    }
    if (*p == '-' || *p == '+') { a->sign = *p == '-' ? -1 : 1; p++; }
    a->rm = is_reg(p);
    if (a->rm < 0) { free(in); return 0; }
    a->hasoff = 2;
    if (sh) {
      op2 o;
      memset(&o, 0, sizeof(o));
      if (!allow_shift) { free(in); error("signed/halfword ldr/str doesn't support shifts"); return 0; }
      if (!parse_shift(sh, &o) || o.shreg) { free(in); return 0; }
      a->shtype = o.rrx ? 3 : o.sht; a->shamt = o.rrx ? NULL : o.shamt; a->shimm_given = 1;
    }
    free(in);
    return 1;
  }
}

static int psr_of(const char *s, int *spsr, int *fields, int need_fields)
{
  const char *p = skip_ws(s);
  int f = 0;
  if (!strnicmp_(p, "cpsr", 4)) *spsr = 0;
  else if (!strnicmp_(p, "spsr", 4)) *spsr = 1;
  else return 0;
  p += 4;
  if (*p == '_') {
    if (!need_fields) return 0;
    p++;
    if (!*p || is_ws(*p)) return 0;
    while (*p && !is_ws(*p)) {
      int c = tolower((unsigned char)*p++);
      if (c == 'c') f |= 1; else if (c == 'x') f |= 2; else if (c == 's') f |= 4; else if (c == 'f') f |= 8;
      else return 0;
    }
  } else f = 15;
  if (*skip_ws(p)) return 0;
  *fields = f;
  return 1;
}

/* ------------------------------------------------------------ ARM create */
static ainsn *new_insn(void)
{
  ainsn *ip = xcalloc(1, sizeof(*ip));
  ip->pool_idx = -1;
  ip->thumb = thumb_mode;
  return ip;
}

static void emit_insn(ainsn *ip)
{
  atom *a = add_atom(AT_INSN);
  a->u.insn.insn = ip;
}

static int need_regs(char **ops, int nops, int first, int n, int *r)
{
  int i;
  if (nops < first + n) return 0;
  for (i = 0; i < n; i++) {
    r[i] = is_reg(ops[first + i]);
    if (r[i] < 0) return 0;
  }
  return 1;
}

static int arm_create(const amn *m, int cond, const char *sfx, char **ops, int nops)
{
  ainsn *ip = new_insn();
  int i;
  ip->m = m;
  ip->cond = cond;
  for (i = 0; sfx[i] && i < (int)sizeof ip->sfx - 1; i++) ip->sfx[i] = sfx[i];
  ip->sfx[i] = 0;
  for (i = 0; ip->sfx[i]; i++) ip->sfx[i] = (char)tolower((unsigned char)ip->sfx[i]);
  switch (m->type) {
  case T_DP: {
    int k;
    if (m->opc == 13 || m->opc == 15) { if (!need_regs(ops, nops, 0, 1, &ip->r[0])) ILLOPS(); k = 1; }
    else if (m->opc >= 8 && m->opc <= 11) { if (!need_regs(ops, nops, 0, 1, &ip->r[1])) ILLOPS(); k = 1; }
    else { if (!need_regs(ops, nops, 0, 2, &ip->r[0])) ILLOPS(); k = 2; }
    if (!parse_op2(ops, nops, k, &ip->o2)) ILLOPS();
    if (!strcmp(ip->sfx, "p") && !arm26())
      warning("TSTP/TEQP/CMNP/CMPP deprecated on 32-bit architectures");
    break;
  }
  case T_MUL:
    if (nops != (m->opc ? 4 : 3) || !need_regs(ops, nops, 0, nops, ip->r)) ILLOPS();
    if (ip->r[0] == ip->r[1]) { error("register r%d was used multiple times", ip->r[0]); return 1; }
    break;
  case T_MULL:
    if (nops != 4 || !need_regs(ops, nops, 0, 4, ip->r)) ILLOPS();
    if (ip->r[0] == ip->r[1] || ip->r[0] == ip->r[2] || ip->r[1] == ip->r[2]) {
      error("register r%d was used multiple times", ip->r[0]); return 1;
    }
    break;
  case T_LDR: case T_STR: {
    int half = !strcmp(ip->sfx, "h") || !strcmp(ip->sfx, "sh") || !strcmp(ip->sfx, "sb");
    int n;
    if (nops < 2 || (ip->r[0] = is_reg(ops[0])) < 0) ILLOPS();
    n = parse_amode(ops, nops, 1, &ip->am, !half);
    if (!n) { if (!errcount) ILLOPS(); return 1; }
    if (ip->am.kind == 3) {
      if (m->type == T_STR) ILLOPS();
      ip->pool = curpool_for(default_section());
    }
    if (strchr(ip->sfx, 't') && ip->am.kind == 1 && ip->am.pre && (ip->am.hasoff || ip->am.wb)) {
      error("post-indexed addressing mode exptected"); return 1;
    }
    if (strchr(ip->sfx, 't') && ip->am.kind == 3) ILLOPS();
    break;
  }
  case T_LDM: case T_STM: {
    const char *p;
    if (!ip->sfx[0]) { error("illegal addressing mode: <none>"); return 1; }
    if (nops != 2) ILLOPS();
    p = ops[0];
    ip->r[0] = get_reg(&p);
    if (ip->r[0] < 0) ILLOPS();
    p = skip_ws(p);
    if (*p == '!') { ip->wb = 1; p = skip_ws(p + 1); }
    if (*p) ILLOPS();
    if (!parse_rlist(ops[1], &ip->rlist, &ip->hat)) ILLOPS();
    break;
  }
  case T_SWP: {
    const char *p;
    if (nops != 3 || !need_regs(ops, nops, 0, 2, ip->r)) ILLOPS();
    p = skip_ws(ops[2]);
    if (*p != '[') ILLOPS();
    p++;
    ip->r[2] = get_reg(&p);
    p = skip_ws(p);
    if (ip->r[2] < 0 || *p != ']' || *skip_ws(p + 1)) ILLOPS();
    break;
  }
  case T_B: case T_BL:
    if (nops != 1) ILLOPS();
    { const char *p = skip_ws(ops[0]); ip->e = pexp(&p); if (!ip->e || *skip_ws(p)) ILLOPS(); }
    break;
  case T_BX:
    if (nops != 1 || (ip->r[0] = is_reg(ops[0])) < 0) ILLOPS();
    break;
  case T_SWI:
    if (nops != 1 || !(ip->e = imm_op(ops[0], 0, NULL))) ILLOPS();
    break;
  case T_MRS:
    if (nops != 2 || (ip->r[0] = is_reg(ops[0])) < 0 || !psr_of(ops[1], &ip->psr, &ip->fields, 0)) ILLOPS();
    break;
  case T_MSR:
    if (nops != 2 || !psr_of(ops[0], &ip->psr, &ip->fields, 1)) ILLOPS();
    if (*skip_ws(ops[1]) == '#') { if (!(ip->e = imm_op(ops[1], 1, NULL))) ILLOPS(); }
    else if ((ip->r[0] = is_reg(ops[1])) < 0) ILLOPS();
    break;
  case T_CDP: case T_MCR: case T_MRC: {
    const char *p;
    if (nops != 5 && nops != 6) ILLOPS();
    p = ops[0]; ip->r[0] = get_cpreg(&p, 'p'); if (ip->r[0] < 0 || *skip_ws(p)) ILLOPS();
    if (!(ip->e = imm_op(ops[1], 0, NULL))) ILLOPS();
    if (m->type == T_CDP) { p = ops[2]; ip->r[1] = get_cpreg(&p, 'c'); if (ip->r[1] < 0 || *skip_ws(p)) ILLOPS(); }
    else if ((ip->r[1] = is_reg(ops[2])) < 0) ILLOPS();
    p = ops[3]; ip->r[2] = get_cpreg(&p, 'c'); if (ip->r[2] < 0 || *skip_ws(p)) ILLOPS();
    p = ops[4]; ip->r[3] = get_cpreg(&p, 'c'); if (ip->r[3] < 0 || *skip_ws(p)) ILLOPS();
    if (nops == 6 && !(ip->e2 = imm_op(ops[5], 0, NULL))) ILLOPS();
    break;
  }
  case T_LDC: case T_STC: {
    const char *p;
    if (nops < 3) ILLOPS();
    p = ops[0]; ip->r[0] = get_cpreg(&p, 'p'); if (ip->r[0] < 0 || *skip_ws(p)) ILLOPS();
    p = ops[1]; ip->r[1] = get_cpreg(&p, 'c'); if (ip->r[1] < 0 || *skip_ws(p)) ILLOPS();
    if (!parse_amode(ops, nops, 2, &ip->am, 0) || ip->am.kind != 1 || ip->am.hasoff == 2) ILLOPS();
    break;
  }
  case T_ADR: case T_ADRL:
    if (nops != 2 || (ip->r[0] = is_reg(ops[0])) < 0) ILLOPS();
    { const char *p = skip_ws(ops[1]); ip->e = pexp(&p); if (!ip->e || *skip_ws(p)) ILLOPS(); }
    if (m->type == T_ADR && ip->sfx[0] == 'l') ip->m = &adrl_mn;
    break;
  case T_NOP:
    if (nops) ILLOPS();
    break;
  }
  emit_insn(ip);
  return 1;
}

/* ------------------------------------------------------------ evaluation */
static int ev(expr *e, eres *r)
{
  eval_expr(e, r);
  if (r->isflt) { r->v = f_to_int(r->f); r->isflt = 0; }
  return r->defined;
}

/* distance from (pc + adj) to the target, when it is known in this section */
static int pcrel(eres *r, section *s, ival pc, ival adj, ival *d)
{
  if (r->ext) return 0;
  if (r->sec == s) { *d = r->v - (pc + adj); return 1; }
  if (!r->sec && (s->flags & SEC_ABS)) { *d = r->v - (s->org + pc + adj); return 1; }
  if (!r->sec) { *d = r->v - (pc + adj); return 1; }
  return 0;
}

static ival cval(expr *e, int enc)
{
  eres r;
  ev(e, &r);
  if (enc && (r.sec || r.ext)) error("constant integer expression required");
  return r.v;
}

/* data-processing immediate, possibly with the complementary opcode */
static int dp_imm(int opc, uint32_t v, int *nopc, int *f)
{
  int alt = -1;
  uint32_t v2 = 0;
  if ((*f = rotimm(v)) >= 0) { *nopc = opc; return 1; }
  switch (opc) {
  case 13: alt = 15; v2 = ~v; break;
  case 15: alt = 13; v2 = ~v; break;
  case 0: alt = 14; v2 = ~v; break;
  case 14: alt = 0; v2 = ~v; break;
  case 4: alt = 2; v2 = -v; break;
  case 2: alt = 4; v2 = -v; break;
  case 10: alt = 11; v2 = -v; break;
  case 11: alt = 10; v2 = -v; break;
  case 5: alt = 6; v2 = ~v; break;
  case 6: alt = 5; v2 = ~v; break;
  }
  if (alt >= 0 && (*f = rotimm(v2)) >= 0) { *nopc = alt; return 1; }
  return 0;
}

static int splittable(int opc) { return opc == 13 || opc == 15 || opc == 4 || opc == 2; }

/* ------------------------------------------------------------ literal decision */
static void lit_decide(ainsn *ip, section *s, ival pc)
{
  eres r;
  ival d;
  int half = ip->sfx[0] == 'h' || ip->sfx[0] == 's';
  ev(ip->am.off, &r);
  ip->litmode = 0;
  if (!r.sec && !r.ext && r.defined) {
    if (rotimm((uint32_t)r.v) >= 0) { ip->litmode = 1; return; }
    if (rotimm(~(uint32_t)r.v) >= 0) { ip->litmode = 2; return; }
  } else if (r.sec == s && !r.ext && !half) {
    d = r.v - (pc + 8);
    if (rotimm((uint32_t)(d < 0 ? -d : d)) >= 0) { ip->litmode = 3; return; }
  }
  ip->pool_idx = pool_add(ip->pool, ip->am.off);
}

static ival arm_size1(ainsn *ip, section *s, ival pc)
{
  eres r;
  switch (ip->m->type) {
  case T_DP:
    if (ip->o2.isimm && !ip->o2.rot) {
      int no, f;
      ev(ip->o2.imm, &r);
      if (!r.sec && !r.ext && !dp_imm(ip->m->opc, (uint32_t)r.v, &no, &f) && splittable(ip->m->opc)) {
        uint32_t a, b;
        if (split_imm((uint32_t)r.v, &a, &b)) return 8;
      }
    }
    return 4;
  case T_ADRL:
    return 8;
  case T_ADR:
    if (opt_adr) {
      ival d;
      ev(ip->e, &r);
      if (pcrel(&r, s, pc, 8, &d) && rotimm((uint32_t)(d < 0 ? -d : d)) < 0) return 8;
    }
    return 4;
  case T_LDR:
    if (ip->am.kind == 3) lit_decide(ip, s, pc);
    return 4;
  }
  return 4;
}

/* ------------------------------------------------------------ ARM encode */
static void put32(bbuf *b, uint32_t w)
{
  if (arm_big) bb_be32(b, w);
  else { bb_byte(b, w); bb_byte(b, w >> 8); bb_byte(b, w >> 16); bb_byte(b, w >> 24); }
}
static void put16(bbuf *b, uint32_t w)
{
  if (arm_big) bb_be16(b, w);
  else { bb_byte(b, w); bb_byte(b, w >> 8); }
}

static uint32_t enc_op2(ainsn *ip, int *opc, int enc)
{
  op2 *o = &ip->o2;
  if (o->isimm) {
    eres r;
    ival v;
    ev(o->imm, &r);
    if (enc && (r.sec || r.ext)) error("constant integer expression required");
    v = r.v;
    if (o->rot) {
      ival rot = cval(o->rot, enc);
      if (enc && (v < 0 || v > 255)) error("8-bit unsigned constant required: %lld", (long long)v);
      else if (enc && (rot < 0 || rot > 30 || (rot & 1))) error("rotate constant must be an even number between 0 and 30: %lld", (long long)rot);
      return 0x02000000 | (uint32_t)(((rot / 2) & 15) << 8) | (uint32_t)(v & 255);
    } else {
      int f, no;
      if (!dp_imm(*opc, (uint32_t)v, &no, &f)) {
        if (enc) error("constant (0x%llx) not suitable for 8-bit rotated immediate", (unsigned long long)(uint32_t)v);
        return 0x02000000;
      }
      *opc = no;
      return 0x02000000 | (uint32_t)f;
    }
  }
  if (o->shreg) return (uint32_t)(((o->shreg & 15) << 8) | (o->sht << 5) | 0x10 | o->rm);
  if (o->rrx) return 0x60 | o->rm;
  if (o->shamt) {
    ival n = cval(o->shamt, enc);
    ival hi = (o->sht == 1 || o->sht == 2) ? 32 : 31;
    if (enc && (n < 0 || n > hi)) error("illegal immediate shift count (%lld)", (long long)n);
    return (uint32_t)(((n & 31) << 7) | (o->sht << 5) | o->rm);
  }
  return (uint32_t)o->rm;
}

static uint32_t dp_word(int cond, int opc, int s, int rn, int rd, uint32_t op2)
{
  return ((uint32_t)cond << 28) | ((uint32_t)opc << 21) | ((uint32_t)s << 20) | ((uint32_t)rn << 16) |
         ((uint32_t)rd << 12) | op2;
}

static void enc_dp(ainsn *ip, bbuf *out, ival size)
{
  int opc = ip->m->opc, s = ip->sfx[0] == 's', rn = 0, rd = 0;
  uint32_t o2;
  if (opc >= 8 && opc <= 11) { s = 1; rn = ip->r[1]; rd = ip->sfx[0] == 'p' ? 15 : 0; }
  else if (opc == 13 || opc == 15) rd = ip->r[0];
  else { rd = ip->r[0]; rn = ip->r[1]; }
  if (size == 8) {
    eres r;
    uint32_t a, b;
    int o2opc = opc == 13 ? 12 : opc == 15 ? 1 : opc;
    ev(ip->o2.imm, &r);
    split_imm((uint32_t)r.v, &a, &b);
    put32(out, dp_word(ip->cond, opc, s, rn, rd, 0x02000000 | (uint32_t)rotimm(a)));
    put32(out, dp_word(ip->cond, o2opc, s, rd, rd, 0x02000000 | (uint32_t)rotimm(b)));
    return;
  }
  o2 = enc_op2(ip, &opc, 1);
  put32(out, dp_word(ip->cond, opc, s, rn, rd, o2));
}

/* pc-relative ldr/str offset; adds the U bit */
static uint32_t pc_off(ival d, int bits, int *u)
{
  ival lim = bits == 12 ? 4095 : 255;
  *u = d >= 0;
  if (d < 0) d = -d;
  if (d > lim) error("PC-relative load/store (offset %lld) out of range", (long long)(*u ? d : -d));
  return (uint32_t)(d & lim);
}

static uint32_t half_off(uint32_t v) { return ((v & 0xf0) << 4) | (v & 15); }

static void enc_ldst(ainsn *ip, section *s, ival pc, bbuf *out)
{
  amode *a = &ip->am;
  const char *sf = ip->sfx;
  int L = ip->m->type == T_LDR;
  int half = !strcmp(sf, "h") || !strcmp(sf, "sh") || !strcmp(sf, "sb");
  int B = sf[0] == 'b', T = strchr(sf, 't') != NULL;
  uint32_t w = (uint32_t)ip->cond << 28 | (uint32_t)ip->r[0] << 12 | (uint32_t)L << 20;
  uint32_t sh = !strcmp(sf, "h") ? 0xb0 : !strcmp(sf, "sb") ? 0xd0 : 0xf0;
  int P = 1, U = 1, W = 0, rn = a->rn;
  uint32_t off = 0;
  int I = 0;
  if (a->kind == 3) {
    if (ip->litmode == 1) {
      eres r; ev(a->off, &r);
      put32(out, ((uint32_t)ip->cond << 28) | 0x03a00000 | (uint32_t)ip->r[0] << 12 | (uint32_t)rotimm((uint32_t)r.v));
      return;
    }
    if (ip->litmode == 2) {
      eres r; ev(a->off, &r);
      put32(out, ((uint32_t)ip->cond << 28) | 0x03e00000 | (uint32_t)ip->r[0] << 12 | (uint32_t)rotimm(~(uint32_t)r.v));
      return;
    }
    if (ip->litmode == 3) {
      eres r; ival d;
      ev(a->off, &r);
      d = r.v - (pc + 8);
      put32(out, ((uint32_t)ip->cond << 28) | (d < 0 ? 0x024f0000 : 0x028f0000) | (uint32_t)rotimm((uint32_t)(d < 0 ? -d : d)));
      return;
    }
    {
      ival d = (ival)ip->pool_idx * 4 - (pc + 8);
      if (half) {
        off = pc_off(d, 8, &U);
        put32(out, w | 0x01400000 | (uint32_t)U << 23 | sh | half_off(off));
      } else {
        off = pc_off(d, 12, &U);
        put32(out, w | 0x05000000 | (uint32_t)U << 23 | (uint32_t)B << 22 | 15u << 16 | off);
      }
      return;
    }
  }
  if (a->kind == 2) {
    eres r; ival d = 0;
    ev(a->off, &r);
    rn = 15;
    if (!pcrel(&r, s, pc, 8, &d)) error("label from current section required");
    off = pc_off(d, half ? 8 : 12, &U);
    I = half;
    if (half) rn = 0;     /* vasm: the base register field stays 0 */
  } else if (a->hasoff == 1) {
    ival v = cval(a->off, 1);
    ival lim = half ? 255 : 4095;
    U = v > 0 || (v == 0 && a->explicit_sign != -1);
    if (v < 0) v = -v;
    if (v > lim) error("%d-bit immediate offset out of range (%lld)", half ? 8 : 12, (long long)(U ? v : -v));
    off = (uint32_t)(v & lim);
    I = half;
    P = a->pre; W = a->pre ? a->wb : 0;
  } else if (a->hasoff == 2) {
    U = a->sign > 0;
    if (half) { off = (uint32_t)a->rm; I = 0; }
    else {
      I = 1;
      off = (uint32_t)a->rm;
      if (a->shimm_given) {
        if (!a->shamt) off |= 0x60;
        else {
          ival n = cval(a->shamt, 1);
          ival hi = (a->shtype == 1 || a->shtype == 2) ? 32 : 31;
          if (n < 0 || n > hi) error("illegal immediate shift count (%lld)", (long long)n);
          off |= (uint32_t)(((n & 31) << 7) | (a->shtype << 5));
        }
      }
    }
    P = a->pre; W = a->pre ? a->wb : 0;
  } else {
    /* [Rn] or [Rn]! */
    I = half;
    U = !half;
    W = a->wb;
  }
  if (T) { P = 0; W = 1; }
  if (half)
    put32(out, w | (uint32_t)P << 24 | (uint32_t)U << 23 | (uint32_t)I << 22 | (uint32_t)W << 21 |
               (uint32_t)rn << 16 | sh | (I ? half_off(off) : off));
  else
    put32(out, w | 0x04000000 | (uint32_t)I << 25 | (uint32_t)P << 24 | (uint32_t)U << 23 | (uint32_t)B << 22 |
               (uint32_t)W << 21 | (uint32_t)rn << 16 | off);
}

static void enc_arm(ainsn *ip, section *s, ival pc, bbuf *out, ival size)
{
  uint32_t c = (uint32_t)ip->cond << 28;
  eres r;
  switch (ip->m->type) {
  case T_DP: enc_dp(ip, out, size); return;
  case T_MUL:
    put32(out, c | (uint32_t)ip->m->opc << 21 | (uint32_t)(ip->sfx[0] == 's') << 20 | (uint32_t)ip->r[0] << 16 |
               (ip->m->opc ? (uint32_t)ip->r[3] << 12 : 0) | (uint32_t)ip->r[2] << 8 | 0x90 | (uint32_t)ip->r[1]);
    return;
  case T_MULL:
    put32(out, c | 0x00800090 | (uint32_t)ip->m->opc << 21 | (uint32_t)(ip->sfx[0] == 's') << 20 |
               (uint32_t)ip->r[1] << 16 | (uint32_t)ip->r[0] << 12 | (uint32_t)ip->r[3] << 8 | (uint32_t)ip->r[2]);
    return;
  case T_LDR: case T_STR: enc_ldst(ip, s, pc, out); return;
  case T_LDM: case T_STM: {
    static const int pu[8] = { 1, 3, 0, 2, 0, 0, 0, 0 };  /* ia ib da db -> P<<1|U */
    const char *md = ip->sfx;
    int L = ip->m->type == T_LDM, k;
    static const char *mn[] = { "ia", "ib", "da", "db", "fd", "ed", "fa", "ea" };
    for (k = 0; k < 8; k++) if (!strcmp(md, mn[k])) break;
    if (k >= 4) {
      /* stack modes */
      static const int ldm_m[4] = { 0, 1, 2, 3 }, stm_m[4] = { 3, 2, 1, 0 };
      k = L ? ldm_m[k - 4] : stm_m[k - 4];
    }
    {
      int x = k == 0 ? 1 : k == 1 ? 3 : k == 2 ? 0 : 2;
      (void)pu;
      put32(out, c | 0x08000000 | (uint32_t)(x >> 1) << 24 | (uint32_t)(x & 1) << 23 | (uint32_t)ip->hat << 22 |
                 (uint32_t)ip->wb << 21 | (uint32_t)L << 20 | (uint32_t)ip->r[0] << 16 | (ip->rlist & 0xffff));
    }
    return;
  }
  case T_SWP:
    put32(out, c | 0x01000090 | (uint32_t)(ip->sfx[0] == 'b') << 22 | (uint32_t)ip->r[2] << 16 |
               (uint32_t)ip->r[0] << 12 | (uint32_t)ip->r[1]);
    return;
  case T_B: case T_BL: {
    ival d = 0;
    uint32_t w = c | 0x0a000000 | (uint32_t)(ip->m->type == T_BL) << 24;
    ev(ip->e, &r);
    if (pcrel(&r, s, pc, 8, &d)) {
      if (d & 3) error("branch to an unaligned address (offset %lld)", (long long)d);
      else if (d < -0x2000000 || d > 0x1fffffc) error("branch offset (%lld) is out of range", (long long)d);
    } else {
      add_reloc(s, pc, 24, R_PC, r.sec, r.ext, r.v);
      d = r.v - 8;
    }
    put32(out, w | ((uint32_t)(d >> 2) & 0xffffff));
    return;
  }
  case T_BX: put32(out, c | 0x012fff10 | (uint32_t)ip->r[0]); return;
  case T_SWI: {
    ival v = cval(ip->e, 1);
    if (v < 0 || v > 0xffffff) error("24-bit unsigned immediate expected");
    put32(out, c | 0x0f000000 | ((uint32_t)v & 0xffffff));
    return;
  }
  case T_MRS: put32(out, c | 0x010f0000 | (uint32_t)ip->psr << 22 | (uint32_t)ip->r[0] << 12); return;
  case T_MSR:
    if (ip->e) {
      ival v = cval(ip->e, 1);
      int f = rotimm((uint32_t)v);
      if (f < 0) { error("constant (0x%llx) not suitable for 8-bit rotated immediate", (unsigned long long)(uint32_t)v); f = 0; }
      put32(out, c | 0x0320f000 | (uint32_t)ip->psr << 22 | (uint32_t)ip->fields << 16 | (uint32_t)f);
    } else
      put32(out, c | 0x0120f000 | (uint32_t)ip->psr << 22 | (uint32_t)ip->fields << 16 | (uint32_t)ip->r[0]);
    return;
  case T_CDP: {
    ival o1 = cval(ip->e, 1), o2 = ip->e2 ? cval(ip->e2, 1) : 0;
    if (o1 < 0 || o1 > 15 || o2 < 0 || o2 > 7) error("illegal coprocessor operation mode or type: %lld\n", (long long)(o1 > 15 ? o1 : o2));
    put32(out, c | 0x0e000000 | (uint32_t)(o1 & 15) << 20 | (uint32_t)ip->r[2] << 16 | (uint32_t)ip->r[1] << 12 |
               (uint32_t)ip->r[0] << 8 | (uint32_t)(o2 & 7) << 5 | (uint32_t)ip->r[3]);
    return;
  }
  case T_MCR: case T_MRC: {
    ival o1 = cval(ip->e, 1), o2 = ip->e2 ? cval(ip->e2, 1) : 0;
    if (o1 < 0 || o1 > 7 || o2 < 0 || o2 > 7) error("illegal coprocessor operation mode or type: %lld\n", (long long)(o1 > 7 ? o1 : o2));
    put32(out, c | 0x0e000010 | (uint32_t)(o1 & 7) << 21 | (uint32_t)(ip->m->type == T_MRC) << 20 |
               (uint32_t)ip->r[2] << 16 | (uint32_t)ip->r[1] << 12 | (uint32_t)ip->r[0] << 8 |
               (uint32_t)(o2 & 7) << 5 | (uint32_t)ip->r[3]);
    return;
  }
  case T_LDC: case T_STC: {
    amode *a = &ip->am;
    int P = 1, U = 0, W = 0;
    uint32_t off = 0;
    if (a->hasoff == 1) {
      ival v = cval(a->off, 1);
      U = v > 0 || (v == 0 && a->explicit_sign != -1);
      if (v < 0) v = -v;
      if (v & 3) error("ldc/stc offset has to be a multiple of 4");
      if (v > 1020) error("%d-bit immediate offset out of range (%lld)", 8, (long long)v);
      off = (uint32_t)((v >> 2) & 255);
      P = a->pre; W = a->pre ? a->wb : 0;
    } else W = a->wb;
    /* vasm: the 'l' suffix (N bit) is accepted but not encoded */
    put32(out, c | 0x0c000000 | (uint32_t)P << 24 | (uint32_t)U << 23 |
               (uint32_t)W << 21 | (uint32_t)(ip->m->type == T_LDC) << 20 | (uint32_t)a->rn << 16 |
               (uint32_t)ip->r[1] << 12 | (uint32_t)ip->r[0] << 8 | off);
    return;
  }
  case T_ADR: case T_ADRL: {
    ival d = 0;
    uint32_t ad;
    int f;
    ev(ip->e, &r);
    if (!pcrel(&r, s, pc, 8, &d)) error("label from current section required");
    ad = (uint32_t)(d < 0 ? -d : d);
    if (size == 8) {
      uint32_t a = 0, b = 0;
      uint32_t op = d < 0 ? 0x02400000 : 0x02800000;
      if (!split_imm(ad, &a, &b)) {
        if ((f = rotimm(ad)) >= 0) { a = ad; b = 0; }
        else error("cannot make rotated immediate from PC-relative offset (0x%llx)", (unsigned long long)(uint32_t)d);
      }
      put32(out, c | op | 15u << 16 | (uint32_t)ip->r[0] << 12 | (uint32_t)(rotimm(a) & 0xfff));
      put32(out, c | op | (uint32_t)ip->r[0] << 16 | (uint32_t)ip->r[0] << 12 | (uint32_t)(rotimm(b) & 0xfff));
      return;
    }
    f = rotimm(ad);
    if (f < 0) { error("cannot make rotated immediate from PC-relative offset (0x%llx)", (unsigned long long)(uint32_t)d); f = 0; }
    put32(out, c | (d < 0 ? 0x024f0000 : 0x028f0000) | (uint32_t)ip->r[0] << 12 | (uint32_t)f);
    return;
  }
  case T_NOP: put32(out, c | 0x01a00000); return;
  }
}

/* ------------------------------------------------------------ THUMB */
enum { TH_SHIFT, TH_ADD, TH_SUB, TH_MOV, TH_CMP, TH_ALU, TH_BX, TH_LDST, TH_PUSH, TH_POP,
       TH_LDMIA, TH_STMIA, TH_SWI, TH_B, TH_BCC, TH_BL, TH_ADR, TH_NOP };
/* TH_LDST op: 0 str 1 strb 2 ldr 3 ldrb 4 strh 5 ldsb 6 ldrh 7 ldsh */
typedef struct tmn { const char *name; int kind; int op; } tmn;
static const tmn ttab[] = {
  { "lsl", TH_SHIFT, 0 }, { "lsr", TH_SHIFT, 1 }, { "asr", TH_SHIFT, 2 },
  { "add", TH_ADD, 0 }, { "sub", TH_SUB, 0 }, { "mov", TH_MOV, 0 }, { "cmp", TH_CMP, 0 },
  { "and", TH_ALU, 0 }, { "eor", TH_ALU, 1 }, { "adc", TH_ALU, 5 }, { "sbc", TH_ALU, 6 },
  { "ror", TH_ALU, 7 }, { "tst", TH_ALU, 8 }, { "neg", TH_ALU, 9 }, { "cmn", TH_ALU, 11 },
  { "orr", TH_ALU, 12 }, { "mul", TH_ALU, 13 }, { "bic", TH_ALU, 14 }, { "mvn", TH_ALU, 15 },
  { "bx", TH_BX, 0 }, { "str", TH_LDST, 0 }, { "strb", TH_LDST, 1 }, { "ldr", TH_LDST, 2 },
  { "ldrb", TH_LDST, 3 }, { "strh", TH_LDST, 4 }, { "ldsb", TH_LDST, 5 }, { "ldrh", TH_LDST, 6 },
  { "ldsh", TH_LDST, 7 }, { "push", TH_PUSH, 0 }, { "pop", TH_POP, 0 }, { "ldmia", TH_LDMIA, 0 },
  { "stmia", TH_STMIA, 0 }, { "swi", TH_SWI, 0 }, { "b", TH_B, 0 }, { "bl", TH_BL, 0 },
  { "adr", TH_ADR, 0 }, { "nop", TH_NOP, 0 },
  { NULL, 0, 0 }
};

static int lo(int r) { return r >= 0 && r < 8; }

static int thumb_create(const char *m, size_t ml, char **ops, int nops)
{
  const tmn *t = NULL;
  ainsn *ip;
  int i;
  for (i = 0; ttab[i].name; i++)
    if (strlen(ttab[i].name) == ml && !strnicmp_(m, ttab[i].name, ml)) { t = &ttab[i]; break; }
  ip = new_insn();
  ip->thumb = 1;
  ip->cond = 14;
  if (!t) {
    int c;
    if (ml == 3 && tolower((unsigned char)m[0]) == 'b' && (c = cond_of(m + 1)) >= 0 && c < 14) {
      ip->tid = TH_BCC; ip->cond = c;
    } else
      return 0;
  } else {
    ip->tid = t->kind;
    ip->lflag = t->op;
  }
  switch (ip->tid) {
  case TH_B: case TH_BCC: case TH_BL: case TH_SWI:
    if (nops != 1 || !(ip->e = imm_op(ops[0], 0, NULL))) ILLOPS();
    break;
  case TH_BX:
    if (nops != 1 || (ip->r[0] = is_reg(ops[0])) < 0) ILLOPS();
    break;
  case TH_PUSH: case TH_POP: {
    /* vasm: any register above r7 sets the lr/pc bit */
    if (nops != 1 || !parse_rlist(ops[0], &ip->rlist, &ip->hat) || ip->hat) ILLOPS();
    break;
  }
  case TH_LDMIA: case TH_STMIA: {
    const char *p;
    if (nops != 2) ILLOPS();
    p = ops[0];
    ip->r[0] = get_reg(&p);
    p = skip_ws(p);
    if (!lo(ip->r[0]) || *p != '!' || *skip_ws(p + 1)) ILLOPS();
    if (!parse_rlist(ops[1], &ip->rlist, &ip->hat) || ip->hat || (ip->rlist & ~0xffu)) ILLOPS();
    break;
  }
  case TH_ADR:
    if (nops != 2 || !lo(ip->r[0] = is_reg(ops[0]))) ILLOPS();
    { const char *p = skip_ws(ops[1]); ip->e = pexp(&p); if (!ip->e || *skip_ws(p)) ILLOPS(); }
    break;
  case TH_NOP:
    if (nops) ILLOPS();
    break;
  case TH_LDST: {
    int n;
    if (nops < 2 || !lo(ip->r[0] = is_reg(ops[0]))) ILLOPS();
    n = parse_amode(ops, nops, 1, &ip->am, 0);
    if (!n || ip->am.kind == 3) ILLOPS();
    if (ip->am.kind == 2 && ip->lflag != 2) ILLOPS();
    if (ip->am.kind == 1) {
      amode *a = &ip->am;
      int op = ip->lflag;
      if (!a->pre || a->wb || !a->hasoff) ILLOPS();
      if (a->hasoff == 2 && (a->sign < 0 || !lo(a->rm) || !lo(a->rn))) ILLOPS();
      if (a->hasoff == 1) {
        if (op == 5 || op == 7) ILLOPS();
        if (a->rn == 13) { if (op != 0 && op != 2) ILLOPS(); }
        else if (a->rn == 15) { if (op != 2) ILLOPS(); }
        else if (!lo(a->rn)) ILLOPS();
      }
    }
    break;
  }
  default: {
    /* register / immediate forms: up to 3 operands */
    if (nops < 2 || nops > 3) ILLOPS();
    for (i = 0; i < nops; i++) {
      const char *p = skip_ws(ops[i]);
      if (*p == '#') { if (i != nops - 1 || !(ip->e = imm_op(p, 1, NULL))) ILLOPS(); ip->r[i] = -1; }
      else if ((ip->r[i] = is_reg(p)) < 0) ILLOPS();
    }
    ip->r[4] = nops;
    switch (ip->tid) {
    case TH_SHIFT:
      if (nops == 3 ? !(ip->e && lo(ip->r[0]) && lo(ip->r[1])) : !(!ip->e && lo(ip->r[0]) && lo(ip->r[1]))) ILLOPS();
      break;
    case TH_ALU:
      if (nops != 2 || ip->e || !lo(ip->r[0]) || !lo(ip->r[1])) ILLOPS();
      break;
    case TH_MOV: case TH_CMP:
      if (nops != 2) ILLOPS();
      if (ip->e) { if (!lo(ip->r[0])) ILLOPS(); }
      break;
    case TH_ADD: case TH_SUB:
      if (nops == 3) {
        if (ip->r[1] == 15 || ip->r[1] == 13) {
          if (ip->tid != TH_ADD || !ip->e || !lo(ip->r[0])) ILLOPS();
        } else if (!lo(ip->r[0]) || !lo(ip->r[1]) || (!ip->e && !lo(ip->r[2]))) ILLOPS();
      } else {
        if (ip->e) { if (!lo(ip->r[0]) && ip->r[0] != 13) ILLOPS(); }
        else if (ip->tid == TH_SUB || (lo(ip->r[0]) && lo(ip->r[1]))) ILLOPS();
      }
      break;
    }
  }
  }
  emit_insn(ip);
  return 1;
}

static ival thumb_size(ainsn *ip, section *s, ival pc)
{
  if (ip->tid == TH_BL || ip->tid == TH_NOP) return 4;
  if (ip->tid == TH_BCC) {
    eres r; ival d;
    ev(ip->e, &r);
    if (pcrel(&r, s, pc, 4, &d) && (d < -256 || d > 254)) return 4;
  }
  return 2;
}

static uint32_t tconst(ainsn *ip, ival lo_, ival hi_, int scale, int bits)
{
  ival v = cval(ip->e, 1);
  if (v < lo_ || v > hi_ || (v % scale))
    error("%d-bit unsigned immediate offset out of range (%lld)", bits, (long long)v);
  return (uint32_t)(v / scale);
}

static void enc_thumb(ainsn *ip, section *s, ival pc, bbuf *out, ival size)
{
  eres r;
  ival d = 0;
  int *R = ip->r;
  uint32_t w = 0;
  switch (ip->tid) {
  case TH_SHIFT:
    if (ip->r[4] == 3) w = (uint32_t)ip->lflag << 11 | (tconst(ip, 0, 31, 1, 5) & 31) << 6 | (uint32_t)R[1] << 3 | (uint32_t)R[0];
    else w = 0x4000 | (uint32_t)(ip->lflag == 0 ? 2 : ip->lflag == 1 ? 3 : 4) << 6 | (uint32_t)R[1] << 3 | (uint32_t)R[0];
    break;
  case TH_ALU: w = 0x4000 | (uint32_t)ip->lflag << 6 | (uint32_t)R[1] << 3 | (uint32_t)R[0]; break;
  case TH_ADD: case TH_SUB: {
    int sub = ip->tid == TH_SUB;
    if (ip->r[4] == 3) {
      if (R[1] == 15) w = 0xa000 | (uint32_t)R[0] << 8 | (tconst(ip, 0, 1020, 4, 10) & 255);
      else if (R[1] == 13) w = 0xa800 | (uint32_t)R[0] << 8 | (tconst(ip, 0, 1020, 4, 10) & 255);
      else if (ip->e) w = (sub ? 0x1e00 : 0x1c00) | (tconst(ip, 0, 7, 1, 3) & 7) << 6 | (uint32_t)R[1] << 3 | (uint32_t)R[0];
      else w = (sub ? 0x1a00 : 0x1800) | (uint32_t)R[2] << 6 | (uint32_t)R[1] << 3 | (uint32_t)R[0];
    } else if (ip->e) {
      if (R[0] == 13) w = 0xb000 | (sub ? 0x80 : 0) | (tconst(ip, 0, 508, 4, 9) & 127);
      else w = (sub ? 0x3800 : 0x3000) | (uint32_t)R[0] << 8 | (tconst(ip, 0, 255, 1, 8) & 255);
    } else
      w = 0x4400 | (uint32_t)(R[0] & 8) << 4 | (uint32_t)(R[1] & 15) << 3 | (uint32_t)(R[0] & 7);
    break;
  }
  case TH_MOV:
    if (ip->e) w = 0x2000 | (uint32_t)R[0] << 8 | (tconst(ip, 0, 255, 1, 8) & 255);
    else if (lo(R[0]) && lo(R[1])) w = 0x1c00 | (uint32_t)R[1] << 3 | (uint32_t)R[0];
    else w = 0x4600 | (uint32_t)(R[0] & 8) << 4 | (uint32_t)(R[1] & 15) << 3 | (uint32_t)(R[0] & 7);
    break;
  case TH_CMP:
    if (ip->e) w = 0x2800 | (uint32_t)R[0] << 8 | (tconst(ip, 0, 255, 1, 8) & 255);
    else if (lo(R[0]) && lo(R[1])) w = 0x4280 | (uint32_t)R[1] << 3 | (uint32_t)R[0];
    else w = 0x4500 | (uint32_t)(R[0] & 8) << 4 | (uint32_t)(R[1] & 15) << 3 | (uint32_t)(R[0] & 7);
    break;
  case TH_BX: w = 0x4700 | (uint32_t)(R[0] & 15) << 3; break;
  case TH_LDST: {
    amode *a = &ip->am;
    static const uint32_t regop[8] = { 0x5000, 0x5400, 0x5800, 0x5c00, 0x5200, 0x5600, 0x5a00, 0x5e00 };
    int op = ip->lflag;
    if (a->kind == 2) {
      ev(a->off, &r);
      if (!pcrel(&r, s, (pc + 4) & ~(ival)3, 0, &d)) error("label from current section required");
      if (d < 0 || d > 1020 || (d & 3)) error("%d-bit unsigned immediate offset out of range (%lld)", 10, (long long)d);
      w = 0x4800 | (uint32_t)R[0] << 8 | (uint32_t)((d >> 2) & 255);
    } else if (a->hasoff == 2)
      w = regop[op] | (uint32_t)a->rm << 6 | (uint32_t)a->rn << 3 | (uint32_t)R[0];
    else {
      ival v = cval(a->off, 1);
      if (a->rn == 15 || a->rn == 13) {
        if (v < 0 || v > 1020 || (v & 3)) error("%d-bit unsigned immediate offset out of range (%lld)", 10, (long long)v);
        w = (a->rn == 15 ? 0x4800 : op == 0 ? 0x9000 : 0x9800) | (uint32_t)R[0] << 8 | (uint32_t)((v >> 2) & 255);
      } else {
        int sc = op <= 1 ? (op == 0 ? 4 : 1) : op == 2 ? 4 : op == 3 ? 1 : 2;
        uint32_t base = op == 0 ? 0x6000 : op == 1 ? 0x7000 : op == 2 ? 0x6800 : op == 3 ? 0x7800 : op == 4 ? 0x8000 : 0x8800;
        if (v < 0 || v > 31 * sc || (v % sc)) error("%d-bit unsigned immediate offset out of range (%lld)", sc == 4 ? 7 : sc == 2 ? 6 : 5, (long long)v);
        w = base | (uint32_t)((v / sc) & 31) << 6 | (uint32_t)a->rn << 3 | (uint32_t)R[0];
      }
    }
    break;
  }
  case TH_PUSH: w = 0xb400 | (uint32_t)((ip->rlist & 0xff00) != 0) << 8 | (ip->rlist & 0xff); break;
  case TH_POP: w = 0xbc00 | (uint32_t)((ip->rlist & 0xff00) != 0) << 8 | (ip->rlist & 0xff); break;
  case TH_LDMIA: w = 0xc800 | (uint32_t)R[0] << 8 | (ip->rlist & 0xff); break;
  case TH_STMIA: w = 0xc000 | (uint32_t)R[0] << 8 | (ip->rlist & 0xff); break;
  case TH_SWI: w = 0xdf00 | (tconst(ip, 0, 255, 1, 8) & 255); break;
  case TH_ADR:
    ev(ip->e, &r);
    if (!pcrel(&r, s, (pc + 4) & ~(ival)3, 0, &d)) error("label from current section required");
    if (d < 0 || d > 1020 || (d & 3)) error("%d-bit unsigned immediate offset out of range (%lld)", 10, (long long)d);
    w = 0xa000 | (uint32_t)R[0] << 8 | (uint32_t)((d >> 2) & 255);
    break;
  case TH_B:
    ev(ip->e, &r);
    if (!pcrel(&r, s, pc, 4, &d)) { error("operation not allowed on external symbols"); d = 0; }
    if (d < -2048 || d > 2046) error("branch offset (%lld) is out of range", (long long)d);
    w = 0xe000 | (uint32_t)((d >> 1) & 0x7ff);
    break;
  case TH_BCC:
    ev(ip->e, &r);
    if (!pcrel(&r, s, pc, 4, &d)) { error("operation not allowed on external symbols"); d = 0; }
    if (size == 4) {
      put16(out, 0xd000 | (uint32_t)(ip->cond ^ 1) << 8);
      if (d < 0) d -= 2;   /* vasm: forward targets stay relative to the Bcc */
      if (d < -2048 || d > 2046) error("branch offset (%lld) is out of range", (long long)d);
      w = 0xe000 | (uint32_t)((d >> 1) & 0x7ff);
      break;
    }
    w = 0xd000 | (uint32_t)ip->cond << 8 | (uint32_t)((d >> 1) & 0xff);
    break;
  case TH_BL:
    ev(ip->e, &r);
    if (!pcrel(&r, s, pc, 4, &d)) {
      add_reloc(s, pc, 22, R_PC, r.sec, r.ext, r.v);
      d = r.v - 4;
    } else if (d < -0x400000 || d > 0x3ffffe)
      error("branch offset (%lld) is out of range", (long long)d);
    put16(out, 0xf000 | (uint32_t)((d >> 12) & 0x7ff));
    w = 0xf800 | (uint32_t)((d >> 1) & 0x7ff);
    break;
  case TH_NOP:
    put32(out, 0xe1a00000);
    return;
  }
  put16(out, w);
}

/* ------------------------------------------------------------ module glue */
static int lastpass = -1;

static ival arm_size(atom *a, section *s, ival pc)
{
  ainsn *ip = a->u.insn.insn;
  if (pass_no != lastpass) {
    pool *p;
    for (p = pools; p; p = p->next) p->n = 0;
    lastpass = pass_no;
  }
  if (ip->tid == -1) {
    ival abspc = pc + ((s->flags & SEC_ABS) ? s->org : 0);
    if (!ip->pool->n) return 0;
    if (s->align < 4) s->align = 4;
    return ((4 - (abspc & 3)) & 3) + (ival)ip->pool->n * 4;
  }
  {
    ival sz = ip->thumb ? thumb_size(ip, s, pc) : arm_size1(ip, s, pc);
    if (grow_only && sz < a->size) sz = a->size;
    return sz;
  }
}

static void arm_encode(atom *a, section *s, ival pc, bbuf *out)
{
  ainsn *ip = a->u.insn.insn;
  ival abspc = pc + ((s->flags & SEC_ABS) ? s->org : 0);
  if (ip->tid == -1) {
    pool *p = ip->pool;
    int i;
    if (!p->n && p->explicit_) warning("literal pool has no references");
    if (p->n && (abspc & 3)) warning("instruction has been auto-aligned");
    while (p->n && (abspc & 3)) { bb_byte(out, 0); abspc++; pc++; }
    for (i = 0; i < p->n; i++) {
      eres r;
      ev(p->ent[i].e, &r);
      put32(out, (uint32_t)r.v);
    }
    for (i = p->n - 1; i >= 0; i--) {     /* vasm: relocations in reverse order */
      eres r;
      ev(p->ent[i].e, &r);
      if (r.sec || r.ext) add_reloc(s, pc + 4 * i, 32, R_ABS, r.sec, r.ext, r.v);
    }
    return;
  }
  if (abspc & ((ip->thumb && ip->tid != TH_NOP) ? 1 : 3)) error("instruction at unaligned address");
  if (ip->thumb) enc_thumb(ip, s, pc, out, a->size);
  else enc_arm(ip, s, pc, out, a->size);
}

static void add_pool_atom(pool *p)
{
  ainsn *ip = new_insn();
  atom *a;
  ip->tid = -1;
  ip->pool = p;
  a = add_atom(AT_INSN);
  a->u.insn.insn = ip;
  p->atom = a;
}

static int arm_instruction(const char *mn, size_t ml, const char *ext,
                           char **ops, int nops, int col)
{
  const amn *m;
  int cond;
  char sfx[64];
  (void)ext; (void)col;
  if (thumb_mode) return thumb_create(mn, ml, ops, nops);
  if (ml >= sizeof sfx) return 0;
  m = parse_mnem(mn, ml, &cond, sfx);
  if (!m) return 0;
  return arm_create(m, cond, sfx, ops, nops);
}

static int arm_directive(const char *name, const char *args)
{
  (void)args;
  if (!stricmp_(name, ".arm")) { thumb_mode = 0; return 1; }
  if (!stricmp_(name, ".thumb")) { thumb_mode = 1; return 1; }
  if (!stricmp_(name, ".ltorg")) {
    section *s = default_section();
    pool *p = curpool_for(s);
    secpool *sp;
    p->explicit_ = 1;
    add_pool_atom(p);
    for (sp = secpools; sp; sp = sp->next) if (sp->s == s) sp->p = new_pool(s, 0);
    return 1;
  }
  return 0;
}

static void arm_parse_done(void)
{
  secpool *sp;
  section *save = cursec;
  for (sp = secpools; sp; sp = sp->next) {
    cursec = sp->s;
    add_pool_atom(sp->p);
  }
  cursec = save;
}

/* ELF mapping symbols $a/$t/$d */
static void arm_post_layout(void)
{
  extern out_module out_elf;
  section *s;
  if (outmod != &out_elf) return;
  for (s = sec_first; s; s = s->next) {
    atom *a;
    int cur = 0;
    if ((s->flags & SEC_OFFSET) || s->type == ST_BSS) continue;
    for (a = s->first; a; a = a->next) {
      int k = 0;
      ival at = a->off;
      if (!a->size) continue;
      if (a->type == AT_INSN) {
        ainsn *ip = a->u.insn.insn;
        k = ip->tid == -1 ? 'd' : (ip->thumb && ip->tid != TH_NOP) ? 't' : 'a';
        if (ip->tid == -1) at = a->off + a->size - 4 * ip->pool->n;
      } else if (a->type == AT_DATA || a->type == AT_BYTES || a->type == AT_FLOAT)   /* not .space (as vasm does) */
        k = 'd';
      if (k && k != cur) {
        char n[3] = { '$', (char)k, 0 };
        symbol *y = sym_new_anon(n);
        y->type = S_LABEL;
        y->sec = s;
        y->val = at;
        y->flags |= SF_DEFINED;
        y->stype = k == 'd' ? 1 : 2;
        cur = k;
      }
    }
  }
}

static int arm_option(const char *o)
{
  static const struct { const char *n; int a; } cpus[] = {
    { "-m2", 2 }, { "-m250", 2 }, { "-m3", 2 }, { "-m6", 3 }, { "-m600", 3 }, { "-m610", 3 },
    { "-m7", 3 }, { "-m710", 3 }, { "-m7500", 3 }, { "-m7d", 3 }, { "-m7di", 3 }, { "-m7dm", 4 },
    { "-m7dmi", 4 }, { "-m7tdmi", 5 }, { "-m8", 4 }, { "-m810", 4 }, { "-m9", 5 }, { "-m920", 5 },
    { "-m920t", 5 }, { "-m9tdmi", 5 }, { "-msa1", 4 }, { "-mstrongarm", 4 }, { "-mstrongarm110", 4 },
    { "-mstrongarm1100", 4 }, { "-a2", 2 }, { "-a3", 3 }, { "-a3m", 4 }, { "-a4", 4 }, { "-a4t", 5 },
    { NULL, 0 } };
  int i;
  for (i = 0; cpus[i].n; i++) if (!strcmp(o, cpus[i].n)) { arch = cpus[i].a; arch_m = arch >= 4; arch_t = arch >= 5; return 1; }
  if (!strcmp(o, "-big")) { arm_big = 1; cpu_arm.big_endian = 1; return 1; }
  if (!strcmp(o, "-little")) { arm_big = 0; cpu_arm.big_endian = 0; return 1; }
  if (!strcmp(o, "-opt-adr")) { opt_adr = 1; return 1; }
  if (!strcmp(o, "-opt-ldrpc")) { opt_ldrpc = 1; return 1; }
  if (!strcmp(o, "-thumb")) { thumb_mode = 1; return 1; }
  return 0;
}

static void arm_define_symbols(void)
{
  set_internal(sym_get("__VASM"), 0);
  sym_get("__VASM")->flags |= SF_INTERNAL;
}

static int arm_init(void) { return 1; }

static int arm_elf_reloc(reloc *r)
{
  if (r->type == R_ABS) return r->size == 32 ? 2 : r->size == 16 ? 5 : r->size == 8 ? 8 : -1;
  if (r->type == R_PC) return r->size == 24 ? 1 : r->size == 22 ? 10 : r->size == 32 ? 3 : -1;
  return -1;
}

cpu_module cpu_arm = {
  "arm",
  arm_init,
  arm_option,
  arm_instruction,
  arm_directive,
  arm_size,
  arm_encode,
  NULL,
  arm_define_symbols,
  NULL,
  0, 1, 0, 32, 40, arm_elf_reloc,
  1, 0x04000000, arm_parse_done, arm_post_layout
};
