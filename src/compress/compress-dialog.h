#ifndef COMPRESS_DIALOG_H
#define COMPRESS_DIALOG_H
#include <adwaita.h>
G_BEGIN_DECLS
#define COMPRESS_TYPE_DIALOG (compress_dialog_get_type())
G_DECLARE_FINAL_TYPE(CompressDialog, compress_dialog, COMPRESS, DIALOG, GtkBox)
/**
 * Show the compression dialog as a standalone window.
 * @parent: parent GtkWindow (may be NULL)
 * @files: GList of char* file paths (dialog takes a copy)
 */
void compress_dialog_show(GtkWindow *parent, GList *files);

void compress_dialog_show_in_container(GtkWindow *parent_window,
                                       GList *files,
                                       GtkBox *container);
G_END_DECLS
#endif /* COMPRESS_DIALOG_H */
