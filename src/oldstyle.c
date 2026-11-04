/* gfasm - "oldstyle" syntax module for 8-bit CPUs (vasm "oldstyle"
 * compatible).  MIT License.  Written from the vasm manual's chapter on the
 * oldstyle syntax module and black-box comparison with a vasm binary. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

extern int opt_esc, ident_dots;
extern cpu_module cpu_z80;
static int opt_dotdir, opt_autoexp, opt_ast, opt_noc, opt_noi, opt_sect, opt_ldots;
static char pcchar = '*';
static char *last_global;
static symbol *line_anchor;
static section *stmt_sec;
static atom *stmt_prev;
static int inline_id, inline_depth;

static macro *defmacro;
static int defmacro_nest;
static char **rept_lines; static int rept_n, rept_cap, rept_nest, rept_count;
static int collecting;

static int is_z80(void) { return cpu == &cpu_z80; }

/* ------------------------------------------------------------ numbers */
static expr *os_ident(const char *n, size_t len);

static int digval(int c) { c = tolower(c); return isdigit(c) ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : 99; }

static int os_number(const char **pp, expr **out)
{
  const char *p = *pp, *s;
  uval v = 0;
  int base = 0;
  if (*p == '$' && isxdigit((unsigned char)p[1])) { base = 16; p++; }
  else if (*p == '%' && (p[1] == '0' || p[1] == '1')) { base = 2; p++; }
  else if (*p == '@' && isdigit((unsigned char)p[1])) { base = is_z80() ? 2 : 8; p++; }
  else if (*p == '&' && is_z80() && isxdigit((unsigned char)p[1])) { base = 16; p++; }
  else if (*p == '#' && is_z80() && isxdigit((unsigned char)p[1])) { base = 16; p++; }
  if (base) {
    while (digval((unsigned char)*p) < base) v = v * base + digval((unsigned char)*p++);
    *pp = p;
    *out = new_num((ival)(int32_t)(uint32_t)v);
    return 1;
  }
  if (*p == pcchar && !(isalnum((unsigned char)p[1]) || p[1] == '_' || p[1] == pcchar)) {
    *pp = p + 1;
    *out = syntax_pcexpr();
    return 1;
  }
  if (!isdigit((unsigned char)*p)) return 0;
  s = p;
  /* N#digits: arbitrary base 2..9 */
  if (p[1] == '#' && *p >= '2' && *p <= '9' && isdigit((unsigned char)p[2])) {
    base = *p - '0';
    p += 2;
    while (isdigit((unsigned char)*p) && *p - '0' < base) v = v * base + (*p++ - '0');
    *pp = p;
    *out = new_num((ival)(int32_t)(uint32_t)v);
    return 1;
  }
  while (isalnum((unsigned char)*p)) p++;
  {
    size_t n = p - s;
    int last = tolower((unsigned char)s[n - 1]);
    size_t i;
    int ok = 1;
    /* local label "123$" */
    if (*p == '$' && !isalnum((unsigned char)p[1])) {
      for (i = 0; i < n; i++) if (!isdigit((unsigned char)s[i])) ok = 0;
      if (ok) { *out = os_ident(s, n + 1); *pp = p + 1; return *out ? 1 : -1; }
      ok = 1;
    }
    if (!opt_noc && n > 2 && s[0] == '0' && tolower((unsigned char)s[1]) == 'x') { base = 16; s += 2; n -= 2; }
    else if (!opt_noi && n > 1 && last == 'h') { base = 16; n--; }
    else if (!opt_noc && n > 2 && s[0] == '0' && tolower((unsigned char)s[1]) == 'b' &&
             strspn(s + 2, "01") == n - 2) { base = 2; s += 2; n -= 2; }
    else if (!opt_noi && n > 1 && last == 'b' && strspn(s, "01") == n - 1) { base = 2; n--; }
    else if (!opt_noi && n > 1 && (last == 'o' || last == 'q')) { base = 8; n--; }
    else if (!opt_noi && n > 1 && last == 'd' && strspn(s, "0123456789") == n - 1) { base = 10; n--; }
    else if (!opt_noc && n > 1 && s[0] == '0' && strspn(s, "0123456789") == n) {
      /* C-style octal; the oracle stops at the first non-octal digit */
      for (i = 0; i < n && s[i] < '8'; i++) v = v * 8 + (s[i] - '0');
      *pp = p;
      *out = new_num((ival)(int32_t)(uint32_t)v);
      return 1;
    }
    else base = 10;
    for (i = 0; i < n; i++) {
      int d = digval((unsigned char)s[i]);
      if (d >= base) { ok = 0; break; }
      v = v * base + d;
    }
    if (!ok) {
      /* not a number after all: stop at the decimal digits */
      p = *pp; v = 0;
      while (isdigit((unsigned char)*p)) v = v * 10 + (*p++ - '0');
    }
  }
  *pp = p;
  *out = new_num((ival)(int32_t)(uint32_t)v);
  return 1;
}

/* ------------------------------------------------------------ symbols */
static int is_local(const char *n, size_t len)
{
  if (len > 1 && n[0] == '.') return 1;
  if (len > 1 && n[len - 1] == '$') return 1;
  return 0;
}

static symbol *get_sym(const char *n, size_t len)
{
  char *m;
  symbol *s;
  const char *dot = opt_ldots ? NULL : memchr(n + 1, '.', len ? len - 1 : 0);
  if (is_local(n, len))
    m = strprintf("%s\x1f%.*s", last_global ? last_global : "", (int)len, n);
  else if (dot && dot > n && dot < n + len - 1)
    m = strprintf("%.*s\x1f%.*s", (int)(dot - n), n, (int)(len - (dot - n)), dot);   /* global.local */
  else
    m = xstrndup(n, len);
  s = sym_lookup(m);
  if (!s) {
    s = sym_get(m);
    if (strchr(m, '\x1f')) { s->flags |= SF_LOCAL; s->dispname = xstrndup(n, len); }
  }
  free(m);
  return s;
}

static expr *os_ident(const char *n, size_t len)
{
  symbol *s;
  if (len == 1 && n[0] == pcchar) return syntax_pcexpr();
  s = get_sym(n, len);
  s->flags |= SF_REFD;
  if (s->type == S_SET && s->e) { s->flags |= SF_USED; return copy_expr(s->e); }
  return new_sym(s);
}

static expr *os_pc(void)
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

static void define_label(const char *n, size_t len)
{
  symbol *s;
  if (!is_local(n, len)) {
    free(last_global);
    last_global = inline_depth ? strprintf("%.*s\x1e%d", (int)len, n, inline_id) : xstrndup(n, len);
  }
  s = get_sym(n, len);
  if (s->type != S_NONE && s->type != S_IMPORT) { error("label <%.*s> has already been defined", (int)len, n); return; }
  if (s->type == S_IMPORT) s->flags |= SF_EXPORT;
  if (opt_autoexp && !is_local(n, len)) s->flags |= SF_EXPORT;
  add_label_atom(s);
}

/* ------------------------------------------------------------ helpers */
/* is c at p the start of a quoted string/char constant? */
static int quote_at(const char *line, const char *p)
{
  if (*p == '"') return 1;
  if (*p != '\'') return 0;
  /* Z80 af' is not a quote */
  if (p > line && (isalnum((unsigned char)p[-1]) || p[-1] == '_')) return 0;
  return 1;
}

static const char *skip_quote(const char *line, const char *p)
{
  char q = *p++;
  (void)line;
  while (*p && *p != q) { if (*p == '\\' && opt_esc && p[1]) p++; p++; }
  return *p ? p + 1 : p;
}

static int split_args(const char *s, char ***out)
{
  char **v = NULL;
  int n = 0, par = 0;
  const char *st, *p, *line = s;
  s = skip_ws(s);
  if (!*s) { *out = NULL; return 0; }
  st = p = s;
  for (;;) {
    if (!*p || (!par && *p == ',')) {
      const char *a = skip_ws(st), *b = p;
      while (b > a && is_ws(b[-1])) b--;
      v = xrealloc(v, (n + 1) * sizeof(char *));
      v[n++] = xstrndup(a, b - a);
      if (!*p) break;
      st = ++p;
      continue;
    }
    if (quote_at(line, p)) { p = skip_quote(line, p); continue; }
    if (*p == '(' || *p == '[') par++;
    else if ((*p == ')' || *p == ']') && par) par--;
    p++;
  }
  *out = v;
  return n;
}

static void free_args(char **v, int n) { int i; for (i = 0; i < n; i++) free(v[i]); free(v); }

static expr *pexpr(const char *s)
{
  const char *p = skip_ws(s);
  int lohi = 0;
  expr *e;
  if (*p == '<' || *p == '>') lohi = *p++;
  e = parse_expr(&p);
  if (!e) { error("syntax error"); return new_num(0); }
  if (*skip_ws(p)) error("garbage at end of line");
  if (lohi == '<') e = new_op(X_AND, e, new_num(255));
  else if (lohi == '>') e = new_op(X_AND, new_op(X_SHR, e, new_num(8)), new_num(255));
  return e;
}

static int cexpr(const char *s, ival *v)
{
  if (!eval_const(pexpr(s), v)) { error("expression must be constant"); *v = 0; return 0; }
  return 1;
}

static char *unq(const char *s)
{
  size_t n;
  s = skip_ws(s);
  n = strlen(s);
  while (n && is_ws(s[n - 1])) n--;
  if (n >= 2 && (s[0] == '"' || s[0] == '\'' || s[0] == '<') && (s[n - 1] == s[0] || (s[0] == '<' && s[n - 1] == '>')))
    return xstrndup(s + 1, n - 2);
  return xstrndup(s, n);
}

static int is_string(const char *o)
{
  size_t n = strlen(o);
  if (n < 2) return 0;
  if (o[0] == '"' && o[n - 1] == '"') return 1;
  if (o[0] == '\'' && o[n - 1] == '\'' && n != 3) return 1;   /* 'c' is a number */
  return 0;
}

static int parse_string(const char *s, char **out)
{
  char q = *s++;
  char *b = xmalloc(strlen(s) + 1);
  int n = 0;
  while (*s && *s != q) {
    int c = (unsigned char)*s++;
    if (c == '\\' && opt_esc && *s) {
      c = (unsigned char)*s++;
      switch (c) {
      case 'b': c = 8; break;
      case 'f': c = 12; break;
      case 'n': c = 10; break;
      case 'r': c = 13; break;
      case 't': c = 9; break;
      case 'e': c = 27; break;
      case '0': c = 0; break;
      case 'x': case 'X': {
        int x = 0;
        while (isxdigit((unsigned char)*s)) x = x * 16 + digval((unsigned char)*s++);
        c = x & 255;
        break;
      }
      }
    }
    b[n++] = (char)c;
  }
  *out = b;
  return n;
}

/* byte/word/... data; mod: abyte modifier (NULL), term: extra terminator byte (-1 none),
 * hibit: set bit 7 of the last character */
static int mask16;
static void data(char **ops, int nops, int esize, int strings, int term, int hibit, expr *mod)
{
  int i;
  static char zero[] = "0";
  static char *zop[1] = { zero };
  if (!nops) { ops = zop; nops = 1; }   /* oracle: no operand = one zero element */
  for (i = 0; i < nops; i++) {
    const char *o = ops[i];
    atom *a;

    if (!*o) continue;
    if (strings && is_string(o) && !mod) {
      char *str;
      int n = parse_string(o, &str);
      if (hibit && n && i == nops - 1) str[n - 1] |= 0x80;
      a = add_atom(AT_DATA);
      a->u.data.esize = 1;
      a->u.data.items = xcalloc(2, sizeof(dataitem));
      a->u.data.n = 1;
      a->u.data.items[0].str = str;
      a->u.data.items[0].slen = n;
      a->size = n;
      continue;
    }
    if (strings && is_string(o) && mod) {
      char *str;
      int n = parse_string(o, &str), k;
      for (k = 0; k < n; k++) {
        a = add_atom(AT_DATA);
        a->u.data.esize = 1;
        a->u.data.items = xcalloc(2, sizeof(dataitem));
        a->u.data.n = 1;
        a->u.data.items[0].e = new_op(X_ADD, new_num((unsigned char)str[k]), copy_expr(mod));
        a->size = 1;
      }
      free(str);
      continue;
    }
    default_section();
    line_anchor = NULL;
    stmt_sec = cursec;
    stmt_prev = cursec->last;
    {
      expr *e = pexpr(o);
      if (mod) e = new_op(X_ADD, e, copy_expr(mod));
      if (mask16) e = new_op(X_AND, e, new_num(0xffff));
      a = add_atom(AT_DATA);
      a->u.data.esize = esize;
      a->u.data.items = xcalloc(2, sizeof(dataitem));
      a->u.data.n = 1;
      a->u.data.items[0].e = e;
      a->size = esize;
    }
  }

  if (term >= 0) {
    atom *a = add_atom(AT_DATA);
    a->u.data.esize = 1;
    a->u.data.items = xcalloc(2, sizeof(dataitem));
    a->u.data.n = 1;
    a->u.data.items[0].e = new_num(term);
    a->size = 1;
  }
}

static void space(const char *cnt, const char *fill, int esize)
{
  atom *a = add_atom(AT_SPACE);
  a->u.space.count = pexpr(cnt);
  a->u.space.esize = esize;
  if (fill && *skip_ws(fill)) a->u.space.fill = pexpr(fill);
  else a->u.space.uninit = 1;
}

static void align_to(ival bytes)
{
  atom *a = add_atom(AT_ALIGN);
  a->u.align.align = bytes;
}

static void set_section(const char *name, const char *flags)
{
  int st = ST_DATA;
  section *s;
  if (flags) {
    if (strchr(flags, 'c') || strchr(flags, 'x')) st = ST_CODE;
    else if (strchr(flags, 'u')) st = ST_BSS;
  } else if (!strcmp(name, "text") || !strcmp(name, ".text")) st = ST_CODE;
  else if (!strcmp(name, "bss") || !strcmp(name, ".bss")) st = ST_BSS;
  else if (strcmp(name, "data") && strcmp(name, ".data") && strcmp(name, "rodata") && strcmp(name, ".rodata")) st = ST_CODE;
  for (s = sec_first; s; s = s->next)
    if (!(s->flags & (SEC_ABS | SEC_OFFSET)) && !strcmp(s->name, name)) break;
  if (!s) s = new_section(name, st, 0);
  cursec = s;
}

/* ------------------------------------------------------------ conditionals */
static int is_cond(const char *d)
{
  static const char *c[] = { "if", "ifdef", "ifndef", "ifd", "ifnd", "ifeq", "ifne", "ifgt", "ifge",
                             "iflt", "ifle", "ifused", "ifnused", NULL };
  int i;
  for (i = 0; c[i]; i++) if (!stricmp_(d, c[i])) return 1;
  return 0;
}

static int cond_value(const char *d, const char *rest)
{
  ival v = 0;
  if (!stricmp_(d, "ifdef") || !stricmp_(d, "ifndef") || !stricmp_(d, "ifd") || !stricmp_(d, "ifnd") ||
      !stricmp_(d, "ifused") || !stricmp_(d, "ifnused")) {
    const char *p = skip_ws(rest), *q = p;
    symbol *s;
    int r;
    while (*q && !is_ws(*q)) q++;
    s = get_sym(p, q - p);
    if (tolower((unsigned char)d[2]) == 'u' || tolower((unsigned char)d[3]) == 'u')
      r = (s->flags & SF_REFD) != 0;
    else
      r = s->type != S_NONE && s->type != S_IMPORT;
    return (!stricmp_(d, "ifdef") || !stricmp_(d, "ifd") || !stricmp_(d, "ifused")) ? r : !r;
  }
  cexpr(rest, &v);
  if (!stricmp_(d, "if") || !stricmp_(d, "ifne")) return v != 0;
  if (!stricmp_(d, "ifeq")) return v == 0;
  if (!stricmp_(d, "ifgt")) return v > 0;
  if (!stricmp_(d, "ifge")) return v >= 0;
  if (!stricmp_(d, "iflt")) return v < 0;
  if (!stricmp_(d, "ifle")) return v <= 0;
  return 0;
}

static const char *dirname_of(const char *w, size_t l, char *buf, size_t bs)
{
  if (opt_dotdir && *w == '.') { w++; l--; }   /* oracle: the dot is optional */
  if (l >= bs) return NULL;
  memcpy(buf, w, l);
  buf[l] = 0;
  return buf;
}

static void collect(const char *line)
{
  const char *p = line, *w;
  size_t l;
  char d[24];
  const char *dn;
  /* the directive may follow a label in the first column */
  if (*p && !is_ws(*p)) { while (*p && !is_ws(*p) && *p != ':') p++; if (*p == ':') p++; }
  p = skip_ws(p);
  w = p;
  while (*p && !is_ws(*p) && *p != ';') p++;
  l = p - w;
  dn = dirname_of(w, l, d, sizeof d);
  if (defmacro) {
    if (dn && (!stricmp_(dn, "endm") || !stricmp_(dn, "endmac") || !stricmp_(dn, "endmacro"))) {
      if (!defmacro_nest) { defmacro = NULL; return; }
      defmacro_nest--;
    } else if (dn && (!stricmp_(dn, "macro") || !stricmp_(dn, "mac"))) defmacro_nest++;
    macro_add_line(defmacro, line);
    return;
  }
  if (dn && (!stricmp_(dn, "endr") || !stricmp_(dn, "endrep") || !stricmp_(dn, "endrepeat"))) {
    if (!rept_nest) { collecting = 0; push_rept(rept_lines, rept_n, rept_count); return; }
    rept_nest--;
  } else if (dn && (!stricmp_(dn, "rept") || !stricmp_(dn, "repeat"))) rept_nest++;
  if (rept_n >= rept_cap) { rept_cap = rept_cap * 2 + 16; rept_lines = xrealloc(rept_lines, rept_cap * sizeof(char *)); }
  rept_lines[rept_n++] = xstrdup(line);
}

static void start_macro(const char *name, size_t nl, const char *args)
{
  char **a;
  int n, i;
  char *t = xstrdup(args ? args : "");
  defmacro = new_macro(xstrndup(name, nl));
  defmacro_nest = 0;
  for (i = 0; t[i]; i++) if (is_ws(t[i])) t[i] = ',';
  n = split_args(t, &a);
  free(t);
  defmacro->argnames = xcalloc(n + 1, sizeof(char *));
  for (i = 0; i < n; i++) if (*a[i]) defmacro->argnames[defmacro->nargnames++] = a[i];
}

/* ------------------------------------------------------------ directives */
static int directive(const char *d, const char *rest, char **ops, int nops, const char *lab, size_t ll)
{
#define IS(x) (!stricmp_(d, x))
  ival v;
  if (IS("byte") || IS("db") || IS("defb") || IS("dfb") || IS("fcb") || IS("asc") || (IS("data") && !opt_sect)) { data(ops, nops, 1, 1, -1, 0, NULL); return 1; }
  if (IS("word") || IS("dw") || IS("defw") || IS("dfw") || IS("fdb") || IS("addr") || IS("wor")) { data(ops, nops, 2, 0, -1, 0, NULL); return 1; }
  if (IS("defl")) { data(ops, nops, 4, 0, -1, 0, NULL); return 1; }
  if (IS("defp")) { mask16 = 1; data(ops, nops, 2, 0, -1, 0, NULL); mask16 = 0; return 1; }   /* oracle: 2 bytes */
  if ((IS("text") && !opt_sect) || IS("defm") || IS("ascii") || IS("fcc")) { data(ops, nops, 1, 1, -1, 0, NULL); return 1; }
  if (IS("fcs")) { data(ops, nops, 1, 1, -1, 1, NULL); return 1; }
  if (IS("str")) { data(ops, nops, 1, 1, 13, 0, NULL); return 1; }
  if (IS("string") || IS("asciiz")) { data(ops, nops, 1, 1, 0, 0, NULL); return 1; }
  if (IS("abyte")) {
    if (nops < 1) { error("missing operand"); return 1; }
    data(ops + 1, nops - 1, 1, 1, -1, 0, pexpr(ops[0]));
    return 1;
  }
  if (IS("blk") || IS("bsz") || IS("dc") || IS("defs") || IS("ds") || IS("dsb") || IS("zmb") || IS("rmb")) {
    if (nops < 1) { error("missing operand"); return 1; }
    space(ops[0], nops > 1 ? ops[1] : NULL, 1);
    return 1;
  }
  if (IS("blkw") || IS("dsw")) { if (nops < 1) return 1; space(ops[0], nops > 1 ? ops[1] : NULL, 2); return 1; }
  if (IS("fill") || IS("reserve") || IS("spc")) { if (nops < 1) return 1; space(ops[0], "0", 1); return 1; }
  if (IS("byt")) { space("1", "0", 1); return 1; }
  if (IS("wrd")) { space("1", "0", 2); return 1; }
  if (IS("align")) { if (nops < 1 || !cexpr(ops[0], &v)) return 1; align_to((ival)1 << v); return 1; }
  if (IS("even")) { align_to(2); return 1; }
  if (IS("org")) {
    const char *p = skip_ws(rest);
    if (*p == '#') p++;
    if (!cexpr(p, &v)) return 1;
    if (cursec && (cursec->flags & SEC_ABS) && v >= cursec->org) {
      /* oracle: org inside an absolute section pads up to the new address */
      atom *a = add_atom(AT_RORG);
      a->u.rorg.target = new_num(v - cursec->org);
      return 1;
    }
    cursec = new_org_section(v);
    return 1;
  }
  if (IS("roffs")) { atom *a = add_atom(AT_RORG); a->u.rorg.target = pexpr(rest); return 1; }
  if (opt_sect && (IS("text") || IS("data") || IS("bss"))) {
    set_section(d, IS("text") ? "acrx" : IS("data") ? "adrw" : "aurw");
    return 1;
  }
  if (IS("section")) {
    char *n, *f = NULL;
    if (nops < 1) { error("missing operand"); return 1; }
    n = unq(ops[0]);
    if (nops > 1) f = unq(ops[1]);
    set_section(n, f);
    free(n); free(f);
    return 1;
  }
  if (IS("global") || IS("xdef") || IS("xref") || IS("xlib") || IS("extern") || IS("weak") || IS("local")) {
    int i;
    for (i = 0; i < nops; i++) {
      symbol *s = get_sym(ops[i], strlen(ops[i]));
      if (IS("local")) { s->flags &= ~SF_EXPORT; continue; }
      s->flags |= SF_EXPORT;
      if (IS("weak")) s->flags |= SF_WEAK;
    }
    return 1;
  }
  if (IS("set") && !lab) return 0;     /* e.g. the Z80 instruction */
  if (IS("equ") || IS("eq") || IS("set") || IS("=")) {
    symbol *s;
    if (!lab) { error("identifier expected"); return 1; }
    s = get_sym(lab, ll);
    if (!IS("set") && s->type != S_NONE && s->type != S_IMPORT && !(s->type == S_SET && s->flags & SF_INTERNAL)) {
      error("label <%.*s> has already been defined", (int)ll, lab); return 1;
    }
    if (IS("set")) { s->type = S_SET; s->e = simplify_expr(pexpr(rest)); }
    else { s->type = S_EXPR; s->e = pexpr(rest); }
    s->flags |= SF_DEFINED;
    return 1;
  }
  if (IS("defc")) {
    const char *eq = strchr(rest, '=');
    symbol *s;
    const char *p = skip_ws(rest), *q;
    if (!eq) { error("= expected"); return 1; }
    q = eq;
    while (q > p && is_ws(q[-1])) q--;
    s = get_sym(p, q - p);
    s->type = S_EXPR; s->e = pexpr(eq + 1); s->flags |= SF_DEFINED;
    return 1;
  }
  if (IS("include")) { char *f = unq(rest); push_file(f); free(f); return 1; }
  if (IS("incdir")) { char *f = unq(rest); add_incpath(f); free(f); return 1; }
  if (IS("incbin") || IS("binary") || IS("mdat")) {
    char *f, *path; FILE *fp; atom *a; long sz; ival off = 0, cnt = -1;
    if (nops < 1) return 1;
    f = unq(ops[0]);
    if (nops > 1) cexpr(ops[1], &off);
    if (nops > 2) cexpr(ops[2], &cnt);
    path = find_include(f, 1);
    if (!path || !(fp = fopen(path, "rb"))) { error("cannot open binary file \"%s\"", f); return 1; }
    fseek(fp, 0, SEEK_END); sz = ftell(fp);
    if (off > sz) off = sz;
    sz -= (long)off;
    if (cnt >= 0 && cnt < sz) sz = (long)cnt;
    fseek(fp, (long)off, SEEK_SET);
    a = add_atom(AT_BYTES);
    a->u.bytes.d = xmalloc(sz ? sz : 1);
    a->u.bytes.n = fread(a->u.bytes.d, 1, sz, fp);
    a->size = a->u.bytes.n;
    fclose(fp);
    return 1;
  }
  if (IS("rept") || IS("repeat")) {
    cexpr(rest, &v);
    collecting = 1; rept_nest = 0; rept_n = 0; rept_count = v < 0 ? 0 : (int)v;
    return 1;
  }
  if (IS("macro") || IS("mac")) {
    if (lab) { start_macro(lab, ll, rest); return 1; }
    {
      const char *p = skip_ws(rest), *q = p;
      while (*q && !is_ws(*q) && *q != ',') q++;
      start_macro(p, q - p, *q ? q + 1 : q);
    }
    return 1;
  }
  if (IS("endm") || IS("endmac") || IS("endmacro") || IS("endr") || IS("endrep") || IS("endrepeat")) {
    error("unexpected \"%s\"", d); return 1;
  }
  if (IS("end")) { end_assembly(); return 1; }
  if (IS("fail")) { error("%s", rest); return 1; }
  if (IS("assert")) {
    if (nops >= 1 && cexpr(ops[0], &v) && !v) error("assertion failed%s%s", nops > 1 ? ": " : "", nops > 1 ? ops[1] : "");
    return 1;
  }
  if (IS("inline")) { inline_depth++; inline_id++; free(last_global); last_global = strprintf("\x1e%d", inline_id); return 1; }
  if (IS("einline")) { if (inline_depth) inline_depth--; free(last_global); last_global = strprintf("\x1e%d", ++inline_id); return 1; }
  if (IS("list") || IS("nolist")) return 1;
#undef IS
  return 0;
}

/* ------------------------------------------------------------ lines */
static char *strip_comment(const char *l)
{
  const char *p = l, *e;
  if (opt_ast && *l == '*') return xstrdup("");
  while (*p) {
    if (quote_at(l, p)) { p = skip_quote(l, p); continue; }
    if (*p == ';') break;
    p++;
  }
  e = p;
  while (e > l && is_ws(e[-1])) e--;
  return xstrndup(l, e - l);
}

static int is_labchar(int c) { return isalnum(c) || c == '_' || c == '.' || c == '$' || c == '@'; }

static void statement(const char *w0, const char *lab, size_t ll, int *labdone)
{
  const char *p = skip_ws(w0), *w = p;
  size_t l;
  char **ops; int nops;
  char d[32];
  const char *dn;
  if (!*p) return;
  if (*p == '=') l = 1, p++;
  else { while (*p && !is_ws(*p) && *p != '=') p++; l = p - w; }
  if (l > 1 && w[l - 1] == ':' && cond_active()) {
    /* a label that does not start in the first column */
    if (lab && !*labdone) { define_label(lab, ll); *labdone = 1; }
    define_label(w, l - 1);
    statement(p, NULL, 0, labdone);
    return;
  }
  dn = (l == 1 && *w == '=') ? "=" : dirname_of(w, l, d, sizeof d);
  /* conditionals (also while skipping) */
  if (dn) {
    if (is_cond(dn)) { if (cond_active()) cond_push(cond_value(dn, skip_ws(p))); else cond_push(0); *labdone = 1; return; }
    if (!stricmp_(dn, "else") || !stricmp_(dn, "el")) { cond_else(); *labdone = 1; return; }
    if (!stricmp_(dn, "endif") || !stricmp_(dn, "fi") || (!stricmp_(dn, "ei") && !is_z80())) { cond_end(); *labdone = 1; return; }
  }
  if (!cond_active()) { *labdone = 1; return; }
  p = skip_ws(p);
  if (*p == '=' && dn != NULL && !(l == 1 && *w == '=')) {
    /* "label op = expr" is not a thing; "sym = expr" without label column */
  }
  if (lab && !*labdone && !(dn && (!stricmp_(dn, "equ") || !stricmp_(dn, "eq") || !stricmp_(dn, "set") ||
                                   !strcmp(dn, "=") || !stricmp_(dn, "macro") || !stricmp_(dn, "mac")))) {
    define_label(lab, ll);
    *labdone = 1;
  }
  if (dn && (!stricmp_(dn, "equ") || !stricmp_(dn, "eq") || !stricmp_(dn, "set") || !strcmp(dn, "=") ||
             !stricmp_(dn, "macro") || !stricmp_(dn, "mac")))
    *labdone = 1;
  nops = split_args(p, &ops);
  if (dn && directive(dn, p, ops, nops, lab, ll)) { free_args(ops, nops); return; }
  {
    macro *m = find_macro(w, l);
    if (m) {
      char **args = xcalloc(m->nargnames + nops + 1, sizeof(char *));
      int i, n = nops > m->nargnames ? nops : m->nargnames;
      for (i = 0; i < n; i++) args[i] = xstrdup(i < nops ? ops[i] : "");
      push_macro(m, args, n, NULL);
      free_args(ops, nops);
      return;
    }
  }
  if (!cpu->instruction(w, l, NULL, ops, nops, 0))
    error("unknown mnemonic <%.*s>", (int)l, w);
  free_args(ops, nops);
}

static void os_parse_line(const char *raw)
{
  char *line;
  const char *p, *lab = NULL;
  size_t ll = 0;
  int labdone = 0;
  line_anchor = NULL;
  stmt_sec = cursec;
  stmt_prev = cursec ? cursec->last : NULL;
  if (defmacro || collecting) { collect(raw); return; }
  line = strip_comment(raw);
  p = line;
  if (*p == pcchar && skip_ws(p + 1)[0] == '=') {
    /* *=expr sets the origin */
    ival v;
    if (cond_active() && cexpr(skip_ws(p + 1) + 1, &v)) cursec = new_org_section(v);
    free(line);
    return;
  }
  if (*p && !is_ws(*p)) {
    const char *q = p;
    while (is_labchar((unsigned char)*q)) q++;
    if (q > p) { lab = p; ll = q - p; }
    p = q;
    if (*p == ':') p++;
  }
  /* statements, separated by ':' on the Z80 */
  for (;;) {
    const char *s = skip_ws(p), *e = s;
    char *st;
    int par = 0;
    {
      /* "label:" that does not start in the first column */
      const char *q = s;
      while (is_labchar((unsigned char)*q)) q++;
      if (q > s && *q == ':' && (q[1] == 0 || is_ws(q[1]))) {
        if (cond_active()) {
          if (lab && !labdone) { define_label(lab, ll); labdone = 1; }
          define_label(s, q - s);
        }
        p = q + 1;
        continue;
      }
    }
    if (is_z80()) {
      while (*e) {
        if (quote_at(line, e)) { e = skip_quote(line, e); continue; }
        if (*e == '(') par++;
        else if (*e == ')' && par) par--;
        else if (*e == ':' && !par) break;
        e++;
      }
    } else
      e = s + strlen(s);
    st = xstrndup(s, e - s);
    statement(st, lab, ll, &labdone);
    free(st);
    if (!*e) break;
    p = e + 1;
    stmt_prev = cursec ? cursec->last : NULL;
    stmt_sec = cursec;
    line_anchor = NULL;
  }
  if (lab && !labdone && cond_active()) define_label(lab, ll);
  free(line);
}

static void os_init(void)
{
  syntax_number = os_number;
  syntax_ident = os_ident;
  syntax_pcexpr = os_pc;
  if (is_z80()) pcchar = '$';
  opt_esc = 0;     /* oracle: no escape sequences by default */
  { extern int char_bs_pair; char_bs_pair = 1; }
  ident_dots = 1;
  { extern const char *default_sec_name; default_sec_name = ".text"; }
  macro_id_plain = 0;
  sym_reptn = sym_get("REPTN");
  set_internal(sym_reptn, -1);
  sym_narg = sym_get("NARG");
  sym_carg = sym_get("CARG");
  sym_reptn->flags |= SF_INTERNAL; sym_narg->flags |= SF_INTERNAL; sym_carg->flags |= SF_INTERNAL;
}

static void os_finish(void)
{
  if (defmacro) error("missing endm");
  if (collecting) error("missing endr");
  if (cond_level) error("missing endif");
}

static int os_option(const char *o)
{
  if (!strcmp(o, "-dotdir")) { opt_dotdir = 1; return 1; }
  if (!strcmp(o, "-ldots")) { opt_ldots = 1; return 1; }
  if (!strcmp(o, "-autoexp")) { opt_autoexp = 1; return 1; }
  if (!strcmp(o, "-ast")) { opt_ast = 1; return 1; }
  if (!strcmp(o, "-noc")) { opt_noc = 1; return 1; }
  if (!strcmp(o, "-noi")) { opt_noi = 1; return 1; }
  if (!strcmp(o, "-sect")) { opt_sect = 1; return 1; }
  if (!strcmp(o, "-i")) return 1;
  if (!strcmp(o, "-noesc")) { opt_esc = 0; return 1; }
  return 0;
}

syntax_module syntax_oldstyle = { "oldstyle", os_init, os_parse_line, os_finish, os_option };
