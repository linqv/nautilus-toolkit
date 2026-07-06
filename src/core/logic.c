#define _GNU_SOURCE
#include "encoding.h"
#include "exec7z.h"
#include "extract_util.h"
#include "fsutil.h"
#include "log.h"
#include "path.h"
#include "polyglot_zip_extract.h"
#include "pwdlib.h"
#include "strbuf.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>

/* =============== 密码变体 =============== */
void varvec_init(PwdVarVec *v) {
  v->data = NULL;
  v->len = v->cap = 0;
}
void varvec_free(PwdVarVec *v) {
  for (size_t i = 0; i < v->len; i++) {
    free(v->data[i].bytes);
    free(v->data[i].locale);
  }
  free(v->data);
  v->data = NULL;
  v->len = v->cap = 0;
}
static void varvec_push(PwdVarVec *v, const char *bytes, const char *locale,
                        int transcoded) {
  if (v->len == v->cap) {
    size_t new_cap = v->cap ? v->cap * 2 : 8;
    PwdVariant *tmp =
        (PwdVariant *)realloc(v->data, new_cap * sizeof(PwdVariant));
    if (!tmp)
      return;
    v->data = tmp;
    v->cap = new_cap;
  }
  v->data[v->len].bytes = str_dup(bytes);
  v->data[v->len].locale = str_dup(locale);
  v->data[v->len].transcoded = transcoded;
  if (v->data[v->len].bytes && v->data[v->len].locale)
    v->len++;
  else {
    free(v->data[v->len].bytes);
    free(v->data[v->len].locale);
    v->data[v->len].bytes = NULL;
    v->data[v->len].locale = NULL;
  }
}

void build_password_variants(const char *utf8_pwd, PwdVarVec *out) {
  varvec_push(out, utf8_pwd, "", 0);
  if (!has_non_ascii(utf8_pwd))
    return;
  StrBuf conv;
  sb_init(&conv);
  char *gb18030_bytes = NULL;
  if (convert_encoding(utf8_pwd, "GB18030", &conv)) {
    varvec_push(out, conv.data, "zh_CN.GB18030", 1);
    gb18030_bytes = str_dup(conv.data);
  }
  if (convert_encoding(utf8_pwd, "GBK", &conv)) {
    /* Skip GBK variant if it produces identical bytes to GB18030,
       which is the common case for most Chinese characters. */
    if (!gb18030_bytes || strcmp(conv.data, gb18030_bytes) != 0)
      varvec_push(out, conv.data, "zh_CN.GBK", 1);
  }
  free(gb18030_bytes);
  sb_free(&conv);
}

/* =============== 依赖检查 =============== */
void ignore_sigpipe_once(void) {
  static int done = 0;
  if (!done) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_IGN;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPIPE, &sa, NULL);
    done = 1;
  }
}

int dependency_in_path(const char *dep) {
  const char *path_env = getenv("PATH");
  if (!path_env || !*path_env)
    return 0;
  char *copy = str_dup(path_env);
  if (!copy)
    return 0;

  int found = 0;
  char *saveptr = NULL;
  char *part = strtok_r(copy, ":", &saveptr);
  while (part) {
    const char *dir = (*part) ? part : ".";
    StrBuf full;
    sb_init(&full);
    sb_append(&full, dir, strlen(dir));
    sb_append_c(&full, '/');
    sb_append(&full, dep, strlen(dep));
    if (full.data && access(full.data, X_OK) == 0) {
      found = 1;
      sb_free(&full);
      break;
    }
    sb_free(&full);
    part = strtok_r(NULL, ":", &saveptr);
  }
  free(copy);
  return found;
}

/* =============== 输出目录判定 =============== */
static char *next_available_output_dir(char *candidate, const char *base_dir) {
  if (!candidate)
    return NULL;
  if (!strcmp(candidate, base_dir))
    return candidate;
  if (!path_exists(candidate))
    return candidate;

  const char *name = strrchr(candidate, '/');
  name = name ? name + 1 : candidate;
  size_t name_len = strlen(name);
  DIR *dir = opendir(base_dir);
  if (dir && name_len > 0) {
    /* Scan directory to find the max used suffix number, then pick max+1.
       This avoids allocating a large bitmap on the stack. */
    int max_used = 0;
    struct dirent *de = NULL;
    while ((de = readdir(dir)) != NULL) {
      const char *dname = de->d_name;
      if (!strncmp(dname, name, name_len) && dname[name_len] == '_') {
        const char *digits = dname + name_len + 1;
        if (!*digits)
          continue;
        errno = 0;
        char *endptr = NULL;
        long n = strtol(digits, &endptr, 10);
        if (errno == 0 && endptr && *endptr == '\0' && n >= 1 &&
            n <= 10000) {
          if ((int)n > max_used)
            max_used = (int)n;
        }
      }
    }
    closedir(dir);

    for (int i = max_used + 1; i <= 10000; i++) {
      char buf[4096];
      snprintf(buf, sizeof(buf), "%s_%d", candidate, i);
      if (!path_exists(buf)) {
        free(candidate);
        return str_dup(buf);
      }
    }
  } else if (dir) {
    closedir(dir);
  }

  for (int i = 1; i <= 10000; i++) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s_%d", candidate, i);
    if (!path_exists(buf)) {
      free(candidate);
      return str_dup(buf);
    }
  }
  free(candidate);
  return NULL;
}

/* Parse one `7z l -ba` line and return the in-place path field. */
static int parse_7z_list_ba_path(char *line, char **out_path) {
  if (!line || !out_path)
    return 0;
  *out_path = NULL;
  if (!isdigit((unsigned char)line[0]))
    return 0;

  char *p = line;
  for (int field = 0; field < 5; field++) {
    while (*p == ' ')
      p++;
    if (!*p)
      return 0;
    while (*p && *p != ' ')
      p++;
  }
  while (*p == ' ')
    p++;
  if (!*p)
    return 0;
  *out_path = p;
  return 1;
}

/* Internal: determine output dir from pre-captured listing data.
   `listing` is mutable (strtok_r modifies it in-place). */
static char *determine_output_dir_impl(const char *filepath,
                                       char *listing, int list_ec,
                                       int *needs_password_recheck,
                                       int had_password) {
  char *base_dir = path_parent(filepath);
  char *stem = path_stem(filepath);
  if (!base_dir || !stem) {
    free(base_dir);
    free(stem);
    return NULL;
  }

  size_t path_count = 0;
  int has_root_file = 0;
  int single_top = 1;
  int overwrite_when_extract_to_base = 0;
  char *top = NULL;
  StrBuf target;
  sb_init(&target);

  char *saveptr = NULL;
  char *line = listing ? strtok_r(listing, "\n", &saveptr) : NULL;
  while (line) {
    char *p = NULL;
    if (parse_7z_list_ba_path(line, &p)) {
      if (!strcmp(p, ".") || !strcmp(p, "./")) {
        line = strtok_r(NULL, "\n", &saveptr);
        continue;
      }
      for (char *q = p; *q; ++q)
        if (*q == '\\')
          *q = '/';

      path_count++;
      if (single_top) {
        char *slash = strchr(p, '/');
        size_t top_len = slash ? (size_t)(slash - p) : strlen(p);
        if (!slash)
          has_root_file = 1;
        if (top_len > 0) {
          if (!top) {
            top = (char *)malloc(top_len + 1);
            if (top) {
              memcpy(top, p, top_len);
              top[top_len] = 0;
            } else {
              single_top = 0;
            }
          } else if (strncmp(top, p, top_len) != 0 || top[top_len] != 0) {
            single_top = 0;
          }
        }
      }

      if (!overwrite_when_extract_to_base) {
        target.len = 0;
        if (target.data)
          target.data[0] = 0;
        if (sb_append(&target, base_dir, strlen(base_dir)) &&
            sb_append_c(&target, '/') &&
            sb_append(&target, p, strlen(p)) && target.data &&
            path_exists(target.data)) {
          overwrite_when_extract_to_base = 1;
        }
      }
    }
    line = strtok_r(NULL, "\n", &saveptr);
  }

  char *candidate = NULL;
  if (path_count == 0 || list_ec != 0) {
    StrBuf tmp;
    sb_init(&tmp);
    sb_append(&tmp, base_dir, strlen(base_dir));
    sb_append_c(&tmp, '/');
    sb_append(&tmp, stem, strlen(stem));
    candidate = tmp.data;
  } else {
    int single_top_dir = (single_top && !has_root_file && !overwrite_when_extract_to_base);
    if (single_top_dir) {
      candidate = str_dup(base_dir);
    } else {
      StrBuf tmp;
      sb_init(&tmp);
      sb_append(&tmp, base_dir, strlen(base_dir));
      sb_append_c(&tmp, '/');
      sb_append(&tmp, stem, strlen(stem));
      candidate = tmp.data;
    }
  }

  if (needs_password_recheck && !had_password &&
      (list_ec != 0 || path_count == 0)) {
    *needs_password_recheck = 1;
  }

  candidate = next_available_output_dir(candidate, base_dir);
  sb_free(&target);
  free(top);
  free(base_dir);
  free(stem);
  return candidate;
}

char *determine_output_dir(const char *filepath, const char *password,
                           int *needs_password_recheck) {
  if (needs_password_recheck)
    *needs_password_recheck = 0;

  /* 构建文件指纹用于缓存查找 */
  char *fingerprint = build_file_fingerprint(filepath);
  if (!fingerprint)
    goto fallback;

  /* 尝试从缓存获取 */
  static _Thread_local ArchiveMetadataCache g_archive_cache = {NULL, 0, 0};
  static _Thread_local int cache_initialized = 0;
  if (!cache_initialized) {
    archive_cache_init(&g_archive_cache);
    cache_initialized = 1;
  }

  const ArchiveMetadata *cached = archive_cache_get(&g_archive_cache, fingerprint);
  if (cached) {
    /* 缓存命中：检查密码状态是否匹配 */
    int current_has_pwd = (password && *password) ? 1 : 0;
    if (cached->had_password == current_has_pwd) {
      char *result = determine_output_dir_impl(
          filepath, cached->listing ? str_dup(cached->listing) : NULL,
          cached->list_ec, needs_password_recheck, cached->had_password);
      free(fingerprint);
      return result;
    }
  }

fallback:;
  /* 缓存未命中或密码状态不匹配：执行 7z 扫描 */
  char *args[10];
  int argn = 0;
  args[argn++] = "7z";
  args[argn++] = "l";
  args[argn++] = "-ba";
  args[argn++] = "-bb0";
  args[argn++] = "-mmt=off";

  if (password && *password) {
    args[argn++] = "-p";
  }
  args[argn++] = (char *)filepath;

  StrBuf output;
  sb_init(&output);
  int list_ec =
      run_7z_capture(args, argn, &output, 0, NULL, 0.0, 0.0, NULL, NULL, 0, 0,
                     NULL, (password && *password) ? password : NULL);

  /* 将扫描结果存入缓存 */
  if (fingerprint) {
    archive_cache_put(&g_archive_cache, fingerprint, output.data, list_ec,
                      (password && *password) ? 1 : 0);
    free(fingerprint);
  }

  char *result = determine_output_dir_impl(
      filepath, output.data, list_ec, needs_password_recheck,
      (password && *password));
  sb_free(&output);
  return result;
}

char *determine_output_dir_with_listing(const char *filepath,
                                        const char *listing, int list_ec,
                                        int *needs_password_recheck) {
  if (needs_password_recheck)
    *needs_password_recheck = 0;
  /* Make a mutable copy since strtok_r modifies the buffer in-place. */
  char *buf = listing ? str_dup(listing) : NULL;
  char *result = determine_output_dir_impl(
      filepath, buf, list_ec, needs_password_recheck, 1);
  free(buf);
  return result;
}

/* =============== 归档元数据缓存 =============== */
static size_t hash_string_fnv1a(const char *s) {
  unsigned long long h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    h ^= (unsigned long long)(*p);
    h *= 1099511628211ULL;
  }
  return (size_t)h;
}

void archive_cache_init(ArchiveMetadataCache *c) {
  c->slots = NULL;
  c->len = c->cap = 0;
}

static int archive_cache_rehash(ArchiveMetadataCache *c, size_t new_cap) {
  ArchiveMetadata *new_slots =
      (ArchiveMetadata *)calloc(new_cap, sizeof(ArchiveMetadata));
  if (!new_slots)
    return 0;
  for (size_t i = 0; i < c->cap; i++) {
    if (!c->slots[i].fingerprint)
      continue;
    size_t idx = hash_string_fnv1a(c->slots[i].fingerprint) & (new_cap - 1);
    while (new_slots[idx].fingerprint)
      idx = (idx + 1) & (new_cap - 1);
    new_slots[idx] = c->slots[i];
  }
  free(c->slots);
  c->slots = new_slots;
  c->cap = new_cap;
  return 1;
}

static ArchiveMetadata *archive_cache_lookup(ArchiveMetadataCache *c,
                                             const char *fingerprint) {
  if (!c || !c->slots || c->cap == 0 || !fingerprint)
    return NULL;
  size_t idx = hash_string_fnv1a(fingerprint) & (c->cap - 1);
  while (1) {
    ArchiveMetadata *e = &c->slots[idx];
    if (!e->fingerprint || strcmp(e->fingerprint, fingerprint) == 0)
      return e;
    idx = (idx + 1) & (c->cap - 1);
  }
}

void archive_cache_free(ArchiveMetadataCache *c) {
  if (!c)
    return;
  for (size_t i = 0; i < c->cap; i++) {
    free(c->slots[i].fingerprint);
    free(c->slots[i].listing);
  }
  free(c->slots);
  c->slots = NULL;
  c->len = c->cap = 0;
}

const ArchiveMetadata *archive_cache_get(ArchiveMetadataCache *c,
                                         const char *fingerprint) {
  ArchiveMetadata *e = archive_cache_lookup(c, fingerprint);
  if (!e || !e->fingerprint)
    return NULL;
  return e;
}

void archive_cache_put(ArchiveMetadataCache *c, const char *fingerprint,
                       const char *listing, int list_ec, int had_password) {
  if (!c || !fingerprint)
    return;
  if (!c->slots) {
    c->cap = 64;
    c->slots = (ArchiveMetadata *)calloc(c->cap, sizeof(ArchiveMetadata));
    if (!c->slots) {
      c->cap = 0;
      return;
    }
  }
  if ((c->len + 1) * 10 > c->cap * 7) {
    if (c->cap > (size_t)-1 / 2 || !archive_cache_rehash(c, c->cap * 2))
      return;
  }

  ArchiveMetadata *e = archive_cache_lookup(c, fingerprint);
  if (!e)
    return;
  if (e->fingerprint) {
    char *new_listing = listing ? str_dup(listing) : NULL;
    free(e->listing);
    e->listing = new_listing;
    e->list_ec = list_ec;
    e->had_password = had_password;
    return;
  }

  e->fingerprint = str_dup(fingerprint);
  e->listing = listing ? str_dup(listing) : NULL;
  e->list_ec = list_ec;
  e->had_password = had_password;
  if (e->fingerprint) {
    c->len++;
    return;
  }
  free(e->fingerprint);
  free(e->listing);
  e->fingerprint = NULL;
  e->listing = NULL;
}

/* =============== 密码命中缓存 =============== */
void hit_cache_init(PasswordHitCache *c) {
  c->slots = NULL;
  c->len = c->cap = 0;
}

static int hit_cache_rehash(PasswordHitCache *c, size_t new_cap) {
  PasswordHit *new_slots =
      (PasswordHit *)calloc(new_cap, sizeof(PasswordHit));
  if (!new_slots)
    return 0;
  for (size_t i = 0; i < c->cap; i++) {
    if (!c->slots[i].fingerprint)
      continue;
    size_t idx = hash_string_fnv1a(c->slots[i].fingerprint) & (new_cap - 1);
    while (new_slots[idx].fingerprint)
      idx = (idx + 1) & (new_cap - 1);
    new_slots[idx] = c->slots[i];
  }
  free(c->slots);
  c->slots = new_slots;
  c->cap = new_cap;
  return 1;
}

static PasswordHit *hit_cache_lookup(PasswordHitCache *c,
                                     const char *fingerprint) {
  if (!c || !c->slots || c->cap == 0 || !fingerprint)
    return NULL;
  size_t idx = hash_string_fnv1a(fingerprint) & (c->cap - 1);
  while (1) {
    PasswordHit *e = &c->slots[idx];
    if (!e->fingerprint || strcmp(e->fingerprint, fingerprint) == 0)
      return e;
    idx = (idx + 1) & (c->cap - 1);
  }
}

void hit_cache_free(PasswordHitCache *c) {
  if (!c)
    return;
  for (size_t i = 0; i < c->cap; i++) {
    free(c->slots[i].fingerprint);
    free(c->slots[i].password);
  }
  free(c->slots);
  c->slots = NULL;
  c->len = c->cap = 0;
}

const char *hit_cache_get(PasswordHitCache *c, const char *fingerprint) {
  PasswordHit *e = hit_cache_lookup(c, fingerprint);
  if (!e || !e->fingerprint)
    return NULL;
  return e->password;
}

void hit_cache_put(PasswordHitCache *c, const char *fingerprint,
                   const char *password) {
  if (!c || !fingerprint || !password || !*password)
    return;
  if (!c->slots) {
    c->cap = 64;
    c->slots = (PasswordHit *)calloc(c->cap, sizeof(PasswordHit));
    if (!c->slots) {
      c->cap = 0;
      return;
    }
  }
  if ((c->len + 1) * 10 > c->cap * 7) {
    if (c->cap > (size_t)-1 / 2 || !hit_cache_rehash(c, c->cap * 2))
      return;
  }

  PasswordHit *e = hit_cache_lookup(c, fingerprint);
  if (!e)
    return;
  if (e->fingerprint) {
    char *new_pwd = str_dup(password);
    if (!new_pwd)
      return;
    free(e->password);
    e->password = new_pwd;
    return;
  }

  e->fingerprint = str_dup(fingerprint);
  e->password = str_dup(password);
  if (e->fingerprint && e->password) {
    c->len++;
    return;
  }
  free(e->fingerprint);
  free(e->password);
  e->fingerprint = NULL;
  e->password = NULL;
}

char *build_file_fingerprint(const char *filepath) {
  struct stat st;
  if (stat(filepath, &st) != 0)
    return str_dup(filepath);
  char buf[8192];
  snprintf(buf, sizeof(buf), "%s|%lld|%lld|%lld|%lld", filepath,
           (long long)st.st_size, (long long)st.st_mtime, (long long)st.st_dev,
           (long long)st.st_ino);
  return str_dup(buf);
}

static int ci_char_eq(char a, char b) {
  unsigned char ua = (unsigned char)a;
  unsigned char ub = (unsigned char)b;
  return tolower(ua) == tolower(ub);
}

static int ci_ext_eq(const char *ext, const char *target) {
  if (!ext || !target)
    return 0;
  size_t a_len = strlen(ext);
  size_t b_len = strlen(target);
  if (a_len != b_len)
    return 0;
  for (size_t i = 0; i < a_len; i++) {
    if (!ci_char_eq(ext[i], target[i]))
      return 0;
  }
  return 1;
}

static char *build_tagged_group_key(const char *tag, const char *path,
                                    size_t prefix_len, const char *suffix) {
  size_t tag_len = strlen(tag);
  size_t suffix_len = suffix ? strlen(suffix) : 0;
  if (tag_len > (size_t)-1 - prefix_len - suffix_len - 3)
    return NULL;
  size_t total_len = tag_len + 1 + prefix_len + suffix_len;
  char *key = (char *)malloc(total_len + 1);
  if (!key)
    return NULL;
  size_t off = 0;
  memcpy(key + off, tag, tag_len);
  off += tag_len;
  key[off++] = '|';
  memcpy(key + off, path, prefix_len);
  off += prefix_len;
  if (suffix_len > 0) {
    memcpy(key + off, suffix, suffix_len);
    off += suffix_len;
  }
  key[off] = 0;
  return key;
}

static int parse_split_numeric_suffix(const char *path, char **group_key,
                                      long *part_no) {
  if (!path || !*path || !group_key || !part_no)
    return 0;
  *group_key = NULL;
  *part_no = 0;

  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  const char *dot = strrchr(name, '.');
  if (!dot || !dot[1])
    return 0;

  const char *digits = dot + 1;
  size_t dlen = strlen(digits);
  if (dlen < 2 || dlen > 6)
    return 0;
  for (size_t i = 0; i < dlen; i++) {
    if (digits[i] < '0' || digits[i] > '9')
      return 0;
  }

  long num = strtol(digits, NULL, 10);
  if (num <= 0)
    return 0;

  size_t key_len = (size_t)(dot - path);
  char *key = build_tagged_group_key("NUM", path, key_len, "");
  if (!key)
    return 0;

  *group_key = key;
  *part_no = num;
  return 1;
}

static int parse_split_part_suffix(const char *path, char **group_key,
                                   long *part_no) {
  if (!path || !*path || !group_key || !part_no)
    return 0;

  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  const char *dot_ext = strrchr(name, '.');
  if (!dot_ext || !dot_ext[1])
    return 0;

  const char *canonical_ext = NULL;
  if (ci_ext_eq(dot_ext, ".rar"))
    canonical_ext = ".rar";
  else if (ci_ext_eq(dot_ext, ".zip"))
    canonical_ext = ".zip";
  else if (ci_ext_eq(dot_ext, ".7z"))
    canonical_ext = ".7z";
  else
    return 0;

  const char *digits_end = dot_ext;
  const char *digits_start = digits_end;
  while (digits_start > name &&
         isdigit((unsigned char)digits_start[-1])) {
    digits_start--;
  }
  if (digits_start == digits_end)
    return 0;
  if ((size_t)(digits_end - digits_start) > 6)
    return 0;

  if ((size_t)(digits_start - name) < 5)
    return 0;
  const char *tag = digits_start - 5;
  if (!(tag[0] == '.' && ci_char_eq(tag[1], 'p') && ci_char_eq(tag[2], 'a') &&
        ci_char_eq(tag[3], 'r') && ci_char_eq(tag[4], 't')))
    return 0;

  long num = strtol(digits_start, NULL, 10);
  if (num <= 0)
    return 0;

  size_t prefix_len = (size_t)(tag - path);
  char *key =
      build_tagged_group_key("PART", path, prefix_len, canonical_ext);
  if (!key)
    return 0;

  *group_key = key;
  *part_no = num;
  return 1;
}

static int parse_split_zr_suffix(const char *path, char **group_key,
                                 long *part_no) {
  if (!path || !*path || !group_key || !part_no)
    return 0;

  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  const char *dot = strrchr(name, '.');
  if (!dot || !dot[1])
    return 0;

  const char *ext = dot + 1;
  size_t ext_len = strlen(ext);
  if (ext_len < 2 || ext_len > 5)
    return 0;

  char lead = (char)tolower((unsigned char)ext[0]);
  if (lead != 'z' && lead != 'r')
    return 0;
  for (size_t i = 1; i < ext_len; i++) {
    if (!isdigit((unsigned char)ext[i]))
      return 0;
  }

  long num = strtol(ext + 1, NULL, 10);
  if (num < 0)
    return 0;

  const char *canonical_ext = (lead == 'z') ? ".zip" : ".rar";
  long order = (lead == 'r') ? (num + 2) : (num + 1);
  if (order <= 1)
    order = 2;

  size_t prefix_len = (size_t)(dot - path);
  char *key = build_tagged_group_key("ZR", path, prefix_len, canonical_ext);
  if (!key)
    return 0;

  *group_key = key;
  *part_no = order;
  return 1;
}

static int parse_split_zr_primary(const char *path, char **group_key,
                                  long *part_no) {
  if (!path || !*path || !group_key || !part_no)
    return 0;

  const char *name = strrchr(path, '/');
  name = name ? name + 1 : path;
  const char *dot = strrchr(name, '.');
  if (!dot || !dot[1])
    return 0;

  const char *canonical_ext = NULL;
  if (ci_ext_eq(dot, ".zip"))
    canonical_ext = ".zip";
  else if (ci_ext_eq(dot, ".rar"))
    canonical_ext = ".rar";
  else
    return 0;

  size_t prefix_len = (size_t)(dot - path);
  char *key = build_tagged_group_key("ZR", path, prefix_len, canonical_ext);
  if (!key)
    return 0;

  *group_key = key;
  *part_no = 1;
  return 1;
}

static int parse_split_group_key(const char *path, char **group_key,
                                 long *part_no) {
  if (parse_split_numeric_suffix(path, group_key, part_no))
    return 1;
  if (parse_split_part_suffix(path, group_key, part_no))
    return 1;
  if (parse_split_zr_suffix(path, group_key, part_no))
    return 1;
  if (parse_split_zr_primary(path, group_key, part_no))
    return 1;
  return 0;
}

typedef struct {
  const char *key;
  size_t member_count;
  size_t rep_index;
  int rep_is_first;
  long rep_part;
} SplitGroupEntry;

typedef struct {
  SplitGroupEntry *slots;
  size_t cap;
} SplitGroupMap;

static size_t next_pow2_at_least(size_t n) {
  size_t p = 1;
  while (p < n && p <= (size_t)-1 / 2)
    p <<= 1;
  return p < 16 ? 16 : p;
}

static int split_group_map_init(SplitGroupMap *map, size_t expected_items) {
  if (!map)
    return 0;
  size_t target = expected_items;
  if (target > ((size_t)-1 - 1) / 2)
    target = ((size_t)-1 - 1) / 2;
  size_t cap = next_pow2_at_least(target * 2 + 1);
  map->slots = (SplitGroupEntry *)calloc(cap, sizeof(SplitGroupEntry));
  if (!map->slots) {
    map->cap = 0;
    return 0;
  }
  map->cap = cap;
  return 1;
}

static void split_group_map_free(SplitGroupMap *map) {
  if (!map)
    return;
  free(map->slots);
  map->slots = NULL;
  map->cap = 0;
}

static SplitGroupEntry *split_group_map_lookup(SplitGroupMap *map,
                                               const char *key) {
  if (!map || !map->slots || map->cap == 0 || !key)
    return NULL;
  size_t idx = hash_string_fnv1a(key) & (map->cap - 1);
  while (1) {
    SplitGroupEntry *e = &map->slots[idx];
    if (!e->key || strcmp(e->key, key) == 0)
      return e;
    idx = (idx + 1) & (map->cap - 1);
  }
}

static int split_group_map_update(SplitGroupMap *map, const char *key,
                                  size_t idx, long part_no) {
  SplitGroupEntry *e = split_group_map_lookup(map, key);
  if (!e)
    return 0;
  if (!e->key) {
    e->key = key;
    e->member_count = 1;
    e->rep_index = idx;
    e->rep_is_first = (part_no == 1);
    e->rep_part = part_no;
    return 1;
  }
  e->member_count++;
  if (part_no == 1) {
    if (!e->rep_is_first || idx < e->rep_index) {
      e->rep_is_first = 1;
      e->rep_index = idx;
      e->rep_part = part_no;
    }
  } else if (!e->rep_is_first &&
             (part_no < e->rep_part ||
              (part_no == e->rep_part && idx < e->rep_index))) {
    e->rep_index = idx;
    e->rep_part = part_no;
  }
  return 1;
}

void task_path_list_free(TaskPathList *t) {
  if (!t)
    return;
  free(t->data);
  t->data = NULL;
  t->len = 0;
}

int build_task_paths(char **paths, size_t count, TaskPathList *out) {
  out->data = NULL;
  out->len = 0;
  if (!paths || count == 0)
    return 1;

  char **group_keys = (char **)calloc(count, sizeof(char *));
  long *part_no = (long *)calloc(count, sizeof(long));
  int *has_group = (int *)calloc(count, sizeof(int));
  int *keep = (int *)calloc(count, sizeof(int));
  SplitGroupMap group_map = {0};
  if (!group_keys || !part_no || !has_group || !keep ||
      !split_group_map_init(&group_map, count)) {
    free(group_keys);
    free(part_no);
    free(has_group);
    free(keep);
    split_group_map_free(&group_map);
    return 0;
  }

  for (size_t i = 0; i < count; i++) {
    keep[i] = 1;
    has_group[i] = parse_split_group_key(paths[i], &group_keys[i], &part_no[i]);
    if (has_group[i] &&
        !split_group_map_update(&group_map, group_keys[i], i, part_no[i])) {
      for (size_t k = 0; k < count; k++)
        free(group_keys[k]);
      free(group_keys);
      free(part_no);
      free(has_group);
      free(keep);
      split_group_map_free(&group_map);
      return 0;
    }
  }

  for (size_t i = 0; i < count; i++) {
    if (!has_group[i] || !group_keys[i])
      continue;
    SplitGroupEntry *e = split_group_map_lookup(&group_map, group_keys[i]);
    if (e && e->key && e->member_count >= 2)
      keep[i] = (i == e->rep_index) ? 1 : 0;
  }

  char **task_paths = (char **)malloc(count * sizeof(char *));
  if (!task_paths) {
    for (size_t i = 0; i < count; i++)
      free(group_keys[i]);
    free(group_keys);
    free(part_no);
    free(has_group);
    free(keep);
    split_group_map_free(&group_map);
    return 0;
  }

  size_t n = 0;
  for (size_t i = 0; i < count; i++) {
    if (keep[i])
      task_paths[n++] = paths[i];
  }

  for (size_t i = 0; i < count; i++)
    free(group_keys[i]);
  free(group_keys);
  free(part_no);
  free(has_group);
  free(keep);
  split_group_map_free(&group_map);

  out->data = task_paths;
  out->len = n;
  return 1;
}

/* =============== 密码库撞库 =============== */
typedef struct {
  char *bytes;
  char *locale;
  const char *origin_pwd;
  int hit_count;
} PwdTryCandidate;

typedef struct {
  PwdTryCandidate *data;
  size_t len;
  size_t cap;
} PwdTryCandidateVec;

typedef struct {
  char **slots;
  size_t len;
  size_t cap;
} StringSet;

static void strset_init(StringSet *set) {
  if (!set)
    return;
  set->slots = NULL;
  set->len = 0;
  set->cap = 0;
}

static void strset_free(StringSet *set) {
  if (!set)
    return;
  if (set->slots) {
    for (size_t i = 0; i < set->cap; i++)
      free(set->slots[i]);
  }
  free(set->slots);
  set->slots = NULL;
  set->len = 0;
  set->cap = 0;
}

static size_t strset_hash(const char *s) {
  unsigned long long h = 1469598103934665603ULL;
  for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
    h ^= (unsigned long long)(*p);
    h *= 1099511628211ULL;
  }
  return (size_t)h;
}

static int strset_rehash(StringSet *set, size_t new_cap) {
  char **new_slots = (char **)calloc(new_cap, sizeof(char *));
  if (!new_slots)
    return 0;
  for (size_t i = 0; i < set->cap; i++) {
    char *key = set->slots[i];
    if (!key)
      continue;
    size_t idx = strset_hash(key) & (new_cap - 1);
    while (new_slots[idx])
      idx = (idx + 1) & (new_cap - 1);
    new_slots[idx] = key;
  }
  free(set->slots);
  set->slots = new_slots;
  set->cap = new_cap;
  return 1;
}

/* Insert unique string into hash set: 1=inserted, 0=already exists, -1=OOM. */
static int strset_insert(StringSet *set, const char *s) {
  if (!set || !s || !*s)
    return 0;
  if (!set->slots) {
    set->cap = 256;
    set->slots = (char **)calloc(set->cap, sizeof(char *));
    if (!set->slots)
      return -1;
  }
  if ((set->len + 1) * 10 > set->cap * 7) {
    if (set->cap > (size_t)-1 / 2)
      return -1;
    if (!strset_rehash(set, set->cap * 2))
      return -1;
  }
  size_t idx = strset_hash(s) & (set->cap - 1);
  while (set->slots[idx]) {
    if (strcmp(set->slots[idx], s) == 0)
      return 0;
    idx = (idx + 1) & (set->cap - 1);
  }
  set->slots[idx] = str_dup(s);
  if (!set->slots[idx])
    return -1;
  set->len++;
  return 1;
}

static void candvec_init(PwdTryCandidateVec *v) {
  v->data = NULL;
  v->len = v->cap = 0;
}

static void candvec_free(PwdTryCandidateVec *v) {
  if (!v)
    return;
  for (size_t i = 0; i < v->len; i++) {
    free(v->data[i].bytes);
    free(v->data[i].locale);
  }
  free(v->data);
  v->data = NULL;
  v->len = v->cap = 0;
}

static int candvec_push(PwdTryCandidateVec *v, const char *bytes,
                        const char *locale, const char *origin_pwd,
                        int hit_count) {
  if (!v || !bytes || !*bytes || !origin_pwd)
    return 0;
  if (v->len == v->cap) {
    size_t new_cap = v->cap ? v->cap * 2 : 128;
    PwdTryCandidate *tmp =
        (PwdTryCandidate *)realloc(v->data, new_cap * sizeof(PwdTryCandidate));
    if (!tmp)
      return 0;
    v->data = tmp;
    v->cap = new_cap;
  }
  v->data[v->len].bytes = str_dup(bytes);
  v->data[v->len].locale = str_dup(locale ? locale : "");
  v->data[v->len].origin_pwd = origin_pwd;
  v->data[v->len].hit_count = hit_count;
  if (!v->data[v->len].bytes || !v->data[v->len].locale) {
    free(v->data[v->len].bytes);
    free(v->data[v->len].locale);
    v->data[v->len].bytes = NULL;
    v->data[v->len].locale = NULL;
    v->data[v->len].origin_pwd = NULL;
    return 0;
  }
  v->len++;
  return 1;
}

static int cand_cmp_hit_desc(const void *a, const void *b) {
  const PwdTryCandidate *ca = (const PwdTryCandidate *)a;
  const PwdTryCandidate *cb = (const PwdTryCandidate *)b;
  if (cb->hit_count != ca->hit_count)
    return (cb->hit_count > ca->hit_count) - (cb->hit_count < ca->hit_count);
  return 0;
}

static int build_try_candidates(PwdVec *v, PwdTryCandidateVec *out) {
  candvec_init(out);
  if (!v)
    return 1;
  StringSet seen;
  strset_init(&seen);
  for (size_t i = 0; i < v->len; i++) {
    if (!v->data[i].value || !*v->data[i].value)
      continue;
    PwdVarVec vars;
    varvec_init(&vars);
    build_password_variants(v->data[i].value, &vars);
    for (size_t k = 0; k < vars.len; k++) {
      if (!vars.data[k].bytes || !*vars.data[k].bytes)
        continue;
      int ins = strset_insert(&seen, vars.data[k].bytes);
      if (ins < 0) {
        varvec_free(&vars);
        strset_free(&seen);
        candvec_free(out);
        return 0;
      }
      if (ins == 0)
        continue;
      if (!candvec_push(out, vars.data[k].bytes, vars.data[k].locale,
                        v->data[i].value, v->data[i].hit_count)) {
        varvec_free(&vars);
        strset_free(&seen);
        candvec_free(out);
        return 0;
      }
    }
    varvec_free(&vars);
  }
  strset_free(&seen);
  /* Sort by hit_count descending so frequently-used passwords are tried first. */
  if (out->len > 1)
    qsort(out->data, out->len, sizeof(PwdTryCandidate), cand_cmp_hit_desc);
  return 1;
}

typedef struct {
  pid_t pid;
  int result_read_fd;
} TryWorkerProc;

static void cleanup_workers(TryWorkerProc *workers, int n) {
  if (!workers)
    return;
  for (int i = 0; i < n; i++) {
    if (workers[i].result_read_fd >= 0) {
      close(workers[i].result_read_fd);
      workers[i].result_read_fd = -1;
    }
  }
}

static void signal_worker_terminate(pid_t pid, int sig) {
  if (pid <= 0)
    return;
  /* Worker child calls setpgid(0, 0), so kill process group first. */
  kill(-pid, sig);
  kill(pid, sig);
}

static void reap_worker_with_timeout(pid_t pid, int wait_ms) {
  if (pid <= 0)
    return;
  int elapsed = 0;
  while (elapsed < wait_ms) {
    pid_t r = waitpid(pid, NULL, WNOHANG);
    if (r == pid)
      return;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      if (errno == ECHILD)
        return;
      break;
    }
    usleep(20000);
    elapsed += 20;
  }

  signal_worker_terminate(pid, SIGKILL);
  elapsed = 0;
  while (elapsed < 200) {
    pid_t r = waitpid(pid, NULL, WNOHANG);
    if (r == pid)
      return;
    if (r < 0) {
      if (errno == EINTR)
        continue;
      if (errno == ECHILD)
        return;
      break;
    }
    usleep(20000);
    elapsed += 20;
  }
}

static int read_hit_from_fd(int fd, char **hit) {
  if (fd < 0 || !hit)
    return 0;
  char buf[4096];
  size_t used = 0;
  while (used < sizeof(buf) - 1) {
    ssize_t n = read(fd, buf + used, sizeof(buf) - 1 - used);
    if (n > 0) {
      used += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }
  close(fd);
  if (used == 0)
    return 0;
  buf[used] = 0;
  *hit = str_dup(buf);
  return (*hit != NULL);
}

static void report_try_progress(TryPasswordProgressFn cb, void *ud,
                                int attempted, int total) {
  if (!cb || total <= 0)
    return;
  if (attempted < 0)
    attempted = 0;
  if (attempted > total)
    attempted = total;
  cb(attempted, total, ud);
}

static int should_use_bsdtar_gbk_password_probe(const char *filepath) {
  StrBuf listing;
  sb_init(&listing);
  int probe_rc =
      run_7z_probe_password_fast(filepath, NULL, NULL, 0, &listing, 0);
  int use_bsdtar =
      probe_rc == 1 &&
      archive_needs_legacy_gbk_password_before_extract(filepath, listing.data);
  sb_free(&listing);
  return use_bsdtar;
}

static void drain_try_progress_fd(int fd, int *attempted, int total,
                                  TryPasswordProgressFn cb, void *ud) {
  if (fd < 0 || !attempted || total <= 0)
    return;
  while (1) {
    unsigned char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      for (ssize_t i = 0; i < n; i++) {
        *attempted += (int)buf[i];
      }
      if (*attempted > total)
        *attempted = total;
      report_try_progress(cb, ud, *attempted, total);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      break;
    break;
  }
}

int try_password_list(const char *filepath, PwdVec *v, char **hit, int jobs,
                      StrBuf *listing_out, TryPasswordProgressFn progress_cb,
                      void *progress_ud) {
  if (hit)
    *hit = NULL;
  if (listing_out) {
    listing_out->len = 0;
    if (listing_out->data)
      listing_out->data[0] = 0;
  }
  if (!filepath || !v || v->len == 0)
    return 0;

  PwdTryCandidateVec cands;
  if (!build_try_candidates(v, &cands))
    return -1;
  if (cands.len == 0) {
    candvec_free(&cands);
    return 0;
  }
  int total_candidates = (int)cands.len;
  int attempted_candidates = 0;
  int use_bsdtar_gbk_probe = should_use_bsdtar_gbk_password_probe(filepath);
  report_try_progress(progress_cb, progress_ud, 0, total_candidates);

  if (jobs <= 1) {
    for (size_t i = 0; i < cands.len; i++) {
      if (run_7z_is_cancel_requested()) {
        candvec_free(&cands);
        return -1;
      }
      int r = use_bsdtar_gbk_probe
                  ? run_bsdtar_probe_password_for_file(filepath,
                                                       cands.data[i].bytes,
                                                       NULL)
                  : run_7z_probe_password_fast(filepath, cands.data[i].bytes,
                                               cands.data[i].locale, 1,
                                               listing_out, 1);
      attempted_candidates++;
      report_try_progress(progress_cb, progress_ud, attempted_candidates,
                          total_candidates);
      if (r == 1) {
        if (hit)
          *hit = str_dup(cands.data[i].origin_pwd);
        candvec_free(&cands);
        return 1;
      }
      if (r < 0) {
        candvec_free(&cands);
        return -1;
      }
    }
    candvec_free(&cands);
    return 0;
  }

  if (jobs > 64)
    jobs = 64;
  if ((size_t)jobs > cands.len)
    jobs = (int)cands.len;

  TryWorkerProc *workers = (TryWorkerProc *)calloc((size_t)jobs, sizeof(TryWorkerProc));
  if (!workers) {
    candvec_free(&cands);
    return -1;
  }
  for (int i = 0; i < jobs; i++) {
    workers[i].pid = -1;
    workers[i].result_read_fd = -1;
  }

  int progress_pipe[2] = {-1, -1};
  if (pipe(progress_pipe) == 0) {
    int flags = fcntl(progress_pipe[0], F_GETFL, 0);
    if (flags >= 0)
      (void)fcntl(progress_pipe[0], F_SETFL, flags | O_NONBLOCK);
  }

  int started = 0;
  for (int w = 0; w < jobs; w++) {
    if (run_7z_is_cancel_requested())
      break;
    int result_pipe[2];
    if (pipe(result_pipe) != 0)
      break;

    pid_t pid = fork();
    if (pid < 0) {
      close(result_pipe[0]);
      close(result_pipe[1]);
      break;
    }
    if (pid == 0) {
      close(result_pipe[0]);
      if (progress_pipe[0] >= 0)
        close(progress_pipe[0]);
      setpgid(0, 0);

      /* Batch size: test multiple passwords per 7z invocation */
      #define BATCH_SIZE 10
      const char *batch_pwd[BATCH_SIZE];
      const char *batch_locale[BATCH_SIZE];
      int batch_origin_idx[BATCH_SIZE];

      for (size_t idx = (size_t)w; idx < cands.len; idx += (size_t)jobs * BATCH_SIZE) {
        /* Collect a batch of passwords: consecutive slots owned by this worker */
        int batch_count = 0;
        for (size_t b = idx; b < cands.len && batch_count < BATCH_SIZE; b += (size_t)jobs) {
          batch_pwd[batch_count] = cands.data[b].bytes;
          batch_locale[batch_count] = cands.data[b].locale;
          batch_origin_idx[batch_count] = (int)b;
          batch_count++;
        }

        if (batch_count == 0)
          break;

        /* Test the batch */
        int hit_idx = -1;
        int attempted_in_batch = 0;
        int r = -2;
        if (use_bsdtar_gbk_probe) {
          for (int bi = 0; bi < batch_count; bi++) {
            attempted_in_batch++;
            int pr =
                run_bsdtar_probe_password_for_file(filepath, batch_pwd[bi],
                                                   NULL);
            if (pr == 1) {
              hit_idx = bi;
              r = bi;
              break;
            }
            if (pr < 0) {
              r = -1;
              break;
            }
          }
        } else {
          r = run_7z_probe_password_batch(filepath, batch_pwd, batch_locale,
                                          batch_count, &hit_idx,
                                          &attempted_in_batch);
        }
        if (progress_pipe[1] >= 0 && attempted_in_batch > 0) {
          unsigned char d = (unsigned char)(attempted_in_batch > 255
                                                ? 255
                                                : attempted_in_batch);
          (void)write(progress_pipe[1], &d, 1);
        }

        if (r >= 0 && hit_idx >= 0) {
          /* Password matched */
          int origin_idx = batch_origin_idx[hit_idx];
          const char *pwd = cands.data[origin_idx].origin_pwd;
          if (pwd && *pwd) {
            size_t pwd_len = strlen(pwd);
            size_t off = 0;
            while (off < pwd_len) {
              ssize_t n = write(result_pipe[1], pwd + off, pwd_len - off);
              if (n > 0) {
                off += (size_t)n;
                continue;
              }
              if (n < 0 && errno == EINTR)
                continue;
              break;
            }
          }
          close(result_pipe[1]);
          if (progress_pipe[1] >= 0)
            close(progress_pipe[1]);
          _exit(0);
        }
        if (r == -1) {
          /* Non-password error */
          close(result_pipe[1]);
          if (progress_pipe[1] >= 0)
            close(progress_pipe[1]);
          _exit(2);
        }
        /* r == -2: no match in this batch, continue to next batch */
      }
      #undef BATCH_SIZE
      close(result_pipe[1]);
      if (progress_pipe[1] >= 0)
        close(progress_pipe[1]);
      _exit(1);
    }
    close(result_pipe[1]);
    workers[w].pid = pid;
    workers[w].result_read_fd = result_pipe[0];
    started++;
  }

  if (started == 0) {
    if (progress_pipe[0] >= 0)
      close(progress_pipe[0]);
    if (progress_pipe[1] >= 0)
      close(progress_pipe[1]);
    cleanup_workers(workers, jobs);
    free(workers);
    candvec_free(&cands);
    return -1;
  }
  if (progress_pipe[1] >= 0) {
    close(progress_pipe[1]);
    progress_pipe[1] = -1;
  }

  int result = 0;
  int stop_early = 0;
  int live = started;
  while (live > 0) {
    drain_try_progress_fd(progress_pipe[0], &attempted_candidates,
                          total_candidates, progress_cb, progress_ud);
    if (run_7z_is_cancel_requested()) {
      result = -1;
      stop_early = 1;
      break;
    }
    int status = 0;
    pid_t pid = waitpid(-1, &status, WNOHANG);
    if (pid == 0) {
      usleep(50000);
      continue;
    }
    if (pid < 0) {
      if (errno == EINTR)
        continue;
      if (errno == ECHILD) {
        live = 0;
        break;
      }
      break;
    }

    int idx = -1;
    for (int i = 0; i < started; i++) {
      if (workers[i].pid == pid) {
        idx = i;
        workers[i].pid = -1;
        break;
      }
    }
    live--;
    if (idx < 0)
      continue;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
      if (hit && workers[idx].result_read_fd >= 0 &&
          read_hit_from_fd(workers[idx].result_read_fd, hit))
        result = 1;
      else
        result = -1;
      workers[idx].result_read_fd = -1;
      stop_early = 1;
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
      if (workers[idx].result_read_fd >= 0) {
        close(workers[idx].result_read_fd);
        workers[idx].result_read_fd = -1;
      }
      result = -1;
      stop_early = 1;
    } else {
      if (workers[idx].result_read_fd >= 0) {
        close(workers[idx].result_read_fd);
        workers[idx].result_read_fd = -1;
      }
    }

    if (stop_early)
      break;
  }

  if (stop_early) {
    for (int i = 0; i < started; i++) {
      if (workers[i].pid > 0)
        signal_worker_terminate(workers[i].pid, SIGTERM);
    }
  }

  for (int i = 0; i < started; i++) {
    if (workers[i].pid > 0) {
      reap_worker_with_timeout(workers[i].pid, 800);
      workers[i].pid = -1;
    }
  }
  drain_try_progress_fd(progress_pipe[0], &attempted_candidates,
                        total_candidates, progress_cb, progress_ud);
  if (progress_pipe[0] >= 0) {
    close(progress_pipe[0]);
    progress_pipe[0] = -1;
  }

  cleanup_workers(workers, jobs);
  free(workers);
  candvec_free(&cands);

  return result;
}

int try_password_list_polyglot_zip(const char *filepath, uint64_t zip_start,
                                   PwdVec *v, char **hit,
                                   TryPasswordProgressFn progress_cb,
                                   void *progress_ud) {
  if (hit)
    *hit = NULL;
  if (!filepath || !v || v->len == 0)
    return 0;

  PwdTryCandidateVec cands;
  if (!build_try_candidates(v, &cands))
    return -1;
  if (cands.len == 0) {
    candvec_free(&cands);
    return 0;
  }

  int total_candidates = (int)cands.len;
  int attempted_candidates = 0;
  report_try_progress(progress_cb, progress_ud, 0, total_candidates);

  for (size_t i = 0; i < cands.len; i++) {
    if (run_7z_is_cancel_requested()) {
      candvec_free(&cands);
      return -1;
    }

    int r =
        polyglot_probe_zip_password(filepath, zip_start, cands.data[i].bytes);
    attempted_candidates++;
    report_try_progress(progress_cb, progress_ud, attempted_candidates,
                        total_candidates);

    if (r == 1) {
      if (hit) {
        *hit = str_dup(cands.data[i].origin_pwd);
        if (!*hit) {
          candvec_free(&cands);
          return -1;
        }
      }
      candvec_free(&cands);
      return 1;
    }

    if (r < 0) {
      candvec_free(&cands);
      return -1;
    }
  }

  candvec_free(&cands);
  return 0;
}

/* 旧 UI 主流程已删除，仅保留 GTK UI 构建路径。 */
