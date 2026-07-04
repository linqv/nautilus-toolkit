#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "encoding.h"
#include "exec7z.h"
#include "strbuf.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static _Thread_local int g_cancel_requested = 0;
static _Thread_local pid_t g_active_7z_pid = -1;
static _Thread_local const int *g_external_cancel_flag = NULL;

static int external_cancel_requested(void) {
  if (!g_external_cancel_flag)
    return 0;
  return __atomic_load_n(g_external_cancel_flag, __ATOMIC_RELAXED) != 0;
}

static int cancel_requested(void) {
  return g_cancel_requested || external_cancel_requested();
}

static void set_active_7z_pid(pid_t pid) {
  g_active_7z_pid = pid;
}

static void clear_active_7z_pid(pid_t pid) {
  if (g_active_7z_pid == pid)
    g_active_7z_pid = -1;
}

static void terminate_pid(pid_t pid) {
  if (pid > 0)
    kill(pid, SIGTERM);
}

/* Non-blocking reap with SIGTERM→SIGKILL escalation.
   Avoids infinite blocking in waitpid() when 7z ignores SIGTERM. */
static int reap_7z_pid(pid_t pid, int *status) {
  if (pid <= 0)
    return -1;
  int st = 0;
  /* Phase 1: try immediate reap */
  pid_t r = waitpid(pid, &st, WNOHANG);
  if (r == pid) { *status = st; return 0; }
  if (r < 0 && errno == ECHILD) { *status = 0; return 0; }
  /* Phase 2: SIGTERM + poll up to 2s */
  kill(pid, SIGTERM);
  for (int i = 0; i < 100; i++) {
    usleep(20000);
    r = waitpid(pid, &st, WNOHANG);
    if (r == pid) { *status = st; return 0; }
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno == ECHILD) { *status = 0; return 0; }
      break;
    }
  }
  /* Phase 3: SIGKILL + poll up to 500ms */
  kill(pid, SIGKILL);
  for (int i = 0; i < 25; i++) {
    usleep(20000);
    r = waitpid(pid, &st, WNOHANG);
    if (r == pid) { *status = st; return 0; }
    if (r < 0) {
      if (errno == EINTR) continue;
      if (errno == ECHILD) { *status = 0; return 0; }
      break;
    }
  }
  *status = 0;
  return -1;
}

static uint16_t le16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int utf8_next_codepoint(const char **cursor, uint32_t *out_cp) {
  const unsigned char *s = (const unsigned char *)*cursor;
  if (!s || !*s)
    return 0;

  uint32_t cp = 0;
  size_t len = 0;
  if (s[0] < 0x80) {
    cp = s[0];
    len = 1;
  } else if ((s[0] & 0xe0) == 0xc0) {
    cp = s[0] & 0x1f;
    len = 2;
  } else if ((s[0] & 0xf0) == 0xe0) {
    cp = s[0] & 0x0f;
    len = 3;
  } else if ((s[0] & 0xf8) == 0xf0) {
    cp = s[0] & 0x07;
    len = 4;
  } else {
    return 0;
  }

  for (size_t i = 1; i < len; i++) {
    if ((s[i] & 0xc0) != 0x80)
      return 0;
    cp = (cp << 6) | (uint32_t)(s[i] & 0x3f);
  }

  *cursor += len;
  if (out_cp)
    *out_cp = cp;
  return 1;
}

static int utf8_contains_cjk(const char *s) {
  if (!s || !*s)
    return 0;
  const char *p = s;
  while (*p) {
    uint32_t cp = 0;
    if (!utf8_next_codepoint(&p, &cp))
      return 0;
    if ((cp >= 0x3400 && cp <= 0x4dbf) ||
        (cp >= 0x4e00 && cp <= 0x9fff) ||
        (cp >= 0xf900 && cp <= 0xfaff) ||
        (cp >= 0x3000 && cp <= 0x303f) ||
        (cp >= 0xff00 && cp <= 0xffef))
      return 1;
  }
  return 0;
}

static int zip_name_bytes_look_gbk(const unsigned char *name, size_t len) {
  if (!name || len == 0 || memchr(name, '\0', len))
    return 0;

  int has_non_ascii = 0;
  for (size_t i = 0; i < len; i++) {
    if (name[i] >= 0x80) {
      has_non_ascii = 1;
      break;
    }
  }
  if (!has_non_ascii)
    return 0;

  char *raw = (char *)malloc(len + 1);
  if (!raw)
    return 0;
  memcpy(raw, name, len);
  raw[len] = 0;

  if (is_valid_utf8(raw)) {
    free(raw);
    return 0;
  }

  StrBuf converted;
  sb_init(&converted);
  int ok = convert_encoding_from(raw, "GBK", &converted);
  if (!ok)
    ok = convert_encoding_from(raw, "GB18030", &converted);
  free(raw);

  int looks_gbk = ok && converted.data && is_valid_utf8(converted.data) &&
                  utf8_contains_cjk(converted.data);
  sb_free(&converted);
  return looks_gbk;
}

static int zip_extra_has_field(const unsigned char *extra, size_t len,
                               uint16_t field_id) {
  size_t off = 0;
  while (off + 4 <= len) {
    uint16_t id = le16(extra + off);
    uint16_t size = le16(extra + off + 2);
    off += 4;
    if (size > len - off)
      return 0;
    if (id == field_id)
      return 1;
    off += size;
  }
  return 0;
}

int archive_has_legacy_gbk_zip_names(const char *filepath) {
  if (!filepath || !*filepath)
    return 0;

  FILE *f = fopen(filepath, "rb");
  if (!f)
    return 0;

  int result = 0;
  if (fseeko(f, 0, SEEK_END) != 0)
    goto done;
  off_t file_size = ftello(f);
  if (file_size < 22)
    goto done;

  size_t scan_len = (file_size < 66000) ? (size_t)file_size : 66000;
  unsigned char *buf = (unsigned char *)malloc(scan_len);
  if (!buf)
    goto done;

  if (fseeko(f, file_size - (off_t)scan_len, SEEK_SET) != 0 ||
      fread(buf, 1, scan_len, f) != scan_len) {
    free(buf);
    goto done;
  }

  unsigned char *eocd = NULL;
  for (size_t i = scan_len - 22;; i--) {
    if (buf[i] == 'P' && buf[i + 1] == 'K' && buf[i + 2] == 0x05 &&
        buf[i + 3] == 0x06) {
      uint16_t comment_len = le16(buf + i + 20);
      if (i + 22u + comment_len == scan_len) {
        eocd = buf + i;
        break;
      }
    }
    if (i == 0)
      break;
  }

  if (!eocd) {
    free(buf);
    goto done;
  }

  uint16_t disk_no = le16(eocd + 4);
  uint16_t cd_disk = le16(eocd + 6);
  uint16_t entries_on_disk = le16(eocd + 8);
  uint16_t entries = le16(eocd + 10);
  uint32_t cd_size = le32(eocd + 12);
  uint32_t cd_offset = le32(eocd + 16);
  free(buf);

  if (disk_no != 0 || cd_disk != 0 || entries_on_disk != entries ||
      entries == 0 || entries == 0xffff || cd_size == 0 ||
      cd_size == 0xffffffffu || cd_offset == 0xffffffffu ||
      (uint64_t)cd_offset + (uint64_t)cd_size > (uint64_t)file_size)
    goto done;

  if (fseeko(f, (off_t)cd_offset, SEEK_SET) != 0)
    goto done;

  for (uint16_t i = 0; i < entries; i++) {
    unsigned char hdr[46];
    if (fread(hdr, 1, sizeof(hdr), f) != sizeof(hdr))
      goto done;
    if (hdr[0] != 'P' || hdr[1] != 'K' || hdr[2] != 0x01 || hdr[3] != 0x02)
      goto done;

    uint16_t flags = le16(hdr + 8);
    uint16_t name_len = le16(hdr + 28);
    uint16_t extra_len = le16(hdr + 30);
    uint16_t comment_len = le16(hdr + 32);

    unsigned char *name = NULL;
    unsigned char *extra = NULL;
    if (name_len > 0) {
      name = (unsigned char *)malloc(name_len);
      if (!name)
        goto done;
      if (fread(name, 1, name_len, f) != name_len) {
        free(name);
        goto done;
      }
    }

    if (extra_len > 0) {
      extra = (unsigned char *)malloc(extra_len);
      if (!extra) {
        free(name);
        goto done;
      }
      if (fread(extra, 1, extra_len, f) != extra_len) {
        free(name);
        free(extra);
        goto done;
      }
    }

    int has_unicode_path = zip_extra_has_field(extra, extra_len, 0x7075);
    if (name && !(flags & 0x0800) && !has_unicode_path &&
        zip_name_bytes_look_gbk(name, name_len)) {
      free(name);
      free(extra);
      result = 1;
      goto done;
    }
    free(name);
    free(extra);

    if (fseeko(f, (off_t)comment_len, SEEK_CUR) != 0)
      goto done;
  }

done:
  fclose(f);
  return result;
}

int archive_needs_legacy_gbk_zip_fallback(const char *filepath,
                                          const char *listing) {
  if (!listing || !*listing)
    return 0;
  return archive_has_legacy_gbk_zip_names(filepath) && !is_valid_utf8(listing);
}

int archive_needs_legacy_gbk_password_before_extract(const char *filepath,
                                                     const char *listing) {
  if (!archive_needs_legacy_gbk_zip_fallback(filepath, listing))
    return 0;
  return archive_needs_password_before_extract(filepath);
}

int archive_needs_password_before_extract(const char *filepath) {
  return archive_has_encrypted_content(filepath) == 1;
}

static pid_t spawn_capture_child(char **argv, int *out_fd) {
  int pipefd[2];
  if (!argv || !argv[0] || !out_fd || pipe(pipefd) != 0)
    return -1;

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    unsetenv("LC_ALL");
    setenv("LC_MESSAGES", "C", 1);
    setenv("LC_CTYPE", "C.UTF-8", 1);
    setenv("LANG", "C.UTF-8", 1);

    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);

    execvp(argv[0], argv);
    dprintf(STDERR_FILENO, "execvp(%s) failed: %s\n", argv[0],
            strerror(errno));
    _exit(127);
  }

  close(pipefd[1]);
  *out_fd = pipefd[0];
  return pid;
}

static int run_child_capture(char **argv, StrBuf *out) {
  if (!out)
    return 127;
  out->len = 0;
  if (out->data)
    out->data[0] = 0;
  if (cancel_requested())
    return 130;

  int read_fd = -1;
  pid_t pid = spawn_capture_child(argv, &read_fd);
  if (pid < 0)
    return 127;
  set_active_7z_pid(pid);

  int flags = fcntl(read_fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);

  int status = 0;
  int have_status = 0;
  int finished = 0;
  int was_cancelled = 0;
  while (!finished) {
    if (cancel_requested()) {
      was_cancelled = 1;
      terminate_pid(pid);
    }

    struct pollfd pfd = {.fd = read_fd, .events = POLLIN | POLLHUP};
    int pr = poll(&pfd, 1, 200);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (pr == 0) {
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        have_status = 1;
        finished = 1;
      }
      continue;
    }

    if (pfd.revents & (POLLIN | POLLHUP)) {
      while (1) {
        char tmp[4096];
        ssize_t n = read(read_fd, tmp, sizeof(tmp));
        if (n > 0) {
          if (!sb_append(out, tmp, (size_t)n))
            finished = 1;
          continue;
        }
        if (n == 0) {
          finished = 1;
          break;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        if (errno == EINTR)
          continue;
        finished = 1;
        break;
      }
    }

    if (!finished) {
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        have_status = 1;
        finished = 1;
      }
    }
  }

  if (!was_cancelled) {
    while (1) {
      char tmp[4096];
      ssize_t n = read(read_fd, tmp, sizeof(tmp));
      if (n > 0) {
        if (!sb_append(out, tmp, (size_t)n))
          break;
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      break;
    }
  }
  close(read_fd);

  if (!have_status) {
    reap_7z_pid(pid, &status);
    have_status = 1;
  }
  clear_active_7z_pid(pid);

  if (was_cancelled)
    return 130;
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return 127;
}

void run_7z_request_cancel(void) {
  g_cancel_requested = 1;
  terminate_pid(g_active_7z_pid);
}

void run_7z_clear_cancel_request(void) {
  g_cancel_requested = 0;
}

int run_7z_is_cancel_requested(void) { return cancel_requested(); }

void run_7z_bind_cancel_flag(const int *cancel_flag) {
  g_external_cancel_flag = cancel_flag;
}

void run_7z_unbind_cancel_flag(void) { g_external_cancel_flag = NULL; }

static void rolling_append(char *buf, size_t cap, size_t *used,
                           const char *chunk, size_t n) {
  if (!buf || cap == 0 || !used || !chunk || n == 0)
    return;
  if (n >= cap - 1) {
    size_t start = 0;
    if (n > cap - 1)
      start = n - (cap - 1);
    memcpy(buf, chunk + start, cap - 1);
    *used = cap - 1;
    buf[*used] = 0;
    return;
  }
  if (*used + n >= cap) {
    size_t keep = cap - 1 - n;
    memmove(buf, buf + (*used - keep), keep);
    memcpy(buf + keep, chunk, n);
    *used = keep + n;
    buf[*used] = 0;
    return;
  }
  memcpy(buf + *used, chunk, n);
  *used += n;
  buf[*used] = 0;
}

static void clear_captured_listing(StrBuf *captured_listing) {
  if (!captured_listing)
    return;
  captured_listing->len = 0;
  if (captured_listing->data)
    captured_listing->data[0] = 0;
}

static pid_t spawn_7z_child(char **args, int argn, const char *ctype_locale,
                            const char *stdin_password, int *out_fd);

typedef struct {
  char *filepath;
  int has_encrypted_content;
  char *test_file;
  int test_requires_full_read;
  int ready;
  /* 1 = headers are encrypted (7z l without password fails).
     When headers_encrypted=1, a successful "7z l -p<pwd>" is sufficient
     proof that the password is correct — no need for probe_password_test. */
  int headers_encrypted;
  int headers_encrypted_known;
} ProbeTargetCache;

static _Thread_local ProbeTargetCache g_probe_target_cache = {0};

static void probe_target_cache_reset(void) {
  free(g_probe_target_cache.filepath);
  free(g_probe_target_cache.test_file);
  g_probe_target_cache.filepath = NULL;
  g_probe_target_cache.has_encrypted_content = 0;
  g_probe_target_cache.test_file = NULL;
  g_probe_target_cache.test_requires_full_read = 0;
  g_probe_target_cache.ready = 0;
  g_probe_target_cache.headers_encrypted = 0;
  g_probe_target_cache.headers_encrypted_known = 0;
}

static int method_requires_full_password_test(const char *method) {
  if (!method)
    return 0;
  while (*method == ' ' || *method == '\t')
    method++;
  return strncmp(method, "Copy", 4) == 0 &&
         (method[4] == 0 || isspace((unsigned char)method[4]));
}

static void maybe_pick_probe_file(const char *path, int folder, int encrypted,
                                  long long size, const char *method,
                                  int *has_encrypted, char **best_path,
                                  long long *best_size,
                                  int *best_requires_full_read) {
  if (!path || !*path)
    return;
  if (encrypted != 1)
    return;

  *has_encrypted = 1;
  if (folder)
    return;

  int should_replace = 0;
  if (!*best_path) {
    should_replace = 1;
  } else if (size >= 0 && (*best_size < 0 || size < *best_size)) {
    should_replace = 1;
  }

  if (should_replace) {
    char *dup = str_dup(path);
    if (!dup)
      return;
    free(*best_path);
    *best_path = dup;
    *best_size = size;
    if (best_requires_full_read)
      *best_requires_full_read = method_requires_full_password_test(method);
  }
}

static int scan_probe_target_file(const char *filepath, const char *pwd_bytes,
                                  int *has_encrypted, char **test_file,
                                  int *requires_full_read) {
  if (has_encrypted)
    *has_encrypted = 0;
  if (test_file)
    *test_file = NULL;
  if (requires_full_read)
    *requires_full_read = 0;
  if (!filepath || !*filepath)
    return -1;

  char *args[9];
  int argn = 0;
  args[argn++] = "7z";
  args[argn++] = "l";
  args[argn++] = "-slt";
  args[argn++] = "-bb0";
  args[argn++] = "-mmt=off";
  if (pwd_bytes && *pwd_bytes)
    args[argn++] = "-p";
  args[argn++] = (char *)filepath;

  StrBuf output;
  sb_init(&output);
  int ec =
      run_7z_capture(args, argn, &output, 0, NULL, 0.0, 0.0, NULL, NULL, 0, 0,
                     NULL, (pwd_bytes && *pwd_bytes) ? pwd_bytes : NULL);
  if (ec != 0) {
    sb_free(&output);
    return -1;
  }

  int found_encrypted = 0;
  char *best_path = NULL;
  long long best_size = -1;
  int best_requires_full_read = 0;

  char *cur_path = NULL;
  char *cur_method = NULL;
  int cur_folder = 0;
  int cur_encrypted = -1;
  long long cur_size = -1;

  char *saveptr = NULL;
  for (char *line = strtok_r(output.data, "\n", &saveptr); line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    size_t n = strlen(line);
    while (n > 0 && line[n - 1] == '\r') {
      line[n - 1] = 0;
      n--;
    }

    while (*line == ' ' || *line == '\t')
      line++;
    if (!*line)
      continue;

    if (!strncmp(line, "Path = ", 7)) {
      maybe_pick_probe_file(cur_path, cur_folder, cur_encrypted, cur_size,
                            cur_method, &found_encrypted, &best_path,
                            &best_size, &best_requires_full_read);
      free(cur_path);
      free(cur_method);
      cur_path = str_dup(line + 7);
      cur_method = NULL;
      cur_folder = 0;
      cur_encrypted = -1;
      cur_size = -1;
      continue;
    }

    if (!cur_path)
      continue;

    if (!strncmp(line, "Folder = ", 9)) {
      cur_folder = (line[9] == '+') ? 1 : 0;
      continue;
    }
    if (!strncmp(line, "Encrypted = ", 12)) {
      if (line[12] == '+')
        cur_encrypted = 1;
      else if (line[12] == '-')
        cur_encrypted = 0;
      continue;
    }
    if (!strncmp(line, "Size = ", 7)) {
      errno = 0;
      char *endptr = NULL;
      long long v = strtoll(line + 7, &endptr, 10);
      if (errno == 0 && endptr && *endptr == 0 && v >= 0)
        cur_size = v;
      continue;
    }
    if (!strncmp(line, "Method = ", 9)) {
      free(cur_method);
      cur_method = str_dup(line + 9);
      continue;
    }
  }

  maybe_pick_probe_file(cur_path, cur_folder, cur_encrypted, cur_size,
                        cur_method, &found_encrypted, &best_path, &best_size,
                        &best_requires_full_read);
  free(cur_path);
  free(cur_method);
  sb_free(&output);

  if (has_encrypted)
    *has_encrypted = found_encrypted;
  if (test_file)
    *test_file = best_path;
  else
    free(best_path);
  if (requires_full_read)
    *requires_full_read = best_requires_full_read;
  return 0;
}

static int ensure_probe_target_cached(const char *filepath,
                                      const char *pwd_bytes,
                                      int *has_encrypted,
                                      const char **test_file,
                                      int *requires_full_read) {
  if (!filepath || !*filepath)
    return -1;

  if (!g_probe_target_cache.ready || !g_probe_target_cache.filepath ||
      strcmp(g_probe_target_cache.filepath, filepath) != 0) {
    probe_target_cache_reset();
    g_probe_target_cache.filepath = str_dup(filepath);
    if (!g_probe_target_cache.filepath)
      return -1;

    int found_encrypted = 0;
    int full_read = 0;
    char *picked_file = NULL;
    if (scan_probe_target_file(filepath, pwd_bytes, &found_encrypted,
                               &picked_file, &full_read) != 0) {
      probe_target_cache_reset();
      return -1;
    }

    g_probe_target_cache.has_encrypted_content = found_encrypted;
    g_probe_target_cache.test_file = picked_file;
    g_probe_target_cache.test_requires_full_read = full_read;
    g_probe_target_cache.ready = 1;
  }

  if (has_encrypted)
    *has_encrypted = g_probe_target_cache.has_encrypted_content;
  if (test_file)
    *test_file = g_probe_target_cache.test_file;
  if (requires_full_read)
    *requires_full_read = g_probe_target_cache.test_requires_full_read;
  return 0;
}

static int known_magic_matches(const unsigned char *buf, size_t len) {
  if (!buf)
    return -1;

  if (len >= 6) {
    static const unsigned char sig_7z[] = {0x37, 0x7a, 0xbc,
                                           0xaf, 0x27, 0x1c};
    static const unsigned char sig_xz[] = {0xfd, '7', 'z', 'X', 'Z', 0x00};
    if (memcmp(buf, sig_7z, sizeof(sig_7z)) == 0 ||
        memcmp(buf, sig_xz, sizeof(sig_xz)) == 0)
      return 1;
    if (memcmp(buf, "Rar!\x1a\x07", 6) == 0 &&
        (len < 7 || buf[6] == 0x00 || buf[6] == 0x01))
      return 1;
    if (memcmp(buf, "GIF87a", 6) == 0 || memcmp(buf, "GIF89a", 6) == 0)
      return 1;
  }
  if (len >= 4) {
    if (buf[0] == 'P' && buf[1] == 'K' &&
        (buf[2] == 3 || buf[2] == 5 || buf[2] == 7) &&
        (buf[3] == 4 || buf[3] == 6 || buf[3] == 8))
      return 1;
    if (buf[0] == 0x28 && buf[1] == 0xb5 && buf[2] == 0x2f &&
        buf[3] == 0xfd)
      return 1;
    if (memcmp(buf, "%PDF", 4) == 0)
      return 1;
    if (buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F')
      return 1;
  }
  if (len >= 3) {
    if (buf[0] == 0xff && buf[1] == 0xd8 && buf[2] == 0xff)
      return 1;
    if (buf[0] == 'B' && buf[1] == 'Z' && buf[2] == 'h')
      return 1;
  }
  if (len >= 2 && buf[0] == 0x1f && buf[1] == 0x8b)
    return 1;
  if (len >= 8) {
    static const unsigned char sig_png[] = {0x89, 'P', 'N', 'G',
                                            0x0d, 0x0a, 0x1a, 0x0a};
    if (memcmp(buf, sig_png, sizeof(sig_png)) == 0)
      return 1;
  }
  if (len >= 262 && memcmp(buf + 257, "ustar", 5) == 0)
    return 1;

  return -1;
}

static int prefix_looks_random_binary(const unsigned char *buf, size_t len) {
  if (!buf || len < 128)
    return 0;

  size_t printable = 0;
  size_t controls = 0;
  size_t high = 0;
  size_t zeros = 0;
  for (size_t i = 0; i < len; i++) {
    unsigned char c = buf[i];
    if (c == 0)
      zeros++;
    if (c >= 0x80)
      high++;
    if ((c >= 0x20 && c <= 0x7e) || c == '\n' || c == '\r' || c == '\t')
      printable++;
    else if (c < 0x20)
      controls++;
  }

  return high * 100 > len * 30 && printable * 100 < len * 65 &&
         controls * 100 > len * 5 && zeros * 100 < len * 3;
}

static int run_7z_probe_password_magic(const char *filepath,
                                       const char *test_file,
                                       const char *pwd_bytes,
                                       const char *locale) {
  const size_t want = 512;

  char *args[14];
  int argn = 0;
  args[argn++] = "7z";
  args[argn++] = "e";
  args[argn++] = (char *)filepath;
  args[argn++] = (char *)test_file;
  args[argn++] = "-so";
  if (pwd_bytes && *pwd_bytes)
    args[argn++] = "-p";
  args[argn++] = "-bb0";
  args[argn++] = "-bd";
  args[argn++] = "-bso0";
  args[argn++] = "-bse0";
  args[argn++] = "-bsp0";
  args[argn++] = "-mmt=off";

  int read_fd = -1;
  const char *pwd = (pwd_bytes && *pwd_bytes) ? pwd_bytes : NULL;
  pid_t pid = spawn_7z_child(args, argn, locale, pwd, &read_fd);
  if (pid < 0)
    return -1;
  set_active_7z_pid(pid);

  int flags = fcntl(read_fd, F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);

  unsigned char prefix[512];
  size_t used = 0;
  int elapsed_ms = 0;
  while (used < want && elapsed_ms < 5000) {
    if (cancel_requested())
      break;
    struct pollfd pfd = {.fd = read_fd, .events = POLLIN | POLLHUP};
    int pr = poll(&pfd, 1, 100);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (pr == 0) {
      elapsed_ms += 100;
      continue;
    }
    if (pfd.revents & (POLLIN | POLLHUP)) {
      while (used < want) {
        ssize_t n = read(read_fd, prefix + used, want - used);
        if (n > 0) {
          used += (size_t)n;
          continue;
        }
        if (n == 0) {
          elapsed_ms = 5000;
          break;
        }
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        elapsed_ms = 5000;
        break;
      }
    }
  }

  terminate_pid(pid);
  close(read_fd);
  int status = 0;
  reap_7z_pid(pid, &status);
  clear_active_7z_pid(pid);

  if (cancel_requested())
    return -1;
  int magic_rc = known_magic_matches(prefix, used);
  if (magic_rc == 1)
    return 1;
  if (prefix_looks_random_binary(prefix, used))
    return 0;
  return -1;
}

static void tail_ring_push(char *buf, size_t cap_with_nul, size_t *len,
                           size_t *write_pos, const char *chunk, size_t n) {
  if (!buf || cap_with_nul <= 1 || !len || !write_pos || !chunk || n == 0)
    return;
  size_t cap = cap_with_nul - 1;
  if (n > cap) {
    chunk += (n - cap);
    n = cap;
  }
  size_t first = cap - *write_pos;
  if (first > n)
    first = n;
  memcpy(buf + *write_pos, chunk, first);
  if (n > first)
    memcpy(buf, chunk + first, n - first);
  *write_pos = (*write_pos + n) % cap;
  if (*len + n >= cap)
    *len = cap;
  else
    *len += n;
}

static int tail_ring_finalize(StrBuf *out, const char *buf, size_t cap_with_nul,
                              size_t len, size_t write_pos) {
  if (!out || !buf || cap_with_nul <= 1) {
    if (out) {
      out->len = 0;
      if (out->data)
        out->data[0] = 0;
    }
    return 1;
  }
  if (!sb_reserve(out, len + 1))
    return 0;

  const size_t cap = cap_with_nul - 1;
  if (len == 0) {
    out->len = 0;
    out->data[0] = 0;
    return 1;
  }
  if (len < cap) {
    memcpy(out->data, buf, len);
    out->len = len;
    out->data[len] = 0;
    return 1;
  }

  size_t first = cap - write_pos;
  memcpy(out->data, buf + write_pos, first);
  if (write_pos > 0)
    memcpy(out->data + first, buf, write_pos);
  out->len = cap;
  out->data[cap] = 0;
  return 1;
}

typedef struct {
  int last;
  int cur;
  int in_num;
} PercentParser;

static void percent_parser_init(PercentParser *p) {
  p->last = -1;
  p->cur = 0;
  p->in_num = 0;
}

static void percent_parser_feed(PercentParser *p, const char *s, size_t n) {
  if (!p || !s || n == 0)
    return;
  for (size_t i = 0; i < n; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (isdigit(ch)) {
      if (!p->in_num) {
        p->cur = (int)(ch - '0');
        p->in_num = 1;
      } else if (p->cur <= 1000) {
        p->cur = p->cur * 10 + (int)(ch - '0');
      }
      continue;
    }
    if (ch == '%' && p->in_num && p->cur >= 0 && p->cur <= 100)
      p->last = p->cur;
    p->in_num = 0;
    p->cur = 0;
  }
}

/* Shared helper: fork a 7z child process with output pipe and optional
   password pipe.  Returns child pid on success, -1 on failure.
   *out_fd is set to the read end of the stdout/stderr pipe. */
static pid_t spawn_7z_child(char **args, int argn, const char *ctype_locale,
                            const char *stdin_password, int *out_fd) {
  int pipefd[2];
  if (pipe(pipefd) != 0)
    return -1;

  int pwd_pipe[2] = {-1, -1};
  int use_pwd_pipe = (stdin_password && *stdin_password);
  if (use_pwd_pipe && pipe(pwd_pipe) != 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    if (use_pwd_pipe) {
      close(pwd_pipe[0]);
      close(pwd_pipe[1]);
    }
    return -1;
  }

  if (pid == 0) {
    /* Child process */
    unsetenv("LC_ALL");
    setenv("LC_MESSAGES", "C", 1);
    const char *ctype =
        (ctype_locale && *ctype_locale) ? ctype_locale : "C.UTF-8";
    setenv("LC_CTYPE", ctype, 1);
    setenv("LANG", ctype, 1);

    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[0]);
    close(pipefd[1]);

    char *argv[argn + 2];
    argv[0] = args[0];
    argv[1] = "-sccUTF-8";
    for (int i = 1; i < argn; i++)
      argv[i + 1] = args[i];
    argv[argn + 1] = NULL;

    if (use_pwd_pipe) {
      close(pwd_pipe[1]);
      char pwd_buf[4096];
      size_t pwd_len = 0;
      while (pwd_len < sizeof(pwd_buf) - 1) {
        ssize_t r = read(pwd_pipe[0], pwd_buf + pwd_len,
                         sizeof(pwd_buf) - 1 - pwd_len);
        if (r > 0) {
          pwd_len += (size_t)r;
          continue;
        }
        if (r < 0 && errno == EINTR)
          continue;
        break;
      }
      close(pwd_pipe[0]);
      pwd_buf[pwd_len] = '\0';

      char *p_arg_buf = (char *)malloc(pwd_len + 3);
      if (!p_arg_buf)
        _exit(127);
      p_arg_buf[0] = '-';
      p_arg_buf[1] = 'p';
      memcpy(p_arg_buf + 2, pwd_buf, pwd_len);
      p_arg_buf[pwd_len + 2] = '\0';
      for (int i = 0; i < argn + 1; i++) {
        if (argv[i][0] == '-' && argv[i][1] == 'p' && argv[i][2] == '\0') {
          argv[i] = p_arg_buf;
          break;
        }
      }
    }

    execvp("7z", argv);
    _exit(127);
  }

  /* Parent process */
  close(pipefd[1]);
  if (use_pwd_pipe) {
    close(pwd_pipe[0]);
    size_t pwd_len = strlen(stdin_password);
    size_t off = 0;
    while (off < pwd_len) {
      ssize_t w = write(pwd_pipe[1], stdin_password + off, pwd_len - off);
      if (w > 0) {
        off += (size_t)w;
        continue;
      }
      if (w < 0 && errno == EINTR)
        continue;
      break;
    }
    close(pwd_pipe[1]);
  }

  *out_fd = pipefd[0];
  return pid;
}

int run_7z_capture(char **args, int argn, StrBuf *out, int nonblock,
                   FILE *progress_pipe, double start_pct, double slot_size,
                   const char *ctype_locale, const char *archive_label,
                   int task_index, int task_total,
                   int *global_progress_floor,
                   const char *stdin_password) {
  out->len = 0;
  if (out->data)
    out->data[0] = 0;
  if (cancel_requested())
    return 130;

  int read_fd = -1;
  pid_t pid = spawn_7z_child(args, argn, ctype_locale, stdin_password, &read_fd);
  if (pid < 0)
    return 127;
  set_active_7z_pid(pid);

  /* Always set non-blocking so that cancel checks via poll() work
     even for the nonblock=0 (no progress reporting) path. */
  {
    int flags = fcntl(read_fd, F_GETFL, 0);
    if (flags >= 0)
      fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);
  }

  int status = 0;
  int have_status = 0;
  int finished = 0;
  int last_local_pct = -1;
  int ui_display_pct = 0;  /* Smoothed progress value sent to UI */
  PercentParser pct_parser;
  percent_parser_init(&pct_parser);
  const size_t tail_cap = 32768;
  char *tail_buf = NULL;
  size_t tail_len = 0;
  size_t tail_write_pos = 0;
  int use_tail_ring = 0;
  if (nonblock) {
    tail_buf = (char *)malloc(tail_cap);
    if (tail_buf)
      use_tail_ring = 1;
  }

  while (!finished) {
    if (cancel_requested()) {
      terminate_pid(pid);
    }
    if (nonblock) {
      struct pollfd pfd = {.fd = read_fd, .events = POLLIN | POLLHUP};
      int pr = poll(&pfd, 1, 200);
      if (pr < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (pr == 0) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
          have_status = 1;
          finished = 1;
        }
        continue;
      }
      if (pfd.revents & (POLLIN | POLLHUP)) {
        while (1) {
          char tmp[4096];
          ssize_t n = read(read_fd, tmp, sizeof(tmp));
          if (n > 0) {
            size_t chunk = (size_t)n;
            if (use_tail_ring) {
              tail_ring_push(tail_buf, tail_cap, &tail_len, &tail_write_pos,
                             tmp, chunk);
            } else {
              if (!sb_append(out, tmp, chunk)) {
                finished = 1;
                break;
              }
            }
            percent_parser_feed(&pct_parser, tmp, chunk);
            continue;
          }
          if (n == 0) {
            finished = 1;
            break;
          }
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          if (errno == EINTR)
            continue;
          finished = 1;
          break;
        }
        int pct = pct_parser.last;
        if (pct >= 0 && progress_pipe) {
          if (pct > 99)
            pct = 99;

          int global_pct = (int)(start_pct + (pct * (slot_size / 100.0)));
          if (global_pct > 99)
            global_pct = 99;
          if (global_progress_floor && *global_progress_floor >= 0 &&
              global_pct < *global_progress_floor) {
            global_pct = *global_progress_floor;
          }

          /* Smoothing: gradually catch up to real progress.
             Apply smoothing BEFORE comparing with last_global_pct to ensure
             we output values incrementally even when 7z jumps from 0 to 60%. */
          int step = (global_pct - ui_display_pct) / 3;
          if (step < 1) step = 1;
          if (step > 10) step = 10;  /* Cap max step to 10% */

          int new_ui_pct = ui_display_pct + step;
          if (new_ui_pct > global_pct)
            new_ui_pct = global_pct;
          if (new_ui_pct > 99)
            new_ui_pct = 99;

          /* Only update if there's actual progress */
          if (new_ui_pct > ui_display_pct || pct != last_local_pct) {
            ui_display_pct = new_ui_pct;

            const char *label = (archive_label && *archive_label)
                                    ? archive_label
                                    : "未知文件";
            if (task_total > 1 && task_index > 0) {
              fprintf(progress_pipe,
                      "# [%d/%d] 解压进度: %d%% (当前: %s, %d%%)\n", task_index,
                      task_total, ui_display_pct, label, pct);
            } else {
              fprintf(progress_pipe, "# 解压进度: %d%% (当前: %s, %d%%)\n",
                      ui_display_pct, label, pct);
            }

            fprintf(progress_pipe, "%d\n", ui_display_pct);
            fflush(progress_pipe);

            last_local_pct = pct;
            if (global_progress_floor &&
                ui_display_pct > *global_progress_floor) {
              *global_progress_floor = ui_display_pct;
            }
          }
        }
      }
      if (!finished) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
          have_status = 1;
          finished = 1;
        }
      }
    } else {
      /* No progress reporting, but still use poll() so we can
         periodically check cancel_requested() instead of blocking
         indefinitely inside read(). */
      struct pollfd pfd = {.fd = read_fd, .events = POLLIN | POLLHUP};
      int pr = poll(&pfd, 1, 200);
      if (pr < 0) {
        if (errno != EINTR)
          finished = 1;
      } else if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
        while (1) {
          char tmp[4096];
          ssize_t n = read(read_fd, tmp, sizeof(tmp));
          if (n > 0) {
            if (!sb_append(out, tmp, (size_t)n))
              finished = 1;
            continue;
          }
          if (n == 0) {
            finished = 1;
            break;
          }
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          if (errno == EINTR)
            continue;
          finished = 1;
          break;
        }
      }
      /* pr == 0: poll timeout, loop back to cancel check */
    }
  }

  /* Drain remaining pipe data. If cancelled, skip to avoid blocking
     the GTK main thread in g_thread_join for longer than necessary. */
  if (!cancel_requested()) {
    while (1) {
      char tmp[4096];
      ssize_t n = read(read_fd, tmp, sizeof(tmp));
      if (n > 0) {
        if (use_tail_ring) {
          tail_ring_push(tail_buf, tail_cap, &tail_len, &tail_write_pos, tmp,
                         (size_t)n);
        } else {
          if (!sb_append(out, tmp, (size_t)n))
            break;
        }
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      break;
    }
  }
  close(read_fd);
  if (use_tail_ring) {
    (void)tail_ring_finalize(out, tail_buf, tail_cap, tail_len, tail_write_pos);
  }
  free(tail_buf);
  if (!have_status) {
    reap_7z_pid(pid, &status);
    have_status = 1;
  }
  clear_active_7z_pid(pid);
  if (WIFEXITED(status))
    return WEXITSTATUS(status);
  return 127;
}

int run_extract_for_file(const char *filepath, const char *outdir,
                         const char *pwd_bytes, const char *locale,
                         FILE *progress_pipe, double start_pct, double slot_size,
                         StrBuf *out, const char *archive_label, int task_index,
                         int task_total, int *global_progress_floor) {
  if (!filepath || !outdir || !out)
    return 127;

  char *args[16];
  int argn = 0;
  args[argn++] = "7z";
  args[argn++] = "x";
  args[argn++] = (char *)filepath;
  if (pwd_bytes && *pwd_bytes) {
    args[argn++] = "-p";
  }
  size_t outdir_len = strlen(outdir);
  if (outdir_len > (size_t)-1 - 3) {
    return 127;
  }
  char *o_arg = (char *)malloc(outdir_len + 3);
  if (!o_arg) {
    return 127;
  }
  snprintf(o_arg, outdir_len + 3, "-o%s", outdir);
  args[argn++] = o_arg;
  args[argn++] = "-y";
  // Route progress stream to stderr to reduce buffering stalls in non-TTY runs.
  args[argn++] = "-bsp2";
  args[argn++] = "-bb0";
  args[argn++] = "-mmt=on";
  int ec = run_7z_capture(args, argn, out, 1, progress_pipe, start_pct, slot_size,
                          locale, archive_label, task_index, task_total,
                          global_progress_floor,
                          (pwd_bytes && *pwd_bytes) ? pwd_bytes : NULL);
  free(o_arg);
  return ec;
}

int run_extract_gbk_zip_for_file(const char *filepath, const char *outdir,
                                 const char *pwd_bytes,
                                 FILE *progress_pipe, double start_pct,
                                 double slot_size, StrBuf *out,
                                 const char *archive_label, int task_index,
                                 int task_total,
                                 int *global_progress_floor) {
  if (!filepath || !outdir || !out)
    return 127;

  if (progress_pipe) {
    const char *label =
        (archive_label && *archive_label) ? archive_label : "未知文件";
    if (task_total > 1 && task_index > 0) {
      fprintf(progress_pipe, "# [%d/%d] 使用 GBK 文件名兼容模式: %s\n",
              task_index, task_total, label);
    } else {
      fprintf(progress_pipe, "# 使用 GBK 文件名兼容模式: %s\n", label);
    }
    fflush(progress_pipe);
  }

  char *args[10];
  int argn = 0;
  args[argn++] = "bsdtar";
  args[argn++] = "--options";
  args[argn++] = "hdrcharset=GBK";
  if (pwd_bytes && *pwd_bytes) {
    args[argn++] = "--passphrase";
    args[argn++] = (char *)pwd_bytes;
  }
  args[argn++] = "-xf";
  args[argn++] = (char *)filepath;
  args[argn++] = "-C";
  args[argn++] = (char *)outdir;
  args[argn++] = NULL;
  int ec = run_child_capture(args, out);
  if (ec == 0 && progress_pipe) {
    int done_pct = (int)(start_pct + slot_size);
    if (done_pct > 99)
      done_pct = 99;
    if (global_progress_floor && *global_progress_floor >= 0 &&
        done_pct < *global_progress_floor) {
      done_pct = *global_progress_floor;
    }
    fprintf(progress_pipe, "%d\n", done_pct);
    fflush(progress_pipe);
    if (global_progress_floor && done_pct > *global_progress_floor)
      *global_progress_floor = done_pct;
  }
  return ec;
}

static int first_file_from_bsdtar_listing(const char *listing, StrBuf *entry) {
  if (!listing || !entry)
    return 0;
  entry->len = 0;
  if (entry->data)
    entry->data[0] = 0;

  char *copy = str_dup(listing);
  if (!copy)
    return 0;
  int found = 0;
  char *saveptr = NULL;
  for (char *line = strtok_r(copy, "\n", &saveptr); line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' ||
                       line[len - 1] == '\t')) {
      line[--len] = 0;
    }
    while (*line == ' ' || *line == '\t')
      line++;
    if (!*line || (len > 0 && line[len - 1] == '/'))
      continue;
    found = sb_append(entry, line, strlen(line));
    break;
  }
  free(copy);
  return found;
}

int run_bsdtar_probe_password_for_file(const char *filepath,
                                       const char *pwd_bytes, StrBuf *out) {
  if (out) {
    out->len = 0;
    if (out->data)
      out->data[0] = 0;
  }
  if (!filepath || !*filepath || !pwd_bytes || !*pwd_bytes)
    return -1;
  if (cancel_requested())
    return -1;

  StrBuf listing;
  sb_init(&listing);
  char *list_args[] = {"bsdtar", "--options", "hdrcharset=GBK", "-tf",
                       (char *)filepath, NULL};
  int list_ec = run_child_capture(list_args, &listing);
  if (list_ec != 0) {
    if (out && listing.data)
      sb_append(out, listing.data, listing.len);
    sb_free(&listing);
    return -1;
  }

  StrBuf entry;
  sb_init(&entry);
  int have_entry = first_file_from_bsdtar_listing(listing.data, &entry);
  sb_free(&listing);
  if (!have_entry || !entry.data || !*entry.data) {
    sb_free(&entry);
    return -1;
  }

  char *args[] = {"bsdtar", "--passphrase", (char *)pwd_bytes, "--options",
                  "hdrcharset=GBK", "-xOf", (char *)filepath, entry.data,
                  NULL};
  int read_fd = -1;
  pid_t pid = spawn_capture_child(args, &read_fd);
  sb_free(&entry);
  if (pid < 0)
    return -1;
  set_active_7z_pid(pid);

  int flags = fcntl(read_fd, F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);

  int finished = 0;
  int status = 0;
  int have_status = 0;
  int early_ok = 0;
  size_t total_read = 0;
  static const size_t PROBE_OK_THRESHOLD = 65536;
  char tail[4096];
  size_t tail_used = 0;
  tail[0] = 0;

  while (!finished) {
    if (cancel_requested())
      terminate_pid(pid);

    struct pollfd pfd = {.fd = read_fd, .events = POLLIN | POLLHUP};
    int pr = poll(&pfd, 1, 200);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      finished = 1;
      break;
    }
    if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      while (1) {
        char tmp[4096];
        ssize_t n = read(read_fd, tmp, sizeof(tmp));
        if (n > 0) {
          size_t chunk = (size_t)n;
          rolling_append(tail, sizeof(tail), &tail_used, tmp, chunk);
          if (out)
            sb_append(out, tmp, chunk);
          total_read += chunk;
          if (total_read >= PROBE_OK_THRESHOLD) {
            early_ok = 1;
            terminate_pid(pid);
            finished = 1;
            break;
          }
          continue;
        }
        if (n == 0) {
          finished = 1;
          break;
        }
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        finished = 1;
        break;
      }
    }

    if (!finished) {
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        have_status = 1;
        finished = 1;
      }
    }
  }

  while (1) {
    char tmp[4096];
    ssize_t n = read(read_fd, tmp, sizeof(tmp));
    if (n > 0) {
      if (!early_ok) {
        rolling_append(tail, sizeof(tail), &tail_used, tmp, (size_t)n);
        if (out)
          sb_append(out, tmp, (size_t)n);
      }
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }
  close(read_fd);

  if (!have_status)
    reap_7z_pid(pid, &status);
  clear_active_7z_pid(pid);

  if (cancel_requested())
    return -1;
  if (early_ok)
    return 1;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return 1;
  if (need_password_from_output(tail))
    return 0;
  return -1;
}

int run_7z_probe_password_test(const char *filepath, const char *test_file,
                               const char *pwd_bytes, const char *locale,
                               int require_full_read) {
  if (!filepath || !*filepath || !test_file || !*test_file)
    return -1;
  if (cancel_requested())
    return -1;

  if (require_full_read) {
    int magic_rc =
        run_7z_probe_password_magic(filepath, test_file, pwd_bytes, locale);
    if (magic_rc == 0 || magic_rc == 1)
      return magic_rc;
  }

  char *args[14];
  int argn = 0;
  args[argn++] = "7z";
  args[argn++] = require_full_read ? "t" : "e";
  args[argn++] = (char *)filepath;
  args[argn++] = (char *)test_file;
  if (!require_full_read)
    args[argn++] = "-so";
  if (pwd_bytes && *pwd_bytes)
    args[argn++] = "-p";
  args[argn++] = "-bb0";
  args[argn++] = "-bd";
  args[argn++] = "-bso1";
  args[argn++] = "-bse2";
  args[argn++] = "-bsp0";
  args[argn++] = "-mmt=off";

  int read_fd = -1;
  const char *pwd = (pwd_bytes && *pwd_bytes) ? pwd_bytes : NULL;
  pid_t pid = spawn_7z_child(args, argn, locale, pwd, &read_fd);
  if (pid < 0)
    return -1;
  set_active_7z_pid(pid);

  int flags = fcntl(read_fd, F_GETFL, 0);
  if (flags >= 0)
    (void)fcntl(read_fd, F_SETFL, flags | O_NONBLOCK);

  int finished = 0;
  int status = 0;
  int have_status = 0;
  /* If 7z successfully outputs this many bytes, the password is correct.
     Wrong passwords cause 7z to exit almost immediately with an error,
     so reaching this threshold means decryption is working. */
  static const size_t PROBE_OK_THRESHOLD = 65536;
  size_t total_read = 0;
  int early_ok = 0;

  char tail[4096];
  size_t tail_used = 0;
  tail[0] = 0;

  while (!finished) {
    if (cancel_requested())
      terminate_pid(pid);

    struct pollfd pfd = {.fd = read_fd, .events = POLLIN | POLLHUP};
    int pr = poll(&pfd, 1, 200);
    if (pr < 0) {
      if (errno == EINTR)
        continue;
      finished = 1;
      break;
    }
    if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
      while (1) {
        char tmp[4096];
        ssize_t n = read(read_fd, tmp, sizeof(tmp));
        if (n > 0) {
          size_t chunk = (size_t)n;
          rolling_append(tail, sizeof(tail), &tail_used, tmp, chunk);
          total_read += chunk;
          if (!require_full_read && total_read >= PROBE_OK_THRESHOLD) {
            /* Enough data received — password is correct. Kill 7z early
               to avoid reading the entire (potentially huge) file. */
            early_ok = 1;
            terminate_pid(pid);
            finished = 1;
            break;
          }
          continue;
        }
        if (n == 0) {
          finished = 1;
          break;
        }
        if (errno == EINTR)
          continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
          break;
        finished = 1;
        break;
      }
    }

    if (!finished) {
      pid_t r = waitpid(pid, &status, WNOHANG);
      if (r == pid) {
        have_status = 1;
        finished = 1;
      }
    }
  }

  /* Drain remaining pipe data so 7z doesn't block on write. */
  while (1) {
    char tmp[4096];
    ssize_t n = read(read_fd, tmp, sizeof(tmp));
    if (n > 0) {
      if (!early_ok)
        rolling_append(tail, sizeof(tail), &tail_used, tmp, (size_t)n);
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }
  close(read_fd);

  if (!have_status) {
    reap_7z_pid(pid, &status);
  }
  clear_active_7z_pid(pid);

  if (cancel_requested())
    return -1;
  if (strstr(tail, "No files to process"))
    return -1;
  if (early_ok)
    return 1;
  if (!require_full_read && total_read == 0 &&
      WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return -1;
  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return 1;
  if (need_password_from_output(tail))
    return 0;
  return -1;
}

int archive_has_encrypted_content(const char *filepath) {
  int has_encrypted = 0;
  if (ensure_probe_target_cached(filepath, NULL, &has_encrypted, NULL,
                                 NULL) != 0)
    return -1;
  return has_encrypted ? 1 : 0;
}

int run_7z_probe_password_fast(const char *filepath, const char *pwd_bytes,
                               const char *locale, int use_multithread,
                               StrBuf *captured_listing, int blocking) {
  clear_captured_listing(captured_listing);
  if (!filepath)
    return -1;
  if (cancel_requested())
    return -1;

  char *args[8];
  int argn = 0;
  args[argn++] = "7z";
  args[argn++] = "l";
  args[argn++] = "-ba";
  args[argn++] = (char *)filepath;
  if (pwd_bytes && *pwd_bytes)
    args[argn++] = "-p";
  args[argn++] = "-bb0";
  args[argn++] = use_multithread ? "-mmt=on" : "-mmt=off";

  int read_fd = -1;
  const char *pwd = (pwd_bytes && *pwd_bytes) ? pwd_bytes : NULL;
  pid_t pid = spawn_7z_child(args, argn, locale, pwd, &read_fd);
  if (pid < 0)
    return -1;
  set_active_7z_pid(pid);

  char tail[4096];
  size_t tail_used = 0;
  tail[0] = 0;

  if (blocking) {
    /* Blocking read: fastest path for cracking worker subprocesses.
       No cancel check needed — parent kills the whole process group. */
    while (1) {
      char tmp[4096];
      ssize_t n = read(read_fd, tmp, sizeof(tmp));
      if (n > 0) {
        rolling_append(tail, sizeof(tail), &tail_used, tmp, (size_t)n);
        if (captured_listing)
          sb_append(captured_listing, tmp, (size_t)n);
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      break;
    }
  } else {
    /* Non-blocking poll loop: allows periodic cancel checks in GTK worker thread. */
    int fl = fcntl(read_fd, F_GETFL, 0);
    if (fl >= 0)
      (void)fcntl(read_fd, F_SETFL, fl | O_NONBLOCK);

    int probe_finished = 0;
    while (!probe_finished) {
      if (cancel_requested())
        terminate_pid(pid);
      struct pollfd pfd = {.fd = read_fd, .events = POLLIN | POLLHUP};
      int pr = poll(&pfd, 1, 200);
      if (pr < 0) {
        if (errno == EINTR)
          continue;
        break;
      }
      if (pr == 0)
        continue; /* timeout — re-check cancel */
      if (pfd.revents & (POLLIN | POLLHUP)) {
        while (1) {
          char tmp[4096];
          ssize_t n = read(read_fd, tmp, sizeof(tmp));
          if (n > 0) {
            rolling_append(tail, sizeof(tail), &tail_used, tmp, (size_t)n);
            if (captured_listing)
              sb_append(captured_listing, tmp, (size_t)n);
            continue;
          }
          if (n == 0) {
            probe_finished = 1;
            break;
          }
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
          if (errno == EINTR)
            continue;
          probe_finished = 1;
          break;
        }
      }
    }
  }
  close(read_fd);

  int status = 0;
  if (reap_7z_pid(pid, &status) < 0) {
    clear_active_7z_pid(pid);
    return -1;
  }
  clear_active_7z_pid(pid);

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
    if (!(pwd_bytes && *pwd_bytes))
      return 1;

    /* Check if headers are encrypted for this filepath.
       If headers are encrypted, "7z l -p<pwd>" succeeding is sufficient
       proof — wrong passwords cause immediate exit!=0, so no need for
       the expensive probe_password_test (which stalls on solid archives). */
    if (!g_probe_target_cache.headers_encrypted_known ||
        !g_probe_target_cache.filepath ||
        strcmp(g_probe_target_cache.filepath, filepath) != 0) {
      /* Run a no-password probe to determine header encryption status. */
      char *nopass_args[7];
      int nopass_argn = 0;
      nopass_args[nopass_argn++] = "7z";
      nopass_args[nopass_argn++] = "l";
      nopass_args[nopass_argn++] = "-ba";
      nopass_args[nopass_argn++] = (char *)filepath;
      nopass_args[nopass_argn++] = "-bb0";
      nopass_args[nopass_argn++] = "-mmt=off";
      int nopass_fd = -1;
      pid_t nopass_pid = spawn_7z_child(nopass_args, nopass_argn, NULL, NULL,
                                        &nopass_fd);
      int headers_enc = 0;
      if (nopass_pid > 0) {
        /* Drain output */
        while (1) {
          char tmp[4096];
          ssize_t n = read(nopass_fd, tmp, sizeof(tmp));
          if (n > 0) continue;
          if (n < 0 && errno == EINTR) continue;
          break;
        }
        close(nopass_fd);
        int nopass_st = 0;
        reap_7z_pid(nopass_pid, &nopass_st);
        /* Non-zero exit without password = headers encrypted */
        headers_enc = !(WIFEXITED(nopass_st) && WEXITSTATUS(nopass_st) == 0);
      }
      /* Cache the result (reuse filepath slot if same file) */
      if (!g_probe_target_cache.filepath ||
          strcmp(g_probe_target_cache.filepath, filepath) != 0) {
        probe_target_cache_reset();
        g_probe_target_cache.filepath = str_dup(filepath);
      }
      g_probe_target_cache.headers_encrypted = headers_enc;
      g_probe_target_cache.headers_encrypted_known = 1;
    }

    if (g_probe_target_cache.headers_encrypted) {
      /* Headers encrypted: "7z l" success with password is conclusive. */
      return 1;
    }

    int has_encrypted = 0;
    int require_full_read = 0;
    const char *test_file = NULL;
    if (ensure_probe_target_cached(filepath, pwd_bytes, &has_encrypted,
                                   &test_file, &require_full_read) != 0)
      return -1;

    if (!has_encrypted)
      return 1;
    if (!test_file || !*test_file)
      return -1;

    int test_rc = run_7z_probe_password_test(filepath, test_file, pwd_bytes,
                                             locale, require_full_read);
    if (test_rc == 1)
      return 1;
    clear_captured_listing(captured_listing);
    return test_rc;
  }
  /* Probe failed – discard any captured listing. */
  clear_captured_listing(captured_listing);
  if (need_password_from_output(tail))
    return 0;
  return -1;
}

int need_password_from_output(const char *out) {
  if (!out)
    return 0;
  const char *keys[] = {"Wrong password",    "Enter password",
                        "encrypted archive", "Can not open encrypted archive",
                        "Incorrect passphrase",
                        "Too many incorrect passphrases",
                        "CRC Failed",        "密码错误",
                        "需要密码",          "请输入密码",
                        "输入密码",          "加密文件",
                        "加密档案"};
  for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    if (strstr(out, keys[i]))
      return 1;
  }
  return 0;
}

/* Batch password verification: test multiple passwords in a single 7z process.
   Returns:
     >= 0: index of the first matching password in pwd_list
     -1: non-password error (file corrupt, etc.)
     -2: none of the passwords matched
*/
int run_7z_probe_password_batch(const char *filepath,
                                const char **pwd_bytes_list,
                                const char **locale_list, int count,
                                int *hit_index, int *attempted_count) {
  if (hit_index)
    *hit_index = -1;
  if (attempted_count)
    *attempted_count = 0;
  if (!filepath || !pwd_bytes_list || count <= 0)
    return -1;

  /* Try each password sequentially with fast probe */
  for (int i = 0; i < count; i++) {
    if (!pwd_bytes_list[i] || !*pwd_bytes_list[i])
      continue;

    const char *locale = (locale_list && locale_list[i]) ? locale_list[i] : NULL;
    if (attempted_count)
      (*attempted_count)++;
    int r = run_7z_probe_password_fast(filepath, pwd_bytes_list[i], locale, 0, NULL, 1);

    if (r == 1) {
      /* Password matched */
      if (hit_index)
        *hit_index = i;
      return i;
    }
    if (r < 0) {
      /* Non-password error */
      return -1;
    }
    /* r == 0: wrong password, continue to next */
  }

  /* None matched */
  return -2;
}
