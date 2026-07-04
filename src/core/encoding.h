#pragma once
#include "strbuf.h"

int has_non_ascii(const char *s);
int convert_encoding(const char *input, const char *to_charset, StrBuf *out);
int convert_encoding_from(const char *input, const char *from_charset,
                          StrBuf *out);
int convert_locale_to_utf8(const char *input, const char *locale, StrBuf *out);
int is_valid_utf8(const char *s);
