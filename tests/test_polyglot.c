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

static void test_retry_policy_allows_empty_probe_output(void) {
  expect_int("empty probe output is retryable",
             polyglot_should_try_after_7z_error(""), 1);
  expect_int("null probe output is retryable",
             polyglot_should_try_after_7z_error(NULL), 1);
}

int main(void) {
  test_ignores_false_mp4_tail_zip_signature();
  test_finds_rar5_polyglot_archive_start();
  test_retry_policy_allows_embedded_zip_parse_errors();
  test_retry_policy_skips_environmental_errors();
  test_retry_policy_skips_truncated_archive_errors();
  test_retry_policy_allows_empty_probe_output();

  return failures == 0 ? 0 : 1;
}
