/* ===========================================================================
 * settings_window.h — the Hacienda settings window
 *
 * One singleton window (File → Settings…), in the Blue Notes settings
 * style: plain GtkWindow, bold section headings, write-through controls
 * (every change lands in the ini immediately — no OK/Apply buttons).
 *
 * Sections:
 *   Google Tasks Sync — master enable switch, Sign In / Sign Out with
 *     the session state, auto-sync interval.  Sign in once, stay signed
 *     in: the browser flow stores a refresh token in the ini (see
 *     oauth.h); Sign Out removes it.  The OAuth client itself is not
 *     entered here — it comes from the client-secret JSON file, the
 *     legacy ini keys, or the baked-in build default (oauth.h).
 *   Blue Notes — the action-items integration switch and CLI path.
 *   Appearance — toolbar style, bold task titles, native macOS menubar.
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
