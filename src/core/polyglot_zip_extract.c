#define _GNU_SOURCE
#include "polyglot_zip_extract.h"

#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

typedef struct {
  int fd;
  uint64_t zip_start;
  uint64_t zip_size;
  unsigned char *buf;
  size_t buf_size;
} ZipReader;

typedef struct {
  char *path;
  int is_dir;
} CreatedPath;

typedef struct {
  CreatedPath *items;
  size_t len;
  size_t cap;
} CreatedList;

static void append_errorf(StrBuf *out, const char *fmt, ...) {
  if (!out || !fmt)
    return;

  char tmp[512];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  size_t len = (size_t)n;
  if (len >= sizeof(tmp))
    len = sizeof(tmp) - 1;
  (void)sb_append(out, tmp, len);
  if (len == 0 || tmp[len - 1] != '\n')
    (void)sb_append_c(out, '\n');
}

static void created_list_free(CreatedList *created) {
  if (!created)
    return;
  for (size_t i = 0; i < created->len; i++)
    free(created->items[i].path);
  free(created->items);
  created->items = NULL;
  created->len = 0;
  created->cap = 0;
}

static int created_list_add(CreatedList *created, const char *path, int is_dir,
                            StrBuf *out) {
  if (!created)
    return 1;
  if (created->len == created->cap) {
    size_t newcap = created->cap ? created->cap * 2 : 16;
    if (newcap < created->cap) {
      append_errorf(out, "Too many ZIP outputs to track for rollback");
      return 0;
    }
    CreatedPath *tmp =
        (CreatedPath *)realloc(created->items, newcap * sizeof(*tmp));
    if (!tmp) {
      append_errorf(out, "Out of memory while tracking ZIP outputs");
      return 0;
    }
    created->items = tmp;
    created->cap = newcap;
  }

  char *copy = strdup(path);
  if (!copy) {
    append_errorf(out, "Out of memory while tracking ZIP output path");
    return 0;
  }
  created->items[created->len].path = copy;
  created->items[created->len].is_dir = is_dir;
  created->len++;
  return 1;
}

static void rollback_created_outputs(int root_fd, CreatedList *created) {
  if (!created)
    return;
  for (size_t i = created->len; i > 0; i--) {
    CreatedPath *item = &created->items[i - 1];
    (void)unlinkat(root_fd, item->path, item->is_dir ? AT_REMOVEDIR : 0);
  }
}

static la_ssize_t zip_read_cb(struct archive *archive, void *client_data,
                              const void **buffer) {
  (void)archive;
  ZipReader *zr = (ZipReader *)client_data;
  *buffer = zr->buf;

  off_t pos = lseek(zr->fd, 0, SEEK_CUR);
  if (pos < 0)
    return -1;
  if ((uint64_t)pos < zr->zip_start)
    return -1;

  uint64_t logical = (uint64_t)pos - zr->zip_start;
  if (logical >= zr->zip_size)
    return 0;

  uint64_t remaining = zr->zip_size - logical;
  size_t to_read = zr->buf_size;
  if (remaining < (uint64_t)to_read)
    to_read = (size_t)remaining;

  for (;;) {
    ssize_t n = read(zr->fd, zr->buf, to_read);
    if (n < 0 && errno == EINTR)
      continue;
    return n;
  }
}

static la_int64_t zip_seek_cb(struct archive *archive, void *client_data,
                              la_int64_t request, int whence) {
  (void)archive;
  ZipReader *zr = (ZipReader *)client_data;
  if (zr->zip_size > (uint64_t)INT64_MAX)
    return ARCHIVE_FATAL;

  int64_t base = 0;
  if (whence == SEEK_SET) {
    base = 0;
  } else if (whence == SEEK_CUR) {
    off_t pos = lseek(zr->fd, 0, SEEK_CUR);
    if (pos < 0 || (uint64_t)pos < zr->zip_start)
      return ARCHIVE_FATAL;
    uint64_t logical = (uint64_t)pos - zr->zip_start;
    if (logical > (uint64_t)INT64_MAX)
      return ARCHIVE_FATAL;
    base = (int64_t)logical;
  } else if (whence == SEEK_END) {
    base = (int64_t)zr->zip_size;
  } else {
    return ARCHIVE_FATAL;
  }

  if ((request > 0 && base > INT64_MAX - request) ||
      (request < 0 && base < INT64_MIN - request)) {
    return ARCHIVE_FATAL;
  }
  int64_t logical = base + request;
  if (logical < 0 || (uint64_t)logical > zr->zip_size)
    return ARCHIVE_FATAL;

  uint64_t real = zr->zip_start + (uint64_t)logical;
  if (real < zr->zip_start || real > (uint64_t)INT64_MAX)
    return ARCHIVE_FATAL;
  if (lseek(zr->fd, (off_t)real, SEEK_SET) < 0)
    return ARCHIVE_FATAL;
  return (la_int64_t)logical;
}

static int normalize_entry_path(const char *raw, char **normalized,
                                StrBuf *out) {
  *normalized = NULL;
  if (!raw || !*raw) {
    append_errorf(out, "ZIP entry has an empty path");
    return 0;
  }

  size_t len = strlen(raw);
  if ((raw[0] == '/') || (raw[0] == '\\') ||
      (len >= 2 && isalpha((unsigned char)raw[0]) && raw[1] == ':')) {
    append_errorf(out, "Unsafe ZIP entry path rejected: %s", raw);
    return 0;
  }

  char *path = (char *)malloc(len + 1);
  if (!path) {
    append_errorf(out, "Out of memory while normalizing ZIP entry path");
    return 0;
  }
  for (size_t i = 0; i < len; i++)
    path[i] = raw[i] == '\\' ? '/' : raw[i];
  path[len] = 0;

  while (len > 0 && path[len - 1] == '/')
    path[--len] = 0;
  if (len == 0 || path[0] == '/') {
    append_errorf(out, "Unsafe ZIP entry path rejected: %s", raw);
    free(path);
    return 0;
  }

  size_t comp_start = 0;
  while (comp_start <= len) {
    size_t comp_end = comp_start;
    while (comp_end < len && path[comp_end] != '/')
      comp_end++;
    size_t comp_len = comp_end - comp_start;
    if (comp_len == 0 ||
        (comp_len == 1 && path[comp_start] == '.') ||
        (comp_len == 2 && path[comp_start] == '.' &&
         path[comp_start + 1] == '.')) {
      append_errorf(out, "Unsafe ZIP entry path rejected: %s", raw);
      free(path);
      return 0;
    }
    if (comp_end == len)
      break;
    comp_start = comp_end + 1;
  }

  *normalized = path;
  return 1;
}

static int open_output_root(const char *outdir, StrBuf *out) {
  int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
  int fd = open(outdir, flags);
  if (fd < 0)
    append_errorf(out, "Failed to open output directory '%s': %s", outdir,
                  strerror(errno));
  return fd;
}

static int append_component_path(char **path, size_t *len, size_t *cap,
                                 const char *part, StrBuf *out) {
  size_t part_len = strlen(part);
  size_t extra = part_len + (*len > 0 ? 1U : 0U);
  if (extra > (size_t)-1 - *len - 1) {
    append_errorf(out, "ZIP output path is too long");
    return 0;
  }
  size_t need = *len + extra + 1;
  if (need > *cap) {
    size_t newcap = *cap ? *cap * 2 : 64;
    while (newcap < need) {
      if (newcap > (size_t)-1 / 2) {
        newcap = need;
        break;
      }
      newcap *= 2;
    }
    char *tmp = (char *)realloc(*path, newcap);
    if (!tmp) {
      append_errorf(out, "Out of memory while tracking output directory");
      return 0;
    }
    *path = tmp;
    *cap = newcap;
  }
  if (*len > 0)
    (*path)[(*len)++] = '/';
  memcpy(*path + *len, part, part_len);
  *len += part_len;
  (*path)[*len] = 0;
  return 1;
}

static int open_or_create_dir_path(int root_fd, const char *dir_path,
                                   CreatedList *created, StrBuf *out) {
  if (!dir_path || !*dir_path)
    return dup(root_fd);

  char *tmp = strdup(dir_path);
  if (!tmp) {
    append_errorf(out, "Out of memory while creating output directories");
    return -1;
  }

  int dir_fd = dup(root_fd);
  if (dir_fd < 0) {
    append_errorf(out, "Failed to duplicate output directory fd: %s",
                  strerror(errno));
    free(tmp);
    return -1;
  }

  char *current_path = NULL;
  size_t current_len = 0;
  size_t current_cap = 0;
  char *saveptr = NULL;
  for (char *part = strtok_r(tmp, "/", &saveptr); part;
       part = strtok_r(NULL, "/", &saveptr)) {
    if (!append_component_path(&current_path, &current_len, &current_cap, part,
                               out)) {
      close(dir_fd);
      free(current_path);
      free(tmp);
      return -1;
    }

    int made_dir = 0;
    if (mkdirat(dir_fd, part, 0777) == 0) {
      made_dir = 1;
    } else if (errno != EEXIST) {
      append_errorf(out, "Failed to create directory '%s': %s", dir_path,
                    strerror(errno));
      close(dir_fd);
      free(current_path);
      free(tmp);
      return -1;
    }

    if (made_dir && !created_list_add(created, current_path, 1, out)) {
      (void)unlinkat(dir_fd, part, AT_REMOVEDIR);
      close(dir_fd);
      free(current_path);
      free(tmp);
      return -1;
    }

    int next_fd =
        openat(dir_fd, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (next_fd < 0) {
      append_errorf(out, "Failed to open directory '%s': %s", dir_path,
                    strerror(errno));
      close(dir_fd);
      free(current_path);
      free(tmp);
      return -1;
    }
    close(dir_fd);
    dir_fd = next_fd;
  }

  free(current_path);
  free(tmp);
  return dir_fd;
}

static int open_parent_dir(int root_fd, const char *path, char **basename,
                           CreatedList *created, StrBuf *out) {
  *basename = NULL;
  const char *slash = strrchr(path, '/');
  if (!slash) {
    *basename = strdup(path);
    if (!*basename) {
      append_errorf(out, "Out of memory while opening output file");
      return -1;
    }
    return dup(root_fd);
  }

  size_t parent_len = (size_t)(slash - path);
  char *parent = strndup(path, parent_len);
  *basename = strdup(slash + 1);
  if (!parent || !*basename) {
    append_errorf(out, "Out of memory while opening output file");
    free(parent);
    free(*basename);
    *basename = NULL;
    return -1;
  }

  int fd = open_or_create_dir_path(root_fd, parent, created, out);
  free(parent);
  if (fd < 0) {
    free(*basename);
    *basename = NULL;
  }
  return fd;
}

static int pwrite_full(int fd, const void *buf, size_t len,
                       la_int64_t archive_offset, StrBuf *out) {
  if (archive_offset < 0 || (la_int64_t)(off_t)archive_offset != archive_offset) {
    append_errorf(out, "ZIP entry contains an invalid data offset");
    return 0;
  }

  const unsigned char *p = (const unsigned char *)buf;
  size_t left = len;
  off_t offset = (off_t)archive_offset;
  while (left > 0) {
    size_t chunk = left;
    if (chunk > (size_t)SSIZE_MAX)
      chunk = (size_t)SSIZE_MAX;
    ssize_t n = pwrite(fd, p, chunk, offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      append_errorf(out, "Failed to write extracted file data: %s",
                    n < 0 ? strerror(errno) : "short write");
      return 0;
    }
    p += n;
    left -= (size_t)n;
    offset += n;
  }
  return 1;
}

static int extract_regular_file(struct archive *archive,
                                struct archive_entry *entry, int root_fd,
                                const char *path, CreatedList *created,
                                StrBuf *out) {
  (void)entry;
  char *basename = NULL;
  int parent_fd = open_parent_dir(root_fd, path, &basename, created, out);
  if (parent_fd < 0)
    return 0;

  int file_fd =
      openat(parent_fd, basename,
             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0666);
  if (file_fd < 0) {
    append_errorf(out, "Failed to create extracted file '%s': %s", path,
                  strerror(errno));
    close(parent_fd);
    free(basename);
    return 0;
  }

  int ok = 1;
  for (;;) {
    const void *block = NULL;
    size_t size = 0;
    la_int64_t offset = 0;
    int r = archive_read_data_block(archive, &block, &size, &offset);
    if (r == ARCHIVE_EOF)
      break;
    if (r != ARCHIVE_OK) {
      append_errorf(out, "Failed to read ZIP entry '%s': %s", path,
                    archive_error_string(archive)
                        ? archive_error_string(archive)
                        : "archive read error");
      ok = 0;
      break;
    }
    if (!pwrite_full(file_fd, block, size, offset, out)) {
      ok = 0;
      break;
    }
  }

  if (close(file_fd) < 0) {
    append_errorf(out, "Failed to close extracted file '%s': %s", path,
                  strerror(errno));
    ok = 0;
  }
  if (!ok)
    (void)unlinkat(parent_fd, basename, 0);

  close(parent_fd);
  free(basename);
  if (ok && !created_list_add(created, path, 0, out)) {
    (void)unlinkat(root_fd, path, 0);
    return 0;
  }
  return ok;
}

static void emit_completion_progress(FILE *progress_pipe, double start_pct,
                                     double slot_size,
                                     int *global_progress_floor) {
  if (!progress_pipe)
    return;
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

int polyglot_extract_plain_zip(const char *src_path,
                               uint64_t zip_start,
                               const char *outdir,
                               FILE *progress_pipe,
                               double start_pct,
                               double slot_size,
                               StrBuf *out,
                               const char *archive_label,
                               int task_index,
                               int task_total,
                               int *global_progress_floor) {
  if (!src_path || !outdir) {
    append_errorf(out, "Invalid ZIP extraction arguments");
    return 127;
  }

  if (progress_pipe) {
    const char *label =
        (archive_label && *archive_label) ? archive_label : "未知文件";
    if (task_total > 1 && task_index > 0) {
      fprintf(progress_pipe, "# [%d/%d] 使用内置 ZIP 快速路径: %s\n",
              task_index, task_total, label);
    } else {
      fprintf(progress_pipe, "# 使用内置 ZIP 快速路径: %s\n", label);
    }
    fflush(progress_pipe);
  }

  int rc = 95;
  int src_fd = -1;
  int root_fd = -1;
  struct archive *archive = NULL;
  CreatedList created = {0};
  ZipReader zr;
  memset(&zr, 0, sizeof(zr));
  zr.fd = -1;
  zr.buf_size = 64 * 1024;

  src_fd = open(src_path, O_RDONLY | O_CLOEXEC);
  if (src_fd < 0) {
    append_errorf(out, "Failed to open source file '%s': %s", src_path,
                  strerror(errno));
    goto cleanup;
  }

  struct stat st;
  if (fstat(src_fd, &st) < 0) {
    append_errorf(out, "Failed to stat source file '%s': %s", src_path,
                  strerror(errno));
    goto cleanup;
  }
  if (st.st_size < 0 || zip_start > (uint64_t)st.st_size) {
    append_errorf(out, "Invalid ZIP offset for '%s'", src_path);
    goto cleanup;
  }

  zr.fd = src_fd;
  zr.zip_start = zip_start;
  zr.zip_size = (uint64_t)st.st_size - zip_start;
  if (zr.zip_size > (uint64_t)INT64_MAX) {
    append_errorf(out, "ZIP payload is too large for this extractor");
    goto cleanup;
  }
  zr.buf = (unsigned char *)malloc(zr.buf_size);
  if (!zr.buf) {
    append_errorf(out, "Out of memory while opening ZIP payload");
    goto cleanup;
  }

  root_fd = open_output_root(outdir, out);
  if (root_fd < 0)
    goto cleanup;

  archive = archive_read_new();
  if (!archive) {
    append_errorf(out, "Failed to allocate ZIP reader");
    goto cleanup;
  }
  if (archive_read_support_filter_none(archive) != ARCHIVE_OK ||
      archive_read_support_format_zip(archive) != ARCHIVE_OK ||
      archive_read_set_read_callback(archive, zip_read_cb) != ARCHIVE_OK ||
      archive_read_set_seek_callback(archive, zip_seek_cb) != ARCHIVE_OK ||
      archive_read_set_callback_data(archive, &zr) != ARCHIVE_OK) {
    append_errorf(out, "Failed to configure ZIP reader: %s",
                  archive_error_string(archive)
                      ? archive_error_string(archive)
                      : "archive configuration error");
    goto cleanup;
  }
  if (lseek(src_fd, (off_t)zip_start, SEEK_SET) < 0) {
    append_errorf(out, "Failed to seek to embedded ZIP payload: %s",
                  strerror(errno));
    goto cleanup;
  }
  if (archive_read_open1(archive) != ARCHIVE_OK) {
    append_errorf(out, "Failed to open embedded ZIP payload: %s",
                  archive_error_string(archive)
                      ? archive_error_string(archive)
                      : "archive open error");
    goto cleanup;
  }

  struct archive_entry *entry = NULL;
  for (;;) {
    int r = archive_read_next_header(archive, &entry);
    if (r == ARCHIVE_EOF)
      break;
    if (r != ARCHIVE_OK) {
      append_errorf(out, "Failed to read ZIP header: %s",
                    archive_error_string(archive)
                        ? archive_error_string(archive)
                        : "archive header error");
      goto cleanup;
    }

    int encrypted = archive_entry_is_encrypted(entry);
    if (encrypted != 0) {
      append_errorf(out, "Encrypted ZIP entry is not supported");
      goto cleanup;
    }

    const char *raw_path = archive_entry_pathname(entry);
    char *path = NULL;
    if (!normalize_entry_path(raw_path, &path, out))
      goto cleanup;

    mode_t filetype = archive_entry_filetype(entry);
    int ok = 0;
    if (filetype == AE_IFDIR) {
      int dir_fd = open_or_create_dir_path(root_fd, path, &created, out);
      if (dir_fd >= 0) {
        close(dir_fd);
        ok = 1;
      }
    } else if (filetype == AE_IFREG) {
      ok = extract_regular_file(archive, entry, root_fd, path, &created, out);
    } else {
      append_errorf(out, "Unsupported ZIP entry type for '%s'", path);
    }
    free(path);
    if (!ok)
      goto cleanup;
  }

  emit_completion_progress(progress_pipe, start_pct, slot_size,
                           global_progress_floor);
  rc = 0;

cleanup:
  if (rc != 0 && root_fd >= 0)
    rollback_created_outputs(root_fd, &created);
  if (archive)
    (void)archive_read_free(archive);
  created_list_free(&created);
  free(zr.buf);
  if (root_fd >= 0)
    close(root_fd);
  if (src_fd >= 0)
    close(src_fd);
  return rc;
}
