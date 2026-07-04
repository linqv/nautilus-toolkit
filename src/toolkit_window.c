/*
 * toolkit_window.c — Unified "解压/压缩" window.
 *
 * Creates an AdwWindow with an AdwViewStack:
 *   • "解压" page  — reuses ExtractionContext / ui_gtk_build_setup_page
 *   • "压缩" page  — reuses compress-dialog widget building
 *
 * When all selected items are folders, the "解压" page is insensitive.
 */

#define _GNU_SOURCE

#include "toolkit_window.h"
#include "compress/compress-dialog.h"
#include "ui_gtk/ui_gtk.h"
#include "core/extract_util.h"
#include "core/log.h"
#include "core/path.h"
#include "core/pwdlib.h"
#include "style.h"

#include <nautilus-extension.h>
#include <adwaita.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ══════════════════════════════════════════════════════════════════════════
 *  CSS provider — loaded once, shared across all windows
 * ══════════════════════════════════════════════════════════════════════════ */

static GtkCssProvider *ntk_css_provider = NULL;

static void
ensure_css_provider(void)
{
    if (ntk_css_provider)
        return;
    ntk_css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(ntk_css_provider, NTK_STYLESHEET);
    gtk_style_context_add_provider_for_display(
        gdk_display_get_default(),
        GTK_STYLE_PROVIDER(ntk_css_provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Extraction context — heap-allocated, freed on window close
 * ══════════════════════════════════════════════════════════════════════════ */

static void
extraction_ctx_shutdown(ExtractionContext *ctx)
{
    if (!ctx)
        return;

    log_msg("shutdown: begin");

    /* Detach GLib sources first so no further UI callbacks use this ctx. */
    if (ctx->pipe_source_id != 0) {
        g_source_remove(ctx->pipe_source_id);
        ctx->pipe_source_id = 0;
    }
    if (ctx->pulse_source_id != 0) {
        g_source_remove(ctx->pulse_source_id);
        ctx->pulse_source_id = 0;
    }
    g_atomic_int_set(&ctx->pulse_active, 0);

    /* Stop worker and wait for clean exit. */
    g_atomic_int_set(&ctx->cancelled, 1);

    log_msg("shutdown: closing pipe fds (write=%d, read=%d)",
            ctx->pipe_write_fd, ctx->pipe_read_fd);

    /* Close write end of progress pipe BEFORE joining so that any
       pipe_writef() in the worker thread gets EPIPE instead of blocking
       on a full pipe buffer. */
    if (ctx->pipe_write_fd >= 0) {
        close(ctx->pipe_write_fd);
        ctx->pipe_write_fd = -1;
    }

    /* Close read end BEFORE joining too.  The GLib source was already
       removed above so nothing reads from this fd any more.  Closing it
       ensures that any write to the pipe (via the dup'd fd held by the
       worker's pipe_stream / fprintf) immediately gets EPIPE instead of
       blocking when the pipe buffer is full. */
    if (ctx->pipe_read_fd >= 0) {
        close(ctx->pipe_read_fd);
        ctx->pipe_read_fd = -1;
    }

    log_msg("shutdown: joining worker thread");
    if (ctx->worker_thread) {
        g_thread_join(ctx->worker_thread);
        ctx->worker_thread = NULL;
    }
    log_msg("shutdown: done");
}

static void
extraction_ctx_free(ExtractionContext *ctx)
{
    if (!ctx)
        return;
    extraction_ctx_shutdown(ctx);
    g_free(ctx->dest_dir);
    free(ctx->archive_dir);
    pwdvec_free(&ctx->password_lib);
    task_path_list_free(&ctx->tasks);
    for (int i = 0; i < ctx->result_count; i++) {
        free(ctx->results[i].filename);
        free(ctx->results[i].outdir);
        free(ctx->results[i].failure_reason);
        free(ctx->results[i].used_password);
    }
    free(ctx->results);
    for (int i = 0; i < ctx->saved_password_count; i++)
        free(ctx->saved_passwords[i]);
    free(ctx->saved_passwords);
    if (ctx->paths) {
        for (size_t i = 0; i < ctx->path_count; i++)
            free(ctx->paths[i]);
        free(ctx->paths);
    }
    free(ctx);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Extract page — close / cancel / extract handlers
 * ══════════════════════════════════════════════════════════════════════════ */

static void
on_extract_clicked(GtkButton *btn, gpointer ud)
{
    (void)btn;
    ExtractionContext *ctx = (ExtractionContext *)ud;
    ui_gtk_switch_to_progress(ctx);
    ui_gtk_start_extraction(ctx);
}

static gboolean
on_close_request(GtkWindow *win, gpointer ud)
{
    (void)win;
    ExtractionContext *ctx = (ExtractionContext *)ud;
    extraction_ctx_shutdown(ctx);
    return FALSE; /* allow close */
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Build the extract page (setup + progress + done in a sub-stack)
 * ══════════════════════════════════════════════════════════════════════════ */

static GtkWidget *
build_extract_page(ExtractionContext *ctx)
{
    /* The extract page uses its own GtkStack for setup/progress/done */
    ctx->stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(ctx->stack,
                                  GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
    gtk_stack_set_transition_duration(ctx->stack, 250);

    /* SETUP page */
    GtkWidget *setup_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(setup_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(setup_scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(setup_scroll), 420);
    gtk_widget_set_vexpand(setup_scroll, TRUE);

    AdwClamp *setup_clamp = ADW_CLAMP(adw_clamp_new());
    adw_clamp_set_maximum_size(setup_clamp, 500);
    adw_clamp_set_tightening_threshold(setup_clamp, 400);

    GtkBox *setup_content =
        GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 24));
    gtk_widget_set_margin_start(GTK_WIDGET(setup_content), 16);
    gtk_widget_set_margin_end(GTK_WIDGET(setup_content), 16);
    gtk_widget_set_margin_top(GTK_WIDGET(setup_content), 16);
    gtk_widget_set_margin_bottom(GTK_WIDGET(setup_content), 16);

    ui_gtk_build_setup_page(ctx, setup_content);

    /* Extract button at bottom of setup */
    ctx->btn_extract = GTK_BUTTON(gtk_button_new_with_label("开始解压"));
    gtk_widget_add_css_class(GTK_WIDGET(ctx->btn_extract), "suggested-action");
    gtk_widget_add_css_class(GTK_WIDGET(ctx->btn_extract), "pill");
    gtk_widget_set_halign(GTK_WIDGET(ctx->btn_extract), GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(GTK_WIDGET(ctx->btn_extract), 8);
    g_signal_connect(ctx->btn_extract, "clicked",
                     G_CALLBACK(on_extract_clicked), ctx);
    gtk_box_append(setup_content, GTK_WIDGET(ctx->btn_extract));

    adw_clamp_set_child(setup_clamp, GTK_WIDGET(setup_content));
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(setup_scroll),
                                  GTK_WIDGET(setup_clamp));
    gtk_stack_add_named(ctx->stack, setup_scroll, "setup");

    /* PROGRESS page */
    GtkBox *progress_content =
        GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 8));
    ui_gtk_build_progress_page(ctx, progress_content);
    gtk_stack_add_named(ctx->stack, GTK_WIDGET(progress_content), "progress");

    /* DONE page */
    GtkWidget *done_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(done_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(
        GTK_SCROLLED_WINDOW(done_scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(
        GTK_SCROLLED_WINDOW(done_scroll), 420);
    gtk_widget_set_vexpand(done_scroll, TRUE);

    GtkBox *done_content =
        GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 16));
    ui_gtk_build_done_page(ctx, done_content);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(done_scroll),
                                  GTK_WIDGET(done_content));
    gtk_stack_add_named(ctx->stack, done_scroll, "done");

    gtk_stack_set_visible_child_name(ctx->stack, "setup");

    return GTK_WIDGET(ctx->stack);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Public entry point
 * ══════════════════════════════════════════════════════════════════════════ */

void
toolkit_window_show(GList *nautilus_files, bool all_folders)
{
    /* ── Load custom CSS (once) ── */
    ensure_css_provider();
    /* ── Collect file paths from NautilusFileInfo list ── */
    GList *path_list = NULL;
    int    path_count = 0;

    for (GList *l = nautilus_files; l; l = l->next) {
        NautilusFileInfo *info = l->data;
        GFile *location = nautilus_file_info_get_location(info);
        char  *path     = g_file_get_path(location);
        g_object_unref(location);
        if (path) {
            path_list = g_list_append(path_list, path);
            path_count++;
        }
    }

    if (path_count == 0) {
        g_list_free(path_list);
        return;
    }

    /* ── Build extraction context (heap-allocated) ── */
    ExtractionContext *ext_ctx = calloc(1, sizeof(ExtractionContext));
    if (!ext_ctx) {
        g_list_free_full(path_list, g_free);
        return;
    }
    ext_ctx->pipe_read_fd  = -1;
    ext_ctx->pipe_write_fd = -1;

    /* Copy paths into ext_ctx->paths (char**) */
    ext_ctx->paths = calloc((size_t)path_count, sizeof(char *));
    if (!ext_ctx->paths) {
        free(ext_ctx);
        g_list_free_full(path_list, g_free);
        return;
    }
    ext_ctx->path_count = (size_t)path_count;
    {
        int i = 0;
        for (GList *l = path_list; l; l = l->next, i++)
            ext_ctx->paths[i] = strdup((char *)l->data);
    }

    /* Build task paths for extraction (split-archive grouping) */
    if (!all_folders) {
        build_task_paths(ext_ctx->paths, ext_ctx->path_count, &ext_ctx->tasks);
        if (ext_ctx->tasks.len > 0) {
            ext_ctx->archive_dir = path_parent(ext_ctx->tasks.data[0]);
            ext_ctx->total_files = (int)ext_ctx->tasks.len;
        }
    }

    /* Load password library */
    pwdvec_init(&ext_ctx->password_lib);
    load_password_lib(&ext_ctx->password_lib);

    /* ── Create the unified window ── */
    AdwWindow *window = ADW_WINDOW(adw_window_new());
    gtk_window_set_title(GTK_WINDOW(window), "解压/压缩");
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 480);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);

    ext_ctx->window = window;

    g_signal_connect(window, "close-request",
                     G_CALLBACK(on_close_request), ext_ctx);
    /* Free extraction context when window is destroyed */
    g_object_set_data_full(G_OBJECT(window), "ext-ctx", ext_ctx,
                           (GDestroyNotify)extraction_ctx_free);

    /* ── Main layout ── */
    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());

    /* Header bar with view switcher */
    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());

    /* ── View stack for 解压 / 压缩 ── */
    AdwViewStack *view_stack = ADW_VIEW_STACK(adw_view_stack_new());

    /* Title widget: AdwViewSwitcher */
    AdwViewSwitcher *switcher = ADW_VIEW_SWITCHER(adw_view_switcher_new());
    adw_view_switcher_set_stack(switcher, view_stack);
    adw_view_switcher_set_policy(switcher, ADW_VIEW_SWITCHER_POLICY_WIDE);
    adw_header_bar_set_title_widget(header, GTK_WIDGET(switcher));

    adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(header));

    /* ── Extract page ── */
    GtkWidget *extract_page = build_extract_page(ext_ctx);
    AdwViewStackPage *ep = adw_view_stack_add_titled(
        view_stack, extract_page, "extract", "解压");
    adw_view_stack_page_set_icon_name(ep, "package-x-generic-symbolic");

    if (all_folders || ext_ctx->tasks.len == 0)
        gtk_widget_set_sensitive(extract_page, FALSE);

    GList *compress_paths = NULL;
    for (GList *l = path_list; l; l = l->next)
        compress_paths = g_list_append(compress_paths, g_strdup(l->data));

    GtkWidget *compress_page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    compress_dialog_show_in_container(GTK_WINDOW(window),
                                      compress_paths,
                                      GTK_BOX(compress_page));
    g_list_free_full(compress_paths, g_free);

    AdwViewStackPage *cp = adw_view_stack_add_titled(
        view_stack, compress_page, "compress", "压缩");
    adw_view_stack_page_set_icon_name(cp, "folder-new-symbolic");

    if (all_folders || ext_ctx->tasks.len == 0)
        adw_view_stack_set_visible_child_name(view_stack, "compress");

    adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(view_stack));
    adw_window_set_content(window, GTK_WIDGET(toolbar_view));

    g_list_free_full(path_list, g_free);

    gtk_window_present(GTK_WINDOW(window));
}
