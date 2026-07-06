#define _GNU_SOURCE
#include "ui_gtk.h"
#include "../core/exec7z.h"
#include "../core/extract_util.h"
#include "../core/fsutil.h"
#include "../core/log.h"
#include "../core/path.h"
#include "../core/polyglot.h"
#include "../core/polyglot_zip_extract.h"
#include "../core/strbuf.h"

#include <errno.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Write a line to the progress pipe (thread-safe, no GTK calls). */
static void pipe_writef(int fd, const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  size_t len = (size_t)n;
  if (len >= sizeof(buf))
    len = sizeof(buf) - 1;
  size_t off = 0;
  while (off < len) {
    ssize_t w = write(fd, buf + off, len - off);
    if (w > 0) {
      off += (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR)
      continue;
    break;
  }
}

typedef struct {
  int write_fd;
  int task_index;
  int task_total;
  double start_pct;
  double slot_size;
  int last_global_pct;
  int last_local_pct;
} TryAllProgressCtx;

static void tryall_progress_cb(int attempted, int total, void *user_data) {
  TryAllProgressCtx *ctx = (TryAllProgressCtx *)user_data;
  if (!ctx || total <= 0)
    return;

  int local_pct = (attempted * 100) / total;
  if (local_pct < 0)
    local_pct = 0;
  if (local_pct > 100)
    local_pct = 100;

  int global_pct =
      (int)(ctx->start_pct + (local_pct * (ctx->slot_size / 100.0)));
  if (global_pct < 0)
    global_pct = 0;
  if (global_pct > 99)
    global_pct = 99;

  if (local_pct == ctx->last_local_pct && global_pct <= ctx->last_global_pct)
    return;

  if (ctx->task_total > 1) {
    pipe_writef(ctx->write_fd, "# [%d/%d] 撞库进度: %d%%\n", ctx->task_index,
                ctx->task_total, local_pct);
  } else {
    pipe_writef(ctx->write_fd, "# 撞库进度: %d%%\n", local_pct);
  }

  if (global_pct > ctx->last_global_pct) {
    pipe_writef(ctx->write_fd, "%d\n", global_pct);
    ctx->last_global_pct = global_pct;
  }
  ctx->last_local_pct = local_pct;
}

static void trim_ascii_inplace(char *s) {
  if (!s || !*s)
    return;
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t'))
    s[--n] = '\0';
  size_t i = 0;
  while (s[i] == ' ' || s[i] == '\t')
    i++;
  if (i > 0)
    memmove(s, s + i, strlen(s + i) + 1);
}

static char *extract_last_nonempty_line(const char *out) {
  if (!out || !*out)
    return NULL;
  char *copy = str_dup(out);
  if (!copy)
    return NULL;

  char *last = NULL;
  char *saveptr = NULL;
  for (char *line = strtok_r(copy, "\n", &saveptr); line;
       line = strtok_r(NULL, "\n", &saveptr)) {
    trim_ascii_inplace(line);
    if (*line)
      last = line;
  }

  char *ret = (last && *last) ? str_dup(last) : NULL;
  free(copy);
  return ret;
}

static char *compose_failure_reason(const char *out, int exit_code,
                                    int cancelled, int exhausted_passwords) {
  if (cancelled || exit_code == 130)
    return str_dup("用户取消了解压");
  if (exhausted_passwords)
    return str_dup("密码错误或未找到可用密码");

  if (out && *out) {
    if (extraction_error_may_need_password(out, 0))
      return str_dup("密码错误或需要密码");
    if (strstr(out, "No space left on device") ||
        strstr(out, "not enough space"))
      return str_dup("磁盘空间不足");
    if (strstr(out, "Permission denied") || strstr(out, "Access is denied"))
      return str_dup("权限不足，无法写入目标目录");
    if (strstr(out, "No such file or directory"))
      return str_dup("文件不存在或路径无效");
    if (strstr(out, "Is not archive") ||
        strstr(out, "Cannot open the file as"))
      return str_dup("不是可识别的压缩文件或格式不受支持");
    if (strstr(out, "Unexpected end of archive"))
      return str_dup("分卷不完整，缺少后续分卷文件");
    if (strstr(out, "Data Error") || strstr(out, "Headers Error") ||
        strstr(out, "Unexpected end of data") || strstr(out, "CRC Failed"))
      return str_dup("压缩包损坏或校验失败");

    char *line = extract_last_nonempty_line(out);
    if (line && *line)
      return line;
    free(line);
  }

  char buf[96];
  if (exit_code >= 0)
    snprintf(buf, sizeof(buf), "解压失败（7z 退出码 %d）", exit_code);
  else
    snprintf(buf, sizeof(buf), "解压失败（未知错误）");
  return str_dup(buf);
}

static void update_failure_reason(char **dst, const char *out, int exit_code,
                                  int cancelled, int exhausted_passwords) {
  if (!dst)
    return;
  char *msg =
      compose_failure_reason(out, exit_code, cancelled, exhausted_passwords);
  if (!msg)
    return;
  free(*dst);
  *dst = msg;
}

typedef struct {
  char **data;
  size_t len;
  size_t cap;
} DirSnapshot;

static void dir_snapshot_init(DirSnapshot *snap) {
  if (!snap)
    return;
  snap->data = NULL;
  snap->len = 0;
  snap->cap = 0;
}

static void dir_snapshot_free(DirSnapshot *snap) {
  if (!snap)
    return;
  for (size_t i = 0; i < snap->len; i++)
    free(snap->data[i]);
  free(snap->data);
  snap->data = NULL;
  snap->len = 0;
  snap->cap = 0;
}

static int dir_snapshot_push(DirSnapshot *snap, const char *name) {
  if (!snap || !name)
    return 0;
  if (snap->len == snap->cap) {
    size_t new_cap = snap->cap ? snap->cap * 2 : 32;
    char **tmp = (char **)realloc(snap->data, new_cap * sizeof(char *));
    if (!tmp)
      return 0;
    snap->data = tmp;
    snap->cap = new_cap;
  }
  snap->data[snap->len] = str_dup(name);
  if (!snap->data[snap->len])
    return 0;
  snap->len++;
  return 1;
}

static int dir_snapshot_contains(const DirSnapshot *snap, const char *name) {
  if (!snap || !name)
    return 0;
  for (size_t i = 0; i < snap->len; i++) {
    if (snap->data[i] && strcmp(snap->data[i], name) == 0)
      return 1;
  }
  return 0;
}

static int capture_dir_snapshot(const char *dirpath, DirSnapshot *snap) {
  if (!dirpath || !snap)
    return 0;
  DIR *dir = opendir(dirpath);
  if (!dir)
    return 0;

  struct dirent *de = NULL;
  while ((de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    if (!dir_snapshot_push(snap, de->d_name)) {
      closedir(dir);
      return 0;
    }
  }
  closedir(dir);
  return 1;
}

static int normalize_new_entries_from_snapshot(const char *dirpath,
                                               const char *locale,
                                               const DirSnapshot *before) {
  if (!dirpath || !locale || !*locale || !before)
    return 0;
  DIR *dir = opendir(dirpath);
  if (!dir)
    return 0;

  int renamed = 0;
  struct dirent *de = NULL;
  while ((de = readdir(dir)) != NULL) {
    if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
      continue;
    if (dir_snapshot_contains(before, de->d_name))
      continue;

    size_t dir_len = strlen(dirpath);
    size_t name_len = strlen(de->d_name);
    size_t need_sep = (dir_len > 0 && dirpath[dir_len - 1] != '/') ? 1u : 0u;
    char *path = (char *)malloc(dir_len + need_sep + name_len + 1);
    if (!path)
      continue;
    memcpy(path, dirpath, dir_len);
    if (need_sep)
      path[dir_len++] = '/';
    memcpy(path + dir_len, de->d_name, name_len);
    path[dir_len + name_len] = 0;

    renamed += normalize_tree_utf8_names(path, locale, 1);
    free(path);
  }

  closedir(dir);
  return renamed;
}

static char *path_with_numeric_suffix(const char *path, int suffix) {
  if (suffix <= 0)
    return str_dup(path);

  char buf[32];
  snprintf(buf, sizeof(buf), "_%d", suffix);

  StrBuf out;
  sb_init(&out);
  if (!sb_append(&out, path, strlen(path)) ||
      !sb_append(&out, buf, strlen(buf))) {
    sb_free(&out);
    return NULL;
  }
  return out.data;
}

static int prepare_extract_outdir(char **outdir, const char *parent,
                                  const char *custom_dest,
                                  int *created_by_us,
                                  int *existed_before) {
  if (created_by_us)
    *created_by_us = 0;
  if (existed_before)
    *existed_before = 0;
  if (!outdir || !*outdir)
    return 0;

  if (custom_dest || !parent || strcmp(*outdir, parent) == 0) {
    int existed = path_exists(*outdir);
    int rc = mkdirs(*outdir);
    if (rc != 0 && !path_exists(*outdir))
      return 0;
    if (existed_before)
      *existed_before = existed;
    return 1;
  }

  char *base = str_dup(*outdir);
  if (!base)
    return 0;

  for (int i = 0; i <= 10000; i++) {
    char *candidate = path_with_numeric_suffix(base, i);
    if (!candidate)
      break;

    if (mkdir(candidate, 0755) == 0) {
      free(*outdir);
      *outdir = candidate;
      free(base);
      if (created_by_us)
        *created_by_us = 1;
      return 1;
    }

    if (errno == EEXIST) {
      free(candidate);
      continue;
    }

    free(candidate);
    break;
  }

  free(base);
  return 0;
}

/* ── Worker thread function ── */
gpointer ui_gtk_worker_func(gpointer data) {
  ExtractionContext *ctx = (ExtractionContext *)data;
  int write_fd = ctx->pipe_write_fd;

  run_7z_bind_cancel_flag((const int *)&ctx->cancelled);
  ignore_sigpipe_once();

  /* Collect extraction parameters from the SETUP page.
     These were set before the thread was spawned. */
  int pwd_mode = (int)adw_combo_row_get_selected(ctx->combo_mode);
  char *manual_pwd = NULL;
  char *selected_pwd = NULL;

  if (pwd_mode == PWD_MODE_MANUAL) {
    const char *text =
        gtk_editable_get_text(GTK_EDITABLE(ctx->entry_pwd));
    if (text && *text)
      manual_pwd = str_dup(text);
  } else if (pwd_mode == PWD_MODE_SELECT) {
    guint sel = adw_combo_row_get_selected(ctx->combo_pwd_select);
    if (sel < ctx->password_lib.len && ctx->password_lib.data[sel].value)
      selected_pwd = str_dup(ctx->password_lib.data[sel].value);
  }

  /* Custom destination directory */
  const char *custom_dest = ctx->dest_dir;

  int total = ctx->total_files;
  int success_count = 0;
  int progress_floor = -1;
  const int tryall_jobs = 8;
  int dup_fd = dup(write_fd);
  FILE *pipe_stream = dup_fd >= 0 ? fdopen(dup_fd, "w") : NULL;
  if (pipe_stream)
    setvbuf(pipe_stream, NULL, _IOLBF, 0);
  else if (dup_fd >= 0)
    close(dup_fd);

  PasswordHitCache hit_cache;
  hit_cache_init(&hit_cache);

  char *cached_password = NULL;

  /* Allocate results array */
  ctx->results = (FileResult *)calloc((size_t)total, sizeof(FileResult));
  ctx->result_count = total;

  for (int i = 0; i < total && !g_atomic_int_get(&ctx->cancelled); i++) {
    const char *filepath = ctx->tasks.data[i];
    const char *archive_path = filepath;
    char *polyglot_temp_path = NULL;
    int polyglot_retry_attempted = 0;
    char *filename = path_filename(filepath);
    char *parent = path_parent(filepath);
    char *fingerprint = build_file_fingerprint(filepath);
    const char *history_pwd = hit_cache_get(&hit_cache, fingerprint);

    log_msg("Processing: %s", filename);

    /* Report current file to progress page */
    if (total > 1)
      pipe_writef(write_fd, "# [%d/%d] 正在分析: %s\n", i + 1, total,
                  filename);
    else
      pipe_writef(write_fd, "# 正在分析: %s\n", filename);

    int use_password = 0;
    char *password_to_use = NULL;
    int tried_cached = 0;
    int tried_history = 0;
    int tried_selected = 0;
    int tried_manual = 0;
    int tried_tryall = 0;
    double slot_size = 100.0 / total;
    double start_pct = (double)i * slot_size;
    char *outdir = NULL;
    int outdir_with_password = 0;
    int outdir_needs_password_recheck = 0;
    int legacy_gbk_zip = archive_has_legacy_gbk_zip_names(archive_path);
    int legacy_gbk_zip_needs_fallback = 0;
    int legacy_gbk_zip_can_extract_without_password = 0;
    int legacy_gbk_zip_waits_for_password = 0;
    int archive_waits_for_password = 0;
    int success = 0;
    char *failure_reason = NULL;
    int need_save_pwd = 0;
    char *final_password = NULL;
    char *extract_locale_used = NULL;

    /* Pre-probe: run a no-password 7z l -ba to capture the listing.
       ZIPs with clear headers may list successfully even when file contents
       are encrypted, so encryption is checked separately below. */
    StrBuf pre_listing;
    sb_init(&pre_listing);
    int pre_probe_rc = -1;
    if (!custom_dest) {
      pre_probe_rc = run_7z_probe_password_fast(archive_path, NULL, NULL, 0,
                                                 &pre_listing, 0);
      if (pre_probe_rc == 1 && pre_listing.data && pre_listing.len > 0) {
        /* Listing is available without a password; file contents may still
           be encrypted, so only use it for output-directory selection. */
        outdir = determine_output_dir_with_listing(
            filepath, pre_listing.data, 0, NULL);
        outdir_with_password = 0;
        outdir_needs_password_recheck = 0;
      } else if (pre_probe_rc == 0) {
        /* Archive needs a password – outdir will be determined later
           once we have a valid password. */
        outdir_needs_password_recheck = 1;
      }
    } else if (legacy_gbk_zip) {
      pre_probe_rc = run_7z_probe_password_fast(archive_path, NULL, NULL, 0,
                                                 &pre_listing, 0);
    }
    if (pre_probe_rc == 1 &&
        archive_needs_legacy_gbk_zip_fallback(archive_path, pre_listing.data)) {
      legacy_gbk_zip_needs_fallback = 1;
      int encrypted_rc = archive_has_encrypted_content(archive_path);
      if (encrypted_rc == 1) {
        legacy_gbk_zip_waits_for_password = 1;
        log_msg("Legacy GBK ZIP filename fallback needs passphrase: %s",
                filepath);
      } else if (encrypted_rc == 0) {
        legacy_gbk_zip_can_extract_without_password = 1;
        log_msg("Legacy GBK ZIP filename fallback enabled: %s", filepath);
      } else {
        log_msg("Legacy GBK ZIP encryption probe failed, deferring fallback: %s",
                filepath);
      }
    }
    sb_free(&pre_listing);

    if (cached_password && *cached_password) {
      password_to_use = str_dup(cached_password);
      if (password_to_use) {
        use_password = 1;
        tried_cached = 1;
      }
    } else if (history_pwd && *history_pwd) {
      password_to_use = str_dup(history_pwd);
      if (password_to_use) {
        use_password = 1;
        tried_history = 1;
      }
    } else if (pwd_mode == PWD_MODE_SELECT && selected_pwd && *selected_pwd) {
      password_to_use = str_dup(selected_pwd);
      if (password_to_use) {
        use_password = 1;
        tried_selected = 1;
      }
    } else if (pwd_mode == PWD_MODE_MANUAL && manual_pwd && *manual_pwd) {
      password_to_use = str_dup(manual_pwd);
      if (password_to_use) {
        use_password = 1;
        tried_manual = 1;
      }
    }

    if (!use_password) {
      if (pre_probe_rc == 0 || legacy_gbk_zip_waits_for_password) {
        archive_waits_for_password = 1;
      } else if (archive_needs_password_before_extract(archive_path)) {
        archive_waits_for_password = 1;
        log_msg("Archive encrypted content needs passphrase: %s", filepath);
      }
    }

    while (!g_atomic_int_get(&ctx->cancelled)) {
      const char *effective_password =
          use_password ? password_to_use : "";

      /* Determine output directory */
      if (!outdir) {
        if (custom_dest) {
          outdir = str_dup(custom_dest);
        } else if (effective_password && *effective_password) {
          /* We have a password but no outdir yet (encrypted archive).
             The probe phase will capture listing and recompute outdir below,
             so use a stem-based fallback for now. */
          char *stem = path_stem(filepath);
          if (stem && parent) {
            StrBuf tmp;
            sb_init(&tmp);
            sb_append(&tmp, parent, strlen(parent));
            sb_append_c(&tmp, '/');
            sb_append(&tmp, stem, strlen(stem));
            outdir = tmp.data;
          }
          free(stem);
          outdir_with_password = 0;
          outdir_needs_password_recheck = 1;
        } else {
          outdir = determine_output_dir(filepath, effective_password,
                                        &outdir_needs_password_recheck);
          outdir_with_password =
              (effective_password && *effective_password) ? 1 : 0;
        }
      } else if (!custom_dest && effective_password &&
                 *effective_password && !outdir_with_password &&
                 outdir_needs_password_recheck) {
        /* Outdir will be recomputed from the probe listing below
           once the password is verified – no need to fork 7z here. */
      }
      if (!outdir)
      {
        update_failure_reason(&failure_reason, NULL, -1,
                              g_atomic_int_get(&ctx->cancelled), 0);
        break;
      }

      if (total > 1)
        pipe_writef(write_fd, "# [%d/%d] 解压中: %s\n", i + 1, total,
                    filename);
      else
        pipe_writef(write_fd, "# 解压中: %s\n", filename);

      /* Attempt extraction – reset progress floor so that a previous
         failed-password attempt (which reports 0%→100% instantly) does
         not inflate the baseline for the real extraction. */
      progress_floor = (int)start_pct;
      int non_pwd_error = 0;
      int used_transcoded = 0;
      int last_ec = -1;
      StrBuf out;
      sb_init(&out);
      int outdir_created_by_us = 0;
      int outdir_existed_before = 0;
      DirSnapshot outdir_snapshot;
      int have_outdir_snapshot = 0;
      dir_snapshot_init(&outdir_snapshot);

      if (use_password && password_to_use && *password_to_use) {
        PwdVarVec vars;
        varvec_init(&vars);
        build_password_variants(password_to_use, &vars);

        /* Fast probe: verify password before full extraction. */
        int probe_hit = -1;
        StrBuf probe_listing;
        sb_init(&probe_listing);
        for (size_t k = 0; k < vars.len; k++) {
          if (!vars.data[k].bytes || g_atomic_int_get(&ctx->cancelled))
            continue;
          int pr = -1;
          if (legacy_gbk_zip_needs_fallback &&
              strcmp(archive_path, filepath) == 0) {
            pr = run_bsdtar_probe_password_for_file(
                archive_path, vars.data[k].bytes, NULL);
          } else {
            pr = run_7z_probe_password_fast(
                archive_path, vars.data[k].bytes, vars.data[k].locale, 1,
                &probe_listing, 0);
          }
          if (pr == 1) {
            probe_hit = (int)k;
            break;
          }
          if (pr < 0) {
            /* Non-password error during probe – treat as fatal. */
            non_pwd_error = 1;
            break;
          }
        }

        /* Use cached listing to recompute outdir if needed. */
        if (probe_hit >= 0 && !custom_dest &&
            outdir_needs_password_recheck &&
            probe_listing.data && probe_listing.len > 0) {
          char *recomputed = determine_output_dir_with_listing(
              filepath, probe_listing.data, 0, NULL);
          if (recomputed) {
            char *old_outdir = outdir;
            outdir = recomputed;
            free(old_outdir);
            outdir_with_password = 1;
            outdir_needs_password_recheck = 0;
          }
        }
        sb_free(&probe_listing);

        if (probe_hit >= 0 && !non_pwd_error &&
            !g_atomic_int_get(&ctx->cancelled)) {
          /* Only extract with the verified variant. */
          if (!prepare_extract_outdir(&outdir, parent, custom_dest,
                                      &outdir_created_by_us,
                                      &outdir_existed_before)) {
            sb_append(&out, "无法创建输出目录", strlen("无法创建输出目录"));
            non_pwd_error = 1;
          } else {
            if (vars.data[probe_hit].locale && vars.data[probe_hit].locale[0] &&
                outdir_existed_before) {
              have_outdir_snapshot =
                  capture_dir_snapshot(outdir, &outdir_snapshot);
            }
            pipe_writef(write_fd, "# RESET_PROGRESS\n");
            progress_floor = (int)start_pct;
            if (legacy_gbk_zip_needs_fallback &&
                strcmp(archive_path, filepath) == 0) {
              last_ec = run_extract_gbk_zip_for_file(
                  archive_path, outdir, vars.data[probe_hit].bytes,
                  pipe_stream, start_pct, slot_size, &out, filename, i + 1,
                  total, &progress_floor);
            } else {
              last_ec = run_extract_for_file(
                  archive_path, outdir, vars.data[probe_hit].bytes,
                  vars.data[probe_hit].locale,
                  pipe_stream, start_pct, slot_size, &out, filename, i + 1,
                  total, &progress_floor);
            }
            if (last_ec == 0) {
              success = 1;
              used_transcoded = vars.data[probe_hit].transcoded;
              free(extract_locale_used);
              extract_locale_used = str_dup(vars.data[probe_hit].locale);
            } else if (!extraction_error_may_need_password(
                           out.data ? out.data : "", 1)) {
              non_pwd_error = 1;
            }
          }
        } else if (probe_hit < 0 && !non_pwd_error) {
          /* Probe rejected all variants – password is wrong, skip
             expensive full extraction entirely. */
        }

        varvec_free(&vars);
      } else {
        if (archive_waits_for_password) {
          sb_append(&out, "需要密码", strlen("需要密码"));
          last_ec = 255;
        } else if (!prepare_extract_outdir(&outdir, parent, custom_dest,
                                           &outdir_created_by_us,
                                           &outdir_existed_before)) {
          sb_append(&out, "无法创建输出目录", strlen("无法创建输出目录"));
          non_pwd_error = 1;
        } else {
          pipe_writef(write_fd, "# RESET_PROGRESS\n");
          if (legacy_gbk_zip_needs_fallback &&
              legacy_gbk_zip_can_extract_without_password &&
              strcmp(archive_path, filepath) == 0) {
            last_ec = run_extract_gbk_zip_for_file(
                archive_path, outdir, NULL, pipe_stream, start_pct, slot_size,
                &out, filename, i + 1, total, &progress_floor);
          } else {
            last_ec = run_extract_for_file(archive_path, outdir, "", NULL,
                                           pipe_stream, start_pct, slot_size,
                                           &out, filename, i + 1, total,
                                           &progress_floor);
          }
          if (last_ec == 0)
            success = 1;
          else if (!extraction_error_may_need_password(
                       out.data ? out.data : "", 0))
            non_pwd_error = 1;
        }
      }

      if (success) {
        log_msg("Success: %s", filename);
        if (extract_locale_used && *extract_locale_used) {
          int renamed = 0;
          if (outdir_existed_before && have_outdir_snapshot)
            renamed = normalize_new_entries_from_snapshot(
                outdir, extract_locale_used, &outdir_snapshot);
          else
            renamed = normalize_tree_utf8_names(outdir, extract_locale_used, 0);
          if (renamed > 0)
            log_msg("Normalized %d extracted path names to UTF-8", renamed);
        }
        if (use_password && password_to_use && *password_to_use) {
          if (!cached_password || !*cached_password) {
            free(cached_password);
            cached_password = str_dup(password_to_use);
          }
          hit_cache_put(&hit_cache, fingerprint, password_to_use);
          bump_password_hit(&ctx->password_lib, password_to_use);
          if (used_transcoded)
            log_msg("Password matched via legacy encoding fallback");
          /* Check if this was a manual password that should be saved */
          if (manual_pwd && strcmp(password_to_use, manual_pwd) == 0)
            need_save_pwd = 1;
          final_password = str_dup(password_to_use);
        }
        success_count++;
        dir_snapshot_free(&outdir_snapshot);
        sb_free(&out);
        break;
      }

      log_msg("Failed: %s (exit=%d, non_pwd=%d, cancelled=%d)",
              filename, last_ec, non_pwd_error,
              g_atomic_int_get(&ctx->cancelled));
      if (out.data && out.len > 0) {
        size_t tail_len = out.len > 4096 ? 4096 : out.len;
        const char *tail = out.data + out.len - tail_len;
        log_msg("7z output(tail): %.*s", (int)tail_len, tail);
      }

      log_msg("worker: before remove_tree (outdir=%s)", outdir ? outdir : "(null)");
      /* Only clean up the auto-generated directory this attempt created. */
      if (outdir_created_by_us &&
          extract_failure_should_cleanup_output(
              non_pwd_error, g_atomic_int_get(&ctx->cancelled))) {
        remove_tree(outdir, parent);
      } else if (outdir_created_by_us) {
        log_msg("worker: preserving partial output after non-password error: %s",
                outdir ? outdir : "(null)");
      }
      log_msg("worker: after remove_tree");

      if (non_pwd_error) {
        log_msg("worker: non_pwd_error path, polyglot_retry=%d", polyglot_retry_attempted);
        update_failure_reason(&failure_reason, out.data ? out.data : "",
                              last_ec, g_atomic_int_get(&ctx->cancelled), 0);
        if (!polyglot_retry_attempted &&
            !g_atomic_int_get(&ctx->cancelled)) {
          /* 前置过滤：明确的非 polyglot 错误直接跳过检测 */
          const char *err = out.data ? out.data : "";
          int skip_polyglot = !polyglot_should_try_after_7z_error(err);
          if (skip_polyglot) {
            log_msg("Polyglot skipped due to non-polyglot error: %s", filepath);
            sb_free(&out);
            break;
          }
          polyglot_retry_attempted = 1;
          uint64_t zip_start = 0;
          char *fixed_path = NULL;
          int find_rc = polyglot_find_zip_start(filepath, &zip_start);
          if (find_rc == 1 && zip_start > 0 && use_password &&
              password_to_use && *password_to_use) {
            int fast_outdir_created_by_us = 0;
            int fast_outdir_existed_before = 0;
            if (prepare_extract_outdir(&outdir, parent, custom_dest,
                                       &fast_outdir_created_by_us,
                                       &fast_outdir_existed_before)) {
              pipe_writef(write_fd,
                          "# 检测到加密 polyglot ZIP，使用快速模式\n");
              log_msg("Polyglot encrypted ZIP zero-copy fallback enabled: %s "
                      "(zip_start=%llu, outdir_existed=%d)",
                      filepath, (unsigned long long)zip_start,
                      fast_outdir_existed_before);

              PwdVarVec vars;
              varvec_init(&vars);
              build_password_variants(password_to_use, &vars);
              int fast_hit = -1;
              StrBuf fast_out;
              sb_init(&fast_out);
              for (size_t k = 0; k < vars.len &&
                                 !g_atomic_int_get(&ctx->cancelled);
                   k++) {
                if (!vars.data[k].bytes)
                  continue;
                fast_out.len = 0;
                if (fast_out.data)
                  fast_out.data[0] = 0;
                int fast_ec = polyglot_extract_zip_with_password(
                    filepath, zip_start, outdir, vars.data[k].bytes,
                    pipe_stream, start_pct, slot_size, &fast_out, filename,
                    i + 1, total, &progress_floor);
                if (fast_ec == 0) {
                  fast_hit = (int)k;
                  break;
                }
              }

              if (fast_hit >= 0) {
                log_msg("Polyglot encrypted ZIP zero-copy fallback succeeded: %s",
                        filepath);
                success = 1;
                if (!cached_password || !*cached_password) {
                  free(cached_password);
                  cached_password = str_dup(password_to_use);
                }
                hit_cache_put(&hit_cache, fingerprint, password_to_use);
                bump_password_hit(&ctx->password_lib, password_to_use);
                if (vars.data[fast_hit].transcoded)
                  log_msg("Password matched via legacy encoding fallback");
                if (manual_pwd && strcmp(password_to_use, manual_pwd) == 0)
                  need_save_pwd = 1;
                final_password = str_dup(password_to_use);
                success_count++;
                varvec_free(&vars);
                sb_free(&fast_out);
                sb_free(&out);
                dir_snapshot_free(&outdir_snapshot);
                break;
              }

              log_msg("Polyglot encrypted ZIP zero-copy fallback failed: %s",
                      fast_out.data ? fast_out.data : "(no output)");
              varvec_free(&vars);
              sb_free(&fast_out);
              if (fast_outdir_created_by_us) {
                remove_tree(outdir, parent);
                free(outdir);
                outdir = NULL;
                outdir_with_password = 0;
                outdir_needs_password_recheck = 0;
              }
            } else {
              log_msg("Polyglot encrypted ZIP zero-copy fallback skipped: "
                      "cannot prepare outdir");
            }
          }
          if (find_rc == 1 && zip_start > 0 && !use_password) {
            int fast_outdir_created_by_us = 0;
            int fast_outdir_existed_before = 0;
            if (prepare_extract_outdir(&outdir, parent, custom_dest,
                                       &fast_outdir_created_by_us,
                                       &fast_outdir_existed_before)) {
              pipe_writef(write_fd, "# 检测到 polyglot ZIP，使用快速模式\n");
              log_msg("Polyglot zero-copy ZIP fallback enabled: %s "
                      "(zip_start=%llu, outdir_existed=%d)",
                      filepath, (unsigned long long)zip_start,
                      fast_outdir_existed_before);
              StrBuf fast_out;
              sb_init(&fast_out);
              int fast_ec = polyglot_extract_plain_zip(
                  filepath, zip_start, outdir, pipe_stream, start_pct,
                  slot_size, &fast_out, filename, i + 1, total,
                  &progress_floor);
              if (fast_ec == 0) {
                log_msg("Polyglot zero-copy ZIP fallback succeeded: %s",
                        filepath);
                success = 1;
                success_count++;
                sb_free(&fast_out);
                sb_free(&out);
                dir_snapshot_free(&outdir_snapshot);
                break;
              }
              log_msg("Polyglot zero-copy ZIP fallback failed: %s",
                      fast_out.data ? fast_out.data : "(no output)");
              if (fast_outdir_created_by_us) {
                remove_tree(outdir, parent);
                free(outdir);
                outdir = NULL;
                outdir_with_password = 0;
                outdir_needs_password_recheck = 0;
              }
              sb_free(&fast_out);
            } else {
              log_msg("Polyglot zero-copy ZIP fallback skipped: cannot prepare "
                      "outdir");
            }
          }
          if (find_rc == 1 && zip_start > 0 && !use_password &&
              !tried_tryall && pwd_mode == PWD_MODE_TRYALL &&
              !g_atomic_int_get(&ctx->cancelled)) {
            tried_tryall = 1;
            log_msg("worker: entering polyglot zip tryall for %s", filename);
            if (total > 1)
              pipe_writef(write_fd, "# [%d/%d] 正在撞库: %s\n", i + 1,
                          total, filename);
            else
              pipe_writef(write_fd, "# 正在撞库: %s\n", filename);

            TryAllProgressCtx tryall_prog = {
                .write_fd = write_fd,
                .task_index = i + 1,
                .task_total = total,
                .start_pct = start_pct,
                .slot_size = slot_size,
                .last_global_pct = -1,
                .last_local_pct = -1};
            char *hit = NULL;
            int r = try_password_list_polyglot_zip(
                filepath, zip_start, &ctx->password_lib, &hit,
                tryall_progress_cb, &tryall_prog);
            if (r == 1 && hit) {
              free(password_to_use);
              password_to_use = hit;
              hit = NULL;
              use_password = 1;

              if (!outdir) {
                if (custom_dest) {
                  outdir = str_dup(custom_dest);
                } else {
                  char *stem = path_stem(filepath);
                  if (stem && parent) {
                    StrBuf tmp;
                    sb_init(&tmp);
                    sb_append(&tmp, parent, strlen(parent));
                    sb_append_c(&tmp, '/');
                    sb_append(&tmp, stem, strlen(stem));
                    outdir = tmp.data;
                  }
                  free(stem);
                }
              }

              int fast_outdir_created_by_us = 0;
              int fast_outdir_existed_before = 0;
              if (prepare_extract_outdir(&outdir, parent, custom_dest,
                                         &fast_outdir_created_by_us,
                                         &fast_outdir_existed_before)) {
                pipe_writef(write_fd,
                            "# 密码库命中，使用 polyglot ZIP 快速模式\n");
                log_msg("Polyglot ZIP tryall hit, zero-copy extraction "
                        "enabled: %s (zip_start=%llu, outdir_existed=%d)",
                        filepath, (unsigned long long)zip_start,
                        fast_outdir_existed_before);

                PwdVarVec vars;
                varvec_init(&vars);
                build_password_variants(password_to_use, &vars);
                int fast_hit = -1;
                StrBuf fast_out;
                sb_init(&fast_out);
                for (size_t k = 0; k < vars.len &&
                                   !g_atomic_int_get(&ctx->cancelled);
                     k++) {
                  if (!vars.data[k].bytes)
                    continue;
                  fast_out.len = 0;
                  if (fast_out.data)
                    fast_out.data[0] = 0;
                  int fast_ec = polyglot_extract_zip_with_password(
                      filepath, zip_start, outdir, vars.data[k].bytes,
                      pipe_stream, start_pct, slot_size, &fast_out, filename,
                      i + 1, total, &progress_floor);
                  if (fast_ec == 0) {
                    fast_hit = (int)k;
                    break;
                  }
                }

                if (fast_hit >= 0) {
                  log_msg("Polyglot ZIP tryall zero-copy extraction "
                          "succeeded: %s",
                          filepath);
                  success = 1;
                  if (!cached_password || !*cached_password) {
                    free(cached_password);
                    cached_password = str_dup(password_to_use);
                  }
                  hit_cache_put(&hit_cache, fingerprint, password_to_use);
                  bump_password_hit(&ctx->password_lib, password_to_use);
                  if (vars.data[fast_hit].transcoded)
                    log_msg("Password matched via legacy encoding fallback");
                  if (manual_pwd && strcmp(password_to_use, manual_pwd) == 0)
                    need_save_pwd = 1;
                  final_password = str_dup(password_to_use);
                  success_count++;
                  varvec_free(&vars);
                  sb_free(&fast_out);
                  sb_free(&out);
                  dir_snapshot_free(&outdir_snapshot);
                  break;
                }

                log_msg("Polyglot ZIP tryall zero-copy extraction failed: %s",
                        fast_out.data ? fast_out.data : "(no output)");
                varvec_free(&vars);
                sb_free(&fast_out);
                if (fast_outdir_created_by_us) {
                  remove_tree(outdir, parent);
                  free(outdir);
                  outdir = NULL;
                  outdir_with_password = 0;
                  outdir_needs_password_recheck = 0;
                }
              } else {
                log_msg("Polyglot ZIP tryall zero-copy extraction skipped: "
                        "cannot prepare outdir");
              }
            } else if (r == 0) {
              log_msg("Polyglot ZIP tryall found no matching password: %s",
                      filepath);
              free(hit);
              update_failure_reason(&failure_reason, "需要密码", 255,
                                    g_atomic_int_get(&ctx->cancelled), 1);
              sb_free(&out);
              dir_snapshot_free(&outdir_snapshot);
              break;
            } else {
              log_msg("Polyglot ZIP tryall probe failed, falling back to "
                      "temporary fixed archive: %s",
                      filepath);
              free(hit);
            }
          }

          int fix_rc = polyglot_make_temp_fixed_archive(filepath, &fixed_path,
                                                        &zip_start);
          if (fix_rc == 1 && fixed_path) {
            pipe_writef(write_fd, "# 检测到 polyglot，自动重试\n");
            log_msg("Polyglot fallback enabled: %s -> %s (zip_start=%llu)",
                    filepath, fixed_path, (unsigned long long)zip_start);
            archive_path = fixed_path;
            polyglot_temp_path = fixed_path;
            dir_snapshot_free(&outdir_snapshot);
            sb_free(&out);
            continue;
          }
          if (fix_rc == 0) {
            log_msg("Polyglot fallback not applicable: %s", filepath);
          } else {
            log_msg("Polyglot fallback failed: %s", filepath);
            polyglot_cleanup_temp(&fixed_path);
          }
        }
        sb_free(&out);
        dir_snapshot_free(&outdir_snapshot);
        log_msg("worker: breaking from non_pwd_error");
        break;
      }

      /* Password wasn't provided or was wrong — try fallbacks */
      log_msg("worker: entering password fallback (cancelled=%d)",
              g_atomic_int_get(&ctx->cancelled));
      if (!use_password)
        use_password = 1;

      /* Try cached password */
      if (cached_password && *cached_password && !tried_cached) {
        tried_cached = 1;
        free(password_to_use);
        password_to_use = str_dup(cached_password);
        sb_free(&out);
        continue;
      }

      /* Try history password */
      if (history_pwd && *history_pwd && !tried_history) {
        tried_history = 1;
        free(password_to_use);
        password_to_use = str_dup(history_pwd);
        sb_free(&out);
        continue;
      }

      /* Try user-selected password (from SETUP page) */
      if (pwd_mode == PWD_MODE_SELECT && selected_pwd && !tried_selected) {
        tried_selected = 1;
        free(password_to_use);
        password_to_use = str_dup(selected_pwd);
        sb_free(&out);
        continue;
      }

      /* Try manual password */
      if (pwd_mode == PWD_MODE_MANUAL && manual_pwd && !tried_manual) {
        tried_manual = 1;
        free(password_to_use);
        password_to_use = str_dup(manual_pwd);
        sb_free(&out);
        continue;
      }

      /* Try all passwords from library */
      if ((pwd_mode == PWD_MODE_TRYALL ||
           (pwd_mode == PWD_MODE_SELECT && tried_selected)) &&
          !tried_tryall) {
        tried_tryall = 1;
        log_msg("worker: entering tryall for %s", filename);
        if (total > 1)
          pipe_writef(write_fd, "# [%d/%d] 正在撞库: %s\n", i + 1, total,
                      filename);
        else
          pipe_writef(write_fd, "# 正在撞库: %s\n", filename);
        TryAllProgressCtx tryall_prog = {
            .write_fd = write_fd,
            .task_index = i + 1,
            .task_total = total,
            .start_pct = start_pct,
            .slot_size = slot_size,
            .last_global_pct = -1,
            .last_local_pct = -1};
        char *hit = NULL;
        StrBuf tryall_listing;
        sb_init(&tryall_listing);
        int r = try_password_list(archive_path, &ctx->password_lib, &hit,
                                  tryall_jobs, &tryall_listing,
                                  tryall_progress_cb, &tryall_prog);
        if (r == 1 && hit) {
          free(password_to_use);
          password_to_use = hit;
          /* Recompute outdir using cached listing from probe. */
          if (!custom_dest && outdir_needs_password_recheck &&
              tryall_listing.data && tryall_listing.len > 0) {
            char *recomputed = determine_output_dir_with_listing(
                filepath, tryall_listing.data, 0, NULL);
            if (recomputed) {
              char *old_outdir = outdir;
              outdir = recomputed;
              free(old_outdir);
              outdir_with_password = 1;
              outdir_needs_password_recheck = 0;
            }
          }
          sb_free(&tryall_listing);
          dir_snapshot_free(&outdir_snapshot);
          sb_free(&out);
          continue;
        }
        sb_free(&tryall_listing);
        free(hit);
        if (r == -1) {
          update_failure_reason(&failure_reason,
                                "密码库探测失败", -1,
                                g_atomic_int_get(&ctx->cancelled), 0);
          sb_free(&out);
          dir_snapshot_free(&outdir_snapshot);
          break;
        }
      }

      /* All password strategies exhausted */
      update_failure_reason(&failure_reason, out.data ? out.data : "", last_ec,
                            g_atomic_int_get(&ctx->cancelled), 1);
      sb_free(&out);
      dir_snapshot_free(&outdir_snapshot);
      break;
    }

    if (success) {
      free(failure_reason);
      failure_reason = NULL;
    } else if (!failure_reason) {
      update_failure_reason(&failure_reason, NULL, -1,
                            g_atomic_int_get(&ctx->cancelled), 0);
    }

    /* Store result */
    ctx->results[i].filename = str_dup(filename);
    ctx->results[i].outdir = success ? str_dup(outdir) : NULL;
    ctx->results[i].success = success;
    ctx->results[i].failure_reason = success ? NULL : failure_reason;
    ctx->results[i].need_save_pwd = need_save_pwd;
    ctx->results[i].used_password = final_password;

    /* Track passwords to save */
    if (need_save_pwd && manual_pwd) {
      /* Check if already tracked */
      int already = 0;
      for (int j = 0; j < ctx->saved_password_count; j++) {
        if (strcmp(ctx->saved_passwords[j], manual_pwd) == 0) {
          already = 1;
          break;
        }
      }
      if (!already) {
        char **new_saved_passwords = (char **)realloc(
            ctx->saved_passwords,
            (size_t)(ctx->saved_password_count + 1) * sizeof(char *));
        if (new_saved_passwords) {
          char *saved_pwd = str_dup(manual_pwd);
          ctx->saved_passwords = new_saved_passwords;
          if (saved_pwd) {
            ctx->saved_passwords[ctx->saved_password_count] = saved_pwd;
            ctx->saved_password_count++;
          }
        } else {
          log_msg("Failed to grow saved password list");
        }
      }
    }

    log_msg("worker: file %d/%d done (success=%d)", i + 1, total, success);
    free(outdir);
    polyglot_cleanup_temp(&polyglot_temp_path);
    free(password_to_use);
    free(extract_locale_used);
    if (success)
      free(failure_reason);
    free(parent);
    free(fingerprint);
    free(filename);
  }

  ctx->success_count = success_count;

  log_msg("worker: extraction loop done, closing pipe_stream");
  if (pipe_stream)
    fclose(pipe_stream);

  log_msg("worker: pipe_stream closed, sending DONE");
  /* Signal completion via pipe (may silently fail if cancelled and
     the UI thread already closed the fd — that is expected). */
  pipe_writef(write_fd, "100\n");
  pipe_writef(write_fd, "# DONE\n");
  /* Only close if UI thread hasn't already closed it during cancel.
     The UI thread is blocked in g_thread_join at this point, so
     reading ctx->pipe_write_fd here is safe (no concurrent mutation). */
  if (ctx->pipe_write_fd >= 0) {
    close(write_fd);
    ctx->pipe_write_fd = -1;
  }

  log_msg("worker: cleanup");
  free(cached_password);
  free(manual_pwd);
  free(selected_pwd);
  hit_cache_free(&hit_cache);
  run_7z_unbind_cancel_flag();

  log_msg("worker: exit");
  return NULL;
}
