/* ===========================================================================
 * bnotes.c — Records integration via its CLI (see bnotes.h)
 * =========================================================================== */

#include "bnotes.h"
#include "app.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * bt_bnotes_cli_path() — resolve the Records CLI binary: the
 * "blue_notes_cli" setting (path or bare command name), else PATH.
 * Returns a new string (g_free), or NULL when nothing resolves.
 *
 * The config KEY keeps its pre-rename name (`blue_notes_cli`) — it is
 * stored in users' ini files, so renaming it would silently drop their
 * configured path — but the PROGRAM it looks for is now `records`.
 * `blue_notes` stays as a second candidate: an install still carrying the
 * old binary name keeps working, and a stale `blue_notes` on PATH is
 * never preferred over the current one.
 * ------------------------------------------------------------------------- */
static gchar *
bt_bnotes_cli_path(void)
{
    gchar *configured = bt_app_config_get("blue_notes_cli");
    if (configured != NULL) {
        if (g_file_test(configured, G_FILE_TEST_IS_EXECUTABLE))
            return configured;
        /* A bare command name in the setting still searches PATH.           */
        gchar *found = g_find_program_in_path(configured);
        g_free(configured);
        return found;
    }
    gchar *found = g_find_program_in_path("records");
    return found != NULL ? found
                         : g_find_program_in_path("blue_notes");
}

/* ---------------------------------------------------------------------------
 * run_cli() — spawn the Records CLI with up to four arguments and
 * collect stdout.  TRUE on a zero exit; FALSE with *err set otherwise.
 * `out` may be NULL when only success matters.
 * ------------------------------------------------------------------------- */
static gboolean
run_cli(const gchar *a1, const gchar *a2, const gchar *a3,
        const gchar *a4, gchar **out, gchar **err)
{
    if (out != NULL)
        *out = NULL;
    gchar *cli = bt_bnotes_cli_path();
    if (cli == NULL) {
        *err = g_strdup("Records CLI not found \xe2\x80\x94 set its "
                        "path in File \xe2\x86\x92 Settings\xe2\x80\xa6");
        return FALSE;
    }
    gchar *argv[] = { cli, (gchar *)a1, (gchar *)a2, (gchar *)a3,
                      (gchar *)a4, NULL };
    gchar *sout = NULL;              /* captured stdout                     */
    gchar *serr = NULL;              /* captured stderr                     */
    gint   wait_status = 0;
    GError *gerr = NULL;
    gboolean spawned = g_spawn_sync(NULL, argv, NULL,
                                    G_SPAWN_DEFAULT, NULL, NULL,
                                    &sout, &serr, &wait_status, &gerr);
    g_free(cli);
    if (!spawned) {
        *err = g_strdup_printf("cannot run the Records CLI: %s",
                               gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_free(sout);
        g_free(serr);
        return FALSE;
    }
    if (!g_spawn_check_wait_status(wait_status, NULL)) {
        gchar *detail = serr != NULL ? g_strstrip(serr) : NULL;
        *err = g_strdup_printf("Records reported: %s",
                               detail != NULL && *detail != '\0'
                               ? detail : "command failed");
        g_free(sout);
        g_free(serr);
        return FALSE;
    }
    g_free(serr);
    if (out != NULL)
        *out = sout;
    else
        g_free(sout);
    return TRUE;
}

/* bt_bnotes_actions_free() — free an array of BtNoteAction*.  NULL-safe.    */
void
bt_bnotes_actions_free(GPtrArray *a)
{
    if (a == NULL)
        return;
    for (guint i = 0; i < a->len; i++) {
        BtNoteAction *na = g_ptr_array_index(a, i);
        g_free(na->ref);
        g_free(na->text);
        g_free(na);
    }
    g_ptr_array_free(a, TRUE);
}

/* ---------------------------------------------------------------------------
 * bt_bnotes_actions() — `action list --uid` → parsed rows (see bnotes.h).
 * ------------------------------------------------------------------------- */
GPtrArray *
bt_bnotes_actions(gchar **err)
{
    *err = NULL;
    gchar *out = NULL;               /* the CLI's stdout                    */
    if (!run_cli("action", "list", "--uid", NULL, &out, err))
        return NULL;

    GPtrArray *items = g_ptr_array_new();
    gchar **lines = g_strsplit(out != NULL ? out : "", "\n", -1);
    for (gint i = 0; lines[i] != NULL; i++) {
        if (*lines[i] == '\0')
            continue;
        /* UID \t NOTEID:ORD \t [x]|[ ] \t due|- \t text — the text may
         * itself contain tabs, so split into at most five fields.           */
        gchar **f = g_strsplit(lines[i], "\t", 5);
        gchar  *endp = NULL;         /* end of the parsed uid               */
        gint64  uid  = f[0] != NULL
                     ? g_ascii_strtoll(f[0], &endp, 10) : 0;
        /* A row counts only with a well-formed uid and the legacy
         * positional address still in field 2 — anything else is a
         * format we do not understand, and silently mirroring it would
         * bind a task to the wrong item.                                    */
        if (uid > 0 && endp != NULL && *endp == '\0' &&
            f[1] != NULL && f[2] != NULL && f[3] != NULL &&
            f[4] != NULL && strchr(f[1], ':') != NULL) {
            BtNoteAction *na = g_new0(BtNoteAction, 1);
            na->uid  = uid;
            na->ref  = g_strdup(f[1]);
            na->done = strcmp(f[2], "[x]") == 0;
            na->due  = bt_due_parse(f[3]);     /* "-" parses to 0           */
            na->text = g_strdup(f[4]);
            g_ptr_array_add(items, na);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(out);
    return items;
}

/* ---------------------------------------------------------------------------
 * bt_bnotes_supports_uid() — is --uid understood (see bnotes.h)?
 * ------------------------------------------------------------------------- */
gboolean
bt_bnotes_supports_uid(void)
{
    gchar *err = NULL;               /* discarded — the verdict is the
                                      * exit status alone                   */
    gboolean ok = run_cli("action", "list", "--uid", NULL, NULL, &err);
    g_free(err);
    return ok;
}

/* ---------------------------------------------------------------------------
 * bt_bnotes_action_set_done() — `action done|undone UID` (see bnotes.h).
 * ------------------------------------------------------------------------- */
gboolean
bt_bnotes_action_set_done(gint64 uid, gboolean done, gchar **err)
{
    *err = NULL;
    gchar *tok = g_strdup_printf("%" G_GINT64_FORMAT, uid);
    gboolean ok = run_cli("action", done ? "done" : "undone", tok, NULL,
                          NULL, err);
    g_free(tok);
    return ok;
}

/* ---------------------------------------------------------------------------
 * bt_bnotes_action_set_due() — `action due UID DATE|-` (see bnotes.h).
 * ------------------------------------------------------------------------- */
gboolean
bt_bnotes_action_set_due(gint64 uid, gint64 due, gchar **err)
{
    *err = NULL;
    gchar *tok  = g_strdup_printf("%" G_GINT64_FORMAT, uid);
    /* ISO date, or "-" to clear.                                            */
    gchar *date = due == 0 ? g_strdup("-") : bt_due_format_iso(due);
    gboolean ok = run_cli("action", "due", tok, date, NULL, err);
    g_free(date);
    g_free(tok);
    return ok;
}
