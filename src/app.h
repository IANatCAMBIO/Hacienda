/* ===========================================================================
 * app.h — shared application context for Hacienda
 *
 * A single BtApp instance is created in main() and passed to every window.
 * It owns the database handle, tracks open task-editor windows, and hosts
 * the notification hooks the library window installs.  Companion app to
 * Blue Notes — same design language: plain C + GTK3 + SQLite, no
 * HeaderBars, window titles "Hacienda - <thing>".
 * =========================================================================== */

#ifndef BT_APP_H
#define BT_APP_H

#include <gtk/gtk.h>
#include "db.h"

/* Semantic version, baked in by the Makefile (-DBT_VERSION="x.y.z").        */
#ifndef BT_VERSION
#define BT_VERSION "dev"
#endif

/* ---------------------------------------------------------------------------
 * BtApp — global application state.
 *
 * Fields:
 *   gtk_app        — the GtkApplication driving the main loop.
 *   db             — open tasks database (owned; closed at shutdown).
 *   editors        — map of open editor windows keyed by task id
 *                    (gint64* keys, GtkWindow* values).
 *   bn_editors     — map of open Blue Notes action-item editors keyed
 *                    by their "NOTEID:ORD" address (owned gchar* keys,
 *                    GtkWindow* values).
 *   library_window — the (single) library window, or NULL before startup.
 *   notify_changed — hook installed by the library window: FULL refresh
 *                    (sidebar + task pane + open editors).  For
 *                    structural changes: lists created/renamed/deleted,
 *                    sync applied.  May be NULL.
 *   notify_tasks   — lighter hook, also installed by the library window:
 *                    refreshes only the task pane.  Editor saves and
 *                    subtask/attachment edits use this — they can never
 *                    change the sidebar, and the saving editor is itself
 *                    the source of truth (reloading every editor per
 *                    autosave would also re-run the Blue Notes CLI).
 *                    May be NULL.
 *   notify_status  — hook installed by the library window: shows an event
 *                    message on its status bar.  Post through
 *                    bt_app_status(), which handles the hook being NULL.
 *   sync_running   — TRUE while the Google Tasks sync worker is running
 *                    (main-thread flag; blocks a second concurrent sync).
 *   sync_timer     — the periodic auto-sync GSource id, or 0.
 *   toolbar_style  — how toolbar buttons render (icons only, text below
 *                    icons, or text only); persisted as "toolbar_style".
 *   toolbars       — every live toolbar, so a style change can be
 *                    applied to all open windows at once.  Entries
 *                    remove themselves on destroy.
 *   icons_dir      — absolute path of the local icons/ folder the
 *                    toolbar button PNGs are loaded from (owned string).
 * ------------------------------------------------------------------------- */
typedef struct BtApp {
    GtkApplication  *gtk_app;
    BtDatabase      *db;
    GHashTable      *editors;
    GHashTable      *bn_editors;
    GtkWidget       *library_window;
    void           (*notify_changed)(struct BtApp *app);
    void           (*notify_tasks)(struct BtApp *app);
    void           (*notify_status)(struct BtApp *app, const gchar *message);
    gboolean         sync_running;
    guint            sync_timer;
    GtkToolbarStyle  toolbar_style;
    GPtrArray       *toolbars;
    gchar           *icons_dir;
} BtApp;

/* ---------------------------------------------------------------------------
 * bt_app_widget_add_css() — attach a one-off CSS snippet to a single
 * widget's style context (application priority).  The provider is owned
 * by the style context after this call.
 * ------------------------------------------------------------------------- */
void bt_app_widget_add_css(GtkWidget *widget, const gchar *css_text);

/* ---------------------------------------------------------------------------
 * bt_app_init_icons_dir() — locate the icons/ folder next to the
 * executable (via bt_app_exe_dir(); bt_app_config_init() must have run)
 * and remember it in app->icons_dir.
 * ------------------------------------------------------------------------- */
void bt_app_init_icons_dir(BtApp *app);

/* ---------------------------------------------------------------------------
 * bt_app_icon_image_sized() — build a GtkImage for icon `name` from
 * "<icons_dir>/<name>.png" (or .svg), rendered at an explicit logical
 * pixel size, HiDPI-sharp (backing pixels scale with the display).
 * Returns NULL when no loadable file exists — callers fall back to a
 * text label.
 * ------------------------------------------------------------------------- */
GtkWidget *bt_app_icon_image_sized(BtApp *app, const gchar *name,
                                   gint size);

/* ---------------------------------------------------------------------------
 * bt_app_tool_item_new() — create a toolbar button that honors the
 * app-wide toolbar style: `icon_name` names a local icon file (see
 * bt_app_icon_image_sized), `fallback_markup` is Pango markup rendered
 * as the "icon" when that file is missing (NULL falls back to the plain
 * label).  The label shows in text/both modes.
 * ------------------------------------------------------------------------- */
GtkToolItem *bt_app_tool_item_new(BtApp *app, const gchar *icon_name,
                                  const gchar *fallback_markup,
                                  const gchar *label,
                                  const gchar *tooltip);

/* ---------------------------------------------------------------------------
 * bt_app_register_toolbar() — apply the current style to `toolbar`, keep
 * it updated when the style changes, and offer the icons/both/text radio
 * menu on right-click.  The toolbar unregisters itself when destroyed.
 * ------------------------------------------------------------------------- */
void bt_app_register_toolbar(BtApp *app, GtkWidget *toolbar);

/* ---------------------------------------------------------------------------
 * bt_app_set_toolbar_style() — change the style on every live toolbar
 * and persist the choice ("toolbar_style" = icons|both|text).
 * ------------------------------------------------------------------------- */
void bt_app_set_toolbar_style(BtApp *app, GtkToolbarStyle style);

/* bt_app_load_toolbar_style() — read the persisted style into the app
 * context (default: icons only).                                            */
void bt_app_load_toolbar_style(BtApp *app);

/* ---------------------------------------------------------------------------
 * bt_app_status() — post a one-line event message to the library window's
 * status bar (printf-style).  Safe to call from anywhere on the main
 * thread: a no-op until the library window has installed notify_status.
 * ------------------------------------------------------------------------- */
void bt_app_status(BtApp *app, const gchar *fmt, ...) G_GNUC_PRINTF(2, 3);

/* ---------------------------------------------------------------------------
 * bt_app_notify_changed() — fire the notify_changed hook (full refresh:
 * sidebar + task pane + open editors).  Safe when the hook is NULL or
 * not yet installed.
 * ------------------------------------------------------------------------- */
void bt_app_notify_changed(BtApp *app);

/* ---------------------------------------------------------------------------
 * bt_app_notice() — run a modal OK message dialog and destroy it.
 * ------------------------------------------------------------------------- */
void bt_app_notice(GtkWindow *parent, GtkMessageType type,
                   const gchar *title, const gchar *fmt, ...)
                   G_GNUC_PRINTF(4, 5);

/* ---------------------------------------------------------------------------
 * bt_app_confirm() — run a modal Yes/No question dialog; TRUE on Yes.
 * ------------------------------------------------------------------------- */
gboolean bt_app_confirm(GtkWindow *parent, const gchar *title,
                        const gchar *fmt, ...) G_GNUC_PRINTF(3, 4);

/* ---------------------------------------------------------------------------
 * Config — same model as Blue Notes: an ini next to the binary (portable
 * mode) falling back to ~/.config/hacienda/hacienda.ini when that
 * directory is unwritable.  Loaded ONCE into memory; written through on
 * every change.  Keys used (see hacienda.ini.defaults): sync —
 * google_sync_enabled, google_client_id, google_client_secret,
 * gtasks_refresh_token, sync_interval_min; Blue Notes — blue_notes_sync,
 * blue_notes_cli, blue_notes_embed_list; UI — toolbar_style,
 * bold_task_titles, native_menubar,
 * show_completed, sidebar_visible, weekly_forecast, win_w, win_h.
 * ------------------------------------------------------------------------- */
void      bt_app_config_init(const gchar *argv0);
gchar    *bt_app_config_get(const gchar *key);         /* NULL when unset   */
void      bt_app_config_set(const gchar *key, const gchar *value);

/* bt_app_config_get_bool() — read a 0/1 setting; `def` when unset.  The
 * app only ever writes "0"/"1", so any other stored value reads as "1".     */
gboolean  bt_app_config_get_bool(const gchar *key, gboolean def);

/* bt_app_exe_dir() — the directory holding the binary, resolved once by
 * bt_app_config_init().  Borrowed string; do not free.                      */
const gchar *bt_app_exe_dir(void);

/* ---------------------------------------------------------------------------
 * Date helpers shared by the two windows and the sync engine.
 * ------------------------------------------------------------------------- */

/* bt_day_bounds() — local midnight bounds of "today + offset_days":
 * lo = that day's local midnight, hi = the next day's.                      */
void bt_day_bounds(gint offset_days, gint64 *lo, gint64 *hi);

/* bt_due_format() — "" for no date, else e.g. "Jul 13, 2026".  Returns a
 * new string (g_free it).                                                   */
gchar *bt_due_format(gint64 due);

/* bt_due_format_iso() — "" for no date, else the canonical "YYYY-MM-DD"
 * spelling (local calendar day).  Returns a new string (g_free it).         */
gchar *bt_due_format_iso(gint64 due);

/* bt_due_color() — urgency tint for a due timestamp: overdue red, today
 * gold, ahead green (the Blue Notes action-item palette), or NULL for no
 * tint (due == 0).  Static string; do not free.                             */
const gchar *bt_due_color(gint64 due);

/* bt_due_parse() — parse "YYYY-MM-DD" (also "M/D/YY[YY]") into a local-
 * midnight unix timestamp; 0 when the text is empty/unparseable.            */
gint64 bt_due_parse(const gchar *text);

/* bt_due_from_ymd() — validated year/month/day → local-midnight unix
 * timestamp; 0 when the fields are out of range.                            */
gint64 bt_due_from_ymd(gint y, gint m, gint d);

#endif /* BT_APP_H */
