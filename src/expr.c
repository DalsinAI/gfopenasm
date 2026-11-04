/* gfasm - expression parser and evaluator.  MIT License.
 *
 * Semantics follow the vasm manual (operator set and priorities) and
 * black-box observations: comparisons and logical operators yield -1 for
 * true, unary ! yields 1/0, and integer results are wrapped to the 32-bit
 * signed target address range after every operation. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
int char_bs_pair;

int (*syntax_number)(const char **pp, expr **out);
expr *(*syntax_ident)(const char *name, size_t len);
expr *(*syntax_pcexpr)(void);
int expr_unsigned_shift;
int ident_dots;

int is_idstart(int c) { return isalpha(c) || c == '_' || c == '.' ; }
int is_idchar(int c) { return isalnum(c) || c == '_' || (ident_dots && c == '.'); }

expr *new_num(ival v)
{
  expr *e = xcalloc(1, sizeof(*e));
  e->op = X_NUM;
  e->v = v;
  return e;
}
expr *new_flt(long double f)
{
  expr *e = xcalloc(1, sizeof(*e));
  e->op = X_FLT;
  e->f = f;
  return e;
}
expr *new_sym(symbol *s)
{
  expr *e = xcalloc(1, sizeof(*e));
  e->op = X_SYM;
  e->sym = s;
  return e;
}
expr *new_op(int op, expr *l, expr *r)
{
  expr *e = xcalloc(1, sizeof(*e));
  e->op = op;
  e->l = l;
  e->r = r;
  return e;
}
expr *copy_expr(expr *e)
{
  expr *n;
  if (!e) return NULL;
  n = xmalloc(sizeof(*n));
  *n = *e;
  n->l = copy_expr(e->l);
  n->r = copy_expr(e->r);
  return n;
}

static ival wrap32(ival v) { return (ival)(int32_t)(uint32_t)v; }

/* ---------------------------------------------------------- parser */
static const char *ep;
static int perr;

static expr *parse_level(int lvl);

static int parse_charconst(const char **pp, ival *v)
{
  const char *p = *pp;
  char q = *p++;
  uval val = 0;
  int n = 0;
  for (;;) {
    int c = (unsigned char)*p;
    if (!c) return 0;
    if (c == q) {
      if (p[1] == q) { p += 2; val = (val << 8) | (unsigned char)q; n++; continue; }
      p++;
      break;
    }
    if (c == '\\' && !opt_esc && char_bs_pair && p[1] && p[1] != q) {
      /* oldstyle: "\\" is one backslash even without escapes (oracle) */
      p += 2; val = (val << 8) | '\\'; n++; continue;
    }
    if (c == '\\' && opt_esc) {
      p++;
      c = (unsigned char)*p++;
      switch (c) {
      case 'b': c = 8; break;
      case 'f': c = 12; break;
      case 'n': c = 10; break;
      case 'r': c = 13; break;
      case 't': c = 9; break;
      case 'e': c = 27; break;
      case 'x': case 'X': {
        int x = 0;
        while (isxdigit((unsigned char)*p)) {
          int d = *p++;
          x = x * 16 + (isdigit(d) ? d - '0' : (tolower(d) - 'a' + 10));
        }
        c = x & 255;
        break;
      }
      default:
        if (c >= '0' && c <= '7') {
          int x = c - '0';
          while (*p >= '0' && *p <= '7') x = x * 8 + (*p++ - '0');
          c = x & 255;
        }
        break;
      }
      val = (val << 8) | (unsigned)c;
      n++;
      continue;
    }
    val = (val << 8) | (unsigned)c;
    n++;
    p++;
  }
  (void)n;
  *pp = p;
  if (val <= 0xffffffffULL) *v = wrap32((ival)val); else *v = (ival)val;
  return 1;
}

static expr *parse_primary(void)
{
  const char *p = skip_ws(ep);
  expr *e = NULL;
  ep = p;
  switch (*p) {
  case '-':
    ep = p + 1;
    e = parse_primary();
    return e ? new_op(X_NEG, e, NULL) : NULL;
  case '+':
    ep = p + 1;
    return parse_primary();
  case '~':
    ep = p + 1;
    e = parse_primary();
    return e ? new_op(X_CPL, e, NULL) : NULL;
  case '!':
    ep = p + 1;
    e = parse_primary();
    return e ? new_op(X_NOT, e, NULL) : NULL;
  case '(':
    ep = p + 1;
    e = parse_level(0);
    if (!e) return NULL;
    ep = skip_ws(ep);
    if (*ep != ')') { perr = 1; return NULL; }
    ep++;
    return e;
  case '\'': case '"': {
    ival v;
    if (!parse_charconst(&p, &v)) { perr = 1; return NULL; }
    ep = p;
    return new_num(v);
  }
  case '*':
    if (syntax_pcexpr) {
      ep = p + 1;
      return syntax_pcexpr();
    }
    break;
  }
  if (syntax_number) {
    int r = syntax_number(&p, &e);
    if (r < 0) { perr = 1; return NULL; }
    if (r > 0) { ep = p; return e; }
  }
  if (is_idstart((unsigned char)*p) || *p == '@') {
    const char *s = p;
    p++;
    while (is_idchar((unsigned char)*p)) p++;
    if (*p == '$') p++;
    e = syntax_ident(s, p - s);
    if (!e) { perr = 1; return NULL; }
    ep = p;
    return e;
  }
  perr = 1;
  return NULL;
}

/* binary operator at ep at given level; returns op or -1 and length */
static int binop(int lvl, int *len)
{
  const char *p = skip_ws(ep);
  *len = 1;
  switch (lvl) {
  case 1: /* || */
    if (p[0] == '|' && p[1] == '|') { *len = 2; return X_LOR; }
    break;
  case 2:
    if (p[0] == '&' && p[1] == '&') { *len = 2; return X_LAND; }
    break;
  case 3:
    if (p[0] == '=' && p[1] == '=') { *len = 2; return X_EQ; }
    if (p[0] == '!' && p[1] == '=') { *len = 2; return X_NE; }
    if (p[0] == '=') return X_EQ;
    break;
  case 4:
    if (p[0] == '<' && p[1] == '=') { *len = 2; return X_LE; }
    if (p[0] == '>' && p[1] == '=') { *len = 2; return X_GE; }
    if (p[0] == '<' && p[1] != '<' && p[1] != '>') return X_LT;
    if (p[0] == '>' && p[1] != '>') return X_GT;
    break;
  case 5:
    if (p[0] == '+') return X_ADD;
    if (p[0] == '-') return X_SUB;
    break;
  case 6:
    if (p[0] == '*') return X_MUL;
    if (p[0] == '/' && p[1] == '/') { *len = 2; return X_MOD; }
    if (p[0] == '/') return X_DIV;
    if (p[0] == '%') return X_MOD;
    break;
  case 7:
    if (p[0] == '|' && p[1] != '|') return X_OR;
    if (p[0] == '!' && p[1] != '=') return X_OR;
    break;
  case 8:
    if (p[0] == '^') return X_XOR;
    if (p[0] == '~') return X_XOR;
    break;
  case 9:
    if (p[0] == '&' && p[1] != '&') return X_AND;
    break;
  case 10:
    if (p[0] == '<' && p[1] == '<') { *len = 2; return X_SHL; }
    if (p[0] == '>' && p[1] == '>') { *len = 2; return X_SHR; }
    break;
  }
  return -1;
}

static expr *parse_level(int lvl)
{
  expr *l;
  if (lvl > 10) return parse_primary();
  l = parse_level(lvl + 1);
  if (!l) return NULL;
  for (;;) {
    int len, op = binop(lvl, &len);
    expr *r;
    if (op < 0) break;
    ep = skip_ws(ep) + len;
    r = parse_level(lvl + 1);
    if (!r) return NULL;
    l = new_op(op, l, r);
  }
  return l;
}

expr *parse_expr(const char **pp)
{
  expr *e;
  ep = *pp;
  perr = 0;
  e = parse_level(0);
  if (!e || perr) {
    *pp = ep;
    return NULL;
  }
  *pp = ep;
  return e;
}

/* ---------------------------------------------------------- evaluator */
static int evaldepth;

static void set_bad(eres *r) { r->bad = 1; r->sec = NULL; r->ext = NULL; }

int eval_expr(expr *e, eres *r)
{
  eres a, b;
  memset(r, 0, sizeof(*r));
  r->defined = 1;
  if (!e) { r->defined = 0; return 0; }
  switch (e->op) {
  case X_NUM:
    r->v = e->v;
    return 1;
  case X_FLT:
    r->isflt = 1;
    r->f = e->f;
    return 1;
  case X_SYM: {
    symbol *s = e->sym;
    s->flags |= SF_USED;
    switch (s->type) {
    case S_LABEL:
      if (s->sec && (s->sec->flags & SEC_ABS)) {
        r->v = wrap32(s->sec->org + s->val);
      } else if (s->sec && (s->sec->flags & SEC_OFFSET)) {
        r->v = s->sec->org + s->val;
      } else {
        r->v = s->val;
        r->sec = s->sec;
      }
      return 1;
    case S_EXPR: case S_SET:
      if (evaldepth > 200) { r->defined = 0; r->bad = 1; return 0; }
      evaldepth++;
      eval_expr(s->e, r);
      evaldepth--;
      return !r->bad;
    case S_IMPORT: case S_COMMON:
      r->ext = s;
      r->v = 0;
      return 1;
    case S_REG: case S_REGLIST: case S_FREG: case S_FREGLIST:
      r->v = s->val;
      r->bad = 1;
      return 0;
    default:
      r->defined = 0;
      r->ext = s;    /* treat as external for sizing */
      return 1;
    }
  }
  }
  eval_expr(e->l, &a);
  if (e->r) eval_expr(e->r, &b); else { memset(&b, 0, sizeof(b)); b.defined = 1; }
  r->defined = a.defined && b.defined;
  if (a.bad || b.bad) { set_bad(r); return 0; }
  if (a.isflt || b.isflt) {
    long double x = a.isflt ? a.f : (long double)a.v;
    long double y = b.isflt ? b.f : (long double)b.v;
    if (a.sec || a.ext || b.sec || b.ext) { set_bad(r); return 0; }
    r->isflt = 1;
    switch (e->op) {
    case X_NEG: r->f = -x; return 1;
    case X_ADD: r->f = x + y; return 1;
    case X_SUB: r->f = x - y; return 1;
    case X_MUL: r->f = x * y; return 1;
    case X_DIV: r->f = y != 0 ? x / y : 0; return 1;
    default:
      r->isflt = 0;
      a.v = (ival)x; b.v = (ival)y;
      a.isflt = b.isflt = 0;
      break;
    }
  }
  switch (e->op) {
  case X_ADD:
    if ((a.sec || a.ext) && (b.sec || b.ext)) { set_bad(r); return 0; }
    r->v = wrap32(a.v + b.v);
    r->sec = a.sec ? a.sec : b.sec;
    r->ext = a.ext ? a.ext : b.ext;
    return 1;
  case X_SUB:
    if (b.sec || b.ext) {
      if (a.sec && a.sec == b.sec) { r->v = wrap32(a.v - b.v); return 1; }
      if (a.ext && a.ext == b.ext) { r->v = wrap32(a.v - b.v); return 1; }
      if (!a.defined || !b.defined) { r->v = 0; r->defined = 0; return 1; }
      set_bad(r);
      return 0;
    }
    r->v = wrap32(a.v - b.v);
    r->sec = a.sec;
    r->ext = a.ext;
    return 1;
  default:
    break;
  }
  if (a.sec || a.ext || b.sec || b.ext) {
    if (!r->defined) { r->v = 0; return 1; }
    set_bad(r);
    return 0;
  }
  switch (e->op) {
  case X_NEG: r->v = wrap32(-a.v); break;
  case X_CPL: r->v = wrap32(~a.v); break;
  case X_NOT: r->v = a.v == 0; break;
  case X_MUL: r->v = wrap32(a.v * b.v); break;
  case X_DIV:
    if (b.v == 0) { if (r->defined && !diag_suppress) error("division by zero"); r->v = 0; }
    else r->v = wrap32(a.v / b.v);
    break;
  case X_MOD:
    if (b.v == 0) { if (r->defined && !diag_suppress) error("division by zero"); r->v = 0; }
    else r->v = wrap32(a.v % b.v);
    break;
  case X_SHL: r->v = (b.v >= 64 || b.v < 0) ? 0 : wrap32((ival)((uval)a.v << b.v)); break;
  case X_SHR:
    if (expr_unsigned_shift)
      r->v = (b.v >= 32 || b.v < 0) ? 0 : wrap32((ival)((uint32_t)a.v >> b.v));
    else
      r->v = (b.v >= 64 || b.v < 0) ? (a.v < 0 ? -1 : 0) : wrap32(a.v >> b.v);
    break;
  case X_AND: r->v = wrap32(a.v & b.v); break;
  case X_OR:  r->v = wrap32(a.v | b.v); break;
  case X_XOR: r->v = wrap32(a.v ^ b.v); break;
  case X_LT: r->v = a.v < b.v ? -1 : 0; break;
  case X_GT: r->v = a.v > b.v ? -1 : 0; break;
  case X_LE: r->v = a.v <= b.v ? -1 : 0; break;
  case X_GE: r->v = a.v >= b.v ? -1 : 0; break;
  case X_EQ: r->v = a.v == b.v ? -1 : 0; break;
  case X_NE: r->v = a.v != b.v ? -1 : 0; break;
  case X_LAND: r->v = (a.v && b.v) ? -1 : 0; break;
  case X_LOR: r->v = (a.v || b.v) ? -1 : 0; break;
  default: r->v = 0; break;
  }
  return 1;
}

int eval_const(expr *e, ival *v)
{
  eres r;
  eval_expr(e, &r);
  if (r.isflt) { *v = (ival)r.f; return r.defined; }
  *v = r.v;
  return r.defined && !r.bad && !r.sec && !r.ext;
}

int expr_is_float(expr *e)
{
  eres r;
  eval_expr(e, &r);
  return r.isflt;
}

long double eres_float(eres *r)
{
  return r->isflt ? r->f : (long double)r->v;
}

/* Fold constant sub-trees (used to freeze SET values). */
static int has_label(expr *e)
{
  if (!e) return 0;
  if (e->op == X_SYM) return e->sym && e->sym->type == S_LABEL;
  return has_label(e->l) || has_label(e->r);
}

expr *simplify_expr(expr *e)
{
  eres r;
  if (!e) return e;
  if (has_label(e)) return e;     /* label addresses are not final yet */
  eval_expr(e, &r);
  if (r.defined && !r.bad && !r.sec && !r.ext) {
    if (r.isflt) return new_flt(r.f);
    return new_num(r.v);
  }
  return e;
}
