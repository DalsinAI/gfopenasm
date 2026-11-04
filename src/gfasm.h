/* gfopenasm (gfasm) - an open-source, vasm-compatible multi-target assembler.
 * Copyright (c) 2026 DalsinAI. MIT License, see LICENSE.
 *
 * Common core declarations.  The documents the assembler is written from
 * are listed in docs/REFERENCES.md.
 */
#ifndef GFASM_H
#define GFASM_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>

#define GFASM_VERSION "1.1.0"
#define GFASM_DATE    "4.11.2026"   /* release date, for the AmigaOS version string */

typedef int64_t ival;
typedef uint64_t uval;
ival flt_to_ival(double f);         /* the same result on every host */

/* ---------------------------------------------------------------- util */
void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);
int  stricmp_(const char *a, const char *b);
int  strnicmp_(const char *a, const char *b, size_t n);
char *strprintf(const char *fmt, ...);

/* host file names: '/' everywhere, also '\\' and "C:" on Windows, and
 * "volume:" with '/' for the parent directory on AmigaOS, MorphOS and AROS */
double gf_frexp(double x, int *e);   /* frexp() without libm */
int  path_is_sep(int c);
int  path_is_absolute(const char *p);
const char *path_base(const char *p);

typedef struct bbuf { uint8_t *d; size_t n, cap; } bbuf;
void bb_put(bbuf *b, const void *p, size_t n);
void bb_byte(bbuf *b, int v);
void bb_be16(bbuf *b, uint32_t v);
void bb_be32(bbuf *b, uint32_t v);
void bb_fill(bbuf *b, int v, size_t n);

/* --------------------------------------------------------- diagnostics */
extern int errcount, warncount, maxerrors, nowarn_all, quiet;
void error(const char *fmt, ...);
void warning(const char *fmt, ...);
void fatal(const char *fmt, ...);
void note_suppressed_begin(void);   /* suppress diagnostics (sizing passes) */
void note_suppressed_end(void);
extern int diag_suppress;

/* ----------------------------------------------------------- sources */
typedef struct srcfile {
  char *name;          /* name as given */
  char *path;          /* path opened */
  char **lines;
  int nlines;
} srcfile;

typedef struct srcpos {
  srcfile *file;
  int line;            /* 1-based line in file */
  const char *text;    /* expanded text (for messages) */
} srcpos;
extern srcpos curpos;

srcfile *load_source(const char *name, const char *path);

/* --------------------------------------------------------- expressions */
enum {
  X_NUM, X_FLT, X_SYM, X_NEG, X_CPL, X_NOT,
  X_MUL, X_DIV, X_MOD, X_SHL, X_SHR, X_AND, X_OR, X_XOR,
  X_ADD, X_SUB, X_LT, X_GT, X_LE, X_GE, X_EQ, X_NE, X_LAND, X_LOR
};

struct symbol;
struct section;

typedef struct expr {
  int op;
  struct expr *l, *r;
  ival v;
  double f;
  struct symbol *sym;
} expr;

/* Result of evaluating an expression.  A value is either absolute
 * (sec==NULL && ext==NULL), relative to the start of a section (sec), or
 * relative to an imported symbol (ext). */
typedef struct eres {
  ival v;
  struct section *sec;
  struct symbol *ext;
  int defined;      /* all symbols known */
  int bad;          /* illegal relocation */
  int isflt;
  double f;
} eres;

expr *new_num(ival v);
expr *new_flt(double f);
expr *new_sym(struct symbol *s);
expr *new_op(int op, expr *l, expr *r);
expr *copy_expr(expr *e);
/* parse an expression from *pp; returns NULL on syntax error */
expr *parse_expr(const char **pp);
int  eval_expr(expr *e, eres *r);
int  eval_const(expr *e, ival *v);      /* 1 if absolute & defined */
int  expr_is_float(expr *e);
double eres_float(eres *r);
expr *simplify_expr(expr *e);

/* hooks from syntax module for expression parsing */
extern int (*syntax_number)(const char **pp, expr **out);
extern expr *(*syntax_ident)(const char *name, size_t len);
extern expr *(*syntax_pcexpr)(void);
extern int expr_unsigned_shift;
int  is_idstart(int c);
int  is_idchar(int c);

/* ------------------------------------------------------------ symbols */
enum { S_NONE, S_LABEL, S_EXPR, S_SET, S_IMPORT, S_REG, S_REGLIST, S_FREG,
       S_FREGLIST, S_COMMON };
#define SF_EXPORT   1
#define SF_XREF     2
#define SF_WEAK     4
#define SF_USED     8
#define SF_LOCAL    16
#define SF_INTERNAL 32
#define SF_REFD     64
#define SF_DEFINED  128
#define SF_FLOAT    256
#define SF_NOLIST   512

typedef struct symbol {
  char *name;          /* full (mangled) name */
  char *dispname;      /* name as written */
  int type;
  int flags;
  struct section *sec;
  struct atom *atom;   /* label atom */
  ival val;            /* label offset / reg number / list mask / common size */
  ival align;
  expr *e;             /* S_EXPR / S_SET */
  struct symbol *hnext;
  struct symbol *lnext; /* definition order */
  int order;
  int stype;           /* ELF symbol type (0 notype, 1 object, 2 func) */
  ival ssize;          /* ELF symbol size */
  expr *size_e;        /* .size expression, evaluated at output */
} symbol;

symbol *sym_lookup(const char *name);
symbol *sym_get(const char *name);     /* create as S_NONE if missing */
symbol *sym_new_internal(void);
symbol *sym_new_anon(const char *name);   /* listed, not in the hash table */
extern symbol *sym_first, *sym_last;
void sym_define_label(symbol *s, struct section *sec, struct atom *a);
int  sym_is_defined(symbol *s);

/* ----------------------------------------------------------- sections */
#define ST_CODE 0
#define ST_DATA 1
#define ST_BSS  2

#define SEC_ABS     1   /* absolute ORG section */
#define SEC_OFFSET  2   /* offset section, not emitted */
#define SEC_UNNAMED 4

enum { AT_LABEL, AT_INSN, AT_DATA, AT_SPACE, AT_ALIGN, AT_BYTES, AT_RORG,
       AT_FLOAT };

typedef struct dataitem {
  expr *e;             /* value, or NULL for string */
  char *str; int slen; /* string */
} dataitem;

typedef struct atom {
  int type;
  struct atom *next;
  ival off;
  ival size;
  srcpos pos;
  int lfirst;          /* listing: line index */
  union {
    struct { symbol *sym; } label;
    struct { void *insn; } insn;
    struct { int esize; int ftype; int n; dataitem *items; int reldiff; } data;
    struct { expr *count; int esize; expr *fill; int ftype; int uninit; } space;
    struct { ival align; ival offset; int nopfill; int bitalign; expr *fill; int fillsize; ival maxpad; } align;
    struct { uint8_t *d; size_t n; } bytes;
    struct { expr *target; expr *fill; } rorg;
  } u;
} atom;

typedef struct reloc {
  struct reloc *next;
  ival off;            /* byte offset in section */
  int size;            /* bits */
  int type;            /* R_ABS, R_PC, R_SD, ... */
  int mod;             /* cpu specific modifier (e.g. PPC @l/@h/@ha) */
  uint32_t mask;       /* bit mask inside the field, 0 = whole field */
  struct section *tsec;
  symbol *tsym;
  ival addend;
  srcpos pos;
} reloc;
#define R_ABS 0
#define R_PC  1
#define R_SD  2
#define R_SD2 3
#define R_PLT 4
#define R_GOT 5

typedef struct section {
  char *name;
  int type;
  uint32_t memflags;
  int flags;
  ival org;
  atom *first, *last;
  ival size;
  ival align;
  int idx;
  bbuf data;
  reloc *relocs, *rlast;
  int nrelocs;
  int has_code;
  struct section *next;
  int sdsec;
  int elfflags;        /* SHF_* from std section attributes, 0 = default */
} section;

extern section *sec_first, *sec_last, *cursec;
section *find_section(const char *name, int type, uint32_t mem);
section *new_section(const char *name, int type, uint32_t mem);
section *new_org_section(ival org);
section *default_section(void);
atom *add_atom(int type);
void add_label_atom(symbol *s);
extern int default_sec_is_org;

/* ------------------------------------------------------ assembly passes */
void resolve_sections(void);
void finalize_sections(void);
void add_reloc(section *s, ival off, int size, int type, section *tsec,
               symbol *tsym, ival addend);
extern int final_pass;
extern int pass_no;
extern int grow_only;

/* ----------------------------------------------------- source handling */
typedef struct macro {
  char *name;
  char **lines; int nlines;
  char **argnames; int nargnames;
  char **argdefs;
  struct macro *next;
  srcpos defpos;
} macro;

macro *find_macro(const char *name, size_t len);
macro *new_macro(const char *name);
void macro_add_line(macro *m, const char *l);
void push_file(const char *name);
void push_macro(macro *m, char **args, int nargs, const char *qual);
void push_rept(char **lines, int nlines, int count);
void push_irp(char **lines, int nlines, const char *argname, char **vals, int nvals);
const char *next_line(void);
void exit_macro(void);
int  in_macro(void);
void end_assembly(void);
char *find_include(const char *name, int bin);
void add_incpath(const char *p);
extern int macro_args_max;
extern int macro_id_plain;
extern int max_macro_recursion;
extern int ignore_mult_inc;
extern char *compile_dir;
extern int end_reached;

/* conditional assembly */
void cond_push(int truth);
void cond_else(void);
void cond_elseif(int truth);
void cond_end(void);
int  cond_active(void);
int  cond_depth(void);
int  cond_parent_active(void);
extern int cond_level;

/* ------------------------------------------------------- module hooks */
typedef struct cpu_module {
  const char *name;
  int  (*init)(void);
  int  (*option)(const char *opt);
  /* returns 1 if mnemonic recognized and handled */
  int  (*instruction)(const char *mnem, size_t mlen, const char *ext,
                      char **ops, int nops, int startcol);
  int  (*directive)(const char *name, const char *args);
  ival (*insn_size)(atom *a, section *s, ival pc);
  void (*insn_encode)(atom *a, section *s, ival pc, bbuf *out);
  int  (*parse_reg)(const char **pp);           /* for equr */
  void (*define_symbols)(void);
  int  (*float_data)(int ftype, eres *r, uint8_t *out); /* encode float */
  int  big_endian;
  int  insn_align;     /* automatic instruction alignment */
  int  data_align;     /* syntax modules align data naturally */
  int  addr_bits;
  int  elf_machine;
  /* ELF relocation type for a relocation, -1 if unsupported */
  int  (*elf_reloc)(struct reloc *r);
  int  elf_rel;        /* ELF: REL (addend in place) instead of RELA */
  uint32_t elf_flags;  /* ELF e_flags */
  void (*parse_done)(void);    /* after the source was read */
  void (*post_layout)(void);   /* after the final pass, before output */
} cpu_module;
extern cpu_module cpu_ppc, cpu_arm, cpu_x86, cpu_z80, cpu_6502;

typedef struct syntax_module {
  const char *name;
  void (*init)(void);
  void (*parse_line)(const char *line);
  void (*finish)(void);
  int  (*option)(const char *opt);
} syntax_module;
extern syntax_module *syntax;
extern syntax_module syntax_mot, syntax_std, syntax_oldstyle;
extern cpu_module *cpu;
extern cpu_module cpu_m68k;

typedef struct out_module {
  const char *name;
  int  (*option)(const char *opt);
  int  (*write)(FILE *f, const char *outname);
  int  named_sections;
  int  default_org;   /* default section is ORG 0 */
} out_module;
extern out_module *outmod;
extern out_module out_hunk, out_hunkexe, out_elf, out_bin, out_vobj_dummy;

/* --------------------------------------------------------- globals */
extern const char *input_name;
extern const char *output_name;
extern int opt_nosym, opt_esc, opt_nocase, opt_spaces, opt_x, opt_pic;
extern int opt_unnamed_sections, opt_noialign, opt_chklabels;
extern ival opt_uspc;
extern uint8_t opt_pad[8]; extern int opt_padlen;
extern int listing_enabled;
extern char *idnt_name;
extern char *elf_file_name;   /* std .file */
extern char *list_name;
extern int list_on;

void listing_line(const char *text);
void write_listing(void);

/* misc parse helpers (mot syntax) */
const char *skip_ws(const char *p);
int is_ws(int c);

/* reptn etc. */
extern symbol *sym_reptn, *sym_narg, *sym_carg;
void set_internal(symbol *s, ival v);

void dump_symbol_names_for_test(void);

#endif
