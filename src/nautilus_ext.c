/*
 * nautilus_ext.c — Unified Nautilus extension for 解压/压缩.
 *
 * Single menu item "解压/压缩" shown for all file types.
 * Clicking it opens the unified toolkit window directly.
 * Nautilus 4 only, C17.
 */

#define _GNU_SOURCE

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#include <nautilus-extension.h>

#include "toolkit_window.h"

/* ══════════════════════════════════════════════════════════════════════════
 *  GType boilerplate
 * ══════════════════════════════════════════════════════════════════════════ */

typedef struct {
    GObject parent;
} NautilusToolkitExt;

typedef struct {
    GObjectClass parent_class;
} NautilusToolkitExtClass;

static void ntk_menu_provider_iface_init(NautilusMenuProviderInterface *iface);

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    NautilusToolkitExt, ntk, G_TYPE_OBJECT, 0,
    G_IMPLEMENT_INTERFACE_DYNAMIC(NAUTILUS_TYPE_MENU_PROVIDER,
                                  ntk_menu_provider_iface_init))

/* ══════════════════════════════════════════════════════════════════════════
 *  Menu item callback
 * ══════════════════════════════════════════════════════════════════════════ */

static void
on_toolkit_activate(NautilusMenuItem *item,
                    gpointer          user_data G_GNUC_UNUSED)
{
    GList *files = g_object_get_data(G_OBJECT(item), "ntk-files");
    if (!files)
        return;

    gboolean all_folders = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "ntk-all-folders"));

    toolkit_window_show(files, all_folders);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  NautilusMenuProvider — get_file_items
 * ══════════════════════════════════════════════════════════════════════════ */

static GList *
ntk_get_file_items(NautilusMenuProvider *provider G_GNUC_UNUSED,
                   GList                *files)
{
    if (!files)
        return NULL;

    /* Check if all selected items are folders */
    gboolean all_folders = TRUE;
    gboolean has_local = FALSE;

    for (GList *l = files; l; l = l->next) {
        NautilusFileInfo *fi = (NautilusFileInfo *)l->data;

        GFile *gf = nautilus_file_info_get_location(fi);
        if (!gf)
            continue;
        if (!g_file_is_native(gf)) {
            g_object_unref(gf);
            continue;
        }
        g_object_unref(gf);

        has_local = TRUE;

        GFileType ftype = nautilus_file_info_get_file_type(fi);
        if (ftype != G_FILE_TYPE_DIRECTORY)
            all_folders = FALSE;
    }

    if (!has_local)
        return NULL;

    NautilusMenuItem *item = nautilus_menu_item_new(
        "NautilusToolkitExt::toolkit",
        "解压/压缩",
        "智能解压与压缩工具",
        "package-x-generic-symbolic");

    g_object_set_data_full(G_OBJECT(item), "ntk-files",
                           nautilus_file_info_list_copy(files),
                           (GDestroyNotify)nautilus_file_info_list_free);

    g_object_set_data(G_OBJECT(item), "ntk-all-folders",
                      GINT_TO_POINTER(all_folders));

    g_signal_connect(item, "activate",
                     G_CALLBACK(on_toolkit_activate), NULL);

    return g_list_append(NULL, item);
}

/* ══════════════════════════════════════════════════════════════════════════
 *  Interface & GObject init
 * ══════════════════════════════════════════════════════════════════════════ */

static void
ntk_menu_provider_iface_init(NautilusMenuProviderInterface *iface)
{
    iface->get_file_items = ntk_get_file_items;
}

static void ntk_init(NautilusToolkitExt *self G_GNUC_UNUSED) {}
static void ntk_class_init(NautilusToolkitExtClass *klass G_GNUC_UNUSED) {}
static void ntk_class_finalize(NautilusToolkitExtClass *klass G_GNUC_UNUSED) {}

/* ══════════════════════════════════════════════════════════════════════════
 *  Nautilus module entry points
 * ══════════════════════════════════════════════════════════════════════════ */

void
nautilus_module_initialize(GTypeModule *module)
{
    ntk_register_type(module);
}

void
nautilus_module_shutdown(void)
{
}

void
nautilus_module_list_types(const GType **types, int *num_types)
{
    static GType type_list[1];
    type_list[0] = ntk_get_type();
    *types     = type_list;
    *num_types = 1;
}
