#pragma once

#include "strbuf.h"

#include <stdint.h>
#include <stdio.h>

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
