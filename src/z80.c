/* gfasm - Z80 cpu module (Zilog syntax, with the common undocumented
 * instructions).  MIT License.
 *
 * Encodings from the public Zilog "Z80 Family CPU User Manual" (UM0080)
 * and the widely published undocumented-opcode tables (ixh/ixl, sll).
 * vasm behaviour (options, lo/hi operators, messages) from the vasm
 * manual's z80 chapter and comparison with vasm's output; see
 * docs/REFERENCES.md. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ------------------------------------------------------------ operands */
enum { Z_NONE, Z_R8, Z_R16, Z_IND, Z_IDX, Z_MEM, Z_IMM, Z_C, Z_I, Z_R, Z_AFX, Z_F };
typedef struct zop {
  int kind;
  int reg;          /* R8: 0-7 (6 = (hl)); R16: 0 bc 1 de 2 hl 3 sp 4 af; IND: 0 bc 1 de 2 hl 3 sp */
  int pfx;          /* 0, 0xdd, 0xfd */
  int name;         /* raw register id for conditions: see cond_of() */
  expr *e;
} zop;

typedef struct zinsn {
  char *mn;
  int nops;
  zop op[3];
} zinsn;

static int zregname(const char *s, size_t n, zop *o)
{
  static const struct { const char *n; int kind, reg, pfx; } t[] = {
    { "b", Z_R8, 0, 0 }, { "c", Z_R8, 1, 0 }, { "d", Z_R8, 2, 0 }, { "e", Z_R8, 3, 0 },
    { "h", Z_R8, 4, 0 }, { "l", Z_R8, 5, 0 }, { "a", Z_R8, 7, 0 },
    { "ixh", Z_R8, 4, 0xdd }, { "ixl", Z_R8, 5, 0xdd }, { "iyh", Z_R8, 4, 0xfd }, { "iyl", Z_R8, 5, 0xfd },
    { "bc", Z_R16, 0, 0 }, { "de", Z_R16, 1, 0 }, { "hl", Z_R16, 2, 0 }, { "sp", Z_R16, 3, 0 },
    { "af", Z_R16, 4, 0 }, { "ix", Z_R16, 2, 0xdd }, { "iy", Z_R16, 2, 0xfd },
    { "i", Z_I, 0, 0 }, { "r", Z_R, 0, 0 }, { "af'", Z_AFX, 0, 0 },
    { NULL, 0, 0, 0 } };
  int i;
  for (i = 0; t[i].n; i++)
    if (strlen(t[i].n) == n && !strnicmp_(s, t[i].n, n)) {
      o->kind = t[i].kind; o->reg = t[i].reg; o->pfx = t[i].pfx;
      return 1;
    }
  return 0;
}

static expr *zexpr(const char *s)
{
  const char *p = skip_ws(s);
  int lohi = 0;
  expr *e;
  if (*p == '<' || *p == '>') { lohi = *p; p++; }
  e = parse_expr(&p);
  if (!e || *skip_ws(p)) return NULL;
  if (lohi == '<') e = new_op(X_AND, e, new_num(255));
  else if (lohi == '>') e = new_op(X_AND, new_op(X_SHR, e, new_num(8)), new_num(255));
  return e;
}

static int parse_zop(const char *s, zop *o)
{
  const char *p = skip_ws(s);
  size_t n = strlen(p);
  memset(o, 0, sizeof(*o));
  while (n && is_ws(p[n - 1])) n--;
  if (zregname(p, n, o)) return 1;
  if (*p == '(' && p[n - 1] == ')') {
    /* is the whole operand one parenthesis pair? */
    int depth = 0;
    size_t i;
    for (i = 0; i < n; i++) {
      if (p[i] == '(') depth++;
      else if (p[i] == ')') { depth--; if (!depth && i != n - 1) break; }
    }
    if (i == n) {
      char *in = xstrndup(p + 1, n - 2);
      const char *q = skip_ws(in);
      size_t m = strlen(q), k;
      zop r;
      while (m && is_ws(q[m - 1])) m--;
      if (zregname(q, m, &r)) {
        free(in);
        if (r.kind == Z_R16 && r.reg != 4) {
          if (r.pfx) { o->kind = Z_IDX; o->pfx = r.pfx; o->e = new_num(0); return 1; }
          o->kind = Z_IND; o->reg = r.reg; return 1;
        }
        if (r.kind == Z_R8 && r.reg == 1 && !r.pfx) { o->kind = Z_C; return 1; }
        return 0;
      }
      /* (ix+d) / (iy-d) */
      for (k = 0; k < m && (isalpha((unsigned char)q[k])); k++) ;
      if (k == 2 && zregname(q, 2, &r) && r.kind == Z_R16 && r.pfx && (q[2] == '+' || q[2] == '-' || is_ws(q[2]))) {
        const char *d = skip_ws(q + 2);
        o->kind = Z_IDX; o->pfx = r.pfx;
        if (*d == '+') d++;
        else if (*d == '-') { o->e = zexpr(d); free(in); if (!o->e) return 0; o->e = o->e; return 1; }
        o->e = zexpr(d);
        free(in);
        return o->e != NULL;
      }
      o->kind = Z_MEM;
      o->e = zexpr(q);
      free(in);
      return o->e != NULL;
    }
  }
  o->kind = Z_IMM;
  if (*p == '#' && !isxdigit((unsigned char)p[1])) p++;
  o->e = zexpr(p);
  return o->e != NULL;
}

/* condition code of an operand written as a name (c doubles as register) */
static int cond_code(const char *s)
{
  static const char *cc[] = { "nz", "z", "nc", "c", "po", "pe", "p", "m", NULL };
  const char *p = skip_ws(s);
  size_t n = strlen(p);
  int i;
  while (n && is_ws(p[n - 1])) n--;
  for (i = 0; cc[i]; i++) if (strlen(cc[i]) == n && !strnicmp_(p, cc[i], n)) return i;
  return -1;
}

static zinsn *z80_create(const char *mn, size_t ml, char **ops, int nops)
{
  zinsn *ip;
  int i;
  ip = xcalloc(1, sizeof(*ip));
  ip->mn = xstrndup(mn, ml);
  for (i = 0; ip->mn[i]; i++) ip->mn[i] = (char)tolower((unsigned char)ip->mn[i]);
  ip->nops = nops;
  for (i = 0; i < nops; i++) {
    int c = cond_code(ops[i]);
    if (!parse_zop(ops[i], &ip->op[i])) ip->op[i].kind = -1;
    if (!stricmp_(skip_ws(ops[i]), "f")) ip->op[i].kind = Z_F;
    ip->op[i].name = c;
  }
  return ip;
}

/* ------------------------------------------------------------ encoding */
static int enc_on;
static section *esec;
static ival epc;

static void val8(bbuf *b, expr *e, int sgn)
{
  eres r;
  eval_expr(e, &r);
  if (enc_on) {
    if (r.sec || r.ext) add_reloc(esec, epc + (ival)b->n, 8, R_ABS, r.sec, r.ext, r.v);
    else if (r.v < -128 || r.v > 255 || (sgn && r.v > 127))
      error(sgn ? "index offset out of bounds (%d)" : "out of range for 8 bit expression (%d)", (int)r.v);
  }
  bb_byte(b, (int)r.v);
}

static void val16(bbuf *b, expr *e)
{
  eres r;
  eval_expr(e, &r);
  if (enc_on && (r.sec || r.ext)) add_reloc(esec, epc + (ival)b->n, 16, R_ABS, r.sec, r.ext, r.v);
  bb_byte(b, (int)r.v);
  bb_byte(b, (int)(r.v >> 8));
}

static void rel8(bbuf *b, expr *e)
{
  eres r;
  ival d;
  eval_expr(e, &r);
  if (r.sec == esec || (!r.sec && !r.ext)) {
    ival base = (esec->flags & SEC_ABS) && !r.sec ? esec->org : 0;
    d = r.v - (base + epc + 2);
    if (enc_on && (d < -128 || d > 127)) error("jr target out of range (%d)", (int)d);
  } else {
    if (enc_on) error("invalid branch type for jr");
    d = 0;
  }
  bb_byte(b, (int)d);
}

/* emit prefix/opcode/displacement for an 8-bit register-ish operand */
static int r8code(zop *o, int *pfx, expr **disp)
{
  if (o->kind == Z_R8) { if (o->pfx) { if (*pfx && *pfx != o->pfx) return -1; *pfx = o->pfx; } return o->reg; }
  if (o->kind == Z_IND && o->reg == 2) return 6;
  if (o->kind == Z_C) return 1;            /* vasm: (c) is c */
  if (o->kind == Z_IDX) { *pfx = o->pfx; *disp = o->e; return 6; }
  return -1;
}

static void opx(bbuf *b, int pfx, int op, expr *disp)
{
  if (pfx) bb_byte(b, pfx);
  bb_byte(b, op);
  if (disp) val8(b, disp, 1);
}

static int alu_index(const char *m)
{
  static const char *n[] = { "add", "adc", "sub", "sbc", "and", "xor", "or", "cp", NULL };
  int i;
  for (i = 0; n[i]; i++) if (!strcmp(m, n[i])) return i;
  return -1;
}
static int rot_index(const char *m)
{
  static const char *n[] = { "rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl", NULL };
  int i;
  for (i = 0; n[i]; i++) if (!strcmp(m, n[i])) return i;
  return -1;
}

static const struct { const char *n; int len; uint8_t b[2]; } simple[] = {
  { "nop", 1, { 0x00 } }, { "halt", 1, { 0x76 } }, { "di", 1, { 0xf3 } }, { "ei", 1, { 0xfb } },
  { "rlca", 1, { 0x07 } }, { "rla", 1, { 0x17 } }, { "rrca", 1, { 0x0f } }, { "rra", 1, { 0x1f } },
  { "daa", 1, { 0x27 } }, { "cpl", 1, { 0x2f } }, { "scf", 1, { 0x37 } }, { "ccf", 1, { 0x3f } },
  { "exx", 1, { 0xd9 } }, { "neg", 2, { 0xed, 0x44 } }, { "reti", 2, { 0xed, 0x4d } },
  { "retn", 2, { 0xed, 0x45 } }, { "rld", 2, { 0xed, 0x6f } }, { "rrd", 2, { 0xed, 0x67 } },
  { "ldi", 2, { 0xed, 0xa0 } }, { "ldir", 2, { 0xed, 0xb0 } }, { "ldd", 2, { 0xed, 0xa8 } },
  { "lddr", 2, { 0xed, 0xb8 } }, { "cpi", 2, { 0xed, 0xa1 } }, { "cpir", 2, { 0xed, 0xb1 } },
  { "cpd", 2, { 0xed, 0xa9 } }, { "cpdr", 2, { 0xed, 0xb9 } }, { "ini", 2, { 0xed, 0xa2 } },
  { "inir", 2, { 0xed, 0xb2 } }, { "ind", 2, { 0xed, 0xaa } }, { "indr", 2, { 0xed, 0xba } },
  { "outi", 2, { 0xed, 0xa3 } }, { "otir", 2, { 0xed, 0xb3 } }, { "outd", 2, { 0xed, 0xab } },
  { "otdr", 2, { 0xed, 0xbb } },
  { NULL, 0, { 0 } } };

/* returns 0 ok, -1 unknown mnemonic, -2 illegal operands */
static int z80_emit(zinsn *ip, bbuf *b)
{
  const char *m = ip->mn;
  zop *o = ip->op;
  int n = ip->nops, i, k, pfx = 0;
  expr *disp = NULL;
  for (i = 0; simple[i].n; i++)
    if (!strcmp(m, simple[i].n)) { if (n) return -2; bb_put(b, simple[i].b, simple[i].len); return 0; }
  if (!strcmp(m, "ld")) {
    int d, s;
    if (n != 2) return -2;
    /* 8-bit register / memory moves */
    pfx = 0;
    if ((d = r8code(&o[0], &pfx, &disp)) >= 0) {
      int pd = pfx;
      expr *dd = disp;
      int ps = pd;
      expr *ds = NULL;
      if ((s = r8code(&o[1], &ps, &ds)) >= 0) {
        if (pd && ps && pd != ps) return -2;
        opx(b, ps ? ps : pd, 0x40 | d << 3 | s, dd ? dd : ds);
        return 0;
      }
      if (o[1].kind == Z_IMM) { opx(b, pd, 0x06 | d << 3, dd); val8(b, o[1].e, 0); return 0; }
      if (d == 7 && o[1].kind == Z_IND && o[1].reg < 2) { bb_byte(b, o[1].reg ? 0x1a : 0x0a); return 0; }
      if (d == 7 && o[1].kind == Z_MEM) { bb_byte(b, 0x3a); val16(b, o[1].e); return 0; }
      if (d == 7 && o[1].kind == Z_I) { bb_byte(b, 0xed); bb_byte(b, 0x57); return 0; }
      if (d == 7 && o[1].kind == Z_R) { bb_byte(b, 0xed); bb_byte(b, 0x5f); return 0; }
      return -2;
    }
    if (o[0].kind == Z_IND && o[0].reg < 2 && o[1].kind == Z_R8 && o[1].reg == 7 && !o[1].pfx) { bb_byte(b, o[0].reg ? 0x12 : 0x02); return 0; }
    if (o[0].kind == Z_MEM && o[1].kind == Z_R8 && o[1].reg == 7 && !o[1].pfx) { bb_byte(b, 0x32); val16(b, o[0].e); return 0; }
    if (o[0].kind == Z_I && o[1].kind == Z_R8 && o[1].reg == 7) { bb_byte(b, 0xed); bb_byte(b, 0x47); return 0; }
    if (o[0].kind == Z_R && o[1].kind == Z_R8 && o[1].reg == 7) { bb_byte(b, 0xed); bb_byte(b, 0x4f); return 0; }
    if (o[0].kind == Z_R16 && o[0].reg < 4) {
      int r = o[0].reg;
      if (o[1].kind == Z_IMM) { opx(b, o[0].pfx, 0x01 | r << 4, NULL); val16(b, o[1].e); return 0; }
      if (o[1].kind == Z_MEM) {
        if (r == 2) { opx(b, o[0].pfx, 0x2a, NULL); }
        else { bb_byte(b, 0xed); bb_byte(b, 0x4b | r << 4); }
        val16(b, o[1].e);
        return 0;
      }
      if (r == 3 && o[1].kind == Z_R16 && o[1].reg == 2) { opx(b, o[1].pfx, 0xf9, NULL); return 0; }
      return -2;
    }
    if (o[0].kind == Z_MEM && o[1].kind == Z_R16 && o[1].reg < 4) {
      int r = o[1].reg;
      if (r == 2) opx(b, o[1].pfx, 0x22, NULL);
      else { bb_byte(b, 0xed); bb_byte(b, 0x43 | r << 4); }
      val16(b, o[0].e);
      return 0;
    }
    return -2;
  }
  if ((k = alu_index(m)) >= 0) {
    zop *src;
    /* 16-bit forms */
    if (n == 2 && o[0].kind == Z_R16 && o[0].reg == 2 && o[1].kind == Z_R16 && o[1].reg < 4) {
      int s = o[1].reg;
      if (k == 0) {
        if (o[1].pfx && o[1].pfx != o[0].pfx) return -2;
        if (s == 2 && !o[1].pfx && o[0].pfx) return -2;
        opx(b, o[0].pfx, 0x09 | s << 4, NULL);
        return 0;
      }
      if ((k == 1 || k == 3) && !o[1].pfx) {
        /* vasm: adc/sbc ix,rr keep the index prefix */
        if (o[0].pfx) bb_byte(b, o[0].pfx);
        bb_byte(b, 0xed); bb_byte(b, (k == 1 ? 0x4a : 0x42) | s << 4);
        return 0;
      }
      return -2;
    }
    if (n == 2) {
      if (!(o[0].kind == Z_R8 && o[0].reg == 7 && !o[0].pfx)) return -2;
      src = &o[1];
    } else if (n == 1) src = &o[0];
    else return -2;
    if (src->kind == Z_IMM) { bb_byte(b, 0xc6 | k << 3); val8(b, src->e, 0); return 0; }
    if ((i = r8code(src, &pfx, &disp)) < 0) return -2;
    opx(b, pfx, 0x80 | k << 3 | i, disp);
    return 0;
  }
  if (!strcmp(m, "inc") || !strcmp(m, "dec")) {
    int dec = m[0] == 'd';
    if (n != 1) return -2;
    if (o[0].kind == Z_R16 && o[0].reg < 4) { opx(b, o[0].pfx, (dec ? 0x0b : 0x03) | o[0].reg << 4, NULL); return 0; }
    if ((i = r8code(&o[0], &pfx, &disp)) < 0) return -2;
    opx(b, pfx, (dec ? 0x05 : 0x04) | i << 3, disp);
    return 0;
  }
  if ((k = rot_index(m)) >= 0 || !strcmp(m, "bit") || !strcmp(m, "res") || !strcmp(m, "set")) {
    int op, bitn = 0;
    zop *t;
    if (k >= 0) { if (n != 1) return -2; t = &o[0]; op = k << 3; }
    else {
      eres r;
      if (n != 2 || o[0].kind != Z_IMM) return -2;
      eval_expr(o[0].e, &r);
      bitn = (int)r.v;
      if (enc_on && (r.v < 0 || r.v > 7)) error("invalid bit number (%d) should be in range 0..7", (int)r.v);
      t = &o[1];
      op = (m[0] == 'b' ? 0x40 : m[0] == 'r' ? 0x80 : 0xc0) | (bitn & 7) << 3;
    }
    if ((i = r8code(t, &pfx, &disp)) < 0) return -2;
    if (t->kind == Z_R8 && t->pfx) { bb_byte(b, t->pfx); bb_byte(b, 0xcb); bb_byte(b, op | i); return 0; }  /* as vasm does */
    if (disp) { bb_byte(b, pfx); bb_byte(b, 0xcb); val8(b, disp, 1); bb_byte(b, op | 6); return 0; }
    bb_byte(b, 0xcb); bb_byte(b, op | i);
    return 0;
  }
  if (!strcmp(m, "push") || !strcmp(m, "pop")) {
    int pop = m[1] == 'o', r;
    if (n != 1 || o[0].kind != Z_R16) return -2;
    r = o[0].reg;
    if (r == 3) return -2;
    if (r == 4) r = 3;
    opx(b, o[0].pfx, (pop ? 0xc1 : 0xc5) | r << 4, NULL);
    return 0;
  }
  if (!strcmp(m, "ex")) {
    if (n != 2) return -2;
    if (o[0].kind == Z_R16 && o[0].reg == 4 && o[1].kind == Z_AFX) { bb_byte(b, 0x08); return 0; }
    if (o[0].kind == Z_R16 && o[0].reg == 1 && o[1].kind == Z_R16 && o[1].reg == 2 && !o[1].pfx) { bb_byte(b, 0xeb); return 0; }
    if (o[0].kind == Z_IND && o[0].reg == 3 && o[1].kind == Z_R16 && o[1].reg == 2) { opx(b, o[1].pfx, 0xe3, NULL); return 0; }
    return -2;
  }
  if (!strcmp(m, "jp") || !strcmp(m, "call")) {
    int call = m[0] == 'c';
    if (n == 1) {
      if (!call && (o[0].kind == Z_IND && o[0].reg == 2)) { bb_byte(b, 0xe9); return 0; }
      if (!call && o[0].kind == Z_IDX) { bb_byte(b, o[0].pfx); bb_byte(b, 0xe9); return 0; }
      if (o[0].kind != Z_IMM) return -2;
      bb_byte(b, call ? 0xcd : 0xc3); val16(b, o[0].e);
      return 0;
    }
    if (n == 2 && o[0].name >= 0 && o[1].kind == Z_IMM) {
      bb_byte(b, (call ? 0xc4 : 0xc2) | o[0].name << 3); val16(b, o[1].e);
      return 0;
    }
    return -2;
  }
  if (!strcmp(m, "jr") || !strcmp(m, "djnz")) {
    if (m[0] == 'd') { if (n != 1 || o[0].kind != Z_IMM) return -2; bb_byte(b, 0x10); rel8(b, o[0].e); return 0; }
    if (n == 1 && o[0].kind == Z_IMM) { bb_byte(b, 0x18); rel8(b, o[0].e); return 0; }
    if (n == 2 && o[0].name >= 0 && o[0].name < 4 && o[1].kind == Z_IMM) { bb_byte(b, 0x20 | o[0].name << 3); rel8(b, o[1].e); return 0; }
    return -2;
  }
  if (!strcmp(m, "ret")) {
    if (!n) { bb_byte(b, 0xc9); return 0; }
    if (n == 1 && o[0].name >= 0) { bb_byte(b, 0xc0 | o[0].name << 3); return 0; }
    return -2;
  }
  if (!strcmp(m, "rst")) {
    eres r;
    if (n != 1 || o[0].kind != Z_IMM) return -2;
    eval_expr(o[0].e, &r);
    if (enc_on && (r.v & ~0x38)) error("rst value out of range (%d/0x%02x)", (int)r.v, (int)r.v);
    bb_byte(b, 0xc7 | ((int)r.v & 0x38));
    return 0;
  }
  if (!strcmp(m, "im")) {
    eres r;
    if (n != 1 || o[0].kind != Z_IMM) return -2;
    eval_expr(o[0].e, &r);
    if (enc_on && (r.v < 0 || r.v > 2)) error("im value out of range (%d)", (int)r.v);
    bb_byte(b, 0xed); bb_byte(b, r.v == 0 ? 0x46 : r.v == 1 ? 0x56 : 0x5e);
    return 0;
  }
  if (!strcmp(m, "in")) {
    if (n == 1 && o[0].kind == Z_C) { bb_byte(b, 0xed); bb_byte(b, 0x70); return 0; }
    if (n != 2) return -2;
    if (o[0].kind == Z_R8 && o[0].reg == 7 && !o[0].pfx && o[1].kind == Z_MEM) { bb_byte(b, 0xdb); val8(b, o[1].e, 0); return 0; }
    if (o[0].kind == Z_R8 && !o[0].pfx && o[1].kind == Z_C) { bb_byte(b, 0xed); bb_byte(b, 0x40 | o[0].reg << 3); return 0; }
    if (o[0].kind == Z_F && o[1].kind == Z_C) { bb_byte(b, 0xed); bb_byte(b, 0x70); return 0; }
    return -2;
  }
  if (!strcmp(m, "out")) {
    if (n != 2) return -2;
    if (o[0].kind == Z_MEM && o[1].kind == Z_R8 && o[1].reg == 7 && !o[1].pfx) { bb_byte(b, 0xd3); val8(b, o[0].e, 0); return 0; }
    if (o[0].kind == Z_C && o[1].kind == Z_R8 && !o[1].pfx) { bb_byte(b, 0xed); bb_byte(b, 0x41 | o[1].reg << 3); return 0; }
    if (o[0].kind == Z_C && o[1].kind == Z_IMM) { bb_byte(b, 0xed); bb_byte(b, 0x71); return 0; }
    return -2;
  }
  return -1;
}

static ival z80_size(atom *a, section *s, ival pc)
{
  bbuf b = { 0 };
  ival n;
  enc_on = 0; esec = s; epc = pc;
  z80_emit(a->u.insn.insn, &b);
  n = (ival)b.n;
  free(b.d);
  return n;
}

static void z80_encode(atom *a, section *s, ival pc, bbuf *out)
{
  bbuf b = { 0 };
  int r;
  enc_on = 1; esec = s; epc = pc;
  r = z80_emit(a->u.insn.insn, &b);
  if (r == -2) error("illegal operand types");
  bb_put(out, b.d, b.n);
  free(b.d);
}

static int z80_instruction(const char *mn, size_t ml, const char *ext,
                           char **ops, int nops, int col)
{
  zinsn *ip;
  bbuf b = { 0 };
  int r, i;
  atom *a;
  (void)ext; (void)col;
  if (nops > 3) return 0;
  ip = z80_create(mn, ml, ops, nops);
  enc_on = 0; esec = default_section(); epc = 0;
  r = z80_emit(ip, &b);
  free(b.d);
  if (r == -1) return 0;
  for (i = 0; i < nops; i++) if (ip->op[i].kind < 0) r = -2;
  if (r == -2) { error("illegal operand types"); return 1; }
  a = add_atom(AT_INSN);
  a->u.insn.insn = ip;
  return 1;
}

static int z80_option(const char *o)
{
  if (!strcmp(o, "-8080") || !strcmp(o, "-gbz80") || !strcmp(o, "-hd64180") || !strcmp(o, "-intel-syntax") ||
      !strncmp(o, "-rcm", 4))
    { fprintf(stderr, "fatal error: option %s is not supported by gfasm yet\n", o); exit(1); }
  if (!strcmp(o, "-swapixiy") || !strcmp(o, "-z80asm")) return 1;
  return 0;
}

static void z80_define_symbols(void)
{
  set_internal(sym_get("__VASM"), 0);
  sym_get("__VASM")->flags |= SF_INTERNAL;
}

static int z80_init(void) { return 1; }

cpu_module cpu_z80 = {
  "z80",
  z80_init,
  z80_option,
  z80_instruction,
  NULL,
  z80_size,
  z80_encode,
  NULL,
  z80_define_symbols,
  NULL,
  0, 1, 0, 16, 0, NULL,
  0, 0, NULL, NULL
};
