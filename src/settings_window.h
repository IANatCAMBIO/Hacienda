/* ===========================================================================
 * settings_window.h — the Blue Tasks settings window
 *
 * One singleton window (File → Settings…), in the Blue Notes settings
 * style: plain GtkWindow, bold section headings, write-through controls
 * (every change lands in the ini immediately — no OK/Apply buttons).
 *
 * Sections:
 *   Google Tasks Sync — OAuth client id/secret (the app-identifying
 *     credentials from the user's Google Cloud console; NOT a grant),
 *     auto-sync interval, Sign In / Sign Out and the session state.
 *     Sign-in is per session: a browser round trip that yields an
 *     in-memory access token only (see oauth.h) — nothing token-like is
 *     ever stored.
 *   Database — where the SQLite file lives (informational).
 * =========================================================================== */

#ifndef BT_SETTINGS_WINDOW_H
#define BT_SETTINGS_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * bt_settings_window_open() — show (or raise) the settings window.
 *   app     — the application context.
 *   parent  — transient parent (the library window).
 *   db_path — the database path shown in the Database section and used
 *             to restart the auto-sync timer when the interval changes.
 * ------------------------------------------------------------------------- */
void bt_settings_window_open(BtApp *app, GtkWindow *parent,
                             const gchar *db_path);

#endif /* BT_SETTINGS_WINDOW_H */
