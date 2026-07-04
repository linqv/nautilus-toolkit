#include "compress-dialog.h"
#include "compress-backend.h"
#include "../core/pwdlib.h"
#include <json-glib/json-glib.h>
#include <string.h>

/* ── CompressDialog type definition ── */

struct _CompressDialog {
    GtkBox parent_instance;

    /* File list */
    GList *files;

    /* Tool availability */
    ToolAvailability *tools;

    /* Header bar buttons */
    AdwHeaderBar *header_bar;
    GtkButton *cancel_btn;
    GtkButton *create_btn;

    /* Settings stack (swapped with progress) */
    GtkWidget *settings_box;
    GtkWidget *progress_box;
    GtkStack  *main_stack;

    /* Basic settings */
    AdwEntryRow *filename_row;
    AdwActionRow *location_row;
    GtkLabel    *location_label;
    char        *location_path;
    AdwComboRow *format_row;

    /* Compression options group */
    AdwPreferencesGroup *options_group;
    AdwComboRow *level_row;
    AdwEntryRow *split_row;
    AdwComboRow *split_unit_row;

    /* Encryption group */
    AdwPreferencesGroup *encrypt_group;
    AdwComboRow *pw_library_row;
    AdwPasswordEntryRow *password_row;
    AdwPasswordEntryRow *confirm_row;
    AdwSwitchRow *encrypt_header_row;

    /* Password library data */
    GPtrArray *pw_ids;     /* array of char* (display labels) */
    GPtrArray *pw_values;  /* array of char* (actual passwords) */

    /* Progress widgets */
    GtkProgressBar *progress_bar;
    GtkLabel       *progress_label;
    GtkLabel       *progress_title;

    /* Batch compress */
    AdwSwitchRow *batch_row;
    gboolean      multi_select;

    /* Running task / batch state */
    CompressTask *running_task;
    GList        *batch_tasks;     /* GList of CompressTask* for batch mode */
    int           batch_total;
    int           batch_current;
    GtkWindow    *host_window;
    GtkWindow    *owner_window;
    gboolean      embedded_mode;
};

G_DEFINE_TYPE(CompressDialog, compress_dialog, GTK_TYPE_BOX)

/* Format mapping: index in combo → CompressFormat enum */
/* PLACEHOLDER - will be filled dynamically based on tool availability */
static CompressFormat format_map[FORMAT_COUNT];
static int format_map_count = 0;

/* ── Level definitions per format ── */

typedef struct {
    const char *label;
    int         value_7z;
    int         value_xz;
    int         value_zst;
} LevelDef;

static const LevelDef levels_7z[] = {
    { "存储", 0, 0, 0 }, { "最快", 1, 0, 0 }, { "快速", 3, 0, 0 },
    { "标准", 5, 0, 0 }, { "较高", 7, 0, 0 }, { "最大", 9, 0, 0 },
    { "极限", 10, 0, 0 },
};
static const LevelDef levels_zip[] = {
    { "存储", 0, 0, 0 }, { "最快", 1, 0, 0 }, { "快速", 3, 0, 0 },
    { "标准", 5, 0, 0 }, { "较高", 7, 0, 0 }, { "最大", 9, 0, 0 },
};
static const LevelDef levels_xz[] = {
    { "最快", 0, 1, 0 }, { "快速", 0, 3, 0 }, { "标准", 0, 6, 0 },
    { "较高", 0, 7, 0 }, { "最大", 0, 9, 0 },
};
static const LevelDef levels_zst[] = {
    { "最快", 0, 0, 1 }, { "快速", 0, 0, 3 }, { "标准", 0, 0, 6 },
    { "较高", 0, 0, 15 }, { "最大", 0, 0, 19 },
};

static int
get_actual_level(CompressFormat fmt, int combo_index)
{
    switch (fmt) {
    case FORMAT_7Z: case FORMAT_7Z_SPLIT:
        if (combo_index >= 0 && combo_index < 7) return levels_7z[combo_index].value_7z;
        return 5;
    case FORMAT_ZIP: case FORMAT_ZIP_SPLIT:
        if (combo_index >= 0 && combo_index < 6) return levels_zip[combo_index].value_7z;
        return 5;
    case FORMAT_TAR_XZ:
        if (combo_index >= 0 && combo_index < 5) return levels_xz[combo_index].value_xz;
        return 6;
    case FORMAT_TAR_ZST:
        if (combo_index >= 0 && combo_index < 5) return levels_zst[combo_index].value_zst;
        return 6;
    default:
        return 0;
    }
}

/* ── Password library ── */
static char *
password_file_path(void)
{
    char *path = password_lib_path();
    return path ? path : g_strdup("passwords.json");
}

static void
load_password_library(CompressDialog *self)
{
    self->pw_ids = g_ptr_array_new_with_free_func(g_free);
    self->pw_values = g_ptr_array_new_with_free_func(g_free);

    /* First entry: "不使用" (manual input) */
    g_ptr_array_add(self->pw_ids, g_strdup("手动输入"));
    g_ptr_array_add(self->pw_values, g_strdup(""));

    char *pw_path = password_file_path();
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, pw_path, NULL)) {
        g_free(pw_path);
        g_object_unref(parser);
        return;
    }
    g_free(pw_path);

    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return;
    }

    JsonObject *obj = json_node_get_object(root);
    JsonArray *arr = json_object_get_array_member(obj, "passwords");
    if (!arr) {
        g_object_unref(parser);
        return;
    }

    guint len = json_array_get_length(arr);
    for (guint i = 0; i < len; i++) {
        JsonObject *entry = json_array_get_object_element(arr, i);
        if (!entry) continue;

        const char *id = json_object_get_string_member_with_default(entry, "id", NULL);
        const char *value = json_object_get_string_member_with_default(entry, "value", NULL);
        if (!id || !value) continue;

        g_ptr_array_add(self->pw_ids, g_strdup(id));
        g_ptr_array_add(self->pw_values, g_strdup(value));
    }

    g_object_unref(parser);
}

static void
on_pw_library_changed(AdwComboRow *row, GParamSpec *pspec G_GNUC_UNUSED,
                      CompressDialog *self)
{
    guint idx = adw_combo_row_get_selected(row);
    if (idx == 0 || idx >= self->pw_values->len) {
        if (self->password_row && GTK_IS_EDITABLE(self->password_row))
            gtk_editable_set_text(GTK_EDITABLE(self->password_row), "");
        if (self->confirm_row && GTK_IS_EDITABLE(self->confirm_row))
            gtk_editable_set_text(GTK_EDITABLE(self->confirm_row), "");
        if (self->password_row)
            gtk_widget_set_sensitive(GTK_WIDGET(self->password_row), TRUE);
        if (self->confirm_row)
            gtk_widget_set_sensitive(GTK_WIDGET(self->confirm_row), TRUE);
        return;
    }

    const char *pw = g_ptr_array_index(self->pw_values, idx);
    if (self->password_row && GTK_IS_EDITABLE(self->password_row))
        gtk_editable_set_text(GTK_EDITABLE(self->password_row), pw);
    if (self->confirm_row && GTK_IS_EDITABLE(self->confirm_row))
        gtk_editable_set_text(GTK_EDITABLE(self->confirm_row), pw);
    if (self->password_row)
        gtk_widget_set_sensitive(GTK_WIDGET(self->password_row), FALSE);
    if (self->confirm_row)
        gtk_widget_set_sensitive(GTK_WIDGET(self->confirm_row), FALSE);
}

/* ── Format change handler ── */

static gboolean
format_supports_encryption(CompressFormat fmt)
{
    return fmt == FORMAT_7Z || fmt == FORMAT_7Z_SPLIT ||
           fmt == FORMAT_ZIP || fmt == FORMAT_ZIP_SPLIT;
}

static gboolean
format_supports_split(CompressFormat fmt)
{
    return fmt == FORMAT_7Z || fmt == FORMAT_7Z_SPLIT ||
           fmt == FORMAT_ZIP || fmt == FORMAT_ZIP_SPLIT;
}

static gboolean
format_supports_level(CompressFormat fmt)
{
    return fmt == FORMAT_7Z || fmt == FORMAT_7Z_SPLIT ||
           fmt == FORMAT_ZIP || fmt == FORMAT_ZIP_SPLIT ||
           fmt == FORMAT_TAR_XZ || fmt == FORMAT_TAR_ZST;
}

static void
update_batch_row_state(CompressDialog *self)
{
    if (!self || !self->batch_row)
        return;

    gtk_widget_set_visible(GTK_WIDGET(self->batch_row), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->batch_row), self->multi_select);
    if (!self->multi_select)
        adw_switch_row_set_active(self->batch_row, FALSE);

    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(self->batch_row),
        self->multi_select ? "每个文件/文件夹单独压缩" : "仅多选时可用");
}

static void
update_level_options(CompressDialog *self, CompressFormat fmt)
{
    const LevelDef *defs = NULL;
    int count = 0;
    int default_idx = 3;

    switch (fmt) {
    case FORMAT_7Z: case FORMAT_7Z_SPLIT:
        defs = levels_7z; count = 7; default_idx = 3; break;
    case FORMAT_ZIP: case FORMAT_ZIP_SPLIT:
        defs = levels_zip; count = 6; default_idx = 3; break;
    case FORMAT_TAR_XZ:
        defs = levels_xz; count = 5; default_idx = 2; break;
    case FORMAT_TAR_ZST:
        defs = levels_zst; count = 5; default_idx = 2; break;
    default:
        break;
    }

    GtkStringList *model = gtk_string_list_new(NULL);
    for (int i = 0; i < count; i++)
        gtk_string_list_append(model, defs[i].label);

    adw_combo_row_set_model(self->level_row, G_LIST_MODEL(model));
    g_object_unref(model);

    if (count > 0)
        adw_combo_row_set_selected(self->level_row, default_idx);
}

static void
on_format_changed(AdwComboRow *row, GParamSpec *pspec G_GNUC_UNUSED, CompressDialog *self)
{
    guint idx = adw_combo_row_get_selected(row);
    if (idx >= (guint)format_map_count) return;
    CompressFormat fmt = format_map[idx];

    /* Show/hide encryption group */
    gtk_widget_set_visible(GTK_WIDGET(self->encrypt_group), format_supports_encryption(fmt));

    /* Encrypt header only for 7z */
    gtk_widget_set_visible(GTK_WIDGET(self->encrypt_header_row),
                           fmt == FORMAT_7Z || fmt == FORMAT_7Z_SPLIT);

    /* Show/hide split row */
    gtk_widget_set_visible(GTK_WIDGET(self->split_row), format_supports_split(fmt));
    gtk_widget_set_visible(GTK_WIDGET(self->split_unit_row), format_supports_split(fmt));

    /* Show/hide level row and update options */
    gboolean has_level = format_supports_level(fmt);
    gtk_widget_set_visible(GTK_WIDGET(self->level_row), has_level);
    if (has_level)
        update_level_options(self, fmt);

    /* Show/hide entire options group */
    gtk_widget_set_visible(GTK_WIDGET(self->options_group),
                           has_level || format_supports_split(fmt));

    update_batch_row_state(self);
}

/* ── Validation ── */

static gboolean
validate_inputs(CompressDialog *self)
{
    const char *name = gtk_editable_get_text(GTK_EDITABLE(self->filename_row));
    if (!name || !name[0]) return FALSE;
    if (!self->location_path || !self->location_path[0]) return FALSE;

    /* Password consistency */
    const char *pw1 = gtk_editable_get_text(GTK_EDITABLE(self->password_row));
    const char *pw2 = gtk_editable_get_text(GTK_EDITABLE(self->confirm_row));
    if (pw1 && pw1[0] && g_strcmp0(pw1, pw2) != 0) return FALSE;

    /* Split size validation */
    guint fmt_idx = adw_combo_row_get_selected(self->format_row);
    if (fmt_idx < (guint)format_map_count && format_supports_split(format_map[fmt_idx])) {
        const char *split = gtk_editable_get_text(GTK_EDITABLE(self->split_row));
        if (split && split[0]) {
            char *end;
            long val = strtol(split, &end, 10);
            if (*end != '\0' || val <= 0) return FALSE;
        }
    }
    return TRUE;
}

static void
on_input_changed(GtkEditable *editable G_GNUC_UNUSED, CompressDialog *self)
{
    gtk_widget_set_sensitive(GTK_WIDGET(self->create_btn), validate_inputs(self));
}

/* ── Progress and finish callbacks ── */

static void run_next_batch_task(CompressDialog *self);

static void
on_progress(double fraction, const char *current_file, gpointer user_data)
{
    CompressDialog *self = (CompressDialog *)user_data;
    if (self->batch_total > 1) {
        /* Aggregate progress: each task contributes 1/batch_total */
        double overall = ((self->batch_current - 1) + fraction) / self->batch_total;
        gtk_progress_bar_set_fraction(self->progress_bar, overall);
        char *title = g_strdup_printf("正在压缩… (%d/%d)", self->batch_current, self->batch_total);
        gtk_label_set_text(self->progress_title, title);
        g_free(title);
    } else {
        gtk_progress_bar_set_fraction(self->progress_bar, fraction);
    }
    if (current_file)
        gtk_label_set_text(self->progress_label, current_file);
}

static GtkWindow *
dialog_parent_window(CompressDialog *self)
{
    if (self->embedded_mode) {
        GtkRoot *root = NULL;
        if (self->main_stack)
            root = gtk_widget_get_root(GTK_WIDGET(self->main_stack));
        if (root && GTK_IS_WINDOW(root))
            return GTK_WINDOW(root);
        if (self->host_window)
            return self->host_window;
    }
    return self->owner_window;
}

static void
on_finish(gboolean success, const char *error_msg, gpointer user_data)
{
    CompressDialog *self = (CompressDialog *)user_data;
    self->running_task = NULL;

    if (success) {
        /* Check if there are more batch tasks */
        if (self->batch_tasks) {
            run_next_batch_task(self);
            return;
        }
        /* Send desktop notification via notify-send */
        const char *name = gtk_editable_get_text(GTK_EDITABLE(self->filename_row));
        char *body;
        if (self->batch_total > 1)
            body = g_strdup_printf("已完成 %d 个压缩任务", self->batch_total);
        else
            body = g_strdup_printf("%s 压缩完成", name ? name : "文件");
        GSubprocess *notify = g_subprocess_new(
            G_SUBPROCESS_FLAGS_NONE, NULL,
            "notify-send", "-i", "package-x-generic", "压缩完成", body, NULL);
        if (notify) g_object_unref(notify);
        g_free(body);
        if (self->embedded_mode) {
            gtk_stack_set_visible_child_name(self->main_stack, "settings");
            gtk_widget_set_sensitive(GTK_WIDGET(self->create_btn), TRUE);
            gtk_widget_set_sensitive(GTK_WIDGET(self->cancel_btn), TRUE);
            gtk_progress_bar_set_fraction(self->progress_bar, 0.0);
            gtk_label_set_text(self->progress_title, "正在压缩…");
            gtk_label_set_text(self->progress_label, "");
        } else if (self->owner_window) {
            gtk_window_close(self->owner_window);
        }
        return;
    }

    /* Show error dialog */
    AdwAlertDialog *dlg = ADW_ALERT_DIALOG(adw_alert_dialog_new(
        "压缩失败", error_msg));
    adw_alert_dialog_add_response(dlg, "ok", "确定");
    adw_alert_dialog_set_default_response(dlg, "ok");
    adw_dialog_present(ADW_DIALOG(dlg), GTK_WIDGET(dialog_parent_window(self)));

    /* Clean up remaining batch tasks */
    g_list_free_full(self->batch_tasks, (GDestroyNotify)compress_task_free);
    self->batch_tasks = NULL;

    /* Switch back to settings */
    gtk_stack_set_visible_child_name(self->main_stack, "settings");
    gtk_widget_set_sensitive(GTK_WIDGET(self->create_btn), TRUE);
    gtk_widget_set_sensitive(GTK_WIDGET(self->cancel_btn), TRUE);
    gtk_label_set_text(self->progress_title, "正在压缩…");
}

static void
run_next_batch_task(CompressDialog *self)
{
    if (!self->batch_tasks) return;

    CompressTask *task = self->batch_tasks->data;
    self->batch_tasks = g_list_delete_link(self->batch_tasks, self->batch_tasks);
    self->batch_current++;

    char *title = g_strdup_printf("正在压缩… (%d/%d)", self->batch_current, self->batch_total);
    gtk_label_set_text(self->progress_title, title);
    g_free(title);
    /* Set progress to aggregate baseline — don't reset to 0 */
    gtk_progress_bar_set_fraction(self->progress_bar,
        (double)(self->batch_current - 1) / self->batch_total);

    self->running_task = task;
    compress_backend_run_async(task, on_progress, on_finish, self);
}

/* ── Cancel compression ── */

static void
on_cancel_clicked(GtkButton *btn G_GNUC_UNUSED, CompressDialog *self)
{
    GtkWindow *parent = dialog_parent_window(self);
    if (parent)
        gtk_window_close(parent);
}

/* ── Location chooser ── */

static void
on_location_selected(GObject *source, GAsyncResult *result, gpointer user_data)
{
    CompressDialog *self = (CompressDialog *)user_data;
    GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
    GFile *folder = gtk_file_dialog_select_folder_finish(dlg, result, NULL);
    if (folder) {
        g_free(self->location_path);
        self->location_path = g_file_get_path(folder);
        gtk_label_set_text(self->location_label, self->location_path);
        g_object_unref(folder);
        on_input_changed(NULL, self);
    }
}

static void
on_choose_location(GtkButton *btn G_GNUC_UNUSED, CompressDialog *self)
{
    GtkFileDialog *dlg = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dlg, "选择保存位置");
    if (self->location_path) {
        GFile *initial = g_file_new_for_path(self->location_path);
        gtk_file_dialog_set_initial_folder(dlg, initial);
        g_object_unref(initial);
    }
    gtk_file_dialog_select_folder(dlg, dialog_parent_window(self), NULL,
                                  on_location_selected, self);
    g_object_unref(dlg);
}

/* ── Create button handler ── */

/* Helper: build a CompressTask for the given source files and output name */
static CompressTask *
build_task(CompressDialog *self, CompressFormat fmt,
           const char *output_name, char **sources, int source_count,
           const char *split_text, gboolean has_split)
{
    const char *ext = compress_format_extension(fmt);
    char *name_ext = g_strdup_printf("%s%s", output_name, ext);
    char *output = g_build_filename(self->location_path, name_ext, NULL);
    g_free(name_ext);

    CompressTask *task = g_new0(CompressTask, 1);
    task->format = fmt;
    task->output_path = output;
    task->cancellable = g_cancellable_new();
    task->source_files = g_new0(char *, source_count + 1);
    task->source_count = source_count;
    for (int i = 0; i < source_count; i++)
        task->source_files[i] = g_strdup(sources[i]);

    if (format_supports_level(fmt))
        task->compress_level = get_actual_level(fmt, adw_combo_row_get_selected(self->level_row));

    if (format_supports_encryption(fmt)) {
        const char *pw = gtk_editable_get_text(GTK_EDITABLE(self->password_row));
        if (pw && pw[0])
            task->password = g_strdup(pw);
    }

    task->encrypt_header = adw_switch_row_get_active(self->encrypt_header_row);

    if (has_split) {
        guint unit_idx = adw_combo_row_get_selected(self->split_unit_row);
        const char *unit = (unit_idx == 0) ? "m" : "g";
        task->volume_size = g_strdup_printf("%s%s", split_text, unit);
    }

    return task;
}

static void
on_create_clicked(GtkButton *btn G_GNUC_UNUSED, CompressDialog *self)
{
    if (!validate_inputs(self)) return;

    guint fmt_idx = adw_combo_row_get_selected(self->format_row);
    CompressFormat fmt = format_map[fmt_idx];

    /* Determine if this is a split variant */
    const char *split_text = gtk_editable_get_text(GTK_EDITABLE(self->split_row));
    gboolean has_split = (split_text && split_text[0] && format_supports_split(fmt));
    if (has_split) {
        if (fmt == FORMAT_7Z) fmt = FORMAT_7Z_SPLIT;
        else if (fmt == FORMAT_ZIP) fmt = FORMAT_ZIP_SPLIT;
    }

    gboolean batch_mode = self->multi_select &&
                          adw_switch_row_get_active(self->batch_row);

    /* Switch to progress view */
    gtk_stack_set_visible_child(self->main_stack, self->progress_box);
    gtk_progress_bar_set_fraction(self->progress_bar, 0.0);
    gtk_label_set_text(self->progress_label, "正在准备…");
    gtk_widget_set_sensitive(GTK_WIDGET(self->create_btn), FALSE);

    if (batch_mode) {
        /* Create one task per source file/folder */
        self->batch_tasks = NULL;
        for (GList *l = self->files; l; l = l->next) {
            char *path = l->data;
            char *basename = g_path_get_basename(path);
            /* Remove extension for files */
            if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
                char *dot = strrchr(basename, '.');
                if (dot && dot != basename)
                    *dot = '\0';
            }
            CompressTask *task = build_task(self, fmt, basename,
                                            &path, 1, split_text, has_split);
            self->batch_tasks = g_list_append(self->batch_tasks, task);
            g_free(basename);
        }
        self->batch_total = g_list_length(self->batch_tasks);
        self->batch_current = 0;
        run_next_batch_task(self);
    } else {
        /* Single task with all files */
        const char *name = gtk_editable_get_text(GTK_EDITABLE(self->filename_row));
        int count = g_list_length(self->files);
        char **sources = g_new0(char *, count + 1);
        int i = 0;
        for (GList *l = self->files; l; l = l->next, i++)
            sources[i] = l->data;

        CompressTask *task = build_task(self, fmt, name,
                                        sources, count, split_text, has_split);
        g_free(sources); /* shallow free, strings owned by self->files */

        self->batch_total = 1;
        self->batch_current = 1;
        self->running_task = task;
        compress_backend_run_async(task, on_progress, on_finish, self);
    }
}

/* ── GObject lifecycle ── */

static void
compress_dialog_dispose(GObject *object)
{
    CompressDialog *self = COMPRESS_DIALOG(object);

    self->host_window = NULL;
    self->owner_window = NULL;

    if (self->running_task) {
        compress_backend_cancel(self->running_task);
        self->running_task = NULL;
    }

    /* Clean up remaining batch tasks */
    g_list_free_full(self->batch_tasks, (GDestroyNotify)compress_task_free);
    self->batch_tasks = NULL;

    G_OBJECT_CLASS(compress_dialog_parent_class)->dispose(object);
}

static void
compress_dialog_finalize(GObject *object)
{
    CompressDialog *self = COMPRESS_DIALOG(object);
    g_list_free_full(self->files, g_free);
    g_free(self->location_path);
    compress_tools_free(self->tools);
    if (self->pw_ids) g_ptr_array_unref(self->pw_ids);
    if (self->pw_values) {
        /* Zero out password values before freeing */
        for (guint i = 0; i < self->pw_values->len; i++) {
            char *v = g_ptr_array_index(self->pw_values, i);
            if (v && v[0]) explicit_bzero(v, strlen(v));
        }
        g_ptr_array_unref(self->pw_values);
    }
    G_OBJECT_CLASS(compress_dialog_parent_class)->finalize(object);
}

static void
compress_dialog_class_init(CompressDialogClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    object_class->dispose = compress_dialog_dispose;
    object_class->finalize = compress_dialog_finalize;
}

/* ── Build UI in instance init ── */

static void
compress_dialog_init(CompressDialog *self)
{
    self->host_window = NULL;
    self->owner_window = NULL;
    self->embedded_mode = FALSE;
    self->tools = compress_tools_detect();
    gtk_orientable_set_orientation(GTK_ORIENTABLE(self), GTK_ORIENTATION_VERTICAL);

    /* ── Main layout: AdwToolbarView ── */
    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());

    /* Header bar */
    AdwHeaderBar *header = ADW_HEADER_BAR(adw_header_bar_new());
    self->header_bar = header;
    self->cancel_btn = GTK_BUTTON(gtk_button_new_with_label("关闭"));
    self->create_btn = GTK_BUTTON(gtk_button_new_with_label("创建"));
    gtk_widget_add_css_class(GTK_WIDGET(self->create_btn), "suggested-action");
    gtk_widget_set_sensitive(GTK_WIDGET(self->create_btn), FALSE);
    adw_header_bar_pack_start(header, GTK_WIDGET(self->cancel_btn));
    adw_header_bar_pack_end(header, GTK_WIDGET(self->create_btn));
    adw_toolbar_view_add_top_bar(toolbar_view, GTK_WIDGET(header));

    g_signal_connect(self->cancel_btn, "clicked",
                     G_CALLBACK(on_cancel_clicked), self);
    g_signal_connect(self->create_btn, "clicked",
                     G_CALLBACK(on_create_clicked), self);

    /* ── Stack for settings vs progress ── */
    self->main_stack = GTK_STACK(gtk_stack_new());
    gtk_stack_set_transition_type(self->main_stack, GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(self->main_stack, 200);

    /* ── Settings page ── */
    self->settings_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 24);
    gtk_widget_set_margin_start(self->settings_box, 24);
    gtk_widget_set_margin_end(self->settings_box, 24);
    gtk_widget_set_margin_top(self->settings_box, 12);
    gtk_widget_set_margin_bottom(self->settings_box, 24);

    /* -- Basic settings group -- */
    AdwPreferencesGroup *basic_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(basic_group, "基本设置");

    /* Filename row */
    self->filename_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->filename_row), "文件名");
    g_signal_connect(self->filename_row, "changed",
                     G_CALLBACK(on_input_changed), self);
    adw_preferences_group_add(basic_group, GTK_WIDGET(self->filename_row));

    /* Location row */
    self->location_row = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->location_row), "位置");
    self->location_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_ellipsize(self->location_label, PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_set_hexpand(GTK_WIDGET(self->location_label), TRUE);
    gtk_widget_set_halign(GTK_WIDGET(self->location_label), GTK_ALIGN_END);
    GtkButton *loc_btn = GTK_BUTTON(gtk_button_new_with_label("选择"));
    gtk_widget_set_valign(GTK_WIDGET(loc_btn), GTK_ALIGN_CENTER);
    g_signal_connect(loc_btn, "clicked", G_CALLBACK(on_choose_location), self);
    adw_action_row_add_suffix(self->location_row, GTK_WIDGET(self->location_label));
    adw_action_row_add_suffix(self->location_row, GTK_WIDGET(loc_btn));
    adw_preferences_group_add(basic_group, GTK_WIDGET(self->location_row));

    /* Format row */
    self->format_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->format_row), "格式");
    GtkStringList *fmt_model = gtk_string_list_new(NULL);
    format_map_count = 0;
    for (int f = 0; f < FORMAT_COUNT; f++) {
        /* Skip split variants - they are auto-selected when split size is set */
        if (f == FORMAT_7Z_SPLIT || f == FORMAT_ZIP_SPLIT) continue;
        if (compress_format_available(self->tools, f)) {
            gtk_string_list_append(fmt_model, compress_format_display_name(f));
            format_map[format_map_count++] = f;
        }
    }
    adw_combo_row_set_model(self->format_row, G_LIST_MODEL(fmt_model));
    g_object_unref(fmt_model);
    g_signal_connect(self->format_row, "notify::selected",
                     G_CALLBACK(on_format_changed), self);
    adw_preferences_group_add(basic_group, GTK_WIDGET(self->format_row));

    /* Batch compress row */
    self->batch_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->batch_row), "分别压缩");
    adw_action_row_set_subtitle(ADW_ACTION_ROW(self->batch_row), "每个文件/文件夹单独压缩");
    adw_preferences_group_add(basic_group, GTK_WIDGET(self->batch_row));

    gtk_box_append(GTK_BOX(self->settings_box), GTK_WIDGET(basic_group));

    /* -- Compression options group -- */
    self->options_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->options_group, "压缩选项");

    self->level_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->level_row), "压缩等级");
    adw_preferences_group_add(self->options_group, GTK_WIDGET(self->level_row));

    self->split_row = ADW_ENTRY_ROW(adw_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->split_row), "分卷大小");
    g_signal_connect(self->split_row, "changed",
                     G_CALLBACK(on_input_changed), self);
    adw_preferences_group_add(self->options_group, GTK_WIDGET(self->split_row));

    self->split_unit_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->split_unit_row), "分卷单位");
    GtkStringList *unit_model = gtk_string_list_new((const char *[]){ "MB", "GB", NULL });
    adw_combo_row_set_model(self->split_unit_row, G_LIST_MODEL(unit_model));
    g_object_unref(unit_model);
    adw_preferences_group_add(self->options_group, GTK_WIDGET(self->split_unit_row));

    gtk_box_append(GTK_BOX(self->settings_box), GTK_WIDGET(self->options_group));

    /* -- Encryption group -- */
    self->encrypt_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(self->encrypt_group, "加密");

    /* Password library combo */
    load_password_library(self);
    self->pw_library_row = ADW_COMBO_ROW(adw_combo_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->pw_library_row), "密码库");
    GtkStringList *pw_model = gtk_string_list_new(NULL);
    for (guint i = 0; i < self->pw_ids->len; i++)
        gtk_string_list_append(pw_model, g_ptr_array_index(self->pw_ids, i));
    adw_combo_row_set_model(self->pw_library_row, G_LIST_MODEL(pw_model));
    g_object_unref(pw_model);
    g_signal_connect(self->pw_library_row, "notify::selected",
                     G_CALLBACK(on_pw_library_changed), self);
    adw_preferences_group_add(self->encrypt_group, GTK_WIDGET(self->pw_library_row));

    self->password_row = ADW_PASSWORD_ENTRY_ROW(adw_password_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->password_row), "密码");
    g_signal_connect(self->password_row, "changed",
                     G_CALLBACK(on_input_changed), self);
    adw_preferences_group_add(self->encrypt_group, GTK_WIDGET(self->password_row));

    self->confirm_row = ADW_PASSWORD_ENTRY_ROW(adw_password_entry_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->confirm_row), "确认密码");
    g_signal_connect(self->confirm_row, "changed",
                     G_CALLBACK(on_input_changed), self);
    adw_preferences_group_add(self->encrypt_group, GTK_WIDGET(self->confirm_row));

    self->encrypt_header_row = ADW_SWITCH_ROW(adw_switch_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(self->encrypt_header_row), "加密文件名");
    adw_preferences_group_add(self->encrypt_group, GTK_WIDGET(self->encrypt_header_row));

    gtk_box_append(GTK_BOX(self->settings_box), GTK_WIDGET(self->encrypt_group));

    /* Wrap settings in scrolled window */
    GtkScrolledWindow *scroll = GTK_SCROLLED_WINDOW(gtk_scrolled_window_new());
    gtk_scrolled_window_set_policy(scroll, GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(scroll, TRUE);
    gtk_scrolled_window_set_max_content_height(scroll, 420);
    gtk_widget_set_vexpand(GTK_WIDGET(scroll), TRUE);
    gtk_scrolled_window_set_child(scroll, self->settings_box);

    gtk_stack_add_named(self->main_stack, GTK_WIDGET(scroll), "settings");

    /* ── Progress page ── */
    self->progress_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(self->progress_box, 24);
    gtk_widget_set_margin_end(self->progress_box, 24);
    gtk_widget_set_margin_top(self->progress_box, 24);
    gtk_widget_set_margin_bottom(self->progress_box, 24);
    gtk_widget_set_valign(self->progress_box, GTK_ALIGN_CENTER);

    /* Animated spinner */
    GtkWidget *compress_spinner = gtk_spinner_new();
    gtk_spinner_set_spinning(GTK_SPINNER(compress_spinner), TRUE);
    gtk_widget_set_size_request(compress_spinner, 48, 48);
    gtk_widget_set_halign(compress_spinner, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_bottom(compress_spinner, 8);
    gtk_widget_add_css_class(compress_spinner, "ntk-progress-icon");
    gtk_box_append(GTK_BOX(self->progress_box), compress_spinner);

    self->progress_title = GTK_LABEL(gtk_label_new("正在压缩…"));
    gtk_widget_add_css_class(GTK_WIDGET(self->progress_title), "title-3");
    gtk_widget_set_margin_bottom(GTK_WIDGET(self->progress_title), 4);
    gtk_box_append(GTK_BOX(self->progress_box), GTK_WIDGET(self->progress_title));

    self->progress_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_progress_bar_set_show_text(self->progress_bar, TRUE);
    gtk_widget_add_css_class(GTK_WIDGET(self->progress_bar), "ntk-progress");
    gtk_box_append(GTK_BOX(self->progress_box), GTK_WIDGET(self->progress_bar));

    self->progress_label = GTK_LABEL(gtk_label_new(""));
    gtk_label_set_ellipsize(self->progress_label, PANGO_ELLIPSIZE_MIDDLE);
    gtk_widget_add_css_class(GTK_WIDGET(self->progress_label), "dim-label");
    gtk_widget_set_margin_top(GTK_WIDGET(self->progress_label), 4);
    gtk_box_append(GTK_BOX(self->progress_box), GTK_WIDGET(self->progress_label));

    gtk_stack_add_named(self->main_stack, self->progress_box, "progress");
    gtk_stack_set_visible_child_name(self->main_stack, "settings");

    adw_toolbar_view_set_content(toolbar_view, GTK_WIDGET(self->main_stack));
    gtk_box_append(GTK_BOX(self), GTK_WIDGET(toolbar_view));

    /* Trigger initial format update */
    update_batch_row_state(self);
    if (format_map_count > 0)
        on_format_changed(self->format_row, NULL, self);
}

/* ── Public API ── */

void
compress_dialog_show(GtkWindow *parent, GList *files)
{
    CompressDialog *self = g_object_new(COMPRESS_TYPE_DIALOG, NULL);
    AdwWindow *window = ADW_WINDOW(adw_window_new());
    self->owner_window = GTK_WINDOW(window);
    gtk_window_set_default_size(GTK_WINDOW(window), 520, 480);
    gtk_window_set_resizable(GTK_WINDOW(window), FALSE);
    gtk_window_set_modal(GTK_WINDOW(window), TRUE);
    gtk_window_set_title(GTK_WINDOW(window), "压缩");
    adw_window_set_content(window, GTK_WIDGET(self));

    if (parent)
        gtk_window_set_transient_for(GTK_WINDOW(window), parent);

    /* Copy file list */
    self->files = NULL;
    for (GList *l = files; l; l = l->next)
        self->files = g_list_append(self->files, g_strdup(l->data));

    /* Set default filename */
    int count = g_list_length(self->files);
    char *default_name = NULL;
    if (count == 1) {
        const char *path = self->files->data;
        char *basename = g_path_get_basename(path);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            default_name = basename;
        } else {
            /* Remove extension */
            char *dot = strrchr(basename, '.');
            if (dot && dot != basename)
                *dot = '\0';
            default_name = basename;
        }
    } else if (count > 1) {
        /* Use parent directory name */
        const char *first = self->files->data;
        char *dir = g_path_get_dirname(first);
        default_name = g_path_get_basename(dir);
        g_free(dir);
    }
    if (default_name && self->filename_row && GTK_IS_EDITABLE(self->filename_row)) {
        gtk_editable_set_text(GTK_EDITABLE(self->filename_row), default_name);
        g_free(default_name);
    } else {
        g_free(default_name);
    }

    /* Set default location */
    if (self->files) {
        const char *first = self->files->data;
        self->location_path = g_path_get_dirname(first);
        gtk_label_set_text(self->location_label, self->location_path);
    }

    /* Show batch option for multi-selection */
    self->multi_select = (count > 1);
    update_batch_row_state(self);

    on_input_changed(NULL, self);
    gtk_window_present(GTK_WINDOW(window));
}

void
compress_dialog_show_in_container(GtkWindow *parent_window,
                                  GList *files,
                                  GtkBox *container)
{
    CompressDialog *self = g_object_new(COMPRESS_TYPE_DIALOG, NULL);
    self->embedded_mode = TRUE;
    self->host_window = parent_window;
    self->owner_window = NULL;

    /* In embedded mode, the toolkit window already has its own header bar
       with view switcher.  Strip the compress dialog's header down to just
       the "创建" button. */
    gtk_widget_set_visible(GTK_WIDGET(self->cancel_btn), FALSE);
    adw_header_bar_set_show_title(self->header_bar, FALSE);
    adw_header_bar_set_show_end_title_buttons(self->header_bar, FALSE);
    adw_header_bar_set_show_start_title_buttons(self->header_bar, FALSE);

    self->files = NULL;
    for (GList *l = files; l; l = l->next)
        self->files = g_list_append(self->files, g_strdup(l->data));

    int count = g_list_length(self->files);
    char *default_name = NULL;
    if (count == 1) {
        const char *path = self->files->data;
        char *basename = g_path_get_basename(path);
        if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
            default_name = basename;
        } else {
            char *dot = strrchr(basename, '.');
            if (dot && dot != basename)
                *dot = '\0';
            default_name = basename;
        }
    } else if (count > 1) {
        const char *first = self->files->data;
        char *dir = g_path_get_dirname(first);
        default_name = g_path_get_basename(dir);
        g_free(dir);
    }

    if (default_name && self->filename_row && GTK_IS_EDITABLE(self->filename_row)) {
        gtk_editable_set_text(GTK_EDITABLE(self->filename_row), default_name);
        g_free(default_name);
    } else {
        g_free(default_name);
    }

    if (self->files) {
        const char *first = self->files->data;
        self->location_path = g_path_get_dirname(first);
        gtk_label_set_text(self->location_label, self->location_path);
    }

    self->multi_select = (count > 1);
    update_batch_row_state(self);
    on_input_changed(NULL, self);
    gtk_box_append(container, GTK_WIDGET(self));
}
