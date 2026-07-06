#pragma once
#include "strbuf.h"
#include <stdio.h>

int run_7z_capture(char **args, int argn, StrBuf *out, int nonblock,
                   FILE *progress_pipe, double start_pct, double slot_size,
                   const char *ctype_locale, const char *archive_label,
                   int task_index, int task_total,
                   int *global_progress_floor,
                   const char *stdin_password);

int run_extract_for_file(const char *filepath, const char *outdir,
                         const char *pwd_bytes, const char *locale,
                         FILE *progress_pipe, double start_pct, double slot_size,
                         StrBuf *out, const char *archive_label, int task_index,
                         int task_total, int *global_progress_floor);

int archive_has_legacy_gbk_zip_names(const char *filepath);
int archive_needs_legacy_gbk_zip_fallback(const char *filepath,
                                          const char *listing);
int archive_needs_legacy_gbk_password_before_extract(const char *filepath,
                                                     const char *listing);
int archive_needs_password_before_extract(const char *filepath);

int run_extract_gbk_zip_for_file(const char *filepath, const char *outdir,
                                 const char *pwd_bytes,
                                 FILE *progress_pipe, double start_pct,
                                 double slot_size, StrBuf *out,
                                 const char *archive_label, int task_index,
                                 int task_total,
                                 int *global_progress_floor);

int run_bsdtar_probe_password_for_file(const char *filepath,
                                       const char *pwd_bytes, StrBuf *out);

/* blocking=1: use blocking read (faster for cracking workers in subprocesses).
   blocking=0: use poll-based non-blocking read (allows cancel checks in GTK worker thread). */
int run_7z_probe_password_fast(const char *filepath, const char *pwd_bytes,
                               const char *locale, int use_multithread,
                               StrBuf *captured_listing, int blocking);

int run_7z_probe_password_batch(const char *filepath,
                                const char **pwd_bytes_list,
                                const char **locale_list, int count,
                                int *hit_index, int *attempted_count);

/* Bind a per-thread external cancel flag (typically ExtractionContext.cancelled).
   The pointer must stay valid while bound. */
void run_7z_bind_cancel_flag(const int *cancel_flag);
void run_7z_unbind_cancel_flag(void);

/* Per-thread cancellation helpers. */
void run_7z_request_cancel(void);
void run_7z_clear_cancel_request(void);
int run_7z_is_cancel_requested(void);

int need_password_from_output(const char *out);
int extraction_error_may_need_password(const char *out, int attempted_password);
int extract_failure_should_cleanup_output(int non_password_error,
                                          int cancelled);

/* Probe password by streaming a specific encrypted file from the archive.
   This is used when headers are not encrypted and `7z l -p...` can return
   success for wrong passwords. The probe normally stops early after enough
   stream data is observed, but stored/encrypted entries must be read to the
   end because wrong passwords can emit garbage until CRC verification.
   Returns: 1 = password correct, 0 = wrong password, -1 = non-password error */
int run_7z_probe_password_test(const char *filepath, const char *test_file,
                               const char *pwd_bytes, const char *locale,
                               int require_full_read);

/* Check if an archive has encrypted content by examining `7z l -slt` output.
   Returns: 1 = has encrypted files, 0 = no encryption detected, -1 = error */
int archive_has_encrypted_content(const char *filepath);
