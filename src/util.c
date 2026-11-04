/* gfasm - utilities, diagnostics, source loading.  MIT License. */
#include "gfasm.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int errcount, warncount, maxerrors = 5, nowarn_all, quiet;
int diag_suppress;
srcpos curpos;

void *xmalloc(size_t n)
{
  void *p = malloc(n ? n : 1);
  if (!p) { fprintf(stderr, "out of memory\n"); exit(20); }
  return p;
}
void *xcalloc(size_t n, size_t m)
{
  void *p = calloc(n ? n : 1, m ? m : 1);
  if (!p) { fprintf(stderr, "out of memory\n"); exit(20); }
  return p;
}
void *xrealloc(void *p, size_t n)
{
  p = realloc(p, n ? n : 1);
  if (!p) { fprintf(stderr, "out of memory\n"); exit(20); }
  return p;
}
char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
char *xstrndup(const char *s, size_t n)
{
  char *d = xmalloc(n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}
int stricmp_(const char *a, const char *b)
{
  while (*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
int strnicmp_(const char *a, const char *b, size_t n)
{
  while (n && *a && tolower((unsigned char)*a) == tolower((unsigned char)*b))
    a++, b++, n--;
  if (!n) return 0;
  return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}
char *strprintf(const char *fmt, ...)
{
  va_list ap;
  int n;
  char *s;
  va_start(ap, fmt);
  n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  s = xmalloc(n + 1);
  va_start(ap, fmt);
  vsnprintf(s, n + 1, fmt, ap);
  va_end(ap);
  return s;
}

void bb_put(bbuf *b, const void *p, size_t n)
{
  if (b->n + n > b->cap) {
    b->cap = (b->n + n) * 2 + 64;
    b->d = xrealloc(b->d, b->cap);
  }
  if (p) memcpy(b->d + b->n, p, n); else memset(b->d + b->n, 0, n);
  b->n += n;
}
void bb_byte(bbuf *b, int v) { uint8_t c = (uint8_t)v; bb_put(b, &c, 1); }
void bb_be16(bbuf *b, uint32_t v) { bb_byte(b, v >> 8); bb_byte(b, v); }
void bb_be32(bbuf *b, uint32_t v) { bb_be16(b, v >> 16); bb_be16(b, v & 0xffff); }
void bb_fill(bbuf *b, int v, size_t n) { while (n--) bb_byte(b, v); }

static void diag(const char *kind, const char *fmt, va_list ap)
{
  fprintf(stderr, "\n%s", kind);
  if (curpos.file)
    fprintf(stderr, " in line %d of \"%s\"", curpos.line, curpos.file->name);
  fprintf(stderr, ": ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  if (curpos.text) fprintf(stderr, ">%s\n", curpos.text);
}

void error(const char *fmt, ...)
{
  va_list ap;
  if (diag_suppress) return;
  errcount++;
  va_start(ap, fmt);
  diag("error", fmt, ap);
  va_end(ap);
  if (maxerrors && errcount >= maxerrors) {
    fprintf(stderr, "\nfatal error: maximum number of errors reached\naborting...\n");
    exit(20);
  }
}
void warning(const char *fmt, ...)
{
  va_list ap;
  if (diag_suppress || nowarn_all) return;
  warncount++;
  va_start(ap, fmt);
  diag("warning", fmt, ap);
  va_end(ap);
}
void fatal(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  diag("fatal error", fmt, ap);
  va_end(ap);
  fprintf(stderr, "aborting...\n");
  exit(20);
}

srcfile *load_source(const char *name, const char *path)
{
  FILE *f = fopen(path, "rb");
  srcfile *sf;
  char *buf, *p, *e;
  long n;
  int cap = 0;
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  buf = xmalloc(n + 2);
  if (n > 0 && fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
  fclose(f);
  buf[n] = 0;
  sf = xcalloc(1, sizeof(*sf));
  sf->name = xstrdup(name);
  sf->path = xstrdup(path);
  p = buf;
  e = buf + n;
  while (p < e) {
    char *q = p;
    while (q < e && *q != '\n' && *q != '\r') q++;
    if (sf->nlines >= cap) {
      cap = cap * 2 + 256;
      sf->lines = xrealloc(sf->lines, cap * sizeof(char *));
    }
    {
      /* strip a trailing ^Z and NUL bytes */
      size_t len = q - p;
      char *l = xstrndup(p, len);
      size_t i;
      for (i = 0; i < len; i++) if (l[i] == 0 || l[i] == 0x1a) l[i] = ' ';
      sf->lines[sf->nlines++] = l;
    }
    if (q < e && *q == '\r') { q++; if (q < e && *q == '\n') q++; }
    else if (q < e) q++;
    p = q;
  }
  free(buf);
  return sf;
}

/* ------------------------------------------------------- host paths */
#if defined(_WIN32)
#define HOST_DOS 1
#elif defined(__amigaos__) || defined(__amigaos4__) || defined(__MORPHOS__) || defined(__AROS__)
#define HOST_AMIGA 1
#endif

int path_is_sep(int c)
{
#if defined(HOST_DOS)
  return c == '/' || c == '\\' || c == ':';
#elif defined(HOST_AMIGA)
  return c == '/' || c == ':';
#else
  return c == '/';
#endif
}

int path_is_absolute(const char *p)
{
#if defined(HOST_DOS)
  return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
#elif defined(HOST_AMIGA)
  return strchr(p, ':') != NULL;    /* "volume:dir/file"; a leading '/' is the parent */
#else
  return p[0] == '/';
#endif
}

/* Sources written on Unix or Windows name the parent directory "..".  On
 * AmigaOS, MorphOS and AROS the parent is an empty part ("/x", "a//x"), so
 * "." and ".." parts are rewritten; elsewhere the name is used as it is. */
char *path_host(const char *p)
{
#if defined(HOST_AMIGA)
  char *o = xmalloc(strlen(p) + 1), *q = o;
  while (*p) {
    if (p[0] == '.' && (p[1] == '/' || !p[1]) && (q == o || q[-1] == '/' || q[-1] == ':')) {
      p += p[1] ? 2 : 1;                       /* "./" */
    } else if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || !p[2]) &&
               (q == o || q[-1] == '/' || q[-1] == ':')) {
      *q++ = '/';                              /* "../" -> "/" */
      p += p[2] ? 3 : 2;
    } else {
      while (*p && *p != '/') *q++ = *p++;
      if (*p) *q++ = *p++;
    }
  }
  *q = 0;
  return o;
#else
  return xstrdup(p);
#endif
}

const char *path_base(const char *p)
{
  const char *b = p;
  for (; *p; p++) if (path_is_sep((unsigned char)*p)) b = p + 1;
  return b;
}

int is_ws(int c) { return c == ' ' || c == '\t'; }
const char *skip_ws(const char *p) { while (is_ws(*p)) p++; return p; }
