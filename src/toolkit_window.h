#pragma once

#include <glib.h>
#include <stdbool.h>

/**
 * Open the unified toolkit window.
 *
 * @param files       GList of NautilusFileInfo* (the selected items)
 * @param all_folders TRUE when every selected item is a directory
 *                    (disables the "解压" tab)
 */
void toolkit_window_show(GList *files, bool all_folders);
