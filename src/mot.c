/* gfasm - Motorola syntax module (vasm "mot" compatible).  MIT License.
 *
 * Written from the vasm manual's description of the mot syntax module
 * (labels, comments, numbers, directives) and black-box comparison. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

extern int ident_dots;
void encode_float(int ftype, long double f, uint8_t *o);

int opt_esc, opt_spaces, opt_align, opt_devpac, opt_phxass, opt_localu;
int opt_warncomm;
int list_on = 1;
uint16_t cnop_code = 0x4e71;

static char *last_global;         /* scope for local labels */
static int inline_id, inline_depth;
static int inline_stack[64];
static symbol *line_anchor;       /* symbol for '*' on this statement */
static symbol *sym_rs, *sym_so, *sym_fo;

/* section stack */
static section *secstack[64];
static int nsecstack;

/* macro / rept collection */
static macro *defmacro;
static int defmacro_nest;
static char **rept_lines; static int rept_n, rept_cap, rept_nest, rept_count;
static int collecting_rept;
static int in_rem;
static char *irp_name; static char **irp_vals; static int irp_nvals; static int rept_kind;

/* ------------------------------------------------------------ numbers */
static int mot_number(const char **pp, expr **out)
{
  const char *p = *pp;
  uval v = 0;
  int digits = 0;
  if (*p == '$') {
    p++;
    while (isxdigit((unsigned char)*p)) {
      int c = tolower((unsigned char)*p++);
      v = v * 16 + (isdigit(c) ? c - '0' : c - 'a' + 10);
      digits++;
    }
    if (!digits) return 0;
  } else if (*p == '%') {
    p++;
    while (*p == '0' || *p == '1') { v = v * 2 + (*p++ - '0'); digits++; }
    if (!digits) return 0;
  } else if (*p == '@' && p[1] >= '0' && p[1] <= '7') {
    p++;
    while (*p >= '0' && *p <= '7') { v = v * 8 + (*p++ - '0'); digits++; }
  } else if (*p == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
    p += 2;
    while (isxdigit((unsigned char)*p)) {
      int c = tolower((unsigned char)*p++);
      v = v * 16 + (isdigit(c) ? c - '0' : c - 'a' + 10);
    }
  } else if (isdigit((unsigned char)*p)) {
    const char *s = p;
    while (isdigit((unsigned char)*p)) { v = v * 10 + (*p++ - '0'); }
    if ((*p == '.' && isdigit((unsigned char)p[1])) ||
        ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) ||
         ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2]))))) {
      char *end;
      long double f = (long double)strtod(s, &end);
      *pp = end;
      *out = new_flt(f);
      return 1;
    }
  } else
    return 0;
  *pp = p;
  if (v <= 0xffffffffULL) *out = new_num((ival)(int32_t)(uint32_t)v);
  else *out = new_num((ival)v);
  return 1;
}

/* ------------------------------------------------------------ symbols */
static int is_local_name(const char *n, size_t len)
{
  if (opt_localu) { if (n[0] == '_') return 1; }
  else if (n[0] == '.') return 1;
  return len > 1 && n[len - 1] == '$';
}

static char *mangle(const char *n, size_t len)
{
  if (is_local_name(n, len)) {
    char scope[32];
    if (inline_depth) snprintf(scope, sizeof(scope), " inl%d", inline_stack[inline_depth - 1]);
    else scope[0] = 0;
    return strprintf("%s%s\x1f%.*s", last_global ? last_global : "", scope, (int)len, n);
  }
  return xstrndup(n, len);
}

static symbol *get_symbol(const char *n, size_t len)
{
  char *m = mangle(n, len);
  symbol *s = sym_lookup(m);
  if (!s) {
    s = sym_get(m);
    if (is_local_name(n, len)) {
      s->flags |= SF_LOCAL;
      s->dispname = xstrndup(n, len);
    }
  }
  free(m);
  return s;
}

static expr *mot_ident(const char *n, size_t len)
{
  symbol *s = get_symbol(n, len);
  s->flags |= SF_REFD;
  if (s->type == S_SET && s->e) {
    s->flags |= SF_USED;
    return copy_expr(s->e);
  }
  if (s->type == S_REG || s->type == S_REGLIST || s->type == S_FREG || s->type == S_FREGLIST)
    return NULL;
  return new_sym(s);
}

static section *stmt_sec;
static atom *stmt_prev;

static expr *mot_pc(void)
{
  if (!line_anchor) {
    section *s = default_section();
    atom *a;
    line_anchor = sym_new_internal();
    if (s != stmt_sec) { add_label_atom(line_anchor); return new_sym(line_anchor); }
    a = xcalloc(1, sizeof(*a));
    a->type = AT_LABEL;
    a->pos = curpos;
    a->u.label.sym = line_anchor;
    if (stmt_prev) { a->next = stmt_prev->next; stmt_prev->next = a; }
    else { a->next = s->first; s->first = a; }
    if (s->last == stmt_prev) s->last = a;
    line_anchor->type = S_LABEL;
    line_anchor->sec = s;
    line_anchor->atom = a;
    line_anchor->flags |= SF_DEFINED;
  }
  return new_sym(line_anchor);
}

/* -------------------------------------------------- operand splitting */
static int op_end(const char *p)
{
  /* returns length of the operand field */
  const char *s = p;
  char q = 0;
  int par = 0;
  while (*p) {
    if (q) {
      if (*p == q) {
        if (p[1] == q) { p += 2; continue; }
        q = 0;
      }
      p++;
      continue;
    }
    if (*p == '\'' || *p == '"') {
      /* quotes only start a string at operand item starts or after # */
      q = *p++;
      continue;
    }
    if (*p == ';') break;
    if (*p == '(' || *p == '[' || *p == '{') par++;
    if (*p == ')' || *p == ']' || *p == '}') par--;
    if (is_ws(*p)) {
      if (!opt_spaces) break;
      /* with -spaces: blanks are allowed, comments must use ';' */
    }
    p++;
  }
  while (p > s && is_ws(p[-1])) p--;
  return (int)(p - s);
}

/* split at top-level commas */
static int split_ops(const char *s, int len, char ***out)
{
  char **v = NULL;
  int n = 0, par = 0;
  char q = 0;
  const char *start = s, *p = s, *e = s + len;
  if (len == 0) { *out = NULL; return 0; }
  while (p <= e) {
    if (p == e || (!q && par == 0 && *p == ',')) {
      const char *a = start, *b = p;
      while (a < b && is_ws(*a)) a++;
      while (b > a && is_ws(b[-1])) b--;
      v = xrealloc(v, (n + 1) * sizeof(char *));
      v[n++] = xstrndup(a, b - a);
      start = p + 1;
      p++;
      continue;
    }
    if (q) {
      if (*p == q) { if (p + 1 < e && p[1] == q) p++; else q = 0; }
    } else if (*p == '\'' || *p == '"') q = *p;
    else if (*p == '(' || *p == '[' || *p == '{') par++;
    else if (*p == ')' || *p == ']' || *p == '}') par--;
    p++;
  }
  *out = v;
  return n;
}

static void free_ops(char **v, int n)
{
  int i;
  for (i = 0; i < n; i++) free(v[i]);
  free(v);
}

/* macro arguments: like split_ops, but <...> groups */
static int split_macro_args(const char *s, int len, char ***out)
{
  char **v = NULL;
  int n = 0;
  const char *p = s, *e = s + len;
  if (len == 0) { *out = NULL; return 0; }
  for (;;) {
    const char *a = p;
    char *arg;
    if (p < e && *p == '<') {
      const char *q = p + 1;
      while (q < e && *q != '>') q++;
      arg = xstrndup(p + 1, q - p - 1);
      p = q < e ? q + 1 : q;
      while (p < e && *p != ',') p++;
    } else {
      int par = 0; char qq = 0;
      while (p < e) {
        if (qq) { if (*p == qq) { if (p + 1 < e && p[1] == qq) p++; else qq = 0; } }
        else if (*p == '\'' || *p == '"') qq = *p;
        else if (*p == '(' || *p == '[') par++;
        else if (*p == ')' || *p == ']') par--;
        else if (*p == ',' && par <= 0) break;
        p++;
      }
      arg = xstrndup(a, p - a);
    }
    v = xrealloc(v, (n + 1) * sizeof(char *));
    v[n++] = arg;
    if (p >= e) break;
    p++;    /* skip comma */
  }
  *out = v;
  return n;
}

/* ------------------------------------------------------------- helpers */
static expr *parse_full_expr(const char *s)
{
  const char *p = s;
  expr *e;
  p = skip_ws(p);
  e = parse_expr(&p);
  if (!e) { error("expression syntax error"); return NULL; }
  p = skip_ws(p);
  if (*p) error("garbage at end of line");
  return e;
}

static int const_expr(const char *s, ival *v)
{
  expr *e = parse_full_expr(s);
  if (!e) { *v = 0; return 0; }
  if (!eval_const(e, v)) {
    error("expression must be a constant");
    return 0;
  }
  return 1;
}

static char *unquote(const char *s)
{
  size_t n = strlen(s);
  s = skip_ws(s);
  n = strlen(s);
  while (n && is_ws(s[n - 1])) n--;
  if (n >= 2 && (s[0] == '"' || s[0] == '\'') && s[n - 1] == s[0])
    return xstrndup(s + 1, n - 2);
  if (n >= 2 && s[0] == '<' && s[n - 1] == '>')
    return xstrndup(s + 1, n - 2);
  return xstrndup(s, n);
}

static void define_label_here(const char *name, size_t len, int exportit)
{
  symbol *s;
  if (!is_local_name(name, len)) {
    free(last_global);
    last_global = xstrndup(name, len);
  }
  s = get_symbol(name, len);
  if (s->type != S_NONE && s->type != S_IMPORT) {
    if (!(s->type == S_LABEL && s->flags & SF_INTERNAL))
      error("symbol <%.*s> redefined", (int)len, name);
    return;
  }
  if (s->type == S_IMPORT) s->flags |= SF_EXPORT;
  add_label_atom(s);
  if (exportit) s->flags |= SF_EXPORT;
}

static symbol *define_equ(const char *name, size_t len, expr *e, int settype)
{
  symbol *s = get_symbol(name, len);
  if (settype) {
    if (s->type != S_NONE && s->type != S_SET) {
      error("symbol <%.*s> redefined", (int)len, name);
      return s;
    }
    s->type = S_SET;
    s->e = simplify_expr(e);
  } else {
    if (s->type != S_NONE && s->type != S_IMPORT) {
      error("symbol <%.*s> redefined", (int)len, name);
      return s;
    }
    if (s->type == S_IMPORT) s->flags |= SF_EXPORT;
    s->type = S_EXPR;
    s->e = e;
  }
  s->flags |= SF_DEFINED;
  return s;
}

static int sizeof_ext(int c)
{
  switch (tolower(c)) {
  case 'b': return 1;
  case 'w': return 2;
  case 'l': return 4;
  case 'q': return 8;
  case 's': return 4;
  case 'd': return 8;
  case 'x': return 12;
  case 'p': return 12;
  }
  return 0;
}

/* --------------------------------------------------------- data atoms */
static void do_data(int esize, int ftype, char **ops, int nops, int reldiff)
{
  atom *a;
  int i;
  ival total = 0;
  if (opt_align && esize > 1) {
    atom *al = add_atom(AT_ALIGN);
    al->u.align.align = 2;
  }
  if (!nops) { error("missing operand"); return; }
  a = add_atom(AT_DATA);
  a->u.data.esize = esize;
  a->u.data.ftype = ftype;
  a->u.data.reldiff = reldiff;
  a->u.data.items = xcalloc(nops, sizeof(dataitem));
  for (i = 0; i < nops; i++) {
    char *o = ops[i];
    size_t l = strlen(o);
    dataitem *it = &a->u.data.items[a->u.data.n];
    if (!ftype && l >= 2 && (o[0] == '"' || o[0] == '\'') && o[l - 1] == o[0]) {
      /* a pure string operand */
      char q = o[0];
      const char *p = o + 1;
      int closed_at_end = 1;
      char *buf = xmalloc(l);
      int n = 0;
      while (*p) {
        if (*p == q) {
          if (p[1] == q) { buf[n++] = q; p += 2; continue; }
          if (p[1]) closed_at_end = 0;
          break;
        }
        if (*p == '\\' && opt_esc && p[1]) {
          int c;
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
              int d = tolower((unsigned char)*p++);
              x = x * 16 + (isdigit(d) ? d - '0' : d - 'a' + 10);
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
          }
          buf[n++] = (char)c;
          continue;
        }
        buf[n++] = *p++;
      }
      if (closed_at_end && (esize == 1 || n > esize)) {
        it->str = buf;
        it->slen = n;
        a->u.data.n++;
        total += ((n + esize - 1) / esize) * esize;
        continue;
      }
      free(buf);
    }
    {
      const char *p = o;
      it->e = parse_expr(&p);
      p = skip_ws(p);
      if (!it->e) { error("invalid data operand"); it->e = new_num(0); }
      else if (*p) error("garbage at end of line");
      a->u.data.n++;
      total += esize;
    }
  }
  a->size = total;
}

static void do_space(int esize, int ftype, const char *count, const char *fill, int uninit)
{
  atom *a;
  if (opt_align && esize > 1) {
    atom *al = add_atom(AT_ALIGN);
    al->u.align.align = 2;
  }
  a = add_atom(AT_SPACE);
  a->u.space.count = parse_full_expr(count);
  if (!a->u.space.count) a->u.space.count = new_num(0);
  a->u.space.esize = esize;
  a->u.space.ftype = ftype;
  a->u.space.uninit = uninit;
  if (fill) a->u.space.fill = parse_full_expr(fill);
}

static void do_align(ival align, ival offset, int nop)
{
  atom *a = add_atom(AT_ALIGN);
  a->u.align.align = align;
  a->u.align.offset = offset;
  a->u.align.nopfill = nop ? cnop_code : 0;
}

/* --------------------------------------------------------- sections */
static int parse_sectype(const char *t, int *type, uint32_t *mem)
{
  size_t l = strlen(t);
  char *s = xstrdup(t);
  int ok = 1;
  *mem = 0;
  if (l > 2 && s[l - 2] == '_') {
    int c = tolower((unsigned char)s[l - 1]);
    if (c == 'c') *mem = 0x40000000;
    else if (c == 'f') *mem = 0x80000000;
    else if (c == 'p') *mem = 0;
    else ok = 0;
    s[l - 2] = 0;
  }
  if (!stricmp_(s, "code") || !stricmp_(s, "text")) *type = ST_CODE;
  else if (!stricmp_(s, "data")) *type = ST_DATA;
  else if (!stricmp_(s, "bss")) *type = ST_BSS;
  else ok = 0;
  free(s);
  return ok;
}

static void switch_section(const char *name, int type, uint32_t mem)
{
  section *s = find_section(name, type, mem);
  if (!s) s = new_section(name, type, mem);
  cursec = s;
}

static void do_section(char **ops, int nops)
{
  char *name;
  int type = ST_CODE;
  uint32_t mem = 0;
  if (nops < 1) { error("missing operand"); return; }
  name = unquote(ops[0]);
  if (nops >= 2) {
    if (!parse_sectype(ops[1], &type, &mem)) {
      error("illegal section type");
      free(name);
      return;
    }
  } else if (!outmod->named_sections) {
    uint32_t m2;
    if (!parse_sectype(name, &type, &m2)) type = ST_CODE;
    else mem = m2;
  }
  if (nops >= 3) {
    if (!stricmp_(ops[2], "chip")) mem = 0x40000000;
    else if (!stricmp_(ops[2], "fast")) mem = 0x80000000;
    else {
      ival v;
      if (const_expr(ops[2], &v)) mem = (uint32_t)v;
      else error("illegal memory type");
    }
  }
  switch_section(name, type, mem);
  free(name);
}

/* --------------------------------------------------------- xdef etc */
static void do_xdef(char **ops, int nops, int kind)
{
  int i;
  for (i = 0; i < nops; i++) {
    const char *n = ops[i];
    size_t l = strlen(n);
    symbol *s;
    if (!l) continue;
    s = get_symbol(n, l);
    switch (kind) {
    case 0: /* xdef */
      s->flags |= SF_EXPORT;
      break;
    case 1: /* xref */
      if (s->type == S_NONE) s->type = S_IMPORT;
      s->flags |= SF_XREF;
      break;
    case 2: /* public */
      s->flags |= SF_EXPORT;
      if (s->type == S_NONE) s->flags |= SF_XREF;
      break;
    case 3: /* weak */
      s->flags |= SF_EXPORT | SF_WEAK;
      break;
    }
  }
}

/* ------------------------------------------------------- conditionals */
static int is_cond_dir(const char *m, size_t l)
{
  static const char *names[] = { "if", "ifeq", "ifne", "ifgt", "ifge", "iflt",
    "ifle", "ifb", "ifnb", "ifc", "ifnc", "ifd", "ifnd", "ifmacrod",
    "ifmacrond", "if1", "if2", "ifp1", "ifp2", NULL };
  int i;
  for (i = 0; names[i]; i++)
    if (strlen(names[i]) == l && !strnicmp_(m, names[i], l)) return 1;
  return 0;
}

static int str_blank(const char *s)
{
  s = skip_ws(s);
  return *s == 0 || *s == ';';
}

static int eval_cond(const char *m, size_t l, const char *args, char **ops, int nops)
{
  char d[16];
  ival v = 0;
  if (l >= sizeof(d)) return 0;
  memcpy(d, m, l); d[l] = 0;
  for (char *c = d; *c; c++) *c = tolower((unsigned char)*c);
  if (!strcmp(d, "if1") || !strcmp(d, "ifp1")) return 1;
  if (!strcmp(d, "if2") || !strcmp(d, "ifp2")) return 0;
  if (!strcmp(d, "ifb")) return str_blank(args);
  if (!strcmp(d, "ifnb")) return !str_blank(args);
  if (!strcmp(d, "ifc") || !strcmp(d, "ifnc")) {
    int r = 0;
    if (nops >= 2) {
      char *a = unquote(ops[0]), *b = unquote(ops[1]);
      r = !strcmp(a, b);
      free(a); free(b);
    } else if (nops == 1) {
      char *a = unquote(ops[0]);
      r = !*a;
      free(a);
    } else r = 1;
    return d[2] == 'c' ? r : !r;
  }
  if (!strcmp(d, "ifd") || !strcmp(d, "ifnd")) {
    int r = 0;
    if (nops >= 1) {
      char *mn = mangle(ops[0], strlen(ops[0]));
      symbol *s = sym_lookup(mn);
      free(mn);
      r = s && s->type != S_NONE && s->type != S_IMPORT;
    }
    return d[2] == 'd' ? r : !r;
  }
  if (!strcmp(d, "ifmacrod") || !strcmp(d, "ifmacrond")) {
    int r = nops >= 1 && find_macro(ops[0], strlen(ops[0])) != NULL;
    return d[7] == 'd' ? r : !r;
  }
  {
    expr *e = parse_full_expr(args);
    if (e && !eval_const(e, &v)) {
      error("expression must be a constant");
      v = 0;
    }
  }
  if (!strcmp(d, "if") || !strcmp(d, "ifne")) return v != 0;
  if (!strcmp(d, "ifeq")) return v == 0;
  if (!strcmp(d, "ifgt")) return v > 0;
  if (!strcmp(d, "ifge")) return v >= 0;
  if (!strcmp(d, "iflt")) return v < 0;
  if (!strcmp(d, "ifle")) return v <= 0;
  return 0;
}

/* --------------------------------------------------------- line split */
typedef struct pline {
  const char *label; size_t llen; int lexport; int lcolon;
  const char *mnem; size_t mlen;
  const char *ext; size_t elen;
  const char *ops; int olen;
  const char *rest;     /* raw text after mnemonic */
  int equals;           /* label = expr */
} pline;

static int is_mnemchar(int c) { return isalnum(c) || c == '_' || c == '.'; }

static void split_line(const char *line, pline *pl)
{
  const char *p = line;
  memset(pl, 0, sizeof(*pl));
  if (*p == '*' || *p == ';') return;
  if (*p && !is_ws(*p)) {
    const char *s = p;
    if (is_idstart((unsigned char)*p) || *p == '@' || (opt_localu && *p == '_')) {
      p++;
      while (is_idchar((unsigned char)*p) || (ident_dots && *p == '.')) p++;
      if (*p == '$') p++;
      pl->label = s;
      pl->llen = p - s;
      if (*p == ':') {
        p++;
        pl->lcolon = 1;
        if (*p == ':') { p++; pl->lexport = 1; }
      }
    } else {
      /* junk at line start: let the mnemonic parser complain */
    }
  } else {
    const char *q = skip_ws(p);
    if (is_idstart((unsigned char)*q)) {
      const char *s = q;
      q++;
      while (is_idchar((unsigned char)*q)) q++;
      if (*q == '$') q++;
      if (*q == ':') {
        pl->label = s;
        pl->llen = q - s;
        pl->lcolon = 1;
        q++;
        if (*q == ':') { q++; pl->lexport = 1; }
        p = q;
      }
    }
  }
  p = skip_ws(p);
  if (*p == '=') {
    pl->equals = 1;
    pl->mnem = p;
    pl->mlen = 1;
    p++;
    if (*p == '.' && isalpha((unsigned char)p[1])) { pl->ext = p + 1; pl->elen = 1; p += 2; }
    p = skip_ws(p);
    pl->rest = p;
    pl->ops = p;
    pl->olen = op_end(p);
    return;
  }
  if (!*p || *p == ';' || *p == '*') return;
  {
    const char *s = p;
    while (*p && is_mnemchar((unsigned char)*p)) p++;
    if (p == s) {
      /* not a mnemonic: let the caller report it */
      pl->mnem = s;
      pl->mlen = 0;
      pl->rest = s;
      return;
    }
    pl->mnem = s;
    pl->mlen = p - s;
    {
      const char *d = memchr(s, '.', p - s);
      if (d && d != s) {
        pl->mlen = d - s;
        pl->ext = d + 1;
        pl->elen = p - d - 1;
      }
    }
  }
  pl->rest = p;
  if (*p && !is_ws(*p) && *p != ';') {
    /* no space before operands */
    pl->ops = p;
    pl->olen = op_end(p);
    return;
  }
  p = skip_ws(p);
  pl->rest = p;
  pl->ops = p;
  pl->olen = (*p == ';') ? 0 : op_end(p);
}

static int mnem_is(const pline *pl, const char *name)
{
  return pl->mlen == strlen(name) && !strnicmp_(pl->mnem, name, pl->mlen);
}

/* ------------------------------------------------------ the directives */
typedef struct dirctx {
  pline *pl;
  char **ops; int nops;
  char ext;
} dirctx;

static int label_handled;

static void need_label_define(pline *pl)
{
  if (pl->label && !label_handled) {
    define_label_here(pl->label, pl->llen, pl->lexport);
    label_handled = 1;
  }
}

static void do_offset_dir(int esize, symbol *ctr, pline *pl, const char *arg, int down)
{
  ival cur = 0, n = 1;
  eres r;
  if (ctr->type == S_SET) { eval_expr(ctr->e, &r); cur = r.v; }
  if (arg && *arg) { if (!const_expr(arg, &n)) n = 0; }
  if (down) {
    cur -= n * esize;
    if (pl->label) { define_equ(pl->label, pl->llen, new_num(cur), 0); label_handled = 1; }
  } else {
    if (pl->label) {
      symbol *s = define_equ(pl->label, pl->llen, new_num(cur), 0);
      (void)s;
      label_handled = 1;
    }
    cur += n * esize;
  }
  set_internal(ctr, cur);
}

static void begin_rept(int kind, int count)
{
  collecting_rept = 1;
  rept_kind = kind;
  rept_nest = 0;
  rept_n = 0;
  rept_cap = 16;
  rept_lines = xmalloc(rept_cap * sizeof(char *));
  rept_count = count;
}

static int handle_directive(pline *pl, const char *op, int olen)
{
  char name[24];
  char **ops = NULL;
  int nops = 0;
  char ext = pl->elen == 1 ? tolower((unsigned char)pl->ext[0]) : 0;
  char *opstr;
  size_t i;
  if (pl->mlen >= sizeof(name)) return 0;
  for (i = 0; i < pl->mlen; i++) name[i] = tolower((unsigned char)pl->mnem[i]);
  name[pl->mlen] = 0;
  opstr = xstrndup(op, olen);

#define IS(n) (!strcmp(name, n))
#define OPS() (nops = split_ops(opstr, olen, &ops))

  /* label-consuming directives first */
  if (pl->equals || IS("equ") || IS("set") || IS("fequ") ) {
    expr *e;
    if (!pl->label) { error("missing label"); goto done; }
    if (IS("set") && pl->elen) { /* set.x unsupported */ }
    e = parse_full_expr(opstr);
    if (!e) e = new_num(0);
    define_equ(pl->label, pl->llen, e, IS("set"));
    label_handled = 1;
    goto done;
  }
  if (IS("equr") || IS("fequr")) {
    symbol *s;
    const char *p = opstr;
    int r;
    if (!pl->label) { error("missing label"); goto done; }
    r = cpu->parse_reg ? cpu->parse_reg(&p) : -1;
    if (r < 0) { error("register expected"); goto done; }
    s = get_symbol(pl->label, pl->llen);
    s->type = IS("equr") ? S_REG : S_FREG;
    s->val = r;
    s->flags |= SF_DEFINED;
    label_handled = 1;
    goto done;
  }
  if (IS("reg") || IS("equrl") || IS("freg") || IS("fequrl")) {
    /* register list symbol; the cpu module parses the list */
    symbol *s;
    extern int m68k_parse_reglist_str(const char *s, uint32_t *mask, int fp);
    uint32_t mask;
    int fp = name[0] == 'f';
    if (!pl->label) { error("missing label"); goto done; }
    if (!m68k_parse_reglist_str(opstr, &mask, fp)) { error("invalid register list"); goto done; }
    s = get_symbol(pl->label, pl->llen);
    s->type = fp ? S_FREGLIST : S_REGLIST;
    s->val = mask;
    s->flags |= SF_DEFINED;
    label_handled = 1;
    goto done;
  }
  if (IS("macro")) {
    const char *mn;
    size_t ml;
    char **args = NULL;
    int nargs = 0;
    if (pl->label) {
      mn = pl->label; ml = pl->llen;
      label_handled = 1;
      OPS();
      args = ops; nargs = nops; ops = NULL; nops = 0;
    } else {
      const char *p = skip_ws(opstr);
      const char *s = p;
      while (*p && !is_ws(*p) && *p != ',') p++;
      mn = s; ml = p - s;
      if (!ml) { error("macro name expected"); goto done; }
      p = skip_ws(p);
      if (*p == ',') p++;
      nargs = split_ops(p, (int)strlen(p), &args);
    }
    {
      char *mname = xstrndup(mn, ml);
      defmacro = new_macro(mname);
      free(mname);
      if (nargs) {
        int k;
        defmacro->argnames = args;
        defmacro->nargnames = nargs;
        for (k = 0; k < nargs; k++) {
          char *eq = strchr(args[k], '=');
          if (eq) *eq = 0;
        }
      }
    }
    defmacro_nest = 0;
    goto done;
  }
  if (IS("rs") || IS("so") || IS("fo")) {
    symbol *ctr = IS("rs") ? sym_rs : IS("so") ? sym_so : sym_fo;
    int es = ext ? sizeof_ext(ext) : 2;
    if (!es) { error("invalid extension"); goto done; }
    do_offset_dir(es, ctr, pl, opstr, IS("fo"));
    goto done;
  }
  if (IS("ttl") && pl->label && !*skip_ws(opstr)) {
    idnt_name = xstrndup(pl->label, pl->llen);
    label_handled = 1;
    goto done;
  }
  if (IS("cargs")) {
    const char *p;
    ival off = 4;
    int k;
    OPS();
    k = 0;
    if (nops && ops[0][0] == '#') {
      if (!const_expr(ops[0] + 1, &off)) off = 4;
      k = 1;
    }
    for (; k < nops; k++) {
      char *s = ops[k];
      char *dot = strrchr(s, '.');
      int sz = 2;
      if (dot && dot != s && dot[1] && !dot[2]) {
        if (tolower((unsigned char)dot[1]) == 'l') sz = 4;
        *dot = 0;
      }
      p = s;
      define_equ(s, strlen(s), new_num(off), 0);
      off += sz;
    }
    goto done;
  }

  /* conditionals */
  if (is_cond_dir(pl->mnem, pl->mlen)) {
    int t;
    need_label_define(pl);
    OPS();
    t = eval_cond(pl->mnem, pl->mlen, opstr, ops, nops);
    cond_push(t);
    goto done;
  }
  if (IS("else")) { cond_else(); goto done; }
  if (IS("elseif")) {
    int t = 0;
    if (!cond_parent_active() || 1) {
      /* evaluate only if needed */
      ival v = 0;
      expr *e;
      diag_suppress++;
      e = parse_full_expr(opstr);
      if (e) eval_const(e, &v);
      diag_suppress--;
      t = v != 0;
    }
    cond_elseif(t);
    goto done;
  }
  if (IS("endif") || IS("endc")) { cond_end(); goto done; }
  if (IS("iif")) {
    /* iif <expr> <statement> */
    const char *p = skip_ws(pl->rest);
    expr *e = parse_expr(&p);
    ival v = 0;
    if (!e || !eval_const(e, &v)) { error("expression must be a constant"); goto done; }
    if (v) {
      char *stmt = strprintf("%s%.*s %s", pl->label ? "" : "",
                             pl->label ? (int)pl->llen : 0, pl->label ? pl->label : "", skip_ws(p));
      extern void mot_parse_line(const char *line);
      label_handled = 1;
      mot_parse_line(stmt);
      free(stmt);
    } else
      need_label_define(pl);
    goto done;
  }

  need_label_define(pl);

  if (IS("dc") || IS("db") || IS("dw") || IS("dl") || IS("dr")) {
    int es = 2, ft = 0;
    if (IS("db")) es = 1;
    else if (IS("dw")) es = 2;
    else if (IS("dl")) es = 4;
    else if (ext) {
      es = sizeof_ext(ext);
      if (ext == 's' || ext == 'd' || ext == 'x' || ext == 'p') ft = ext;
      if (!es) { error("invalid extension"); goto done; }
    }
    OPS();
    if (cursec && cursec->type == ST_BSS && 0) {}
    do_data(es, ft, ops, nops, IS("dr"));
    goto done;
  }
  if (IS("ds") || IS("dcb") || IS("blk") || IS("dx")) {
    int es = 2, ft = 0;
    if (ext) {
      es = sizeof_ext(ext);
      if (ext == 's' || ext == 'd' || ext == 'x' || ext == 'p') ft = ext;
      if (!es) { error("invalid extension"); goto done; }
    }
    OPS();
    if (nops < 1) { error("missing operand"); goto done; }
    do_space(es, ft, ops[0], (!IS("ds") && !IS("dx") && nops > 1) ? ops[1] : NULL,
             IS("ds") || IS("dx"));
    goto done;
  }
  if (IS("even")) { do_align(2, 0, 1); goto done; }
  if (IS("odd")) { do_align(2, -1, 0); goto done; }
  if (IS("cnop")) {
    ival off, al;
    OPS();
    if (nops != 2) { error("missing operand"); goto done; }
    if (!const_expr(ops[0], &off) || !const_expr(ops[1], &al)) goto done;
    do_align(al, off, !opt_devpac);
    goto done;
  }
  if (IS("align")) {
    ival bits;
    OPS();
    if (nops < 1 || !const_expr(ops[0], &bits)) goto done;
    do_align((ival)1 << bits, 0, 0);
    goto done;
  }
  if (IS("section")) { OPS(); do_section(ops, nops); goto done; }
  if (IS("code") || IS("text") || IS("cseg")) { switch_section(IS("code") && outmod->named_sections ? "CODE" : "CODE", ST_CODE, 0); goto done; }
  if (IS("data") || IS("dseg")) { switch_section("DATA", ST_DATA, 0); goto done; }
  if (IS("bss")) { switch_section("BSS", ST_BSS, 0); goto done; }
  if (IS("code_c")) { switch_section("CODE_C", ST_CODE, 0x40000000); goto done; }
  if (IS("code_f")) { switch_section("CODE_F", ST_CODE, 0x80000000); goto done; }
  if (IS("code_p")) { switch_section("CODE_P", ST_CODE, 0); goto done; }
  if (IS("data_c")) { switch_section("DATA_C", ST_DATA, 0x40000000); goto done; }
  if (IS("data_f")) { switch_section("DATA_F", ST_DATA, 0x80000000); goto done; }
  if (IS("data_p")) { switch_section("DATA_P", ST_DATA, 0); goto done; }
  if (IS("bss_c")) { switch_section("BSS_C", ST_BSS, 0x40000000); goto done; }
  if (IS("bss_f")) { switch_section("BSS_F", ST_BSS, 0x80000000); goto done; }
  if (IS("bss_p")) { switch_section("BSS_P", ST_BSS, 0); goto done; }
  if (IS("pushsection")) {
    if (nsecstack < 64) secstack[nsecstack++] = default_section();
    goto done;
  }
  if (IS("popsection")) {
    if (nsecstack) cursec = secstack[--nsecstack];
    else error("section stack is empty");
    goto done;
  }
  if (IS("org")) {
    ival v;
    if (!const_expr(opstr, &v)) goto done;
    cursec = new_org_section(v);
    goto done;
  }
  if (IS("rorg")) {
    atom *a;
    OPS();
    if (nops < 1) { error("missing operand"); goto done; }
    a = add_atom(AT_RORG);
    a->u.rorg.target = parse_full_expr(ops[0]);
    if (nops > 1) a->u.rorg.fill = parse_full_expr(ops[1]);
    goto done;
  }
  if (IS("offset")) {
    ival v = 0;
    static ival last_offset;
    section *s;
    if (*skip_ws(opstr)) { if (!const_expr(opstr, &v)) v = 0; }
    else v = last_offset;
    s = xcalloc(1, sizeof(*s));
    s->name = xstrdup("OFFSET");
    s->flags = SEC_OFFSET;
    s->org = v;
    s->type = ST_BSS;
    /* not linked into the section list: labels evaluate absolute */
    cursec = s;
    {
      extern section *sec_last;
      /* link it so that sizes are computed */
      if (sec_last) sec_last->next = s; else sec_first = s;
      sec_last = s;
    }
    goto done;
  }
  if (IS("xdef") || IS("global") || IS("globl")) { OPS(); do_xdef(ops, nops, 0); goto done; }
  if (IS("xref") || IS("nref")) { OPS(); do_xdef(ops, nops, 1); goto done; }
  if (IS("public")) { OPS(); do_xdef(ops, nops, 2); goto done; }
  if (IS("weak")) { OPS(); do_xdef(ops, nops, 3); goto done; }
  if (IS("comm")) {
    symbol *s;
    ival sz;
    OPS();
    if (nops < 2) { error("missing operand"); goto done; }
    s = get_symbol(ops[0], strlen(ops[0]));
    if (!const_expr(ops[1], &sz)) goto done;
    s->type = S_COMMON;
    s->val = sz;
    s->align = 4;
    s->flags |= SF_EXPORT | SF_DEFINED;
    goto done;
  }
  if (IS("rsreset") || IS("clrso") || IS("clrfo")) {
    set_internal(IS("rsreset") ? sym_rs : IS("clrso") ? sym_so : sym_fo, 0);
    goto done;
  }
  if (IS("rsset") || IS("setso") || IS("setfo")) {
    ival v;
    if (const_expr(opstr, &v))
      set_internal(IS("rsset") ? sym_rs : IS("setso") ? sym_so : sym_fo, v);
    goto done;
  }
  if (IS("include")) {
    char *f = unquote(opstr);
    push_file(f);
    free(f);
    goto done;
  }
  if (IS("incdir")) {
    int k;
    OPS();
    for (k = 0; k < nops; k++) { char *f = unquote(ops[k]); add_incpath(f); free(f); }
    goto done;
  }
  if (IS("incbin")) {
    char *f, *path;
    FILE *fp;
    ival off = 0, len = -1;
    atom *a;
    OPS();
    if (nops < 1) { error("missing operand"); goto done; }
    f = unquote(ops[0]);
    if (nops > 1 && !const_expr(ops[1], &off)) off = 0;
    if (nops > 2 && !const_expr(ops[2], &len)) len = -1;
    path = find_include(f, 1);
    if (!path || !(fp = fopen(path, "rb"))) {
      error("can't open binary file \"%s\"", f);
      free(f); free(path);
      goto done;
    }
    {
      long sz;
      fseek(fp, 0, SEEK_END);
      sz = ftell(fp);
      if (off > sz) off = sz;
      if (len < 0 || off + len > sz) len = sz - off;
      fseek(fp, (long)off, SEEK_SET);
      a = add_atom(AT_BYTES);
      a->u.bytes.d = xmalloc(len ? len : 1);
      a->u.bytes.n = fread(a->u.bytes.d, 1, len, fp);
      a->size = a->u.bytes.n;
      fclose(fp);
    }
    free(f); free(path);
    goto done;
  }
  if (IS("rept")) {
    ival n;
    if (!const_expr(opstr, &n)) n = 0;
    if (n < 0) n = 0;
    begin_rept(0, (int)n);
    goto done;
  }
  if (IS("irp") || IS("irpc")) {
    /* irp name,val1,val2,... */
    OPS();
    if (nops < 1) { error("missing operand"); goto done; }
    begin_rept(IS("irp") ? 1 : 2, 0);
    free(irp_name);
    irp_name = xstrdup(ops[0]);
    if (IS("irp")) {
      irp_nvals = nops - 1;
      irp_vals = xcalloc(irp_nvals + 1, sizeof(char *));
      for (int k = 1; k < nops; k++) irp_vals[k - 1] = xstrdup(ops[k]);
    } else {
      const char *v = nops > 1 ? ops[1] : "";
      irp_nvals = (int)strlen(v);
      irp_vals = xcalloc(irp_nvals + 1, sizeof(char *));
      for (int k = 0; k < irp_nvals; k++) irp_vals[k] = xstrndup(v + k, 1);
    }
    goto done;
  }
  if (IS("endr")) { error("endr without rept"); goto done; }
  if (IS("endm")) { error("endm without macro"); goto done; }
  if (IS("mexit")) { if (in_macro()) exit_macro(); else error("mexit outside of a macro"); goto done; }
  if (IS("end")) { end_assembly(); goto done; }
  if (IS("fail")) { error("%s", skip_ws(pl->rest)); goto done; }
  if (IS("echo") || IS("printt")) {
    int k;
    if (IS("echo")) { char *s = unquote(pl->rest); printf("%s\n", s); free(s); goto done; }
    OPS();
    for (k = 0; k < nops; k++) { char *s = unquote(ops[k]); printf("%s\n", s); free(s); }
    goto done;
  }
  if (IS("printv")) {
    int k;
    OPS();
    for (k = 0; k < nops; k++) {
      expr *e = parse_full_expr(ops[k]);
      ival v = 0;
      if (e) eval_const(e, &v);
      printf("$%08llX %lld\n", (unsigned long long)(uint32_t)v, (long long)v);
    }
    goto done;
  }
  if (IS("idnt") || IS("ttl")) { free(idnt_name); idnt_name = unquote(opstr); goto done; }
  if (IS("list")) { list_on = 1; goto done; }
  if (IS("nolist")) { list_on = 0; goto done; }
  if (IS("page") || IS("nopage") || IS("llen") || IS("plen") || IS("spc") ||
      IS("comment") || IS("msource") || IS("output") || IS("showoffset") ||
      IS("xlist") || IS("mask2") || IS("auto") || IS("plen") || IS("debug") ||
      IS("listfile") || IS("format") || IS("noformat") || IS("dsource") || IS("tttl") ||
      IS("subttl") || IS("sttl")) {
    if (IS("output") && !output_name) { output_name = unquote(opstr); }
    goto done;
  }
  if (IS("rem")) { in_rem = 1; goto done; }
  if (IS("inline")) {
    if (inline_depth < 64) inline_stack[inline_depth++] = ++inline_id;
    else error("maximum inline nesting depth exceeded (64)");
    goto done;
  }
  if (IS("einline")) {
    if (inline_depth) inline_depth--; else error("einline without inline");
    goto done;
  }
  free(opstr);
  return 0;
done:
  free(opstr);
  if (ops) free_ops(ops, nops);
  return 1;
#undef IS
#undef OPS
}

/* ------------------------------------------------------- line parsing */
static void collect_line(const char *line)
{
  pline pl;
  split_line(line, &pl);
  if (defmacro) {
    if (pl.mnem && (mnem_is(&pl, "endm"))) {
      if (defmacro_nest == 0) { defmacro = NULL; return; }
      defmacro_nest--;
    } else if (pl.mnem && mnem_is(&pl, "macro")) defmacro_nest++;
    macro_add_line(defmacro, line);
    return;
  }
  /* rept */
  if (pl.mnem && mnem_is(&pl, "endr")) {
    if (rept_nest == 0) {
      collecting_rept = 0;
      if (rept_kind == 0) push_rept(rept_lines, rept_n, rept_count);
      else push_irp(rept_lines, rept_n, irp_name, irp_vals, irp_nvals);
      return;
    }
    rept_nest--;
  } else if (pl.mnem && (mnem_is(&pl, "rept") || mnem_is(&pl, "irp") || mnem_is(&pl, "irpc")))
    rept_nest++;
  if (rept_n >= rept_cap) { rept_cap *= 2; rept_lines = xrealloc(rept_lines, rept_cap * sizeof(char *)); }
  rept_lines[rept_n++] = xstrdup(line);
}

void mot_parse_line(const char *line)
{
  pline pl;
  label_handled = 0;
  line_anchor = NULL;
  stmt_sec = cursec;
  stmt_prev = cursec ? cursec->last : NULL;
  if (defmacro || collecting_rept) { collect_line(line); return; }
  if (in_rem) {
    split_line(line, &pl);
    if (pl.mnem && mnem_is(&pl, "erem")) in_rem = 0;
    return;
  }
  split_line(line, &pl);
  if (!cond_active()) {
    if (!pl.mnem) return;
    if (is_cond_dir(pl.mnem, pl.mlen)) { cond_push(0); return; }
    if (mnem_is(&pl, "else")) { cond_else(); return; }
    if (mnem_is(&pl, "elseif")) {
      ival v = 0;
      if (cond_parent_active()) {
        char *s = xstrndup(pl.ops, pl.olen);
        expr *e;
        const char *p = s;
        e = parse_expr(&p);
        if (e) eval_const(e, &v);
        free(s);
      }
      cond_elseif(v != 0);
      return;
    }
    if (mnem_is(&pl, "endif") || mnem_is(&pl, "endc")) { cond_end(); return; }
    return;
  }
  if (!pl.mnem) {
    if (pl.label) define_label_here(pl.label, pl.llen, pl.lexport);
    return;
  }
  if (pl.mlen == 0) {
    error("mnemonic expected");
    return;
  }
  if (handle_directive(&pl, pl.ops ? pl.ops : "", pl.olen)) return;
  /* cpu specific directives (opt, machine, near, ...) */
  if (cpu->directive) {
    char nm[32];
    if (pl.mlen < sizeof(nm)) {
      char *args = xstrndup(pl.ops ? pl.ops : "", pl.olen);
      int r;
      memcpy(nm, pl.mnem, pl.mlen);
      nm[pl.mlen] = 0;
      if (pl.elen) { /* e.g. .sdreg - not applicable */ }
      r = cpu->directive(nm, args);
      free(args);
      if (r) { need_label_define(&pl); return; }
    }
  }
  /* macro call */
  {
    macro *m = find_macro(pl.mnem, pl.mlen);
    if (m) {
      char **args;
      int nargs;
      char *q = pl.elen ? xstrndup(pl.ext, pl.elen) : NULL;
      need_label_define(&pl);
      nargs = split_macro_args(pl.ops ? pl.ops : "", pl.olen, &args);
      push_macro(m, args, nargs, q);
      free(q);
      return;
    }
  }
  /* instruction */
  {
    char **ops;
    int nops = split_ops(pl.ops ? pl.ops : "", pl.olen, &ops);
    char *ext = pl.elen ? xstrndup(pl.ext, pl.elen) : NULL;
    need_label_define(&pl);
    if (!cpu->instruction(pl.mnem, pl.mlen, ext, ops, nops, 0)) {
      error("unknown mnemonic <%.*s>", (int)(pl.mlen + (pl.elen ? pl.elen + 1 : 0)), pl.mnem);
    }
    free(ext);
    free_ops(ops, nops);
  }
}

void mot_init(void)
{
  syntax_number = mot_number;
  syntax_ident = mot_ident;
  syntax_pcexpr = mot_pc;
  sym_rs = sym_get("__RS");
  sym_so = sym_get("__SO");
  sym_fo = sym_get("__FO");
  if (opt_devpac) {
    set_internal(sym_rs, 0);
    set_internal(sym_so, 0);
    set_internal(sym_fo, 0);
  } else {
    set_internal(sym_rs, 0);
    set_internal(sym_so, 0);
    set_internal(sym_fo, 0);
  }
  sym_reptn = sym_get("REPTN");
  set_internal(sym_reptn, -1);
  sym_narg = sym_get("NARG");
  sym_carg = sym_get("CARG");
  if (opt_devpac || opt_phxass) macro_args_max = 35;
  if (opt_devpac) ident_dots = 1;
}

void mot_finish(void)
{
  if (defmacro) error("missing endm");
  if (collecting_rept) error("missing endr");
  if (cond_level) error("missing endif");
}
