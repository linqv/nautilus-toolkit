#ifndef COMPRESS_BACKEND_H
#define COMPRESS_BACKEND_H

#include <glib.h>
#include <gio/gio.h>

/* Compression format enumeration */
typedef enum {
    FORMAT_7Z,
    FORMAT_7Z_SPLIT,
    FORMAT_ZIP,
    FORMAT_ZIP_SPLIT,
    FORMAT_CBZ,
    FORMAT_TAR,
    FORMAT_TAR_XZ,
    FORMAT_TAR_ZST,
    FORMAT_WIM,
    FORMAT_COUNT
} CompressFormat;

/* Compression task description */
typedef struct {
    CompressFormat  format;
    char          **source_files;   /* NULL-terminated path array */
    int             source_count;
    char           *output_path;    /* full output path with extension */
    int             compress_level; /* raw level value */
    char           *password;       /* NULL = no encryption */
    gboolean        encrypt_header; /* encrypt filenames (7z only) */
    char           *volume_size;    /* NULL = no split, e.g. "100m" */
    GCancellable   *cancellable;
} CompressTask;

/* Tool availability detection */
typedef struct {
    gboolean has_7z;
    gboolean has_tar;
    gboolean has_zstd;
    char    *path_7z;
    char    *path_tar;
    char    *path_zstd;
} ToolAvailability;

/* Progress callback: fraction 0.0–1.0, current_file may be NULL */
typedef void (*CompressProgressCb)(double       fraction,
                                   const char  *current_file,
                                   gpointer     user_data);

/* Finish callback: success flag and optional error message */
typedef void (*CompressFinishCb)(gboolean     success,
                                 const char  *error_msg,
                                 gpointer     user_data);

/* Detect available compression tools. Caller must free with compress_tools_free(). */
ToolAvailability *compress_tools_detect(void);
void              compress_tools_free(ToolAvailability *tools);

/* Check if a format is available given the detected tools */
gboolean compress_format_available(const ToolAvailability *tools, CompressFormat fmt);

/* Get the file extension string for a format */
const char *compress_format_extension(CompressFormat fmt);

/* Get the display name for a format */
const char *compress_format_display_name(CompressFormat fmt);

/* Resolve output path conflicts by appending (1), (2), etc. Caller frees result. */
char *compress_resolve_output_path(const char *base_path);

/* Run compression asynchronously. Takes ownership of task. */
void compress_backend_run_async(CompressTask       *task,
                                CompressProgressCb  progress_cb,
                                CompressFinishCb    finish_cb,
                                gpointer            user_data);

/* Cancel a running compression task */
void compress_backend_cancel(CompressTask *task);

/* Free a CompressTask */
void compress_task_free(CompressTask *task);

#endif /* COMPRESS_BACKEND_H */
