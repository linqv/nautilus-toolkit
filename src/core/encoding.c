#include "encoding.h"
#include <glib.h>
#include <iconv.h>
#include <string.h>

int has_non_ascii(const char *s) {
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
    if (*p >= 0x80)
      return 1;
  return 0;
}

/* ── iconv descriptor cache ── */
typedef struct {
  const char *charset; /* static string pointer, not owned */
  iconv_t cd;
} IconvCacheEntry;

#define ICONV_CACHE_CAP 8
static _Thread_local IconvCacheEntry iconv_cache[ICONV_CACHE_CAP];
static _Thread_local int iconv_cache_len = 0;

static iconv_t iconv_cached_open(const char *to_charset) {
  for (int i = 0; i < iconv_cache_len; i++) {
    if (strcmp(iconv_cache[i].charset, to_charset) == 0)
      return iconv_cache[i].cd;
  }
  iconv_t cd = iconv_open(to_charset, "UTF-8");
  if (cd == (iconv_t)-1)
    return (iconv_t)-1;
  if (iconv_cache_len < ICONV_CACHE_CAP) {
    iconv_cache[iconv_cache_len].charset = to_charset;
    iconv_cache[iconv_cache_len].cd = cd;
    iconv_cache_len++;
  } else {
    /* Cache full – evict the oldest entry to avoid leaking the descriptor. */
    iconv_close(iconv_cache[0].cd);
    for (int i = 1; i < ICONV_CACHE_CAP; i++)
      iconv_cache[i - 1] = iconv_cache[i];
    iconv_cache[ICONV_CACHE_CAP - 1].charset = to_charset;
    iconv_cache[ICONV_CACHE_CAP - 1].cd = cd;
  }
  return cd;
}

int convert_encoding(const char *input, const char *to_charset, StrBuf *out) {
  iconv_t cd = iconv_cached_open(to_charset);
  if (cd == (iconv_t)-1)
    return 0;
  out->len = 0;
  size_t in_left = strlen(input);
  size_t out_left = in_left * 4 + 16;
  if (!sb_reserve(out, out_left + 1))
    return 0;
  char *in_buf = (char *)input;
  char *out_buf = out->data;
  size_t res = iconv(cd, &in_buf, &in_left, &out_buf, &out_left);
  /* Reset descriptor state for next use. */
  iconv(cd, NULL, NULL, NULL, NULL);
  if (res == (size_t)-1)
    return 0;
  out->len = (size_t)(out_buf - out->data);
  out->data[out->len] = 0;
  return 1;
}

int convert_encoding_from(const char *input, const char *from_charset,
                          StrBuf *out) {
  if (!input || !from_charset || !*from_charset || !out)
    return 0;
  iconv_t cd = iconv_open("UTF-8", from_charset);
  if (cd == (iconv_t)-1)
    return 0;

  out->len = 0;
  size_t in_left = strlen(input);
  size_t out_left = in_left * 4 + 16;
  if (!sb_reserve(out, out_left + 1)) {
    iconv_close(cd);
    return 0;
  }

  char *in_buf = (char *)input;
  char *out_buf = out->data;
  size_t res = iconv(cd, &in_buf, &in_left, &out_buf, &out_left);
  iconv_close(cd);
  if (res == (size_t)-1)
    return 0;

  out->len = (size_t)(out_buf - out->data);
  out->data[out->len] = 0;
  return 1;
}

int convert_locale_to_utf8(const char *input, const char *locale, StrBuf *out) {
  if (!input || !locale || !*locale || !out)
    return 0;
  const char *dot = strchr(locale, '.');
  const char *charset = dot ? dot + 1 : locale;
  if (!*charset)
    return 0;

  size_t len = strcspn(charset, "@");
  if (len == 0)
    return 0;
  if (len == strlen("UTF-8") && strncmp(charset, "UTF-8", len) == 0)
    return 0;
  if (len == strlen("utf8") && strncmp(charset, "utf8", len) == 0)
    return 0;

  char buf[64];
  if (len >= sizeof(buf))
    return 0;
  memcpy(buf, charset, len);
  buf[len] = 0;
  return convert_encoding_from(input, buf, out);
}

int is_valid_utf8(const char *s) {
  return s && g_utf8_validate(s, -1, NULL);
}
