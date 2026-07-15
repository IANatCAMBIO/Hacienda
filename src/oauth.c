/* ===========================================================================
 * oauth.c — Google OAuth 2.0 installed-app flow with PKCE (see oauth.h)
 * =========================================================================== */

#include "oauth.h"
#include "http.h"
#include "json.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#define BT_OAUTH_SCOPE     "https://www.googleapis.com/auth/tasks"
#define BT_OAUTH_AUTH_URL  "https://accounts.google.com/o/oauth2/v2/auth"
#define BT_OAUTH_TOKEN_URL "https://oauth2.googleapis.com/token"
#define BT_OAUTH_TIMEOUT_S 300       /* give up waiting for the redirect    */

/* ---------------------------------------------------------------------------
 * The app's own OAuth client, baked in at build time (put the values in
 * client_credentials.mk — see the Makefile).  Users never see or enter
 * these: they identify Hacienda to Google, exactly like the embedded
 * client every desktop OAuth app ships (Google documents that installed-
 * app client secrets are NOT confidential).  The one-time registration
 * is the developer's job; after that "Sync" is just a browser sign-in.
 * The ini keys google_client_id/google_client_secret still OVERRIDE the
 * built-ins for people who want to run their own client.
 * ------------------------------------------------------------------------- */
#ifndef BT_GOOGLE_CLIENT_ID
#define BT_GOOGLE_CLIENT_ID ""
#endif
#ifndef BT_GOOGLE_CLIENT_SECRET
#define BT_GOOGLE_CLIENT_SECRET ""
#endif

static gboolean token_post(const gchar *form, gchar **access,
                           gchar **refresh, gint64 *expires_in,
                           gchar **error);

/* ---------------------------------------------------------------------------
 * Client credentials + tokens — written on the main thread (bt_oauth_init
 * and the flow completion), read/refreshed by the sync worker under the
 * mutex.  The refresh token persists in the ini ("gtasks_refresh_token");
 * access tokens stay in memory.
 * ------------------------------------------------------------------------- */
static GMutex  cred_lock;
static gchar  *cred_client_id     = NULL;
static gchar  *cred_client_secret = NULL;
static gchar  *cred_refresh_token = NULL;   /* the persistent grant         */
static gchar  *session_access     = NULL;   /* current access token         */
static gint64  session_expiry     = 0;      /* unix time it goes stale      */

/* esc() — shorthand for the URI escaping every form/URL builder needs.      */
static gchar *
esc(const gchar *s)
{
    return g_uri_escape_string(s, NULL, FALSE);
}

/* session_cache() — store a fresh access token; call under cred_lock.
 * The 60 s haircut keeps a token from expiring mid-request.                 */
static void
session_cache(const gchar *access, gint64 expires_in)
{
    g_free(session_access);
    session_access = g_strdup(access);
    session_expiry = g_get_real_time() / G_USEC_PER_SEC + expires_in - 60;
}

/* The single in-flight interactive flow, or inactive when service==NULL.    */
static struct {
    GSocketService *service;         /* loopback listener                   */
    guint16         port;            /* its bound port                      */
    gchar          *verifier;        /* PKCE code_verifier                  */
    gchar          *state;           /* CSRF state parameter                */
    guint           timeout_id;      /* the give-up timer                   */
    BtOauthDoneFn   done;            /* completion callback                 */
    gpointer        user_data;
} flow;

/* The Google Cloud console's downloaded OAuth client file, looked for
 * next to the binary, then in the user config dir.  It is gitignored —
 * never commit it.                                                          */
#define BT_CLIENT_FILE "client_secret.apps.googleusercontent.com.json"

/* ---------------------------------------------------------------------------
 * load_client_file() — read client id/secret out of Google's standard
 * client-secret JSON ({"installed": {"client_id": …}}; "web" is
 * accepted too).  Fills the id/secret out-params (g_free) and returns
 * TRUE when a usable file was found.
 * ------------------------------------------------------------------------- */
static gboolean
load_client_file(gchar **id, gchar **secret)
{
    const gchar *dirs[2] = { bt_app_exe_dir(), g_get_user_config_dir() };
    for (gsize i = 0; i < G_N_ELEMENTS(dirs); i++) {
        if (dirs[i] == NULL)
            continue;
        gchar *path = i == 0
            ? g_build_filename(dirs[i], BT_CLIENT_FILE, NULL)
            : g_build_filename(dirs[i], "hacienda", BT_CLIENT_FILE,
                               NULL);
        gchar *text = NULL;
        gboolean loaded = g_file_get_contents(path, &text, NULL, NULL);
        g_free(path);
        if (!loaded)
            continue;
        BtJson *root = bt_json_parse(text, -1);
        g_free(text);
        BtJson *client = bt_json_get(root, "installed");
        if (client == NULL)
            client = bt_json_get(root, "web");
        const gchar *cid = bt_json_str(client, "client_id");
        if (cid != NULL) {
            *id = g_strdup(cid);
            *secret = g_strdup(bt_json_str(client, "client_secret"));
            bt_json_free(root);
            return TRUE;
        }
        bt_json_free(root);
    }
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * bt_oauth_init() — snapshot the client credentials (see oauth.h):
 * Google's client-secret JSON file first, then the legacy ini keys,
 * then the baked-in client.
 * ------------------------------------------------------------------------- */
void
bt_oauth_init(void)
{
    gchar *file_id = NULL, *file_secret = NULL;
    load_client_file(&file_id, &file_secret);

    g_mutex_lock(&cred_lock);
    g_free(cred_client_id);
    g_free(cred_client_secret);
    g_free(cred_refresh_token);
    cred_client_id     = file_id;
    cred_client_secret = file_secret;
    cred_refresh_token = bt_app_config_get("gtasks_refresh_token");
    if (cred_client_id == NULL) {
        cred_client_id     = bt_app_config_get("google_client_id");
        cred_client_secret = bt_app_config_get("google_client_secret");
    }
    if (cred_client_id == NULL && *BT_GOOGLE_CLIENT_ID != '\0')
        cred_client_id = g_strdup(BT_GOOGLE_CLIENT_ID);
    if (cred_client_secret == NULL && *BT_GOOGLE_CLIENT_SECRET != '\0')
        cred_client_secret = g_strdup(BT_GOOGLE_CLIENT_SECRET);
    g_mutex_unlock(&cred_lock);
}

/* bt_oauth_have_client() — TRUE when a client id is configured.             */
gboolean
bt_oauth_have_client(void)
{
    g_mutex_lock(&cred_lock);
    gboolean yes = cred_client_id != NULL;
    g_mutex_unlock(&cred_lock);
    return yes;
}

/* bt_oauth_authenticated() — TRUE when sync can get a token without a
 * browser trip (valid session token OR stored refresh token).               */
gboolean
bt_oauth_authenticated(void)
{
    g_mutex_lock(&cred_lock);
    gboolean yes = cred_refresh_token != NULL ||
                   (session_access != NULL &&
                    g_get_real_time() / G_USEC_PER_SEC < session_expiry);
    g_mutex_unlock(&cred_lock);
    return yes;
}

/* bt_oauth_signout() — drop the session token AND the stored grant.         */
void
bt_oauth_signout(void)
{
    bt_app_config_set("gtasks_refresh_token", NULL);
    g_mutex_lock(&cred_lock);
    g_clear_pointer(&session_access, g_free);
    g_clear_pointer(&cred_refresh_token, g_free);
    session_expiry = 0;
    g_mutex_unlock(&cred_lock);
}

/* ---------------------------------------------------------------------------
 * bt_oauth_access_token() — cached token, or a silent refresh via the
 * stored refresh token (see oauth.h).  BLOCKING on refresh; sync worker
 * thread only.
 * ------------------------------------------------------------------------- */
gchar *
bt_oauth_access_token(gchar **err)
{
    *err = NULL;
    g_mutex_lock(&cred_lock);
    if (session_access != NULL &&
        g_get_real_time() / G_USEC_PER_SEC < session_expiry) {
        gchar *tok = g_strdup(session_access);
        g_mutex_unlock(&cred_lock);
        return tok;
    }
    if (cred_refresh_token == NULL || cred_client_id == NULL) {
        g_mutex_unlock(&cred_lock);
        *err = g_strdup("not signed in to Google \xe2\x80\x94 sign in "
                        "from the Sync button or File \xe2\x86\x92 "
                        "Settings\xe2\x80\xa6");
        return NULL;
    }
    gchar *cid  = g_strdup(cred_client_id);
    gchar *csec = g_strdup(cred_client_secret);
    gchar *rtok = g_strdup(cred_refresh_token);
    g_mutex_unlock(&cred_lock);

    /* Silent refresh — network I/O runs OUTSIDE the lock.                   */
    gchar *rtok_esc = esc(rtok);
    gchar *form = g_strdup_printf(
        "grant_type=refresh_token&refresh_token=%s&client_id=%s"
        "&client_secret=%s",
        rtok_esc, cid, csec != NULL ? csec : "");
    gchar  *access = NULL;           /* the refreshed token                 */
    gint64  expires_in = 0;
    gchar  *perr = NULL;             /* token_post's failure reason         */
    gboolean ok = token_post(form, &access, NULL, &expires_in, &perr);
    g_free(form);
    g_free(rtok_esc);
    g_free(rtok);
    g_free(cid);
    g_free(csec);

    if (!ok) {
        *err = g_strdup_printf("%s \xe2\x80\x94 sign in again from File "
                               "\xe2\x86\x92 Settings\xe2\x80\xa6",
                               perr != NULL ? perr : "token refresh failed");
        g_free(perr);
        return NULL;
    }

    /* Cache only while still signed in: Sign Out may have cleared the
     * refresh token while this round trip was in flight, and re-caching
     * would resurrect the "signed-out" session for another hour.            */
    g_mutex_lock(&cred_lock);
    if (cred_refresh_token != NULL)
        session_cache(access, expires_in);
    g_mutex_unlock(&cred_lock);
    return access;
}

/* ---------------------------------------------------------------------------
 * base64url() — RFC 4648 §5 unpadded base64url of a byte buffer.
 * ------------------------------------------------------------------------- */
static gchar *
base64url(const guchar *data, gsize len)
{
    gchar *b64 = g_base64_encode(data, len);
    for (gchar *p = b64; *p != '\0'; p++) {
        if (*p == '+') *p = '-';
        else if (*p == '/') *p = '_';
        else if (*p == '=') { *p = '\0'; break; }
    }
    return b64;
}

/* random_urlsafe() — base64url of `n` bytes of OS randomness (PKCE
 * verifier / state).  Falls back to GRand only if /dev/urandom is
 * unreadable, which does not happen on the supported platforms.             */
static gchar *
random_urlsafe(gsize n)
{
    guchar buf[64];
    g_assert(n <= sizeof buf);
    gsize got = 0;                   /* bytes actually read                 */
    FILE *f = fopen("/dev/urandom", "rb");
    if (f != NULL) {
        got = fread(buf, 1, n, f);
        fclose(f);
    }
    for (gsize i = got; i < n; i++)
        buf[i] = (guchar)g_random_int_range(0, 256);
    return base64url(buf, n);
}

/* sha256_base64url() — the PKCE S256 code challenge for a verifier.         */
static gchar *
sha256_base64url(const gchar *verifier)
{
    guchar digest[32];
    gsize dlen = sizeof digest;
    GChecksum *ck = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(ck, (const guchar *)verifier, -1);
    g_checksum_get_digest(ck, digest, &dlen);
    g_checksum_free(ck);
    return base64url(digest, dlen);
}

/* ---------------------------------------------------------------------------
 * flow_finish() — tear down the in-flight flow and fire its callback
 * exactly once (main thread).
 * ------------------------------------------------------------------------- */
static void
flow_finish(gboolean ok, const gchar *error)
{
    if (flow.service == NULL)
        return;
    BtOauthDoneFn done = flow.done;  /* fire after teardown                 */
    gpointer user     = flow.user_data;
    if (flow.timeout_id != 0)
        g_source_remove(flow.timeout_id);
    g_socket_service_stop(flow.service);
    g_socket_listener_close(G_SOCKET_LISTENER(flow.service));
    g_object_unref(flow.service);
    g_free(flow.verifier);
    g_free(flow.state);
    memset(&flow, 0, sizeof flow);
    if (done != NULL)
        done(ok, error, user);
}

/* ---------------------------------------------------------------------------
 * token_post() — POST a form body to the token endpoint and parse the
 * token fields out of the JSON reply (shared by the code exchange and
 * the silent refresh — the two paths previously duplicated all of this).
 * `refresh` may be NULL when the caller doesn't expect one.  Returns
 * TRUE with the access token and expiry set; FALSE with the error set
 * (g_free).  BLOCKING; worker threads only.
 * ------------------------------------------------------------------------- */
static gboolean
token_post(const gchar *form, gchar **access, gchar **refresh,
           gint64 *expires_in, gchar **error)
{
    *access = NULL;
    *error = NULL;
    if (refresh != NULL)
        *refresh = NULL;
    glong status = 0;
    gchar *terr = NULL;
    gchar *body = bt_http_request("POST", BT_OAUTH_TOKEN_URL, NULL,
                                  "application/x-www-form-urlencoded",
                                  NULL, form, &status, &terr);
    if (body == NULL) {
        *error = terr != NULL ? terr : g_strdup("network failure");
        return FALSE;
    }
    g_free(terr);
    BtJson *root = bt_json_parse(body, -1);
    gboolean ok = FALSE;
    if (status == 200 && bt_json_str(root, "access_token") != NULL) {
        *access = g_strdup(bt_json_str(root, "access_token"));
        if (refresh != NULL)
            *refresh = g_strdup(bt_json_str(root, "refresh_token"));
        BtJson *exp = bt_json_get(root, "expires_in");
        *expires_in = (exp != NULL && exp->type == BT_JSON_NUMBER)
                      ? (gint64)exp->num : 3600;
        ok = TRUE;
    } else {
        const gchar *desc = bt_json_str(root, "error_description");
        const gchar *code = bt_json_str(root, "error");
        *error = g_strdup_printf("token request failed (HTTP %ld): %s",
                                 status,
                                 desc != NULL ? desc
                                 : code != NULL ? code : "unknown error");
    }
    bt_json_free(root);
    g_free(body);
    return ok;
}

/* --------------------------------------------------------------------------
 * The token exchange runs on a short-lived worker thread (it is an HTTPS
 * round trip); its result is marshalled back to the main thread.
 * ------------------------------------------------------------------------- */
typedef struct {
    gchar    *code;                  /* authorization code from redirect    */
    gchar    *redirect_uri;
    gchar    *verifier;
    gchar    *access;                /* out: the session token              */
    gchar    *refresh;               /* out: the persistent grant           */
    gint64    expires_in;
    gchar    *error;                 /* out: failure reason                 */
} ExchangeJob;

/* exchange_job_free() — free an ExchangeJob and everything it owns.         */
static void
exchange_job_free(ExchangeJob *job)
{
    g_free(job->code);
    g_free(job->redirect_uri);
    g_free(job->verifier);
    g_free(job->access);
    g_free(job->refresh);
    g_free(job->error);
    g_free(job);
}

/* exchange_apply() — main-thread completion: cache the session token,
 * persist the refresh token (sign in once, stay signed in), finish the
 * flow.  When the flow already ended (the 5-minute timeout fired while
 * the exchange was in flight), the tokens are DISCARDED — the user was
 * just told sign-in failed, and silently storing a grant anyway would
 * leave the UI saying signed-out while the app syncs.                       */
static gboolean
exchange_apply(gpointer data)
{
    ExchangeJob *job = data;
    if (flow.service == NULL) {
        exchange_job_free(job);
        return G_SOURCE_REMOVE;
    }
    if (job->error == NULL && job->access != NULL) {
        if (job->refresh != NULL)
            bt_app_config_set("gtasks_refresh_token", job->refresh);
        g_mutex_lock(&cred_lock);
        if (job->refresh != NULL) {
            g_free(cred_refresh_token);
            cred_refresh_token = g_strdup(job->refresh);
        }
        session_cache(job->access, job->expires_in);
        g_mutex_unlock(&cred_lock);
        flow_finish(TRUE, NULL);
    } else {
        flow_finish(FALSE, job->error != NULL
                    ? job->error : "Google returned no access token");
    }
    exchange_job_free(job);
    return G_SOURCE_REMOVE;
}

/* exchange_thread() — worker: authorization code → access token.            */
static gpointer
exchange_thread(gpointer data)
{
    ExchangeJob *job = data;
    g_mutex_lock(&cred_lock);
    gchar *cid  = g_strdup(cred_client_id);
    gchar *csec = g_strdup(cred_client_secret);
    g_mutex_unlock(&cred_lock);

    gchar *code_esc = esc(job->code);
    gchar *uri_esc  = esc(job->redirect_uri);
    gchar *form = g_strdup_printf(
        "grant_type=authorization_code&code=%s&client_id=%s"
        "&client_secret=%s&redirect_uri=%s&code_verifier=%s",
        code_esc, cid != NULL ? cid : "", csec != NULL ? csec : "",
        uri_esc, job->verifier);
    token_post(form, &job->access, &job->refresh, &job->expires_in,
               &job->error);
    g_free(form);
    g_free(code_esc);
    g_free(uri_esc);
    g_free(cid);
    g_free(csec);
    g_idle_add(exchange_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * redirect_param() — extract one query parameter from the redirect's
 * request line ("GET /?code=…&state=… HTTP/1.1").  Returns the decoded
 * value (g_free) or NULL.
 * ------------------------------------------------------------------------- */
static gchar *
redirect_param(const gchar *request, const gchar *name)
{
    const gchar *q = strchr(request, '?');
    if (q == NULL)
        return NULL;
    q++;
    const gchar *end = strpbrk(q, " \r\n");     /* end of the URL           */
    if (end == NULL)
        end = q + strlen(q);
    gchar *query = g_strndup(q, (gsize)(end - q));
    gchar *value = NULL;             /* the decoded parameter               */
    gchar **parts = g_strsplit(query, "&", -1);
    for (gint i = 0; parts[i] != NULL && value == NULL; i++) {
        gchar *eq = strchr(parts[i], '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        if (strcmp(parts[i], name) == 0)
            value = g_uri_unescape_string(eq + 1, NULL);
    }
    g_strfreev(parts);
    g_free(query);
    return value;
}

/* redirect_respond() — write the tiny "done" HTML page and close.           */
static void
redirect_respond(GSocketConnection *conn, const gchar *message)
{
    gchar *page = g_strdup_printf(
        "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
        "Connection: close\r\n\r\n"
        "<html><head><title>Hacienda</title></head>"
        "<body style=\"font-family: sans-serif; margin: 3em\">"
        "<h2>Hacienda</h2><p>%s</p>"
        "<p>You can close this tab and return to the app.</p>"
        "</body></html>", message);
    GOutputStream *out =
        g_io_stream_get_output_stream(G_IO_STREAM(conn));
    g_output_stream_write_all(out, page, strlen(page), NULL, NULL, NULL);
    g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
    g_free(page);
}

/* redirect_read_done() — the browser's request arrived: validate it,
 * answer it, and hand the code to the exchange worker.                      */
static void
redirect_read_done(GObject *src, GAsyncResult *res, gpointer data)
{
    GSocketConnection *conn = data;
    gchar *buf = g_object_get_data(G_OBJECT(conn), "bt-buf");
    gssize n = g_input_stream_read_finish(G_INPUT_STREAM(src), res, NULL);
    if (n <= 0 || flow.service == NULL) {
        g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
        g_object_unref(conn);
        return;
    }
    buf[MIN(n, 8191)] = '\0';

    /* Browsers also ask for /favicon.ico — ignore everything that is not
     * the redirect (no code/error parameter).                               */
    gchar *code  = redirect_param(buf, "code");
    gchar *error = redirect_param(buf, "error");
    gchar *state = redirect_param(buf, "state");
    if (code == NULL && error == NULL) {
        g_io_stream_close(G_IO_STREAM(conn), NULL, NULL);
        g_object_unref(conn);
        return;
    }

    if (error != NULL) {
        redirect_respond(conn, "Access was denied.");
        gchar *msg = g_strdup_printf("Google reported: %s", error);
        flow_finish(FALSE, msg);
        g_free(msg);
    } else if (state == NULL || flow.state == NULL ||
               strcmp(state, flow.state) != 0) {
        redirect_respond(conn, "State mismatch — try again.");
        flow_finish(FALSE, "OAuth state mismatch");
    } else {
        redirect_respond(conn, "Signed in \xe2\x9c\x93");
        ExchangeJob *job = g_new0(ExchangeJob, 1);
        job->code         = g_strdup(code);
        job->redirect_uri = g_strdup_printf("http://127.0.0.1:%u",
                                            (guint)flow.port);
        job->verifier     = g_strdup(flow.verifier);
        GThread *th = g_thread_new("bt-oauth-exchange",
                                   exchange_thread, job);
        g_thread_unref(th);
        /* The flow stays "open" until exchange_apply finishes it — but
         * the listener has served its purpose.                              */
    }
    g_free(code);
    g_free(error);
    g_free(state);
    g_object_unref(conn);
}

/* redirect_incoming() — a browser connected to the loopback listener.       */
static gboolean
redirect_incoming(GSocketService *service, GSocketConnection *conn,
                  GObject *source, gpointer data)
{
    (void)service; (void)source; (void)data;
    gchar *buf = g_new0(gchar, 8192);
    g_object_set_data_full(G_OBJECT(conn), "bt-buf", buf, g_free);
    g_input_stream_read_async(
        g_io_stream_get_input_stream(G_IO_STREAM(conn)),
        buf, 8191, G_PRIORITY_DEFAULT, NULL,
        redirect_read_done, g_object_ref(conn));
    return TRUE;
}

/* flow_timeout() — nobody came back from the browser.                       */
static gboolean
flow_timeout(gpointer data)
{
    (void)data;
    flow.timeout_id = 0;
    flow_finish(FALSE, "timed out waiting for the browser sign-in");
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * bt_oauth_begin() — run the interactive sign-in flow (see oauth.h).
 * access_type=offline requests the refresh token; prompt=consent forces
 * the consent screen so Google issues one EVERY interactive sign-in —
 * without it, only the first-ever grant includes a refresh token and a
 * re-sign-in after Sign Out would leave us with nothing to store.
 * ------------------------------------------------------------------------- */
void
bt_oauth_begin(GtkWindow *parent, BtOauthDoneFn done, gpointer user_data)
{
    bt_oauth_init();
    g_mutex_lock(&cred_lock);
    gchar *cid = g_strdup(cred_client_id);
    g_mutex_unlock(&cred_lock);

    if (cid == NULL) {
        if (done != NULL)
            done(FALSE, "No OAuth client configured.  Place the Google "
                 "Cloud console's \xe2\x80\x9c""Desktop app\xe2\x80\x9d "
                 "client-secret JSON (" BT_CLIENT_FILE ") next to the "
                 "app, or build with client_credentials.mk (Google "
                 "Tasks API enabled either way).", user_data);
        return;
    }
    if (flow.service != NULL) {
        if (done != NULL)
            done(FALSE, "a sign-in is already in progress", user_data);
        g_free(cid);
        return;
    }

    /* Loopback listener on an ephemeral port.                               */
    GError *gerr = NULL;
    flow.service = g_socket_service_new();
    flow.port = g_socket_listener_add_any_inet_port(
        G_SOCKET_LISTENER(flow.service), NULL, &gerr);
    if (flow.port == 0) {
        gchar *msg = g_strdup_printf("cannot open loopback port: %s",
                                     gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_clear_object(&flow.service);
        if (done != NULL)
            done(FALSE, msg, user_data);
        g_free(msg);
        g_free(cid);
        return;
    }
    flow.verifier   = random_urlsafe(32);
    flow.state      = random_urlsafe(16);
    flow.done       = done;
    flow.user_data  = user_data;
    flow.timeout_id = g_timeout_add_seconds(BT_OAUTH_TIMEOUT_S,
                                            flow_timeout, NULL);
    g_signal_connect(flow.service, "incoming",
                     G_CALLBACK(redirect_incoming), NULL);
    g_socket_service_start(flow.service);

    gchar *challenge = sha256_base64url(flow.verifier);
    gchar *cid_esc   = esc(cid);
    gchar *scope_esc = esc(BT_OAUTH_SCOPE);
    gchar *redirect  = g_strdup_printf("http%%3A%%2F%%2F127.0.0.1%%3A%u",
                                       (guint)flow.port);
    gchar *url = g_strdup_printf(
        BT_OAUTH_AUTH_URL "?client_id=%s&redirect_uri=%s"
        "&response_type=code&scope=%s&code_challenge=%s"
        "&code_challenge_method=S256&state=%s"
        "&access_type=offline&prompt=consent",
        cid_esc, redirect, scope_esc, challenge, flow.state);

    if (!gtk_show_uri_on_window(parent, url, GDK_CURRENT_TIME, &gerr)) {
        gchar *msg = g_strdup_printf("cannot open the browser: %s",
                                     gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        flow_finish(FALSE, msg);
        g_free(msg);
    }
    g_free(challenge);
    g_free(cid_esc);
    g_free(scope_esc);
    g_free(redirect);
    g_free(url);
    g_free(cid);
}
