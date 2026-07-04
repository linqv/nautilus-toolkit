#pragma once
#include <adwaita.h>
#include <gtk/gtk.h>
#include <stddef.h>

#include "../core/extract_util.h"
#include "../core/pwdlib.h"

/* ── Password mode ── */
enum { PWD_MODE_MANUAL = 0, PWD_MODE_SELECT = 1, PWD_MODE_TRYALL = 2 };

/* ── Per-file extraction result ── */
typedef struct {
  char *filename;
  char *outdir;
  int success;
  char *failure_reason;
  int need_save_pwd; /* manual password succeeded */
  char *used_password;
} FileResult;

/* ── Parameters collected from SETUP page ── */
typedef struct {
  int pwd_mode;          /* PWD_MODE_* */
  char *manual_password; /* owned, for MODE_MANUAL */
  int selected_pwd_idx;  /* for MODE_SELECT */
  char *dest_dir;        /* NULL = auto (archive parent) */
} ExtractParams;

/* ── Application-wide context ── */
typedef struct {
  AdwWindow *window;

  /* Input */
  char **paths;
  size_t path_count;
  TaskPathList tasks;

  /* Password library */
  PwdVec password_lib;

  /* SETUP page widgets */
  AdwComboRow *combo_mode;
  AdwPreferencesGroup *grp_manual;
  AdwEntryRow *entry_pwd;
  AdwPreferencesGroup *grp_select;
  AdwComboRow *combo_pwd_select;
  AdwPreferencesGroup *grp_tryall;
  AdwActionRow *row_tryall_count;
  AdwActionRow *row_tryall_est;
  GtkLabel *lbl_dest_path;
  GtkButton *btn_dest_reset;
  GtkButton *btn_extract;
  char *dest_dir;
  char *archive_dir;

  /* Page stack */
  GtkStack *stack;

  /* PROGRESS page widgets */
  GtkProgressBar *progress_bar;       /* total progress */
  GtkLabel *lbl_progress_status;
  GtkLabel *lbl_progress_file;
  GtkWidget *spinner;
  int current_file_idx;               /* 1-based, parsed from pipe */
  char current_file_name[256];        /* current file display name */

  /* DONE page widgets */
  GtkBox *done_content;

  /* Worker thread state */
  GThread *worker_thread;
  gint cancelled;
  int pipe_read_fd;
  int pipe_write_fd;
  guint pipe_source_id;
  guint pulse_source_id;
  char pipe_line_buf[4096];
  size_t pipe_line_len;
  int last_progress_pct;
  gint pulse_active;

  /* Results */
  FileResult *results;
  int result_count;
  int success_count;
  int total_files;

  /* Passwords to offer saving */
  char **saved_passwords;
  int saved_password_count;
} ExtractionContext;

/* ── Internal functions shared between ui_gtk_*.c and toolkit_window.c ── */
void ui_gtk_build_setup_page(ExtractionContext *ctx, GtkBox *content);
void ui_gtk_build_progress_page(ExtractionContext *ctx, GtkBox *content);
void ui_gtk_build_done_page(ExtractionContext *ctx, GtkBox *content);
void ui_gtk_switch_to_progress(ExtractionContext *ctx);
void ui_gtk_switch_to_done(ExtractionContext *ctx);
void ui_gtk_start_extraction(ExtractionContext *ctx);
gboolean ui_gtk_pipe_callback(int fd, GIOCondition cond, gpointer data);
gpointer ui_gtk_worker_func(gpointer data);
