/* gfasm - floating point in software.  MIT License.
 *
 * Float constants are IEEE 754 doubles, kept as their 64 bits (fval) and
 * computed here with integer arithmetic only: decimal to binary
 * conversion, + - * /, conversion to single precision and to integers, and
 * binary to decimal for packed decimal constants.  Every result is
 * correctly rounded (to nearest, ties to even), denormals included, so the
 * output is the same on every host, whatever its FPU, its C library or its
 * long double, and the same as vasm's on x86-64.  A NaN that an operation
 * makes is the one x86 makes (sign set, quiet).
 *
 * The add, subtract, multiply and round-and-pack steps follow the
 * algorithms of IEEE 754 itself (as in J. Hauser's public SoftFloat
 * design); decimal conversion uses exact big-integer arithmetic. */
#include "gfasm.h"
#include <string.h>
#include <ctype.h>

#define F_SIGN(a) ((int)((a) >> 63))
#define F_EXP(a)  ((int)(((a) >> 52) & 0x7ff))
#define F_FRAC(a) ((a) & 0x000fffffffffffffULL)
#define F_PACK(s, e, m) (((uint64_t)(s) << 63) + ((uint64_t)(e) << 52) + (m))
#define F_DEFNAN 0xfff8000000000000ULL   /* x86's default NaN */

int f_isnan(fval a) { return F_EXP(a) == 0x7ff && F_FRAC(a) != 0; }
int f_isinf(fval a) { return F_EXP(a) == 0x7ff && F_FRAC(a) == 0; }
int f_iszero(fval a) { return (a << 1) == 0; }
int f_sign(fval a) { return F_SIGN(a); }
fval f_neg(fval a) { return a ^ 0x8000000000000000ULL; }
int f_eq(fval a, fval b) { return !f_isnan(a) && !f_isnan(b) && (a == b || ((a | b) << 1) == 0); }

static int clz64(uint64_t a)
{
  int n = 0;
  if (!a) return 64;
  while (!(a & 0x8000000000000000ULL)) { a <<= 1; n++; }
  return n;
}

static uint64_t shr_jam(uint64_t a, int dist)   /* shift right, OR the lost bits into bit 0 */
{
  if (dist <= 0) return a;
  if (dist >= 64) return a != 0;
  return (a >> dist) | ((a << (64 - dist)) != 0);
}

/* nan operand(s): pass the first NaN on, made quiet */
static fval nan2(fval a, fval b)
{
  if (f_isnan(a)) return a | 0x0008000000000000ULL;
  return b | 0x0008000000000000ULL;
}

/* sig: the significand with its leading bit at bit 62 and 10 rounding
 * bits; the value is sig * 2^(exp - 0x3fe - 62) */
static fval round_pack(int sign, int exp, uint64_t sig)
{
  int rbits = (int)(sig & 0x3ff);
  if ((unsigned)exp >= 0x7fd) {
    if (exp < 0) {
      sig = shr_jam(sig, -exp);
      exp = 0;
      rbits = (int)(sig & 0x3ff);
    } else if (exp > 0x7fd || sig + 0x200 >= 0x8000000000000000ULL) {
      return F_PACK(sign, 0x7ff, 0);
    }
  }
  sig = (sig + 0x200) >> 10;
  if (rbits == 0x200) sig &= ~(uint64_t)1;     /* a tie: to even */
  if (!sig) exp = 0;
  return F_PACK(sign, exp, sig);
}

static fval norm_round_pack(int sign, int exp, uint64_t sig)
{
  int sh = clz64(sig) - 1;
  exp -= sh;
  if (sh >= 10 && (unsigned)exp < 0x7fd)
    return F_PACK(sign, sig ? exp : 0, sig << (sh - 10));
  return round_pack(sign, exp, sig << sh);
}

static fval add_mags(fval a, fval b, int sign)
{
  int ea = F_EXP(a), eb = F_EXP(b), d = ea - eb, ez;
  uint64_t sa = F_FRAC(a), sb = F_FRAC(b), sz;
  if (!d) {
    if (!ea) return a + sb;                        /* both denormal */
    if (ea == 0x7ff) return (sa | sb) ? nan2(a, b) : a;
    ez = ea;
    sz = (0x0020000000000000ULL + sa + sb) << 9;
  } else {
    sa <<= 9; sb <<= 9;
    if (d < 0) {
      if (eb == 0x7ff) return sb ? nan2(a, b) : F_PACK(sign, 0x7ff, 0);
      ez = eb;
      if (ea) sa += 0x2000000000000000ULL; else sa <<= 1;
      sa = shr_jam(sa, -d);
    } else {
      if (ea == 0x7ff) return sa ? nan2(a, b) : a;
      ez = ea;
      if (eb) sb += 0x2000000000000000ULL; else sb <<= 1;
      sb = shr_jam(sb, d);
    }
    sz = 0x2000000000000000ULL + sa + sb;
    if (sz < 0x4000000000000000ULL) { ez--; sz <<= 1; }
  }
  return round_pack(sign, ez, sz);
}

static fval sub_mags(fval a, fval b, int sign)
{
  int ea = F_EXP(a), eb = F_EXP(b), d = ea - eb, ez;
  uint64_t sa = F_FRAC(a), sb = F_FRAC(b), sz;
  if (!d) {
    int64_t diff;
    int sh;
    if (ea == 0x7ff) return (sa | sb) ? nan2(a, b) : F_DEFNAN;
    diff = (int64_t)(sa - sb);
    if (!diff) return 0;                           /* +0 */
    if (ea) ea--;
    if (diff < 0) { sign = !sign; diff = -diff; }
    sh = clz64((uint64_t)diff) - 11;
    ez = ea - sh;
    if (ez < 0) { sh = ea; ez = 0; }
    return F_PACK(sign, ez, (uint64_t)diff << sh);
  }
  sa <<= 10; sb <<= 10;
  if (d < 0) {
    sign = !sign;
    if (eb == 0x7ff) return sb ? nan2(a, b) : F_PACK(sign, 0x7ff, 0);
    sa += ea ? 0x4000000000000000ULL : sa;
    sa = shr_jam(sa, -d);
    sb |= 0x4000000000000000ULL;
    ez = eb;
    sz = sb - sa;
  } else {
    if (ea == 0x7ff) return sa ? nan2(a, b) : a;
    sb += eb ? 0x4000000000000000ULL : sb;
    sb = shr_jam(sb, d);
    sa |= 0x4000000000000000ULL;
    ez = ea;
    sz = sa - sb;
  }
  return norm_round_pack(sign, ez - 1, sz);
}

fval f_add(fval a, fval b)
{
  return F_SIGN(a) == F_SIGN(b) ? add_mags(a, b, F_SIGN(a)) : sub_mags(a, b, F_SIGN(a));
}

fval f_sub(fval a, fval b)
{
  return F_SIGN(a) == F_SIGN(b) ? sub_mags(a, b, F_SIGN(a)) : add_mags(a, b, F_SIGN(a));
}

static void norm_sub(int *e, uint64_t *s)    /* normalise a denormal significand */
{
  int sh = clz64(*s) - 11;
  *e = 1 - sh;
  *s <<= sh;
}

fval f_mul(fval a, fval b)
{
  int sign = F_SIGN(a) ^ F_SIGN(b);
  int ea = F_EXP(a), eb = F_EXP(b), ez;
  uint64_t sa = F_FRAC(a), sb = F_FRAC(b), sz, hi, lo, t;
  uint32_t a0, a1, b0, b1;
  if (ea == 0x7ff) {
    if (sa || (eb == 0x7ff && sb)) return nan2(a, b);
    return (eb | sb) ? F_PACK(sign, 0x7ff, 0) : F_DEFNAN;
  }
  if (eb == 0x7ff) {
    if (sb) return nan2(a, b);
    return (ea | sa) ? F_PACK(sign, 0x7ff, 0) : F_DEFNAN;
  }
  if (!ea) { if (!sa) return F_PACK(sign, 0, 0); norm_sub(&ea, &sa); }
  if (!eb) { if (!sb) return F_PACK(sign, 0, 0); norm_sub(&eb, &sb); }
  ez = ea + eb - 0x3ff;
  sa = (sa | 0x0010000000000000ULL) << 10;
  sb = (sb | 0x0010000000000000ULL) << 11;
  /* 64 x 64 -> 128 bits */
  a0 = (uint32_t)sa; a1 = (uint32_t)(sa >> 32);
  b0 = (uint32_t)sb; b1 = (uint32_t)(sb >> 32);
  lo = (uint64_t)a0 * b0;
  t = (uint64_t)a1 * b0 + (lo >> 32);
  hi = (uint64_t)a1 * b1 + (t >> 32);
  t = (uint64_t)a0 * b1 + (uint32_t)t;
  hi += t >> 32;
  lo = (t << 32) | (uint32_t)lo;
  sz = hi | (lo != 0);
  if (sz < 0x4000000000000000ULL) { ez--; sz <<= 1; }
  return round_pack(sign, ez, sz);
}

fval f_div(fval a, fval b)
{
  int sign = F_SIGN(a) ^ F_SIGN(b);
  int ea = F_EXP(a), eb = F_EXP(b), ez, i;
  uint64_t sa = F_FRAC(a), sb = F_FRAC(b), q = 0, rem;
  if (ea == 0x7ff) {
    if (sa) return nan2(a, b);
    if (eb == 0x7ff) return sb ? nan2(a, b) : F_DEFNAN;
    return F_PACK(sign, 0x7ff, 0);
  }
  if (eb == 0x7ff) return sb ? nan2(a, b) : F_PACK(sign, 0, 0);
  if (!eb) {
    if (!sb) return (ea | sa) ? F_PACK(sign, 0x7ff, 0) : F_DEFNAN;
    norm_sub(&eb, &sb);
  }
  if (!ea) { if (!sa) return F_PACK(sign, 0, 0); norm_sub(&ea, &sa); }
  ez = ea - eb + 0x3fe;
  sa |= 0x0010000000000000ULL;
  sb |= 0x0010000000000000ULL;
  if (sa < sb) { ez--; sa <<= 11; } else sa <<= 10;
  sb <<= 11;
  /* q = sa * 2^63 / sb, bit by bit; sa < sb < 2^64 */
  rem = sa;
  for (i = 62; i >= 0; i--) {
    int carry = (int)(rem >> 63);
    rem <<= 1;
    if (carry || rem >= sb) { rem -= sb; q |= (uint64_t)1 << i; }
  }
  if (rem) q |= 1;
  return round_pack(sign, ez, q);
}

fval f_from_int(ival v)
{
  int sign = v < 0;
  uint64_t m = sign ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
  if (!m) return 0;
  if (!(m << 1)) return F_PACK(sign, 0x43e, 0);   /* -2^63 */
  return norm_round_pack(sign, 0x43c, m);
}

/* truncate; NaN and values outside 64 bits give INT64_MIN, as on x86-64 */
ival f_to_int(fval a)
{
  int e = F_EXP(a), sh;
  uint64_t m;
  if (e < 0x3ff) return 0;
  if (e >= 0x43e) return INT64_MIN;
  m = F_FRAC(a) | 0x0010000000000000ULL;
  sh = e - 0x433;
  m = sh >= 0 ? m << sh : m >> -sh;
  return F_SIGN(a) ? -(ival)m : (ival)m;
}

/* to single precision bits, rounded to nearest even */
uint32_t f_to_single(fval a)
{
  int sign = F_SIGN(a), e = F_EXP(a), rbits;
  uint64_t fr = F_FRAC(a);
  uint32_t sig;
  if (e == 0x7ff) return ((uint32_t)sign << 31) | 0x7f800000 | (fr ? 0x400000 | (uint32_t)(fr >> 29) : 0);
  sig = (uint32_t)shr_jam(fr, 22);
  if (!(e | sig)) return (uint32_t)sign << 31;
  sig |= 0x40000000;
  e -= 0x381;
  rbits = sig & 0x7f;
  if ((unsigned)e >= 0xfd) {
    if (e < 0) {
      sig = (uint32_t)shr_jam(sig, -e);
      e = 0;
      rbits = sig & 0x7f;
    } else if (e > 0xfd || sig + 0x40 >= 0x80000000u) {
      return ((uint32_t)sign << 31) | 0x7f800000;
    }
  }
  sig = (sig + 0x40) >> 7;
  if (rbits == 0x40) sig &= ~1u;
  if (!sig) e = 0;
  return ((uint32_t)sign << 31) + ((uint32_t)e << 23) + sig;
}

fval f_from_single(uint32_t s)
{
  int sign = (int)(s >> 31), e = (int)((s >> 23) & 0xff);
  uint64_t fr = s & 0x7fffff;
  if (e == 0xff) return F_PACK(sign, 0x7ff, fr << 29);
  if (!e) {
    if (!fr) return F_PACK(sign, 0, 0);
    e = 1;
    while (!(fr & 0x800000)) { fr <<= 1; e--; }
    fr &= 0x7fffff;
  }
  return F_PACK(sign, e + 0x380, fr << 29);
}

int f_is_single(fval a) { return f_isnan(a) ? 0 : f_from_single(f_to_single(a)) == a; }

/* a whole number in lo..hi (not -0) */
int f_is_int_in(fval a, ival lo, ival hi)
{
  ival v;
  if (f_isnan(a) || f_isinf(a) || a == 0x8000000000000000ULL) return 0;
  v = f_to_int(a);
  return v >= lo && v <= hi && f_eq(f_from_int(v), a);
}

/* a power of two (positive or negative) */
int f_is_pow2(fval a)
{
  int e = F_EXP(a);
  uint64_t fr = F_FRAC(a);
  if (e == 0x7ff) return 0;
  if (e) return fr == 0;
  return fr && !(fr & (fr - 1));
}

/* ------------------------------------------------- big integers (decimal) */
#define BN_MAX 160              /* 32-bit limbs: enough for 10^800 * 2^1100 */
typedef struct { int n; uint32_t d[BN_MAX]; } bn;

static void bn_set(bn *a, uint64_t v) { a->n = 0; while (v) { a->d[a->n++] = (uint32_t)v; v >>= 32; } }
static void bn_muladd(bn *a, uint32_t m, uint32_t add)
{
  uint64_t c = add;
  int i;
  for (i = 0; i < a->n; i++) { c += (uint64_t)a->d[i] * m; a->d[i] = (uint32_t)c; c >>= 32; }
  if (c && a->n < BN_MAX) a->d[a->n++] = (uint32_t)c;
}
static void bn_shl(bn *a, int s)
{
  int w = s / 32, b = s % 32, i;
  if (!a->n || s <= 0) return;
  if (a->n + w + 1 > BN_MAX) w = BN_MAX - a->n - 1;    /* cannot happen in range */
  a->d[a->n] = 0;
  for (i = a->n; i >= 0; i--) {
    uint32_t v = a->d[i] << b;
    if (b && i) v |= a->d[i - 1] >> (32 - b);
    a->d[i + w] = v;
  }
  for (i = 0; i < w; i++) a->d[i] = 0;
  a->n += w + 1;
  while (a->n && !a->d[a->n - 1]) a->n--;
}
static int bn_bits(const bn *a)
{
  int b;
  if (!a->n) return 0;
  b = 32 * (a->n - 1);
  { uint32_t t = a->d[a->n - 1]; while (t) { b++; t >>= 1; } }
  return b;
}
static int bn_cmp(const bn *a, const bn *b)
{
  int i;
  if (a->n != b->n) return a->n < b->n ? -1 : 1;
  for (i = a->n - 1; i >= 0; i--) if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
  return 0;
}
static void bn_sub(bn *a, const bn *b)       /* a -= b, a >= b */
{
  int64_t c = 0;
  int i;
  for (i = 0; i < a->n; i++) {
    c += (int64_t)a->d[i] - (i < b->n ? b->d[i] : 0);
    a->d[i] = (uint32_t)c;
    c = c < 0 ? -1 : 0;
  }
  while (a->n && !a->d[a->n - 1]) a->n--;
}
static uint32_t bn_divsmall(bn *a, uint32_t d)   /* a /= d, returns the remainder */
{
  uint64_t r = 0;
  int i;
  for (i = a->n - 1; i >= 0; i--) { r = (r << 32) | a->d[i]; a->d[i] = (uint32_t)(r / d); r %= d; }
  while (a->n && !a->d[a->n - 1]) a->n--;
  return (uint32_t)r;
}

/* decimal to double, correctly rounded: digits [. digits] [e [+-] digits]
 * (the caller has checked the syntax start); *end is set past the number */
fval f_from_dec(const char *s, const char **end)
{
  static bn num, den, t;
  int nd = 0, e10 = 0, sticky = 0, sh, i, ex = 0, eneg = 0, edig = 0;
  uint64_t q = 0;
  const char *p = s;
  char dig[801];
  for (; isdigit((unsigned char)*p); p++) {
    if (nd || *p != '0') { if (nd < 800) dig[nd++] = *p; else { e10++; if (*p != '0') sticky = 1; } }
  }
  if (*p == '.' && isdigit((unsigned char)p[1])) {
    for (p++; isdigit((unsigned char)*p); p++) {
      if (!nd && *p == '0') { e10--; continue; }
      if (nd < 800) { dig[nd++] = *p; e10--; } else if (*p != '0') sticky = 1;
    }
  } else if (*p == '.' && !isdigit((unsigned char)p[1]))
    p++;
  if ((*p == 'e' || *p == 'E') &&
      (isdigit((unsigned char)p[1]) || ((p[1] == '+' || p[1] == '-') && isdigit((unsigned char)p[2])))) {
    p++;
    if (*p == '+' || *p == '-') eneg = *p++ == '-';
    for (; isdigit((unsigned char)*p); p++) { if (ex < 100000) ex = ex * 10 + (*p - '0'); edig = 1; }
    (void)edig;
    e10 += eneg ? -ex : ex;
  }
  if (end) *end = p;
  if (sticky && nd < 801) dig[nd++] = '1', e10--;    /* a nonzero tail only breaks ties */
  if (!nd) return 0;
  if (nd + e10 > 310) return F_PACK(0, 0x7ff, 0);
  if (nd + e10 < -330) return 0;
  /* value = digits * 10^e10 = num / den */
  bn_set(&num, 0);
  for (i = 0; i < nd; i++) { if (!num.n) bn_set(&num, (uint64_t)(dig[i] - '0')); else bn_muladd(&num, 10, (uint32_t)(dig[i] - '0')); }
  bn_set(&den, 1);
  for (i = 0; i < (e10 < 0 ? -e10 : e10); i++) bn_muladd(e10 < 0 ? &den : &num, 10, 0);
  /* q = num * 2^sh / den with its leading bit at bit 62 */
  sh = 63 - (bn_bits(&num) - bn_bits(&den));
  for (;;) {
    if (sh > 0) { t = num; bn_shl(&t, sh); } else { t = num; }
    {
      bn d2 = den;
      if (sh < 0) bn_shl(&d2, -sh);
      q = 0;
      for (i = 63; i >= 0; i--) {
        bn di = d2;
        bn_shl(&di, i);
        if (bn_cmp(&t, &di) >= 0) { bn_sub(&t, &di); q |= (uint64_t)1 << i; }
      }
    }
    if (q >= 0x8000000000000000ULL) { sh--; continue; }
    if (q < 0x4000000000000000ULL) { sh++; continue; }
    break;
  }
  if (t.n) q |= 1;
  /* value = q * 2^-sh = q * 2^(exp - 0x3fe - 62) */
  return round_pack(0, 0x3fe + 62 - sh, q);
}

/* the 17 significant decimal digits of a finite, nonzero a, rounded to
 * nearest even (as printf("%.16e")): dig[17] and the decimal exponent of
 * the first digit */
void f_to_dec17(fval a, char *dig, int *exp10)
{
  static bn m;
  char buf[1200];
  int e = F_EXP(a), n = 0, i, scale, r;
  uint64_t fr = F_FRAC(a);
  if (e) fr |= 0x0010000000000000ULL; else e = 1;
  e -= 0x433;                      /* a = fr * 2^e */
  bn_set(&m, fr);
  if (e >= 0) { bn_shl(&m, e); scale = 0; }
  else { for (i = 0; i < -e; i++) bn_muladd(&m, 5, 0); scale = -e; }   /* a = m / 10^scale */
  while (m.n) {
    uint32_t c = bn_divsmall(&m, 1000000000);
    for (i = 0; i < 9 && (m.n || c); i++) { buf[n++] = (char)('0' + c % 10); c /= 10; }
  }
  /* buf holds the digits in reverse; n of them */
  *exp10 = n - 1 - scale;
  for (i = 0; i < 17; i++) dig[i] = i < n ? buf[n - 1 - i] - '0' : 0;
  if (n > 17) {
    int half = buf[n - 18] - '0', rest = 0;
    for (i = 0; i < n - 18; i++) if (buf[i] != '0') { rest = 1; break; }
    r = half > 5 || (half == 5 && (rest || (dig[16] & 1)));
    for (i = 16; r && i >= 0; i--) {
      if (++dig[i] == 10) dig[i] = 0; else r = 0;
    }
    if (r) { dig[0] = 1; for (i = 1; i < 17; i++) dig[i] = 0; ++*exp10; }
  }
}
