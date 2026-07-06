#pragma once
#include "pwdlib.h"
#include "strbuf.h"
#include <stddef.h>
#include <stdint.h>

/* ── Password variant (encoding fallback) ── */
typedef struct {
  char *bytes;
  char *locale;
  int transcoded;
} PwdVariant;

typedef struct {
  PwdVariant *data;
  size_t len;
  size_t cap;
} PwdVarVec;

void varvec_init(PwdVarVec *v);
void varvec_free(PwdVarVec *v);
void build_password_variants(const char *utf8_pwd, PwdVarVec *out);

/* ── Password hit cache ── */
typedef struct {
  char *fingerprint;
  char *password;
} PasswordHit;

typedef struct {
  PasswordHit *slots;
  size_t len;
  size_t cap;
} PasswordHitCache;

void hit_cache_init(PasswordHitCache *c);
void hit_cache_free(PasswordHitCache *c);
const char *hit_cache_get(PasswordHitCache *c, const char *fingerprint);
void hit_cache_put(PasswordHitCache *c, const char *fingerprint,
                   const char *password);

/* ── Task path list (split-archive grouping) ── */
typedef struct {
  char **data;
  size_t len;
} TaskPathList;

void task_path_list_free(TaskPathList *t);
int build_task_paths(char **paths, size_t count, TaskPathList *out);

/* ── Archive metadata cache ── */
typedef struct {
  char *fingerprint;
  char *listing;
  int list_ec;
  int had_password;
} ArchiveMetadata;

typedef struct {
  ArchiveMetadata *slots;
  size_t len;
  size_t cap;
} ArchiveMetadataCache;

void archive_cache_init(ArchiveMetadataCache *c);
void archive_cache_free(ArchiveMetadataCache *c);
const ArchiveMetadata *archive_cache_get(ArchiveMetadataCache *c,
                                         const char *fingerprint);
void archive_cache_put(ArchiveMetadataCache *c, const char *fingerprint,
                       const char *listing, int list_ec, int had_password);

typedef void (*TryPasswordProgressFn)(int attempted, int total,
                                      void *user_data);

/* ── Utilities ── */
char *determine_output_dir(const char *filepath, const char *password,
                           int *needs_password_recheck);
char *determine_output_dir_with_listing(const char *filepath,
                                        const char *listing, int list_ec,
                                        int *needs_password_recheck);
int try_password_list(const char *filepath, PwdVec *v, char **hit, int jobs,
                      StrBuf *listing_out, TryPasswordProgressFn progress_cb,
                      void *progress_ud);
int try_password_list_polyglot_zip(const char *filepath, uint64_t zip_start,
                                   PwdVec *v, char **hit,
                                   TryPasswordProgressFn progress_cb,
                                   void *progress_ud);
char *build_file_fingerprint(const char *filepath);
void ignore_sigpipe_once(void);
int dependency_in_path(const char *dep);
