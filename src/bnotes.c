/* ===========================================================================
 * bnotes.c — Blue Notes integration via its CLI (see bnotes.h)
 * =========================================================================== */

#include "bnotes.h"
#include "app.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * bt_bnotes_cli_path() — resolve the blue_notes binary (see bnotes.h).
 * ------------------------------------------------------------------------- */
gchar *
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
    return g_find_program_in_path("blue_notes");
}

/* ---------------------------------------------------------------------------
 * run_cli() — spawn the blue_notes CLI with up to four arguments and
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
        *err = g_strdup("blue_notes CLI not found \xe2\x80\x94 set its "
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
        *err = g_strdup_printf("cannot run blue_notes: %s",
                               gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_free(sout);
        g_free(serr);
        return FALSE;
    }
    if (!g_spawn_check_wait_status(wait_status, NULL)) {
        gchar *detail = serr != NULL ? g_strstrip(serr) : NULL;
        *err = g_strdup_printf("blue_notes reported: %s",
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
 * bt_bnotes_actions() — `action list` → parsed rows (see bnotes.h).
 * ------------------------------------------------------------------------- */
GPtrArray *
bt_bnotes_actions(gchar **err)
{
    *err = NULL;
    gchar *out = NULL;               /* the CLI's stdout                    */
    if (!run_cli("action", "list", NULL, NULL, &out, err))
        return NULL;

    GPtrArray *items = g_ptr_array_new();
    gchar **lines = g_strsplit(out != NULL ? out : "", "\n", -1);
    for (gint i = 0; lines[i] != NULL; i++) {
        if (*lines[i] == '\0')
            continue;
        /* NOTEID:ORD \t [x]|[ ] \t due|- \t text — the text may itself
         * contain tabs, so split into at most four fields.                  */
        gchar **f = g_strsplit(lines[i], "\t", 4);
        if (f[0] != NULL && f[1] != NULL && f[2] != NULL &&
            f[3] != NULL && strchr(f[0], ':') != NULL) {
            BtNoteAction *na = g_new0(BtNoteAction, 1);
            na->ref  = g_strdup(f[0]);
            na->done = strcmp(f[1], "[x]") == 0;
            na->due  = bt_due_parse(f[2]);     /* "-" parses to 0           */
            na->text = g_strdup(f[3]);
            g_ptr_array_add(items, na);
        }
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(out);
    return items;
}

/* ---------------------------------------------------------------------------
 * bt_bnotes_action_set_done() — `action done|undone REF` (see bnotes.h).
 * ------------------------------------------------------------------------- */
gboolean
bt_bnotes_action_set_done(const gchar *ref, gboolean done, gchar **err)
{
    *err = NULL;
    return run_cli("action", done ? "done" : "undone", ref, NULL,
                   NULL, err);
}

/* ---------------------------------------------------------------------------
 * bt_bnotes_action_set_due() — `action due REF DATE|-` (see bnotes.h).
 * ------------------------------------------------------------------------- */
gboolean
bt_bnotes_action_set_due(const gchar *ref, gint64 due, gchar **err)
{
    *err = NULL;
    gchar *date;                     /* ISO date, or "-" to clear           */
    if (due == 0) {
        date = g_strdup("-");
    } else {
        GDateTime *dt = g_date_time_new_from_unix_local(due);
        date = g_date_time_format(dt, "%Y-%m-%d");
        g_date_time_unref(dt);
    }
    gboolean ok = run_cli("action", "due", ref, date, NULL, err);
    g_free(date);
    return ok;
}
