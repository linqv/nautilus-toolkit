#define _GNU_SOURCE
#include "polyglot_zip_extract.h"

#include <archive.h>
#include <archive_entry.h>
#include <string.h>

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
                               int *global_progress_floor) {
  (void)src_path;
  (void)zip_start;
  (void)outdir;
  (void)progress_pipe;
  (void)start_pct;
  (void)slot_size;
  (void)archive_label;
  (void)task_index;
  (void)task_total;
  (void)global_progress_floor;
  if (out)
    sb_append(out, "polyglot ZIP fast path is not implemented",
              strlen("polyglot ZIP fast path is not implemented"));
  return 95;
}
