#include "path.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

char *path_parent(const char *path) {
  if (!path)
    return NULL;
  const char *slash = strrchr(path, '/');
  if (!slash)
    return strdup(".");
  size_t len = (size_t)(slash - path);
  if (len == 0)
    return strdup("/");
  char *out = (char *)malloc(len + 1);
  if (!out)
    return NULL;
  memcpy(out, path, len);
  out[len] = 0;
  return out;
}

char *path_filename(const char *path) {
  if (!path)
    return NULL;
  const char *slash = strrchr(path, '/');
  return strdup(slash ? slash + 1 : path);
}

char *path_stem(const char *path) {
  if (!path)
    return NULL;
  char *fname = path_filename(path);
  if (!fname)
    return NULL;
  /* Recognize common double extensions so that e.g. "archive.tar.gz"
     yields stem "archive" instead of "archive.tar". */
  static const struct {
    const char *ext;
    size_t len;
  } double_exts[] = {
      {".tar.gz", 7},   {".tar.bz2", 8},  {".tar.xz", 7},  {".tar.zst", 8},
      {".tar.lz", 7},   {".tar.lzma", 9},  {".tar.Z", 6},
  };
  size_t flen = strlen(fname);
  for (size_t i = 0; i < sizeof(double_exts) / sizeof(double_exts[0]); i++) {
    size_t elen = double_exts[i].len;
    if (flen > elen) {
      const char *tail = fname + flen - elen;
      if (strcasecmp(tail, double_exts[i].ext) == 0) {
        *((char *)tail) = 0;
        return fname;
      }
    }
  }
  char *dot = strrchr(fname, '.');
  if (!dot || dot == fname)
    return fname;
  *dot = 0;
  return fname;
}

int path_exists(const char *path) {
  if (!path)
    return 0;
  struct stat st;
  return stat(path, &st) == 0;
}

int mkdirs(const char *path) {
  if (!path || !*path)
    return -1;
  size_t len = strlen(path);
  if (len == 0)
    return -1;
  char *tmp = (char *)malloc(len + 1);
  if (!tmp)
    return -1;
  memcpy(tmp, path, len + 1);
  if (tmp[len - 1] == '/')
    tmp[len - 1] = 0;

  for (char *p = tmp + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
      mkdir(tmp, 0755);
      *p = '/';
    }
  }
  int rc = mkdir(tmp, 0755);
  free(tmp);
  return rc;
}
