/* ===========================================================================
 * http.h — small libcurl wrapper for Blue Tasks
 *
 * One synchronous helper for the two HTTPS clients in the app (Google
 * OAuth token endpoint + the Google Tasks REST API).  BLOCKING — call it
 * from the sync worker thread, or from a short-lived helper thread for
 * the interactive OAuth exchange; never from the GTK main loop.
 * =========================================================================== */

#ifndef BT_HTTP_H
#define BT_HTTP_H

#include <glib.h>

/* ---------------------------------------------------------------------------
 * bt_http_request() — perform one HTTPS request and return the body.
 *   method       — "GET", "POST", "PATCH", "DELETE".
 *   url          — full request URL.
 *   bearer       — OAuth access token for "Authorization: Bearer …",
 *                  or NULL for none.
 *   content_type — Content-Type header for `body`, or NULL.
 *   extra_header — one additional raw header ("If-Match: …"), or NULL.
 *   body         — request body, or NULL for none (a non-NULL body is
 *                  sent whatever the method; callers pass NULL for
 *                  GET/DELETE).
 *   status       — out: the HTTP status code (0 on transport failure).
 *   err          — out: transport-level error message (owned by caller),
 *                  or NULL; HTTP error statuses are NOT reported here —
 *                  the caller inspects *status.
 * Returns the response body as a NUL-terminated buffer (g_free it; may
 * be empty), or NULL on transport failure.
 * ------------------------------------------------------------------------- */
gchar *bt_http_request(const gchar *method, const gchar *url,
                       const gchar *bearer, const gchar *content_type,
                       const gchar *extra_header, const gchar *body,
                       glong *status, gchar **err);

#endif /* BT_HTTP_H */
