#define _GNU_SOURCE
#include "ui_gtk.h"
#include "../core/exec7z.h"
#include "../core/log.h"
#include "../core/path.h"
#include "../core/strbuf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Destination folder callbacks ── */
static void on_dest_folder_selected(GObject *src, GAsyncResult *res,
                                    gpointer ud) {
  ExtractionContext *ctx = (ExtractionContext *)ud;
  GtkFileDialog *dlg = GTK_FILE_DIALOG(src);
  GFile *folder = gtk_file_dialog_select_folder_finish(dlg, res, NULL);
  if (!folder)
    return;
  char *path = g_file_get_path(folder);
  g_object_unref(folder);
  if (!path)
    return;
  g_free(ctx->dest_dir);
  ctx->dest_dir = path;
  gtk_label_set_text(ctx->lbl_dest_path, path);
  gtk_widget_set_visible(GTK_WIDGET(ctx->btn_dest_reset), TRUE);
}

static void on_dest_choose_clicked(GtkButton *btn, gpointer ud) {
  (void)btn;
  ExtractionContext *ctx = (ExtractionContext *)ud;
  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "选择解压位置");
  if (ctx->dest_dir) {
    GFile *init = g_file_new_for_path(ctx->dest_dir);
    gtk_file_dialog_set_initial_folder(dlg, init);
    g_object_unref(init);
  } else if (ctx->archive_dir) {
    GFile *init = g_file_new_for_path(ctx->archive_dir);
    gtk_file_dialog_set_initial_folder(dlg, init);
    g_object_unref(init);
  }
  gtk_file_dialog_select_folder(dlg, GTK_WINDOW(ctx->window), NULL,
                                on_dest_folder_selected, ctx);
}

static void on_dest_reset_clicked(GtkButton *btn, gpointer ud) {
  (void)btn;
  ExtractionContext *ctx = (ExtractionContext *)ud;
  g_free(ctx->dest_dir);
  ctx->dest_dir = NULL;
  gtk_label_set_text(ctx->lbl_dest_path,
                     ctx->archive_dir ? ctx->archive_dir : "压缩包所在目录");
  gtk_widget_set_visible(GTK_WIDGET(ctx->btn_dest_reset), FALSE);
}

/* ── Password mode switch ── */
static void on_mode_changed(AdwComboRow *row, GParamSpec *pspec, gpointer ud) {
  (void)pspec;
  ExtractionContext *ctx = (ExtractionContext *)ud;
  guint mode = adw_combo_row_get_selected(row);
  gtk_widget_set_visible(GTK_WIDGET(ctx->grp_manual),
                         mode == PWD_MODE_MANUAL);
  gtk_widget_set_visible(GTK_WIDGET(ctx->grp_select),
                         mode == PWD_MODE_SELECT);
  gtk_widget_set_visible(GTK_WIDGET(ctx->grp_tryall),
                         mode == PWD_MODE_TRYALL);
}

/* ── Password visibility toggle ── */
static void on_pwd_visibility_toggled(GtkButton *btn, gpointer ud) {
  ExtractionContext *ctx = (ExtractionContext *)ud;
  GtkEditable *delegate = gtk_editable_get_delegate(GTK_EDITABLE(ctx->entry_pwd));
  if (!delegate || !GTK_IS_TEXT(delegate))
    return;
  gboolean visible = gtk_text_get_visibility(GTK_TEXT(delegate));
  gtk_text_set_visibility(GTK_TEXT(delegate), !visible);
  gtk_button_set_icon_name(btn, visible ? "view-reveal-symbolic"
                                        : "view-conceal-symbolic");
}

/* ── Build password library selection group ── */
static AdwPreferencesGroup *
build_pwd_select_group(ExtractionContext *ctx) {
  char desc_buf[64];
  snprintf(desc_buf, sizeof(desc_buf), "共 %zu 条密码", ctx->password_lib.len);

  AdwPreferencesGroup *grp =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(grp, "密码库");
  adw_preferences_group_set_description(grp, desc_buf);

  GtkStringList *pwd_model = gtk_string_list_new(NULL);
  for (size_t i = 0; i < ctx->password_lib.len; i++) {
    char item[256];
    const char *id = ctx->password_lib.data[i].id
                         ? ctx->password_lib.data[i].id
                         : "";
    const char *desc = ctx->password_lib.data[i].desc
                           ? ctx->password_lib.data[i].desc
                           : "";
    snprintf(item, sizeof(item), "%s — %s", id, desc);
    gtk_string_list_append(pwd_model, item);
  }

  ctx->combo_pwd_select = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ctx->combo_pwd_select),
                                "选择密码");
  GtkWidget *icon =
      gtk_image_new_from_icon_name("dialog-password-symbolic");
  adw_action_row_add_prefix(ADW_ACTION_ROW(ctx->combo_pwd_select), icon);
  adw_combo_row_set_model(ctx->combo_pwd_select, G_LIST_MODEL(pwd_model));
  if (ctx->password_lib.len > 0)
    adw_combo_row_set_selected(ctx->combo_pwd_select, 0);

  adw_preferences_group_add(grp, GTK_WIDGET(ctx->combo_pwd_select));
  return grp;
}

/* ── Build try-all info group ── */
static AdwPreferencesGroup *build_tryall_group(ExtractionContext *ctx) {
  AdwPreferencesGroup *grp =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(grp, "整库撞库");
  adw_preferences_group_set_description(grp,
                                        "将依次尝试密码库中所有密码（含编码变体）");

  AdwActionRow *r1 = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r1), "密码库条目数");
  GtkWidget *icon1 =
      gtk_image_new_from_icon_name("dialog-password-symbolic");
  adw_action_row_add_prefix(r1, icon1);
  char buf1[32];
  snprintf(buf1, sizeof(buf1), "%zu", ctx->password_lib.len);
  GtkWidget *lbl1 = gtk_label_new(buf1);
  gtk_widget_add_css_class(lbl1, "dim-label");
  adw_action_row_add_suffix(r1, lbl1);
  adw_preferences_group_add(grp, GTK_WIDGET(r1));
  ctx->row_tryall_count = r1;

  AdwActionRow *r2 = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r2), "预计尝试次数");
  GtkWidget *icon2 =
      gtk_image_new_from_icon_name("emblem-important-symbolic");
  adw_action_row_add_prefix(r2, icon2);
  char buf2[32];
  snprintf(buf2, sizeof(buf2), "≤ %zu", ctx->password_lib.len * 3);
  GtkWidget *lbl2 = gtk_label_new(buf2);
  gtk_widget_add_css_class(lbl2, "dim-label");
  adw_action_row_add_suffix(r2, lbl2);
  adw_preferences_group_add(grp, GTK_WIDGET(r2));
  ctx->row_tryall_est = r2;

  return grp;
}

/* ── Build SETUP page content ── */
void ui_gtk_build_setup_page(ExtractionContext *ctx, GtkBox *content) {
  /* File info group */
  AdwPreferencesGroup *grp_info =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(grp_info, "文件信息");

  /* Show file count and first filename */
  AdwActionRow *row_filename = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row_filename), "文件");
  if (ctx->tasks.len == 1) {
    char *fn = path_filename(ctx->tasks.data[0]);
    adw_action_row_set_subtitle(row_filename, fn ? fn : ctx->tasks.data[0]);
    free(fn);
  } else if (ctx->tasks.len > 1) {
    char buf[128];
    char *fn = path_filename(ctx->tasks.data[0]);
    snprintf(buf, sizeof(buf), "%s 等 %zu 个文件",
             fn ? fn : ctx->tasks.data[0], ctx->tasks.len);
    adw_action_row_set_subtitle(row_filename, buf);
    free(fn);
  } else {
    adw_action_row_set_subtitle(row_filename, "无可解压文件");
  }
  GtkWidget *ic_file =
      gtk_image_new_from_icon_name("package-x-generic-symbolic");
  adw_action_row_add_prefix(row_filename, ic_file);
  adw_preferences_group_add(grp_info, GTK_WIDGET(row_filename));

  AdwActionRow *row_location = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row_location), "位置");
  adw_action_row_set_subtitle(row_location,
                              ctx->archive_dir ? ctx->archive_dir : "");
  GtkWidget *ic_loc = gtk_image_new_from_icon_name("folder-symbolic");
  adw_action_row_add_prefix(row_location, ic_loc);
  adw_preferences_group_add(grp_info, GTK_WIDGET(row_location));

  gtk_box_append(content, GTK_WIDGET(grp_info));

  /* Destination group */
  AdwPreferencesGroup *grp_dest =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(grp_dest, "解压位置");
  adw_preferences_group_set_description(grp_dest,
                                        "不选择则默认解压到压缩包所在目录");

  AdwActionRow *row_dest = ADW_ACTION_ROW(adw_action_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row_dest), "目标位置");
  GtkWidget *ic_dest = gtk_image_new_from_icon_name("folder-open-symbolic");
  adw_action_row_add_prefix(row_dest, ic_dest);

  ctx->lbl_dest_path = GTK_LABEL(gtk_label_new(
      ctx->archive_dir ? ctx->archive_dir : "压缩包所在目录"));
  gtk_label_set_ellipsize(ctx->lbl_dest_path, PANGO_ELLIPSIZE_MIDDLE);
  gtk_label_set_max_width_chars(ctx->lbl_dest_path, 22);
  gtk_widget_add_css_class(GTK_WIDGET(ctx->lbl_dest_path), "dim-label");
  gtk_widget_set_valign(GTK_WIDGET(ctx->lbl_dest_path), GTK_ALIGN_CENTER);
  adw_action_row_add_suffix(row_dest, GTK_WIDGET(ctx->lbl_dest_path));

  ctx->btn_dest_reset =
      GTK_BUTTON(gtk_button_new_from_icon_name("edit-clear-symbolic"));
  gtk_widget_add_css_class(GTK_WIDGET(ctx->btn_dest_reset), "flat");
  gtk_widget_set_valign(GTK_WIDGET(ctx->btn_dest_reset), GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(GTK_WIDGET(ctx->btn_dest_reset), "恢复默认位置");
  gtk_widget_set_visible(GTK_WIDGET(ctx->btn_dest_reset), FALSE);
  g_signal_connect(ctx->btn_dest_reset, "clicked",
                   G_CALLBACK(on_dest_reset_clicked), ctx);
  adw_action_row_add_suffix(row_dest, GTK_WIDGET(ctx->btn_dest_reset));

  GtkButton *btn_dest_choose =
      GTK_BUTTON(gtk_button_new_with_label("选择"));
  gtk_widget_add_css_class(GTK_WIDGET(btn_dest_choose), "suggested-action");
  gtk_widget_set_valign(GTK_WIDGET(btn_dest_choose), GTK_ALIGN_CENTER);
  g_signal_connect(btn_dest_choose, "clicked",
                   G_CALLBACK(on_dest_choose_clicked), ctx);
  adw_action_row_add_suffix(row_dest, GTK_WIDGET(btn_dest_choose));

  adw_preferences_group_add(grp_dest, GTK_WIDGET(row_dest));
  gtk_box_append(content, GTK_WIDGET(grp_dest));

  /* Password mode group */
  AdwPreferencesGroup *grp_mode =
      ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(grp_mode, "密码");

  ctx->combo_mode = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ctx->combo_mode),
                                "密码方式");
  GtkWidget *ic_key =
      gtk_image_new_from_icon_name("channel-secure-symbolic");
  adw_action_row_add_prefix(ADW_ACTION_ROW(ctx->combo_mode), ic_key);

  const char *mode_items[] = {"手动输入", "密码库选择", "整库撞库", NULL};
  GtkStringList *model = gtk_string_list_new(mode_items);
  adw_combo_row_set_model(ctx->combo_mode, G_LIST_MODEL(model));
  adw_combo_row_set_selected(ctx->combo_mode, PWD_MODE_MANUAL);
  g_signal_connect(ctx->combo_mode, "notify::selected",
                   G_CALLBACK(on_mode_changed), ctx);

  adw_preferences_group_add(grp_mode, GTK_WIDGET(ctx->combo_mode));
  gtk_box_append(content, GTK_WIDGET(grp_mode));

  /* Manual password group */
  ctx->grp_manual = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(ctx->grp_manual, "输入密码");
  ctx->entry_pwd = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(ctx->entry_pwd), "密码");
  GtkEditable *pwd_delegate =
      gtk_editable_get_delegate(GTK_EDITABLE(ctx->entry_pwd));
  if (pwd_delegate && GTK_IS_TEXT(pwd_delegate))
    gtk_text_set_visibility(GTK_TEXT(pwd_delegate), FALSE);
  GtkWidget *btn_eye =
      gtk_button_new_from_icon_name("view-reveal-symbolic");
  gtk_widget_add_css_class(btn_eye, "flat");
  gtk_widget_set_valign(btn_eye, GTK_ALIGN_CENTER);
  gtk_widget_set_tooltip_text(btn_eye, "显示/隐藏密码");
  g_signal_connect(btn_eye, "clicked",
                   G_CALLBACK(on_pwd_visibility_toggled), ctx);
  adw_entry_row_add_suffix(ctx->entry_pwd, btn_eye);
  adw_preferences_group_add(ctx->grp_manual, GTK_WIDGET(ctx->entry_pwd));
  gtk_box_append(content, GTK_WIDGET(ctx->grp_manual));

  /* Password library selection group */
  ctx->grp_select = build_pwd_select_group(ctx);
  gtk_widget_set_visible(GTK_WIDGET(ctx->grp_select), FALSE);
  gtk_box_append(content, GTK_WIDGET(ctx->grp_select));

  /* Try-all group */
  ctx->grp_tryall = build_tryall_group(ctx);
  gtk_widget_set_visible(GTK_WIDGET(ctx->grp_tryall), FALSE);
  gtk_box_append(content, GTK_WIDGET(ctx->grp_tryall));
}

/* ── Build PROGRESS page content ── */
void ui_gtk_build_progress_page(ExtractionContext *ctx, GtkBox *content) {
  gtk_widget_set_valign(GTK_WIDGET(content), GTK_ALIGN_CENTER);
  gtk_widget_set_vexpand(GTK_WIDGET(content), TRUE);
  gtk_widget_add_css_class(GTK_WIDGET(content), "ntk-progress-page");

  /* Animated spinner */
  GtkWidget *spinner = gtk_spinner_new();
  gtk_spinner_set_spinning(GTK_SPINNER(spinner), FALSE);
  gtk_widget_set_size_request(spinner, 48, 48);
  gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_bottom(spinner, 8);
  gtk_widget_add_css_class(spinner, "ntk-progress-icon");
  gtk_box_append(content, spinner);
  ctx->spinner = spinner;

  /* Status label: "解压中 (2/5)" */
  ctx->lbl_progress_status = GTK_LABEL(gtk_label_new("准备开始..."));
  gtk_label_set_xalign(ctx->lbl_progress_status, 0.5);
  gtk_widget_add_css_class(GTK_WIDGET(ctx->lbl_progress_status), "title-3");
  gtk_widget_set_margin_bottom(GTK_WIDGET(ctx->lbl_progress_status), 4);
  gtk_box_append(content, GTK_WIDGET(ctx->lbl_progress_status));

  /* Current file name + file progress (e.g. "解压中: test.zip  45%") */
  ctx->lbl_progress_file = GTK_LABEL(gtk_label_new(""));
  gtk_label_set_xalign(ctx->lbl_progress_file, 0.5);
  gtk_label_set_ellipsize(ctx->lbl_progress_file, PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_add_css_class(GTK_WIDGET(ctx->lbl_progress_file), "dim-label");
  gtk_widget_set_margin_bottom(GTK_WIDGET(ctx->lbl_progress_file), 16);
  gtk_box_append(content, GTK_WIDGET(ctx->lbl_progress_file));

  /* Total progress bar (no text overlay — percentage shown in status) */
  ctx->progress_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
  gtk_progress_bar_set_show_text(ctx->progress_bar, FALSE);
  gtk_widget_add_css_class(GTK_WIDGET(ctx->progress_bar), "ntk-progress");
  gtk_box_append(content, GTK_WIDGET(ctx->progress_bar));
}

/* ── Build DONE page content (placeholder, filled after extraction) ── */
void ui_gtk_build_done_page(ExtractionContext *ctx, GtkBox *content) {
  ctx->done_content = content;
  gtk_widget_set_margin_start(GTK_WIDGET(content), 20);
  gtk_widget_set_margin_end(GTK_WIDGET(content), 20);
  gtk_widget_set_margin_top(GTK_WIDGET(content), 12);
  gtk_widget_set_margin_bottom(GTK_WIDGET(content), 16);
}

/* ── State transitions ── */
void ui_gtk_switch_to_progress(ExtractionContext *ctx) {
  gtk_stack_set_visible_child_name(ctx->stack, "progress");
  /* Hide extract button during progress */
  if (ctx->btn_extract)
    gtk_widget_set_visible(GTK_WIDGET(ctx->btn_extract), FALSE);
}
