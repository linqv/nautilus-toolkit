#pragma once
#include <stddef.h>

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} StrBuf;

void sb_init(StrBuf *sb);
int sb_reserve(StrBuf *sb, size_t n);
int sb_append(StrBuf *sb, const char *s, size_t n);
int sb_append_c(StrBuf *sb, char c);
void sb_free(StrBuf *sb);

char *str_dup(const char *s);

char *strip_newlines(const char *s);
char *shell_quote(const char *s);
char *escape_for_double_quotes(const char *s);
