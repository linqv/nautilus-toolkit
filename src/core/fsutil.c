#define _GNU_SOURCE
#define _XOPEN_SOURCE 700
#include "encoding.h"
#include "fsutil.h"
#include "strbuf.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int path_is_within_root(const char *path, const char *root) {
  if (!path || !*path || !root || !*root)
    return 0;
  size_t root_len = strlen(root);
  if (root_len == 0)
    return 0;
  if (strcmp(path, root) == 0)
    return 0;
  if (strncmp(path, root, root_len) != 0)
    return 0;
  if (root[root_len - 1] == '/')
    return 1;
  return path[root_len] == '/';
}

static int same_file(const struct stat *a, const struct stat *b) {
  return a && b && a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static int valid_relative_component(const char *name) {
  return name && *name && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
         strchr(name, '/') == NULL;
}

static int open_dir_beneath(int root_fd, const char *rel) {
  int current_fd = dup(root_fd);
  if (current_fd < 0)
    return -1;
  if (!rel || !*rel)
    return current_fd;

  char *copy = str_dup(rel);
  if (!copy) {
    close(current_fd);
    return -1;
  }

  char *saveptr = NULL;
  char *part = strtok_r(copy, "/", &saveptr);
  while (part) {
    if (!valid_relative_component(part)) {
      close(current_fd);
      free(copy);
      errno = EINVAL;
      return -1;
    }

    int next_fd = openat(current_fd, part,
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(current_fd);
    if (next_fd < 0) {
      free(copy);
      return -1;
    }
    current_fd = next_fd;
    part = strtok_r(NULL, "/", &saveptr);
  }

  free(copy);
  return current_fd;
}

static int remove_tree_contents_fd(int dir_fd) {
  int scan_fd = dup(dir_fd);
  if (scan_fd < 0)
    return -1;

  DIR *dir = fdopendir(scan_fd);
  if (!dir) {
    close(scan_fd);
    return -1;
  }

  int rc = 0;
  struct dirent *de = NULL;
  while ((de = readdir(dir)) != NULL) {
    const char *name = de->d_name;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
      continue;

    struct stat before;
    if (fstatat(dir_fd, name, &before, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT)
        continue;
      rc = -1;
      break;
    }

    if (S_ISDIR(before.st_mode)) {
      int child_fd = openat(dir_fd, name,
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (child_fd < 0) {
        if (errno == ENOENT)
          continue;
        rc = -1;
        break;
      }

      struct stat opened;
      if (fstat(child_fd, &opened) != 0 || !same_file(&before, &opened) ||
          remove_tree_contents_fd(child_fd) != 0) {
        close(child_fd);
        rc = -1;
        break;
      }
      close(child_fd);

      struct stat current;
      if (fstatat(dir_fd, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
          !same_file(&before, &current) ||
          unlinkat(dir_fd, name, AT_REMOVEDIR) != 0) {
        if (errno == ENOENT)
          continue;
        rc = -1;
        break;
      }
    } else if (unlinkat(dir_fd, name, 0) != 0 && errno != ENOENT) {
      rc = -1;
      break;
    }
  }

  closedir(dir);
  return rc;
}

int remove_tree(const char *path, const char *allowed_root) {
  if (!path || !*path || !allowed_root || !*allowed_root)
    return -1;

  char resolved_path[PATH_MAX];
  char resolved_root[PATH_MAX];
  if (!realpath(path, resolved_path))
    return -1;
  if (!realpath(allowed_root, resolved_root))
    return -1;

  if (!path_is_within_root(resolved_path, resolved_root))
    return -1;

  size_t root_len = strlen(resolved_root);
  const char *rel = resolved_path + root_len;
  if (*rel == '/')
    rel++;
  if (!*rel)
    return -1;

  const char *slash = strrchr(rel, '/');
  char *parent_rel = NULL;
  const char *leaf = rel;
  if (slash) {
    size_t parent_len = (size_t)(slash - rel);
    parent_rel = (char *)malloc(parent_len + 1);
    if (!parent_rel)
      return -1;
    memcpy(parent_rel, rel, parent_len);
    parent_rel[parent_len] = 0;
    leaf = slash + 1;
  }
  if (!valid_relative_component(leaf)) {
    free(parent_rel);
    errno = EINVAL;
    return -1;
  }

  int root_fd =
      open(resolved_root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root_fd < 0) {
    free(parent_rel);
    return -1;
  }

  int parent_fd = open_dir_beneath(root_fd, parent_rel);
  free(parent_rel);
  close(root_fd);
  if (parent_fd < 0)
    return -1;

  struct stat before;
  int rc = -1;
  if (fstatat(parent_fd, leaf, &before, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISDIR(before.st_mode)) {
    int target_fd = openat(parent_fd, leaf,
                           O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (target_fd >= 0) {
      struct stat opened;
      if (fstat(target_fd, &opened) == 0 && same_file(&before, &opened) &&
          remove_tree_contents_fd(target_fd) == 0) {
        struct stat current;
        if (fstatat(parent_fd, leaf, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
            same_file(&before, &current) &&
            unlinkat(parent_fd, leaf, AT_REMOVEDIR) == 0) {
          rc = 0;
        }
      }
      close(target_fd);
    }
  }

  close(parent_fd);
  return rc;
}

static int locale_needs_utf8_fix(const char *locale) {
  if (!locale || !*locale)
    return 0;
  const char *dot = strchr(locale, '.');
  const char *charset = dot ? dot + 1 : locale;
  if (!*charset)
    return 0;
  return strcasecmp(charset, "UTF-8") != 0 && strcasecmp(charset, "utf8") != 0;
}

static char *join_path(const char *dir, const char *name) {
  if (!dir || !name)
    return NULL;
  size_t dir_len = strlen(dir);
  size_t name_len = strlen(name);
  size_t need_sep = (dir_len > 0 && dir[dir_len - 1] != '/') ? 1u : 0u;
  char *out = (char *)malloc(dir_len + need_sep + name_len + 1);
  if (!out)
    return NULL;
  memcpy(out, dir, dir_len);
  if (need_sep)
    out[dir_len++] = '/';
  memcpy(out + dir_len, name, name_len);
  out[dir_len + name_len] = 0;
  return out;
}

static int rename_path_basename_utf8(const char *path, const char *locale,
                                     int *renamed_count) {
  if (!path || !locale || !*locale)
    return 0;

  const char *slash = strrchr(path, '/');
  const char *base = slash ? slash + 1 : path;
  if (!*base || is_valid_utf8(base))
    return 0;

  StrBuf converted;
  sb_init(&converted);
  int ok = convert_locale_to_utf8(base, locale, &converted);
  if (!ok || !converted.data || !*converted.data || !is_valid_utf8(converted.data) ||
      strchr(converted.data, '/') || strcmp(converted.data, ".") == 0 ||
      strcmp(converted.data, "..") == 0 || strcmp(converted.data, base) == 0) {
    sb_free(&converted);
    return 0;
  }

  char *parent = NULL;
  if (slash) {
    size_t parent_len = (size_t)(slash - path);
    parent = (char *)malloc(parent_len + 1);
    if (!parent) {
      sb_free(&converted);
      return 0;
    }
    memcpy(parent, path, parent_len);
    parent[parent_len] = 0;
  }

  char *target = parent ? join_path(parent, converted.data) : str_dup(converted.data);
  sb_free(&converted);
  free(parent);
  if (!target)
    return 0;

  int exists = access(target, F_OK) == 0;
  int renamed = 0;
  if (!exists && rename(path, target) == 0) {
    renamed = 1;
    if (renamed_count)
      (*renamed_count)++;
  }
  free(target);
  return renamed;
}

static void normalize_tree_utf8_names_impl(const char *path, const char *locale,
                                           int rename_self,
                                           int *renamed_count) {
  struct stat st;
  if (!path || !locale || lstat(path, &st) != 0)
    return;

  if (S_ISDIR(st.st_mode)) {
    DIR *dir = opendir(path);
    if (!dir)
      return;

    struct dirent *de = NULL;
    while ((de = readdir(dir)) != NULL) {
      if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
        continue;
      char *child = join_path(path, de->d_name);
      if (!child)
        continue;
      normalize_tree_utf8_names_impl(child, locale, 1, renamed_count);
      free(child);
    }
    closedir(dir);
  }

  if (rename_self)
    (void)rename_path_basename_utf8(path, locale, renamed_count);
}

int normalize_tree_utf8_names(const char *root, const char *locale,
                              int rename_root) {
  if (!root || !*root || !locale_needs_utf8_fix(locale))
    return 0;
  int renamed = 0;
  normalize_tree_utf8_names_impl(root, locale, rename_root, &renamed);
  return renamed;
}
