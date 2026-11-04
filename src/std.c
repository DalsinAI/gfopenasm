/* gfasm - "std" syntax module (GNU-as like; vasm "std" compatible).
 * MIT License.  Written from the vasm manual's chapter on the standard
 * syntax module. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

extern int opt_esc;
extern int ident_dots;
static int nodotneeded, std_align, std_ac;
static char commentchar = '#';
static char *last_global;
static symbol *line_anchor;
static section *stmt_sec;
static atom *stmt_prev;
static section *secstack[64];
static int nsecstack;

static macro *defmacro;
static int defmacro_nest;
static char **rept_lines; static int rept_n, rept_cap, rept_nest, rept_count, rept_kind;
static int collecting;
static char *irp_name; static char **irp_vals; static int irp_nvals;

/* ------------------------------------------------------------ numbers */
static expr *std_ident(const char *n, size_t len);

/* GNU-style numeric local labels "N:", referenced as "Nb" / "Nf" */
typedef struct numlab { unsigned long n; int count; struct numlab *next; } numlab;
static numlab *numlabs;
static numlab *numlab_get(unsigned long n)
{
  numlab *l;
  for (l = numlabs; l; l = l->next) if (l->n == n) return l;
  l = xcalloc(1, sizeof(*l));
  l->n = n;
  l->next = numlabs;
  numlabs = l;
  return l;
}
static char *numlab_name(unsigned long n, int count) { return strprintf("\x1d%lu.%d", n, count); }

static int std_number(const char **pp, expr **out)
{
  const char *p = *pp;
  uval v = 0;
  if (*p == '$' && !isalnum((unsigned char)p[1]) && p[1] != '_') {
    extern expr *(*syntax_pcexpr)(void);
    *pp = p + 1;
    *out = syntax_pcexpr();
    return 1;
  }
  if (!isdigit((unsigned char)*p)) return 0;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X') && isxdigit((unsigned char)p[2])) {
    p += 2;
    while (isxdigit((unsigned char)*p)) {
      int c = tolower((unsigned char)*p++);
      v = v * 16 + (isdigit(c) ? c - '0' : c - 'a' + 10);
    }
  } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) {
    p += 2;
    while (*p == '0' || *p == '1') v = v * 2 + (*p++ - '0');
  } else {
    const char *s = p;
    int octal = *p == '0';
    while (isdigit((unsigned char)*p)) p++;
    if (*p == '$' && !isalnum((unsigned char)p[1])) {      /* local label 1$ */
      *out = std_ident(s, p + 1 - s);
      *pp = p + 1;
      return *out ? 1 : -1;
    }
    if ((*p == '.' && isdigit((unsigned char)p[1])) || *p == 'e' || *p == 'E') {
      char *end;
      long double f = (long double)strtod(s, &end);
      if (end > p || *p == '.') { *pp = end; *out = new_flt(f); return 1; }
    }
    if ((*p == 'b' || *p == 'f' || *p == 'B' || *p == 'F') && !isalnum((unsigned char)p[1]) && p[1] != '_' && p[1] != '$') {
      /* numeric local label reference */
      unsigned long n = strtoul(s, NULL, 10);
      numlab *l = numlab_get(n);
      char *nm = numlab_name(n, l->count + (tolower((unsigned char)*p) == 'f'));
      symbol *y = sym_get(nm);
      free(nm);
      y->flags |= SF_REFD | SF_LOCAL;
      *pp = p + 1;
      *out = new_sym(y);
      return 1;
    }
    for (p = s; isdigit((unsigned char)*p); p++)
      v = octal ? v * 8 + (*p - '0') : v * 10 + (*p - '0');
  }
  *pp = p;
  *out = new_num(v <= 0xffffffffULL && cpu->addr_bits <= 32 ? (ival)(int32_t)(uint32_t)v : (ival)v);
  return 1;
}

/* ------------------------------------------------------------ symbols */
/* std: local labels are digits only, preceded by '.' or terminated by '$' */
static int is_local(const char *n, size_t len)
{
  size_t i;
  if (len > 1 && n[0] == '.') {
    for (i = 1; i < len; i++) if (!isdigit((unsigned char)n[i])) return 0;
    return 1;
  }
  if (len > 1 && n[len - 1] == '$') {
    for (i = 0; i < len - 1; i++) if (!isdigit((unsigned char)n[i])) return 0;
    return 1;
  }
  return 0;
}

static symbol *get_sym(const char *n, size_t len)
{
  char *m;
  symbol *s;
  if (is_local(n, len))
    m = strprintf("%s\x1f%.*s", last_global ? last_global : "", (int)len, n);
  else
    m = xstrndup(n, len);
  s = sym_lookup(m);
  if (!s) {
    s = sym_get(m);
    if (is_local(n, len)) { s->flags |= SF_LOCAL; s->dispname = xstrndup(n, len); }
  }
  free(m);
  return s;
}

static expr *std_ident(const char *n, size_t len)
{
  symbol *s;
  if (len == 1 && (n[0] == '$' || (n[0] == '.' && cpu->elf_machine == 3))) {
    extern expr *(*syntax_pcexpr)(void);
    return syntax_pcexpr();
  }
  s = get_sym(n, len);
  s->flags |= SF_REFD;
  if (s->type == S_SET && s->e) { s->flags |= SF_USED; return copy_expr(s->e); }
  return new_sym(s);
}

static expr *std_pc(void)
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
  {
    size_t i;
    for (i = 0; i < len && isdigit((unsigned char)n[i]); i++) ;
    if (i == len) {
      unsigned long num = strtoul(n, NULL, 10);
      numlab *l = numlab_get(num);
      char *nm = numlab_name(num, ++l->count);
      s = sym_get(nm);
      free(nm);
      s->flags |= SF_LOCAL;
      if (s->type != S_NONE) { error("symbol <%.*s> redefined", (int)len, n); return; }
      add_label_atom(s);
      return;
    }
  }
  if (!is_local(n, len)) { free(last_global); last_global = xstrndup(n, len); }
  s = get_sym(n, len);
  if (s->type != S_NONE && s->type != S_IMPORT) { error("symbol <%.*s> redefined", (int)len, n); return; }
  if (s->type == S_IMPORT) s->flags |= SF_EXPORT;
  add_label_atom(s);
}

/* ------------------------------------------------------------ helpers */
static int split_args(const char *s, char ***out)
{
  char **v = NULL;
  int n = 0, par = 0;
  char q = 0;
  const char *st = s, *p = s;
  s = skip_ws(s);
  if (!*s) { *out = NULL; return 0; }
  st = p = s;
  for (;; p++) {
    if (!*p || (!q && !par && *p == ',')) {
      const char *a = skip_ws(st), *b = p;
      while (b > a && is_ws(b[-1])) b--;
      v = xrealloc(v, (n + 1) * sizeof(char *));
      v[n++] = xstrndup(a, b - a);
      if (!*p) break;
      st = p + 1;
      continue;
    }
    if (q) { if (*p == '\\' && p[1]) p++; else if (*p == q) q = 0; }
    else if (*p == '"' || *p == '\'') q = *p;
    else if (*p == '(' || *p == '[' || *p == '{') par++;
    else if (*p == ')' || *p == ']' || *p == '}') par--;
  }
  *out = v;
  return n;
}

static void free_args(char **v, int n) { int i; for (i = 0; i < n; i++) free(v[i]); free(v); }

static expr *pexpr(const char *s)
{
  const char *p = skip_ws(s);
  expr *e = parse_expr(&p);
  if (!e) { error("expression syntax error"); return new_num(0); }
  if (*skip_ws(p)) error("scratch at end of line");
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
  if (n >= 2 && (s[0] == '"' || s[0] == '<') && (s[n - 1] == '"' || s[n - 1] == '>'))
    return xstrndup(s + 1, n - 2);
  return xstrndup(s, n);
}

/* string with C escapes -> bytes */
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
      case 'x': case 'X': {
        int x = 0;
        while (isxdigit((unsigned char)*s)) { int d = tolower((unsigned char)*s++); x = x * 16 + (isdigit(d) ? d - '0' : d - 'a' + 10); }
        c = x & 255;
        break;
      }
      default:
        if (c >= '0' && c <= '7') {
          int x = c - '0', k = 1;
          while (k < 3 && *s >= '0' && *s <= '7') { x = x * 8 + (*s++ - '0'); k++; }
          c = x & 255;
        }
      }
    }
    b[n++] = (char)c;
  }
  *out = b;
  return n;
}

static void align_data(int n)
{
  if ((cpu->data_align || std_align) && n > 1) {
    atom *a = add_atom(AT_ALIGN);
    a->u.align.align = n;
  }
}

/* one atom per item, so that "$" in an item is the address of that item */
static void data(char **ops, int nops, int esize, int ftype, int strings, int zero, int ua)
{
  int i;
  if (!ua) align_data(esize > 8 ? 8 : esize);
  for (i = 0; i < nops; i++) {
    const char *o = ops[i];
    atom *a;
    dataitem *it;
    expr *e = NULL;
    if (!(strings && *o == '"')) {
      default_section();
      line_anchor = NULL;
      stmt_sec = cursec;
      stmt_prev = cursec->last;
      e = pexpr(o);
    }
    a = add_atom(AT_DATA);
    a->u.data.esize = esize;
    a->u.data.ftype = ftype;
    a->u.data.items = xcalloc(2, sizeof(dataitem));
    it = &a->u.data.items[0];
    a->u.data.n = 1;
    if (!e) {
      it->slen = parse_string(o, &it->str);
      a->size = it->slen;
      if (zero && i == nops - 1) {
        dataitem *z = &a->u.data.items[a->u.data.n++];
        z->str = xcalloc(1, 1);
        z->slen = 1;
        a->size++;
      }
      continue;
    }
    it->e = e;
    a->size = esize;
  }
}

static void space(const char *cnt, const char *fill)
{
  atom *a = add_atom(AT_SPACE);
  a->u.space.count = pexpr(cnt);
  a->u.space.esize = 1;
  if (fill) a->u.space.fill = pexpr(fill);
  else a->u.space.uninit = 1;
}

static void do_align(ival bytes, char **ops, int nops, int fillsize)
{
  atom *a = add_atom(AT_ALIGN);
  a->u.align.align = bytes;
  if (nops > 1 && *skip_ws(ops[1])) { a->u.align.fill = pexpr(ops[1]); a->u.align.fillsize = fillsize; }
  if (nops > 2 && *skip_ws(ops[2])) { ival m = 0; cexpr(ops[2], &m); a->u.align.maxpad = m; }
}

static void set_section(const char *name, const char *flags, const char *type)
{
  int st = ST_DATA;
  uint32_t mem = 0;
  section *s;
  if (flags) {
    if (strchr(flags, 'x') || strchr(flags, 'c')) st = ST_CODE;
    else if (strchr(flags, 'u')) st = ST_BSS;
    if (strchr(flags, 'C')) mem = 0x40000000;
    if (strchr(flags, 'F')) mem = 0x80000000;
  } else {
    static const char *pre[][2] = { { ".text", "acrx" }, { ".data", "adrw" }, { ".bss", "aurw" },
      { ".rodata", "adr" }, { ".sdata", "adrw" }, { ".sdata2", "adr" }, { ".sbss", "aurw" },
      { ".tocd", "adrw" }, { ".dpage", "adrw" }, { ".stab", "dr" }, { ".stabstr", "dr" }, { NULL, NULL } };
    int k;
    for (k = 0; pre[k][0]; k++)
      if (!strcmp(name, pre[k][0])) { set_section(name, pre[k][1], type); return; }
    if (!strcmp(name, ".text") || !strncmp(name, ".text.", 6) || !strcmp(name, ".init") || !strcmp(name, ".fini")) st = ST_CODE;
    else if (!strcmp(name, ".bss") || !strcmp(name, ".sbss") || !strncmp(name, ".bss.", 5) || !strncmp(name, ".sbss.", 6)) st = ST_BSS;
  }
  if (type && (!strcmp(type, "nobits"))) st = ST_BSS;
  s = find_section(name, st, mem);
  {
    int ef = 0;
    if (flags) {
      if (strchr(flags, 'a')) ef |= 2;
      if (strchr(flags, 'w')) ef |= 1;
      if (strchr(flags, 'x')) ef |= 4;
    }
    if (s && flags) s->elfflags = ef;
    if (!s) {
      section *t;
      for (t = sec_first; t; t = t->next)
        if (!(t->flags & (SEC_ABS | SEC_OFFSET)) && !strcmp(t->name, name)) { s = t; break; }
    }
    if (!s) { s = new_section(name, st, mem); s->elfflags = flags ? ef : 0; }
    cursec = s;
    return;
  }
  if (!s) {
    section *t;
    for (t = sec_first; t; t = t->next)
      if (!(t->flags & (SEC_ABS | SEC_OFFSET)) && !strcmp(t->name, name)) { s = t; break; }
  }
  if (!s) s = new_section(name, st, mem);
  cursec = s;
}

static void section_dir(char **ops, int nops)
{
  char *name, *flags = NULL, *type = NULL;
  if (nops < 1) { error("section name expected"); return; }
  name = unq(ops[0]);
  if (nops > 1) {
    if (ops[1][0] == '"') flags = unq(ops[1]);
    else if (ops[1][0] == '@' || ops[1][0] == '%') type = xstrdup(ops[1] + 1);
  }
  if (nops > 2 && (ops[2][0] == '@' || ops[2][0] == '%')) type = xstrdup(ops[2] + 1);
  set_section(name, flags, type);
  free(name); free(flags); free(type);
}

/* ------------------------------------------------------------ lines */
static int is_cond(const char *d)
{
  static const char *c[] = { "if", "ifeq", "ifne", "ifgt", "ifge", "iflt", "ifle", "ifb", "ifnb",
                             "ifdef", "ifndef", "ifc", "ifnc", NULL };
  int i;
  for (i = 0; c[i]; i++) if (!stricmp_(d, c[i])) return 1;
  return 0;
}

static int cond_value(const char *d, const char *rest)
{
  ival v = 0;
  if (!stricmp_(d, "ifb")) return !*skip_ws(rest);
  if (!stricmp_(d, "ifnb")) return *skip_ws(rest) != 0;
  if (!stricmp_(d, "ifdef") || !stricmp_(d, "ifndef")) {
    const char *p = skip_ws(rest), *q = p;
    symbol *s;
    int r;
    while (*q && !is_ws(*q)) q++;
    s = sym_lookup(xstrndup(p, q - p));
    r = s && s->type != S_NONE && s->type != S_IMPORT;
    return tolower((unsigned char)d[2]) == 'd' ? r : !r;
  }
  if (!stricmp_(d, "ifc") || !stricmp_(d, "ifnc")) {
    char **a; int n = split_args(rest, &a), r = 0;
    if (n >= 2) { char *x = unq(a[0]), *y = unq(a[1]); r = !strcmp(x, y); free(x); free(y); }
    free_args(a, n);
    return tolower((unsigned char)d[2]) == 'c' ? r : !r;
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

static char *strip_comment(const char *l)
{
  const char *p = l;
  char q = 0;
  const char *f = skip_ws(l);
  if (*f == '#' || *f == '*' || (*f == ';' && commentchar == ';')) return xstrdup("");
  for (; *p; p++) {
    if (q) { if (*p == '\\' && p[1]) p++; else if (*p == q) q = 0; continue; }
    if (*p == '"') { q = '"'; continue; }
    if (*p == '\'' && p[1] && p[2] == '\'') { p += 2; continue; }   /* 'c' */
    if (*p == commentchar || (commentchar == '#' && *p == ';' && 0)) break;
  }
  {
    const char *e = p;
    while (e > l && is_ws(e[-1])) e--;
    return xstrndup(l, e - l);
  }
}

static int word_is(const char *w, size_t l, const char *d)
{
  if (*w == '.') { w++; l--; }
  else if (!nodotneeded) return 0;
  return strlen(d) == l && !strnicmp_(w, d, l);
}

static void collect(const char *line)
{
  const char *p = skip_ws(line);
  const char *w = p;
  size_t l;
  while (*p && !is_ws(*p)) p++;
  l = p - w;
  if (defmacro) {
    if (word_is(w, l, "endm")) { if (!defmacro_nest) { defmacro = NULL; return; } defmacro_nest--; }
    else if (word_is(w, l, "macro")) defmacro_nest++;
    macro_add_line(defmacro, line);
    return;
  }
  if (word_is(w, l, "endr")) {
    if (!rept_nest) {
      collecting = 0;
      if (rept_kind == 0) push_rept(rept_lines, rept_n, rept_count);
      else push_irp(rept_lines, rept_n, irp_name, irp_vals, irp_nvals);
      return;
    }
    rept_nest--;
  } else if (word_is(w, l, "rept") || word_is(w, l, "irp") || word_is(w, l, "irpc")) rept_nest++;
  if (rept_n >= rept_cap) { rept_cap = rept_cap * 2 + 16; rept_lines = xrealloc(rept_lines, rept_cap * sizeof(char *)); }
  rept_lines[rept_n++] = xstrdup(line);
}

static int directive(const char *d, const char *rest, char **ops, int nops)
{
#define IS(x) (!stricmp_(d, x))
  ival v;
  if (IS("byte") || IS("ascii")) { data(ops, nops, 1, 0, 1, 0, 1); return 1; }
  if (IS("asciz") || IS("asciiz") || IS("string")) { data(ops, nops, 1, 0, 1, 1, 1); return 1; }
  if (IS("half") || IS("short") || IS("word") || IS("hword")) { data(ops, nops, 2, 0, 0, 0, 0); return 1; }
  if (IS("2byte") || IS("uahalf") || IS("uashort") || IS("uaword")) { data(ops, nops, 2, 0, 0, 0, 1); return 1; }
  if (IS("long") || IS("int")) { data(ops, nops, 4, 0, 0, 0, 0); return 1; }
  if (IS("4byte") || IS("ualong")) { data(ops, nops, 4, 0, 0, 0, 1); return 1; }
  if (IS("quad")) { data(ops, nops, 8, 0, 0, 0, 0); return 1; }
  if (IS("8byte") || IS("uaquad")) { data(ops, nops, 8, 0, 0, 0, 1); return 1; }
  if (IS("float") || IS("single")) { data(ops, nops, 4, 's', 0, 0, 0); return 1; }
  if (IS("double")) { data(ops, nops, 8, 'd', 0, 0, 0); return 1; }
  if (IS("space") || IS("skip") || IS("zero")) {
    if (nops < 1) { error("missing operand"); return 1; }
    space(ops[0], nops > 1 ? ops[1] : NULL);
    return 1;
  }
  if (IS("align") || IS("p2align") || IS("p2alignw") || IS("p2alignl") ||
      IS("balign") || IS("balignw") || IS("balignl")) {
    int bytes = IS("balign") || IS("balignw") || IS("balignl") || (IS("align") && cpu->elf_machine == 3);
    if (nops < 1 || !cexpr(ops[0], &v)) return 1;
    do_align(bytes ? v : ((ival)1 << v), ops, nops,
             (d[strlen(d) - 1] == 'w' || d[strlen(d) - 1] == 'W') ? 2 : (d[strlen(d) - 1] == 'l' || d[strlen(d) - 1] == 'L') ? 4 : 1);
    return 1;
  }
  if (IS("text")) { set_section(".text", "acrx", NULL); return 1; }
  if (IS("data")) { set_section(".data", "adrw", NULL); return 1; }
  if (IS("bss")) { set_section(".bss", "aurw", NULL); return 1; }
  if (IS("rodata")) { set_section(".rodata", "adr", NULL); return 1; }
  if (IS("sdata")) { set_section(".sdata", "adrw", NULL); return 1; }
  if (IS("sdata2")) { set_section(".sdata2", "adr", NULL); return 1; }
  if (IS("sbss")) { set_section(".sbss", "aurw", NULL); return 1; }
  if (IS("tocd")) { set_section(".tocd", "adrw", NULL); return 1; }
  if (IS("dpage")) { set_section(".dpage", "adrw", NULL); return 1; }
  if (IS("stab")) { set_section(".stab", "dr", NULL); return 1; }
  if (IS("stabstr")) { set_section(".stabstr", "dr", NULL); return 1; }
  if (IS("section")) { section_dir(ops, nops); return 1; }
  if (IS("pushsection")) { if (nsecstack < 64) secstack[nsecstack++] = default_section(); section_dir(ops, nops); return 1; }
  if (IS("popsection")) { if (nsecstack) cursec = secstack[--nsecstack]; return 1; }
  if (IS("org")) {
    if (nops < 1 || !cexpr(ops[0], &v)) return 1;
    if (!cursec) cursec = new_org_section(v);
    else { atom *a = add_atom(AT_RORG); a->u.rorg.target = new_num(v); if (nops > 1) a->u.rorg.fill = pexpr(ops[1]); }
    return 1;
  }
  if (IS("global") || IS("globl") || IS("extern") || IS("weak") || IS("local")) {
    int i;
    for (i = 0; i < nops; i++) {
      symbol *s = get_sym(ops[i], strlen(ops[i]));
      if (IS("local")) { s->flags &= ~SF_EXPORT; continue; }
      s->flags |= SF_EXPORT;
      if (IS("weak")) s->flags |= SF_WEAK;
    }
    return 1;
  }
  if (IS("comm") || IS("lcomm")) {
    symbol *s;
    ival al = 0;
    if (nops < 2) { error("missing operand"); return 1; }
    s = get_sym(ops[0], strlen(ops[0]));
    cexpr(ops[1], &v);
    if (nops > 2) cexpr(ops[2], &al);
    if (IS("comm")) {
      s->type = S_COMMON; s->val = v;
      /* vasm: ARM always 4, x86 always 1 */
      s->align = al ? al : cpu->elf_machine == 3 ? 1 : (v < 8 || cpu->elf_machine == 40) ? 4 : 8;
      s->flags |= SF_EXPORT | SF_DEFINED;
    } else {
      section *save = cursec;
      set_section(".bss", "aurw", NULL);
      if (al) { atom *a = add_atom(AT_ALIGN); a->u.align.align = al; }   /* vasm: bytes */
      add_label_atom(s);
      s->stype = 1;
      s->ssize = v;
      { atom *a = add_atom(AT_SPACE); a->u.space.count = new_num(v); a->u.space.esize = 1; a->u.space.uninit = 1; }
      cursec = save;
    }
    return 1;
  }
  if (IS("equ") || IS("set") || IS("equiv")) {
    symbol *s;
    if (nops < 2) { error("missing operand"); return 1; }
    s = get_sym(ops[0], strlen(ops[0]));
    if (IS("equiv") && s->type != S_NONE) { error("symbol <%s> redefined", ops[0]); return 1; }
    s->type = S_SET;
    s->e = simplify_expr(pexpr(ops[1]));
    s->flags |= SF_DEFINED;
    return 1;
  }
  if (IS("include")) { char *f = unq(rest); push_file(f); free(f); return 1; }
  if (IS("incdir")) { char *f = unq(rest); add_incpath(f); free(f); return 1; }
  if (IS("incbin")) {
    char *f, *path; FILE *fp; atom *a; long sz;
    if (nops < 1) return 1;
    f = unq(ops[0]);
    path = find_include(f, 1);
    if (!path || !(fp = fopen(path, "rb"))) { error("can't open binary file \"%s\"", f); return 1; }
    fseek(fp, 0, SEEK_END); sz = ftell(fp); fseek(fp, 0, SEEK_SET);
    a = add_atom(AT_BYTES);
    a->u.bytes.d = xmalloc(sz ? sz : 1);
    a->u.bytes.n = fread(a->u.bytes.d, 1, sz, fp);
    a->size = a->u.bytes.n;
    fclose(fp);
    return 1;
  }
  if (IS("rept")) {
    cexpr(rest, &v);
    collecting = 1; rept_kind = 0; rept_nest = 0; rept_n = 0; rept_count = v < 0 ? 0 : (int)v;
    return 1;
  }
  if (IS("irp") || IS("irpc")) {
    int i;
    if (nops < 1) return 1;
    collecting = 1; rept_kind = 1; rept_nest = 0; rept_n = 0;
    irp_name = xstrdup(ops[0]);
    if (IS("irp")) {
      irp_nvals = nops > 1 ? nops - 1 : 1;
      irp_vals = xcalloc(irp_nvals + 1, sizeof(char *));
      for (i = 0; i < irp_nvals; i++) irp_vals[i] = nops > 1 ? unq(ops[i + 1]) : xstrdup("");
    } else {
      char *s = nops > 1 ? unq(ops[1]) : xstrdup("");
      irp_nvals = (int)strlen(s);
      irp_vals = xcalloc(irp_nvals + 1, sizeof(char *));
      for (i = 0; i < irp_nvals; i++) irp_vals[i] = xstrndup(s + i, 1);
      if (!irp_nvals) { irp_nvals = 1; irp_vals[0] = xstrdup(""); }
    }
    return 1;
  }
  if (IS("macro")) {
    const char *p = skip_ws(rest), *q = p;
    char **a; int n, i;
    while (*q && !is_ws(*q) && *q != ',') q++;
    defmacro = new_macro(xstrndup(p, q - p));
    defmacro_nest = 0;
    p = skip_ws(q);
    if (*p == ',') p++;
    /* argument names: separated by commas or blanks */
    {
      char *t = xstrdup(p);
      for (i = 0; t[i]; i++) if (is_ws(t[i])) t[i] = ',';
      n = split_args(t, &a);
      free(t);
    }
    defmacro->argnames = xcalloc(n + 1, sizeof(char *));
    defmacro->argdefs = xcalloc(n + 1, sizeof(char *));
    for (i = 0; i < n; i++) if (*a[i]) {
      char *eq = strchr(a[i], '=');
      if (eq) { *eq = 0; defmacro->argdefs[defmacro->nargnames] = xstrdup(eq + 1); }
      defmacro->argnames[defmacro->nargnames++] = a[i];
    }
    return 1;
  }
  if (IS("endm") || IS("endr")) { error("unexpected .%s", d); return 1; }
  if (IS("mexit") || IS("exitm")) { exit_macro(); return 1; }
  if (IS("end")) { end_assembly(); return 1; }
  if (IS("err")) { error("%s", rest); return 1; }
  if (IS("abort")) fatal("assembly aborted: %s", rest);
  if (IS("fail")) { cexpr(rest, &v); if (v >= 500) warning(".fail %lld encountered", (long long)v); else error(".fail %lld encountered", (long long)v); return 1; }
  if (IS("file") || IS("ident")) { if (IS("file") && nops) { extern char *idnt_name; free(idnt_name); idnt_name = unq(ops[0]); elf_file_name = unq(ops[0]); } return 1; }
  if (IS("type") && nops >= 2) {
    symbol *s = get_sym(ops[0], strlen(ops[0]));
    const char *t = skip_ws(ops[1]);
    if (*t == '@' || *t == '%') t++;
    if (!strnicmp_(t, "function", 8)) s->stype = 2;
    else if (!strnicmp_(t, "object", 6)) s->stype = 1;
    else { ival v = 0; cexpr(t, &v); s->stype = (int)v; }
    return 1;
  }
  if (IS("size") && nops >= 2) {
    symbol *s = get_sym(ops[0], strlen(ops[0]));
    s->size_e = pexpr(ops[1]);
    return 1;
  }
  if (IS("list") || IS("nolist") || IS("size") || IS("type") || IS("stabs") || IS("stabn") ||
      IS("stabd") || IS("swbeg") || IS("loc") || IS("cfi_startproc") || IS("cfi_endproc") ||
      IS("previous") || IS("machine") || IS("gnu_attribute") || IS("version") || IS("p2alignl_"))
    return 1;
#undef IS
  return 0;
}

static void std_parse_line(const char *raw)
{
  char *line;
  const char *p, *w;
  size_t l;
  char **ops; int nops;
  char dname[40];
  line_anchor = NULL;
  stmt_sec = cursec;
  stmt_prev = cursec ? cursec->last : NULL;
  if (defmacro || collecting) { collect(raw); return; }
  line = strip_comment(raw);
  p = skip_ws(line);
  /* labels (any number, each terminated by ':') */
  for (;;) {
    const char *q = p;
    if (isalpha((unsigned char)*q) || *q == '_' || *q == '.' || *q == '$' || *q == '@' || isdigit((unsigned char)*q)) {
      while (isalnum((unsigned char)*q) || *q == '_' || *q == '.' || *q == '$' || *q == '@') q++;
      if (*q == ':' && q > p) {
        if (cond_active()) define_label(p, q - p);
        p = skip_ws(q + 1);
        continue;
      }
    }
    break;
  }
  if (!*p) { free(line); return; }
  w = p;
  while (*p && !is_ws(*p) && *p != '=') p++;
  l = p - w;
  /* conditionals first (also while skipping) */
  {
    const char *dw = w;
    size_t dl = l;
    if (*dw == '.') { dw++; dl--; }
    if (dl < sizeof(dname)) { memcpy(dname, dw, dl); dname[dl] = 0; } else dname[0] = 0;
    if (*w == '.' || nodotneeded) {
      if (is_cond(dname)) { if (cond_active()) cond_push(cond_value(dname, skip_ws(p))); else cond_push(0); free(line); return; }
      if (!stricmp_(dname, "else")) { cond_else(); free(line); return; }
      if (!stricmp_(dname, "elseif")) { ival v = 0; if (cond_parent_active()) cexpr(skip_ws(p), &v); cond_elseif(v != 0); free(line); return; }
      if (!stricmp_(dname, "endif")) { cond_end(); free(line); return; }
    }
  }
  if (!cond_active()) { free(line); return; }
  /* symbol = expr */
  {
    const char *e = skip_ws(p);
    if (*e == '=' && e[1] != '=') {
      symbol *s = get_sym(w, l);
      s->type = S_SET;
      s->e = simplify_expr(pexpr(e + 1));
      s->flags |= SF_DEFINED;
      free(line);
      return;
    }
  }
  p = skip_ws(p);
  nops = split_args(p, &ops);
  if ((*w == '.' || nodotneeded) && dname[0] && directive(dname, p, ops, nops)) goto done;
  if (cpu->directive && *w == '.' && dname[0]) {
    char dd[48];
    snprintf(dd, sizeof dd, ".%s", dname);
    if (cpu->directive(dd, p)) goto done;
  }
  {
    macro *m = find_macro(w, l);
    if (m) {
      char **args = xcalloc(m->nargnames + nops + 1, sizeof(char *));
      int i, n = nops > m->nargnames ? nops : m->nargnames;
      for (i = 0; i < n; i++) {
        if (i < nops && *ops[i]) args[i] = xstrdup(ops[i]);
        else if (m->argdefs && i < m->nargnames && m->argdefs[i]) args[i] = xstrdup(m->argdefs[i]);
        else args[i] = xstrdup("");
      }
      push_macro(m, args, n, NULL);
      goto done;
    }
  }
  {
    const char *dot = memchr(w, '.', l);
    (void)dot;
    if (!cpu->instruction(w, l, NULL, ops, nops, 0))
      error("unknown mnemonic <%.*s>", (int)l, w);
  }
done:
  free_args(ops, nops);
  free(line);
}

static void std_init(void)
{
  extern expr *(*syntax_pcexpr)(void);
  syntax_number = std_number;
  syntax_ident = std_ident;
  syntax_pcexpr = std_pc;
  opt_esc = 1;
  ident_dots = 1;
  { extern const char *default_sec_name; default_sec_name = ".text"; }
  macro_id_plain = 1;
  commentchar = (cpu->elf_machine == 20 || cpu->elf_machine == 21 || cpu->elf_machine == 3) ? '#' : ';';
  sym_reptn = sym_get("REPTN");
  set_internal(sym_reptn, -1);
  sym_narg = sym_get("NARG");
  sym_carg = sym_get("CARG");
  sym_reptn->flags |= SF_INTERNAL; sym_narg->flags |= SF_INTERNAL; sym_carg->flags |= SF_INTERNAL;
}

static void std_finish(void)
{
  if (defmacro) error("missing .endm");
  if (collecting) error("missing .endr");
  if (cond_level) error("missing .endif");
}

static int std_option(const char *o)
{
  if (!strcmp(o, "-nodotneeded")) { nodotneeded = 1; return 1; }
  if (!strcmp(o, "-align")) { std_align = 1; return 1; }
  if (!strcmp(o, "-ac")) { std_ac = 1; return 1; }
  if (!strcmp(o, "-noesc")) { opt_esc = 0; return 1; }
  if (!strncmp(o, "-sdlimit=", 9)) return 1;
  return 0;
}

syntax_module syntax_std = { "std", std_init, std_parse_line, std_finish, std_option };
