/* gfasm - 6502 cpu module (NMOS 6502, 65C02, WDC 65C02 extensions).
 * MIT License.
 *
 * Encodings from the public MOS 6500 programming manual and the
 * Rockwell/WDC 65C02 data sheets.  vasm behaviour (options, lo/hi
 * modifiers, zero-page optimisation, messages) from the vasm manual's
 * 6502 chapter and comparison with vasm's output; see
 * docs/REFERENCES.md. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

enum { M_IMP, M_ACC, M_IMM, M_ZP, M_ZPX, M_ZPY, M_ABS, M_ABX, M_ABY, M_INX, M_INY, M_IND, M_ZPI, M_AIX,
       M_REL, M_ZPREL, NMODES };
#define C02 1
#define WDC 2

static int cpu_level, opt_branch, bbcade;

typedef struct mop { const char *n; int16_t op[NMODES]; int lvl; } mop;
#define X -1
/*                    imp  acc  imm  zp   zpx  zpy  abs  abx  aby  inx  iny  ind  zpi  aix  rel  zprel */
static const mop tab[] = {
  { "adc", { X, X, 0x69, 0x65, 0x75, X, 0x6d, 0x7d, 0x79, 0x61, 0x71, X, 0x72, X, X, X }, 0 },
  { "and", { X, X, 0x29, 0x25, 0x35, X, 0x2d, 0x3d, 0x39, 0x21, 0x31, X, 0x32, X, X, X }, 0 },
  { "asl", { 0x0a, 0x0a, X, 0x06, 0x16, X, 0x0e, 0x1e, X, X, X, X, X, X, X, X }, 0 },
  { "bit", { X, X, 0x89, 0x24, 0x34, X, 0x2c, 0x3c, X, X, X, X, X, X, X, X }, 0 },
  { "brk", { 0x00, X, 0x00, X, X, X, X, X, X, X, X, X, X, X, X, X }, 0 },
  { "cmp", { X, X, 0xc9, 0xc5, 0xd5, X, 0xcd, 0xdd, 0xd9, 0xc1, 0xd1, X, 0xd2, X, X, X }, 0 },
  { "cpx", { X, X, 0xe0, 0xe4, X, X, 0xec, X, X, X, X, X, X, X, X, X }, 0 },
  { "cpy", { X, X, 0xc0, 0xc4, X, X, 0xcc, X, X, X, X, X, X, X, X, X }, 0 },
  { "dec", { 0x3a, 0x3a, X, 0xc6, 0xd6, X, 0xce, 0xde, X, X, X, X, X, X, X, X }, 0 },
  { "eor", { X, X, 0x49, 0x45, 0x55, X, 0x4d, 0x5d, 0x59, 0x41, 0x51, X, 0x52, X, X, X }, 0 },
  { "inc", { 0x1a, 0x1a, X, 0xe6, 0xf6, X, 0xee, 0xfe, X, X, X, X, X, X, X, X }, 0 },
  { "jmp", { X, X, X, X, X, X, 0x4c, X, X, X, X, 0x6c, X, 0x7c, X, X }, 0 },
  { "jsr", { X, X, X, X, X, X, 0x20, X, X, X, X, X, X, X, X, X }, 0 },
  { "lda", { X, X, 0xa9, 0xa5, 0xb5, X, 0xad, 0xbd, 0xb9, 0xa1, 0xb1, X, 0xb2, X, X, X }, 0 },
  { "ldx", { X, X, 0xa2, 0xa6, X, 0xb6, 0xae, X, 0xbe, X, X, X, X, X, X, X }, 0 },
  { "ldy", { X, X, 0xa0, 0xa4, 0xb4, X, 0xac, 0xbc, X, X, X, X, X, X, X, X }, 0 },
  { "lsr", { 0x4a, 0x4a, X, 0x46, 0x56, X, 0x4e, 0x5e, X, X, X, X, X, X, X, X }, 0 },
  { "ora", { X, X, 0x09, 0x05, 0x15, X, 0x0d, 0x1d, 0x19, 0x01, 0x11, X, 0x12, X, X, X }, 0 },
  { "rol", { 0x2a, 0x2a, X, 0x26, 0x36, X, 0x2e, 0x3e, X, X, X, X, X, X, X, X }, 0 },
  { "ror", { 0x6a, 0x6a, X, 0x66, 0x76, X, 0x6e, 0x7e, X, X, X, X, X, X, X, X }, 0 },
  { "sbc", { X, X, 0xe9, 0xe5, 0xf5, X, 0xed, 0xfd, 0xf9, 0xe1, 0xf1, X, 0xf2, X, X, X }, 0 },
  { "sta", { X, X, X, 0x85, 0x95, X, 0x8d, 0x9d, 0x99, 0x81, 0x91, X, 0x92, X, X, X }, 0 },
  { "stx", { X, X, X, 0x86, X, 0x96, 0x8e, X, X, X, X, X, X, X, X, X }, 0 },
  { "sty", { X, X, X, 0x84, 0x94, X, 0x8c, X, X, X, X, X, X, X, X, X }, 0 },
  { "stz", { X, X, X, 0x64, 0x74, X, 0x9c, 0x9e, X, X, X, X, X, X, X, X }, C02 },
  { "trb", { X, X, X, 0x14, X, X, 0x1c, X, X, X, X, X, X, X, X, X }, C02 },
  { "tsb", { X, X, X, 0x04, X, X, 0x0c, X, X, X, X, X, X, X, X, X }, C02 },
  { "bcc", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0x90, X }, 0 },
  { "bcs", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0xb0, X }, 0 },
  { "beq", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0xf0, X }, 0 },
  { "bmi", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0x30, X }, 0 },
  { "bne", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0xd0, X }, 0 },
  { "bpl", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0x10, X }, 0 },
  { "bvc", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0x50, X }, 0 },
  { "bvs", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0x70, X }, 0 },
  { "bra", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0x80, X }, C02 },
  { "blt", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0x90, X }, 0 },
  { "bge", { X, X, X, X, X, X, X, X, X, X, X, X, X, X, 0xb0, X }, 0 },
  { NULL, { 0 }, 0 }
};
/* implied */
static const struct { const char *n; uint8_t op; int lvl; } imp[] = {
  { "clc", 0x18, 0 }, { "cld", 0xd8, 0 }, { "cli", 0x58, 0 }, { "clv", 0xb8, 0 }, { "dex", 0xca, 0 },
  { "dey", 0x88, 0 }, { "inx", 0xe8, 0 }, { "iny", 0xc8, 0 }, { "nop", 0xea, 0 }, { "pha", 0x48, 0 },
  { "php", 0x08, 0 }, { "pla", 0x68, 0 }, { "plp", 0x28, 0 }, { "rti", 0x40, 0 }, { "rts", 0x60, 0 },
  { "sec", 0x38, 0 }, { "sed", 0xf8, 0 }, { "sei", 0x78, 0 }, { "tax", 0xaa, 0 }, { "tay", 0xa8, 0 },
  { "tsx", 0xba, 0 }, { "txa", 0x8a, 0 }, { "txs", 0x9a, 0 }, { "tya", 0x98, 0 },
  { "phx", 0xda, C02 }, { "phy", 0x5a, C02 }, { "plx", 0xfa, C02 }, { "ply", 0x7a, C02 },
  { "stp", 0xdb, WDC }, { "wai", 0xcb, WDC },
  { NULL, 0, 0 } };

/* modes that the 65C02 adds to NMOS instructions */
static int c02_mode(const mop *m, int md)
{
  if (md == M_ZPI || md == M_AIX) return 1;
  if ((md == M_ACC || md == M_IMP) && (!strcmp(m->n, "inc") || !strcmp(m->n, "dec"))) return 1;
  if (!strcmp(m->n, "bit") && (md == M_IMM || md == M_ZPX || md == M_ABX)) return 1;
  return 0;
}

typedef struct minsn {
  const mop *m;
  int impop;             /* implied opcode, -1 */
  int wdc;               /* 1 rmb/smb, 2 bbr/bbs */
  int bitop;             /* base opcode for wdc forms */
  int kind;              /* parsed: 0 none, 1 acc, 2 imm, 3 ind-x, 4 ind-y, 5 ind, 6 direct (with ,x/,y), 7 bit,zp(,rel) */
  int index;             /* 0, 'x', 'y' */
  int force;             /* 0, '<' zp, '>' abs */
  int lohi;              /* immediate: 0, '<', '>' */
  expr *e, *e2;
} minsn;

static expr *lohi_wrap(expr *e, int lh)
{
  if (lh == '<') return new_op(X_AND, e, new_num(255));
  if (lh == '>') return new_op(X_AND, new_op(X_SHR, e, new_num(8)), new_num(255));
  return e;
}

static expr *mexpr(const char *s, int *mod)
{
  const char *p = skip_ws(s);
  expr *e;
  *mod = 0;
  if (*p == '<' || *p == '>' || *p == '!' || *p == '|') {
    *mod = *p == '<' ? '<' : '>';
    if (bbcade) *mod = *mod == '<' ? '>' : '<';
    p++;
  }
  e = parse_expr(&p);
  if (!e || *skip_ws(p)) return NULL;
  return e;
}

static int parse_operands(minsn *ip, char **ops, int nops)
{
  const char *p;
  size_t n;
  if (nops == 0) { ip->kind = 0; return 1; }
  p = skip_ws(ops[0]);
  n = strlen(p);
  while (n && is_ws(p[n - 1])) n--;
  if (nops == 1 && n == 1 && tolower((unsigned char)*p) == 'a' && (ip->m && (ip->m->op[M_ACC] >= 0))) { ip->kind = 1; return 1; }
  if (*p == '#') {
    if (nops != 1) return 0;
    ip->kind = 2;
    ip->e = mexpr(p + 1, &ip->lohi);
    if (!ip->e) return 0;
    ip->e = lohi_wrap(ip->e, ip->lohi);
    return 1;
  }
  if (*p == '(' && nops <= 2) {
    char *in;
    if (nops == 2) {
      /* (zp),y  */
      if (p[n - 1] != ')' || stricmp_(skip_ws(ops[1]), "y")) return 0;
      in = xstrndup(p + 1, n - 2);
      ip->kind = 4;
    } else {
      size_t k = n;
      int depth = 0, whole = 1;
      size_t i;
      if (p[n - 1] != ')') goto direct;
      for (i = 0; i < n; i++) {
        if (p[i] == '(') depth++;
        else if (p[i] == ')') { depth--; if (!depth && i != n - 1) whole = 0; }
      }
      if (!whole) goto direct;
      in = xstrndup(p + 1, n - 2);
      k = strlen(in);
      while (k && is_ws(in[k - 1])) k--;
      if (k > 2 && in[k - 2] == ',' && tolower((unsigned char)in[k - 1]) == 'x') { in[k - 2] = 0; ip->kind = 3; }
      else ip->kind = 5;
    }
    ip->e = mexpr(in, &ip->force);
    free(in);
    return ip->e != NULL;
  }
direct:
  ip->kind = 6;
  if (ip->m && ip->m->op[M_REL] >= 0) {
    /* branches: no lo/hi modifiers ("!x" is a logical not) */
    const char *q = skip_ws(ops[0]);
    ip->e = parse_expr(&q);
    if (!ip->e || *skip_ws(q)) return 0;
  } else
    ip->e = mexpr(ops[0], &ip->force);
  if (!ip->e) return 0;
  if (nops == 2) {
    const char *r = skip_ws(ops[1]);
    if (!stricmp_(r, "x")) ip->index = 'x';
    else if (!stricmp_(r, "y")) ip->index = 'y';
    else return 0;
  } else if (nops > 2) return 0;
  return 1;
}

static int is_zp(minsn *ip, ival *v)
{
  eres r;
  eval_expr(ip->e, &r);
  *v = r.v;
  if (ip->force == '<') return 1;
  if (ip->force == '>') return 0;
  return r.defined && !r.sec && !r.ext && r.v >= 0 && r.v <= 255;
}

static int enc_on;
static section *esec;
static ival epc;

static void v8(bbuf *b, expr *e)
{
  eres r;
  eval_expr(e, &r);
  if (enc_on && (r.sec || r.ext)) add_reloc(esec, epc + (ival)b->n, 8, R_ABS, r.sec, r.ext, r.v);
  else if (enc_on && r.defined && (r.v < -128 || r.v > 255)) warning("operand doesn't fit into 8 bits");
  bb_byte(b, (int)r.v);
}
static void v16(bbuf *b, expr *e)
{
  eres r;
  eval_expr(e, &r);
  if (enc_on && (r.sec || r.ext)) add_reloc(esec, epc + (ival)b->n, 16, R_ABS, r.sec, r.ext, r.v);
  bb_byte(b, (int)r.v);
  bb_byte(b, (int)(r.v >> 8));
}
static void rel(bbuf *b, expr *e, ival base)
{
  eres r;
  ival d;
  eval_expr(e, &r);
  if (!r.sec && (esec->flags & SEC_ABS)) d = r.v - (esec->org + base);
  else d = r.v - base;
  if (enc_on && (r.ext || (r.sec && r.sec != esec))) error("branch destination out of range");
  else if (enc_on && (d < -128 || d > 127)) error("branch destination out of range");
  bb_byte(b, (int)d);
}

/* returns 0 ok, -2 bad operands */
static int m_emit(minsn *ip, bbuf *b)
{
  const mop *m = ip->m;
  ival v;
  int md, op;
  if (ip->impop >= 0) { if (ip->kind) return -2; bb_byte(b, ip->impop); return 0; }
  if (ip->wdc) {
    eres r;
    if (ip->kind != 6 || ip->index) return -2;
    bb_byte(b, ip->bitop);
    eval_expr(ip->e, &r);
    bb_byte(b, (int)r.v);
    if (ip->wdc == 2) rel(b, ip->e2, epc + 3);
    return 0;
  }
  switch (ip->kind) {
  case 0: md = m->op[M_IMP] >= 0 ? M_IMP : M_ACC; break;
  case 1: md = M_ACC; break;
  case 2: md = M_IMM; break;
  case 3: md = m->op[M_AIX] >= 0 ? M_AIX : M_INX; break;
  case 4: md = M_INY; break;
  case 5: md = m->op[M_IND] >= 0 ? M_IND : M_ZPI; break;
  default:
    if (m->op[M_REL] >= 0) { if (ip->index) return -2; md = M_REL; break; }
    if (is_zp(ip, &v)) {
      md = ip->index == 'x' ? M_ZPX : ip->index == 'y' ? M_ZPY : M_ZP;
      if (m->op[md] < 0 || (!(cpu_level & C02) && c02_mode(m, md))) {
        if (ip->force == '<') return -2;
        md = ip->index == 'x' ? M_ABX : ip->index == 'y' ? M_ABY : M_ABS;
      }
    } else
      md = ip->index == 'x' ? M_ABX : ip->index == 'y' ? M_ABY : M_ABS;
  }
  op = m->op[md];
  if (op < 0 || (!(cpu_level & C02) && c02_mode(m, md))) return -2;
  if (!strcmp(m->n, "brk") && md == M_IMM) { bb_byte(b, 0); v8(b, ip->e); return 0; }
  bb_byte(b, op);
  switch (md) {
  case M_IMP: case M_ACC: break;
  case M_IMM: case M_ZP: case M_ZPX: case M_ZPY: case M_INX: case M_INY: case M_ZPI:
    if (md != M_IMM) {
      eres r;
      eval_expr(ip->e, &r);
      if (enc_on && !r.sec && !r.ext && (r.v < 0 || r.v > 255) && ip->force != '<')
        error("operand not in zero/direct-page range");
      bb_byte(b, (int)r.v);
    } else v8(b, ip->e);
    break;
  case M_REL: rel(b, ip->e, epc + 2); break;
  default: v16(b, ip->e); break;
  }
  return 0;
}

static ival m_size(atom *a, section *s, ival pc)
{
  bbuf b = { 0 };
  ival n;
  enc_on = 0; esec = s; epc = pc;
  m_emit(a->u.insn.insn, &b);
  n = (ival)b.n;
  free(b.d);
  if (grow_only && n < a->size) n = a->size;
  return n;
}

static void m_encode(atom *a, section *s, ival pc, bbuf *out)
{
  bbuf b = { 0 };
  enc_on = 1; esec = s; epc = pc;
  if (m_emit(a->u.insn.insn, &b) < 0) error("illegal operand types");
  bb_put(out, b.d, b.n);
  free(b.d);
}

static int m_instruction(const char *mn, size_t ml, const char *ext, char **ops, int nops, int col)
{
  char name[16];
  minsn *ip;
  size_t i;
  atom *a;
  (void)ext; (void)col;
  if (ml >= sizeof name) return 0;
  for (i = 0; i < ml; i++) name[i] = (char)tolower((unsigned char)mn[i]);
  name[ml] = 0;
  ip = xcalloc(1, sizeof(*ip));
  ip->impop = -1;
  for (i = 0; imp[i].n; i++)
    if (!strcmp(name, imp[i].n)) {
      if (imp[i].lvl & ~cpu_level) { error("instruction not supported on selected architecture"); return 1; }
      ip->impop = imp[i].op;
      break;
    }
  if (ip->impop < 0) {
    for (i = 0; tab[i].n; i++) if (!strcmp(name, tab[i].n)) { ip->m = &tab[i]; break; }
    if (!ip->m) {
      /* WDC rmbN/smbN/bbrN/bbsN, also "rmb N,zp" */
      if (ml >= 3 && (!strncmp(name, "rmb", 3) || !strncmp(name, "smb", 3) || !strncmp(name, "bbr", 3) || !strncmp(name, "bbs", 3))) {
        int bitn;
        ival bv;
        if (!(cpu_level & WDC)) { error("instruction not supported on selected architecture"); return 1; }
        ip->wdc = name[1] == 'm' ? 1 : 2;
        if (ml == 4 && name[3] >= '0' && name[3] <= '7') bitn = name[3] - '0';
        else if (ml == 3 && nops >= 1) {
          const char *bp = ops[0];
          expr *be = parse_expr(&bp);
          if (!be || !eval_const(be, &bv) || bv < 0 || bv > 7) { error("illegal bit number"); return 1; }
          bitn = (int)bv;
          ops++; nops--;
        } else return 0;
        ip->bitop = (name[0] == 'r' ? 0x07 : name[0] == 's' && name[1] == 'm' ? 0x87 : name[2] == 'r' ? 0x0f : 0x8f) + bitn * 16;
        if (nops != (ip->wdc == 2 ? 2 : 1)) { error("illegal operand types"); return 1; }
        ip->kind = 6;
        ip->e = mexpr(ops[0], &ip->force);
        if (ip->wdc == 2) { int f; ip->e2 = mexpr(ops[1], &f); }
        if (!ip->e || (ip->wdc == 2 && !ip->e2)) { error("illegal operand types"); return 1; }
        goto add;
      }
      return 0;
    }
    if ((ip->m->lvl & ~cpu_level)) { error("instruction not supported on selected architecture"); return 1; }
  }
  if (!parse_operands(ip, ops, nops)) { error("illegal operand types"); return 1; }
  {
    bbuf b = { 0 };
    int r;
    enc_on = 0; esec = default_section(); epc = 0;
    r = m_emit(ip, &b);
    free(b.d);
    if (r < 0 && ip->e && ip->kind == 6) {
      eres er;
      eval_expr(ip->e, &er);
      if (!er.defined) r = 0;    /* may become zero page later */
    }
    if (r < 0) {
      error(cpu_level & C02 ? "illegal operand types" : "instruction not supported on selected architecture");
      return 1;
    }
  }
add:
  a = add_atom(AT_INSN);
  a->u.insn.insn = ip;
  return 1;
}

static int m_option(const char *o)
{
  if (!strcmp(o, "-c02")) { cpu_level |= C02; return 1; }
  if (!strcmp(o, "-wdc02")) { cpu_level |= C02 | WDC; return 1; }
  if (!strcmp(o, "-opt-branch")) { opt_branch = 1; return 1; }
  if (!strcmp(o, "-bbcade")) { bbcade = 1; return 1; }
  if (!strcmp(o, "-illegal") || !strcmp(o, "-dtv") || !strcmp(o, "-ce02") || !strcmp(o, "-6280") || !strcmp(o, "-mega65"))
    { fprintf(stderr, "fatal error: option %s is not supported by gfasm yet\n", o); exit(1); }
  return 0;
}

static void m_define_symbols(void)
{
  set_internal(sym_get("__VASM"), 0);
  sym_get("__VASM")->flags |= SF_INTERNAL;
}

static int m_init(void) { return 1; }

cpu_module cpu_6502 = {
  "6502",
  m_init,
  m_option,
  m_instruction,
  NULL,
  m_size,
  m_encode,
  NULL,
  m_define_symbols,
  NULL,
  0, 1, 0, 16, 0, NULL,
  0, 0, NULL, NULL
};
