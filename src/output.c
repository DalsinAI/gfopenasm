/* gfasm - output modules: Amiga hunk object/executable, ELF (m68k),
 * raw binary.  MIT License.
 *
 * Written from the public AmigaDOS hunk format description (AmigaDOS
 * Manual / "The AmigaDOS Technical Reference") and the System V ABI ELF
 * specification with the m68k processor supplement relocation numbers.
 * Layout details that the specs leave open (ordering, padding) were
 * matched by black-box comparison. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>

out_module *outmod;
int opt_keepempty, opt_kick1hunks, opt_hunkpad = 0x4e71, opt_databss;
char *idnt_name;
static FILE *of;
static int out_le;    /* little-endian output (ELF for little-endian cpus) */

static void w32(uint32_t v)
{
  uint8_t b[4] = { v >> 24, v >> 16, v >> 8, v };
  if (out_le) { uint8_t t = b[0]; b[0] = b[3]; b[3] = t; t = b[1]; b[1] = b[2]; b[2] = t; }
  fwrite(b, 1, 4, of);
}
static void w16(uint32_t v)
{
  uint8_t b[2] = { v >> 8, v };
  if (out_le) { uint8_t t = b[0]; b[0] = b[1]; b[1] = t; }
  fwrite(b, 1, 2, of);
}
static void wbytes(const void *p, size_t n) { fwrite(p, 1, n, of); }

/* ------------------------------------------------------- helpers */
static int sec_has_symbols(section *s)
{
  symbol *y;
  for (y = sym_first; y; y = y->lnext)
    if (y->type == S_LABEL && y->sec == s && !(y->flags & SF_INTERNAL)) return 1;
  return 0;
}

static int sec_has_atoms(section *s)
{
  atom *a;
  for (a = s->first; a; a = a->next) if (a->type != AT_LABEL) return 1;
  return 0;
}

static int sec_output(section *s)
{
  if (s->flags & SEC_OFFSET) return 0;
  if (sec_has_atoms(s)) return 1;
  if (s == sec_first && !s->next) return 1;   /* empty source: one empty hunk */
  if (s->size == 0 && !opt_keepempty && !sec_has_symbols(s) && !s->relocs) return 0;
  return 1;
}

/* labels, and equates whose value is an address in a section */
static section *sym_section(symbol *y, ival *val)
{
  if (y->type == S_LABEL) { *val = y->val; return y->sec; }
  if (y->type == S_EXPR || y->type == S_SET) {
    eres r;
    eval_expr(y->e, &r);
    if (r.sec && !r.ext && !r.bad) { *val = r.v; return r.sec; }
  }
  return NULL;
}

static int is_listed_symbol(symbol *y)
{
  ival v;
  section *s;
  if (y->flags & (SF_INTERNAL | SF_LOCAL)) return 0;
  s = sym_section(y, &v);
  if (!s) return 0;
  if (s->flags & SEC_OFFSET) return 0;
  return 1;
}

static void wname(const char *n)
{
  size_t l = strlen(n);
  uint32_t nl = (uint32_t)((l + 3) / 4);
  static const char z[4] = { 0, 0, 0, 0 };
  w32(nl);
  wbytes(n, l);
  wbytes(z, nl * 4 - l);
}

static void wname_type(int type, const char *n)
{
  size_t l = strlen(n);
  uint32_t nl = (uint32_t)((l + 3) / 4);
  static const char z[4] = { 0, 0, 0, 0 };
  w32(((uint32_t)type << 24) | nl);
  wbytes(n, l);
  wbytes(z, nl * 4 - l);
}

static const char *basename_(const char *p)
{
  const char *s = strrchr(p, '/');
  const char *t = strrchr(p, ':');
  if (t && (!s || t > s)) s = t;
  return s ? s + 1 : p;
}

/* ------------------------------------------------------- hunk */
#define HUNK_UNIT    0x3e7
#define HUNK_NAME    0x3e8
#define HUNK_CODE    0x3e9
#define HUNK_DATA    0x3ea
#define HUNK_BSS     0x3eb
#define HUNK_RELOC32 0x3ec
#define HUNK_RELOC16 0x3ed
#define HUNK_RELOC8  0x3ee
#define HUNK_EXT     0x3ef
#define HUNK_SYMBOL  0x3f0
#define HUNK_END     0x3f2
#define HUNK_HEADER  0x3f3
#define HUNK_DREL32  0x3f7
#define HUNK_DREL16  0x3f8
#define HUNK_DREL8   0x3f9
#define HUNK_RELRELOC32 0x3fd
#define HUNK_ABSRELOC16 0x3fe

static section **hsec;
static int nhsec;

static int hunk_index(section *s)
{
  int i;
  for (i = 0; i < nhsec; i++) if (hsec[i] == s) return i;
  return -1;
}

static void collect_hunks(void)
{
  section *s;
  nhsec = 0;
  free(hsec);
  hsec = NULL;
  for (s = sec_first; s; s = s->next)
    if (sec_output(s)) {
      hsec = xrealloc(hsec, (nhsec + 1) * sizeof(*hsec));
      hsec[nhsec++] = s;
    }
}

static uint32_t hunk_type(section *s)
{
  if (s->type == ST_CODE && cpu == &cpu_ppc) return 0x4e9;   /* HUNK_PPC_CODE */
  return s->type == ST_CODE ? HUNK_CODE : s->type == ST_DATA ? HUNK_DATA : HUNK_BSS;
}

static uint32_t mem_bits(section *s, int *extra)
{
  *extra = 0;
  if (s->memflags == 0) return 0;
  if (s->memflags == 0x40000000) return 0x40000000;
  if (s->memflags == 0x80000000) return 0x80000000;
  *extra = 1;
  return 0xC0000000;
}

static uint32_t hunk_longs(section *s) { return (uint32_t)((s->size + 3) / 4); }

static void write_hunk_data(section *s)
{
  size_t n = s->data.n;
  size_t pad = (4 - n % 4) % 4;
  wbytes(s->data.d, n);
  if (s->type == ST_CODE && pad == 2 && !(n & 1) && cpu == &cpu_m68k) w16(opt_hunkpad);
  else while (pad--) fputc(0, of);
}

typedef struct rgroup { int hunk; ival *offs; int n, cap; } rgroup;

static int cmp_ival(const void *a, const void *b)
{
  ival x = *(const ival *)a, y = *(const ival *)b;
  return x < y ? -1 : x > y;
}

/* write section-relative relocations of one kind */
static void write_relocs(section *s, int type, int size, uint32_t hunkid, int shortfmt)
{
  rgroup *g = NULL;
  int ng = 0, i, j;
  reloc *r;
  for (r = s->relocs; r; r = r->next) {
    int h;
    if (r->tsym || r->type != type || r->size != size) continue;
    h = hunk_index(r->tsec);
    if (h < 0) continue;
    for (i = 0; i < ng; i++) if (g[i].hunk == h) break;
    if (i == ng) {
      g = xrealloc(g, (ng + 1) * sizeof(*g));
      memset(&g[ng], 0, sizeof(*g));
      g[ng].hunk = h;
      ng++;
    }
    if (g[i].n >= g[i].cap) { g[i].cap = g[i].cap * 2 + 8; g[i].offs = xrealloc(g[i].offs, g[i].cap * sizeof(ival)); }
    g[i].offs[g[i].n++] = r->off;
  }
  if (!ng) return;
  /* sort groups by hunk */
  for (i = 0; i < ng; i++)
    for (j = i + 1; j < ng; j++)
      if (g[j].hunk < g[i].hunk) { rgroup t = g[i]; g[i] = g[j]; g[j] = t; }
  if (shortfmt) {
    int words = 0;
    w32(HUNK_DREL32);
    for (i = 0; i < ng; i++) {
      qsort(g[i].offs, g[i].n, sizeof(ival), cmp_ival);
      w16(g[i].n);
      w16(g[i].hunk);
      words += 2;
      for (j = 0; j < g[i].n; j++) { w16((uint32_t)g[i].offs[j]); words++; }
    }
    w16(0);
    words++;
    if (words & 1) w16(0);
  } else {
    w32(hunkid);
    for (i = 0; i < ng; i++) {
      qsort(g[i].offs, g[i].n, sizeof(ival), cmp_ival);
      w32(g[i].n);
      w32(g[i].hunk);
      for (j = 0; j < g[i].n; j++) w32((uint32_t)g[i].offs[j]);
    }
    w32(0);
  }
  for (i = 0; i < ng; i++) free(g[i].offs);
  free(g);
}

static int ext_type_for(reloc *r)
{
  if (r->type == R_ABS) {
    if (r->size == 32) return 0x81;                       /* EXT_REF32 */
    if (r->size == 16) return opt_kick1hunks ? 0x83 : 0x8a; /* EXT_ABSREF16 */
    if (r->size == 8) return opt_kick1hunks ? 0x84 : 0x8b;  /* EXT_ABSREF8 */
  } else if (r->type == R_PC) {
    if (r->size == 24) return 0xe5;                       /* EXT_RELREF26 */
    if (r->size == 32) return 0x88;                       /* EXT_RELREF32 */
    if (r->size == 16) return 0x83;                       /* EXT_RELREF16 */
    if (r->size == 8) return 0x84;                        /* EXT_RELREF8 */
  } else if (r->type == R_SD) {
    if (r->size == 32) return 0x85;
    if (r->size == 16) return 0x86;
    if (r->size == 8) return 0x87;
  }
  return -1;
}

static void write_ext(section *s, int first)
{
  reloc *r;
  symbol *y;
  int any = 0;
  /* references: group by symbol and type, in order of first use */
  typedef struct eref { symbol *sym; int type; ival *offs; int n, cap; } eref;
  eref *e = NULL;
  int ne = 0, i;
  for (r = s->relocs; r; r = r->next) {
    int t;
    if (!r->tsym) continue;
    t = ext_type_for(r);
    if (r->tsym->type == S_COMMON && t == 0x81) t = 0x82;
    if (t < 0) { curpos = r->pos; error("reloc type %d, size %d not supported", r->type, r->size); continue; }
    for (i = 0; i < ne; i++) if (e[i].sym == r->tsym && e[i].type == t) break;
    if (i == ne) {
      e = xrealloc(e, (ne + 1) * sizeof(*e));
      memset(&e[ne], 0, sizeof(*e));
      e[ne].sym = r->tsym;
      e[ne].type = t;
      ne++;
    }
    if (e[i].n >= e[i].cap) { e[i].cap = e[i].cap * 2 + 8; e[i].offs = xrealloc(e[i].offs, e[i].cap * sizeof(ival)); }
    e[i].offs[e[i].n++] = r->off;
  }
  for (y = sym_first; y; y = y->lnext) {
    ival v;
    if (!(y->flags & SF_EXPORT)) continue;
    if (sym_section(y, &v) == s ||
        (first && (y->type == S_EXPR || y->type == S_SET) && !(y->flags & SF_INTERNAL) && eval_const(y->e, &v))) { any = 1; break; }
  }
  if (!ne && !any) return;
  w32(HUNK_EXT);
  for (i = 0; i < ne; i++) {
    int j;
    wname_type(e[i].type, e[i].sym->name);
    if (e[i].type == 0x82) w32((uint32_t)e[i].sym->val);
    w32(e[i].n);
    for (j = 0; j < e[i].n; j++) w32((uint32_t)e[i].offs[j]);
    free(e[i].offs);
  }
  free(e);
  for (y = sym_first; y; y = y->lnext) {
    ival sv;
    if (!(y->flags & SF_EXPORT) || (y->flags & SF_INTERNAL)) continue;
    if (sym_section(y, &sv) == s) {
      wname_type(1, y->name);
      w32((uint32_t)sv);
    } else if (first && (y->type == S_EXPR || y->type == S_SET)) {
      ival v;
      if (eval_const(y->e, &v)) {
        wname_type(2, y->name);
        w32((uint32_t)v);
      }
    }
  }
  w32(0);
}

static void write_symbols(section *s)
{
  symbol *y;
  int any = 0;
  if (opt_nosym) return;
  ival v;
  for (y = sym_first; y; y = y->lnext)
    if (is_listed_symbol(y) && sym_section(y, &v) == s) { any = 1; break; }
  if (!any) return;
  w32(HUNK_SYMBOL);
  for (y = sym_first; y; y = y->lnext)
    if (is_listed_symbol(y) && sym_section(y, &v) == s) {
      wname(y->name);
      w32((uint32_t)v);
    }
  w32(0);
}

static void check_unsupported_relocs(section *s, int exe)
{
  reloc *r;
  for (r = s->relocs; r; r = r->next) {
    curpos = r->pos;
    if (r->tsym) {
      if (exe) error("undefined symbol <%s> at %s+0x%lx, reloc type %d",
                     r->tsym->name, s->name, (unsigned long)r->off, r->type);
      continue;
    }
    if (r->type == R_ABS && r->size == 32) continue;
    if (!exe && r->type == R_PC && (r->size == 16 || r->size == 8 || r->size == 32 || r->size == 24)) continue;
    if (!exe && r->type == R_SD && r->size == 16) continue;
    error("reloc type %s, size %d (symbol %s + 0x%lx) not supported",
          r->type == R_ABS ? "abs" : r->type == R_PC ? "pcrel" : "sd", r->size,
          r->tsec ? r->tsec->name : "?", (unsigned long)r->addend);
  }
}

static int hunk_write(FILE *f, const char *outname)
{
  int i;
  (void)outname;
  of = f;
  collect_hunks();
  for (i = 0; i < nhsec; i++) check_unsupported_relocs(hsec[i], 0);
  if (errcount) return 0;
  w32(HUNK_UNIT);
  wname(idnt_name ? idnt_name : input_name);
  for (i = 0; i < nhsec; i++) {
    section *s = hsec[i];
    int extra;
    uint32_t mb = mem_bits(s, &extra);
    if (s->size) {
      w32(HUNK_NAME);
      wname(s->name);
    }
    w32(hunk_type(s) | mb);
    if (extra) w32(s->memflags);
    w32(hunk_longs(s));
    if (s->type != ST_BSS) {
      write_hunk_data(s);
      write_relocs(s, R_ABS, 32, HUNK_RELOC32, 0);
      write_relocs(s, R_PC, 16, HUNK_RELOC16, 0);
      write_relocs(s, R_PC, 8, HUNK_RELOC8, 0);
      write_relocs(s, R_PC, 32, HUNK_RELRELOC32, 0);
      write_relocs(s, R_PC, 24, 0x4ec, 0);   /* HUNK_RELRELOC26 */
      write_relocs(s, R_SD, 16, HUNK_DREL16, 0);
    }
    write_ext(s, i == 0);
    write_symbols(s);
    w32(HUNK_END);
  }
  return 1;
}

static int can_short_relocs(section *s)
{
  reloc *r;
  if (opt_kick1hunks) return 0;
  if (nhsec > 65535) return 0;
  for (r = s->relocs; r; r = r->next)
    if (r->off > 65535) return 0;
  return 1;
}

static int hunkexe_write(FILE *f, const char *outname)
{
  int i;
  (void)outname;
  of = f;
  collect_hunks();
  for (i = 0; i < nhsec; i++) check_unsupported_relocs(hsec[i], 1);
  if (errcount) return 0;
  w32(HUNK_HEADER);
  w32(0);
  w32(nhsec);
  w32(0);
  w32(nhsec ? nhsec - 1 : 0);
  for (i = 0; i < nhsec; i++) {
    int extra;
    uint32_t mb = mem_bits(hsec[i], &extra);
    w32(hunk_longs(hsec[i]) | mb);
    if (extra) w32(hsec[i]->memflags);
  }
  for (i = 0; i < nhsec; i++) {
    section *s = hsec[i];
    w32(hunk_type(s));
    w32(hunk_longs(s));
    if (s->type != ST_BSS) {
      write_hunk_data(s);
      write_relocs(s, R_ABS, 32, HUNK_RELOC32, can_short_relocs(s));
    }
    write_symbols(s);
    w32(HUNK_END);
  }
  return 1;
}

out_module out_hunk = { "hunk", NULL, hunk_write, 1, 0 };
out_module out_hunkexe = { "hunkexe", NULL, hunkexe_write, 1, 0 };

/* ------------------------------------------------------- binary */
static int cmp_secaddr(const void *a, const void *b)
{
  const section *x = *(section * const *)a, *y = *(section * const *)b;
  ival ax = (x->flags & SEC_ABS) ? x->org : 0, ay = (y->flags & SEC_ABS) ? y->org : 0;
  if (ax != ay) return ax < ay ? -1 : 1;
  return x->idx - y->idx;
}

static int bin_write(FILE *f, const char *outname)
{
  int i, n = 0;
  section *s, **v = NULL;
  ival pos = 0;
  (void)outname;
  of = f;
  for (s = sec_first; s; s = s->next) {
    reloc *r;
    if ((s->flags & SEC_OFFSET) || (!s->size && !sec_has_symbols(s))) continue;
    for (r = s->relocs; r; r = r->next) {
      curpos = r->pos;
      if (r->tsym) error("undefined symbol <%s>", r->tsym->name);
      else error("relocation not supported by binary output");
    }
    v = xrealloc(v, (n + 1) * sizeof(*v));
    v[n++] = s;
  }
  if (errcount) return 0;
  qsort(v, n, sizeof(*v), cmp_secaddr);
  for (i = 0; i < n; i++) {
    ival start = (v[i]->flags & SEC_ABS) ? v[i]->org : 0;
    if (i > 0) {
      ival pend = ((v[i - 1]->flags & SEC_ABS) ? v[i - 1]->org : 0) + v[i - 1]->size;
      if (start < pend) {
        fatal("sections <%s>:%llx-%llx and <%s>:%llx-%llx must not overlap",
              v[i - 1]->name, (unsigned long long)((v[i - 1]->flags & SEC_ABS) ? v[i - 1]->org : 0),
              (unsigned long long)pend, v[i]->name, (unsigned long long)start,
              (unsigned long long)(start + v[i]->size));
      }
      while (pos < start) { fputc(opt_pad[0], f); pos++; }
    } else
      pos = start;
    wbytes(v[i]->data.d, v[i]->data.n);
    pos += v[i]->data.n;
  }
  free(v);
  return 1;
}
out_module out_bin = { "bin", NULL, bin_write, 1, 1 };

/* ------------------------------------------------------- ELF (m68k) */
typedef struct strtab { char *d; size_t n, cap; } strtab;
static size_t st_add(strtab *t, const char *s)
{
  size_t l = strlen(s) + 1, off = t->n;
  if (t->n + l > t->cap) { t->cap = (t->n + l) * 2 + 64; t->d = xrealloc(t->d, t->cap); }
  memcpy(t->d + t->n, s, l);
  t->n += l;
  return off;
}

typedef struct esym { uint32_t name, value, size; uint8_t info, other; uint16_t shndx; symbol *sym; } esym;

static int elf_reloc_type(reloc *r)
{
  if (cpu->elf_reloc) return cpu->elf_reloc(r);
  if (r->type == R_ABS) return r->size == 32 ? 1 : r->size == 16 ? 2 : r->size == 8 ? 3 : -1;
  if (r->type == R_PC) return r->size == 32 ? 4 : r->size == 16 ? 5 : r->size == 8 ? 6 : -1;
  return -1;
}

static void wpad(long to)
{
  while (ftell(of) < to) fputc(0, of);
}

static int elf_write(FILE *f, const char *outname)
{
  strtab shs = {0}, strs = {0};
  section *s;
  section **ps = NULL;
  int nps = 0, i, nrel = 0;
  esym *syms = NULL;
  int nsyms = 0, firstglobal = 0;
  symbol *y;
  uint32_t *sh_name;
  long off, shoff, symoff, stroff;
  long *secoff, *reloff;
  int nsh, shstr_idx, symtab_idx, strtab_idx;
  symbol **lv; int nl = 0;
  (void)outname;
  of = f;
  out_le = !cpu->big_endian;
  for (s = sec_first; s; s = s->next)
    if (sec_output(s)) { ps = xrealloc(ps, (nps + 1) * sizeof(*ps)); ps[nps++] = s; }
  for (i = 0; i < nps; i++) if (ps[i]->relocs) nrel++;
  /* section header string table */
  st_add(&shs, "");
  st_add(&shs, ".symtab");
  st_add(&shs, ".strtab");
  st_add(&shs, ".shstrtab");
  sh_name = xcalloc(nps * 2 + 1, sizeof(uint32_t));
  for (i = 0; i < nps; i++) sh_name[i] = (uint32_t)st_add(&shs, ps[i]->name);
  {
    int k = 0;
    for (i = 0; i < nps; i++)
      if (ps[i]->relocs) {
        char *n = strprintf(cpu->elf_rel ? ".rel%s" : ".rela%s", ps[i]->name);
        sh_name[nps + k++] = (uint32_t)st_add(&shs, n);
        free(n);
      }
  }
  nsh = 1 + nps + nrel + 3;
  shstr_idx = 1 + nps + nrel;
  symtab_idx = shstr_idx + 1;
  strtab_idx = shstr_idx + 2;
  /* symbol table */
  st_add(&strs, "");
  syms = xcalloc(1, sizeof(esym));
  nsyms = 1;
  for (i = 0; i < nps; i++) {
    syms = xrealloc(syms, (nsyms + 1) * sizeof(esym));
    memset(&syms[nsyms], 0, sizeof(esym));
    syms[nsyms].info = 3;       /* LOCAL SECTION */
    syms[nsyms].shndx = 1 + i;
    nsyms++;
  }
  syms = xrealloc(syms, (nsyms + 1) * sizeof(esym));
  memset(&syms[nsyms], 0, sizeof(esym));
  syms[nsyms].name = (uint32_t)st_add(&strs, basename_(input_name));
  syms[nsyms].info = 4;         /* LOCAL FILE */
  syms[nsyms].shndx = 0xfff1;
  nsyms++;
  /* collect symbols in reverse definition order */
  lv = NULL;
  for (y = sym_first; y; y = y->lnext) { lv = xrealloc(lv, (nl + 1) * sizeof(*lv)); lv[nl++] = y; }
  for (int pass = 0; pass < 3; pass++) {
    if (pass == 1) firstglobal = nsyms;
    for (int ii = 0; ii < nl; ii++) {
      int global, idx = -1, j;
      i = pass == 2 ? ii : nl - 1 - ii;
      y = lv[i];
      if (y->flags & (SF_INTERNAL | SF_LOCAL)) continue;
      global = (y->flags & SF_EXPORT) || y->type == S_IMPORT || y->type == S_COMMON;
      if (pass == 0 && global) continue;
      if (pass == 1 && (!global || y->type == S_IMPORT)) continue;
      if (pass == 2 && y->type != S_IMPORT) continue;
      if (y->type == S_LABEL) {
        if (!y->sec || (y->sec->flags & SEC_OFFSET)) continue;
        for (j = 0; j < nps; j++) if (ps[j] == y->sec) idx = j;
        if (idx < 0) continue;
        if (!global && opt_nosym) continue;
      } else if (y->type == S_IMPORT) {
        if (!(y->flags & SF_USED) && !(y->flags & SF_REFD)) continue;
        {
          /* only if referenced by a relocation */
          int used = 0;
          for (j = 0; j < nps && !used; j++) {
            reloc *r;
            for (r = ps[j]->relocs; r; r = r->next) if (r->tsym == y) { used = 1; break; }
          }
          if (!used) continue;
        }
      } else if (y->type == S_COMMON) {
      } else if (y->type == S_EXPR || y->type == S_SET) {
        ival cv;
        if (!eval_const(y->e, &cv)) continue;
        if (!global && opt_nosym) continue;
      } else continue;
      syms = xrealloc(syms, (nsyms + 1) * sizeof(esym));
      memset(&syms[nsyms], 0, sizeof(esym));
      syms[nsyms].name = (uint32_t)st_add(&strs, y->name);
      syms[nsyms].sym = y;
      if (y->type == S_LABEL) {
        syms[nsyms].value = (uint32_t)y->val;
        syms[nsyms].shndx = 1 + idx;
        syms[nsyms].info = (global ? ((y->flags & SF_WEAK) ? 0x20 : 0x10) : 0) | (y->stype & 15);
        syms[nsyms].size = (uint32_t)y->ssize;
      } else if (y->type == S_IMPORT) {
        syms[nsyms].info = (y->flags & SF_WEAK) ? 0x20 : 0x10;
        syms[nsyms].shndx = 0;
      } else if (y->type == S_COMMON) {
        syms[nsyms].info = 0x11;
        syms[nsyms].value = (uint32_t)y->align;
        syms[nsyms].size = (uint32_t)y->val;
        syms[nsyms].shndx = 0xfff2;
      } else {
        ival v = 0;
        eval_const(y->e, &v);
        syms[nsyms].value = (uint32_t)v;
        syms[nsyms].info = global ? 0x10 : 0;
        syms[nsyms].shndx = 0xfff1;
      }
      nsyms++;
    }
  }
  /* layout */
  secoff = xcalloc(nps + 1, sizeof(long));
  reloff = xcalloc(nps + 1, sizeof(long));
  off = 52;
  for (i = 0; i < nps; i++) {
    secoff[i] = off;
    if (ps[i]->type != ST_BSS) off += (long)ps[i]->data.n;
  }
  {
    long shstroff = off;
    off += (long)shs.n;
    shoff = (off + 3) & ~3L;
    off = shoff + nsh * 40;
    symoff = (off + 3) & ~3L;
    off = symoff + nsyms * 16;
    stroff = off;
    off += (long)strs.n;
    for (i = 0; i < nps; i++)
      if (ps[i]->relocs) {
        off = (off + 3) & ~3L;
        reloff[i] = off;
        off += ps[i]->nrelocs * (cpu->elf_rel ? 8 : 12);
      }
    /* header */
    {
      uint8_t id[16] = { 0x7f, 'E', 'L', 'F', 1, 2, 1, 0 };
      if (out_le) id[5] = 1;
      wbytes(id, 16);
      w16(1); w16(cpu->elf_machine); w32(1); w32(0); w32(0); w32((uint32_t)shoff); w32(cpu->elf_flags);
      w16(52); w16(0); w16(0); w16(40); w16(nsh); w16(shstr_idx);
    }
    for (i = 0; i < nps; i++)
      if (ps[i]->type != ST_BSS) wbytes(ps[i]->data.d, ps[i]->data.n);
    wbytes(shs.d, shs.n);
    wpad(shoff);
    /* section headers */
    for (i = 0; i < 10; i++) w32(0);
    for (i = 0; i < nps; i++) {
      section *t = ps[i];
      w32(sh_name[i]);
      w32(t->type == ST_BSS ? 8 : 1);
      w32(t->elfflags ? (uint32_t)t->elfflags : t->type == ST_CODE ? 6 : 3);
      w32(0);
      w32((uint32_t)secoff[i]);
      w32((uint32_t)t->size);
      w32(0); w32(0);
      w32((uint32_t)(t->align > 0 ? t->align : 1));
      w32(0);
    }
    {
      int k = 0;
      for (i = 0; i < nps; i++)
        if (ps[i]->relocs) {
          int esz = cpu->elf_rel ? 8 : 12;
          w32(sh_name[nps + k++]);
          w32(cpu->elf_rel ? 9 : 4);
          w32(0);
          w32(0);
          w32((uint32_t)reloff[i]);
          w32(ps[i]->nrelocs * esz);
          w32(symtab_idx);
          w32(1 + i);
          w32(4);
          w32(esz);
        }
    }
    w32(17); w32(3); w32(0); w32(0); w32((uint32_t)shstroff); w32((uint32_t)shs.n); w32(0); w32(0); w32(1); w32(0);
    w32(1); w32(2); w32(0); w32(0); w32((uint32_t)symoff); w32(nsyms * 16); w32(strtab_idx); w32(firstglobal); w32(4); w32(16);
    w32(9); w32(3); w32(0); w32(0); w32((uint32_t)stroff); w32((uint32_t)strs.n); w32(0); w32(0); w32(1); w32(0);
  }
  wpad(symoff);
  for (i = 0; i < nsyms; i++) {
    w32(syms[i].name); w32(syms[i].value); w32(syms[i].size);
    fputc(syms[i].info, f); fputc(syms[i].other, f); w16(syms[i].shndx);
  }
  wbytes(strs.d, strs.n);
  if (!nrel) wpad((ftell(of) + 3) & ~3L);
  for (i = 0; i < nps; i++) {
    reloc *r;
    if (!ps[i]->relocs) continue;
    wpad(reloff[i]);
    for (r = ps[i]->relocs; r; r = r->next) {
      int t = elf_reloc_type(r), symi = 0, j;
      if (t < 0) { curpos = r->pos; error("reloc type not supported"); t = 0; }
      if (r->tsym) {
        for (j = 0; j < nsyms; j++) if (syms[j].sym == r->tsym) symi = j;
      } else {
        for (j = 0; j < nps; j++) if (ps[j] == r->tsec) symi = 1 + j;
      }
      w32((uint32_t)r->off);
      w32(((uint32_t)symi << 8) | t);
      if (!cpu->elf_rel) w32((uint32_t)r->addend);
    }
  }
  return errcount == 0;
}
out_module out_elf = { "elf", NULL, elf_write, 1, 0 };
