#define _GNU_SOURCE

#include "extract_outdir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

static char *make_temp_dir(const char *tag) {
  char tmpl[256];
  snprintf(tmpl, sizeof(tmpl), "/tmp/ntk-extract-outdir-%s-XXXXXX", tag);
  char *path = strdup(tmpl);
  if (!path)
    return NULL;
  if (!mkdtemp(path)) {
    free(path);
    return NULL;
  }
  return path;
}

static char *join_path(const char *dir, const char *name) {
  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  char *out = (char *)malloc(dir_len + 1 + name_len + 1);
  if (!out)
    return NULL;
  memcpy(out, dir, dir_len);
  out[dir_len] = '/';
  memcpy(out + dir_len + 1, name, name_len + 1);
  return out;
}

static int path_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0;
}

static void expect_int(const char *label, int got, int expected) {
  if (got == expected)
    return;
  fprintf(stderr, "%s: expected %d, got %d\n", label, expected, got);
  failures++;
}

static void expect_string(const char *label, const char *got,
                          const char *expected) {
  if (got && expected && strcmp(got, expected) == 0)
    return;
  fprintf(stderr, "%s: expected '%s', got '%s'\n", label,
          expected ? expected : "(null)", got ? got : "(null)");
  failures++;
}

static void expect_missing(const char *label, const char *path) {
  if (!path_exists(path))
    return;
  fprintf(stderr, "%s: expected missing path %s\n", label, path);
  failures++;
}

static void test_reuses_output_dir_created_by_failed_primary_attempt(void) {
  char *parent = make_temp_dir("reuse-parent");
  char *outdir = parent ? join_path(parent, "movie_1") : NULL;
  char *unexpected_retry_suffix = parent ? join_path(parent, "movie_1_1") : NULL;
  if (!parent || !outdir || !unexpected_retry_suffix) {
    fprintf(stderr, "fixture setup failed\n");
    failures++;
    free(parent);
    free(outdir);
    free(unexpected_retry_suffix);
    return;
  }

  char *created_path = strdup(outdir);
  if (!created_path) {
    fprintf(stderr, "outdir copy failed\n");
    failures++;
  }

  int created = 0;
  int existed = 0;
  expect_int("initial prepare",
             prepare_extract_outdir(&outdir, parent, NULL, 0, &created,
                                    &existed),
             1);
  expect_string("initial outdir", outdir, created_path);
  expect_int("initial created", created, 1);
  expect_int("initial existed", existed, 0);

  created = 0;
  existed = 0;
  expect_int("polyglot retry prepare",
             prepare_extract_outdir(&outdir, parent, NULL, 1, &created,
                                    &existed),
             1);
  expect_string("polyglot retry reuses outdir", outdir, created_path);
  expect_int("polyglot retry still owns outdir", created, 1);
  expect_int("polyglot retry existed before", existed, 0);

  expect_missing("polyglot retry suffix", unexpected_retry_suffix);

  rmdir(outdir);
  rmdir(parent);
  free(unexpected_retry_suffix);
  free(created_path);
  free(parent);
  free(outdir);
}

int main(void) {
  test_reuses_output_dir_created_by_failed_primary_attempt();
  return failures == 0 ? 0 : 1;
}
