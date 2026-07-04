#include "compress-backend.h"
#include "compress-progress.h"
#include <glib/gstdio.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif

/* ── Format metadata ── */

static const char *format_extensions[] = {
    [FORMAT_7Z]        = ".7z",
    [FORMAT_7Z_SPLIT]  = ".7z",
    [FORMAT_ZIP]       = ".zip",
    [FORMAT_ZIP_SPLIT] = ".zip",
    [FORMAT_CBZ]       = ".cbz",
    [FORMAT_TAR]       = ".tar",
    [FORMAT_TAR_XZ]    = ".tar.xz",
    [FORMAT_TAR_ZST]   = ".tar.zst",
    [FORMAT_WIM]       = ".wim",
};

static const char *format_names[] = {
    [FORMAT_7Z]        = "7z",
    [FORMAT_7Z_SPLIT]  = "7z (分卷)",
    [FORMAT_ZIP]       = "ZIP",
    [FORMAT_ZIP_SPLIT] = "ZIP (分卷)",
    [FORMAT_CBZ]       = "CBZ",
    [FORMAT_TAR]       = "TAR",
    [FORMAT_TAR_XZ]    = "TAR.XZ",
    [FORMAT_TAR_ZST]   = "ZSTD",
    [FORMAT_WIM]       = "WIM",
};

const char *
compress_format_extension(CompressFormat fmt)
{
    if (fmt >= 0 && fmt < FORMAT_COUNT)
        return format_extensions[fmt];
    return "";
}

const char *
compress_format_display_name(CompressFormat fmt)
{
    if (fmt >= 0 && fmt < FORMAT_COUNT)
        return format_names[fmt];
    return "Unknown";
}

/* ── Tool detection ── */

ToolAvailability *
compress_tools_detect(void)
{
    ToolAvailability *t = g_new0(ToolAvailability, 1);

    t->path_7z = g_find_program_in_path("7z");
    if (!t->path_7z)
        t->path_7z = g_find_program_in_path("7zz");
    t->has_7z = (t->path_7z != NULL);

    t->path_tar = g_find_program_in_path("tar");
    t->has_tar = (t->path_tar != NULL);

    t->path_zstd = g_find_program_in_path("zstd");
    t->has_zstd = (t->path_zstd != NULL);

    return t;
}

void
compress_tools_free(ToolAvailability *tools)
{
    if (!tools) return;
    g_free(tools->path_7z);
    g_free(tools->path_tar);
    g_free(tools->path_zstd);
    g_free(tools);
}

gboolean
compress_format_available(const ToolAvailability *tools, CompressFormat fmt)
{
    switch (fmt) {
    case FORMAT_7Z:
    case FORMAT_7Z_SPLIT:
    case FORMAT_ZIP:
    case FORMAT_ZIP_SPLIT:
    case FORMAT_CBZ:
    case FORMAT_WIM:
        return tools->has_7z;
    case FORMAT_TAR:
    case FORMAT_TAR_XZ:
        return tools->has_tar;
    case FORMAT_TAR_ZST:
        return tools->has_tar && tools->has_zstd;
    default:
        return FALSE;
    }
}

/* ── Output path conflict resolution ── */

static const char *
find_archive_extension(const char *base_path)
{
    const char *basename = strrchr(base_path, '/');
    basename = basename ? basename + 1 : base_path;

    if (g_str_has_suffix(base_path, ".tar.xz"))
        return base_path + strlen(base_path) - 7;
    if (g_str_has_suffix(base_path, ".tar.zst"))
        return base_path + strlen(base_path) - 8;
    return strrchr(basename, '.');
}

static char *
output_path_variant(const char *base_path, int suffix)
{
    if (suffix <= 0)
        return g_strdup(base_path);

    const char *ext = find_archive_extension(base_path);
    char *stem;
    const char *ext_str;
    if (ext && ext > base_path) {
        stem = g_strndup(base_path, ext - base_path);
        ext_str = ext;
    } else {
        stem = g_strdup(base_path);
        ext_str = "";
    }

    char *result = g_strdup_printf("%s (%d)%s", stem, suffix, ext_str);
    g_free(stem);
    return result;
}

char *
compress_resolve_output_path(const char *base_path)
{
    for (int i = 0; i < 10000; i++) {
        char *result = output_path_variant(base_path, i);
        if (!g_file_test(result, G_FILE_TEST_EXISTS))
            return result;
        g_free(result);
    }

    return g_strdup(base_path);
}

/* ── Task lifecycle ── */

void
compress_task_free(CompressTask *task)
{
    if (!task) return;

    if (task->password) {
        explicit_bzero(task->password, strlen(task->password));
        g_free(task->password);
    }
    g_free(task->output_path);
    g_free(task->volume_size);
    if (task->source_files) {
        for (int i = 0; i < task->source_count; i++)
            g_free(task->source_files[i]);
        g_free(task->source_files);
    }
    g_clear_object(&task->cancellable);
    g_free(task);
}

void
compress_backend_cancel(CompressTask *task)
{
    if (task && task->cancellable)
        g_cancellable_cancel(task->cancellable);
}

/* ── Command building ── */

static char *
tar_member_name_from_basename(const char *base)
{
    if (!base)
        return g_strdup("");
    if (base[0] == '-')
        return g_strconcat("./", base, NULL);
    return g_strdup(base);
}

static gboolean
compress_format_is_split(CompressFormat fmt)
{
    return fmt == FORMAT_7Z_SPLIT || fmt == FORMAT_ZIP_SPLIT;
}

static GPtrArray *
build_7z_argv(const CompressTask *task, const char *output_path)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(argv, g_strdup("7z"));
    g_ptr_array_add(argv, g_strdup("a"));
    g_ptr_array_add(argv, g_strdup("-bsp2")); /* progress output to stderr (unbuffered) */

    /* format type */
    switch (task->format) {
    case FORMAT_7Z:
    case FORMAT_7Z_SPLIT:
        g_ptr_array_add(argv, g_strdup("-t7z"));
        break;
    case FORMAT_ZIP:
    case FORMAT_ZIP_SPLIT:
        g_ptr_array_add(argv, g_strdup("-tzip"));
        g_ptr_array_add(argv, g_strdup("-mem=AES256"));
        break;
    case FORMAT_CBZ:
        g_ptr_array_add(argv, g_strdup("-tzip"));
        g_ptr_array_add(argv, g_strdup("-mx=0"));
        break;
    case FORMAT_WIM:
        g_ptr_array_add(argv, g_strdup("-twim"));
        break;
    default:
        break;
    }

    /* compression level (not for CBZ/WIM) */
    if (task->format != FORMAT_CBZ && task->format != FORMAT_WIM) {
        char *level_arg = g_strdup_printf("-mx=%d", task->compress_level);
        g_ptr_array_add(argv, level_arg);

        /* 7z "极限" mode: level 10 is our sentinel for ultra settings */
        if (task->compress_level == 10 &&
            (task->format == FORMAT_7Z || task->format == FORMAT_7Z_SPLIT)) {
            g_ptr_array_add(argv, g_strdup("-ms=on"));
            g_ptr_array_add(argv, g_strdup("-mfb=273"));
            g_ptr_array_add(argv, g_strdup("-md=64m"));
        }
    }

    /* password */
    if (task->password && task->password[0]) {
        g_ptr_array_add(argv, g_strdup("-p"));
    }

    /* encrypt header (7z only) */
    if (task->encrypt_header &&
        (task->format == FORMAT_7Z || task->format == FORMAT_7Z_SPLIT)) {
        g_ptr_array_add(argv, g_strdup("-mhe=on"));
    }

    /* volume splitting */
    if (task->volume_size && task->volume_size[0]) {
        char *vol_arg = g_strdup_printf("-v%s", task->volume_size);
        g_ptr_array_add(argv, vol_arg);
    }

    /* output path */
    g_ptr_array_add(argv, g_strdup(output_path));

    /* source files */
    for (int i = 0; i < task->source_count; i++)
        g_ptr_array_add(argv, g_strdup(task->source_files[i]));

    g_ptr_array_add(argv, NULL);
    return argv;
}

static GPtrArray *
build_tar_argv(const CompressTask *task, const char *output_path)
{
    GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);

    g_ptr_array_add(argv, g_strdup("tar"));

    /* compression method */
    switch (task->format) {
    case FORMAT_TAR:
        g_ptr_array_add(argv, g_strdup("-cf"));
        break;
    case FORMAT_TAR_XZ:
        g_ptr_array_add(argv, g_strdup("-Jcf"));
        break;
    case FORMAT_TAR_ZST: {
        char *zstd_cmd = g_strdup_printf("zstd -%d -T0", task->compress_level);
        g_ptr_array_add(argv, g_strdup("-I"));
        g_ptr_array_add(argv, zstd_cmd);
        g_ptr_array_add(argv, g_strdup("-cf"));
        break;
    }
    default:
        g_ptr_array_add(argv, g_strdup("-cf"));
        break;
    }

    /* output path */
    g_ptr_array_add(argv, g_strdup(output_path));

    /* verbose for progress */
    g_ptr_array_add(argv, g_strdup("-v"));

    /* -C parent_dir and relative filenames */
    /* We use -C to change to the parent directory of each source file */
    for (int i = 0; i < task->source_count; i++) {
        char *dir = g_path_get_dirname(task->source_files[i]);
        char *base = g_path_get_basename(task->source_files[i]);
        char *safe_base = tar_member_name_from_basename(base);
        g_ptr_array_add(argv, g_strdup("-C"));
        g_ptr_array_add(argv, dir);
        g_ptr_array_add(argv, safe_base);
        g_free(base);
    }

    g_ptr_array_add(argv, NULL);
    return argv;
}

static gboolean
send_7z_password(GSubprocess *subprocess, const char *password, GError **error)
{
    if (!subprocess || !password || !password[0])
        return TRUE;

    GOutputStream *stdin_stream = g_subprocess_get_stdin_pipe(subprocess);
    if (!stdin_stream)
        return FALSE;

    char *line = g_strdup_printf("%s\n", password);
    gsize len = strlen(line);
    gsize written = 0;
    gboolean ok = g_output_stream_write_all(stdin_stream, line, len, &written, NULL, error);
    g_free(line);
    if (!ok || written != len)
        return FALSE;
    if (!g_output_stream_close(stdin_stream, NULL, error))
        return FALSE;
    return TRUE;
}

/* ── Async execution context ── */

typedef struct {
    CompressTask       *task;
    CompressProgressCb  progress_cb;
    CompressFinishCb    finish_cb;
    gpointer            user_data;
    GSubprocess        *subprocess;
    char               *temp_dir;
    char               *work_output_path;
    int                 total_files; /* for tar progress */
} AsyncContext;

static void
async_context_free(AsyncContext *ctx)
{
    g_clear_object(&ctx->subprocess);
    g_free(ctx->temp_dir);
    g_free(ctx->work_output_path);
    /* task is freed separately by finish callback owner */
    g_free(ctx);
}

/* Progress callback data for g_idle_add */
typedef struct {
    CompressProgressCb  cb;
    double              fraction;
    char               *current_file;
    gpointer            user_data;
} ProgressIdleData;

static gboolean
progress_idle_cb(gpointer data)
{
    ProgressIdleData *pd = data;
    if (pd->cb)
        pd->cb(pd->fraction, pd->current_file, pd->user_data);
    g_free(pd->current_file);
    g_free(pd);
    return G_SOURCE_REMOVE;
}

static void
emit_progress(AsyncContext *ctx, double fraction, const char *current_file)
{
    if (!ctx->progress_cb)
        return;

    ProgressIdleData *pd = g_new0(ProgressIdleData, 1);
    pd->cb = ctx->progress_cb;
    pd->fraction = fraction;
    pd->current_file = g_strdup(current_file);
    pd->user_data = ctx->user_data;
    g_idle_add(progress_idle_cb, pd);
}

/* Finish callback data for g_idle_add */
typedef struct {
    CompressFinishCb  cb;
    gboolean          success;
    char             *error_msg;
    gpointer          user_data;
} FinishIdleData;

static gboolean
finish_idle_cb(gpointer data)
{
    FinishIdleData *fd = data;
    if (fd->cb)
        fd->cb(fd->success, fd->error_msg, fd->user_data);
    g_free(fd->error_msg);
    g_free(fd);
    return G_SOURCE_REMOVE;
}

static void
emit_finish(AsyncContext *ctx, gboolean success, const char *error_msg)
{
    FinishIdleData *fd = g_new0(FinishIdleData, 1);
    fd->cb = ctx->finish_cb;
    fd->success = success;
    fd->error_msg = g_strdup(error_msg);
    fd->user_data = ctx->user_data;
    g_idle_add(finish_idle_cb, fd);
}

static void
on_cancellable_cancelled(GCancellable *cancellable G_GNUC_UNUSED,
                         gpointer      user_data)
{
    if (user_data && G_IS_SUBPROCESS(user_data))
        g_subprocess_force_exit(G_SUBPROCESS(user_data));
}

static char *
create_temp_output_dir(const char *output_path, GError **error)
{
    char *parent = g_path_get_dirname(output_path);
    char *tmpl = g_build_filename(parent, ".nautilus-toolkit-compress-XXXXXX", NULL);
    g_free(parent);

    if (!g_mkdtemp(tmpl)) {
        int saved_errno = errno;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Failed to create temporary output directory: %s",
                    g_strerror(saved_errno));
        g_free(tmpl);
        return NULL;
    }

    return tmpl;
}

static void
cleanup_temp_dir(const char *temp_dir)
{
    if (!temp_dir)
        return;

    GDir *dir = g_dir_open(temp_dir, 0, NULL);
    if (dir) {
        const char *name;
        while ((name = g_dir_read_name(dir)) != NULL) {
            char *path = g_build_filename(temp_dir, name, NULL);
            g_unlink(path);
            g_free(path);
        }
        g_dir_close(dir);
    }
    g_rmdir(temp_dir);
}

/* Clean up only files in our private temporary output directory. */
static void
cleanup_work_output(const AsyncContext *ctx)
{
    if (!ctx || !ctx->work_output_path)
        return;

    g_unlink(ctx->work_output_path);

    if (compress_format_is_split(ctx->task->format)) {
        for (int i = 1; i < 10000; i++) {
            char *vol_path = g_strdup_printf("%s.%03d", ctx->work_output_path, i);
            if (!g_file_test(vol_path, G_FILE_TEST_EXISTS)) {
                g_free(vol_path);
                break;
            }
            g_unlink(vol_path);
            g_free(vol_path);
        }
    }

    cleanup_temp_dir(ctx->temp_dir);
}

static gboolean
regular_file_exists(const char *path)
{
    GStatBuf st;
    return g_lstat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static gboolean
collect_work_outputs(const AsyncContext *ctx, GPtrArray *paths, GError **error)
{
    if (regular_file_exists(ctx->work_output_path))
        g_ptr_array_add(paths, g_strdup(ctx->work_output_path));

    if (compress_format_is_split(ctx->task->format)) {
        for (int i = 1; i < 10000; i++) {
            char *vol_path = g_strdup_printf("%s.%03d", ctx->work_output_path, i);
            if (!regular_file_exists(vol_path)) {
                g_free(vol_path);
                break;
            }
            g_ptr_array_add(paths, vol_path);
        }
    }

    if (paths->len == 0) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                    "Compression completed but no output files were created");
        return FALSE;
    }

    return TRUE;
}

static char *
final_path_for_work_path(const char *work_base, const char *final_base,
                         const char *work_path)
{
    if (g_strcmp0(work_path, work_base) == 0)
        return g_strdup(final_base);

    return g_strconcat(final_base, work_path + strlen(work_base), NULL);
}

static gboolean
candidate_has_conflict(const AsyncContext *ctx, const char *candidate,
                       GPtrArray *work_paths)
{
    if (compress_format_is_split(ctx->task->format) &&
        g_file_test(candidate, G_FILE_TEST_EXISTS))
        return TRUE;

    for (guint i = 0; i < work_paths->len; i++) {
        const char *work_path = g_ptr_array_index(work_paths, i);
        char *final_path = final_path_for_work_path(ctx->work_output_path,
                                                    candidate, work_path);
        gboolean exists = g_file_test(final_path, G_FILE_TEST_EXISTS);
        g_free(final_path);
        if (exists)
            return TRUE;
    }

    return FALSE;
}

typedef struct {
    char     *work_path;
    char     *final_path;
    gboolean  moved;
} PublishedOutput;

static PublishedOutput *
published_output_new(const char *work_path, char *final_path, gboolean moved)
{
    PublishedOutput *published_output = g_new0(PublishedOutput, 1);
    published_output->work_path = g_strdup(work_path);
    published_output->final_path = final_path;
    published_output->moved = moved;
    return published_output;
}

static void
published_output_free(gpointer data)
{
    PublishedOutput *published_output = data;
    if (!published_output)
        return;
    g_free(published_output->work_path);
    g_free(published_output->final_path);
    g_free(published_output);
}

static void
rollback_published_files(GPtrArray *published)
{
    for (guint i = 0; i < published->len; i++) {
        PublishedOutput *published_output = g_ptr_array_index(published, i);
        if (published_output->moved) {
            if (g_rename(published_output->final_path,
                         published_output->work_path) == 0)
                continue;
        }
        g_unlink(published_output->final_path);
    }
}

static gboolean
rename_file_noreplace(const char *src_path, const char *dst_path,
                      int *saved_errno)
{
    if (saved_errno)
        *saved_errno = 0;

#ifdef SYS_renameat2
    if (syscall(SYS_renameat2, AT_FDCWD, src_path, AT_FDCWD, dst_path,
                RENAME_NOREPLACE) == 0)
        return TRUE;

    if (saved_errno)
        *saved_errno = errno;
#else
    if (saved_errno)
        *saved_errno = ENOSYS;
#endif
    return FALSE;
}

static gboolean
copy_file_exclusive(const char *src_path, const char *dst_path,
                    GCancellable *cancellable, int *saved_errno)
{
    GStatBuf st;
    int in_fd = -1;
    int out_fd = -1;
    int local_errno = 0;
    gboolean created = FALSE;
    guint8 buffer[256 * 1024];

    if (saved_errno)
        *saved_errno = 0;

    if (g_lstat(src_path, &st) != 0) {
        local_errno = errno;
        goto fail;
    }
    if (!S_ISREG(st.st_mode)) {
        local_errno = EINVAL;
        goto fail;
    }

    in_fd = g_open(src_path, O_RDONLY | O_CLOEXEC, 0);
    if (in_fd < 0) {
        local_errno = errno;
        goto fail;
    }

    out_fd = g_open(dst_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                    st.st_mode & 0777);
    if (out_fd < 0) {
        local_errno = errno;
        goto fail;
    }
    created = TRUE;

    for (;;) {
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            local_errno = ECANCELED;
            goto fail;
        }

        ssize_t n = read(in_fd, buffer, sizeof(buffer));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            local_errno = errno;
            goto fail;
        }
        if (n == 0)
            break;

        guint8 *p = buffer;
        ssize_t remaining = n;
        while (remaining > 0) {
            ssize_t written = write(out_fd, p, (size_t)remaining);
            if (written < 0) {
                if (errno == EINTR)
                    continue;
                local_errno = errno;
                goto fail;
            }
            if (written == 0) {
                local_errno = EIO;
                goto fail;
            }
            p += written;
            remaining -= written;
        }
    }

    if (close(out_fd) != 0) {
        out_fd = -1;
        local_errno = errno;
        goto fail;
    }
    out_fd = -1;

    if (close(in_fd) != 0) {
        in_fd = -1;
        local_errno = errno;
        goto fail;
    }
    in_fd = -1;

    return TRUE;

fail:
    if (out_fd >= 0 && close(out_fd) != 0 && local_errno == 0)
        local_errno = errno;
    if (in_fd >= 0)
        close(in_fd);
    if (created)
        g_unlink(dst_path);
    if (saved_errno)
        *saved_errno = local_errno ? local_errno : EIO;
    return FALSE;
}

static gboolean
publish_one_output_file(const AsyncContext *ctx, const char *work_path,
                        const char *final_path, gboolean *moved,
                        int *saved_errno)
{
    if (saved_errno)
        *saved_errno = 0;
    if (moved)
        *moved = FALSE;

    if (link(work_path, final_path) == 0)
        return TRUE;

    int link_errno = errno;
    if (link_errno == EEXIST) {
        if (saved_errno)
            *saved_errno = link_errno;
        return FALSE;
    }

    if (rename_file_noreplace(work_path, final_path, saved_errno)) {
        if (moved)
            *moved = TRUE;
        return TRUE;
    }
    if (saved_errno && *saved_errno == EEXIST)
        return FALSE;

    /* Some filesystems reject hard links or RENAME_NOREPLACE; keep no-overwrite
     * behavior by copying into a freshly created destination file. */
    if (copy_file_exclusive(work_path, final_path, ctx->task->cancellable,
                            saved_errno))
        return TRUE;

    if (saved_errno && *saved_errno == 0)
        *saved_errno = link_errno;
    return FALSE;
}

static gboolean
publish_output_files(AsyncContext *ctx, GError **error)
{
    GPtrArray *work_paths = g_ptr_array_new_with_free_func(g_free);
    if (!collect_work_outputs(ctx, work_paths, error)) {
        g_ptr_array_unref(work_paths);
        return FALSE;
    }

    char *requested_path = g_strdup(ctx->task->output_path);
    for (int attempt = 0; attempt < 10000; attempt++) {
        char *candidate = output_path_variant(requested_path, attempt);
        if (candidate_has_conflict(ctx, candidate, work_paths)) {
            g_free(candidate);
            continue;
        }

        GPtrArray *published =
            g_ptr_array_new_with_free_func(published_output_free);
        int saved_errno = 0;
        gboolean ok = TRUE;
        for (guint i = 0; i < work_paths->len; i++) {
            const char *work_path = g_ptr_array_index(work_paths, i);
            char *final_path = final_path_for_work_path(ctx->work_output_path,
                                                        candidate, work_path);
            gboolean moved = FALSE;
            if (!publish_one_output_file(ctx, work_path, final_path,
                                         &moved, &saved_errno)) {
                g_free(final_path);
                ok = FALSE;
                break;
            }
            g_ptr_array_add(published,
                            published_output_new(work_path, final_path, moved));
        }

        if (ok) {
            for (guint i = 0; i < work_paths->len; i++)
                g_unlink(g_ptr_array_index(work_paths, i));
            if (g_strcmp0(ctx->task->output_path, candidate) != 0) {
                g_free(ctx->task->output_path);
                ctx->task->output_path = g_strdup(candidate);
            }
            g_ptr_array_unref(published);
            g_free(candidate);
            g_free(requested_path);
            g_ptr_array_unref(work_paths);
            cleanup_temp_dir(ctx->temp_dir);
            return TRUE;
        }

        rollback_published_files(published);
        g_ptr_array_unref(published);
        g_free(candidate);

        if (saved_errno == EEXIST)
            continue;

        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(saved_errno),
                    "Failed to publish output archive: %s",
                    g_strerror(saved_errno));
        g_free(requested_path);
        g_ptr_array_unref(work_paths);
        return FALSE;
    }

    g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_EXIST,
                "Could not find an available output path");
    g_free(requested_path);
    g_ptr_array_unref(work_paths);
    return FALSE;
}

/* Read 7z progress from stderr (byte-level, handles \b-based output).
 * 7z uses backspace characters to overwrite progress on the same line.
 * We accumulate characters, and when we see a \b sequence, we treat
 * the accumulated text as a complete progress segment. */
static void
read_7z_progress(AsyncContext *ctx, GInputStream *stream, GString *error_buf)
{
    GString *segment = g_string_new(NULL);
    guint8 buf[4096];
    gssize n;

    while ((n = g_input_stream_read(stream, buf, sizeof(buf),
                                     ctx->task->cancellable, NULL)) > 0) {
        if (g_cancellable_is_cancelled(ctx->task->cancellable))
            break;

        for (gssize i = 0; i < n; i++) {
            guint8 ch = buf[i];
            if (ch == '\b') {
                /* Backspace: the current segment is complete, try to parse it */
                if (segment->len > 0) {
                    char *current_file = NULL;
                    double frac = progress_parse_7z_line(segment->str, &current_file);
                    if (frac >= 0.0) {
                        emit_progress(ctx, frac, current_file);
                    } else if (error_buf) {
                        /* Non-progress text — potential error info */
                        if (error_buf->len > 0)
                            g_string_append_c(error_buf, '\n');
                        g_string_append_len(error_buf, segment->str, segment->len);
                        if (error_buf->len > 4096)
                            g_string_erase(error_buf, 0, error_buf->len - 4096);
                    }
                    g_free(current_file);
                    g_string_truncate(segment, 0);
                }
                /* Skip remaining consecutive backspaces */
                while (i + 1 < n && buf[i + 1] == '\b')
                    i++;
            } else if (ch == '\r' || ch == '\n') {
                /* Also treat CR/LF as segment boundary */
                if (segment->len > 0) {
                    char *current_file = NULL;
                    double frac = progress_parse_7z_line(segment->str, &current_file);
                    if (frac >= 0.0) {
                        emit_progress(ctx, frac, current_file);
                    } else if (error_buf) {
                        if (error_buf->len > 0)
                            g_string_append_c(error_buf, '\n');
                        g_string_append_len(error_buf, segment->str, segment->len);
                        if (error_buf->len > 4096)
                            g_string_erase(error_buf, 0, error_buf->len - 4096);
                    }
                    g_free(current_file);
                    g_string_truncate(segment, 0);
                }
            } else {
                g_string_append_c(segment, ch);
            }
        }
    }

    /* Parse any remaining segment */
    if (segment->len > 0) {
        char *current_file = NULL;
        double frac = progress_parse_7z_line(segment->str, &current_file);
        if (frac >= 0.0)
            emit_progress(ctx, frac, current_file);
        else if (error_buf && segment->len > 0) {
            if (error_buf->len > 0)
                g_string_append_c(error_buf, '\n');
            g_string_append_len(error_buf, segment->str, segment->len);
        }
        g_free(current_file);
    }

    g_string_free(segment, TRUE);
}

/* Read tar -v progress from stdout (line-based). */
static void
read_tar_progress(AsyncContext *ctx, GInputStream *stream)
{
    GDataInputStream *data_stream = g_data_input_stream_new(stream);
    int processed_count = 0;
    char *line;

    while ((line = g_data_input_stream_read_line(data_stream, NULL,
                                                  ctx->task->cancellable, NULL)) != NULL) {
        if (g_cancellable_is_cancelled(ctx->task->cancellable)) {
            g_free(line);
            break;
        }

        double frac = progress_parse_tar_line(line, ctx->total_files, &processed_count);
        if (frac >= 0.0)
            emit_progress(ctx, frac > 1.0 ? 1.0 : frac, line);

        g_free(line);
    }

    g_object_unref(data_stream);
}

/* GTask thread function */
static void
compress_task_thread_func(GTask        *gtask G_GNUC_UNUSED,
                          gpointer      source_object G_GNUC_UNUSED,
                          gpointer      task_data,
                          GCancellable *cancellable)
{
    AsyncContext *ctx = task_data;
    CompressTask *task = ctx->task;
    GError *error = NULL;
    gboolean is_7z;
    GPtrArray *argv;

    /* Select backend */
    switch (task->format) {
    case FORMAT_7Z:
    case FORMAT_7Z_SPLIT:
    case FORMAT_ZIP:
    case FORMAT_ZIP_SPLIT:
    case FORMAT_CBZ:
    case FORMAT_WIM:
        is_7z = TRUE;
        break;
    case FORMAT_TAR:
    case FORMAT_TAR_XZ:
    case FORMAT_TAR_ZST:
        is_7z = FALSE;
        /* Pre-count files for tar progress */
        ctx->total_files = progress_count_files(task->source_files, task->source_count);
        if (ctx->total_files <= 0)
            ctx->total_files = 1;
        break;
    default:
        emit_finish(ctx, FALSE, "Unknown format");
        return;
    }

    ctx->temp_dir = create_temp_output_dir(task->output_path, &error);
    if (!ctx->temp_dir) {
        char *msg = g_strdup_printf("Failed to prepare output path: %s",
                                     error ? error->message : "unknown error");
        emit_finish(ctx, FALSE, msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    char *output_name = g_path_get_basename(task->output_path);
    ctx->work_output_path = g_build_filename(ctx->temp_dir, output_name, NULL);
    g_free(output_name);

    argv = is_7z ? build_7z_argv(task, ctx->work_output_path)
                 : build_tar_argv(task, ctx->work_output_path);

    /* Set up subprocess with appropriate pipes:
     * - 7z: stderr for progress (-bsp2), stdout not needed
     * - tar: stdout for -v progress, stderr for errors */
    GSubprocessLauncher *launcher;
    if (is_7z) {
        GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDERR_PIPE;
        if (task->password && task->password[0])
            flags |= G_SUBPROCESS_FLAGS_STDIN_PIPE;
        launcher = g_subprocess_launcher_new(flags);
    } else {
        launcher = g_subprocess_launcher_new(
            G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    }

    if (task->format == FORMAT_TAR_XZ && task->compress_level > 0) {
        char *xz_opt = g_strdup_printf("-%d", task->compress_level);
        g_subprocess_launcher_setenv(launcher, "XZ_OPT", xz_opt, TRUE);
        g_free(xz_opt);
    }

    ctx->subprocess = g_subprocess_launcher_spawnv(
        launcher, (const char * const *)argv->pdata, &error);
    g_object_unref(launcher);
    g_ptr_array_unref(argv);

    if (!ctx->subprocess) {
        cleanup_work_output(ctx);
        char *msg = g_strdup_printf("Failed to start process: %s",
                                     error ? error->message : "unknown error");
        emit_finish(ctx, FALSE, msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    if (is_7z && task->password && task->password[0]) {
        if (!send_7z_password(ctx->subprocess, task->password, &error)) {
            g_subprocess_force_exit(ctx->subprocess);
            cleanup_work_output(ctx);
            char *msg = g_strdup_printf("Failed to pass password to 7z: %s",
                                        error ? error->message : "unknown error");
            emit_finish(ctx, FALSE, msg);
            g_free(msg);
            g_clear_error(&error);
            return;
        }
    }

    /* Connect cancellable to kill subprocess */
    gulong cancel_handler = 0;
    if (cancellable) {
        cancel_handler = g_cancellable_connect(cancellable,
            G_CALLBACK(on_cancellable_cancelled), ctx->subprocess, NULL);
    }

    /* Read progress:
     * - 7z: byte-level reading from stderr (uses \b for progress updates)
     * - tar: line-based reading from stdout (-v output) */
    GString *error_buf = g_string_new(NULL);
    if (is_7z) {
        GInputStream *stderr_stream = g_subprocess_get_stderr_pipe(ctx->subprocess);
        if (stderr_stream)
            read_7z_progress(ctx, stderr_stream, error_buf);
    } else {
        GInputStream *stdout_stream = g_subprocess_get_stdout_pipe(ctx->subprocess);
        if (stdout_stream)
            read_tar_progress(ctx, stdout_stream);
    }

    /* Wait for subprocess to finish */
    g_subprocess_wait(ctx->subprocess, NULL, &error);

    if (cancel_handler && cancellable)
        g_cancellable_disconnect(cancellable, cancel_handler);

    if (g_cancellable_is_cancelled(cancellable)) {
        cleanup_work_output(ctx);
        emit_finish(ctx, FALSE, "Compression cancelled");
        g_clear_error(&error);
        g_string_free(error_buf, TRUE);
        return;
    }

    if (error) {
        cleanup_work_output(ctx);
        char *msg = g_strdup_printf("Process error: %s", error->message);
        emit_finish(ctx, FALSE, msg);
        g_free(msg);
        g_clear_error(&error);
        g_string_free(error_buf, TRUE);
        return;
    }

    int exit_status = g_subprocess_get_exit_status(ctx->subprocess);
    if (exit_status != 0) {
        cleanup_work_output(ctx);

        /* For tar, stderr was not consumed for progress, so read it now */
        if (!is_7z) {
            GInputStream *stderr_stream = g_subprocess_get_stderr_pipe(ctx->subprocess);
            if (stderr_stream) {
                GBytes *bytes = g_input_stream_read_bytes(stderr_stream, 4096, NULL, NULL);
                if (bytes) {
                    gsize size;
                    const char *data = g_bytes_get_data(bytes, &size);
                    if (size > 0)
                        g_string_append_len(error_buf, data, size);
                    g_bytes_unref(bytes);
                }
            }
        }

        const char *stderr_text = error_buf->len > 0 ? error_buf->str : NULL;
        char *msg = g_strdup_printf("Compression failed (exit code %d)%s%s",
                                     exit_status,
                                     stderr_text ? ": " : "",
                                     stderr_text ? stderr_text : "");
        emit_finish(ctx, FALSE, msg);
        g_free(msg);
        g_string_free(error_buf, TRUE);
        return;
    }

    g_string_free(error_buf, TRUE);

    if (!publish_output_files(ctx, &error)) {
        cleanup_work_output(ctx);
        char *msg = g_strdup_printf("Failed to finalize output: %s",
                                    error ? error->message : "unknown error");
        emit_finish(ctx, FALSE, msg);
        g_free(msg);
        g_clear_error(&error);
        return;
    }

    /* Success */
    emit_finish(ctx, TRUE, NULL);
}

/* ── Public API ── */

void
compress_backend_run_async(CompressTask       *task,
                           CompressProgressCb  progress_cb,
                           CompressFinishCb    finish_cb,
                           gpointer            user_data)
{
    AsyncContext *ctx = g_new0(AsyncContext, 1);
    ctx->task = task;
    ctx->progress_cb = progress_cb;
    ctx->finish_cb = finish_cb;
    ctx->user_data = user_data;

    GTask *gtask = g_task_new(NULL, task->cancellable, NULL, NULL);
    g_task_set_task_data(gtask, ctx, (GDestroyNotify)async_context_free);
    g_task_run_in_thread(gtask, compress_task_thread_func);
    g_object_unref(gtask);
}
