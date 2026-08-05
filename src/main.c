/* ===========================================================================
 * main.c — Lists entry point
 *
 * A GTK3 + SQLite task-list application in plain C — the companion app to
 * Records.  Boot order: config (needs argv[0] for the portable ini) →
 * database → OAuth credential snapshot → GtkApplication → library window
 * → periodic Google Tasks auto-sync.
 * =========================================================================== */

#include <gtk/gtk.h>
#include <curl/curl.h>
#include <sqlite3.h>
#include "app.h"
#include "db.h"
#include "oauth.h"
#include "gtasks.h"
#include "library_window.h"
#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

/* ---------------------------------------------------------------------------
 * integrity_collect() — sqlite3_exec callback: accumulates non-"ok" rows
 * from a PRAGMA integrity_check result into the GString passed as `data`.
 * ------------------------------------------------------------------------- */
static int
integrity_collect(void *data, int argc, char **argv, char **col_names)
{
    (void)col_names;
    GString *out = data;
    for (int i = 0; i < argc; i++) {
        if (argv[i] != NULL && g_strcmp0(argv[i], "ok") != 0) {
            if (out->len > 0)
                g_string_append_c(out, '\n');
            g_string_append(out, argv[i]);
        }
    }
    return 0;
}

/* fk_collect() — sqlite3_exec callback: formats PRAGMA foreign_key_check
 * rows (table, rowid, parent, fkid) into human-readable lines.             */
static int
fk_collect(void *data, int argc, char **argv, char **col_names)
{
    (void)col_names;
    GString *out = data;
    if (argc >= 3 && argv[0] != NULL) {
        if (out->len > 0)
            g_string_append_c(out, '\n');
        g_string_append_printf(out, "  %s (row %s) \xe2\x86\x92 %s",
                               argv[0],
                               argv[1] != NULL ? argv[1] : "?",
                               argv[2] != NULL ? argv[2] : "?");
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * startup_integrity_check() — run PRAGMA integrity_check and PRAGMA
 * foreign_key_check against the open database.  Shows a warning dialog if
 * either check reports problems.  Returns TRUE if both actually RAN and
 * both passed.
 *
 * The sqlite3_exec return codes are load-bearing, not noise: a PRAGMA that
 * never ran (a locked database, an I/O error) collects no rows, which is
 * indistinguishable from a clean result if you only look at the collectors.
 * Reporting "checked, all good" when nothing was checked is the one outcome
 * a health check must never produce, so a failed exec is surfaced with
 * sqlite's own message.
 * ------------------------------------------------------------------------- */
static gboolean
startup_integrity_check(BtApp *app)
{
    GString *ic_errors = g_string_new(NULL);
    gchar   *ic_fail   = NULL;       /* sqlite's message if exec failed     */
    if (sqlite3_exec(app->db->sq, "PRAGMA integrity_check",
                     integrity_collect, ic_errors, NULL) != SQLITE_OK)
        ic_fail = g_strdup(sqlite3_errmsg(app->db->sq));

    GString *fk_errors = g_string_new(NULL);
    gchar   *fk_fail   = NULL;
    if (sqlite3_exec(app->db->sq, "PRAGMA foreign_key_check",
                     fk_collect, fk_errors, NULL) != SQLITE_OK)
        fk_fail = g_strdup(sqlite3_errmsg(app->db->sq));

    gboolean ok = ic_errors->len == 0 && fk_errors->len == 0 &&
                  ic_fail == NULL && fk_fail == NULL;
    if (!ok) {
        GString *msg = g_string_new(NULL);
        if (ic_fail != NULL)
            g_string_append_printf(msg,
                "The integrity check could not be run:\n  %s\n", ic_fail);
        if (fk_fail != NULL)
            g_string_append_printf(msg,
                "The foreign key check could not be run:\n  %s\n", fk_fail);
        if (ic_errors->len > 0) {
            if (msg->len > 0)
                g_string_append_c(msg, '\n');
            g_string_append(msg, "Integrity check errors:\n");
            g_string_append(msg, ic_errors->str);
        }
        if (fk_errors->len > 0) {
            if (msg->len > 0)
                g_string_append(msg, "\n\n");
            g_string_append(msg, "Foreign key violations:\n");
            g_string_append(msg, fk_errors->str);
        }
        /* "found issues" would be a lie when the checks never ran at all,
         * which is exactly the case this dialog exists to expose.           */
        gboolean ran = ic_fail == NULL && fk_fail == NULL;
        bt_app_notice(NULL, GTK_MESSAGE_WARNING,
                      "Lists \xe2\x80\x94 Database Integrity Check",
                      ran ? "The database integrity check found issues:"
                            "\n\n%s"
                          : "The database integrity check did not "
                            "complete:\n\n%s",
                      msg->str);
        g_string_free(msg, TRUE);
    }

    g_free(ic_fail);
    g_free(fk_fail);
    g_string_free(ic_errors, TRUE);
    g_string_free(fk_errors, TRUE);
    return ok;
}

/* ---------------------------------------------------------------------------
 * startup_first_run() — no lists.db found at the expected location:
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
        gtk_window_set_title(GTK_WINDOW(dlg), "Lists - Welcome");
        gtk_dialog_add_buttons(GTK_DIALOG(dlg),
            "_Open a lists.db File",  1,
            "Create a _New lists.db", 2,
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
                             "Lists - Open Database");
        /* Filter to lists.db only — the ini stores db_dir, the
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
     * (same set Records ships; needs the librsvg pixbuf loader).           */
    gchar *theme_dir = g_build_filename(boot->app->icons_dir, "theme",
                                        NULL);
    gtk_icon_theme_prepend_search_path(gtk_icon_theme_get_default(),
                                       theme_dir);
    g_free(theme_dir);

    /* On Linux (and any non-macOS platform) the window manager shows a
     * generic icon unless we set one explicitly.  Load document.png from
     * the icons/ directory and register it as the default for all windows. */
#ifndef HAVE_GTKOSX
    gchar *icon_path = g_build_filename(boot->app->icons_dir,
                                        "document.png", NULL);
    GError *icon_err = NULL;
    gtk_window_set_default_icon_from_file(icon_path, &icon_err);
    g_clear_error(&icon_err);
    g_free(icon_path);
#endif

    /* DB integrity check: run PRAGMA integrity_check + foreign_key_check.    */
    gboolean db_ok = !boot->app->db_integrity_check
                     || startup_integrity_check(boot->app);

    bt_library_window_new(boot->app);
    bt_sync_auto_start(boot->app, boot->db_path);

    if (boot->app->db_integrity_check && db_ok)
        bt_app_status(boot->app, "DB at %s loaded, integrity check passed",
                      boot->app->db->path);

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
        g_printerr("lists: %s\n",
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
    app->db_integrity_check =
        bt_app_config_get_bool("db_integrity_check", TRUE);

    BtBoot boot = { app, db_path };
    app->gtk_app = gtk_application_new("org.example.lists",
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
