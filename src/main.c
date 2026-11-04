/* gfasm - driver and command line.  MIT License. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

cpu_module *cpu = &cpu_m68k;
syntax_module *syntax = &syntax_mot;
const char *input_name;
const char *output_name;
int opt_nosym, opt_x, opt_pic, opt_unnamed_sections, opt_chklabels;
ival opt_uspc;
uint8_t opt_pad[8]; int opt_padlen = 1;
int listing_enabled;
char *list_name;

extern int opt_esc, opt_spaces, opt_align, opt_devpac, opt_phxass, opt_localu, opt_warncomm;
extern int ident_dots;
extern uint16_t cnop_code;
extern int opt_keepempty, opt_kick1hunks, opt_hunkpad, opt_databss;

static int select_cpu(const char *n)
{
  if (!strcmp(n, "68k") || !strcmp(n, "m68k")) { cpu = &cpu_m68k; return 1; }
  if (!strcmp(n, "ppc")) { cpu = &cpu_ppc; return 1; }
  if (!strcmp(n, "arm")) { cpu = &cpu_arm; return 1; }
  if (!strcmp(n, "x86")) { cpu = &cpu_x86; return 1; }
  if (!strcmp(n, "z80")) { cpu = &cpu_z80; return 1; }
  return 0;
}
static int select_syntax(const char *n)
{
  if (!strcmp(n, "mot")) { syntax = &syntax_mot; return 1; }
  if (!strcmp(n, "std")) { syntax = &syntax_std; return 1; }
  if (!strcmp(n, "oldstyle")) { syntax = &syntax_oldstyle; return 1; }
  return 0;
}

static void usage(void)
{
  fprintf(stderr,
    "gfasm " GFASM_VERSION " - clean-room vasm-compatible assembler (MIT)\n"
    "usage: gfasm [-cpu=68k|ppc|arm|x86|z80] [-syntax=mot|std|oldstyle] [options] file\n"
    "  -F<fmt>  output format: hunk, hunkexe, elf, bin\n"
    "  -o file  output file (default a.out)\n"
    "  -I path  include path     -D name[=val] define symbol\n"
    "  -L file  listing file     -nosym -quiet -x -w -maxerrors=n\n"
    "  -m68000..-m68060 -m68020up -mcpu32 -m68881 -m68882 -m68851\n"
    "  -no-opt -opt-<name> -devpac -phxass -spaces -ldots -esc ...\n");
}

static void define_cmdline(const char *d)
{
  const char *eq = strchr(d, '=');
  char *name = eq ? xstrndup(d, eq - d) : xstrdup(d);
  symbol *s = sym_get(name);
  if (eq) {
    const char *p = eq + 1;
    expr *e = parse_expr(&p);
    if (!e) { fprintf(stderr, "bad -D expression: %s\n", d); exit(1); }
    s->e = e;
  } else
    s->e = new_num(1);
  s->type = S_EXPR;
  s->flags |= SF_DEFINED;
  free(name);
}

static int parse_pad(const char *v)
{
  char *end;
  unsigned long x = strtoul(v[0] == '$' ? v + 1 : v, &end, (v[0] == '$' || (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))) ? 16 : 10);
  opt_pad[0] = (uint8_t)x;
  opt_padlen = 1;
  if (x > 0xff) { opt_pad[0] = x >> 8; opt_pad[1] = x; opt_padlen = 2; }
  if (x > 0xffff) { opt_pad[0] = x >> 24; opt_pad[1] = x >> 16; opt_pad[2] = x >> 8; opt_pad[3] = x; opt_padlen = 4; }
  return 1;
}

static long numarg(const char *v)
{
  if (v[0] == '$') return strtol(v + 1, NULL, 16);
  return strtol(v, NULL, 0);
}

int main(int argc, char **argv)
{
  int i;
  char **defs = NULL; int ndefs = 0;
  const char *fmt = NULL;
  FILE *f;
  out_module *om;

  outmod = &out_bin;   /* vasm default would be "test"; bin is our default */
  {
    /* called as vasm<cpu>_<syntax>? */
    const char *b = strrchr(argv[0], '/');
    b = b ? b + 1 : argv[0];
    if (!strncmp(b, "vasm", 4)) {
      const char *u = strchr(b, '_');
      if (u) {
        char *c = xstrndup(b + 4, u - b - 4);
        if (!strcmp(c, "m68k")) select_cpu("68k"); else select_cpu(c);
        free(c);
        select_syntax(u + 1);
      }
    }
    for (i = 1; i < argc; i++) {
      if (!strncmp(argv[i], "-cpu=", 5) && !select_cpu(argv[i] + 5)) {
        fprintf(stderr, "fatal error: cpu <%s> not supported\n", argv[i] + 5); return 1;
      }
      if (!strncmp(argv[i], "-syntax=", 8) && !select_syntax(argv[i] + 8)) {
        fprintf(stderr, "fatal error: syntax <%s> not supported\n", argv[i] + 8); return 1;
      }
    }
  }
  /* first pass: output format, so that defaults depend on it */
  for (i = 1; i < argc; i++)
    if (!strncmp(argv[i], "-F", 2)) fmt = argv[i] + 2;
  if (fmt) {
    if (!strcmp(fmt, "hunk")) om = &out_hunk;
    else if (!strcmp(fmt, "hunkexe")) om = &out_hunkexe;
    else if (!strcmp(fmt, "elf")) om = &out_elf;
    else if (!strcmp(fmt, "bin")) om = &out_bin;
    else { fprintf(stderr, "fatal error: unknown output format <%s>\n", fmt); return 1; }
    outmod = om;
  }
  default_sec_is_org = outmod->default_org;
  cpu->init();

  for (i = 1; i < argc; i++) {
    char *a = argv[i];
    if (a[0] != '-') {
      if (input_name) { fprintf(stderr, "fatal error: multiple input files\n"); return 1; }
      input_name = a;
      continue;
    }
    if (!strncmp(a, "-F", 2)) continue;
    if (!strncmp(a, "-cpu=", 5) || !strncmp(a, "-syntax=", 8)) continue;
    if (syntax->option && syntax->option(a)) continue;
    if (!strncmp(a, "-opt-", 5) && cpu->option(a)) continue;   /* not "-o pt-..." */
    if (!strcmp(a, "-o")) { if (++i < argc) output_name = argv[i]; continue; }
    if (!strncmp(a, "-o", 2) && a[2]) { output_name = a + 2; continue; }
    if (!strcmp(a, "-I")) { if (++i < argc) add_incpath(argv[i]); continue; }
    if (!strncmp(a, "-I", 2)) { add_incpath(a + 2); continue; }
    if (!strcmp(a, "-D")) { if (++i < argc) { defs = xrealloc(defs, (ndefs + 1) * sizeof(char *)); defs[ndefs++] = argv[i]; } continue; }
    if (!strncmp(a, "-D", 2)) { defs = xrealloc(defs, (ndefs + 1) * sizeof(char *)); defs[ndefs++] = a + 2; continue; }
    if (!strcmp(a, "-L")) { if (++i < argc) { list_name = argv[i]; listing_enabled = 1; } continue; }
    if (!strncmp(a, "-L", 2) && (!strcmp(a, "-Lall") || !strcmp(a, "-Llo") || !strcmp(a, "-Lni") ||
        !strcmp(a, "-Lns") || !strncmp(a, "-Lbpl=", 6) || !strncmp(a, "-Lfmt=", 6))) continue;
    if (!strcmp(a, "-nosym")) { opt_nosym = 1; continue; }
    if (!strcmp(a, "-quiet")) { quiet = 1; continue; }
    if (!strcmp(a, "-x")) { opt_x = 1; continue; }
    if (!strcmp(a, "-w")) { nowarn_all = 1; continue; }
    if (!strcmp(a, "-wfail")) continue;
    if (!strncmp(a, "-nowarn=", 8)) continue;
    if (!strncmp(a, "-nomsg=", 7)) continue;
    if (!strncmp(a, "-maxerrors=", 11)) { maxerrors = atoi(a + 11); continue; }
    if (!strncmp(a, "-maxmacrecurs=", 14)) { max_macro_recursion = atoi(a + 14); continue; }
    if (!strcmp(a, "-esc")) { opt_esc = 1; continue; }
    if (!strcmp(a, "-noesc")) { opt_esc = 0; continue; }
    if (!strcmp(a, "-nocase")) { extern int opt_nocase; opt_nocase = 1; continue; }
    if (!strcmp(a, "-pic")) { opt_pic = 1; continue; }
    if (!strcmp(a, "-unnamed-sections")) { opt_unnamed_sections = 1; continue; }
    if (!strcmp(a, "-ignore-mult-inc")) { ignore_mult_inc = 1; continue; }
    if (!strcmp(a, "-noialign")) { opt_noialign = 1; continue; }
    if (!strcmp(a, "-chklabels")) { opt_chklabels = 1; continue; }
    if (!strcmp(a, "-unsshift")) { expr_unsigned_shift = 1; continue; }
    if (!strncmp(a, "-uspc=", 6)) { opt_uspc = numarg(a + 6); continue; }
    if (!strncmp(a, "-pad=", 5)) { parse_pad(a + 5); continue; }
    if (!strncmp(a, "-depend=", 8) || !strncmp(a, "-dependall=", 11) || !strncmp(a, "-dwarf", 6)) continue;
    if (!strcmp(a, "-depfile")) { i++; continue; }
    /* syntax module options */
    if (!strcmp(a, "-align")) { opt_align = 1; continue; }
    if (!strcmp(a, "-allmp")) { macro_args_max = 35; continue; }
    if (!strncmp(a, "-cnop=", 6)) { cnop_code = (uint16_t)numarg(a + 6); continue; }
    if (!strcmp(a, "-ldots")) { ident_dots = 1; continue; }
    if (!strcmp(a, "-localu")) { opt_localu = 1; continue; }
    if (!strcmp(a, "-spaces")) { opt_spaces = 1; continue; }
    if (!strcmp(a, "-warncomm")) { opt_warncomm = 1; continue; }
    if (!strcmp(a, "-devpac")) { opt_devpac = 1; opt_align = 1; opt_esc = 0; ident_dots = 1; cpu->option(a); continue; }
    if (!strcmp(a, "-phxass")) { opt_phxass = 1; opt_esc = 1; opt_spaces = 1; ident_dots = 1; cpu->option(a); continue; }
    /* output module options */
    if (!strcmp(a, "-keepempty")) { opt_keepempty = 1; continue; }
    if (!strcmp(a, "-kick1hunks")) { opt_kick1hunks = 1; cpu->option(a); continue; }
    if (!strncmp(a, "-hunkpad=", 9)) { opt_hunkpad = (int)numarg(a + 9); continue; }
    if (!strcmp(a, "-linedebug")) continue;
    if (!strcmp(a, "-databss")) { opt_databss = 1; continue; }
    if (!strncmp(a, "-exec=", 6)) continue;
    if (cpu->option(a)) continue;
    if (!strcmp(a, "-h") || !strcmp(a, "-help") || !strcmp(a, "--help")) { usage(); return 0; }
    fprintf(stderr, "error: unknown option <%s>\n", a);
    return 1;
  }
  if (!input_name) { usage(); return 1; }
  if (!quiet)
    printf("gfasm %s (c) 2026 DalsinAI - MIT licensed, vasm-compatible\n", GFASM_VERSION);

  {
    const char *sl = strrchr(input_name, '/');
    compile_dir = sl ? xstrndup(input_name, sl - input_name + 1) : xstrdup("");
  }
  syntax->init();
  cpu->define_symbols();
  for (i = 0; i < ndefs; i++) define_cmdline(defs[i]);

  push_file(input_name);
  {
    const char *l;
    while ((l = next_line()) != NULL) syntax->parse_line(l);
  }
  curpos.file = NULL;
  curpos.text = NULL;
  syntax->finish();
  if (cpu->parse_done) cpu->parse_done();

  /* undefined symbols become imports */
  {
    symbol *s;
    for (s = sym_first; s; s = s->lnext)
      if (s->type == S_NONE && (s->flags & SF_REFD)) {
        if (opt_x || outmod == &out_bin || outmod == &out_hunkexe) {
          if (opt_x) error("undefined symbol <%s>", s->dispname);
        }
        s->type = S_IMPORT;
      }
  }
  if (errcount) return 1;
  if (!sec_first) default_section();
  resolve_sections();
  finalize_sections();
  if (cpu->post_layout) cpu->post_layout();
  if (errcount) return 1;
  if (!output_name) output_name = "a.out";
  f = fopen(output_name, "wb");
  if (!f) { fprintf(stderr, "fatal error: can't open output file <%s>\n", output_name); return 1; }
  if (!outmod->write(f, output_name) || errcount) {
    fclose(f);
    remove(output_name);
    return 1;
  }
  fclose(f);
  if (listing_enabled) write_listing();
  if (!quiet) {
    section *s;
    for (s = sec_first; s; s = s->next)
      if (!(s->flags & SEC_OFFSET))
        printf("%s(%s%lu):\t%12lu bytes\n", s->name,
               s->type == ST_CODE ? "acrx" : s->type == ST_DATA ? "adrw" : "aurw",
               (unsigned long)s->align, (unsigned long)s->size);
  }
  return 0;
}

/* minimal listing: address and source line per atom */
void write_listing(void)
{
  FILE *f = fopen(list_name, "w");
  section *s;
  if (!f) { error("can't open listing file"); return; }
  for (s = sec_first; s; s = s->next) {
    atom *a;
    if (s->flags & SEC_OFFSET) continue;
    fprintf(f, "Section %s:\n", s->name);
    for (a = s->first; a; a = a->next) {
      if (a->type == AT_LABEL) continue;
      fprintf(f, "%08lx", (unsigned long)a->off);
      if (s->data.d && a->size && a->off < (ival)s->data.n) {
        ival k;
        fputc(' ', f);
        for (k = 0; k < a->size && k < 8 && a->off + k < (ival)s->data.n; k++)
          fprintf(f, "%02X", s->data.d[a->off + k]);
      }
      fprintf(f, "\t%s\n", a->pos.text ? a->pos.text : "");
    }
  }
  fclose(f);
}

void listing_line(const char *text) { (void)text; }
