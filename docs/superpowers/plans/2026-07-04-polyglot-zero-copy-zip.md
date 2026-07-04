# Polyglot ZIP Zero-Copy Extraction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a zero-copy extraction fast path for unencrypted MP4+ZIP polyglot archives.

**Architecture:** Keep the existing `7z` first attempt and copy-to-temp fallback. Add a small libarchive backend that exposes the original file as a logical ZIP stream starting at `zip_start`, then call it only from the existing non-password polyglot retry block.

**Tech Stack:** C17, CMake, libarchive, existing `StrBuf`, existing GTK worker progress pipe, existing CTest tests.

---

## File Structure

- Create `src/core/polyglot_zip_extract.h`: public declaration for the new plain ZIP polyglot extraction backend.
- Create `src/core/polyglot_zip_extract.c`: libarchive read/seek callbacks, safe ZIP path handling, extraction loop, progress/error reporting.
- Create `tests/test_polyglot_zip_extract.c`: synthetic MP4+ZIP fixtures and direct backend tests.
- Modify `CMakeLists.txt`: add `libarchive` dependency, build/link the new backend, add the new test target.
- Modify `src/ui_gtk/ui_gtk_extract.c`: include the new header and call the fast path before the existing temp-copy fallback.

## Task 1: Add Build Wiring And Backend API Shell

**Files:**
- Create: `src/core/polyglot_zip_extract.h`
- Create: `src/core/polyglot_zip_extract.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add libarchive dependency to CMake**

Modify `CMakeLists.txt` in the dependency block:

```cmake
pkg_check_modules(JSON       REQUIRED json-glib-1.0>=1.6)
pkg_check_modules(LIBARCHIVE REQUIRED libarchive>=3.6)
```

Add `src/core/polyglot_zip_extract.c` to the `nautilus-toolkit` source list after `src/core/polyglot.c`:

```cmake
    src/core/polyglot.c
    src/core/polyglot_zip_extract.c
```

Add libarchive include flags to `target_include_directories(nautilus-toolkit SYSTEM PRIVATE ...)`:

```cmake
    ${LIBARCHIVE_INCLUDE_DIRS}
```

Add libarchive compile flags to `target_compile_options(nautilus-toolkit PRIVATE ...)`:

```cmake
    ${LIBARCHIVE_CFLAGS_OTHER}
```

Add libarchive libraries to `target_link_libraries(nautilus-toolkit PRIVATE ...)`:

```cmake
    ${LIBARCHIVE_LIBRARIES}
```

- [ ] **Step 2: Create the public header**

Create `src/core/polyglot_zip_extract.h`:

```c
#pragma once

#include "strbuf.h"

#include <stdint.h>
#include <stdio.h>

/* Extract an unencrypted ZIP payload embedded at zip_start inside src_path.
   Returns 0 on success and nonzero on failure/unsupported input. */
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
                               int *global_progress_floor);
```

- [ ] **Step 3: Create a compile-only implementation shell**

Create `src/core/polyglot_zip_extract.c`:

```c
#define _GNU_SOURCE
#include "polyglot_zip_extract.h"

#include <archive.h>
#include <archive_entry.h>
#include <string.h>

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
  (void)src_path;
  (void)zip_start;
  (void)outdir;
  (void)progress_pipe;
  (void)start_pct;
  (void)slot_size;
  (void)archive_label;
  (void)task_index;
  (void)task_total;
  (void)global_progress_floor;
  if (out)
    sb_append(out, "polyglot ZIP fast path is not implemented",
              strlen("polyglot ZIP fast path is not implemented"));
  return 95;
}
```

- [ ] **Step 4: Configure and build to verify wiring**

Run:

```bash
cmake --build build --target nautilus-toolkit
```

Expected: build succeeds and links against libarchive.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt src/core/polyglot_zip_extract.h src/core/polyglot_zip_extract.c
git commit -m "Add polyglot ZIP extraction backend shell"
```

## Task 2: Add Synthetic Polyglot ZIP Tests

**Files:**
- Create: `tests/test_polyglot_zip_extract.c`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Add the test target to CMake**

Inside `if(BUILD_TESTING)`, after `test_polyglot`, add:

```cmake
    add_executable(test_polyglot_zip_extract
        tests/test_polyglot_zip_extract.c
        src/core/polyglot.c
        src/core/polyglot_zip_extract.c
        src/core/path.c
        src/core/strbuf.c
    )
    target_include_directories(test_polyglot_zip_extract PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core
    )
    target_include_directories(test_polyglot_zip_extract SYSTEM PRIVATE
        ${LIBARCHIVE_INCLUDE_DIRS}
    )
    target_compile_options(test_polyglot_zip_extract PRIVATE
        ${LIBARCHIVE_CFLAGS_OTHER}
    )
    target_link_libraries(test_polyglot_zip_extract PRIVATE
        ntk_warnings
        ${LIBARCHIVE_LIBRARIES}
    )
    add_test(NAME polyglot_zip_extract COMMAND test_polyglot_zip_extract)
```

- [ ] **Step 2: Write the failing extraction tests**

Create `tests/test_polyglot_zip_extract.c`:

```c
#define _GNU_SOURCE
#include "polyglot.h"
#include "polyglot_zip_extract.h"
#include "strbuf.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static void expect_int(const char *name, int actual, int expected) {
  if (actual == expected)
    return;
  fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
  failures++;
}

static void expect_file_text(const char *path, const char *expected) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    fprintf(stderr, "%s: failed to open: %s\n", path, strerror(errno));
    failures++;
    return;
  }
  char buf[256];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;
  if (strcmp(buf, expected) != 0) {
    fprintf(stderr, "%s: expected %s, got %s\n", path, expected, buf);
    failures++;
  }
}

static uint32_t crc32_bytes(const unsigned char *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (int bit = 0; bit < 8; bit++)
      crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
  }
  return crc ^ 0xFFFFFFFFU;
}

static void put_le16(FILE *f, uint16_t v) {
  fputc((int)(v & 0xff), f);
  fputc((int)((v >> 8) & 0xff), f);
}

static void put_le32(FILE *f, uint32_t v) {
  put_le16(f, (uint16_t)(v & 0xffff));
  put_le16(f, (uint16_t)((v >> 16) & 0xffff));
}

static void put_be32(FILE *f, uint32_t v) {
  fputc((int)((v >> 24) & 0xff), f);
  fputc((int)((v >> 16) & 0xff), f);
  fputc((int)((v >> 8) & 0xff), f);
  fputc((int)(v & 0xff), f);
}

static void write_mp4_prefix(FILE *f) {
  put_be32(f, 16);
  fwrite("ftypisom0000", 1, 12, f);
  put_be32(f, 8);
  fwrite("mdat", 1, 4, f);
}

static long write_zip_payload(FILE *f, const char *name, const char *text,
                              uint16_t method) {
  long zip_start = ftell(f);
  size_t name_len = strlen(name);
  size_t data_len = strlen(text);
  uint32_t crc = method == 0 ? crc32_bytes((const unsigned char *)text, data_len) : 0;

  long local_offset = ftell(f) - zip_start;
  put_le32(f, 0x04034b50U);
  put_le16(f, 20);
  put_le16(f, 0);
  put_le16(f, method);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le32(f, crc);
  put_le32(f, (uint32_t)data_len);
  put_le32(f, (uint32_t)data_len);
  put_le16(f, (uint16_t)name_len);
  put_le16(f, 0);
  fwrite(name, 1, name_len, f);
  fwrite(text, 1, data_len, f);

  long cd_offset = ftell(f) - zip_start;
  put_le32(f, 0x02014b50U);
  put_le16(f, 20);
  put_le16(f, 20);
  put_le16(f, 0);
  put_le16(f, method);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le32(f, crc);
  put_le32(f, (uint32_t)data_len);
  put_le32(f, (uint32_t)data_len);
  put_le16(f, (uint16_t)name_len);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le32(f, 0);
  put_le32(f, (uint32_t)local_offset);
  fwrite(name, 1, name_len, f);

  long cd_size = (ftell(f) - zip_start) - cd_offset;
  put_le32(f, 0x06054b50U);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le16(f, 1);
  put_le16(f, 1);
  put_le32(f, (uint32_t)cd_size);
  put_le32(f, (uint32_t)cd_offset);
  put_le16(f, 0);
  return zip_start;
}

static char *make_temp_dir(const char *tag) {
  char tmpl[256];
  snprintf(tmpl, sizeof(tmpl), "/tmp/ntk-polyglot-zip-%s-XXXXXX", tag);
  char *path = strdup(tmpl);
  return mkdtemp(path);
}

static char *make_polyglot_file(const char *tag, const char *entry,
                                const char *text, uint16_t method,
                                uint64_t *zip_start_out) {
  char tmpl[256];
  snprintf(tmpl, sizeof(tmpl), "/tmp/ntk-polyglot-zip-%s-XXXXXX.mp4", tag);
  int fd = mkstemps(tmpl, 4);
  if (fd < 0)
    return NULL;
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmpl);
    return NULL;
  }
  write_mp4_prefix(f);
  long zip_start = write_zip_payload(f, entry, text, method);
  fclose(f);
  if (zip_start_out)
    *zip_start_out = (uint64_t)zip_start;
  return strdup(tmpl);
}

static void test_extracts_plain_zip_without_copy(void) {
  uint64_t expected_start = 0;
  char *src = make_polyglot_file("plain", "hello.txt", "hello\n", 0,
                                 &expected_start);
  char *outdir = make_temp_dir("plain-out");
  uint64_t found_start = 0;
  expect_int("find zip start", polyglot_find_zip_start(src, &found_start), 1);
  expect_int("zip start value", (int)found_start, (int)expected_start);

  StrBuf out;
  sb_init(&out);
  int rc = polyglot_extract_plain_zip(src, found_start, outdir, NULL, 0.0, 100.0,
                                      &out, "plain.mp4", 1, 1, NULL);
  expect_int("plain extraction", rc, 0);

  char extracted[512];
  snprintf(extracted, sizeof(extracted), "%s/hello.txt", outdir);
  expect_file_text(extracted, "hello\n");

  sb_free(&out);
  unlink(extracted);
  rmdir(outdir);
  unlink(src);
  free(src);
  free(outdir);
}

static void test_extracts_nested_file(void) {
  uint64_t zip_start = 0;
  char *src = make_polyglot_file("nested", "dir/nested.txt", "nested\n", 0,
                                 &zip_start);
  char *outdir = make_temp_dir("nested-out");

  StrBuf out;
  sb_init(&out);
  int rc = polyglot_extract_plain_zip(src, zip_start, outdir, NULL, 0.0, 100.0,
                                      &out, "nested.mp4", 1, 1, NULL);
  expect_int("nested extraction", rc, 0);

  char extracted[512];
  snprintf(extracted, sizeof(extracted), "%s/dir/nested.txt", outdir);
  expect_file_text(extracted, "nested\n");

  unlink(extracted);
  char dirpath[512];
  snprintf(dirpath, sizeof(dirpath), "%s/dir", outdir);
  rmdir(dirpath);
  rmdir(outdir);
  unlink(src);
  sb_free(&out);
  free(src);
  free(outdir);
}

int main(void) {
  test_extracts_plain_zip_without_copy();
  test_extracts_nested_file();
  return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 3: Run the new test and verify it fails against the shell**

Run:

```bash
cmake --build build --target test_polyglot_zip_extract
ctest --test-dir build -R polyglot_zip_extract --output-on-failure
```

Expected: test binary builds; CTest fails because `polyglot_extract_plain_zip()` returns `95`.

Do not commit this failing state.

## Task 3: Implement Zero-Copy Libarchive Extraction

**Files:**
- Modify: `src/core/polyglot_zip_extract.c`
- Test: `tests/test_polyglot_zip_extract.c`

- [ ] **Step 1: Replace the shell with real callback-based extraction**

Replace `src/core/polyglot_zip_extract.c` with this implementation shape:

```c
#define _GNU_SOURCE
#include "polyglot_zip_extract.h"
#include "path.h"

#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

typedef struct {
  int fd;
  uint64_t zip_start;
  uint64_t zip_size;
  unsigned char *buf;
  size_t buf_size;
} PolyglotZipReader;

static void append_errorf(StrBuf *out, const char *fmt, ...) {
  if (!out || !fmt)
    return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0)
    return;
  size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
  if (out->len > 0)
    sb_append_c(out, '\n');
  sb_append(out, buf, len);
}

static la_ssize_t zip_read_cb(struct archive *a, void *client_data,
                              const void **buff) {
  (void)a;
  PolyglotZipReader *r = (PolyglotZipReader *)client_data;
  off_t real_pos = lseek(r->fd, 0, SEEK_CUR);
  if (real_pos < 0)
    return -1;
  uint64_t end = r->zip_start + r->zip_size;
  if ((uint64_t)real_pos < r->zip_start || (uint64_t)real_pos >= end)
    return 0;
  uint64_t remaining = end - (uint64_t)real_pos;
  size_t want = r->buf_size;
  if (remaining < (uint64_t)want)
    want = (size_t)remaining;
  ssize_t n = read(r->fd, r->buf, want);
  if (n < 0)
    return -1;
  *buff = r->buf;
  return (la_ssize_t)n;
}

static la_int64_t zip_seek_cb(struct archive *a, void *client_data,
                              la_int64_t request, int whence) {
  (void)a;
  PolyglotZipReader *r = (PolyglotZipReader *)client_data;
  int64_t logical = 0;
  if (whence == SEEK_SET) {
    logical = request;
  } else if (whence == SEEK_CUR) {
    off_t cur = lseek(r->fd, 0, SEEK_CUR);
    if (cur < 0 || (uint64_t)cur < r->zip_start)
      return -1;
    logical = (int64_t)((uint64_t)cur - r->zip_start) + request;
  } else if (whence == SEEK_END) {
    logical = (int64_t)r->zip_size + request;
  } else {
    return -1;
  }
  if (logical < 0 || (uint64_t)logical > r->zip_size)
    return -1;
  off_t real = (off_t)(r->zip_start + (uint64_t)logical);
  if (lseek(r->fd, real, SEEK_SET) < 0)
    return -1;
  return (la_int64_t)logical;
}

static int normalize_zip_path(const char *raw, char **out_rel) {
  if (!raw || !*raw || !out_rel)
    return -1;
  *out_rel = NULL;
  if (raw[0] == '/' || raw[0] == '\\')
    return -1;
  if (raw[0] && raw[1] == ':')
    return -1;

  size_t len = strlen(raw);
  while (len > 0 && (raw[len - 1] == '/' || raw[len - 1] == '\\'))
    len--;
  if (len == 0)
    return -1;

  char *rel = (char *)malloc(len + 1);
  if (!rel)
    return -1;
  for (size_t i = 0; i < len; i++)
    rel[i] = raw[i] == '\\' ? '/' : raw[i];
  rel[len] = 0;

  char *copy = str_dup(rel);
  if (!copy) {
    free(rel);
    return -1;
  }
  char *saveptr = NULL;
  for (char *part = strtok_r(copy, "/", &saveptr); part;
       part = strtok_r(NULL, "/", &saveptr)) {
    if (!*part || strcmp(part, ".") == 0 || strcmp(part, "..") == 0) {
      free(copy);
      free(rel);
      return -1;
    }
  }
  free(copy);
  *out_rel = rel;
  return 0;
}

static int mkdirat_if_needed(int dir_fd, const char *name) {
  if (mkdirat(dir_fd, name, 0755) == 0)
    return 0;
  if (errno == EEXIST)
    return 0;
  return -1;
}

static int open_parent_dir_for_entry(int root_fd, const char *rel,
                                     char **leaf_out) {
  char *copy = str_dup(rel);
  if (!copy)
    return -1;
  int current = dup(root_fd);
  if (current < 0) {
    free(copy);
    return -1;
  }
  char *saveptr = NULL;
  char *part = strtok_r(copy, "/", &saveptr);
  while (part) {
    char *next = strtok_r(NULL, "/", &saveptr);
    if (!next) {
      *leaf_out = str_dup(part);
      free(copy);
      return current;
    }
    if (mkdirat_if_needed(current, part) != 0) {
      close(current);
      free(copy);
      return -1;
    }
    int child = openat(current, part,
                       O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(current);
    if (child < 0) {
      free(copy);
      return -1;
    }
    current = child;
    part = next;
  }
  close(current);
  free(copy);
  return -1;
}

static int ensure_dir_entry(int root_fd, const char *rel) {
  char *leaf = NULL;
  int parent = open_parent_dir_for_entry(root_fd, rel, &leaf);
  if (parent < 0 || !leaf) {
    free(leaf);
    return -1;
  }
  int rc = mkdirat_if_needed(parent, leaf);
  close(parent);
  free(leaf);
  return rc;
}

static int extract_regular_entry(struct archive *a, struct archive_entry *entry,
                                 int root_fd, const char *rel, StrBuf *out) {
  char *leaf = NULL;
  int parent = open_parent_dir_for_entry(root_fd, rel, &leaf);
  if (parent < 0 || !leaf) {
    append_errorf(out, "无法创建输出路径: %s", rel);
    free(leaf);
    return -1;
  }
  mode_t mode = archive_entry_perm(entry) & 0777;
  if (mode == 0)
    mode = 0644;
  int fd = openat(parent, leaf,
                  O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode);
  if (fd < 0) {
    append_errorf(out, "无法写入文件: %s: %s", rel, strerror(errno));
    close(parent);
    free(leaf);
    return -1;
  }

  const void *block = NULL;
  size_t size = 0;
  la_int64_t offset = 0;
  for (;;) {
    int r = archive_read_data_block(a, &block, &size, &offset);
    if (r == ARCHIVE_EOF)
      break;
    if (r != ARCHIVE_OK) {
      append_errorf(out, "读取 ZIP 条目失败: %s: %s", rel,
                    archive_error_string(a));
      close(fd);
      unlinkat(parent, leaf, 0);
      close(parent);
      free(leaf);
      return -1;
    }
    if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
      append_errorf(out, "写入偏移失败: %s: %s", rel, strerror(errno));
      close(fd);
      unlinkat(parent, leaf, 0);
      close(parent);
      free(leaf);
      return -1;
    }
    const unsigned char *p = (const unsigned char *)block;
    size_t left = size;
    while (left > 0) {
      ssize_t wr = write(fd, p, left);
      if (wr <= 0) {
        append_errorf(out, "写入文件失败: %s: %s", rel, strerror(errno));
        close(fd);
        unlinkat(parent, leaf, 0);
        close(parent);
        free(leaf);
        return -1;
      }
      p += wr;
      left -= (size_t)wr;
    }
  }

  if (close(fd) != 0) {
    append_errorf(out, "关闭文件失败: %s: %s", rel, strerror(errno));
    unlinkat(parent, leaf, 0);
    close(parent);
    free(leaf);
    return -1;
  }
  close(parent);
  free(leaf);
  return 0;
}

static void report_done(FILE *progress_pipe, double start_pct, double slot_size,
                        int *global_progress_floor) {
  if (!progress_pipe)
    return;
  int done_pct = (int)(start_pct + slot_size);
  if (done_pct > 99)
    done_pct = 99;
  if (global_progress_floor && *global_progress_floor >= 0 &&
      done_pct < *global_progress_floor)
    done_pct = *global_progress_floor;
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
  if (!src_path || !*src_path || !outdir || !*outdir || !out || zip_start == 0)
    return 95;

  int fd = open(src_path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    append_errorf(out, "无法打开 polyglot 文件: %s", strerror(errno));
    return 95;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size < 0 || zip_start >= (uint64_t)st.st_size) {
    append_errorf(out, "无效的 polyglot ZIP 偏移");
    close(fd);
    return 95;
  }
  if (lseek(fd, (off_t)zip_start, SEEK_SET) < 0) {
    append_errorf(out, "无法定位 ZIP 起点: %s", strerror(errno));
    close(fd);
    return 95;
  }

  int root_fd = open(outdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root_fd < 0) {
    append_errorf(out, "无法打开输出目录: %s", strerror(errno));
    close(fd);
    return 95;
  }

  PolyglotZipReader reader = {
      .fd = fd,
      .zip_start = zip_start,
      .zip_size = (uint64_t)st.st_size - zip_start,
      .buf = (unsigned char *)malloc(64U * 1024U),
      .buf_size = 64U * 1024U,
  };
  if (!reader.buf) {
    close(root_fd);
    close(fd);
    return 95;
  }

  struct archive *a = archive_read_new();
  archive_read_support_filter_none(a);
  archive_read_support_format_zip(a);
  archive_read_set_read_callback(a, zip_read_cb);
  archive_read_set_seek_callback(a, zip_seek_cb);
  archive_read_set_callback_data(a, &reader);
  int rc = 95;
  if (archive_read_open1(a) != ARCHIVE_OK) {
    append_errorf(out, "libarchive 无法打开嵌入 ZIP: %s",
                  archive_error_string(a));
    goto done;
  }

  if (progress_pipe) {
    const char *label = (archive_label && *archive_label) ? archive_label : "未知文件";
    if (task_total > 1 && task_index > 0)
      fprintf(progress_pipe, "# [%d/%d] 使用 polyglot 快速模式: %s\n",
              task_index, task_total, label);
    else
      fprintf(progress_pipe, "# 使用 polyglot 快速模式: %s\n", label);
    fflush(progress_pipe);
  }

  struct archive_entry *entry = NULL;
  while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
    if (archive_entry_is_encrypted(entry)) {
      append_errorf(out, "嵌入 ZIP 条目已加密，跳过快速模式");
      rc = 95;
      goto done;
    }

    const char *raw_path = archive_entry_pathname(entry);
    char *rel = NULL;
    if (normalize_zip_path(raw_path, &rel) != 0) {
      append_errorf(out, "ZIP 条目路径不安全: %s", raw_path ? raw_path : "(null)");
      rc = 95;
      goto done;
    }

    mode_t type = archive_entry_filetype(entry);
    if (type == AE_IFDIR) {
      if (ensure_dir_entry(root_fd, rel) != 0) {
        append_errorf(out, "无法创建目录: %s: %s", rel, strerror(errno));
        free(rel);
        rc = 95;
        goto done;
      }
    } else if (type == AE_IFREG || type == 0) {
      if (extract_regular_entry(a, entry, root_fd, rel, out) != 0) {
        free(rel);
        rc = 95;
        goto done;
      }
    } else {
      append_errorf(out, "ZIP 条目类型不支持: %s", rel);
      free(rel);
      rc = 95;
      goto done;
    }
    free(rel);
  }

  if (archive_errno(a) != 0) {
    append_errorf(out, "读取 ZIP 目录失败: %s", archive_error_string(a));
    rc = 95;
    goto done;
  }

  report_done(progress_pipe, start_pct, slot_size, global_progress_floor);
  rc = 0;

done:
  archive_read_free(a);
  free(reader.buf);
  close(root_fd);
  close(fd);
  return rc;
}
```

- [ ] **Step 2: Run the extraction tests**

Run:

```bash
cmake --build build --target test_polyglot_zip_extract
ctest --test-dir build -R polyglot_zip_extract --output-on-failure
```

Expected: `polyglot_zip_extract` passes.

- [ ] **Step 3: Run existing polyglot retry tests**

Run:

```bash
ctest --test-dir build -R polyglot --output-on-failure
```

Expected: `polyglot` and `polyglot_zip_extract` pass when both match the regex, or rerun with exact names if needed:

```bash
ctest --test-dir build -R '^polyglot$|^polyglot_zip_extract$' --output-on-failure
```

- [ ] **Step 4: Commit**

```bash
git add src/core/polyglot_zip_extract.c tests/test_polyglot_zip_extract.c CMakeLists.txt
git commit -m "Add zero-copy polyglot ZIP extraction"
```

## Task 4: Add Safety Regression Tests

**Files:**
- Modify: `tests/test_polyglot_zip_extract.c`
- Modify: `src/core/polyglot_zip_extract.c`

- [ ] **Step 1: Add unsafe path, encrypted flag, and unsupported method tests**

First, change the fixture helper in `tests/test_polyglot_zip_extract.c` so tests can set ZIP general-purpose flags. Replace the `write_zip_payload()` function with:

```c
static long write_zip_payload_with_flags(FILE *f, const char *name,
                                         const char *text, uint16_t method,
                                         uint16_t flags) {
  long zip_start = ftell(f);
  size_t name_len = strlen(name);
  size_t data_len = strlen(text);
  uint32_t crc = method == 0 ? crc32_bytes((const unsigned char *)text, data_len) : 0;

  long local_offset = ftell(f) - zip_start;
  put_le32(f, 0x04034b50U);
  put_le16(f, 20);
  put_le16(f, flags);
  put_le16(f, method);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le32(f, crc);
  put_le32(f, (uint32_t)data_len);
  put_le32(f, (uint32_t)data_len);
  put_le16(f, (uint16_t)name_len);
  put_le16(f, 0);
  fwrite(name, 1, name_len, f);
  fwrite(text, 1, data_len, f);

  long cd_offset = ftell(f) - zip_start;
  put_le32(f, 0x02014b50U);
  put_le16(f, 20);
  put_le16(f, 20);
  put_le16(f, flags);
  put_le16(f, method);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le32(f, crc);
  put_le32(f, (uint32_t)data_len);
  put_le32(f, (uint32_t)data_len);
  put_le16(f, (uint16_t)name_len);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le32(f, 0);
  put_le32(f, (uint32_t)local_offset);
  fwrite(name, 1, name_len, f);

  long cd_size = (ftell(f) - zip_start) - cd_offset;
  put_le32(f, 0x06054b50U);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le16(f, 1);
  put_le16(f, 1);
  put_le32(f, (uint32_t)cd_size);
  put_le32(f, (uint32_t)cd_offset);
  put_le16(f, 0);
  return zip_start;
}

static long write_zip_payload(FILE *f, const char *name, const char *text,
                              uint16_t method) {
  return write_zip_payload_with_flags(f, name, text, method, 0);
}
```

Then append this helper after `make_polyglot_file()`:

```c
static char *make_polyglot_file_with_flags(const char *tag, const char *entry,
                                           const char *text, uint16_t method,
                                           uint16_t flags,
                                           uint64_t *zip_start_out) {
  char tmpl[256];
  snprintf(tmpl, sizeof(tmpl), "/tmp/ntk-polyglot-zip-%s-XXXXXX.mp4", tag);
  int fd = mkstemps(tmpl, 4);
  if (fd < 0)
    return NULL;
  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmpl);
    return NULL;
  }
  write_mp4_prefix(f);
  long zip_start = write_zip_payload_with_flags(f, entry, text, method, flags);
  fclose(f);
  if (zip_start_out)
    *zip_start_out = (uint64_t)zip_start;
  return strdup(tmpl);
}
```

Append these test functions before `main()` in `tests/test_polyglot_zip_extract.c`:

```c
static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static void test_rejects_parent_traversal(void) {
  uint64_t zip_start = 0;
  char *src = make_polyglot_file("traversal", "../evil.txt", "evil\n", 0,
                                 &zip_start);
  char *outdir = make_temp_dir("traversal-out");
  char outside[512];
  snprintf(outside, sizeof(outside), "%s/../evil.txt", outdir);
  unlink(outside);

  StrBuf out;
  sb_init(&out);
  int rc = polyglot_extract_plain_zip(src, zip_start, outdir, NULL, 0.0, 100.0,
                                      &out, "traversal.mp4", 1, 1, NULL);
  expect_int("parent traversal rejected", rc == 0 ? 0 : 1, 1);
  expect_int("outside file not created", file_exists(outside), 0);

  rmdir(outdir);
  unlink(src);
  sb_free(&out);
  free(src);
  free(outdir);
}

static void test_rejects_absolute_path(void) {
  uint64_t zip_start = 0;
  char *src = make_polyglot_file("absolute", "/tmp/ntk-absolute-evil.txt",
                                 "evil\n", 0, &zip_start);
  char *outdir = make_temp_dir("absolute-out");
  unlink("/tmp/ntk-absolute-evil.txt");

  StrBuf out;
  sb_init(&out);
  int rc = polyglot_extract_plain_zip(src, zip_start, outdir, NULL, 0.0, 100.0,
                                      &out, "absolute.mp4", 1, 1, NULL);
  expect_int("absolute path rejected", rc == 0 ? 0 : 1, 1);
  expect_int("absolute file not created", file_exists("/tmp/ntk-absolute-evil.txt"), 0);

  rmdir(outdir);
  unlink(src);
  sb_free(&out);
  free(src);
  free(outdir);
}

static void test_unsupported_method_fails(void) {
  uint64_t zip_start = 0;
  char *src = make_polyglot_file("unsupported", "unsupported.bin", "payload\n",
                                 99, &zip_start);
  char *outdir = make_temp_dir("unsupported-out");

  StrBuf out;
  sb_init(&out);
  int rc = polyglot_extract_plain_zip(src, zip_start, outdir, NULL, 0.0, 100.0,
                                      &out, "unsupported.mp4", 1, 1, NULL);
  expect_int("unsupported method rejected", rc == 0 ? 0 : 1, 1);

  char extracted[512];
  snprintf(extracted, sizeof(extracted), "%s/unsupported.bin", outdir);
  unlink(extracted);
  rmdir(outdir);
  unlink(src);
  sb_free(&out);
  free(src);
  free(outdir);
}

static void test_encrypted_flag_fails(void) {
  uint64_t zip_start = 0;
  char *src = make_polyglot_file_with_flags("encrypted", "secret.txt",
                                            "secret\n", 0, 1, &zip_start);
  char *outdir = make_temp_dir("encrypted-out");

  StrBuf out;
  sb_init(&out);
  int rc = polyglot_extract_plain_zip(src, zip_start, outdir, NULL, 0.0, 100.0,
                                      &out, "encrypted.mp4", 1, 1, NULL);
  expect_int("encrypted flag rejected", rc == 0 ? 0 : 1, 1);

  char extracted[512];
  snprintf(extracted, sizeof(extracted), "%s/secret.txt", outdir);
  unlink(extracted);
  rmdir(outdir);
  unlink(src);
  sb_free(&out);
  free(src);
  free(outdir);
}
```

Update `main()`:

```c
int main(void) {
  test_extracts_plain_zip_without_copy();
  test_extracts_nested_file();
  test_rejects_parent_traversal();
  test_rejects_absolute_path();
  test_unsupported_method_fails();
  test_encrypted_flag_fails();
  return failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run tests to verify safety behavior**

Run:

```bash
cmake --build build --target test_polyglot_zip_extract
ctest --test-dir build -R polyglot_zip_extract --output-on-failure
```

Expected: all six test functions pass, and unsupported or encrypted ZIP entries do not leave created output files behind.

- [ ] **Step 3: Commit**

```bash
git add tests/test_polyglot_zip_extract.c src/core/polyglot_zip_extract.c
git commit -m "Harden polyglot ZIP fast path extraction"
```

## Task 5: Integrate The Fast Path Into GTK Extraction

**Files:**
- Modify: `src/ui_gtk/ui_gtk_extract.c`
- Test: `tests/test_polyglot_zip_extract.c`

- [ ] **Step 1: Include the new backend header**

In `src/ui_gtk/ui_gtk_extract.c`, add the include next to the existing polyglot include:

```c
#include "../core/polyglot.h"
#include "../core/polyglot_zip_extract.h"
```

- [ ] **Step 2: Insert fast path before temp-copy fallback**

In the existing non-password polyglot retry block, after:

```c
          polyglot_retry_attempted = 1;
          uint64_t zip_start = 0;
          char *fixed_path = NULL;
```

replace the direct `polyglot_make_temp_fixed_zip()` call block with:

```c
          int find_rc = polyglot_find_zip_start(filepath, &zip_start);
          if (find_rc == 1 && zip_start > 0 && !use_password) {
            int fast_outdir_created_by_us = 0;
            int fast_outdir_existed_before = 0;
            if (prepare_extract_outdir(&outdir, parent, custom_dest,
                                       &fast_outdir_created_by_us,
                                       &fast_outdir_existed_before)) {
              pipe_writef(write_fd, "# 检测到 polyglot ZIP，使用快速模式\n");
              log_msg("Polyglot zero-copy ZIP fallback enabled: %s (zip_start=%llu, outdir_existed=%d)",
                      filepath, (unsigned long long)zip_start,
                      fast_outdir_existed_before);
              StrBuf fast_out;
              sb_init(&fast_out);
              int fast_ec = polyglot_extract_plain_zip(
                  filepath, zip_start, outdir, pipe_stream, start_pct, slot_size,
                  &fast_out, filename, i + 1, total, &progress_floor);
              if (fast_ec == 0) {
                log_msg("Polyglot zero-copy ZIP fallback succeeded: %s", filepath);
                success = 1;
                success_count++;
                sb_free(&fast_out);
                sb_free(&out);
                dir_snapshot_free(&outdir_snapshot);
                break;
              }
              log_msg("Polyglot zero-copy ZIP fallback failed: %s",
                      fast_out.data ? fast_out.data : "(no output)");
              if (fast_outdir_created_by_us)
                remove_tree(outdir, parent);
              sb_free(&fast_out);
            } else {
              log_msg("Polyglot zero-copy ZIP fallback skipped: cannot prepare outdir");
            }
          }

          int fix_rc =
              polyglot_make_temp_fixed_zip(filepath, &fixed_path, &zip_start);
```

Keep the existing `fix_rc == 1`, `fix_rc == 0`, and `fix_rc < 0` handling unchanged after this inserted fast path.

- [ ] **Step 3: Build the extension**

Run:

```bash
cmake --build build --target nautilus-toolkit
```

Expected: extension builds without warnings promoted to errors.

- [ ] **Step 4: Run targeted tests**

Run:

```bash
ctest --test-dir build -R '^polyglot$|^polyglot_zip_extract$' --output-on-failure
```

Expected: both tests pass.

- [ ] **Step 5: Commit**

```bash
git add src/ui_gtk/ui_gtk_extract.c
git commit -m "Use zero-copy extraction for plain polyglot ZIPs"
```

## Task 6: Full Verification

**Files:**
- No source edits expected.

- [ ] **Step 1: Run all tests**

Run:

```bash
ctest --test-dir build --output-on-failure
```

Expected: all tests pass.

- [ ] **Step 2: Build the main module**

Run:

```bash
cmake --build build --target nautilus-toolkit
```

Expected: build succeeds.

- [ ] **Step 3: Inspect final diff**

Run:

```bash
git status --short
git log --oneline -5
```

Expected: either a clean tree after task commits, or only intentional uncommitted verification artifacts. Recent commits should include the backend shell, extraction backend, hardening tests, and UI integration.

- [ ] **Step 4: Manual smoke fixture**

If a desktop smoke test is available, create an MP4+ZIP polyglot fixture using `tests/test_polyglot_zip_extract.c` logic or a small local helper, then extract it through the Nautilus UI. Expected: progress shows polyglot fast mode, output file appears, no `.nautilus-toolkit-polyglot-*` temporary copy remains.
