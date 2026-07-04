#define _GNU_SOURCE
#include "log.h"
#include "path.h"
#include "strbuf.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LOG_KEEP_DAYS 7
#define LOG_MAX_SIZE (10 * 1024 * 1024)
#define LOG_PRUNE_INTERVAL_SEC (24 * 3600)

static const char *log_file_path(void) {
  static char *cached = NULL;
  if (cached)
    return cached;

  const char *state_home = getenv("XDG_STATE_HOME");
  if (state_home && *state_home) {
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, state_home, strlen(state_home));
    sb_append(&sb, "/nautilus-toolkit/nautilus_toolkit.log",
              sizeof("/nautilus-toolkit/nautilus_toolkit.log") - 1);
    cached = sb.data;
    if (cached)
      return cached;
  }

  const char *home = getenv("HOME");
  if (home && *home) {
    StrBuf sb;
    sb_init(&sb);
    sb_append(&sb, home, strlen(home));
    sb_append(&sb, "/.local/state/nautilus-toolkit/nautilus_toolkit.log",
              sizeof("/.local/state/nautilus-toolkit/nautilus_toolkit.log") - 1);
    cached = sb.data;
    if (cached)
      return cached;
  }

  cached = str_dup("/tmp/nautilus_toolkit.log");
  return cached;
}

static FILE *log_stream = NULL;

static void ensure_log_parent_dir(void) {
  const char *path = log_file_path();
  char *dir = path_parent(path);
  if (dir) {
    mkdirs(dir);
    free(dir);
  }
}

static char *prune_stamp_path_for(const char *log_path) {
  if (!log_path)
    return NULL;
  StrBuf sb;
  sb_init(&sb);
  sb_append(&sb, log_path, strlen(log_path));
  sb_append(&sb, ".prune_stamp", sizeof(".prune_stamp") - 1);
  return sb.data;
}

static int should_prune_log_file(const char *path) {
  if (!path)
    return 0;
  struct stat st;
  if (stat(path, &st) != 0)
    return 0;
  if ((size_t)st.st_size > (size_t)LOG_MAX_SIZE)
    return 1;

  char *stamp = prune_stamp_path_for(path);
  if (!stamp)
    return 1;

  struct stat st_stamp;
  int need_prune = 1;
  if (stat(stamp, &st_stamp) == 0) {
    time_t now = time(NULL);
    if (now >= st_stamp.st_mtime &&
        (now - st_stamp.st_mtime) < (time_t)LOG_PRUNE_INTERVAL_SEC) {
      need_prune = 0;
    }
  }
  free(stamp);
  return need_prune;
}

static void touch_prune_stamp(const char *path) {
  char *stamp = prune_stamp_path_for(path);
  if (!stamp)
    return;
  FILE *f = fopen(stamp, "a");
  if (f) {
    fclose(f);
    chmod(stamp, 0600);
  }
  free(stamp);
}

static int parse_log_timestamp(const char *line, time_t *out_ts) {
  if (!line || line[0] != '[')
    return 0;
  if (strlen(line) < 21)
    return 0;
  char buf[20];
  memcpy(buf, line + 1, 19);
  buf[19] = 0;
  struct tm tmv;
  memset(&tmv, 0, sizeof(tmv));
  char *ret = strptime(buf, "%Y-%m-%d %H:%M:%S", &tmv);
  if (!ret || *ret != 0)
    return 0;
  *out_ts = mktime(&tmv);
  return 1;
}

void prune_log_file(void) {
  const char *path = log_file_path();
  ensure_log_parent_dir();
  if (!should_prune_log_file(path))
    return;

  FILE *f = fopen(path, "r");
  if (!f)
    return;

  time_t now = time(NULL);
  time_t cutoff = now - (time_t)(LOG_KEEP_DAYS * 24 * 3600);

  /* Pass 1: find the first line with a valid timestamp >= cutoff.
     We stream through the file without storing all lines in memory. */
  long keep_offset = -1;
  char *line = NULL;
  size_t n = 0;
  long offset = 0;
  while (getline(&line, &n, f) != -1) {
    time_t ts;
    if (parse_log_timestamp(line, &ts) && ts >= cutoff) {
      keep_offset = offset;
      break;
    }
    offset = ftell(f);
  }
  free(line);

  if (keep_offset < 0) {
    /* No lines to keep – truncate the file. */
    fclose(f);
    FILE *wf = fopen(path, "w");
    if (wf) {
      fclose(wf);
      chmod(path, 0600);
    }
    touch_prune_stamp(path);
    return;
  }

  /* Determine total remaining size from keep_offset to EOF. */
  fseek(f, 0, SEEK_END);
  long file_size = ftell(f);
  long remain = file_size - keep_offset;

  /* If remaining data exceeds LOG_MAX_SIZE, advance keep_offset further. */
  if (remain > (long)LOG_MAX_SIZE) {
    long skip = remain - (long)LOG_MAX_SIZE;
    keep_offset += skip;
    /* Align to next line boundary. */
    fseek(f, keep_offset, SEEK_SET);
    int ch;
    while ((ch = fgetc(f)) != EOF && ch != '\n')
      ;
    keep_offset = ftell(f);
  }

  /* Pass 2: copy from keep_offset to a temp file. */
  StrBuf tmp_path;
  sb_init(&tmp_path);
  sb_append(&tmp_path, path, strlen(path));
  sb_append(&tmp_path, ".tmp", 4);
  FILE *wf = tmp_path.data ? fopen(tmp_path.data, "w") : NULL;
  int pruned = 0;
  if (wf) {
    fseek(f, keep_offset, SEEK_SET);
    char buf[8192];
    size_t nr;
    while ((nr = fread(buf, 1, sizeof(buf), f)) > 0) {
      fwrite(buf, 1, nr, wf);
    }
    fclose(wf);
    if (rename(tmp_path.data, path) == 0) {
      chmod(path, 0600);
      pruned = 1;
    }
  }
  fclose(f);
  sb_free(&tmp_path);
  if (pruned)
    touch_prune_stamp(path);
}

static FILE *get_log_stream(void) {
  if (log_stream)
    return log_stream;
  const char *path = log_file_path();
  ensure_log_parent_dir();
  log_stream = fopen(path, "a");
  if (!log_stream)
    return NULL;
  chmod(path, 0600);
  setvbuf(log_stream, NULL, _IOLBF, 0);
  return log_stream;
}

void log_msg(const char *fmt, ...) {
  FILE *f = get_log_stream();
  if (!f)
    return;

  time_t now = time(NULL);
  struct tm *tm = localtime(&now);
  char ts[64];
  strftime(ts, sizeof(ts), "[%F %T] ", tm);
  fputs(ts, f);

  va_list ap;
  va_start(ap, fmt);
  vfprintf(f, fmt, ap);
  va_end(ap);
  fputc('\n', f);
  fflush(f);
}
