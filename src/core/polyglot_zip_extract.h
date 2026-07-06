#pragma once

#include "strbuf.h"

#include <stdint.h>
#include <stdio.h>

/* Extract a ZIP payload embedded at zip_start inside src_path.
   If password is non-empty, it is registered with libarchive for encrypted
   ZIP entries. Returns 0 on success and nonzero on failure/unsupported input. */
int polyglot_extract_zip_with_password(const char *src_path,
                                       uint64_t zip_start,
                                       const char *outdir,
                                       const char *password,
                                       FILE *progress_pipe,
                                       double start_pct,
                                       double slot_size,
                                       StrBuf *out,
                                       const char *archive_label,
                                       int task_index,
                                       int task_total,
                                       int *global_progress_floor);

/* Test whether password can decrypt a ZIP payload embedded at zip_start.
   Returns 1 for a matching password, 0 for a rejected password, and -1 for
   source/setup errors unrelated to password matching. */
int polyglot_probe_zip_password(const char *src_path,
                                uint64_t zip_start,
                                const char *password);

/* Extract an unencrypted ZIP payload embedded at zip_start inside src_path.
   Returns 0 on success and nonzero on failure/unsupported input. */
int polyglot_extract_plain_zip(const char *src_path,
                               uint64_t zip_start,
                               const char *outdir,
                               FILE *progress_pipe,
                               double start_pct,
                               double slot_size,
                               StrBuf *out,
                               const char *archive_label,
                               int task_index,
                               int task_total,
                               int *global_progress_floor);
