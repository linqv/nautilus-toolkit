#include "polyglot.h"

#include <stdio.h>

static int failures = 0;

static void expect_int(const char *name, int actual, int expected) {
  if (actual == expected)
    return;
  fprintf(stderr, "%s: expected %d, got %d\n", name, expected, actual);
  failures++;
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
  test_retry_policy_allows_embedded_zip_parse_errors();
  test_retry_policy_skips_environmental_errors();
  test_retry_policy_skips_truncated_archive_errors();
  test_retry_policy_allows_empty_probe_output();

  return failures == 0 ? 0 : 1;
}
