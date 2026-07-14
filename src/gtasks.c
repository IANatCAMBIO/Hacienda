/* ===========================================================================
 * gtasks.c — two-way Google Tasks sync (see gtasks.h)
 * =========================================================================== */

#include "gtasks.h"
#include "oauth.h"
#include "http.h"
#include "json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BT_TASKS_API "https://tasks.googleapis.com/tasks/v1"

/* ---------------------------------------------------------------------------
 * Remote snapshots — the fields of a Google tasklist / task this sync
 * uses.  Strings are owned.
 * ------------------------------------------------------------------------- */
typedef struct {
    gchar    *gid;                   /* Google tasklist id                  */
    gchar    *title;
    gint64    updated;               /* unix                                */
    gboolean  matched;               /* consumed by the match pass          */
} RemoteList;

typedef struct {
    gchar    *gid;                   /* Google task id                      */
    gchar    *title;
    gchar    *notes;
    gchar    *parent_gid;            /* Google id of the parent, or NULL    */
    gint64    due;                   /* unix local midnight; 0 = none       */
    gint64    updated;               /* unix                                */
    gboolean  done;
    gboolean  deleted;
    gboolean  matched;
} RemoteTask;

static void
remote_list_free(RemoteList *r)
{
    g_free(r->gid);
    g_free(r->title);
    g_free(r);
}

static void
remote_task_free(RemoteTask *r)
{
    g_free(r->gid);
    g_free(r->title);
    g_free(r->notes);
    g_free(r->parent_gid);
    g_free(r);
}

/* Counters for the end-of-sync status line.                                 */
typedef struct {
    gint pushed;                     /* local → Google writes               */
    gint pulled;                     /* Google → local writes               */
    gint deleted;                    /* rows removed on either side         */
} SyncStats;

/* ---------------------------------------------------------------------------
 * Time conversions.
 * ------------------------------------------------------------------------- */

/* rfc3339_to_unix() — parse a Google `updated` timestamp.  0 on failure.    */
static gint64
rfc3339_to_unix(const gchar *s)
{
    if (s == NULL)
        return 0;
    GDateTime *dt = g_date_time_new_from_iso8601(s, NULL);
    if (dt == NULL)
        return 0;
    gint64 u = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return u;
}

/* due_from_rfc3339() — a Google `due` carries only the DATE (the time
 * portion is documented as ignored, always midnight UTC); map that
 * calendar date to LOCAL midnight, the local representation.                */
static gint64
due_from_rfc3339(const gchar *s)
{
    gint y = 0, m = 0, d = 0;        /* the date portion                    */
    if (s == NULL || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3)
        return 0;
    GDateTime *dt = g_date_time_new_local(y, m, d, 0, 0, 0);
    if (dt == NULL)
        return 0;
    gint64 u = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return u;
}

/* due_to_rfc3339() — local midnight unix → "YYYY-MM-DDT00:00:00.000Z"
 * (the calendar date in LOCAL time — matching due_from_rfc3339).
 * Returns NULL for due == 0 (caller emits JSON null to clear the date).     */
static gchar *
due_to_rfc3339(gint64 due)
{
    if (due == 0)
        return NULL;
    GDateTime *dt = g_date_time_new_from_unix_local(due);
    gchar *s = g_strdup_printf("%04d-%02d-%02dT00:00:00.000Z",
                               g_date_time_get_year(dt),
                               g_date_time_get_month(dt),
                               g_date_time_get_day_of_month(dt));
    g_date_time_unref(dt);
    return s;
}

/* ---------------------------------------------------------------------------
 * api_call() — one authorized API request; parses the JSON reply.
 *   body_out — optionally receives the parsed reply (caller frees); pass
 *              NULL when only success/failure matters (DELETE).
 * Returns TRUE on 2xx.  On failure *err is set (g_free).
 * ------------------------------------------------------------------------- */
static gboolean
api_call(const gchar *method, const gchar *url, const gchar *token,
         const gchar *body, BtJson **body_out, gchar **err)
{
    if (body_out != NULL)
        *body_out = NULL;
    glong status = 0;
    gchar *terr = NULL;              /* transport error                     */
    gchar *resp = bt_http_request(method, url, token, "application/json",
                                  body, &status, &terr);
    if (resp == NULL) {
        *err = g_strdup_printf("%s %s: %s", method, url,
                               terr != NULL ? terr : "network failure");
        g_free(terr);
        return FALSE;
    }
    g_free(terr);
    if (status < 200 || status > 299) {
        /* Try to surface Google's error message.                            */
        BtJson *root = bt_json_parse(resp, -1);
        const gchar *msg = bt_json_str(bt_json_get(root, "error"),
                                       "message");
        *err = g_strdup_printf("%s failed (HTTP %ld)%s%s", method, status,
                               msg != NULL ? ": " : "",
                               msg != NULL ? msg : "");
        bt_json_free(root);
        g_free(resp);
        return FALSE;
    }
    if (body_out != NULL && *resp != '\0')
        *body_out = bt_json_parse(resp, -1);
    g_free(resp);
    return TRUE;
}

/* api_call_notfound_ok() — DELETE variant where a 404 counts as success
 * (the row is gone either way — e.g. deleted from the Google side too).     */
static gboolean
api_call_delete(const gchar *url, const gchar *token, gchar **err)
{
    gchar *derr = NULL;
    if (api_call("DELETE", url, token, NULL, NULL, &derr))
        return TRUE;
    gboolean gone = strstr(derr, "HTTP 404") != NULL ||
                    strstr(derr, "HTTP 410") != NULL;
    if (gone) {
        g_free(derr);
        return TRUE;
    }
    *err = derr;
    return FALSE;
}

/* ---------------------------------------------------------------------------
 * fetch_remote_lists() — GET all tasklists (paginated).  NULL + *err on
 * failure.
 * ------------------------------------------------------------------------- */
static GPtrArray *
fetch_remote_lists(const gchar *token, gchar **err)
{
    GPtrArray *out = g_ptr_array_new();
    gchar *page = NULL;              /* nextPageToken                       */
    do {
        gchar *page_esc = page != NULL
            ? g_uri_escape_string(page, NULL, FALSE) : NULL;
        gchar *url = g_strdup_printf(
            BT_TASKS_API "/users/@me/lists?maxResults=100%s%s",
            page_esc != NULL ? "&pageToken=" : "",
            page_esc != NULL ? page_esc : "");
        g_free(page_esc);
        g_free(page);
        page = NULL;

        BtJson *root = NULL;
        gboolean ok = api_call("GET", url, token, NULL, &root, err);
        g_free(url);
        if (!ok) {
            for (guint i = 0; i < out->len; i++)
                remote_list_free(g_ptr_array_index(out, i));
            g_ptr_array_free(out, TRUE);
            return NULL;
        }
        BtJson *items = bt_json_get(root, "items");
        for (guint i = 0; i < bt_json_len(items); i++) {
            BtJson *it = bt_json_at(items, i);
            if (bt_json_str(it, "id") == NULL)
                continue;
            RemoteList *r = g_new0(RemoteList, 1);
            r->gid     = g_strdup(bt_json_str(it, "id"));
            r->title   = g_strdup(bt_json_str(it, "title") != NULL
                                  ? bt_json_str(it, "title") : "");
            r->updated = rfc3339_to_unix(bt_json_str(it, "updated"));
            g_ptr_array_add(out, r);
        }
        page = g_strdup(bt_json_str(root, "nextPageToken"));
        bt_json_free(root);
    } while (page != NULL);
    return out;
}

/* ---------------------------------------------------------------------------
 * fetch_remote_tasks() — GET all tasks of one tasklist, including
 * completed, hidden and (tombstoned) deleted ones.  NULL + *err on
 * failure.
 * ------------------------------------------------------------------------- */
static GPtrArray *
fetch_remote_tasks(const gchar *token, const gchar *list_gid, gchar **err)
{
    GPtrArray *out = g_ptr_array_new();
    gchar *page = NULL;              /* nextPageToken                       */
    do {
        gchar *page_esc = page != NULL
            ? g_uri_escape_string(page, NULL, FALSE) : NULL;
        gchar *gid_esc = g_uri_escape_string(list_gid, NULL, FALSE);
        gchar *url = g_strdup_printf(
            BT_TASKS_API "/lists/%s/tasks?maxResults=100"
            "&showCompleted=true&showHidden=true&showDeleted=true%s%s",
            gid_esc,
            page_esc != NULL ? "&pageToken=" : "",
            page_esc != NULL ? page_esc : "");
        g_free(gid_esc);
        g_free(page_esc);
        g_free(page);
        page = NULL;

        BtJson *root = NULL;
        gboolean ok = api_call("GET", url, token, NULL, &root, err);
        g_free(url);
        if (!ok) {
            for (guint i = 0; i < out->len; i++)
                remote_task_free(g_ptr_array_index(out, i));
            g_ptr_array_free(out, TRUE);
            return NULL;
        }
        BtJson *items = bt_json_get(root, "items");
        for (guint i = 0; i < bt_json_len(items); i++) {
            BtJson *it = bt_json_at(items, i);
            if (bt_json_str(it, "id") == NULL)
                continue;
            RemoteTask *r = g_new0(RemoteTask, 1);
            r->gid        = g_strdup(bt_json_str(it, "id"));
            r->title      = g_strdup(bt_json_str(it, "title") != NULL
                                     ? bt_json_str(it, "title") : "");
            r->notes      = g_strdup(bt_json_str(it, "notes") != NULL
                                     ? bt_json_str(it, "notes") : "");
            r->parent_gid = g_strdup(bt_json_str(it, "parent"));
            r->due        = due_from_rfc3339(bt_json_str(it, "due"));
            r->updated    = rfc3339_to_unix(bt_json_str(it, "updated"));
            r->done       = g_strcmp0(bt_json_str(it, "status"),
                                      "completed") == 0;
            r->deleted    = bt_json_bool(it, "deleted", FALSE);
            g_ptr_array_add(out, r);
        }
        page = g_strdup(bt_json_str(root, "nextPageToken"));
        bt_json_free(root);
    } while (page != NULL);
    return out;
}

/* ---------------------------------------------------------------------------
 * task_body() — the JSON body pushing a local task's synced fields.
 * `due: null` explicitly clears a remote date on PATCH.
 * ------------------------------------------------------------------------- */
static gchar *
task_body(const BtTask *t)
{
    GString *s = g_string_new("{\"title\": ");
    bt_json_escape(s, t->title);
    g_string_append(s, ", \"notes\": ");
    bt_json_escape(s, t->notes);
    g_string_append(s, ", \"status\": ");
    bt_json_escape(s, t->done ? "completed" : "needsAction");
    gchar *due = due_to_rfc3339(t->due);
    g_string_append(s, ", \"due\": ");
    bt_json_escape(s, due);         /* NULL → the literal `null`            */
    g_free(due);
    g_string_append(s, "}");
    return g_string_free(s, FALSE);
}

/* remote_updated_of() — pull the `updated` stamp out of a create/patch
 * reply so the local row can be stamped clean.  Falls back to `fallback`.   */
static gint64
remote_updated_of(BtJson *reply, gint64 fallback)
{
    gint64 u = rfc3339_to_unix(bt_json_str(reply, "updated"));
    return u != 0 ? u : fallback;
}

/* ---------------------------------------------------------------------------
 * sync_lists() — reconcile tasklists.  Fills `pairs` with
 * (local id, gtasks id gchar*) tuples for the task pass — ownership of
 * the gid strings moves to the caller.  FALSE + *err on failure.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  local_id;
    gchar  *gid;
} ListPair;

static gboolean
sync_lists(BtDatabase *db, const gchar *token, gint64 last_sync,
           GArray *pairs, SyncStats *stats, gchar **err)
{
    GPtrArray *remote = fetch_remote_lists(token, err);
    if (remote == NULL)
        return FALSE;
    GPtrArray *local = bt_db_lists(db, TRUE);
    gboolean ok = TRUE;

    for (guint i = 0; i < local->len && ok; i++) {
        BtList *l = g_ptr_array_index(local, i);

        /* Find the remote row this local one is bound to.                   */
        RemoteList *match = NULL;
        if (l->gtasks_id != NULL) {
            for (guint j = 0; j < remote->len; j++) {
                RemoteList *r = g_ptr_array_index(remote, j);
                if (strcmp(r->gid, l->gtasks_id) == 0) {
                    match = r;
                    break;
                }
            }
        } else if (!l->deleted) {
            /* First-sync dedup: adopt an unmatched remote list with the
             * same name instead of creating a duplicate.                    */
            for (guint j = 0; j < remote->len; j++) {
                RemoteList *r = g_ptr_array_index(remote, j);
                if (!r->matched && strcmp(r->title, l->name) == 0) {
                    match = r;
                    bt_db_list_set_gtasks_id(db, l->id, r->gid);
                    g_free(l->gtasks_id);
                    l->gtasks_id = g_strdup(r->gid);
                    break;
                }
            }
        }
        if (match != NULL)
            match->matched = TRUE;

        if (l->deleted) {
            /* Local tombstone: propagate, then purge.                       */
            if (l->gtasks_id != NULL) {
                gchar *gid_esc = g_uri_escape_string(l->gtasks_id, NULL,
                                                     FALSE);
                gchar *url = g_strdup_printf(
                    BT_TASKS_API "/users/@me/lists/%s", gid_esc);
                ok = api_call_delete(url, token, err);
                g_free(url);
                g_free(gid_esc);
            }
            if (ok) {
                bt_db_list_purge(db, l->id);
                stats->deleted++;
            }
            continue;
        }

        if (l->gtasks_id == NULL) {
            /* Local new: create remotely, adopt the id.                     */
            GString *body = g_string_new("{\"title\": ");
            bt_json_escape(body, l->name);
            g_string_append(body, "}");
            BtJson *reply = NULL;
            ok = api_call("POST", BT_TASKS_API "/users/@me/lists", token,
                          body->str, &reply, err);
            if (ok && bt_json_str(reply, "id") != NULL) {
                bt_db_list_set_gtasks_id(db, l->id,
                                         bt_json_str(reply, "id"));
                bt_db_list_apply_remote(db, l->id, l->name,
                    remote_updated_of(reply, l->updated_at));
                g_free(l->gtasks_id);
                l->gtasks_id = g_strdup(bt_json_str(reply, "id"));
                stats->pushed++;
            }
            bt_json_free(reply);
            g_string_free(body, TRUE);
        } else if (match == NULL) {
            /* Bound remote list is gone: deletion wins locally too.         */
            bt_db_list_purge(db, l->id);
            stats->deleted++;
            continue;
        } else if (strcmp(match->title, l->name) != 0) {
            /* Both exist, names differ: newer side wins.                    */
            gboolean local_dirty = l->updated_at > last_sync;
            if (local_dirty && l->updated_at >= match->updated) {
                GString *body = g_string_new("{\"title\": ");
                bt_json_escape(body, l->name);
                g_string_append(body, "}");
                gchar *gid_esc = g_uri_escape_string(l->gtasks_id, NULL,
                                                     FALSE);
                gchar *url = g_strdup_printf(
                    BT_TASKS_API "/users/@me/lists/%s", gid_esc);
                BtJson *reply = NULL;
                ok = api_call("PATCH", url, token, body->str, &reply, err);
                if (ok)
                    stats->pushed++;
                bt_json_free(reply);
                g_free(url);
                g_free(gid_esc);
                g_string_free(body, TRUE);
            } else {
                bt_db_list_apply_remote(db, l->id, match->title,
                                        match->updated);
                stats->pulled++;
            }
        }

        if (ok && l->gtasks_id != NULL) {
            ListPair p = { l->id, g_strdup(l->gtasks_id) };
            g_array_append_val(pairs, p);
        }
    }

    /* Remote lists nobody local claimed: new on the Google side.            */
    for (guint j = 0; j < remote->len && ok; j++) {
        RemoteList *r = g_ptr_array_index(remote, j);
        if (r->matched)
            continue;
        gint64 id = bt_db_list_create(db, r->title, "");
        if (id != 0) {
            bt_db_list_set_gtasks_id(db, id, r->gid);
            bt_db_list_apply_remote(db, id, r->title, r->updated);
            ListPair p = { id, g_strdup(r->gid) };
            g_array_append_val(pairs, p);
            stats->pulled++;
        }
    }

    for (guint i = 0; i < remote->len; i++)
        remote_list_free(g_ptr_array_index(remote, i));
    g_ptr_array_free(remote, TRUE);
    bt_ptr_array_free_lists(local);
    return ok;
}

/* ---------------------------------------------------------------------------
 * push_task_create() — POST one local task to Google (parent_gid NULL for
 * top-level) and adopt the returned id/stamp.  FALSE + *err on failure.
 * ------------------------------------------------------------------------- */
static gboolean
push_task_create(BtDatabase *db, const gchar *token, const gchar *list_gid,
                 BtTask *t, const gchar *parent_gid, SyncStats *stats,
                 gchar **err)
{
    gchar *body = task_body(t);
    gchar *gid_esc = g_uri_escape_string(list_gid, NULL, FALSE);
    gchar *url;
    if (parent_gid != NULL) {
        gchar *parent_esc = g_uri_escape_string(parent_gid, NULL, FALSE);
        url = g_strdup_printf(BT_TASKS_API "/lists/%s/tasks?parent=%s",
                              gid_esc, parent_esc);
        g_free(parent_esc);
    } else {
        url = g_strdup_printf(BT_TASKS_API "/lists/%s/tasks", gid_esc);
    }
    BtJson *reply = NULL;
    gboolean ok = api_call("POST", url, token, body, &reply, err);
    if (ok && bt_json_str(reply, "id") != NULL) {
        bt_db_task_set_gtasks_id(db, t->id, bt_json_str(reply, "id"));
        g_free(t->gtasks_id);
        t->gtasks_id = g_strdup(bt_json_str(reply, "id"));
        /* Stamp clean with the remote's updated time.                       */
        BtTask clean = *t;
        clean.updated_at = remote_updated_of(reply, t->updated_at);
        bt_db_task_apply_remote(db, &clean);
        stats->pushed++;
    }
    bt_json_free(reply);
    g_free(url);
    g_free(gid_esc);
    g_free(body);
    return ok;
}

/* ---------------------------------------------------------------------------
 * sync_tasks_for_list() — reconcile the tasks of one bound list pair.
 * ------------------------------------------------------------------------- */
static gboolean
sync_tasks_for_list(BtDatabase *db, const gchar *token, gint64 list_id,
                    const gchar *list_gid, gint64 last_sync,
                    SyncStats *stats, gchar **err)
{
    GPtrArray *remote = fetch_remote_tasks(token, list_gid, err);
    if (remote == NULL)
        return FALSE;

    /* Local rows of this list, tombstones included; parents before
     * subtasks (bt_db_tasks_all orders by "parent_id IS NOT NULL"), so a
     * new parent is pushed — and owns a gtasks_id — before its children.    */
    GPtrArray *all = bt_db_tasks_all(db);
    GPtrArray *local = g_ptr_array_new();
    for (guint i = 0; i < all->len; i++) {
        BtTask *t = g_ptr_array_index(all, i);
        if (t->list_id == list_id)
            g_ptr_array_add(local, t);
    }

    /* gid → RemoteTask and gid → local BtTask maps for the match passes.    */
    GHashTable *by_gid = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < remote->len; i++) {
        RemoteTask *r = g_ptr_array_index(remote, i);
        g_hash_table_insert(by_gid, r->gid, r);
    }
    GHashTable *local_by_gid = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < local->len; i++) {
        BtTask *t = g_ptr_array_index(local, i);
        if (t->gtasks_id != NULL)
            g_hash_table_insert(local_by_gid, t->gtasks_id, t);
    }

    gboolean ok = TRUE;

    for (guint i = 0; i < local->len && ok; i++) {
        BtTask *t = g_ptr_array_index(local, i);

        RemoteTask *match = t->gtasks_id != NULL
            ? g_hash_table_lookup(by_gid, t->gtasks_id) : NULL;

        /* First-sync dedup: adopt an unmatched live remote task with the
         * same title (top-level against top-level only — subtask titles
         * repeat too easily across parents to guess).                       */
        if (match == NULL && t->gtasks_id == NULL && !t->deleted &&
            t->parent_id == 0) {
            for (guint j = 0; j < remote->len; j++) {
                RemoteTask *r = g_ptr_array_index(remote, j);
                if (!r->matched && !r->deleted && r->parent_gid == NULL &&
                    strcmp(r->title, t->title) == 0) {
                    match = r;
                    bt_db_task_set_gtasks_id(db, t->id, r->gid);
                    g_free(t->gtasks_id);
                    t->gtasks_id = g_strdup(r->gid);
                    g_hash_table_insert(local_by_gid, t->gtasks_id, t);
                    break;
                }
            }
        }
        if (match != NULL)
            match->matched = TRUE;

        if (t->deleted) {
            /* Local tombstone: propagate, then purge.                       */
            if (t->gtasks_id != NULL &&
                (match == NULL || !match->deleted)) {
                gchar *gid_esc  = g_uri_escape_string(list_gid, NULL,
                                                      FALSE);
                gchar *task_esc = g_uri_escape_string(t->gtasks_id, NULL,
                                                      FALSE);
                gchar *url = g_strdup_printf(
                    BT_TASKS_API "/lists/%s/tasks/%s", gid_esc, task_esc);
                ok = api_call_delete(url, token, err);
                g_free(url);
                g_free(gid_esc);
                g_free(task_esc);
            }
            if (ok) {
                bt_db_task_purge(db, t->id);
                stats->deleted++;
            }
            continue;
        }

        if (t->gtasks_id == NULL) {
            /* Local new: parents already carry gtasks_id at this point
             * (ordering above), so a subtask can resolve its parent.        */
            const gchar *parent_gid = NULL;
            if (t->parent_id != 0) {
                BtTask *p = bt_db_task_get(db, t->parent_id);
                parent_gid = p != NULL ? p->gtasks_id : NULL;
                /* p leaks its strings into parent_gid's lifetime — keep
                 * it alive until after the push.                            */
                ok = push_task_create(db, token, list_gid, t,
                                      parent_gid, stats, err);
                bt_task_free(p);
            } else {
                ok = push_task_create(db, token, list_gid, t, NULL,
                                      stats, err);
            }
            if (t->gtasks_id != NULL)
                g_hash_table_insert(local_by_gid, t->gtasks_id, t);
            continue;
        }

        if (match == NULL || match->deleted) {
            /* Deleted on the Google side: deletion wins.                    */
            bt_db_task_purge(db, t->id);
            stats->deleted++;
            continue;
        }

        /* Both sides live: compare the synced fields.                       */
        gboolean differs = strcmp(t->title, match->title) != 0 ||
                           strcmp(t->notes, match->notes) != 0 ||
                           t->due != match->due ||
                           t->done != match->done;
        if (!differs)
            continue;
        gboolean local_dirty = t->updated_at > last_sync;
        if (local_dirty && t->updated_at >= match->updated) {
            gchar *body = task_body(t);
            gchar *gid_esc  = g_uri_escape_string(list_gid, NULL, FALSE);
            gchar *task_esc = g_uri_escape_string(t->gtasks_id, NULL,
                                                  FALSE);
            gchar *url = g_strdup_printf(
                BT_TASKS_API "/lists/%s/tasks/%s", gid_esc, task_esc);
            BtJson *reply = NULL;
            ok = api_call("PATCH", url, token, body, &reply, err);
            if (ok)
                stats->pushed++;
            bt_json_free(reply);
            g_free(url);
            g_free(gid_esc);
            g_free(task_esc);
            g_free(body);
        } else {
            BtTask apply = *t;       /* shallow copy is fine here           */
            apply.title      = match->title;
            apply.notes      = match->notes;
            apply.due        = match->due;
            apply.done       = match->done;
            apply.updated_at = match->updated;
            bt_db_task_apply_remote(db, &apply);
            stats->pulled++;
        }
    }

    /* Remote tasks nobody local claimed: new on the Google side.  Two
     * passes — parents first so subtasks can resolve their local parent.    */
    for (gint pass = 0; pass < 2 && ok; pass++) {
        for (guint j = 0; j < remote->len; j++) {
            RemoteTask *r = g_ptr_array_index(remote, j);
            gboolean is_child = r->parent_gid != NULL;
            if (r->matched || r->deleted || is_child != (pass == 1))
                continue;
            gint64 parent_id = 0;    /* local id of the parent task         */
            if (is_child) {
                BtTask *p = g_hash_table_lookup(local_by_gid,
                                                r->parent_gid);
                if (p == NULL || p->parent_id != 0)
                    continue;        /* orphan / over-deep: skip            */
                parent_id = p->id;
            }
            gint64 id = bt_db_task_create(db, list_id, parent_id,
                                          r->title);
            if (id == 0)
                continue;
            bt_db_task_set_gtasks_id(db, id, r->gid);
            BtTask nt = { 0 };       /* the fields apply_remote writes      */
            nt.id         = id;
            nt.title      = r->title;
            nt.notes      = r->notes;
            nt.due        = r->due;
            nt.done       = r->done;
            nt.updated_at = r->updated;
            bt_db_task_apply_remote(db, &nt);
            r->matched = TRUE;
            stats->pulled++;
            if (!is_child) {
                /* Make the new row findable for pass 2's children.          */
                BtTask *row = bt_db_task_get(db, id);
                if (row != NULL) {
                    g_hash_table_insert(local_by_gid, row->gtasks_id, row);
                    g_ptr_array_add(all, row);     /* owned by `all`        */
                }
            }
        }
    }

    g_hash_table_destroy(by_gid);
    g_hash_table_destroy(local_by_gid);
    g_ptr_array_free(local, TRUE);   /* borrowed pointers only              */
    bt_ptr_array_free_tasks(all);
    for (guint i = 0; i < remote->len; i++)
        remote_task_free(g_ptr_array_index(remote, i));
    g_ptr_array_free(remote, TRUE);
    return ok;
}

/* ===========================================================================
 * The worker thread and its main-thread marshalling.
 * =========================================================================== */

typedef struct {
    BtApp        *app;               /* lives for the program's lifetime    */
    gchar        *db_path;
    BtSyncDoneFn  done;
    gpointer      user_data;
    gboolean      ok;                /* out: result                         */
    gchar        *message;           /* out: summary or error               */
} SyncJob;

/* status_idle() — post a status-bar message from the worker.                */
typedef struct {
    BtApp *app;
    gchar *msg;
} StatusPost;

static gboolean
status_idle(gpointer data)
{
    StatusPost *sp = data;
    bt_app_status(sp->app, "%s", sp->msg);
    g_free(sp->msg);
    g_free(sp);
    return G_SOURCE_REMOVE;
}

static void
post_status(BtApp *app, const gchar *msg)
{
    StatusPost *sp = g_new0(StatusPost, 1);
    sp->app = app;
    sp->msg = g_strdup(msg);
    g_idle_add(status_idle, sp);
}

/* sync_apply() — main-thread completion: clear the running flag, refresh
 * the library, report.                                                      */
static gboolean
sync_apply(gpointer data)
{
    SyncJob *job = data;
    job->app->sync_running = FALSE;
    if (job->app->notify_changed != NULL)
        job->app->notify_changed(job->app);
    bt_app_status(job->app, "%s", job->message);
    if (job->done != NULL)
        job->done(job->app, job->ok, job->message, job->user_data);
    g_free(job->db_path);
    g_free(job->message);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* sync_thread() — the whole sync pass (worker thread, own connection).      */
static gpointer
sync_thread(gpointer data)
{
    SyncJob *job = data;
    SyncStats stats = { 0, 0, 0 };
    gchar *err = NULL;

    gchar *token = bt_oauth_access_token(&err);
    if (token == NULL) {
        job->ok = FALSE;
        job->message = g_strdup_printf("Sync failed: %s",
                                       err != NULL ? err : "no token");
        g_free(err);
        g_idle_add(sync_apply, job);
        return NULL;
    }

    GError *gerr = NULL;
    BtDatabase *db = bt_db_open(job->db_path, &gerr);
    if (db == NULL) {
        job->ok = FALSE;
        job->message = g_strdup_printf("Sync failed: %s",
                                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_free(token);
        g_idle_add(sync_apply, job);
        return NULL;
    }

    /* last_sync gates the "locally dirty" test; the new value is the
     * time this pass STARTED, so mid-sync edits stay dirty for the next.    */
    gchar *ls = bt_db_state_get(db, "last_sync");
    gint64 last_sync = ls != NULL ? g_ascii_strtoll(ls, NULL, 10) : 0;
    g_free(ls);
    gint64 started = g_get_real_time() / G_USEC_PER_SEC;

    post_status(job->app, "Syncing with Google Tasks\xe2\x80\xa6");

    GArray *pairs = g_array_new(FALSE, FALSE, sizeof(ListPair));
    gboolean ok = sync_lists(db, token, last_sync, pairs, &stats, &err);
    for (guint i = 0; i < pairs->len && ok; i++) {
        ListPair *p = &g_array_index(pairs, ListPair, i);
        ok = sync_tasks_for_list(db, token, p->local_id, p->gid,
                                 last_sync, &stats, &err);
    }
    for (guint i = 0; i < pairs->len; i++)
        g_free(g_array_index(pairs, ListPair, i).gid);
    g_array_free(pairs, TRUE);

    if (ok) {
        gchar *stamp = g_strdup_printf("%lld", (long long)started);
        bt_db_state_set(db, "last_sync", stamp);
        g_free(stamp);
        job->ok = TRUE;
        job->message = g_strdup_printf(
            "Sync done: %d pushed, %d pulled, %d deleted",
            stats.pushed, stats.pulled, stats.deleted);
    } else {
        job->ok = FALSE;
        job->message = g_strdup_printf("Sync failed: %s",
                                       err != NULL ? err : "unknown error");
    }
    g_free(err);
    bt_db_close(db);
    g_free(token);
    g_idle_add(sync_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * bt_sync_start() — kick off one pass (see gtasks.h).
 * ------------------------------------------------------------------------- */
void
bt_sync_start(BtApp *app, const gchar *db_path,
              BtSyncDoneFn done, gpointer user_data)
{
    if (app->sync_running) {
        bt_app_status(app, "Sync already running");
        return;
    }
    if (!bt_oauth_authenticated()) {
        bt_app_status(app, "Not signed in to Google \xe2\x80\x94 use the "
                      "Sync button or File \xe2\x86\x92 Settings\xe2\x80\xa6");
        if (done != NULL)
            done(app, FALSE, "not signed in", user_data);
        return;
    }
    app->sync_running = TRUE;
    SyncJob *job = g_new0(SyncJob, 1);
    job->app       = app;
    job->db_path   = g_strdup(db_path);
    job->done      = done;
    job->user_data = user_data;
    GThread *th = g_thread_new("bt-sync", sync_thread, job);
    g_thread_unref(th);
}

/* ---------------------------------------------------------------------------
 * bt_sync_auto_start() — periodic auto-sync (see gtasks.h).
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp *app;
    gchar *db_path;
} AutoSync;

static gboolean
auto_sync_tick(gpointer data)
{
    AutoSync *as = data;
    if (bt_oauth_authenticated() && !as->app->sync_running)
        bt_sync_start(as->app, as->db_path, NULL, NULL);
    return G_SOURCE_CONTINUE;
}

void
bt_sync_auto_start(BtApp *app, const gchar *db_path)
{
    if (app->sync_timer != 0) {
        g_source_remove(app->sync_timer);
        app->sync_timer = 0;
    }
    gchar *v = bt_app_config_get("sync_interval_min");
    gint minutes = v != NULL ? atoi(v) : 5;
    g_free(v);
    if (minutes <= 0)
        return;

    /* The timer's payload lives as long as the app does.                    */
    AutoSync *as = g_new0(AutoSync, 1);
    as->app = app;
    as->db_path = g_strdup(db_path);
    app->sync_timer = g_timeout_add_seconds((guint)(minutes * 60),
                                            auto_sync_tick, as);
    if (bt_oauth_authenticated())
        bt_sync_start(app, db_path, NULL, NULL);
}
