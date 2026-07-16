/* ===========================================================================
 * library_window.h — the main Hacienda window
 *
 * Layout (Blue Notes design language: plain GtkWindow, one unified
 * toolbar, sidebar | content pane, bottom status bar):
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ menubar (File / Help)                                        │
 *   │ toolbar: Sidebar │ New Task  Delete Task  Sync  Show/Hide ✓  │
 *   ├───────────────┬──────────────────────────────────────────────┤
 *   │ Pinned Tasks  │  ✓ │ Task (tall rows: title, notes preview,  │
 *   │ All Tasks     │    │ attachments, subtasks) │ Due │ Pinned   │
 *   │ Due Today     │                                              │
 *   │ Wkly Forecast │                                              │
 *   │ ── Lists ──   │                                              │
 *   │ <the lists>   │                                              │
 *   │       + ✎ −   │  (sidebar mini bar: new/edit/delete list)    │
 *   ├───────────────┴──────────────────────────────────────────────┤
 *   │ selection info                          latest event message │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * The sidebar's top four rows are VIRTUAL lists — aggregates over every
 * real list (pinned flag / all / due today / the current week, Sunday
 * through Saturday, as seven day sections; hidden entirely while the
 * "weekly_forecast" setting is off).  Tasks cannot be created inside
 * them; New Task needs a real list selected.
 * =========================================================================== */

#ifndef BT_LIBRARY_WINDOW_H
#define BT_LIBRARY_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * bt_library_window_new() — build and show the library window; installs
 * app->notify_changed / notify_tasks / notify_status and stores itself
 * in app->library_window.  `db_path` is kept for the sync worker (which
 * opens its own connection on it).
 * ------------------------------------------------------------------------- */
GtkWidget *bt_library_window_new(BtApp *app, const gchar *db_path);

/* ---------------------------------------------------------------------------
 * bt_library_apply_native_menubar() — move the library menu into (or out
 * of) the native macOS menu bar.  A no-op unless built with HAVE_GTKOSX
 * (gtk-mac-integration-gtk3).  Driven by the "native_menubar" setting:
 * applied at startup by main() and live from the Settings window.
 * ------------------------------------------------------------------------- */
void bt_library_apply_native_menubar(BtApp *app, gboolean native);

#endif /* BT_LIBRARY_WINDOW_H */
