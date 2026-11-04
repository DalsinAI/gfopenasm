/* gfasm - core: symbols, sections, atoms, passes, sources, macros,
 * conditional assembly.  MIT License. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

/* ------------------------------------------------------------ symbols */
#define HSIZE 65536
static symbol *symhash[HSIZE];
symbol *sym_first, *sym_last;
static int sym_order;
int opt_nocase;

static unsigned hashstr(const char *s)
{
  unsigned h = 2166136261u;
  while (*s) {
    int c = (unsigned char)*s++;
    if (opt_nocase) c = tolower(c);
    h = (h ^ c) * 16777619u;
  }
  return h & (HSIZE - 1);
}

symbol *sym_lookup(const char *name)
{
  symbol *s;
  for (s = symhash[hashstr(name)]; s; s = s->hnext)
    if (opt_nocase ? !stricmp_(s->name, name) : !strcmp(s->name, name))
      return s;
  return NULL;
}

symbol *sym_get(const char *name)
{
  symbol *s = sym_lookup(name);
  unsigned h;
  if (s) return s;
  s = xcalloc(1, sizeof(*s));
  s->name = xstrdup(name);
  s->dispname = s->name;
  s->type = S_NONE;
  h = hashstr(name);
  s->hnext = symhash[h];
  symhash[h] = s;
  s->order = ++sym_order;
  if (sym_last) sym_last->lnext = s; else sym_first = s;
  sym_last = s;
  return s;
}

symbol *sym_new_internal(void)
{
  static int n;
  char buf[32];
  symbol *s;
  snprintf(buf, sizeof(buf), " *%d", ++n);
  s = sym_get(buf);
  s->flags |= SF_INTERNAL;
  return s;
}

void sym_define_label(symbol *s, section *sec, atom *a)
{
  s->type = S_LABEL;
  s->sec = sec;
  s->atom = a;
  s->val = sec->last ? sec->size : 0;
  s->flags |= SF_DEFINED;
}

int sym_is_defined(symbol *s)
{
  return s && s->type != S_NONE && s->type != S_IMPORT;
}

void set_internal(symbol *s, ival v)
{
  if (!s) return;
  s->type = S_SET;
  s->e = new_num(v);
  s->flags |= SF_DEFINED;
}

/* ----------------------------------------------------------- sections */
section *sec_first, *sec_last, *cursec;
int default_sec_is_org;
const char *default_sec_name = "CODE";

static section *link_section(section *s)
{
  static int idx;
  s->idx = idx++;
  if (sec_last) sec_last->next = s; else sec_first = s;
  sec_last = s;
  return s;
}

section *find_section(const char *name, int type, uint32_t mem)
{
  section *s;
  for (s = sec_first; s; s = s->next) {
    if (s->flags & (SEC_ABS | SEC_OFFSET)) continue;
    if (opt_unnamed_sections) {
      if (s->type == type && s->memflags == mem) return s;
    } else if (!strcmp(s->name, name) && s->type == type && s->memflags == mem)
      return s;
  }
  return NULL;
}

section *new_section(const char *name, int type, uint32_t mem)
{
  section *s = xcalloc(1, sizeof(*s));
  s->name = xstrdup(name);
  s->type = type;
  s->memflags = mem;
  s->align = 1;
  return link_section(s);
}

section *new_org_section(ival org)
{
  static int n;
  section *s = xcalloc(1, sizeof(*s));
  s->name = strprintf("org%04d:%llx", ++n, (unsigned long long)(uint32_t)org);
  s->type = ST_CODE;
  s->flags = SEC_ABS;
  s->org = org;
  s->align = 1;
  return link_section(s);
}

section *default_section(void)
{
  if (!cursec) {
    if (default_sec_is_org)
      cursec = new_org_section(0);
    else
      cursec = new_section(default_sec_name, ST_CODE, 0);
  }
  return cursec;
}

atom *add_atom(int type)
{
  section *s = default_section();
  atom *a = xcalloc(1, sizeof(*a));
  a->type = type;
  a->pos = curpos;
  if (curpos.text) a->pos.text = xstrdup(curpos.text);
  if (s->last) s->last->next = a; else s->first = a;
  s->last = a;
  return a;
}

void add_label_atom(symbol *s)
{
  atom *a = add_atom(AT_LABEL);
  a->u.label.sym = s;
  sym_define_label(s, cursec, a);
}

/* ------------------------------------------------------------ floats */
void encode_float(int ftype, long double f, uint8_t *o)
{
  int i;
  if (ftype == 's') {
    float x = (float)f;
    uint32_t u;
    memcpy(&u, &x, 4);
    for (i = 0; i < 4; i++) o[i] = u >> (24 - 8 * i);
  } else if (ftype == 'd') {
    double x = (double)f;
    uint64_t u;
    memcpy(&u, &x, 8);
    for (i = 0; i < 8; i++) o[i] = u >> (56 - 8 * i);
  } else if (ftype == 'x') {
    int sign = signbit(f) ? 1 : 0;
    int ex, e;
    uint64_t mant = 0;
    long double m;
    memset(o, 0, 12);
    if (sign) f = -f;
    if (isnan(f)) {
      e = 0x7fff; mant = 0xffffffffffffffffULL;
    } else if (isinf(f)) {
      e = 0x7fff; mant = 0;
    } else if (f == 0) {
      e = 0; mant = 0;
    } else {
      m = frexpl(f, &ex);  /* 0.5 <= m < 1 */
      for (i = 0; i < 64; i++) {
        m *= 2;
        mant <<= 1;
        if (m >= 1) { mant |= 1; m -= 1; }
      }
      if (m > 0.5L || (m == 0.5L && (mant & 1))) {
        mant++;
        if (mant == 0) { mant = 0x8000000000000000ULL; ex++; }
      }
      e = ex - 1 + 16383;
      if (e <= 0) {   /* denormal */
        int sh = 1 - e;
        mant = sh < 64 ? mant >> sh : 0;
        e = 0;
      }
    }
    o[0] = (sign << 7) | ((e >> 8) & 0x7f);
    o[1] = e & 0xff;
    for (i = 0; i < 8; i++) o[4 + i] = mant >> (56 - 8 * i);
  } else if (ftype == 'p') {
    /* packed decimal: 17 significant digits of the double value */
    char buf[64], *q;
    int ex, k, d = 0;
    uint8_t dig[17];
    memset(o, 0, 12);
    if (f == 0) return;
    snprintf(buf, sizeof(buf), "%.16e", (double)f);
    q = buf;
    if (*q == '-') { o[0] |= 0x80; q++; }
    for (; *q && *q != 'e'; q++) if (*q >= '0' && *q <= '9' && d < 17) dig[d++] = *q - '0';
    ex = *q == 'e' ? atoi(q + 1) : 0;
    if (ex < 0) { o[0] |= 0x40; ex = -ex; }
    o[0] |= (ex / 100) % 10;
    o[1] = (((ex / 10) % 10) << 4) | (ex % 10);
    o[3] = dig[0];
    for (k = 1; k < 17; k++) {
      int pos = 4 + (k - 1) / 2;
      if ((k - 1) & 1) o[pos] |= dig[k]; else o[pos] |= dig[k] << 4;
    }
  }
}

/* ------------------------------------------------------- value output */
int final_pass, pass_no, grow_only;
int reloc_inplace = 1;

void add_reloc(section *s, ival off, int size, int type, section *tsec,
               symbol *tsym, ival addend)
{
  reloc *r = xcalloc(1, sizeof(*r));
  r->off = off;
  r->size = size;
  r->type = type;
  r->tsec = tsec;
  r->tsym = tsym;
  r->addend = addend;
  r->pos = curpos;
  if (s->rlast) s->rlast->next = r; else s->relocs = r;
  s->rlast = r;
  s->nrelocs++;
}

static void put_be(bbuf *b, uval v, int bytes)
{
  int i;
  if (cpu->big_endian)
    for (i = bytes - 1; i >= 0; i--) bb_byte(b, (int)(v >> (8 * i)));
  else
    for (i = 0; i < bytes; i++) bb_byte(b, (int)(v >> (8 * i)));
}

/* data values for dc.x */
static void emit_data_value(section *s, bbuf *b, expr *e, int esize,
                            int ftype, int reldiff, ival pc)
{
  eres r;
  eval_expr(e, &r);
  if (ftype) {
    uint8_t buf[12];
    if (r.sec || r.ext || r.bad) {
      error("data has illegal type");
      bb_fill(b, 0, esize);
      return;
    }
    encode_float(ftype, eres_float(&r), buf);
    if (!cpu->big_endian) {
      int i, n = ftype == 'x' ? 10 : esize;
      for (i = 0; i < n / 2; i++) { uint8_t t = buf[i]; buf[i] = buf[n - 1 - i]; buf[n - 1 - i] = t; }
    }
    bb_put(b, buf, esize);
    return;
  }
  if (r.isflt && (esize == 4 || esize == 8)) {
    /* float constant in integer data: IEEE bits (oracle) */
    uint8_t fb[8];
    int i;
    encode_float(esize == 8 ? 'd' : 's', r.f, fb);
    if (!cpu->big_endian)
      for (i = 0; i < esize / 2; i++) { uint8_t t = fb[i]; fb[i] = fb[esize - 1 - i]; fb[esize - 1 - i] = t; }
    bb_put(b, fb, esize);
    return;
  }
  if (r.isflt) { r.v = (ival)r.f; r.isflt = 0; }
  if (r.bad) {
    error("illegal relocation");
    bb_fill(b, 0, esize);
    return;
  }
  if (reldiff) {
    /* dr.x: value - pc */
    if (r.sec == s) { r.v -= pc; r.sec = NULL; }
    else if (!r.sec && !r.ext) {
      r.v -= pc + ((s->flags & SEC_ABS) ? s->org : 0);
    } else {
      add_reloc(s, b->n, esize * 8, R_PC, r.sec, r.ext, r.v);
    }
  } else if (r.sec || r.ext) {
    add_reloc(s, b->n, esize * 8, R_ABS, r.sec, r.ext, r.v);
    if (!reloc_inplace) r.v = 0;
  }
  if (esize < 8 && !r.sec && !r.ext) {
    ival lo = -(1LL << (esize * 8 - 1));
    ival hi = (1LL << (esize * 8)) - 1;
    if (esize < 4 && (r.v < lo || r.v > hi))
      error("data out of range");
  }
  if (esize == 8) {
    put_be(b, (uval)r.v, 8);
  } else
    put_be(b, (uval)r.v, esize);
}

/* ------------------------------------------------------------ passes */
static int next_is_insn(atom *a)
{
  for (a = a->next; a; a = a->next) {
    if (a->type == AT_LABEL) continue;
    return a->type == AT_INSN;
  }
  return 0;
}

static ival align_pad(section *s, ival pc, ival align, ival offset)
{
  ival base = (s->flags & SEC_ABS) ? s->org : 0;
  ival addr = base + pc;
  ival pad;
  if (offset < 0) return (addr & 1) ? 0 : 1;   /* odd */
  if (align <= 1) return offset;
  pad = (align - (addr % align)) % align;
  return pad + offset;
}

static ival space_size(atom *a)
{
  ival n;
  if (!eval_const(a->u.space.count, &n)) {
    eres r;
    eval_expr(a->u.space.count, &r);
    if (final_pass) error("space count must be a constant");
    n = r.v;
  }
  if (n < 0) {
    if (final_pass) error("negative space count");
    n = 0;
  }
  return n * a->u.space.esize;
}

int opt_noialign;
static int sizing_pass(void)
{
  section *s;
  int changed = 0;
  for (s = sec_first; s; s = s->next) {
    ival pc = 0;
    atom *a;
    s->size = 0;
    for (a = s->first; a; a = a->next) {
      ival sz = 0;
      curpos = a->pos;
      switch (a->type) {
      case AT_LABEL: {
        ival v = pc;
        if (!opt_noialign && next_is_insn(a) && cpu->insn_align > 1)
          v = (v + cpu->insn_align - 1) / cpu->insn_align * cpu->insn_align;
        if (a->u.label.sym->val != v) changed = 1;
        a->u.label.sym->val = v;
        a->off = v;
        continue;
      }
      case AT_INSN:
        if (!opt_noialign && cpu->insn_align > 1)
          pc = (pc + cpu->insn_align - 1) / cpu->insn_align * cpu->insn_align;
        a->off = pc;
        s->size = pc;
        sz = cpu->insn_size(a, s, pc);
        if (s->align < cpu->insn_align) s->align = cpu->insn_align;
        break;
      case AT_DATA:
        a->off = pc;
        sz = a->size;
        break;
      case AT_SPACE:
        a->off = pc;
        sz = space_size(a);
        break;
      case AT_ALIGN:
        a->off = pc;
        sz = align_pad(s, pc, a->u.align.align, a->u.align.offset);
        if (a->u.align.align > s->align) s->align = a->u.align.align;
        break;
      case AT_BYTES:
        a->off = pc;
        sz = a->u.bytes.n;
        break;
      case AT_RORG: {
        ival t;
        a->off = pc;
        if (!eval_const(a->u.rorg.target, &t)) t = pc;
        sz = t - pc;
        if (sz < 0) { if (final_pass) error("rorg: new pc is lower than current pc"); sz = 0; }
        break;
      }
      }
      if (sz != a->size) changed = 1;
      a->size = sz;
      pc += sz;
      s->size = pc;
    }
    s->size = pc;
  }
  return changed;
}

void resolve_sections(void)
{
  int changed;
  pass_no = 0;
  diag_suppress++;
  do {
    changed = sizing_pass();
    pass_no++;
    if (pass_no > 30) grow_only = 1;
    if (pass_no > 500) { diag_suppress--; fatal("optimizer does not converge"); }
  } while (changed || pass_no < 2);
  diag_suppress--;
}

static const uint8_t *fill_pattern(section *s, int *len)
{
  (void)s;
  *len = opt_padlen;
  return opt_pad;
}

void finalize_sections(void)
{
  section *s;
  final_pass = 1;
  /* one more sizing pass with diagnostics enabled for space counts */
  for (s = sec_first; s; s = s->next) {
    atom *a;
    ival pc = 0;
    bbuf *b = &s->data;
    if (s->flags & SEC_OFFSET) continue;
    for (a = s->first; a; a = a->next) {
      curpos = a->pos;
      if (a->type == AT_LABEL) continue;
      while (pc < a->off) { bb_byte(b, 0); pc++; }  /* insn auto-align */
      switch (a->type) {
      case AT_INSN: {
        size_t before = b->n;
        cpu->insn_encode(a, s, pc, b);
        if ((ival)(b->n - before) != a->size) {
          /* keep layout stable even if encoding disagrees */
          if ((ival)(b->n - before) < a->size)
            bb_fill(b, 0, a->size - (b->n - before));
          else
            error("internal error: instruction size changed (%d != %d)",
                  (int)(b->n - before), (int)a->size);
        }
        break;
      }
      case AT_DATA: {
        int i;
        for (i = 0; i < a->u.data.n; i++) {
          dataitem *it = &a->u.data.items[i];
          if (it->e)
            emit_data_value(s, b, it->e, a->u.data.esize, a->u.data.ftype,
                            a->u.data.reldiff, a->off);
          else {
            int pad = it->slen % a->u.data.esize;
            bb_put(b, it->str, it->slen);
            if (pad) bb_fill(b, 0, a->u.data.esize - pad);
          }
        }
        break;
      }
      case AT_SPACE: {
        ival n = a->size / (a->u.space.esize ? a->u.space.esize : 1);
        uint8_t pat[12];
        int es = a->u.space.esize;
        memset(pat, 0, sizeof(pat));
        if (a->u.space.fill && a->u.space.ftype) {
          /* vasm: float fill for dcb.s yields zero, others are errors */
          if (a->u.space.ftype != 's') error("expression must be constant");
        } else if (a->u.space.fill) {
          bbuf tmp = {0};
          emit_data_value(s, &tmp, a->u.space.fill, es, a->u.space.ftype, 0, pc);
          if (s->rlast && s->rlast->off == 0 && tmp.n) {
            /* relocations in fill patterns are not supported */
          }
          memcpy(pat, tmp.d, es <= 12 ? es : 12);
          free(tmp.d);
        } else if (a->u.space.uninit) {
          int i;
          for (i = 0; i < es; i++) pat[i] = (uint8_t)opt_uspc;
        }
        while (n-- > 0) bb_put(b, pat, es);
        break;
      }
      case AT_ALIGN: {
        ival n = a->size;
        if (a->u.align.nopfill && s->type == ST_CODE) {
          int plen; const uint8_t *pp;
          if ((pc & 1) && n) { bb_byte(b, 0); n--; }
          pp = fill_pattern(s, &plen);
          (void)pp;
          while (n >= 2) { bb_be16(b, a->u.align.nopfill); n -= 2; }
          while (n-- > 0) bb_byte(b, 0);
        } else {
          int plen, i = 0;
          const uint8_t *pp = fill_pattern(s, &plen);
          while (n-- > 0) { bb_byte(b, pp[i % plen]); i++; }
        }
        break;
      }
      case AT_BYTES:
        bb_put(b, a->u.bytes.d, a->u.bytes.n);
        break;
      case AT_RORG: {
        ival n = a->size;
        ival f = 0;
        if (a->u.rorg.fill) eval_const(a->u.rorg.fill, &f);
        while (n-- > 0) bb_byte(b, (int)f);
        break;
      }
      }
      pc = a->off + a->size;
      if ((ival)b->n != pc) {
        if ((ival)b->n < pc) bb_fill(b, 0, pc - b->n);
        else b->n = pc;
      }
    }
    if ((ival)b->n < s->size) bb_fill(b, 0, s->size - b->n);
  }
}

/* ------------------------------------------------------ include paths */
static char **incpaths;
static int nincpaths;
char *compile_dir;
int ignore_mult_inc;

void add_incpath(const char *p)
{
  incpaths = xrealloc(incpaths, (nincpaths + 1) * sizeof(char *));
  incpaths[nincpaths++] = xstrdup(p);
}

static int file_exists(const char *p)
{
  FILE *f = fopen(p, "rb");
  if (f) { fclose(f); return 1; }
  return 0;
}

static char *join_path(const char *dir, const char *name)
{
  size_t l = strlen(dir);
  if (!l) return xstrdup(name);
  if (dir[l - 1] == '/' || dir[l - 1] == ':')
    return strprintf("%s%s", dir, name);
  return strprintf("%s/%s", dir, name);
}

char *find_include(const char *name, int bin)
{
  int i;
  char *p;
  (void)bin;
  if (name[0] == '/' ) return file_exists(name) ? xstrdup(name) : NULL;
  /* 1. compile directory, 2. current directory */
  if (compile_dir && *compile_dir) {
    p = join_path(compile_dir, name);
    if (file_exists(p)) return p;
    free(p);
  }
  if (file_exists(name)) return xstrdup(name);
  for (i = 0; i < nincpaths; i++) {
    p = join_path(incpaths[i], name);
    if (file_exists(p)) return p;
    free(p);
    if (compile_dir && *compile_dir && incpaths[i][0] != '/') {
      char *q = join_path(compile_dir, incpaths[i]);
      p = join_path(q, name);
      free(q);
      if (file_exists(p)) return p;
      free(p);
    }
  }
  return NULL;
}

/* ------------------------------------------------------------ macros */
static macro *macros;
int macro_args_max = 9;
int macro_id_plain;
int max_macro_recursion = 1000;

macro *find_macro(const char *name, size_t len)
{
  macro *m;
  for (m = macros; m; m = m->next)
    if (strlen(m->name) == len && !strnicmp_(m->name, name, len))
      return m;
  return NULL;
}

macro *new_macro(const char *name)
{
  macro *m = xcalloc(1, sizeof(*m));
  m->name = xstrdup(name);
  m->defpos = curpos;
  m->next = macros;
  macros = m;
  return m;
}

void macro_add_line(macro *m, const char *l)
{
  m->lines = xrealloc(m->lines, (m->nlines + 1) * sizeof(char *));
  m->lines[m->nlines++] = xstrdup(l);
}

/* ------------------------------------------------------------ sources */
enum { SRC_FILE, SRC_MACRO, SRC_REPT, SRC_IRP };
typedef struct source {
  struct source *parent;
  int kind;
  srcfile *file;
  char **lines; int nlines; int cur;
  /* macro */
  macro *mac;
  char **args; int nargs;
  char *qual;
  int uid;
  ival carg, saved_narg, saved_carg;
  int narg_defined;
  /* rept */
  int count, iter;
  ival saved_reptn;
  char *irpname; char **irpvals; int nirpvals;
  int condlevel;
  srcpos pos;           /* position of invocation */
  char *linebuf;
} source;

static source *src;
static int macro_depth;
static int uid_counter;
static int *idstack; static int nidstack, capidstack;
int end_reached;
symbol *sym_reptn, *sym_narg, *sym_carg;
static srcfile **loaded; static int nloaded;

void push_file(const char *name)
{
  srcfile *f = NULL;
  char *path;
  source *s;
  int i;
  if (src) {
    path = find_include(name, 0);
    if (!path) { error("can't open source file \"%s\"", name); return; }
  } else
    path = xstrdup(name);
  if (ignore_mult_inc)
    for (i = 0; i < nloaded; i++)
      if (!strcmp(loaded[i]->path, path)) { free(path); return; }
  for (i = 0; i < nloaded; i++)
    if (!strcmp(loaded[i]->path, path)) { f = loaded[i]; break; }
  if (!f) {
    f = load_source(name, path);
    if (!f) { if (!src) fatal("can't open source file \"%s\"", name);
              error("can't open source file \"%s\"", name); return; }
    loaded = xrealloc(loaded, (nloaded + 1) * sizeof(*loaded));
    loaded[nloaded++] = f;
  }
  free(path);
  if (src) uid_counter++;   /* include files consume a unique id (oracle) */
  s = xcalloc(1, sizeof(*s));
  s->kind = SRC_FILE;
  s->file = f;
  s->lines = f->lines;
  s->nlines = f->nlines;
  s->parent = src;
  s->condlevel = cond_level;
  s->pos = curpos;
  src = s;
}

void push_macro(macro *m, char **args, int nargs, const char *qual)
{
  source *s;
  ival v;
  if (macro_depth >= max_macro_recursion) {
    error("maximum macro recursion depth exceeded");
    return;
  }
  s = xcalloc(1, sizeof(*s));
  s->kind = SRC_MACRO;
  s->mac = m;
  s->lines = m->lines;
  s->nlines = m->nlines;
  s->args = args;
  s->nargs = nargs;
  s->qual = qual ? xstrdup(qual) : NULL;
  s->uid = ++uid_counter;
  s->carg = 1;
  s->parent = src;
  s->file = src ? src->file : NULL;
  s->condlevel = cond_level;
  s->pos = curpos;
  s->narg_defined = sym_narg->type == S_SET;
  if (s->narg_defined && eval_const(sym_narg->e, &v)) s->saved_narg = v;
  if (sym_carg->type == S_SET && eval_const(sym_carg->e, &v)) s->saved_carg = v;
  set_internal(sym_narg, nargs);
  set_internal(sym_carg, 1);
  src = s;
  macro_depth++;
}

void push_rept(char **lines, int nlines, int count)
{
  source *s;
  ival v = -1;
  s = xcalloc(1, sizeof(*s));
  s->uid = ++uid_counter;   /* a repeat block consumes one unique id */
  s->kind = SRC_REPT;
  s->lines = lines;
  s->nlines = nlines;
  s->count = count;
  s->iter = 0;
  s->parent = src;
  s->file = src ? src->file : NULL;
  s->condlevel = cond_level;
  s->pos = curpos;
  if (sym_reptn->type == S_SET) eval_const(sym_reptn->e, &v);
  s->saved_reptn = v;
  set_internal(sym_reptn, 0);
  src = s;
}

void push_irp(char **lines, int nlines, const char *argname, char **vals, int nvals)
{
  push_rept(lines, nlines, nvals);
  src->kind = SRC_IRP;
  src->irpname = xstrdup(argname);
  src->irpvals = vals;
  src->nirpvals = nvals;
}

int in_macro(void)
{
  source *s;
  for (s = src; s; s = s->parent) if (s->kind == SRC_MACRO) return 1;
  return 0;
}

static void pop_source(void)
{
  source *s = src;
  if (s->kind == SRC_MACRO) {
    macro_depth--;
    if (s->narg_defined) set_internal(sym_narg, s->saved_narg);
    else { sym_narg->type = S_NONE; sym_narg->flags &= ~SF_DEFINED; }
    set_internal(sym_carg, s->saved_carg);
  }
  if (s->kind == SRC_REPT || s->kind == SRC_IRP)
    set_internal(sym_reptn, s->saved_reptn);
  if (s->kind != SRC_FILE) {
    /* restore conditional level (mexit / unbalanced if) */
    while (cond_level > s->condlevel) cond_end();
  } else if (cond_level != s->condlevel && s->parent) {
    /* an include file may legally leave conditionals open? keep */
  }
  curpos = s->pos;
  src = s->parent;
  free(s->linebuf);
  free(s);
}

void exit_macro(void)
{
  /* leave innermost macro, dropping any rept/irp inside it */
  while (src && src->kind != SRC_MACRO) {
    if (src->kind == SRC_FILE) return;
    pop_source();
  }
  if (src) pop_source();
}

void end_assembly(void)
{
  end_reached = 1;
}

static void append(char **buf, size_t *len, size_t *cap, const char *s, size_t n)
{
  if (*len + n + 1 > *cap) {
    *cap = (*len + n + 1) * 2;
    *buf = xrealloc(*buf, *cap);
  }
  memcpy(*buf + *len, s, n);
  *len += n;
  (*buf)[*len] = 0;
}

static const char *macro_arg(source *s, int i)
{
  if (i < 1 || i > s->nargs) return "";
  return s->args[i - 1] ? s->args[i - 1] : "";
}

/* expand backslash sequences of a macro line */
static char *expand_macro_line(source *s, const char *l)
{
  char *buf = NULL;
  size_t len = 0, cap = 0;
  const char *p = l;
  append(&buf, &len, &cap, "", 0);
  while (*p) {
    if (*p != '\\') {
      const char *q = p;
      while (*q && *q != '\\') q++;
      append(&buf, &len, &cap, p, q - p);
      p = q;
      continue;
    }
    p++;
    if (*p >= '0' && *p <= '9') {
      int n = *p - '0';
      p++;
      if (n == 0) {
        if (s->qual) append(&buf, &len, &cap, s->qual, strlen(s->qual));
      } else {
        const char *a = macro_arg(s, n);
        append(&buf, &len, &cap, a, strlen(a));
      }
      continue;
    }
    if (*p == '@') {
      char tmp[32];
      p++;
      if (*p == '!') {
        p++;
        if (nidstack >= capidstack) { capidstack = capidstack * 2 + 16; idstack = xrealloc(idstack, capidstack * sizeof(int)); }
        idstack[nidstack++] = s->uid;
        snprintf(tmp, sizeof(tmp), "_%06d", s->uid);
      } else if (*p == '?') {
        p++;
        if (nidstack >= capidstack) { capidstack = capidstack * 2 + 16; idstack = xrealloc(idstack, capidstack * sizeof(int)); }
        if (nidstack) {
          idstack[nidstack] = idstack[nidstack - 1];
          idstack[nidstack - 1] = s->uid;
          nidstack++;
        } else {
          error("macro id insert on empty stack");
          idstack[nidstack++] = s->uid;
        }
        snprintf(tmp, sizeof(tmp), "_%06d", s->uid);
      } else if (*p == '@') {
        p++;
        if (nidstack) snprintf(tmp, sizeof(tmp), "_%06d", idstack[--nidstack]);
        else { error("macro id pull without matching push"); tmp[0] = 0; }
      } else
        snprintf(tmp, sizeof(tmp), "_%06d", s->uid);
      if (macro_id_plain && tmp[0] == '_') { int n = atoi(tmp + 1); snprintf(tmp, sizeof(tmp), "%d", n); }
      append(&buf, &len, &cap, tmp, strlen(tmp));
      continue;
    }
    if (*p == '#') {
      char tmp[16];
      p++;
      snprintf(tmp, sizeof(tmp), "%d", s->nargs);
      append(&buf, &len, &cap, tmp, strlen(tmp));
      continue;
    }
    if (*p == '?' && p[1] >= '0' && p[1] <= '9') {
      char tmp[16];
      int n = p[1] - '0';
      p += 2;
      snprintf(tmp, sizeof(tmp), "%d", (int)strlen(macro_arg(s, n)));
      append(&buf, &len, &cap, tmp, strlen(tmp));
      continue;
    }
    if (*p == '.' || *p == '+' || *p == '-') {
      const char *a = macro_arg(s, (int)s->carg);
      append(&buf, &len, &cap, a, strlen(a));
      if (*p == '+') s->carg++;
      if (*p == '-') s->carg--;
      set_internal(sym_carg, s->carg);
      p++;
      continue;
    }
    if (*p == '<') {
      const char *q = strchr(p, '>');
      if (q) {
        int hex = p[1] == '$';
        char *name = xstrndup(p + 1 + hex, q - p - 1 - hex);
        symbol *sy = sym_lookup(name);
        ival v = 0;
        char tmp[32];
        if (sy && (sy->type == S_SET || sy->type == S_EXPR) && eval_const(sy->e, &v)) {
          if (hex) snprintf(tmp, sizeof(tmp), "%llX", (unsigned long long)(uint32_t)v);
          else snprintf(tmp, sizeof(tmp), "%lld", (long long)v);
          append(&buf, &len, &cap, tmp, strlen(tmp));
        } else
          error("invalid numeric expansion");
        free(name);
        p = q + 1;
        continue;
      }
    }
    if (macro_args_max > 9 && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) &&
        !(s->mac && s->mac->nargnames)) {
      int n = tolower((unsigned char)*p) - 'a' + 10;
      const char *a = macro_arg(s, n);
      p++;
      append(&buf, &len, &cap, a, strlen(a));
      continue;
    }
    if (s->mac && s->mac->nargnames && (isalpha((unsigned char)*p) || *p == '_')) {
      const char *q = p;
      int i;
      while (isalnum((unsigned char)*q) || *q == '_') q++;
      for (i = 0; i < s->mac->nargnames; i++)
        if ((size_t)(q - p) == strlen(s->mac->argnames[i]) &&
            !strncmp(p, s->mac->argnames[i], q - p)) break;
      if (i < s->mac->nargnames) {
        const char *a = macro_arg(s, i + 1);
        append(&buf, &len, &cap, a, strlen(a));
        p = q;
        if (*p == '\\') p++;   /* optional terminating backslash */
        continue;
      }
    }
    append(&buf, &len, &cap, "\\", 1);
  }
  return buf;
}

static char *expand_irp_line(source *s, const char *l)
{
  char *buf = NULL;
  size_t len = 0, cap = 0, nl = strlen(s->irpname);
  const char *p = l;
  const char *val = s->irpvals && s->iter < s->nirpvals ? s->irpvals[s->iter] : "";
  append(&buf, &len, &cap, "", 0);
  while (*p) {
    if (*p == '\\' && !strncmp(p + 1, s->irpname, nl) && !isalnum((unsigned char)p[1 + nl]) && p[1 + nl] != '_') {
      append(&buf, &len, &cap, val, strlen(val));
      p += 1 + nl;
      continue;
    }
    append(&buf, &len, &cap, p, 1);
    p++;
  }
  return buf;
}

const char *next_line(void)
{
  while (src) {
    source *s = src;
    if (end_reached) {
      while (src) pop_source();
      return NULL;
    }
    if (s->cur >= s->nlines) {
      if ((s->kind == SRC_REPT || s->kind == SRC_IRP) && ++s->iter < s->count) {
        s->cur = 0;
        set_internal(sym_reptn, s->iter);
        continue;
      }
      if (s->kind == SRC_MACRO || s->kind == SRC_FILE) {
        pop_source();
        continue;
      }
      pop_source();
      continue;
    }
    free(s->linebuf);
    s->linebuf = NULL;
    if (s->kind == SRC_FILE) {
      curpos.file = s->file;
      curpos.line = s->cur + 1;
      curpos.text = s->lines[s->cur];
      return s->lines[s->cur++];
    }
    if (s->kind == SRC_MACRO)
      s->linebuf = expand_macro_line(s, s->lines[s->cur++]);
    else if (s->kind == SRC_IRP)
      s->linebuf = expand_irp_line(s, s->lines[s->cur++]);
    else
      s->linebuf = xstrdup(s->lines[s->cur++]);
    curpos.text = s->linebuf;
    return s->linebuf;
  }
  return NULL;
}

/* ------------------------------------------------- conditional assembly */
typedef struct cstate { char active, done, parent; } cstate;
static cstate *cstack;
static int ccap;
int cond_level;

int cond_active(void)
{
  return cond_level == 0 || cstack[cond_level - 1].active;
}
int cond_parent_active(void)
{
  return cond_level == 0 || cstack[cond_level - 1].parent;
}
int cond_depth(void) { return cond_level; }

void cond_push(int truth)
{
  int par = cond_active();
  if (cond_level >= ccap) {
    ccap = ccap * 2 + 32;
    cstack = xrealloc(cstack, ccap * sizeof(cstate));
  }
  cstack[cond_level].parent = par;
  cstack[cond_level].active = par && truth;
  cstack[cond_level].done = par && truth;
  cond_level++;
}

void cond_else(void)
{
  cstate *c;
  if (!cond_level) { error("else without if"); return; }
  c = &cstack[cond_level - 1];
  c->active = c->parent && !c->done;
  c->done = 1;
}

void cond_elseif(int truth)
{
  cstate *c;
  if (!cond_level) { error("elseif without if"); return; }
  c = &cstack[cond_level - 1];
  if (c->done) c->active = 0;
  else { c->active = c->parent && truth; c->done = c->active; }
}

void cond_end(void)
{
  if (!cond_level) { error("endif without if"); return; }
  cond_level--;
}
