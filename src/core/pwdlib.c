#define _GNU_SOURCE
#include "pwdlib.h"
#include "log.h"
#include "path.h"
#include "strbuf.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

void pwdvec_init(PwdVec *v) {
  v->data = NULL;
  v->len = v->cap = 0;
}
void pwdvec_free(PwdVec *v) {
  for (size_t i = 0; i < v->len; ++i) {
    free(v->data[i].id);
    free(v->data[i].value);
    free(v->data[i].desc);
  }
  free(v->data);
  v->data = NULL;
  v->len = v->cap = 0;
}
int pwdvec_push(PwdVec *v, PwdItem it) {
  if (v->len == v->cap) {
    size_t new_cap = v->cap ? v->cap * 2 : 32;
    PwdItem *tmp = (PwdItem *)realloc(v->data, new_cap * sizeof(PwdItem));
    if (!tmp)
      return 0;
    v->data = tmp;
    v->cap = new_cap;
  }
  v->data[v->len++] = it;
  return 1;
}

/* -------- JSON parser -------- */
typedef struct {
  const char *s;
  size_t i;
} JsonCursor;

static void json_skip_ws(JsonCursor *c) {
  while (c->s[c->i] && isspace((unsigned char)c->s[c->i]))
    c->i++;
}
static int json_consume(JsonCursor *c, char ch) {
  json_skip_ws(c);
  if (c->s[c->i] == ch) {
    c->i++;
    return 1;
  }
  return 0;
}

static void json_append_utf8(StrBuf *out, unsigned cp) {
  #define APPEND_UTF8_BYTE(v) sb_append_c(out, (char)(unsigned char)(v))
  if (cp <= 0x7F)
    APPEND_UTF8_BYTE(cp);
  else if (cp <= 0x7FF) {
    APPEND_UTF8_BYTE(0xC0u | (cp >> 6));
    APPEND_UTF8_BYTE(0x80u | (cp & 0x3Fu));
  } else if (cp <= 0xFFFF) {
    APPEND_UTF8_BYTE(0xE0u | (cp >> 12));
    APPEND_UTF8_BYTE(0x80u | ((cp >> 6) & 0x3Fu));
    APPEND_UTF8_BYTE(0x80u | (cp & 0x3Fu));
  } else {
    APPEND_UTF8_BYTE(0xF0u | (cp >> 18));
    APPEND_UTF8_BYTE(0x80u | ((cp >> 12) & 0x3Fu));
    APPEND_UTF8_BYTE(0x80u | ((cp >> 6) & 0x3Fu));
    APPEND_UTF8_BYTE(0x80u | (cp & 0x3Fu));
  }
  #undef APPEND_UTF8_BYTE
}

static int json_parse_string(JsonCursor *c, StrBuf *out, char **err) {
  json_skip_ws(c);
  if (c->s[c->i] != '"') {
    if (err)
      *err = "expected string";
    return 0;
  }
  c->i++;
  out->len = 0;
  while (c->s[c->i]) {
    char ch = c->s[c->i++];
    if (ch == '"')
      return 1;
    if (ch == '\\') {
      if (!c->s[c->i]) {
        if (err)
          *err = "unterminated escape";
        return 0;
      }
      char esc = c->s[c->i++];
      switch (esc) {
      case '"':
      case '\\':
      case '/':
        sb_append_c(out, esc);
        break;
      case 'b':
        sb_append_c(out, '\b');
        break;
      case 'f':
        sb_append_c(out, '\f');
        break;
      case 'n':
        sb_append_c(out, '\n');
        break;
      case 'r':
        sb_append_c(out, '\r');
        break;
      case 't':
        sb_append_c(out, '\t');
        break;
      case 'u': {
        if (!c->s[c->i] || !c->s[c->i + 1] || !c->s[c->i + 2] ||
            !c->s[c->i + 3]) {
          if (err)
            *err = "truncated unicode escape";
          return 0;
        }
        int h1 = c->s[c->i++], h2 = c->s[c->i++], h3 = c->s[c->i++],
            h4 = c->s[c->i++];
        int val[4] = {0, 0, 0, 0};
        int ok = 1;
        int hs[4] = {h1, h2, h3, h4};
        for (int k = 0; k < 4; k++) {
          int h = hs[k];
          if (h >= '0' && h <= '9')
            val[k] = h - '0';
          else if (h >= 'a' && h <= 'f')
            val[k] = 10 + (h - 'a');
          else if (h >= 'A' && h <= 'F')
            val[k] = 10 + (h - 'A');
          else
            ok = 0;
        }
        if (!ok) {
          if (err)
            *err = "invalid unicode escape";
          return 0;
        }
        int hex = (val[0] << 12) | (val[1] << 8) | (val[2] << 4) | val[3];
        if (hex >= 0xD800 && hex <= 0xDBFF) {
          if (c->s[c->i] == '\\' && c->s[c->i + 1] == 'u') {
            c->i += 2;
            if (!c->s[c->i] || !c->s[c->i + 1] || !c->s[c->i + 2] ||
                !c->s[c->i + 3]) {
              if (err)
                *err = "truncated unicode escape";
              return 0;
            }
            int l1 = c->s[c->i++], l2 = c->s[c->i++], l3 = c->s[c->i++],
                l4 = c->s[c->i++];
            int lval[4] = {0, 0, 0, 0};
            ok = 1;
            int hs2[4] = {l1, l2, l3, l4};
            for (int k = 0; k < 4; k++) {
              int h = hs2[k];
              if (h >= '0' && h <= '9')
                lval[k] = h - '0';
              else if (h >= 'a' && h <= 'f')
                lval[k] = 10 + (h - 'a');
              else if (h >= 'A' && h <= 'F')
                lval[k] = 10 + (h - 'A');
              else
                ok = 0;
            }
            if (!ok) {
              if (err)
                *err = "invalid unicode escape";
              return 0;
            }
            unsigned low = ((unsigned)lval[0] << 12) |
                           ((unsigned)lval[1] << 8) |
                           ((unsigned)lval[2] << 4) | (unsigned)lval[3];
            if (low >= 0xDC00 && low <= 0xDFFF) {
              unsigned full = 0x10000u +
                              ((((unsigned)hex - 0xD800u) << 10) |
                               (low - 0xDC00u));
              json_append_utf8(out, full);
              break;
            }
            if (err)
              *err = "invalid surrogate pair";
            return 0;
          }
          if (err)
            *err = "missing surrogate pair";
          return 0;
        }
        json_append_utf8(out, (unsigned)hex);
        break;
      }
      default:
        if (err)
          *err = "unknown escape";
        return 0;
      }
      continue;
    }
    if ((unsigned char)ch < 0x20) {
      if (err)
        *err = "control char in string";
      return 0;
    }
    sb_append_c(out, ch);
  }
  if (err)
    *err = "unterminated string";
  return 0;
}

static int json_skip_value(JsonCursor *c, char **err);

static int json_parse_pwd_object(JsonCursor *c, PwdItem *it, char **err) {
  if (!json_consume(c, '{')) {
    if (err)
      *err = "expected '{'";
    return 0;
  }
  json_skip_ws(c);
  if (json_consume(c, '}'))
    return 1;
  StrBuf key;
  sb_init(&key);
  StrBuf val;
  sb_init(&val);

  while (1) {
    if (!json_parse_string(c, &key, err))
      goto fail;
    if (!json_consume(c, ':')) {
      if (err)
        *err = "expected ':'";
      goto fail;
    }
    if (strcmp(key.data, "id") == 0) {
      if (!json_parse_string(c, &val, err))
        goto fail;
      it->id = str_dup(val.data);
    } else if (strcmp(key.data, "value") == 0) {
      if (!json_parse_string(c, &val, err))
        goto fail;
      it->value = str_dup(val.data);
    } else if (strcmp(key.data, "desc") == 0) {
      json_skip_ws(c);
      if (c->s[c->i] == '"') {
        if (!json_parse_string(c, &val, err))
          goto fail;
        it->desc = str_dup(val.data);
      } else if (strncmp(c->s + c->i, "null", 4) == 0) {
        c->i += 4;
      } else {
        if (err)
          *err = "expected string or null";
        goto fail;
      }
    } else if (strcmp(key.data, "hit_count") == 0) {
      json_skip_ws(c);
      int neg = 0;
      if (c->s[c->i] == '-') { neg = 1; c->i++; }
      int n = 0;
      while (isdigit((unsigned char)c->s[c->i]))
        n = n * 10 + (c->s[c->i++] - '0');
      it->hit_count = neg ? -n : n;
    } else {
      if (!json_skip_value(c, err))
        goto fail;
    }
    if (json_consume(c, '}'))
      break;
    if (!json_consume(c, ',')) {
      if (err)
        *err = "expected ',' or '}'";
      goto fail;
    }
  }
  sb_free(&key);
  sb_free(&val);
  return 1;
fail:
  sb_free(&key);
  sb_free(&val);
  return 0;
}

static int json_skip_number(JsonCursor *c) {
  json_skip_ws(c);
  size_t start = c->i;
  if (c->s[c->i] == '-')
    c->i++;
  while (isdigit((unsigned char)c->s[c->i]))
    c->i++;
  if (c->s[c->i] == '.') {
    c->i++;
    while (isdigit((unsigned char)c->s[c->i]))
      c->i++;
  }
  if (c->s[c->i] == 'e' || c->s[c->i] == 'E') {
    c->i++;
    if (c->s[c->i] == '-' || c->s[c->i] == '+')
      c->i++;
    while (isdigit((unsigned char)c->s[c->i]))
      c->i++;
  }
  return c->i > start;
}

static int json_skip_value(JsonCursor *c, char **err) {
  json_skip_ws(c);
  char ch = c->s[c->i];
  if (!ch) {
    if (err)
      *err = "unexpected end";
    return 0;
  }
  if (ch == '"') {
    StrBuf tmp;
    sb_init(&tmp);
    int ok = json_parse_string(c, &tmp, err);
    sb_free(&tmp);
    return ok;
  }
  if (ch == '{') {
    c->i++;
    json_skip_ws(c);
    if (json_consume(c, '}'))
      return 1;
    while (1) {
      StrBuf key;
      sb_init(&key);
      if (!json_parse_string(c, &key, err)) {
        sb_free(&key);
        return 0;
      }
      sb_free(&key);
      if (!json_consume(c, ':')) {
        if (err)
          *err = "expected ':'";
        return 0;
      }
      if (!json_skip_value(c, err))
        return 0;
      if (json_consume(c, '}'))
        break;
      if (!json_consume(c, ',')) {
        if (err)
          *err = "expected ',' or '}'";
        return 0;
      }
    }
    return 1;
  }
  if (ch == '[') {
    c->i++;
    json_skip_ws(c);
    if (json_consume(c, ']'))
      return 1;
    while (1) {
      if (!json_skip_value(c, err))
        return 0;
      if (json_consume(c, ']'))
        break;
      if (!json_consume(c, ',')) {
        if (err)
          *err = "expected ',' or ']'";
        return 0;
      }
    }
    return 1;
  }
  if (!strncmp(c->s + c->i, "true", 4)) {
    c->i += 4;
    return 1;
  }
  if (!strncmp(c->s + c->i, "false", 5)) {
    c->i += 5;
    return 1;
  }
  if (!strncmp(c->s + c->i, "null", 4)) {
    c->i += 4;
    return 1;
  }
  if (json_skip_number(c))
    return 1;
  if (err)
    *err = "invalid value";
  return 0;
}

static int json_parse_passwords_array(JsonCursor *c, PwdVec *v, char **err) {
  if (!json_consume(c, '[')) {
    if (err)
      *err = "expected '['";
    return 0;
  }
  json_skip_ws(c);
  if (json_consume(c, ']'))
    return 1;
  while (1) {
    PwdItem it = {0};
    if (!json_parse_pwd_object(c, &it, err))
      return 0;
    if (it.value && *it.value) {
      if (!pwdvec_push(v, it)) {
        if (err)
          *err = "out of memory";
        free(it.id);
        free(it.value);
        free(it.desc);
        return 0;
      }
    } else {
      free(it.id);
      free(it.value);
      free(it.desc);
    }
    if (json_consume(c, ']'))
      break;
    if (!json_consume(c, ',')) {
      if (err)
        *err = "expected ',' or ']'";
      return 0;
    }
  }
  return 1;
}

static int json_parse_password_lib(const char *text, PwdVec *v, char **err) {
  JsonCursor c = {text, 0};
  if (!json_consume(&c, '{')) {
    if (err)
      *err = "expected '{' at root";
    return 0;
  }
  json_skip_ws(&c);
  if (json_consume(&c, '}'))
    return 1;
  StrBuf key;
  sb_init(&key);
  while (1) {
    if (!json_parse_string(&c, &key, err)) {
      sb_free(&key);
      return 0;
    }
    if (!json_consume(&c, ':')) {
      if (err)
        *err = "expected ':'";
      sb_free(&key);
      return 0;
    }
    if (!strcmp(key.data, "passwords")) {
      if (!json_parse_passwords_array(&c, v, err)) {
        sb_free(&key);
        return 0;
      }
    } else {
      if (!json_skip_value(&c, err)) {
        sb_free(&key);
        return 0;
      }
    }
    if (json_consume(&c, '}'))
      break;
    if (!json_consume(&c, ',')) {
      if (err)
        *err = "expected ',' or '}'";
      sb_free(&key);
      return 0;
    }
  }
  sb_free(&key);
  return 1;
}

char *password_lib_path(void) {
  const char *home = getenv("HOME");
  if (!home || !*home)
    return NULL;

  StrBuf sb;
  sb_init(&sb);
  sb_append(&sb, home, strlen(home));
  sb_append(&sb, "/password/passwords.json",
            sizeof("/password/passwords.json") - 1);
  return sb.data;
}

static char *password_lib_migration_path(void) {
  const char *home = getenv("HOME");
  if (!home || !*home)
    return NULL;

  StrBuf sb;
  sb_init(&sb);
  sb_append(&sb, home, strlen(home));
  sb_append(&sb, "/.local/share/nautilus-toolkit/passwords.json",
            sizeof("/.local/share/nautilus-toolkit/passwords.json") - 1);
  return sb.data;
}

static int password_value_exists(const PwdVec *v, const char *value) {
  if (!v || !value || !*value)
    return 0;
  for (size_t i = 0; i < v->len; i++) {
    if (v->data[i].value && strcmp(v->data[i].value, value) == 0)
      return 1;
  }
  return 0;
}

static int password_id_exists(const PwdVec *v, const char *id) {
  if (!v || !id || !*id)
    return 0;
  for (size_t i = 0; i < v->len; i++) {
    if (v->data[i].id && strcmp(v->data[i].id, id) == 0)
      return 1;
  }
  return 0;
}

static char *make_unique_password_id(const PwdVec *v, const char *base_id) {
  if (base_id && *base_id && !password_id_exists(v, base_id))
    return str_dup(base_id);

  const char *seed = (base_id && *base_id) ? base_id : "migrated";
  for (int i = 1; i < 10000; i++) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s-%d", seed, i);
    if (!password_id_exists(v, buf))
      return str_dup(buf);
  }
  return NULL;
}

static int merge_password_lib(PwdVec *dst, const PwdVec *src) {
  if (!dst || !src)
    return 0;
  int added = 0;
  for (size_t i = 0; i < src->len; i++) {
    const PwdItem *it = &src->data[i];
    if (!it->value || !*it->value || password_value_exists(dst, it->value))
      continue;

    PwdItem copy = {0};
    copy.id = make_unique_password_id(dst, it->id);
    copy.value = str_dup(it->value);
    copy.desc = it->desc ? str_dup(it->desc) : NULL;
    copy.hit_count = it->hit_count;
    if (!copy.id || !copy.value || (it->desc && !copy.desc) ||
        !pwdvec_push(dst, copy)) {
      free(copy.id);
      free(copy.value);
      free(copy.desc);
      continue;
    }
    added++;
  }
  return added;
}

static int read_password_lib_file(const char *path, PwdVec *v, int log_missing) {
  if (!path || !v)
    return 0;
  FILE *f = fopen(path, "r");
  if (!f) {
    if (log_missing)
      log_msg("Password lib not found: %s", path);
    return 0;
  }

  fchmod(fileno(f), 0600);
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return 0;
  }
  long n = ftell(f);
  if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return 0;
  }
  if (n == 0) {
    fclose(f);
    return 1;
  }
  if ((unsigned long)n > (unsigned long)((size_t)-1 - 1)) {
    fclose(f);
    return 0;
  }

  size_t nbytes = (size_t)n;
  char *buf = (char *)malloc(nbytes + 1);
  if (!buf) {
    fclose(f);
    return 0;
  }
  size_t got = fread(buf, 1, nbytes, f);
  fclose(f);
  if (got != nbytes) {
    free(buf);
    return 0;
  }
  buf[nbytes] = 0;

  char *err = NULL;
  int ok = json_parse_password_lib(buf, v, &err);
  if (!ok) {
    log_msg("Password lib parse error: %s (%s)", path, err ? err : "unknown");
    pwdvec_free(v);
    pwdvec_init(v);
  }
  free(buf);
  return ok ? 1 : 0;
}

static char *json_escape(const char *s) {
  StrBuf out;
  sb_init(&out);
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    unsigned char ch = *p;
    switch (ch) {
    case '\\':
      sb_append(&out, "\\\\", 2);
      break;
    case '"':
      sb_append(&out, "\\\"", 2);
      break;
    case '\b':
      sb_append(&out, "\\b", 2);
      break;
    case '\f':
      sb_append(&out, "\\f", 2);
      break;
    case '\n':
      sb_append(&out, "\\n", 2);
      break;
    case '\r':
      sb_append(&out, "\\r", 2);
      break;
    case '\t':
      sb_append(&out, "\\t", 2);
      break;
    default:
      if (ch < 0x20) {
        char buf[16];
        snprintf(buf, sizeof(buf), "\\u%04x", ch);
        sb_append(&out, buf, strlen(buf));
      } else
        sb_append_c(&out, (char)ch);
    }
  }
  return out.data;
}

void load_password_lib(PwdVec *v) {
  char *primary = password_lib_path();
  char *migration = password_lib_migration_path();
  if (!primary)
    return;
  int primary_loaded = read_password_lib_file(primary, v, 1);
  int merged = 0;

  if (migration && strcmp(primary, migration) != 0) {
    PwdVec old_lib;
    pwdvec_init(&old_lib);
    if (read_password_lib_file(migration, &old_lib, 0)) {
      merged = merge_password_lib(v, &old_lib);
      if (!primary_loaded || merged > 0) {
        if (save_password_lib(v))
          log_msg("Merged password lib into %s", primary);
      }
    }
    pwdvec_free(&old_lib);
  }

  free(migration);
  free(primary);
}

int save_password_lib(PwdVec *v) {
  char *p = password_lib_path();
  if (!p)
    return 0;
  char *dir = path_parent(p);
  if (dir) {
    mkdirs(dir);
    (void)chmod(dir, 0700);
    free(dir);
  }

  size_t p_len = strlen(p);
  char *tmp = (char *)malloc(p_len + sizeof(".tmp.XXXXXX"));
  if (!tmp) {
    free(p);
    return 0;
  }
  snprintf(tmp, p_len + sizeof(".tmp.XXXXXX"), "%s.tmp.XXXXXX", p);

  int fd = mkstemp(tmp);
  if (fd < 0) {
    log_msg("Failed to create temp password lib: %s", p);
    free(tmp);
    free(p);
    return 0;
  }
  fchmod(fd, 0600);
  FILE *f = fdopen(fd, "w");
  if (!f) {
    close(fd);
    unlink(tmp);
    log_msg("Failed to write temp password lib: %s", p);
    free(tmp);
    free(p);
    return 0;
  }

  int ok = 1;
  if (fprintf(f, "{\n  \"version\": 1,\n  \"passwords\": [\n") < 0)
    ok = 0;
  for (size_t i = 0; i < v->len; i++) {
    char *id = json_escape(v->data[i].id ? v->data[i].id : "");
    char *val = json_escape(v->data[i].value ? v->data[i].value : "");
    if (!id || !val) {
      free(id);
      free(val);
      ok = 0;
      break;
    }
    if (fprintf(f, "    { \"id\": \"%s\", \"value\": \"%s\"", id, val) < 0)
      ok = 0;
    if (v->data[i].desc && *v->data[i].desc) {
      char *desc = json_escape(v->data[i].desc);
      if (!desc || fprintf(f, ", \"desc\": \"%s\"", desc) < 0)
        ok = 0;
      free(desc);
    }
    if (v->data[i].hit_count > 0 &&
        fprintf(f, ", \"hit_count\": %d", v->data[i].hit_count) < 0)
      ok = 0;
    if (fprintf(f, " }%s\n", (i + 1 < v->len) ? "," : "") < 0)
      ok = 0;
    free(id);
    free(val);
    if (!ok)
      break;
  }
  if (ok && fprintf(f, "  ]\n}\n") < 0)
    ok = 0;
  if (ok && fflush(f) != 0)
    ok = 0;
  if (ok && fsync(fd) != 0)
    ok = 0;
  if (fclose(f) != 0)
    ok = 0;

  if (ok && rename(tmp, p) != 0)
    ok = 0;
  if (!ok) {
    int saved_errno = errno;
    unlink(tmp);
    log_msg("Failed to atomically save password lib: %s (%s)", p,
            strerror(saved_errno));
  }

  free(tmp);
  free(p);
  return ok;
}

static char *make_auto_pwd_id(PwdVec *v) {
  time_t t = time(NULL);
  struct tm *tm = localtime(&t);
  char base[64];
  strftime(base, sizeof(base), "manual-%Y%m%d-%H%M%S", tm);
  char *id = strdup(base);
  int i = 1;
  int exists = 1;
  while (exists) {
    exists = 0;
    for (size_t k = 0; k < v->len; k++) {
      if (!strcmp(v->data[k].id, id)) {
        exists = 1;
        break;
      }
    }
    if (exists) {
      free(id);
      char tmp[80];
      snprintf(tmp, sizeof(tmp), "%s-%d", base, i++);
      id = strdup(tmp);
    }
  }
  return id;
}

int add_password_to_lib(PwdVec *v, const char *password) {
  if (!password || !*password)
    return 0;
  for (size_t i = 0; i < v->len; i++)
    if (!strcmp(v->data[i].value, password))
      return 0;
  PwdItem it = {0};
  it.id = make_auto_pwd_id(v);
  it.value = strdup(password);
  it.desc = strdup("手动保存");
  if (!pwdvec_push(v, it)) {
    free(it.id);
    free(it.value);
    free(it.desc);
    return 0;
  }
  return 1;
}

void bump_password_hit(PwdVec *v, const char *password) {
  if (!v || !password || !*password)
    return;
  for (size_t i = 0; i < v->len; i++) {
    if (v->data[i].value && strcmp(v->data[i].value, password) == 0) {
      v->data[i].hit_count++;
      return;
    }
  }
}
