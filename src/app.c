/* ===========================================================================
 * app.c — shared application context for Lists (see app.h)
 * =========================================================================== */

#include "app.h"
#include "db.h"
#include "editor_window.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * bt_app_status() — post an event message to the library status bar.
 * ------------------------------------------------------------------------- */
void
bt_app_status(BtApp *app, const gchar *fmt, ...)
{
    if (app == NULL || app->notify_status == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    app->notify_status(app, msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * bt_app_notify_changed() — fire the full-refresh hook if installed.
 * ------------------------------------------------------------------------- */
void
bt_app_notify_changed(BtApp *app)
{
    if (app != NULL && app->notify_changed != NULL)
        app->notify_changed(app);
}

/* dialog_run() — shared core of notice/confirm: run a modal message
 * dialog and return its response.                                           */
static gint
dialog_run(GtkWindow *parent, GtkMessageType type, GtkButtonsType buttons,
           const gchar *title, const gchar *msg)
{
    GtkWidget *dlg = gtk_message_dialog_new(parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, type,
        buttons, "%s", msg);
    if (title != NULL)
        gtk_window_set_title(GTK_WINDOW(dlg), title);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    return resp;
}

/* ---------------------------------------------------------------------------
 * bt_app_notice() — modal OK message dialog.
 * ------------------------------------------------------------------------- */
void
bt_app_notice(GtkWindow *parent, GtkMessageType type,
              const gchar *title, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    dialog_run(parent, type, GTK_BUTTONS_OK, title, msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * bt_app_confirm() — modal Yes/No question; TRUE on Yes.
 * ------------------------------------------------------------------------- */
gboolean
bt_app_confirm(GtkWindow *parent, const gchar *title, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    gint resp = dialog_run(parent, GTK_MESSAGE_QUESTION,
                           GTK_BUTTONS_YES_NO, title, msg);
    g_free(msg);
    return resp == GTK_RESPONSE_YES;
}

/* ---------------------------------------------------------------------------
 * bt_app_widget_add_css() — one-off CSS on a single widget (see app.h).
 * ------------------------------------------------------------------------- */
void
bt_app_widget_add_css(GtkWidget *widget, const gchar *css_text)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css_text, -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(widget),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ---------------------------------------------------------------------------
 * copy_file() — overwrite-copy src to dest via GIO.
 * ------------------------------------------------------------------------- */
static gboolean
copy_file(const gchar *src, const gchar *dest)
{
    GFile    *fsrc  = g_file_new_for_path(src);
    GFile    *fdest = g_file_new_for_path(dest);
    gboolean  ok    = g_file_copy(fsrc, fdest,
                                  G_FILE_COPY_OVERWRITE,
                                  NULL, NULL, NULL, NULL);
    g_object_unref(fsrc);
    g_object_unref(fdest);
    return ok;
}

/* ---------------------------------------------------------------------------
 * bt_app_switch_database() — move lists.db to a new directory (see app.h).
 * ------------------------------------------------------------------------- */
gboolean
bt_app_switch_database(BtApp *app, const gchar *new_dir)
{
    /* Resolve the target file path.                                          */
    gchar *target;
    if (new_dir != NULL) {
        g_mkdir_with_parents(new_dir, 0755);
        target = g_build_filename(new_dir, BT_DB_FILENAME, NULL);
    } else {
        target = bt_db_default_path();
    }
    if (g_strcmp0(target, app->db->path) == 0) {
        g_free(target);
        return TRUE;                   /* already there: nothing to do        */
    }

    /* If a database already exists at the target, ask before touching it.   */
    gboolean overwrite = FALSE;
    if (g_file_test(target, G_FILE_TEST_EXISTS)) {
        GtkWindow *parent = app->library_window != NULL
                            ? GTK_WINDOW(app->library_window) : NULL;
        GtkWidget *dialog = gtk_message_dialog_new(
            parent, GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "That folder already contains a tasks database.\n\n"
            "Use the tasks stored there, or overwrite it with a copy "
            "of your current database?\n"
            "(Overwriting permanently replaces the file at %s.)", target);
        gtk_window_set_title(GTK_WINDOW(dialog),
                             "Lists - Existing Database");
        gtk_dialog_add_buttons(GTK_DIALOG(dialog),
            "_Cancel",                GTK_RESPONSE_CANCEL,
            "_Use Existing Database", 1,
            "_Overwrite It",          2,
            NULL);
        gint response = gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        if (response != 1 && response != 2) {
            g_free(target);
            return FALSE;              /* cancelled: nothing touched          */
        }
        overwrite = (response == 2);
    }

    bt_editor_close_all(app);

    gchar *old_path = g_strdup(app->db->path);
    bt_db_close(app->db);
    app->db = NULL;

    if (g_file_test(old_path, G_FILE_TEST_EXISTS)) {
        if (overwrite || !g_file_test(target, G_FILE_TEST_EXISTS))
            copy_file(old_path, target);
    }

    GError *gerr = NULL;
    app->db = bt_db_open(target, &gerr);
    gboolean ok = (app->db != NULL);

    if (!ok) {
        g_warning("switch_database: cannot open %s: %s", target,
                  gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        bt_app_notice(app->library_window != NULL
                          ? GTK_WINDOW(app->library_window) : NULL,
                      GTK_MESSAGE_ERROR, NULL,
                      "Could not open a database at that location.\n"
                      "The previous database is still in use.");
        app->db = bt_db_open(old_path, &gerr);
        if (app->db == NULL)
            g_critical("switch_database: cannot revert to %s: %s", old_path,
                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
    } else {
        if (g_file_test(old_path, G_FILE_TEST_EXISTS)) {
            GFile *fold = g_file_new_for_path(old_path);
            if (!g_file_delete(fold, NULL, NULL))
                g_warning("switch_database: could not remove %s", old_path);
            g_object_unref(fold);
        }
        g_free(app->db_dir);
        app->db_dir = g_strdup(new_dir);
        bt_app_config_set("db_dir", new_dir);  /* NULL clears the key        */
    }

    g_free(target);
    g_free(old_path);

    if (ok)
        bt_app_notify_changed(app);
    return ok;
}

/* ===========================================================================
 * Toolbar icons + style (see app.h).
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * bt_app_init_icons_dir() — icons/ next to the executable (see app.h).
 * ------------------------------------------------------------------------- */
void
bt_app_init_icons_dir(BtApp *app)
{
    app->icons_dir = g_build_filename(bt_app_exe_dir(), "icons", NULL);
}

/* ---------------------------------------------------------------------------
 * bt_app_icon_image_sized() — HiDPI-sharp GtkImage for a local icon
 * (see app.h).  Rasterizes at the display's scale factor: `size` is the
 * LOGICAL size, the backing pixels are size × sf, and the cairo
 * surface's device scale maps between the two (raw pixbufs render
 * 1 buffer-pixel = 1 logical px and blur on Retina — Blue Notes
 * gotcha #5).
 * ------------------------------------------------------------------------- */
GtkWidget *
bt_app_icon_image_sized(BtApp *app, const gchar *name, gint size)
{
    static const gchar *EXTS[] = { "png", "svg" };

    gint sf = 1;                     /* display scale factor                */
    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
        GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
        if (monitor == NULL)
            monitor = gdk_display_get_monitor(display, 0);
        if (monitor != NULL)
            sf = gdk_monitor_get_scale_factor(monitor);
    }

    for (gsize i = 0; i < G_N_ELEMENTS(EXTS); i++) {
        gchar *path = g_strdup_printf("%s%c%s.%s",
                                      app->icons_dir, G_DIR_SEPARATOR,
                                      name, EXTS[i]);
        GdkPixbuf *pix = NULL;       /* decoded at backing resolution       */
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            pix = gdk_pixbuf_new_from_file_at_size(path, size * sf,
                                                   size * sf, NULL);
        g_free(path);
        if (pix != NULL) {
            cairo_surface_t *surface =
                gdk_cairo_surface_create_from_pixbuf(pix, sf, NULL);
            g_object_unref(pix);
            GtkWidget *image = gtk_image_new_from_surface(surface);
            cairo_surface_destroy(surface);
            return image;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * bt_app_tool_item_new() — style-aware toolbar button (see app.h).
 * ------------------------------------------------------------------------- */
GtkToolItem *
bt_app_tool_item_new(BtApp *app, const gchar *icon_name,
                     const gchar *fallback_markup, const gchar *label,
                     const gchar *tooltip)
{
    GtkToolItem *item = gtk_tool_button_new(NULL, NULL);
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), label);

    /* Icon: the local PNG if present, else the fallback markup rendered
     * as a label standing in for the icon.                                 */
    GtkWidget *icon = (icon_name != NULL)
                      ? bt_app_icon_image_sized(app, icon_name, 24) : NULL;
    if (icon == NULL) {
        icon = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(icon),
                             fallback_markup != NULL ? fallback_markup
                                                     : label);
    }
    gtk_widget_show(icon);
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(item), icon);

    gtk_tool_item_set_tooltip_text(item, tooltip);
    gtk_tool_item_set_is_important(item, TRUE);
    return item;
}

/* style_name()/style_from_name() — the persisted spelling of a style.       */
static const gchar *
style_name(GtkToolbarStyle style)
{
    return style == GTK_TOOLBAR_TEXT ? "text"
         : style == GTK_TOOLBAR_BOTH ? "both" : "icons";
}

static GtkToolbarStyle
style_from_name(const gchar *name)
{
    if (g_strcmp0(name, "text") == 0) return GTK_TOOLBAR_TEXT;
    if (g_strcmp0(name, "both") == 0) return GTK_TOOLBAR_BOTH;
    return GTK_TOOLBAR_ICONS;
}

/* bt_app_load_toolbar_style() — the persisted style (default icons).        */
void
bt_app_load_toolbar_style(BtApp *app)
{
    gchar *v = bt_app_config_get("toolbar_style");
    app->toolbar_style = style_from_name(v);
    g_free(v);
}

/* ---------------------------------------------------------------------------
 * bt_app_set_toolbar_style() — apply + persist a style change (see app.h).
 * ------------------------------------------------------------------------- */
void
bt_app_set_toolbar_style(BtApp *app, GtkToolbarStyle style)
{
    app->toolbar_style = style;
    bt_app_config_set("toolbar_style", style_name(style));
    if (app->toolbars != NULL)
        for (guint i = 0; i < app->toolbars->len; i++)
            gtk_toolbar_set_style(
                GTK_TOOLBAR(g_ptr_array_index(app->toolbars, i)), style);
}

/* toolbar_destroyed() — drop a dying toolbar from the registry.             */
static void
toolbar_destroyed(GtkWidget *toolbar, gpointer data)
{
    BtApp *app = data;
    if (app->toolbars != NULL)
        g_ptr_array_remove(app->toolbars, toolbar);
}

/* style_menu_toggled() — a radio item in the right-click menu.              */
static void
style_menu_toggled(GtkCheckMenuItem *item, gpointer data)
{
    BtApp *app = data;
    if (!gtk_check_menu_item_get_active(item))
        return;                      /* ignore the deactivating item        */
    bt_app_set_toolbar_style(app, (GtkToolbarStyle)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "bt-style")));
}

/* ---------------------------------------------------------------------------
 * toolbar_context_menu() — "popup-context-menu": right-clicking a
 * toolbar offers the icons/both/text radio choices (fires on empty
 * toolbar area only, like Blue Notes).
 * ------------------------------------------------------------------------- */
static gboolean
toolbar_context_menu(GtkToolbar *toolbar, gint x, gint y, gint button,
                     gpointer data)
{
    (void)x; (void)y; (void)button;
    BtApp *app = data;
    static const struct {
        const gchar    *label;
        GtkToolbarStyle style;
    } CHOICES[] = {
        { "Icons",            GTK_TOOLBAR_ICONS },
        { "Text Below Icons", GTK_TOOLBAR_BOTH  },
        { "Text Only",        GTK_TOOLBAR_TEXT  },
    };
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(toolbar), NULL);
    /* One menu is built per right-click; without this it would stay
     * attached (= alive) until the toolbar dies.  selection-done fires
     * after the chosen item's activate, so destroying there is safe.        */
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);
    GSList *group = NULL;            /* the radio group                     */
    for (gsize i = 0; i < G_N_ELEMENTS(CHOICES); i++) {
        GtkWidget *item =
            gtk_radio_menu_item_new_with_label(group, CHOICES[i].label);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "bt-style",
                          GINT_TO_POINTER(CHOICES[i].style));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                       app->toolbar_style ==
                                       CHOICES[i].style);
        g_signal_connect(item, "toggled",
                         G_CALLBACK(style_menu_toggled), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * bt_app_register_toolbar() — track + style a toolbar (see app.h).
 * ------------------------------------------------------------------------- */
void
bt_app_register_toolbar(BtApp *app, GtkWidget *toolbar)
{
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), app->toolbar_style);
    if (app->toolbars != NULL)
        g_ptr_array_add(app->toolbars, toolbar);
    g_signal_connect(toolbar, "destroy",
                     G_CALLBACK(toolbar_destroyed), app);
    g_signal_connect(toolbar, "popup-context-menu",
                     G_CALLBACK(toolbar_context_menu), app);
}

/* ===========================================================================
 * Config — ini next to the binary, ~/.config fallback (see app.h).
 * =========================================================================== */

#define BT_INI_GROUP "lists"

/* The pre-3.0 ini group.  The app was called Hacienda before the rename,
 * and the GROUP NAME is part of the file format — so an ini written by a
 * pre-rename build carries every setting under a name this build does not
 * read.  Without the migration below, upgrading silently reverted the user
 * to defaults and, because gtasks_refresh_token was among the abandoned
 * keys, made them re-authorize Google.                                     */
#define BT_INI_GROUP_LEGACY "hacienda"

static GKeyFile *config_kf   = NULL; /* the in-memory config                */
static gchar    *config_path = NULL; /* written through on every change     */
static gchar    *exe_dir_cached = NULL;  /* binary's directory (owned)      */

/* exe_dir_from_argv0() — the directory holding the binary (new string).     */
static gchar *
exe_dir_from_argv0(const gchar *argv0)
{
    if (argv0 != NULL && strchr(argv0, '/') != NULL) {
        gchar *abs = g_canonicalize_filename(argv0, NULL);
        gchar *dir = g_path_get_dirname(abs);
        g_free(abs);
        return dir;
    }
    return g_get_current_dir();
}

/* bt_app_exe_dir() — see app.h.                                             */
const gchar *
bt_app_exe_dir(void)
{
    return exe_dir_cached;
}

/* ---------------------------------------------------------------------------
 * Legacy-group migration (pre-3.0 "hacienda" ini → "lists").
 *
 * An ALLOWLIST, deliberately, not a blind group copy: the old group also
 * holds keys this build no longer has (`task_columns`, `task_sort_manual`)
 * which would just be dead weight in the new group.  Add a key here when a
 * pre-3.0 build could have written it AND the current build still reads it.
 *
 * Three sync keys are knowingly LEFT BEHIND:
 *
 *   google_client_id / google_client_secret — the current build still
 *     honors them, but no UI writes them, so one that exists is a
 *     hand-edit, and reviving a stale OAuth client breaks sync outright.
 *     Dropping it just falls back to the baked-in client, which works.
 *
 *   gtasks_refresh_token — tempting (it costs a browser round trip to
 *     replace) but WRONG.  A pre-3.0 token was issued to whatever OAuth
 *     client that build carried; against the current one Google answers
 *     the refresh with invalid_grant, and a failed refresh does NOT clear
 *     the token — so the app would report "signed in" while every sync
 *     failed.  Migrating nothing leaves a clean signed-out state and one
 *     sign-in click.  Verified against a real pre-rename ini: the token
 *     came back invalid_grant.
 *
 * In both cases silent breakage is the worse failure than a re-entry.
 * ------------------------------------------------------------------------- */
static const gchar *const LEGACY_KEYS[] = {
    /* sync */
    "google_sync_enabled", "sync_interval_min", "sync_toolbar_button",
    /* Blue Notes */
    "blue_notes_sync", "blue_notes_cli", "blue_notes_embed_list",
    /* database */
    "db_dir", "db_integrity_check",
    /* UI */
    "toolbar_style", "bold_task_titles", "native_menubar",
    "show_completed", "sidebar_visible", "compact_layout",
    "weekly_forecast", "due_today_show_overdue", "task_list_manual_sort",
    "col_done_visible", "col_due_visible", "win_w", "win_h",
};

/* legacy_key_wanted() — is `key` one the new group should inherit?  The
 * per-view manual order keys are matched by PREFIX: manual_order_list_<id>
 * carries a list id, so they cannot be enumerated.                          */
static gboolean
legacy_key_wanted(const gchar *key)
{
    if (g_str_has_prefix(key, "manual_order_"))
        return TRUE;
    for (gsize i = 0; i < G_N_ELEMENTS(LEGACY_KEYS); i++)
        if (strcmp(key, LEGACY_KEYS[i]) == 0)
            return TRUE;
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * config_migrate_legacy_group() — fold a pre-3.0 "hacienda" group into
 * "lists", then drop it.  No-op when there is no legacy group, which is
 * every launch after the first and every fresh install.
 *
 * Merged PER KEY, and the CURRENT group always wins: this runs against
 * files where the user has already been using the renamed build, so their
 * post-rename choices must not be reverted by an older value.  The legacy
 * group only fills gaps — e.g. db_dir stays whatever "lists" says, while
 * gtasks_refresh_token arrives because "lists" has none.
 *
 * The file is BACKED UP before the first rewrite (it holds an OAuth refresh
 * token, and this is the one operation that removes lines from it), and the
 * legacy group is removed so the migration cannot run twice — a second pass
 * would otherwise resurrect keys the user has since deliberately cleared.
 * ------------------------------------------------------------------------- */
static void
config_migrate_legacy_group(void)
{
    if (config_kf == NULL || config_path == NULL ||
        !g_key_file_has_group(config_kf, BT_INI_GROUP_LEGACY))
        return;

    gsize   nkeys = 0;
    gchar **keys  = g_key_file_get_keys(config_kf, BT_INI_GROUP_LEGACY,
                                        &nkeys, NULL);
    if (keys == NULL)
        return;

    /* Backup first — best effort: a failure here must not block the
     * migration, but the user gets told where the copy went (or didn't).    */
    gchar *backup = g_strconcat(config_path, ".pre-3.0.bak", NULL);
    if (!g_file_test(backup, G_FILE_TEST_EXISTS)) {
        gchar *raw = NULL;
        gsize  len = 0;
        if (g_file_get_contents(config_path, &raw, &len, NULL))
            g_file_set_contents(backup, raw, (gssize)len, NULL);
        g_free(raw);
    }

    guint moved = 0, kept = 0, dropped = 0;
    for (gsize i = 0; i < nkeys; i++) {
        const gchar *key = keys[i];
        if (!legacy_key_wanted(key)) {
            dropped++;               /* no longer part of the config        */
            continue;
        }
        if (g_key_file_has_key(config_kf, BT_INI_GROUP, key, NULL)) {
            kept++;                  /* the newer value stands              */
            continue;
        }
        gchar *v = g_key_file_get_string(config_kf, BT_INI_GROUP_LEGACY,
                                         key, NULL);
        if (v != NULL && *v != '\0') {
            g_key_file_set_string(config_kf, BT_INI_GROUP, key, v);
            moved++;
        }
        g_free(v);
    }
    g_strfreev(keys);

    g_key_file_remove_group(config_kf, BT_INI_GROUP_LEGACY, NULL);
    g_key_file_save_to_file(config_kf, config_path, NULL);

    /* Worth a line on the console: it happens once, silently changes the
     * running configuration, and names the backup if anything looks wrong.  */
    g_message("Migrated %u setting%s from the pre-3.0 [%s] config group "
              "(%u already set here, %u no longer used); backup: %s",
              moved, moved == 1 ? "" : "s", BT_INI_GROUP_LEGACY,
              kept, dropped, backup);
    g_free(backup);
}

/* ---------------------------------------------------------------------------
 * bt_app_config_init() — resolve + load the config file once.  Portable
 * mode: lists.ini next to the binary; when none exists there AND the
 * directory is unwritable, ~/.config/lists/lists.ini.  On first
 * run it is seeded from lists.ini.defaults next to the binary.
 * ------------------------------------------------------------------------- */
void
bt_app_config_init(const gchar *argv0)
{
    if (config_kf != NULL)
        return;

    gchar *exe_dir = exe_dir_from_argv0(argv0);
    exe_dir_cached = g_strdup(exe_dir);
    gchar *local = g_build_filename(exe_dir, "lists.ini", NULL);
    if (g_file_test(local, G_FILE_TEST_EXISTS) ||
        g_access(exe_dir, W_OK) == 0) {
        config_path = local;         /* portable mode                       */
    } else {
        g_free(local);
        gchar *dir = g_build_filename(g_get_user_config_dir(),
                                      "lists", NULL);
        g_mkdir_with_parents(dir, 0700);
        config_path = g_build_filename(dir, "lists.ini", NULL);
        g_free(dir);
    }

    config_kf = g_key_file_new();
    if (!g_key_file_load_from_file(config_kf, config_path,
                                   G_KEY_FILE_NONE, NULL)) {
        /* First launch: seed from the committed defaults, if present.       */
        gchar *defaults = g_build_filename(exe_dir,
                                           "lists.ini.defaults", NULL);
        g_key_file_load_from_file(config_kf, defaults,
                                  G_KEY_FILE_NONE, NULL);
        g_free(defaults);
    }
    /* Before any caller reads a key: an ini from a pre-rename build keeps
     * everything in a group this build ignores (see the banner above).      */
    config_migrate_legacy_group();
    g_free(exe_dir);
}

/* ---------------------------------------------------------------------------
 * bt_app_config_get() — read one setting; NULL when unset/empty.
 * ------------------------------------------------------------------------- */
gchar *
bt_app_config_get(const gchar *key)
{
    if (config_kf == NULL)
        return NULL;
    gchar *v = g_key_file_get_string(config_kf, BT_INI_GROUP, key, NULL);
    if (v != NULL && *v == '\0') {
        g_free(v);
        v = NULL;
    }
    return v;
}

/* ---------------------------------------------------------------------------
 * bt_app_config_get_bool() — read a 0/1 setting (see app.h).
 * ------------------------------------------------------------------------- */
gboolean
bt_app_config_get_bool(const gchar *key, gboolean def)
{
    gchar *v = bt_app_config_get(key);
    if (v == NULL)
        return def;
    gboolean b = strcmp(v, "0") != 0;
    g_free(v);
    return b;
}

/* ---------------------------------------------------------------------------
 * bt_app_config_set() — change one setting and write the ini through.
 * NULL removes the key.  Unchanged values skip the rewrite.
 * ------------------------------------------------------------------------- */
void
bt_app_config_set(const gchar *key, const gchar *value)
{
    if (config_kf == NULL)
        return;
    gchar *old = g_key_file_get_string(config_kf, BT_INI_GROUP, key, NULL);
    gboolean same = (old == NULL && value == NULL) ||
                    (old != NULL && value != NULL &&
                     strcmp(old, value) == 0);
    g_free(old);
    if (same)
        return;
    if (value != NULL)
        g_key_file_set_string(config_kf, BT_INI_GROUP, key, value);
    else
        g_key_file_remove_key(config_kf, BT_INI_GROUP, key, NULL);
    g_key_file_save_to_file(config_kf, config_path, NULL);
}

/* ===========================================================================
 * Date helpers (see app.h).
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * bt_day_bounds() — local midnight bounds of "today + offset_days".
 * ------------------------------------------------------------------------- */
void
bt_day_bounds(gint offset_days, gint64 *lo, gint64 *hi)
{
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *day = g_date_time_add_days(now, offset_days);
    GDateTime *mid = g_date_time_new_local(g_date_time_get_year(day),
                                           g_date_time_get_month(day),
                                           g_date_time_get_day_of_month(day),
                                           0, 0, 0);
    GDateTime *nxt = g_date_time_add_days(mid, 1);
    *lo = g_date_time_to_unix(mid);
    *hi = g_date_time_to_unix(nxt);
    g_date_time_unref(now);
    g_date_time_unref(day);
    g_date_time_unref(mid);
    g_date_time_unref(nxt);
}

/* ---------------------------------------------------------------------------
 * bt_due_format() — human-readable due date ("" for none).
 * ------------------------------------------------------------------------- */
gchar *
bt_due_format(gint64 due)
{
    if (due == 0)
        return g_strdup("");
    GDateTime *dt = g_date_time_new_from_unix_local(due);
    gchar *s = g_date_time_format(dt, "%b %-e, %Y");
    g_date_time_unref(dt);
    return s != NULL ? s : g_strdup("");
}

/* ---------------------------------------------------------------------------
 * bt_due_format_iso() — canonical "YYYY-MM-DD" spelling ("" for none).
 * ------------------------------------------------------------------------- */
gchar *
bt_due_format_iso(gint64 due)
{
    if (due == 0)
        return g_strdup("");
    GDateTime *dt = g_date_time_new_from_unix_local(due);
    gchar *s = g_date_time_format(dt, "%Y-%m-%d");
    g_date_time_unref(dt);
    return s != NULL ? s : g_strdup("");
}

/* ---------------------------------------------------------------------------
 * bt_due_color() — urgency tint (see app.h).  Compares calendar DAYS in
 * local time so the colors roll over at midnight.
 * ------------------------------------------------------------------------- */
const gchar *
bt_due_color(gint64 due)
{
    if (due == 0)
        return NULL;
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *dt  = g_date_time_new_from_unix_local(due);
    gint today = g_date_time_get_year(now) * 10000 +
                 g_date_time_get_month(now) * 100 +
                 g_date_time_get_day_of_month(now);
    gint day   = g_date_time_get_year(dt) * 10000 +
                 g_date_time_get_month(dt) * 100 +
                 g_date_time_get_day_of_month(dt);
    g_date_time_unref(now);
    g_date_time_unref(dt);
    return day < today  ? "#c01c28"          /* overdue: red                */
         : day == today ? "#d19a00"          /* today: gold                 */
                        : "#26a269";         /* ahead: green                */
}

/* ---------------------------------------------------------------------------
 * bt_due_from_ymd() — validated calendar fields → local midnight unix.
 * ------------------------------------------------------------------------- */
gint64
bt_due_from_ymd(gint y, gint m, gint d)
{
    if (m < 1 || m > 12 || d < 1 || d > 31 || y < 1970 || y > 9999)
        return 0;
    GDateTime *dt = g_date_time_new_local(y, m, d, 0, 0, 0);
    if (dt == NULL)
        return 0;
    gint64 u = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return u;
}

/* ---------------------------------------------------------------------------
 * bt_due_parse() — "YYYY-MM-DD" or "M/D/YY[YY]" → local midnight unix.
 * ------------------------------------------------------------------------- */
gint64
bt_due_parse(const gchar *text)
{
    if (text == NULL)
        return 0;
    gchar *t = g_strstrip(g_strdup(text));
    gint y = 0, m = 0, d = 0;        /* parsed calendar fields              */
    gboolean ok = FALSE;
    if (sscanf(t, "%d-%d-%d", &y, &m, &d) == 3) {
        ok = TRUE;
    } else if (sscanf(t, "%d/%d/%d", &m, &d, &y) == 3) {
        if (y < 100)
            y += 2000;
        ok = TRUE;
    }
    g_free(t);
    return ok ? bt_due_from_ymd(y, m, d) : 0;
}
