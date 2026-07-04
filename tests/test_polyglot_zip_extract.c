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
