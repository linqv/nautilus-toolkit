#define _GNU_SOURCE
#include "ui_gtk.h"
#include "../core/log.h"
#include "../core/pwdlib.h"
#include "../core/strbuf.h"

#include <ctype.h>
#include <glib-unix.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static gboolean ui_gtk_progress_pulse_cb(gpointer data) {
  ExtractionContext *ctx = (ExtractionContext *)data;
  if (!ctx || !g_atomic_int_get(&ctx->pulse_active)) {
    if (ctx)
      ctx->pulse_source_id = 0;
    return G_SOURCE_REMOVE;
  }
  gtk_progress_bar_pulse(ctx->progress_bar);
  return G_SOURCE_CONTINUE;
}

static void ui_gtk_set_indeterminate_progress(ExtractionContext *ctx, int on) {
  if (!ctx)
    return;
  if (on) {
    if (g_atomic_int_get(&ctx->pulse_active))
      return;
    g_atomic_int_set(&ctx->pulse_active, 1);
    gtk_progress_bar_set_fraction(ctx->progress_bar, 0.0);
    gtk_label_set_text(ctx->lbl_progress_status, "正在撞库...");
    gtk_label_set_text(ctx->lbl_progress_file, "");
    gtk_progress_bar_set_pulse_step(ctx->progress_bar, 0.04);
    gtk_progress_bar_pulse(ctx->progress_bar);
    if (ctx->pulse_source_id == 0) {
      ctx->pulse_source_id =
          g_timeout_add(120, ui_gtk_progress_pulse_cb, ctx);
    }
    return;
  }

  g_atomic_int_set(&ctx->pulse_active, 0);
  if (ctx->pulse_source_id != 0) {
    g_source_remove(ctx->pulse_source_id);
    ctx->pulse_source_id = 0;
  }
}

/* ── Save password button callback ── */
static void on_save_pwd_clicked(GtkButton *btn, gpointer ud) {
  (void)ud;
  ExtractionContext *c =
      (ExtractionContext *)g_object_get_data(G_OBJECT(btn), "ctx");
  int idx =
      GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "pwd-index"));
  if (idx >= 0 && idx < c->saved_password_count) {
    if (add_password_to_lib(&c->password_lib, c->saved_passwords[idx])) {
      if (save_password_lib(&c->password_lib))
        log_msg("Password saved to library");
      else
        log_msg("Failed to save password lib");
    }
    gtk_button_set_label(btn, "已保存");
    gtk_widget_set_sensitive(GTK_WIDGET(btn), FALSE);
  }
}

/* ── Populate the DONE page with results ── */
static void populate_done_page(ExtractionContext *ctx) {
  GtkBox *content = ctx->done_content;

  /* Summary */
  char summary[256];
  const char *icon_name;
  const char *banner_css;
  if (ctx->success_count == ctx->total_files) {
    snprintf(summary, sizeof(summary), "全部完成 (%d/%d)",
             ctx->success_count, ctx->total_files);
    icon_name = "emblem-ok-symbolic";
    banner_css = "ntk-status-success";
  } else if (ctx->success_count > 0) {
    snprintf(summary, sizeof(summary), "部分完成 (%d/%d)",
             ctx->success_count, ctx->total_files);
    icon_name = "dialog-warning-symbolic";
    banner_css = "ntk-status-warning";
  } else {
    snprintf(summary, sizeof(summary), "解压失败");
    icon_name = "dialog-error-symbolic";
    banner_css = "ntk-status-error";
  }

  /* Status banner with colored background */
  GtkWidget *banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
  gtk_widget_set_halign(banner, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(banner, 12);
  gtk_widget_set_margin_bottom(banner, 4);
  gtk_widget_add_css_class(banner, "ntk-done-banner");
  gtk_widget_add_css_class(banner, banner_css);

  GtkWidget *status_icon = gtk_image_new_from_icon_name(icon_name);
  gtk_image_set_pixel_size(GTK_IMAGE(status_icon), 56);
  gtk_box_append(GTK_BOX(banner), status_icon);

  GtkWidget *status_label = gtk_label_new(summary);
  gtk_widget_add_css_class(status_label, "title-2");
  gtk_box_append(GTK_BOX(banner), status_label);

  gtk_box_append(content, banner);

  /* Per-file results */
  AdwPreferencesGroup *grp_results =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(grp_results, "详细结果");

  for (int i = 0; i < ctx->result_count; i++) {
    FileResult *r = &ctx->results[i];
    if (!r->filename)
      continue;

    AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row),
                                  r->filename);
    if (r->success && r->outdir)
      adw_action_row_set_subtitle(row, r->outdir);
    else if (r->failure_reason && *r->failure_reason)
      adw_action_row_set_subtitle(row, r->failure_reason);
    else
      adw_action_row_set_subtitle(row, "解压失败（未知原因）");

    const char *row_icon = r->success ? "emblem-ok-symbolic"
                                      : "dialog-error-symbolic";
    GtkWidget *ic = gtk_image_new_from_icon_name(row_icon);
    gtk_widget_add_css_class(ic, r->success ? "ntk-result-ok" : "ntk-result-fail");
    adw_action_row_add_prefix(row, ic);

    adw_preferences_group_add(grp_results, GTK_WIDGET(row));
  }

  gtk_box_append(content, GTK_WIDGET(grp_results));

  /* Save password prompt */
  if (ctx->saved_password_count > 0) {
    AdwPreferencesGroup *grp_save =
        ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(grp_save, "保存密码");
    adw_preferences_group_set_description(
        grp_save, "以下手动输入的密码解压成功，是否保存到密码库？");

    for (int i = 0; i < ctx->saved_password_count; i++) {
      AdwActionRow *row = ADW_ACTION_ROW(adw_action_row_new());
      adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), "密码");
      /* Show masked password hint */
      const char *pwd = ctx->saved_passwords[i];
      size_t plen = strlen(pwd);
      char masked[64];
      if (plen <= 2) {
        snprintf(masked, sizeof(masked), "**");
      } else {
        snprintf(masked, sizeof(masked), "%c%.*s%c", pwd[0],
                 (int)(plen - 2 > 8 ? 8 : plen - 2), "********",
                 pwd[plen - 1]);
      }
      adw_action_row_set_subtitle(row, masked);

      GtkWidget *ic =
          gtk_image_new_from_icon_name("dialog-password-symbolic");
      adw_action_row_add_prefix(row, ic);

      /* Save button */
      GtkWidget *btn_save = gtk_button_new_with_label("保存");
      gtk_widget_add_css_class(btn_save, "suggested-action");
      gtk_widget_set_valign(btn_save, GTK_ALIGN_CENTER);
      /* Store password index in widget data */
      g_object_set_data(G_OBJECT(btn_save), "pwd-index",
                        GINT_TO_POINTER(i));
      g_object_set_data(G_OBJECT(btn_save), "ctx", ctx);
      g_signal_connect(btn_save, "clicked",
                       G_CALLBACK(on_save_pwd_clicked), NULL);
      adw_action_row_add_suffix(row, btn_save);

      adw_preferences_group_add(grp_save, GTK_WIDGET(row));
    }

    gtk_box_append(content, GTK_WIDGET(grp_save));
  }

  /* Close button */
  GtkWidget *btn_close = gtk_button_new_with_label("关闭");
  gtk_widget_add_css_class(btn_close, "pill");
  gtk_widget_set_halign(btn_close, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_top(btn_close, 12);
  gtk_widget_set_margin_bottom(btn_close, 4);
  g_signal_connect_swapped(btn_close, "clicked",
                           G_CALLBACK(gtk_window_close),
                           ctx->window);
  gtk_box_append(content, btn_close);
}

/* ── Switch to DONE page ── */
void ui_gtk_switch_to_done(ExtractionContext *ctx) {
  /* Stop spinner */
  if (ctx->spinner)
    gtk_spinner_set_spinning(GTK_SPINNER(ctx->spinner), FALSE);

  populate_done_page(ctx);
  gtk_stack_set_visible_child_name(ctx->stack, "done");

  /* Send desktop notification via notify-send */
  if (ctx->success_count > 0) {
    char msg[128];
    snprintf(msg, sizeof(msg), "成功解压 %d / %d 个文件",
             ctx->success_count, ctx->total_files);
    GSubprocess *notify = g_subprocess_new(
        G_SUBPROCESS_FLAGS_NONE, NULL,
        "notify-send", "-i", "package-x-generic", "解压完成", msg, NULL);
    if (notify) g_object_unref(notify);
    log_msg("Done: %s", msg);
  } else {
    log_msg("Done: no files extracted successfully");
  }
}

static gboolean ui_gtk_finish_from_pipe(ExtractionContext *ctx, int fd) {
  ui_gtk_set_indeterminate_progress(ctx, 0);
  if (ctx->worker_thread) {
    g_thread_join(ctx->worker_thread);
    ctx->worker_thread = NULL;
  }
  ui_gtk_switch_to_done(ctx);
  ctx->pipe_source_id = 0;
  if (ctx->pipe_read_fd >= 0) {
    close(ctx->pipe_read_fd);
    ctx->pipe_read_fd = -1;
  } else {
    close(fd);
  }
  return G_SOURCE_REMOVE;
}

static gboolean ui_gtk_process_pipe_line(ExtractionContext *ctx, int fd,
                                         char *line) {
  if (!line)
    return FALSE;

  size_t len = strlen(line);
  while (len > 0 && line[len - 1] == '\r')
    line[--len] = '\0';
  if (len == 0)
    return FALSE;

  if (line[0] == '#' && line[1] == ' ') {
    const char *text = line + 2;
    if (strcmp(text, "DONE") == 0) {
      return ui_gtk_finish_from_pipe(ctx, fd) == G_SOURCE_REMOVE;
    }
    if (strcmp(text, "PULSE_START") == 0) {
      ui_gtk_set_indeterminate_progress(ctx, 1);
      return FALSE;
    }
    if (strcmp(text, "PULSE_STOP") == 0) {
      ui_gtk_set_indeterminate_progress(ctx, 0);
      return FALSE;
    }
    if (strcmp(text, "RESET_PROGRESS") == 0) {
      ui_gtk_set_indeterminate_progress(ctx, 0);
      ctx->last_progress_pct = -1;
      return FALSE;
    }
    /* Parse [X/Y] prefix from status messages to track file index */
    if (text[0] == '[') {
      int idx = 0, tot = 0;
      if (sscanf(text, "[%d/%d]", &idx, &tot) == 2 && idx > 0) {
        ctx->current_file_idx = idx;
        char status[64];
        snprintf(status, sizeof(status), "解压中 (%d/%d)", idx, tot);
        gtk_label_set_text(ctx->lbl_progress_status, status);
      }
      /* Extract the part after "] " for the file name */
      const char *after = strchr(text, ']');
      if (after) {
        after++;
        while (*after == ' ') after++;
        snprintf(ctx->current_file_name, sizeof(ctx->current_file_name),
                 "%s", after);
        gtk_label_set_text(ctx->lbl_progress_file, after);
      }
    } else {
      snprintf(ctx->current_file_name, sizeof(ctx->current_file_name),
               "%s", text);
      gtk_label_set_text(ctx->lbl_progress_file, text);
    }
    return FALSE;
  }

  char *endptr = NULL;
  long pct = strtol(line, &endptr, 10);
  if (endptr == line || *endptr != '\0')
    return FALSE;
  if (pct < 0 || pct > 100)
    return FALSE;

  int ipct = (int)pct;
  if (ctx->last_progress_pct >= 0 && ipct < ctx->last_progress_pct)
    return FALSE;
  ui_gtk_set_indeterminate_progress(ctx, 0);
  ctx->last_progress_pct = ipct;

  /* Total progress bar */
  gtk_progress_bar_set_fraction(ctx->progress_bar, ipct / 100.0);

  /* Compute current file percentage and show in file label */
  if (ctx->total_files > 1 && ctx->current_file_idx > 0) {
    double slot = 100.0 / ctx->total_files;
    double start = (ctx->current_file_idx - 1) * slot;
    int file_pct = (slot > 0.0)
                       ? (int)(((ipct - start) / slot) * 100.0)
                       : ipct;
    if (file_pct < 0) file_pct = 0;
    if (file_pct > 100) file_pct = 100;
    /* Update file label: "解压中: test.zip  45%" */
    char buf[320];
    snprintf(buf, sizeof(buf), "%s  %d%%",
             ctx->current_file_name[0] ? ctx->current_file_name : "",
             file_pct);
    gtk_label_set_text(ctx->lbl_progress_file, buf);
    /* Update status with total percentage */
    char status[64];
    snprintf(status, sizeof(status), "解压中 (%d/%d)  %d%%",
             ctx->current_file_idx, ctx->total_files, ipct);
    gtk_label_set_text(ctx->lbl_progress_status, status);
  } else {
    /* Single file: show percentage in status */
    char status[64];
    const char *file_text = ctx->current_file_name;
    if (file_text[0] && strstr(file_text, "撞库"))
      snprintf(status, sizeof(status), "撞库中... %d%%", ipct);
    else
      snprintf(status, sizeof(status), "解压中... %d%%", ipct);
    gtk_label_set_text(ctx->lbl_progress_status, status);
  }
  return FALSE;
}

/* ── Pipe callback (GTK main thread) ── */
gboolean ui_gtk_pipe_callback(int fd, GIOCondition cond, gpointer data) {
  ExtractionContext *ctx = (ExtractionContext *)data;

  if (cond & G_IO_IN) {
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      for (ssize_t i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\n') {
          ctx->pipe_line_buf[ctx->pipe_line_len] = '\0';
          if (ui_gtk_process_pipe_line(ctx, fd, ctx->pipe_line_buf))
            return G_SOURCE_REMOVE;
          ctx->pipe_line_len = 0;
          continue;
        }
        if (ctx->pipe_line_len + 1 < sizeof(ctx->pipe_line_buf)) {
          ctx->pipe_line_buf[ctx->pipe_line_len++] = ch;
        } else {
          /* Keep parser state consistent when one line is abnormally long. */
          ctx->pipe_line_len = 0;
        }
      }
      return G_SOURCE_CONTINUE;
    }
    if (n == 0) {
      if (ctx->pipe_line_len > 0) {
        ctx->pipe_line_buf[ctx->pipe_line_len] = '\0';
        if (ui_gtk_process_pipe_line(ctx, fd, ctx->pipe_line_buf))
          return G_SOURCE_REMOVE;
        ctx->pipe_line_len = 0;
      }
      return ui_gtk_finish_from_pipe(ctx, fd);
    }
    if (errno == EINTR)
      return G_SOURCE_CONTINUE;
  }

  if (cond & G_IO_HUP) {
    if (ctx->pipe_line_len > 0) {
      ctx->pipe_line_buf[ctx->pipe_line_len] = '\0';
      if (ui_gtk_process_pipe_line(ctx, fd, ctx->pipe_line_buf))
        return G_SOURCE_REMOVE;
      ctx->pipe_line_len = 0;
    }
    return ui_gtk_finish_from_pipe(ctx, fd);
  }

  return G_SOURCE_CONTINUE;
}

/* ── Start extraction (called from SETUP → PROGRESS transition) ── */
void ui_gtk_start_extraction(ExtractionContext *ctx) {
  g_atomic_int_set(&ctx->cancelled, 0);
  ui_gtk_set_indeterminate_progress(ctx, 0);
  ctx->pipe_line_len = 0;
  ctx->last_progress_pct = -1;
  gtk_progress_bar_set_fraction(ctx->progress_bar, 0.0);
  ctx->current_file_idx = 0;
  ctx->current_file_name[0] = '\0';
  gtk_label_set_text(ctx->lbl_progress_status, "准备开始...");
  gtk_label_set_text(ctx->lbl_progress_file, "");

  /* Start spinner animation */
  if (ctx->spinner)
    gtk_spinner_set_spinning(GTK_SPINNER(ctx->spinner), TRUE);

  /* Create pipe for worker → GTK communication */
  int fds[2];
  if (pipe(fds) != 0) {
    log_msg("Failed to create progress pipe");
    return;
  }
  ctx->pipe_read_fd = fds[0];
  ctx->pipe_write_fd = fds[1];

  /* Watch the read end in the GTK main loop */
  ctx->pipe_source_id = g_unix_fd_add(ctx->pipe_read_fd,
                                       G_IO_IN | G_IO_HUP,
                                       ui_gtk_pipe_callback, ctx);

  /* Spawn worker thread */
  ctx->worker_thread =
      g_thread_new("extract-worker", ui_gtk_worker_func, ctx);
}
