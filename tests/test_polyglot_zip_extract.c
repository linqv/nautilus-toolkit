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

static void expect_path_missing(const char *name, const char *path) {
  if (access(path, F_OK) != 0 && errno == ENOENT)
    return;
  fprintf(stderr, "%s: expected missing path %s\n", name, path);
  failures++;
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

typedef struct {
  const char *name;
  const char *text;
  uint16_t method;
  uint16_t flags;
} TestZipEntry;

static long write_zip_payload_entries(FILE *f, const TestZipEntry *entries,
                                      size_t entry_count) {
  long zip_start = ftell(f);
  long local_offsets[8];
  uint32_t crcs[8];
  size_t data_lens[8];

  if (entry_count > 8)
    return -1;

  for (size_t i = 0; i < entry_count; i++) {
    size_t name_len = strlen(entries[i].name);
    data_lens[i] = strlen(entries[i].text);
    crcs[i] = entries[i].method == 0
                  ? crc32_bytes((const unsigned char *)entries[i].text,
                                data_lens[i])
                  : 0;
    local_offsets[i] = ftell(f) - zip_start;

    put_le32(f, 0x04034b50U);
    put_le16(f, 20);
    put_le16(f, entries[i].flags);
    put_le16(f, entries[i].method);
    put_le16(f, 0);
    put_le16(f, 0);
    put_le32(f, crcs[i]);
    put_le32(f, (uint32_t)data_lens[i]);
    put_le32(f, (uint32_t)data_lens[i]);
    put_le16(f, (uint16_t)name_len);
    put_le16(f, 0);
    fwrite(entries[i].name, 1, name_len, f);
    fwrite(entries[i].text, 1, data_lens[i], f);
  }

  long cd_offset = ftell(f) - zip_start;
  for (size_t i = 0; i < entry_count; i++) {
    size_t name_len = strlen(entries[i].name);
    put_le32(f, 0x02014b50U);
    put_le16(f, 20);
    put_le16(f, 20);
    put_le16(f, entries[i].flags);
    put_le16(f, entries[i].method);
    put_le16(f, 0);
    put_le16(f, 0);
    put_le32(f, crcs[i]);
    put_le32(f, (uint32_t)data_lens[i]);
    put_le32(f, (uint32_t)data_lens[i]);
    put_le16(f, (uint16_t)name_len);
    put_le16(f, 0);
    put_le16(f, 0);
    put_le16(f, 0);
    put_le16(f, 0);
    put_le32(f, 0);
    put_le32(f, (uint32_t)local_offsets[i]);
    fwrite(entries[i].name, 1, name_len, f);
  }

  long cd_size = (ftell(f) - zip_start) - cd_offset;
  put_le32(f, 0x06054b50U);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le16(f, (uint16_t)entry_count);
  put_le16(f, (uint16_t)entry_count);
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

static char *make_polyglot_file_entries(const char *tag,
                                        const TestZipEntry *entries,
                                        size_t entry_count,
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
  long zip_start = write_zip_payload_entries(f, entries, entry_count);
  fclose(f);
  if (zip_start < 0) {
    unlink(tmpl);
    return NULL;
  }
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

static void test_failed_extraction_removes_prior_outputs(void) {
  TestZipEntry entries[] = {
      {"safe.txt", "safe\n", 0, 0},
      {"../evil.txt", "evil\n", 0, 0},
  };
  uint64_t zip_start = 0;
  char *src = make_polyglot_file_entries("rollback", entries, 2, &zip_start);
  char *parent = make_temp_dir("rollback-parent");

  char outdir[512];
  snprintf(outdir, sizeof(outdir), "%s/out", parent);
  expect_int("create rollback outdir", mkdir(outdir, 0700), 0);

  StrBuf out;
  sb_init(&out);
  int rc = polyglot_extract_plain_zip(src, zip_start, outdir, NULL, 0.0, 100.0,
                                      &out, "rollback.mp4", 1, 1, NULL);
  if (rc == 0) {
    fprintf(stderr, "rollback extraction: expected nonzero, got 0\n");
    failures++;
  }

  char safe_path[1024];
  snprintf(safe_path, sizeof(safe_path), "%s/safe.txt", outdir);
  expect_path_missing("rollback safe file", safe_path);

  char evil_path[512];
  snprintf(evil_path, sizeof(evil_path), "%s/evil.txt", parent);
  expect_path_missing("rollback outside file", evil_path);

  unlink(safe_path);
  unlink(evil_path);
  rmdir(outdir);
  rmdir(parent);
  unlink(src);
  sb_free(&out);
  free(src);
  free(parent);
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
  if (rc == 0) {
    fprintf(stderr, "parent traversal: expected nonzero, got 0\n");
    failures++;
  }
  expect_path_missing("outside traversal file", outside);

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
  if (rc == 0) {
    fprintf(stderr, "absolute path: expected nonzero, got 0\n");
    failures++;
  }
  expect_path_missing("absolute file", "/tmp/ntk-absolute-evil.txt");

  rmdir(outdir);
  unlink(src);
  sb_free(&out);
  free(src);
  free(outdir);
}

static void test_unsupported_method_fails_without_output(void) {
  uint64_t zip_start = 0;
  char *src = make_polyglot_file("unsupported", "unsupported.bin", "payload\n",
                                 99, &zip_start);
  char *outdir = make_temp_dir("unsupported-out");

  StrBuf out;
  sb_init(&out);
  int rc = polyglot_extract_plain_zip(src, zip_start, outdir, NULL, 0.0, 100.0,
                                      &out, "unsupported.mp4", 1, 1, NULL);
  if (rc == 0) {
    fprintf(stderr, "unsupported method: expected nonzero, got 0\n");
    failures++;
  }

  char extracted[512];
  snprintf(extracted, sizeof(extracted), "%s/unsupported.bin", outdir);
  expect_path_missing("unsupported output file", extracted);

  rmdir(outdir);
  unlink(src);
  sb_free(&out);
  free(src);
  free(outdir);
}

static void test_encrypted_flag_fails_without_output(void) {
  TestZipEntry entries[] = {
      {"secret.txt", "secret\n", 0, 1},
  };
  uint64_t zip_start = 0;
  char *src = make_polyglot_file_entries("encrypted", entries, 1, &zip_start);
  char *outdir = make_temp_dir("encrypted-out");

  StrBuf out;
  sb_init(&out);
  int rc = polyglot_extract_plain_zip(src, zip_start, outdir, NULL, 0.0, 100.0,
                                      &out, "encrypted.mp4", 1, 1, NULL);
  if (rc == 0) {
    fprintf(stderr, "encrypted flag: expected nonzero, got 0\n");
    failures++;
  }

  char extracted[512];
  snprintf(extracted, sizeof(extracted), "%s/secret.txt", outdir);
  expect_path_missing("encrypted output file", extracted);

  rmdir(outdir);
  unlink(src);
  sb_free(&out);
  free(src);
  free(outdir);
}

int main(void) {
  test_extracts_plain_zip_without_copy();
  test_extracts_nested_file();
  test_failed_extraction_removes_prior_outputs();
  test_rejects_parent_traversal();
  test_rejects_absolute_path();
  test_unsupported_method_fails_without_output();
  test_encrypted_flag_fails_without_output();
  return failures == 0 ? 0 : 1;
}
