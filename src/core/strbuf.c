#include "strbuf.h"
#include <stdlib.h>
#include <string.h>

void sb_init(StrBuf *sb) {
  sb->data = NULL;
  sb->len = 0;
  sb->cap = 0;
}

int sb_reserve(StrBuf *sb, size_t n) {
  if (sb->cap >= n)
    return 1;
  size_t newcap = sb->cap ? sb->cap * 2 : 256;
  if (newcap < sb->cap || newcap < n)
    newcap = n;
  if (newcap < n)
    return 0;
  char *tmp = (char *)realloc(sb->data, newcap);
  if (!tmp)
    return 0;
  sb->data = tmp;
  sb->cap = newcap;
  return 1;
}

int sb_append(StrBuf *sb, const char *s, size_t n) {
  if (!s || n == 0)
    return 1;
  if (n > (size_t)-1 - sb->len - 1)
    return 0;
  if (!sb_reserve(sb, sb->len + n + 1))
    return 0;
  memcpy(sb->data + sb->len, s, n);
  sb->len += n;
  sb->data[sb->len] = 0;
  return 1;
}

int sb_append_c(StrBuf *sb, char c) {
  if (sb->len > (size_t)-1 - 2)
    return 0;
  if (!sb_reserve(sb, sb->len + 2))
    return 0;
  sb->data[sb->len++] = c;
  sb->data[sb->len] = 0;
  return 1;
}

void sb_free(StrBuf *sb) {
  free(sb->data);
  sb->data = NULL;
  sb->len = sb->cap = 0;
}

char *str_dup(const char *s) { return s ? strdup(s) : NULL; }

char *strip_newlines(const char *s) {
  StrBuf out;
  sb_init(&out);
  const char *run = s;
  for (const char *p = s; *p; ++p) {
    if (*p == '\n' || *p == '\r') {
      if (p > run)
        sb_append(&out, run, (size_t)(p - run));
      run = p + 1;
    }
  }
  if (*run)
    sb_append(&out, run, strlen(run));
  if (!out.data)
    return strdup("");
  return out.data;
}

char *shell_quote(const char *s) {
  StrBuf out;
  sb_init(&out);
  sb_append_c(&out, '\'');
  const char *run = s;
  for (const char *p = s; *p; ++p) {
    if (*p == '\'') {
      if (p > run)
        sb_append(&out, run, (size_t)(p - run));
      sb_append(&out, "'\\''", 4);
      run = p + 1;
    }
  }
  if (*run)
    sb_append(&out, run, strlen(run));
  sb_append_c(&out, '\'');
  return out.data;
}

char *escape_for_double_quotes(const char *s) {
  StrBuf out;
  sb_init(&out);
  const char *run = s;
  for (const char *p = s; *p; ++p) {
    if (*p == '\\' || *p == '"') {
      if (p > run)
        sb_append(&out, run, (size_t)(p - run));
      sb_append_c(&out, '\\');
      sb_append_c(&out, *p);
      run = p + 1;
    }
  }
  if (*run)
    sb_append(&out, run, strlen(run));
  return out.data;
}
