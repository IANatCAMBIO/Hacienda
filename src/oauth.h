/* ===========================================================================
 * oauth.h — Google OAuth 2.0 for Blue Tasks (installed-app flow)
 *
 * Implements the "OAuth for native apps" flow (RFC 8252) with PKCE:
 *
 *   1. bt_oauth_begin() starts a loopback listener on 127.0.0.1:<ephemeral>
 *      and opens the user's browser on Google's sign-in page.
 *   2. Google redirects the browser to the loopback URL with a one-time
 *      code; the listener catches it and shows a "you can close this tab"
 *      page.
 *   3. The code is exchanged (on a short-lived worker thread — the
 *      exchange is an HTTPS round trip) for an access token plus a
 *      REFRESH token.
 *
 * Sign in once, stay signed in: the refresh token is persisted in the
 * ini ("gtasks_refresh_token", scoped to Google Tasks only, revocable
 * at myaccount.google.com/permissions) and silently exchanged for fresh
 * access tokens as they expire — the browser only reappears if the
 * grant is revoked or Sign Out removes the stored token.  Access tokens
 * themselves stay in memory.
 *
 * The OAuth client (it identifies the app; it grants nothing by itself)
 * resolves in order: Google's client-secret JSON file next to the
 * binary or under the user config dir → the legacy ini keys
 * google_client_id / google_client_secret (no UI writes them) → the
 * client baked in at build time via client_credentials.mk.  The
 * registration is normally the developer's one-time job; users just
 * see the browser sign-in.
 *
 * Scope requested: https://www.googleapis.com/auth/tasks (read/write).
 * =========================================================================== */

#ifndef BT_OAUTH_H
#define BT_OAUTH_H

#include "app.h"

/* Completion callback for bt_oauth_begin(); runs on the main thread.
 * `error` is a human-readable failure reason (NULL on success; not owned
 * by the callee).                                                           */
typedef void (*BtOauthDoneFn)(gboolean ok, const gchar *error,
                              gpointer user_data);

/* ---------------------------------------------------------------------------
 * bt_oauth_init() — snapshot the OAuth client credentials (id/secret,
 * resolved as described above) AND the stored refresh token into
 * mutex-guarded statics, so the sync worker thread never touches the
 * main-thread config.  Re-reading the refresh token means calling this
 * also resets the signed-in state to what the ini says.  Call at
 * startup and after the settings change; main thread only.
 * ------------------------------------------------------------------------- */
void bt_oauth_init(void);

/* bt_oauth_have_client() — TRUE when a client id is configured.             */
gboolean bt_oauth_have_client(void);

/* ---------------------------------------------------------------------------
 * bt_oauth_begin() — run the interactive browser sign-in flow.  One flow
 * at a time; `done` always fires exactly once on the main thread.
 * ------------------------------------------------------------------------- */
void bt_oauth_begin(GtkWindow *parent, BtOauthDoneFn done,
                    gpointer user_data);

/* bt_oauth_authenticated() — TRUE while sync can get a token without a
 * browser trip: a valid in-memory access token OR a stored refresh
 * token.                                                                    */
gboolean bt_oauth_authenticated(void);

/* bt_oauth_signout() — drop the in-memory access token AND the stored
 * refresh token (main thread).                                              */
void bt_oauth_signout(void);

/* ---------------------------------------------------------------------------
 * bt_oauth_access_token() — a currently valid access token, silently
 * refreshing via the stored refresh token when needed.  BLOCKING when a
 * refresh runs (one HTTPS round trip) — call from the sync worker
 * thread only.  Returns a new string (g_free it), or NULL with *err set
 * (g_free it) — e.g. never signed in, or Google revoked the grant.
 * ------------------------------------------------------------------------- */
gchar *bt_oauth_access_token(gchar **err);

#endif /* BT_OAUTH_H */
