/* ===========================================================================
 * main.c — Hacienda entry point
 *
 * A GTK3 + SQLite task-list application in plain C — the companion app to
 * Blue Notes.  Boot order: config (needs argv[0] for the portable ini) →
 * database → OAuth credential snapshot → GtkApplication → library window
 * → periodic Google Tasks auto-sync.
 * =========================================================================== */

#include <gtk/gtk.h>
#include <curl/curl.h>
#include "app.h"
#include "db.h"
#include "oauth.h"
#include "gtasks.h"
#include "library_window.h"
#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

/* ---------------------------------------------------------------------------
 * startup_first_run() — no hacienda.db found at the expected location:
 * ask whether to open an existing file or create a new one there, instead
 * of silently creating an empty database (a user pointing at a shared
 * folder usually means to OPEN a file that is already there).
 *   expected — the path where the db was looked for (shown in dialog text).
 *   db_dir   — in/out: the configured db directory; replaced (and
 *              persisted to the ini) when an existing file is opened.
 *   db_path  — in/out: the path handed to bt_db_open(); replaced when an
 *              existing file is opened.
 * Returns TRUE to proceed with bt_db_open(*db_path), FALSE to quit.        */
static gboolean
startup_first_run(const gchar *expected, gchar **db_dir, gchar **db_path)
{
    /* Loop so cancelling the file chooser returns to the choice dialog.    */
    for (;;) {
        GtkWidget *dlg = gtk_message_dialog_new(
            NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
            "No tasks database was found at\n%s",
            expected);
        gtk_window_set_title(GTK_WINDOW(dlg), "Hacienda - Welcome");
        gtk_dialog_add_buttons(GTK_DIALOG(dlg),
            "_Open a hacienda.db File",  1,
            "Create a _New hacienda.db", 2,
            NULL);
        gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);

        if (resp == 2)
            return TRUE;             /* bt_db_open() will create it         */
        if (resp != 1)
            return FALSE;            /* dialog closed — quit                */

        GtkWidget *chooser = gtk_file_chooser_dialog_new(
            "Open Tasks Database", NULL,
            GTK_FILE_CHOOSER_ACTION_OPEN,
            "_Cancel", GTK_RESPONSE_CANCEL,
            "_Open",   GTK_RESPONSE_ACCEPT,
            NULL);
        gtk_window_set_title(GTK_WINDOW(chooser),
                             "Hacienda - Open Database");
        /* Filter to hacienda.db only — the ini stores db_dir, the
         * filename is always the fixed constant.                           */
        GtkFileFilter *ff = gtk_file_filter_new();
        gtk_file_filter_add_pattern(ff, BT_DB_FILENAME);
        gtk_file_filter_set_name(ff,
            "Tasks Database (" BT_DB_FILENAME ")");
        gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), ff);

        gchar *file_path = NULL;
        if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
            file_path = gtk_file_chooser_get_filename(
                GTK_FILE_CHOOSER(chooser));
        gtk_widget_destroy(chooser);
        if (file_path == NULL)
            continue;                /* cancelled — back to the choice      */

        gchar *dir = g_path_get_dirname(file_path);
        g_free(file_path);
        bt_app_config_set("db_dir", dir);
        g_free(*db_dir);   *db_dir  = dir;
        g_free(*db_path);  *db_path = g_build_filename(dir, BT_DB_FILENAME,
                                                       NULL);
        return TRUE;
    }
}

/* The single application context, shared with the activate handler.         */
typedef struct {
    BtApp  *app;
    gchar  *db_path;
} BtBoot;

/* ---------------------------------------------------------------------------
 * on_activate() — build the library window (or raise it on re-activate)
 * and start the auto-sync timer.
 * ------------------------------------------------------------------------- */
static void
on_activate(GtkApplication *gtk_app, gpointer data)
{
    (void)gtk_app;
    BtBoot *boot = data;
    if (boot->app->library_window != NULL) {
        gtk_window_present(GTK_WINDOW(boot->app->library_window));
        return;
    }

    /* Bundled scalable theme icons (icons/theme/hicolor/...): provides
     * SVG pan-*-symbolic arrows so tree expanders render crisply on
     * HiDPI displays instead of GTK's built-in 1x raster fallbacks
     * (same set Blue Notes ships; needs the librsvg pixbuf loader).        */
    gchar *theme_dir = g_build_filename(boot->app->icons_dir, "theme",
                                        NULL);
    gtk_icon_theme_prepend_search_path(gtk_icon_theme_get_default(),
                                       theme_dir);
    g_free(theme_dir);

    bt_library_window_new(boot->app);
    bt_sync_auto_start(boot->app, boot->db_path);

#ifdef HAVE_GTKOSX
    /* Honor the persisted native-menu-bar preference, then let the macOS
     * integration finish its launch handshake.                             */
    if (bt_app_config_get_bool("native_menubar", FALSE))
        bt_library_apply_native_menubar(boot->app, TRUE);
    gtkosx_application_ready(gtkosx_application_get());
#endif
}

/* ---------------------------------------------------------------------------
 * main() — set up the context and run the GTK main loop.
 * ------------------------------------------------------------------------- */
int
main(int argc, char **argv)
{
    /* Config first: everything else may read it.                            */
    bt_app_config_init(argc > 0 ? argv[0] : NULL);

    /* libcurl's global init is NOT thread-safe when left to the first
     * curl_easy_init — and ours happen on sync/OAuth worker threads,
     * possibly concurrently.  Initialize once before any thread exists.     */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    gchar *db_dir  = bt_app_config_get("db_dir");
    gchar *db_path = (db_dir != NULL && *db_dir != '\0')
                     ? g_build_filename(db_dir, BT_DB_FILENAME, NULL)
                     : bt_db_default_path();

    /* First-run: if the database file does not yet exist, ask the user
     * whether to open an existing file or create a fresh one — silently
     * creating an empty database when the user meant to point at an
     * existing shared file is a common foot-gun.  Needs GTK up early for
     * the dialog; skipped without a display (headless / non-interactive). */
    if (!g_file_test(db_path, G_FILE_TEST_EXISTS)) {
        gchar *expected = g_strdup(db_path);
        if (gtk_init_check(&argc, &argv) &&
            !startup_first_run(expected, &db_dir, &db_path)) {
            g_free(expected);
            g_free(db_dir);
            g_free(db_path);
            return 0;                /* user closed the welcome dialog      */
        }
        g_free(expected);
    }

    GError *gerr = NULL;
    BtDatabase *db = bt_db_open(db_path, &gerr);
    if (db == NULL) {
        g_printerr("hacienda: %s\n",
                   gerr != NULL ? gerr->message : "cannot open database");
        g_clear_error(&gerr);
        g_free(db_dir);
        g_free(db_path);
        return 1;
    }

    bt_oauth_init();                 /* snapshot Google credentials         */

    BtApp *app = g_new0(BtApp, 1);
    app->db     = db;
    app->db_dir = (db_dir != NULL && *db_dir != '\0')
                  ? g_strdup(db_dir) : NULL;
    app->editors = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                         g_free, NULL);
    app->bn_editors = g_hash_table_new_full(g_str_hash, g_str_equal,
                                            g_free, NULL);
    app->toolbars = g_ptr_array_new();
    bt_app_init_icons_dir(app);
    bt_app_load_toolbar_style(app);

    BtBoot boot = { app, db_path };
    app->gtk_app = gtk_application_new("org.example.hacienda",
                                       G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app->gtk_app, "activate",
                     G_CALLBACK(on_activate), &boot);
    int status = g_application_run(G_APPLICATION(app->gtk_app), argc,
                                   argv);

    g_object_unref(app->gtk_app);
    g_hash_table_destroy(app->editors);
    g_hash_table_destroy(app->bn_editors);
    g_ptr_array_free(app->toolbars, TRUE);
    bt_db_close(app->db);
    g_free(app->db_dir);
    g_free(app);
    g_free(db_dir);
    g_free(db_path);
    curl_global_cleanup();
    return status;
}
