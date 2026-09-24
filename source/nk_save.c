/* nk_save.c -- see nk_save.h.
 *
 * MIT license -- see LICENSE.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nk_save.h"

#define POLY 0xEDB88320u
#define MAX_DEPTH 64

/* ------------------------------------------------------------------ */
/* codec                                                               */
/* ------------------------------------------------------------------ */

uint32_t nk_crc(const unsigned char *d, size_t n) {
  uint32_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    uint32_t x = (d[i] ^ (crc & 0xFF)) & 0xFF;
    x = (x & 1) ? ((x >> 1) ^ POLY) : (x >> 1);            /* round 1: logical */
    for (int r = 0; r < 7; r++) {                           /* rounds 2-8: arithmetic */
      const uint32_t s = (x >> 1) | (x & 0x80000000u);
      x = (x & 1) ? (s ^ POLY) : s;
    }
    crc = x ^ (crc >> 8);
  }
  return crc;
}

long nk_decode(const unsigned char *raw, size_t n, char *out, size_t cap, int *crc_ok) {
  if (n <= NK_HDR || memcmp(raw, "DGDATA", 6) != 0) return -1;
  const size_t len = n - NK_HDR;
  if (len + 1 > cap) return -1;
  char hex[9];
  memcpy(hex, raw + 6, 8);
  hex[8] = 0;
  char *endp = NULL;
  const uint32_t stored = (uint32_t)strtoul(hex, &endp, 16);
  const int hex_ok = endp == hex + 8;
  for (size_t i = 0; i < len; i++)
    out[i] = (char)(unsigned char)(raw[NK_HDR + i] - 21 - (i % 6));
  out[len] = 0;
  if (crc_ok) *crc_ok = hex_ok && nk_crc((const unsigned char *)out, len) == stored;
  return (long)len;
}

long nk_encode(const char *json, size_t n, unsigned char *out, size_t cap) {
  if (NK_HDR + n > cap) return -1;
  char hex[9];
  /* via a temporary: snprintf's NUL would land on the first payload byte */
  snprintf(hex, sizeof hex, "%08x", (unsigned)nk_crc((const unsigned char *)json, n));
  memcpy(out, "DGDATA", 6);
  memcpy(out + 6, hex, 8);
  for (size_t i = 0; i < n; i++)
    out[NK_HDR + i] = (unsigned char)((unsigned char)json[i] + 21 + (i % 6));
  return (long)(NK_HDR + n);
}

/* ------------------------------------------------------------------ */
/* JSON scanning                                                       */
/* ------------------------------------------------------------------ */

static const char *ws(const char *p, const char *e) {
  while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  return p;
}

static const char *skip_str(const char *p, const char *e) {   /* p at '"' */
  for (p++; p < e; p++) {
    if (*p == '\\') { p++; continue; }
    if (*p == '"') return p + 1;
  }
  return NULL;
}

static int is_num_char(char c) {
  return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static const char *skip_val(const char *p, const char *e, int depth) {
  p = ws(p, e);
  if (p >= e || depth > MAX_DEPTH) return NULL;
  switch (*p) {
  case '{':
    p = ws(p + 1, e);
    if (p < e && *p == '}') return p + 1;
    for (;;) {
      p = ws(p, e);
      if (p >= e || *p != '"') return NULL;
      if (!(p = skip_str(p, e))) return NULL;
      p = ws(p, e);
      if (p >= e || *p != ':') return NULL;
      if (!(p = skip_val(p + 1, e, depth + 1))) return NULL;
      p = ws(p, e);
      if (p >= e) return NULL;
      if (*p == ',') { p++; continue; }
      if (*p == '}') return p + 1;
      return NULL;
    }
  case '[':
    p = ws(p + 1, e);
    if (p < e && *p == ']') return p + 1;
    for (;;) {
      if (!(p = skip_val(p, e, depth + 1))) return NULL;
      p = ws(p, e);
      if (p >= e) return NULL;
      if (*p == ',') { p++; continue; }
      if (*p == ']') return p + 1;
      return NULL;
    }
  case '"':
    return skip_str(p, e);
  case 't': return (e - p >= 4 && !memcmp(p, "true", 4))  ? p + 4 : NULL;
  case 'f': return (e - p >= 5 && !memcmp(p, "false", 5)) ? p + 5 : NULL;
  case 'n': return (e - p >= 4 && !memcmp(p, "null", 4))  ? p + 4 : NULL;
  default: {
    const char *s = p;
    while (p < e && is_num_char(*p)) p++;
    return p > s ? p : NULL;
  }
  }
}

int nkj_valid(const char *j, size_t n) {
  const char *e = j + n;
  const char *p = skip_val(j, e, 0);
  return p && ws(p, e) == e;
}

/* Is the string token at p (on its '"') exactly `lit` (len bytes)? Keys and
 * the values matched here are plain identifiers, compared byte for byte. */
static int str_eq(const char *p, const char *e, const char *lit, size_t len) {
  return p < e && *p == '"' && (size_t)(e - p) > len + 1 &&
         !memcmp(p + 1, lit, len) && p[1 + len] == '"';
}

/* In the object at p ('{'), the value of member `key`; NULL if absent. */
static const char *member(const char *p, const char *e, const char *key, size_t klen) {
  p = ws(p, e);
  if (p >= e || *p != '{') return NULL;
  p = ws(p + 1, e);
  if (p < e && *p == '}') return NULL;
  for (;;) {
    p = ws(p, e);
    if (p >= e || *p != '"') return NULL;
    const int hit = str_eq(p, e, key, klen);
    if (!(p = skip_str(p, e))) return NULL;
    p = ws(p, e);
    if (p >= e || *p != ':') return NULL;
    p = ws(p + 1, e);
    if (hit) return p;
    if (!(p = skip_val(p, e, 1))) return NULL;
    p = ws(p, e);
    if (p >= e || *p != ',') return NULL;
    p++;
  }
}

/* In the array at p ('['), element idx; NULL if absent. */
static const char *element(const char *p, const char *e, long idx) {
  p = ws(p, e);
  if (p >= e || *p != '[' || idx < 0) return NULL;
  p = ws(p + 1, e);
  if (p < e && *p == ']') return NULL;
  for (long i = 0;; i++) {
    p = ws(p, e);
    if (i == idx) return p;
    if (!(p = skip_val(p, e, 1))) return NULL;
    p = ws(p, e);
    if (p >= e || *p != ',') return NULL;
    p++;
  }
}

/* In the array at p, the first object whose member k is the string v. */
static const char *element_where(const char *p, const char *e, const char *k, size_t kl,
                                 const char *v, size_t vl) {
  for (long i = 0;; i++) {
    const char *el = element(p, e, i);
    if (!el) return NULL;
    const char *mv = member(el, e, k, kl);
    if (mv && str_eq(mv, e, v, vl)) return el;
  }
}

static const char *resolve(const char *j, size_t n, const char *path) {
  const char *e = j + n;
  const char *cur = ws(j, e);
  const char *s = path;
  while (cur && *s) {
    if (*s == '.') { s++; continue; }
    if (*s == '[') {
      const char *close = strchr(s, ']');
      if (!close) return NULL;
      const char *eq = memchr(s, '=', (size_t)(close - s));
      if (eq) cur = element_where(cur, e, s + 1, (size_t)(eq - s - 1), eq + 1, (size_t)(close - eq - 1));
      else    cur = element(cur, e, strtol(s + 1, NULL, 10));
      s = close + 1;
    } else {
      const char *t = s;
      while (*t && *t != '.' && *t != '[') t++;
      cur = member(cur, e, s, (size_t)(t - s));
      s = t;
    }
  }
  return cur;
}

int nkj_find(const char *j, size_t n, const char *path, size_t *vs, size_t *ve) {
  const char *v = resolve(j, n, path);
  if (!v) return 0;
  const char *end = skip_val(v, j + n, 0);
  if (!end) return 0;
  *vs = (size_t)(v - j);
  *ve = (size_t)(end - j);
  return 1;
}

/* ------------------------------------------------------------------ */
/* editing                                                             */
/* ------------------------------------------------------------------ */

static int replace(char *j, size_t *n, size_t cap, size_t vs, size_t ve, const char *text) {
  const size_t tl = strlen(text), old = ve - vs;
  if (tl == old && !memcmp(j + vs, text, tl)) return 0;          /* already that value */
  if (*n - old + tl + 1 > cap) return 0;
  memmove(j + vs + tl, j + ve, *n - ve + 1);                     /* includes the NUL */
  memcpy(j + vs, text, tl);
  *n = *n - old + tl;
  return 1;
}

static int is_int_token(const char *p, size_t len) {
  size_t i = (len && p[0] == '-') ? 1 : 0;
  if (i == len) return 0;
  for (; i < len; i++) if (p[i] < '0' || p[i] > '9') return 0;
  return 1;
}

int nkj_get_int(const char *j, size_t n, const char *path, long long *out) {
  size_t vs, ve;
  if (!nkj_find(j, n, path, &vs, &ve) || !is_int_token(j + vs, ve - vs) || ve - vs > 20) return 0;
  char num[24];
  memcpy(num, j + vs, ve - vs);
  num[ve - vs] = 0;
  *out = strtoll(num, NULL, 10);
  return 1;
}

int nkj_set_int(char *j, size_t *n, size_t cap, const char *path, long long v) {
  size_t vs, ve;
  if (!nkj_find(j, *n, path, &vs, &ve) || !is_int_token(j + vs, ve - vs)) return 0;
  char num[32];
  snprintf(num, sizeof num, "%lld", v);
  return replace(j, n, cap, vs, ve, num);
}

static int is_bool_token(const char *p, size_t len) {
  return (len == 4 && !memcmp(p, "true", 4)) || (len == 5 && !memcmp(p, "false", 5));
}

int nkj_set_bool(char *j, size_t *n, size_t cap, const char *path, int v) {
  size_t vs, ve;
  if (!nkj_find(j, *n, path, &vs, &ve) || !is_bool_token(j + vs, ve - vs)) return 0;
  return replace(j, n, cap, vs, ve, v ? "true" : "false");
}

int nkj_set_array_bools(char *j, size_t *n, size_t cap, const char *path, int v) {
  const int count = nkj_count(j, *n, path);
  if (count <= 0) return 0;
  size_t vs, ve;
  if (!nkj_find(j, *n, path, &vs, &ve) || j[vs] != '[') return 0;
  int changed = 0;
  char ep[512];
  for (int i = 0; i < count; i++) {                  /* re-resolve: offsets shift */
    snprintf(ep, sizeof ep, "%s[%d]", path, i);
    changed += nkj_set_bool(j, n, cap, ep, v);
  }
  return changed;
}

int nkj_count(const char *j, size_t n, const char *path) {
  const char *e = j + n;
  const char *p = resolve(j, n, path);
  if (!p || (*p != '[' && *p != '{')) return -1;
  const char close = (*p == '[') ? ']' : '}';
  const int obj = *p == '{';
  p = ws(p + 1, e);
  if (p < e && *p == close) return 0;
  int c = 0;
  for (;;) {
    if (obj) {
      if (!(p = skip_str(ws(p, e), e))) return -1;
      p = ws(p, e);
      if (p >= e || *p != ':') return -1;
      p++;
    }
    if (!(p = skip_val(p, e, 1))) return -1;
    c++;
    p = ws(p, e);
    if (p < e && *p == ',') { p++; continue; }
    return (p < e && *p == close) ? c : -1;
  }
}

int nkj_member_name(const char *j, size_t n, const char *path, int idx, char *out, size_t cap) {
  const char *e = j + n;
  const char *p = resolve(j, n, path);
  if (!p || *p != '{' || idx < 0) return 0;
  p = ws(p + 1, e);
  for (int i = 0; p < e && *p == '"'; i++) {
    const char *ks = p + 1;
    if (!(p = skip_str(p, e))) return 0;
    if (i == idx) {
      const size_t kl = (size_t)(p - 1 - ks);
      if (kl + 1 > cap) return 0;
      memcpy(out, ks, kl);
      out[kl] = 0;
      return 1;
    }
    p = ws(p, e);
    if (p >= e || *p != ':') return 0;
    if (!(p = skip_val(p + 1, e, 1))) return 0;
    p = ws(p, e);
    if (p >= e || *p != ',') return 0;
    p = ws(p + 1, e);
  }
  return 0;
}

/* ------------------------------------------------------------------ */
/* string lists                                                        */
/* ------------------------------------------------------------------ */

int nkj_array_has_string(const char *j, size_t n, const char *path, const char *str) {
  const char *e = j + n;
  const char *p = resolve(j, n, path);
  if (!p || *p != '[') return 0;
  const size_t len = strlen(str);
  for (long i = 0;; i++) {
    const char *el = element(p, e, i);
    if (!el) return 0;
    if (str_eq(el, e, str, len)) return 1;
  }
}

static int plain_ident(const char *s) {
  if (!*s) return 0;
  for (; *s; s++)
    if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
          *s == '_' || *s == '-' || *s == '.'))
      return 0;
  return 1;
}

int nkj_array_add_string(char *j, size_t *n, size_t cap, const char *path, const char *str) {
  if (!plain_ident(str) || nkj_array_has_string(j, *n, path, str)) return 0;
  size_t vs, ve;
  if (!nkj_find(j, *n, path, &vs, &ve) || j[vs] != '[' || j[ve - 1] != ']') return 0;
  const char *first = ws(j + vs + 1, j + ve);
  const int empty = *first == ']';
  char item[256];
  const int il = snprintf(item, sizeof item, "%s\"%s\"", empty ? "" : ",", str);
  if (il < 0 || (size_t)il >= sizeof item || *n + (size_t)il + 1 > cap) return 0;
  const size_t at = ve - 1;                       /* just before the closing ']' */
  memmove(j + at + il, j + at, *n - at + 1);      /* includes the NUL */
  memcpy(j + at, item, (size_t)il);
  *n += (size_t)il;
  return 1;
}
