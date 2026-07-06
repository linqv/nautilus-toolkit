#define _GNU_SOURCE
#include "polyglot.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static void expect_int(const char *name, int actual, int expected) {
  if (actual == expected)
    return;
  fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
  failures++;
}

static void put_be32(FILE *f, uint32_t v) {
  fputc((int)((v >> 24) & 0xff), f);
  fputc((int)((v >> 16) & 0xff), f);
  fputc((int)((v >> 8) & 0xff), f);
  fputc((int)(v & 0xff), f);
}

static void put_le16(FILE *f, uint16_t v) {
  fputc((int)(v & 0xff), f);
  fputc((int)((v >> 8) & 0xff), f);
}

static void put_le32(FILE *f, uint32_t v) {
  fputc((int)(v & 0xff), f);
  fputc((int)((v >> 8) & 0xff), f);
  fputc((int)((v >> 16) & 0xff), f);
  fputc((int)((v >> 24) & 0xff), f);
}

static void put_le64(FILE *f, uint64_t v) {
  for (int i = 0; i < 8; i++)
    fputc((int)((v >> (i * 8)) & 0xff), f);
}

static long write_mp4_prefix(FILE *f) {
  put_be32(f, 16);
  fwrite("ftypisom0000", 1, 12, f);
  put_be32(f, 8);
  fwrite("mdat", 1, 4, f);
  return ftell(f);
}

static void write_zip_local_header(FILE *f, uint16_t version_needed,
                                   uint16_t flags, uint16_t method,
                                   uint32_t compressed_size,
                                   uint32_t uncompressed_size,
                                   const char *name,
                                   const unsigned char *extra,
                                   uint16_t extra_len,
                                   const unsigned char *data,
                                   size_t data_len) {
  put_le32(f, 0x04034b50u);
  put_le16(f, version_needed);
  put_le16(f, flags);
  put_le16(f, method);
  put_le16(f, 0);
  put_le16(f, 0);
  put_le32(f, 0);
  put_le32(f, compressed_size);
  put_le32(f, uncompressed_size);
  put_le16(f, (uint16_t)strlen(name));
  put_le16(f, extra_len);
  fwrite(name, 1, strlen(name), f);
  if (extra_len > 0)
    fwrite(extra, 1, extra_len, f);
  if (data_len > 0)
    fwrite(data, 1, data_len, f);
}

static void test_ignores_false_mp4_tail_zip_signature(void) {
  char tmpl[] = "/tmp/ntk-polyglot-false-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    fprintf(stderr, "mkstemp failed\n");
    failures++;
    return;
  }

  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmpl);
    fprintf(stderr, "fdopen failed\n");
    failures++;
    return;
  }

  put_be32(f, 16);
  fwrite("ftypisom0000", 1, 12, f);
  put_be32(f, 8);
  fwrite("mdat", 1, 4, f);

  static const unsigned char fake_local[] = {
      'P', 'K', 0x03, 0x04, 0xd9, 0x5f, 0x2a, 0x13,
      0x8c, 0x1c, 0xde, 0x1d, 0x84, 0xb8, 0xb9, 0x22,
      0x7b, 0x35, 0x4f, 0x59, 0xd1, 0xc3, 0x59, 0xb1,
      0xa2, 0xd6, 0xc1, 0x98, 0x52, 0x9a, 0xef, 0xd6,
  };
  fwrite(fake_local, 1, sizeof(fake_local), f);
  fclose(f);

  uint64_t zip_start = 123;
  int rc = polyglot_find_zip_start(tmpl, &zip_start);
  expect_int("false MP4 tail ZIP signature ignored", rc, 0);
  expect_int("false MP4 tail ZIP start reset", (int)zip_start, 0);

  unlink(tmpl);
}

static void test_finds_winzip_aes_zip_method(void) {
  char tmpl[] = "/tmp/ntk-polyglot-aes-zip-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    fprintf(stderr, "mkstemp failed\n");
    failures++;
    return;
  }

  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmpl);
    fprintf(stderr, "fdopen failed\n");
    failures++;
    return;
  }

  long zip_start = write_mp4_prefix(f);
  static const unsigned char aes_extra[] = {
      0x01, 0x99, 0x07, 0x00, 0x02, 0x00, 'A', 'E', 0x03, 0x08, 0x00};
  static const unsigned char payload[] = {0xde, 0xad, 0xbe, 0xef};
  write_zip_local_header(f, 20, 1, 99, sizeof(payload), sizeof(payload),
                         "207.zip", aes_extra, sizeof(aes_extra), payload,
                         sizeof(payload));
  fclose(f);

  uint64_t found = 0;
  expect_int("WinZip AES ZIP method detected",
             polyglot_find_zip_start(tmpl, &found), 1);
  expect_int("WinZip AES ZIP start", (int)found, (int)zip_start);

  unlink(tmpl);
}

static void test_finds_zip64_local_header_sizes(void) {
  char tmpl[] = "/tmp/ntk-polyglot-zip64-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    fprintf(stderr, "mkstemp failed\n");
    failures++;
    return;
  }

  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmpl);
    fprintf(stderr, "fdopen failed\n");
    failures++;
    return;
  }

  long zip_start = write_mp4_prefix(f);
  unsigned char zip64_extra[20];
  zip64_extra[0] = 0x01;
  zip64_extra[1] = 0x00;
  zip64_extra[2] = 0x10;
  zip64_extra[3] = 0x00;
  for (size_t i = 4; i < sizeof(zip64_extra); i++)
    zip64_extra[i] = 0;
  FILE *mem = fmemopen(zip64_extra + 4, sizeof(zip64_extra) - 4, "wb");
  if (!mem) {
    fclose(f);
    unlink(tmpl);
    fprintf(stderr, "fmemopen failed\n");
    failures++;
    return;
  }
  put_le64(mem, 4);
  put_le64(mem, 4);
  fclose(mem);

  static const unsigned char payload[] = {0xde, 0xad, 0xbe, 0xef};
  write_zip_local_header(f, 45, 0x0800, 8, 0xffffffffu, 0xffffffffu,
                         "[3D] demo.zip", zip64_extra, sizeof(zip64_extra),
                         payload, sizeof(payload));
  fclose(f);

  uint64_t found = 0;
  expect_int("ZIP64 local header sizes detected",
             polyglot_find_zip_start(tmpl, &found), 1);
  expect_int("ZIP64 ZIP start", (int)found, (int)zip_start);

  unlink(tmpl);
}

static void test_finds_rar5_polyglot_archive_start(void) {
  char tmpl[] = "/tmp/ntk-polyglot-rar5-XXXXXX";
  int fd = mkstemp(tmpl);
  if (fd < 0) {
    fprintf(stderr, "mkstemp failed\n");
    failures++;
    return;
  }

  FILE *f = fdopen(fd, "wb");
  if (!f) {
    close(fd);
    unlink(tmpl);
    fprintf(stderr, "fdopen failed\n");
    failures++;
    return;
  }

  put_be32(f, 16);
  fwrite("ftypisom0000", 1, 12, f);
  put_be32(f, 8);
  fwrite("mdat", 1, 4, f);
  long rar_start = ftell(f);
  static const unsigned char rar5_tail[] = {
      'R', 'a', 'r', '!', 0x1a, 0x07, 0x01, 0x00,
      0xce, 0xf0, 0x15, 0x59, 0x02, 0x04, 0x00,
  };
  fwrite(rar5_tail, 1, sizeof(rar5_tail), f);
  fclose(f);

  uint64_t zip_start = 123;
  expect_int("RAR5 polyglot is not ZIP",
             polyglot_find_zip_start(tmpl, &zip_start), 0);
  expect_int("RAR5 ZIP start reset", (int)zip_start, 0);

  uint64_t archive_start = 0;
  expect_int("RAR5 polyglot archive detected",
             polyglot_find_archive_start(tmpl, &archive_start), 1);
  expect_int("RAR5 archive start", (int)archive_start, (int)rar_start);

  char *fixed = NULL;
  uint64_t fixed_start = 0;
  expect_int("RAR5 fixed archive created",
             polyglot_make_temp_fixed_archive(tmpl, &fixed, &fixed_start), 1);
  expect_int("RAR5 fixed start", (int)fixed_start, (int)rar_start);
  if (fixed) {
    FILE *fixed_file = fopen(fixed, "rb");
    if (!fixed_file) {
      fprintf(stderr, "failed to open fixed RAR5 temp\n");
      failures++;
    } else {
      unsigned char sig[8] = {0};
      fread(sig, 1, sizeof(sig), fixed_file);
      fclose(fixed_file);
      expect_int("fixed RAR5 signature",
                 memcmp(sig, rar5_tail, sizeof(sig)) == 0, 1);
    }
    polyglot_cleanup_temp(&fixed);
  }

  unlink(tmpl);
}

static void test_retry_policy_allows_embedded_zip_parse_errors(void) {
  const char *err =
      "Open ERROR: Cannot open the file as [zip] archive\n"
      "ERRORS:\n"
      "Is not archive\n";

  expect_int("not-archive errors can still be polyglot archives",
             polyglot_should_try_after_7z_error(err), 1);
}

static void test_retry_policy_skips_environmental_errors(void) {
  expect_int("disk full is not retryable",
             polyglot_should_try_after_7z_error("No space left on device"), 0);
  expect_int("permission denied is not retryable",
             polyglot_should_try_after_7z_error("Permission denied"), 0);
  expect_int("missing path is not retryable",
             polyglot_should_try_after_7z_error("No such file or directory"),
             0);
}

static void test_retry_policy_skips_truncated_archive_errors(void) {
  expect_int("headers error is not retryable",
             polyglot_should_try_after_7z_error("Headers Error"), 0);
  expect_int("unexpected end is not retryable",
             polyglot_should_try_after_7z_error("Unexpected end of archive"),
             0);
}

static void test_retry_policy_skips_crc_archive_errors(void) {
  expect_int("CRC failures are not retryable",
             polyglot_should_try_after_7z_error("CRC Failed"), 0);
  expect_int("data errors are not retryable",
             polyglot_should_try_after_7z_error("Data Error"), 0);
}

static void test_retry_policy_allows_empty_probe_output(void) {
  expect_int("empty probe output is retryable",
             polyglot_should_try_after_7z_error(""), 1);
  expect_int("null probe output is retryable",
             polyglot_should_try_after_7z_error(NULL), 1);
}

int main(void) {
  test_ignores_false_mp4_tail_zip_signature();
  test_finds_winzip_aes_zip_method();
  test_finds_zip64_local_header_sizes();
  test_finds_rar5_polyglot_archive_start();
  test_retry_policy_allows_embedded_zip_parse_errors();
  test_retry_policy_skips_environmental_errors();
  test_retry_policy_skips_truncated_archive_errors();
  test_retry_policy_skips_crc_archive_errors();
  test_retry_policy_allows_empty_probe_output();

  return failures == 0 ? 0 : 1;
}
