#define _GNU_SOURCE
#include "extract_outdir.h"

#include "path.h"
#include "strbuf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

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

static int path_is_dir(const char *path) {
  struct stat st;
  return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int prepare_extract_outdir(char **outdir, const char *parent,
                           const char *custom_dest,
                           int reuse_existing_created,
                           int *created_by_us, int *existed_before) {
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

  if (reuse_existing_created && path_is_dir(*outdir)) {
    if (created_by_us)
      *created_by_us = 1;
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
