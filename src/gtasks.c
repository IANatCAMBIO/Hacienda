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

static void post_status(BtApp *app, const gchar *msg);

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
    gint64    completed;             /* unix completion time; 0 = none      */
    gchar    *etag;                  /* concurrency tag                     */
    gchar    *web_link;              /* Google Tasks UI deep link           */
    gchar    *glinks;                /* links[] as raw JSON, or NULL        */
    gchar    *assigned;              /* assignmentInfo as raw JSON, / NULL  */
    gboolean  done;
    gboolean  deleted;
    gboolean  hidden;                /* completed + cleared on Google       */
    gboolean  matched;
} RemoteTask;

/* remote_list_free() / remote_task_free() — free one snapshot row
 * (gpointer-typed so fetch_paginated can free either kind).                 */
static void
remote_list_free(gpointer data)
{
    RemoteList *r = data;
    g_free(r->gid);
    g_free(r->title);
    g_free(r);
}

static void
remote_task_free(gpointer data)
{
    RemoteTask *r = data;
    g_free(r->gid);
    g_free(r->title);
    g_free(r->notes);
    g_free(r->parent_gid);
    g_free(r->etag);
    g_free(r->web_link);
    g_free(r->glinks);
    g_free(r->assigned);
    g_free(r);
}

/* Counters for the end-of-sync status line.                                 */
typedef struct {
    gint pushed;                     /* local → Google writes               */
    gint pulled;                     /* Google → local writes               */
    gint deleted;                    /* rows removed on either side         */
} SyncStats;

/* escaped() — shorthand for the URI-escaping every URL builder needs.       */
static gchar *
escaped(const gchar *s)
{
    return g_uri_escape_string(s, NULL, FALSE);
}

/* tasklist_url() — "…/users/@me/lists[/<gid>]".                             */
static gchar *
tasklist_url(const gchar *gid)
{
    if (gid == NULL)
        return g_strdup(BT_TASKS_API "/users/@me/lists");
    gchar *gid_esc = escaped(gid);
    gchar *url = g_strdup_printf(BT_TASKS_API "/users/@me/lists/%s",
                                 gid_esc);
    g_free(gid_esc);
    return url;
}

/* task_url() — "…/lists/<list>/tasks[/<task>]".                             */
static gchar *
task_url(const gchar *list_gid, const gchar *task_gid)
{
    gchar *list_esc = escaped(list_gid);
    gchar *url;
    if (task_gid == NULL) {
        url = g_strdup_printf(BT_TASKS_API "/lists/%s/tasks", list_esc);
    } else {
        gchar *task_esc = escaped(task_gid);
        url = g_strdup_printf(BT_TASKS_API "/lists/%s/tasks/%s",
                              list_esc, task_esc);
        g_free(task_esc);
    }
    g_free(list_esc);
    return url;
}

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
    return bt_due_from_ymd(y, m, d);
}

/* due_to_rfc3339() — local midnight unix → "YYYY-MM-DDT00:00:00.000Z"
 * (the calendar date in LOCAL time — matching due_from_rfc3339).
 * Returns NULL for due == 0 (caller emits JSON null to clear the date).     */
static gchar *
due_to_rfc3339(gint64 due)
{
    if (due == 0)
        return NULL;
    gchar *date = bt_due_format_iso(due);
    gchar *s = g_strdup_printf("%sT00:00:00.000Z", date);
    g_free(date);
    return s;
}

/* unix_to_rfc3339() — full UTC timestamp (unlike `due`, the `completed`
 * and `updatedMin` fields carry real times).  NULL for 0.                   */
static gchar *
unix_to_rfc3339(gint64 unix_time)
{
    if (unix_time == 0)
        return NULL;
    GDateTime *dt = g_date_time_new_from_unix_utc(unix_time);
    gchar *s = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%S.000Z");
    g_date_time_unref(dt);
    return s;
}

/* json_subtree_dup() — one member's raw JSON text (bt_json_write), or
 * NULL when absent.                                                          */
static gchar *
json_subtree_dup(BtJson *obj, const gchar *key)
{
    BtJson *v = bt_json_get(obj, key);
    if (v == NULL)
        return NULL;
    GString *s = g_string_new(NULL);
    bt_json_write(s, v);
    return g_string_free(s, FALSE);
}

/* ---------------------------------------------------------------------------
 * api_call() — one authorized API request; parses the JSON reply.
 *   if_match — an etag for optimistic concurrency ("If-Match"), or NULL.
 *   body_out — optionally receives the parsed reply (caller frees); pass
 *              NULL when only success/failure matters (DELETE).
 * Returns TRUE on 2xx.  On failure *err is set (g_free).
 * ------------------------------------------------------------------------- */
static gboolean
api_call(const gchar *method, const gchar *url, const gchar *token,
         const gchar *if_match, const gchar *body, BtJson **body_out,
         gchar **err)
{
    if (body_out != NULL)
        *body_out = NULL;
    gchar *match_hdr = if_match != NULL
        ? g_strdup_printf("If-Match: %s", if_match) : NULL;
    glong status = 0;
    gchar *terr = NULL;              /* transport error                     */
    gchar *resp = bt_http_request(method, url, token, "application/json",
                                  match_hdr, body, &status, &terr);
    g_free(match_hdr);
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

/* api_call_delete() — DELETE variant where a 404 or 410 counts as
 * success (the row is gone either way — e.g. deleted from the Google
 * side too).                                                                */
static gboolean
api_call_delete(const gchar *url, const gchar *token, gchar **err)
{
    gchar *derr = NULL;
    if (api_call("DELETE", url, token, NULL, NULL, NULL, &derr))
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

/* str_dup_or_empty() — dup a string member, "" when absent.                 */
static gchar *
str_dup_or_empty(BtJson *obj, const gchar *key)
{
    const gchar *v = bt_json_str(obj, key);
    return g_strdup(v != NULL ? v : "");
}

/* ---------------------------------------------------------------------------
 * fetch_paginated() — run one paginated GET: `base_url` must already
 * carry a query string (a pageToken is appended with '&'); every items[]
 * entry that has an "id" goes through `add_item`, which appends a
 * snapshot row to `out`.  On failure the collected rows are freed with
 * `free_row` and NULL is returned (+ *err).
 * ------------------------------------------------------------------------- */
static GPtrArray *
fetch_paginated(const gchar *base_url, const gchar *token,
                void (*add_item)(BtJson *it, GPtrArray *out),
                void (*free_row)(gpointer row), gchar **err)
{
    GPtrArray *out = g_ptr_array_new();
    gchar *page = NULL;              /* nextPageToken                       */
    do {
        gchar *page_esc = page != NULL ? escaped(page) : NULL;
        gchar *url = g_strdup_printf("%s%s%s", base_url,
            page_esc != NULL ? "&pageToken=" : "",
            page_esc != NULL ? page_esc : "");
        g_free(page_esc);
        g_free(page);
        page = NULL;

        BtJson *root = NULL;
        gboolean ok = api_call("GET", url, token, NULL, NULL, &root, err);
        g_free(url);
        if (!ok) {
            for (guint i = 0; i < out->len; i++)
                free_row(g_ptr_array_index(out, i));
            g_ptr_array_free(out, TRUE);
            return NULL;
        }
        BtJson *items = bt_json_get(root, "items");
        for (guint i = 0; i < bt_json_len(items); i++) {
            BtJson *it = bt_json_at(items, i);
            if (bt_json_str(it, "id") != NULL)
                add_item(it, out);
        }
        page = g_strdup(bt_json_str(root, "nextPageToken"));
        bt_json_free(root);
    } while (page != NULL);
    return out;
}

/* add_remote_list() — one tasklist item → RemoteList snapshot row.           */
static void
add_remote_list(BtJson *it, GPtrArray *out)
{
    RemoteList *r = g_new0(RemoteList, 1);
    r->gid     = g_strdup(bt_json_str(it, "id"));
    r->title   = str_dup_or_empty(it, "title");
    r->updated = rfc3339_to_unix(bt_json_str(it, "updated"));
    g_ptr_array_add(out, r);
}

/* add_remote_task() — one task item → RemoteTask snapshot row.               */
static void
add_remote_task(BtJson *it, GPtrArray *out)
{
    RemoteTask *r = g_new0(RemoteTask, 1);
    r->gid        = g_strdup(bt_json_str(it, "id"));
    r->title      = str_dup_or_empty(it, "title");
    r->notes      = str_dup_or_empty(it, "notes");
    r->parent_gid = g_strdup(bt_json_str(it, "parent"));
    r->due        = due_from_rfc3339(bt_json_str(it, "due"));
    r->updated    = rfc3339_to_unix(bt_json_str(it, "updated"));
    r->completed  = rfc3339_to_unix(bt_json_str(it, "completed"));
    r->etag       = g_strdup(bt_json_str(it, "etag"));
    r->web_link   = g_strdup(bt_json_str(it, "webViewLink"));
    r->glinks     = json_subtree_dup(it, "links");
    r->assigned   = json_subtree_dup(it, "assignmentInfo");
    r->done       = g_strcmp0(bt_json_str(it, "status"),
                              "completed") == 0;
    r->deleted    = bt_json_bool(it, "deleted", FALSE);
    r->hidden     = bt_json_bool(it, "hidden", FALSE);
    g_ptr_array_add(out, r);
}

/* ---------------------------------------------------------------------------
 * fetch_remote_lists() — GET all tasklists (paginated).  NULL + *err on
 * failure.
 * ------------------------------------------------------------------------- */
static GPtrArray *
fetch_remote_lists(const gchar *token, gchar **err)
{
    return fetch_paginated(
        BT_TASKS_API "/users/@me/lists?maxResults=100",
        token, add_remote_list, remote_list_free, err);
}

/* ---------------------------------------------------------------------------
 * fetch_remote_tasks() — GET the tasks of one tasklist, including
 * completed, hidden and (tombstoned) deleted ones.  When `updated_min`
 * is non-zero only items CHANGED since then come back (incremental
 * sync) — an item missing from such a listing means "unchanged", never
 * "deleted" (deletions arrive as deleted:true items).  NULL + *err on
 * failure.
 * ------------------------------------------------------------------------- */
static GPtrArray *
fetch_remote_tasks(const gchar *token, const gchar *list_gid,
                   gint64 updated_min, gchar **err)
{
    gchar *min_param = g_strdup("");
    if (updated_min > 0) {
        gchar *stamp = unix_to_rfc3339(updated_min);
        gchar *stamp_esc = escaped(stamp);
        g_free(min_param);
        min_param = g_strdup_printf("&updatedMin=%s", stamp_esc);
        g_free(stamp_esc);
        g_free(stamp);
    }
    gchar *gid_esc = escaped(list_gid);
    gchar *base_url = g_strdup_printf(
        BT_TASKS_API "/lists/%s/tasks?maxResults=100"
        "&showCompleted=true&showHidden=true&showDeleted=true%s",
        gid_esc, min_param);
    g_free(gid_esc);
    g_free(min_param);
    GPtrArray *out = fetch_paginated(base_url, token, add_remote_task,
                                     remote_task_free, err);
    g_free(base_url);
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
    /* Preserve the real completion time across the sync (Google would
     * otherwise stamp "now" when status flips to completed).                */
    if (t->done && t->completed_at != 0) {
        gchar *completed = unix_to_rfc3339(t->completed_at);
        g_string_append(s, ", \"completed\": ");
        bt_json_escape(s, completed);
        g_free(completed);
    }
    g_string_append(s, "}");
    return g_string_free(s, FALSE);
}

/* list_body() — the JSON body for a tasklist create/rename.                 */
static gchar *
list_body(const gchar *name)
{
    GString *s = g_string_new("{\"title\": ");
    bt_json_escape(s, name);
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
 * fetch_default_list_gid() — GET the account's DEFAULT tasklist
 * (endpoint id "@default") and persist its id as
 * sync_state."default_list_gid".  The default list cannot be deleted
 * (tasklists.delete → 400 "Invalid Value" from any client), so the GUI
 * uses the stored id to refuse the deletion up front.  FALSE + *err on
 * failure.
 * ------------------------------------------------------------------------- */
static gboolean
fetch_default_list_gid(BtDatabase *db, const gchar *token, gchar **err)
{
    gchar *url = tasklist_url("@default");
    BtJson *reply = NULL;
    gboolean ok = api_call("GET", url, token, NULL, NULL, &reply, err);
    if (ok && bt_json_str(reply, "id") != NULL)
        bt_db_state_set(db, "default_list_gid",
                        bt_json_str(reply, "id"));
    bt_json_free(reply);
    g_free(url);
    return ok;
}

/* ---------------------------------------------------------------------------
 * sync_lists() — reconcile tasklists.  Fills `pairs` with
 * (local id, gtasks id gchar*) tuples for the task pass — ownership of
 * the gid strings moves to the caller.  `app` is only for post_status
 * (idle-marshalled; safe from this worker).  FALSE + *err on failure.
 * ------------------------------------------------------------------------- */
typedef struct {
    gint64  local_id;
    gchar  *gid;
} ListPair;

static gboolean
sync_lists(BtApp *app, BtDatabase *db, const gchar *token,
           gint64 last_sync, GArray *pairs, SyncStats *stats, gchar **err)
{
    if (!fetch_default_list_gid(db, token, err))
        return FALSE;
    gchar *default_gid = bt_db_state_get(db, "default_list_gid");

    GPtrArray *remote = fetch_remote_lists(token, err);
    if (remote == NULL) {
        g_free(default_gid);
        return FALSE;
    }
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
            /* Google's DEFAULT tasklist is undeletable (the GUI refuses
             * up front; this catches a tombstone from an older build):
             * RESTORE the list and its same-moment task tombstones —
             * remote is the source of truth and still has everything.       */
            if (l->gtasks_id != NULL && default_gid != NULL &&
                strcmp(l->gtasks_id, default_gid) == 0) {
                bt_db_list_restore(db, l->id);
                post_status(app, "Google's default list cannot be "
                            "deleted \xe2\x80\x94 restored");
                ListPair p = { l->id, g_strdup(l->gtasks_id) };
                g_array_append_val(pairs, p);
                continue;
            }
            /* Local tombstone: propagate, then purge.                       */
            if (l->gtasks_id != NULL) {
                gchar *url = tasklist_url(l->gtasks_id);
                ok = api_call_delete(url, token, err);
                g_free(url);
            }
            if (ok) {
                bt_db_list_purge(db, l->id);
                stats->deleted++;
            }
            continue;
        }

        if (l->gtasks_id == NULL || match == NULL) {
            /* Local new — or its bound remote list vanished without a
             * local tombstone.  NON-DESTRUCTIVE: absence never deletes;
             * the list exists here, so (re-)create it remotely and
             * adopt the new id.  On a re-create the list's tasks drop
             * their stale Google identities too, so the task pass
             * pushes every one of them as a new remote task.                */
            gboolean rebind = l->gtasks_id != NULL;
            gchar *body = list_body(l->name);
            BtJson *reply = NULL;
            gchar *url = tasklist_url(NULL);
            ok = api_call("POST", url, token, NULL, body, &reply, err);
            g_free(url);
            if (ok && bt_json_str(reply, "id") != NULL) {
                if (rebind)
                    bt_db_tasks_clear_gtasks_ids(db, l->id);
                bt_db_list_set_gtasks_id(db, l->id,
                                         bt_json_str(reply, "id"));
                bt_db_list_apply_remote(db, l->id, l->name,
                    remote_updated_of(reply, l->updated_at));
                g_free(l->gtasks_id);
                l->gtasks_id = g_strdup(bt_json_str(reply, "id"));
                stats->pushed++;
            }
            bt_json_free(reply);
            g_free(body);
        } else if (strcmp(match->title, l->name) != 0) {
            /* Both exist, names differ: newer side wins.                    */
            gboolean local_dirty = l->updated_at > last_sync;
            if (local_dirty && l->updated_at >= match->updated) {
                gchar *body = list_body(l->name);
                gchar *url = tasklist_url(l->gtasks_id);
                ok = api_call("PATCH", url, token, NULL, body, NULL, err);
                if (ok)
                    stats->pushed++;
                g_free(url);
                g_free(body);
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

    /* Google's undeletable DEFAULT list always wears a 🔴 indicator:
     * seeded only while the emoji is empty, so a user's later Edit List
     * choice sticks (clearing it brings the dot back next sync).             */
    if (ok && default_gid != NULL)
        bt_db_list_emoji_if_empty(db, default_gid, "\xf0\x9f\x94\xb4");

    for (guint i = 0; i < remote->len; i++)
        remote_list_free(g_ptr_array_index(remote, i));
    g_ptr_array_free(remote, TRUE);
    bt_ptr_array_free_lists(local);
    g_free(default_gid);
    return ok;
}

/* stamp_clean() — after a successful create/patch, write the local row
 * back clean: the reply's updated time, etag and webViewLink (keeping
 * the stored link when the reply omits it).  Shallow overlay — nothing
 * in *t is modified or freed.                                               */
static void
stamp_clean(BtDatabase *db, const BtTask *t, BtJson *reply,
            SyncStats *stats)
{
    BtTask clean = *t;
    clean.updated_at = remote_updated_of(reply, t->updated_at);
    clean.etag       = (gchar *)bt_json_str(reply, "etag");
    clean.web_link   = bt_json_str(reply, "webViewLink") != NULL
                       ? (gchar *)bt_json_str(reply, "webViewLink")
                       : t->web_link;
    bt_db_task_apply_remote(db, &clean);
    stats->pushed++;
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
    gchar *url = task_url(list_gid, NULL);
    if (parent_gid != NULL) {
        gchar *parent_esc = escaped(parent_gid);
        gchar *with_parent = g_strdup_printf("%s?parent=%s", url,
                                             parent_esc);
        g_free(parent_esc);
        g_free(url);
        url = with_parent;
    }
    BtJson *reply = NULL;
    gboolean ok = api_call("POST", url, token, NULL, body, &reply, err);
    if (ok && bt_json_str(reply, "id") != NULL) {
        bt_db_task_set_gtasks_id(db, t->id, bt_json_str(reply, "id"));
        g_free(t->gtasks_id);
        t->gtasks_id = g_strdup(bt_json_str(reply, "id"));
        stamp_clean(db, t, reply, stats);
    }
    bt_json_free(reply);
    g_free(url);
    g_free(body);
    return ok;
}

/* ---------------------------------------------------------------------------
 * push_task_patch() — PATCH one local task's synced fields, guarded by
 * the stored etag ("If-Match"): a 412 means the remote changed since we
 * last pulled it — the push is SKIPPED (not a sync failure) and the
 * next pull reconciles.  On success the reply's updated/etag stamp the
 * row clean.
 * ------------------------------------------------------------------------- */
static gboolean
push_task_patch(BtDatabase *db, const gchar *token, const gchar *list_gid,
                const BtTask *t, SyncStats *stats, gchar **err)
{
    gchar *body = task_body(t);
    gchar *url = task_url(list_gid, t->gtasks_id);
    BtJson *reply = NULL;
    gboolean ok = api_call("PATCH", url, token, t->etag, body,
                           &reply, err);
    if (ok) {
        stamp_clean(db, t, reply, stats);
    } else if (*err != NULL && strstr(*err, "HTTP 412") != NULL) {
        g_clear_pointer(err, g_free);  /* remote moved on: theirs wins      */
        ok = TRUE;
    }
    bt_json_free(reply);
    g_free(url);
    g_free(body);
    return ok;
}

/* push_as_new() — POST one local task as a NEW remote one: resolve the
 * parent's gid (parents are pushed first, so a subtask's parent already
 * carries one), create, and index the adopted gid so later children can
 * find it.                                                                  */
static gboolean
push_as_new(BtDatabase *db, const gchar *token, const gchar *list_gid,
            BtTask *t, GHashTable *local_by_gid, SyncStats *stats,
            gchar **err)
{
    BtTask *p = t->parent_id != 0 ? bt_db_task_get(db, t->parent_id)
                                  : NULL;
    gboolean ok = push_task_create(db, token, list_gid, t,
                                   p != NULL ? p->gtasks_id : NULL,
                                   stats, err);
    bt_task_free(p);                 /* owns parent_gid until after push    */
    if (t->gtasks_id != NULL)
        g_hash_table_insert(local_by_gid, t->gtasks_id, t);
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
    /* After the first sync, fetch INCREMENTALLY: only items changed
     * since the last pass (with a 5-minute overlap for clock skew —
     * re-applying an unchanged item is a no-op).  A partial listing
     * changes the deletion inference below: "not in the response" means
     * unchanged, not deleted.                                               */
    gboolean full_listing = last_sync == 0;
    GPtrArray *remote = fetch_remote_tasks(token, list_gid,
        full_listing ? 0 : MAX(last_sync - 300, 1), err);
    if (remote == NULL)
        return FALSE;

    /* This list's rows, tombstones included; parents before subtasks, so
     * a new parent is pushed — and owns a gtasks_id — before its
     * children.  (One per-list query, not an all-rows scan per list.)        */
    GPtrArray *local = bt_db_tasks_in_list_all(db, list_id);

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
         * repeat too easily across parents to guess).  Only meaningful
         * against a FULL listing.                                           */
        if (full_listing &&
            match == NULL && t->gtasks_id == NULL && !t->deleted &&
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
                gchar *url = task_url(list_gid, t->gtasks_id);
                ok = api_call_delete(url, token, err);
                g_free(url);
            }
            if (ok) {
                bt_db_task_purge(db, t->id);
                stats->deleted++;
            }
            continue;
        }

        if (t->gtasks_id == NULL) {
            /* Local new.                                                    */
            ok = push_as_new(db, token, list_gid, t, local_by_gid,
                             stats, err);
            continue;
        }

        if (match == NULL) {
            if (full_listing) {
                /* Gone from a FULL listing (deleted on Google without a
                 * local tombstone).  NON-DESTRUCTIVE: absence never
                 * deletes — the task exists here, so drop the stale
                 * Google identity and push it back as a NEW remote
                 * task.  Explicit deletes still propagate: a local
                 * tombstone DELETEs remotely (above) and a remote
                 * `deleted:true` purges locally (below).                    */
                g_hash_table_remove(local_by_gid, t->gtasks_id);
                bt_db_task_set_gtasks_id(db, t->id, NULL);
                g_clear_pointer(&t->gtasks_id, g_free);
                ok = push_as_new(db, token, list_gid, t, local_by_gid,
                                 stats, err);
            } else if (t->updated_at > last_sync) {
                /* Incremental listing: absent just means unchanged — but
                 * the LOCAL side is dirty, so push (etag-guarded).          */
                ok = push_task_patch(db, token, list_gid, t, stats, err);
            }
            continue;
        }
        if (match->deleted) {
            bt_db_task_purge(db, t->id);
            stats->deleted++;
            continue;
        }

        /* Both sides live: compare the synced fields.                       */
        gboolean differs = strcmp(t->title, match->title) != 0 ||
                           strcmp(t->notes, match->notes) != 0 ||
                           t->due != match->due ||
                           t->done != match->done;
        if (!differs) {
            /* Content equal — still refresh the mirror metadata when the
             * remote bumped OR the row predates the metadata columns
             * (etag/web_link empty while the remote has them).              */
            gboolean meta_stale =
                (t->etag == NULL && match->etag != NULL) ||
                (t->web_link == NULL && match->web_link != NULL);
            if (match->updated > t->updated_at || meta_stale) {
                BtTask apply = *t;
                apply.updated_at   = match->updated;
                apply.completed_at = match->completed;
                apply.etag         = match->etag;
                apply.web_link     = match->web_link;
                apply.glinks       = match->glinks;
                apply.assigned     = match->assigned;
                bt_db_task_apply_remote(db, &apply);
            }
            continue;
        }
        gboolean local_dirty = t->updated_at > last_sync;
        if (local_dirty && t->updated_at >= match->updated) {
            ok = push_task_patch(db, token, list_gid, t, stats, err);
        } else {
            BtTask apply = *t;       /* shallow copy is fine here           */
            apply.title        = match->title;
            apply.notes        = match->notes;
            apply.due          = match->due;
            apply.done         = match->done;
            apply.updated_at   = match->updated;
            apply.completed_at = match->completed;
            apply.etag         = match->etag;
            apply.web_link     = match->web_link;
            apply.glinks       = match->glinks;
            apply.assigned     = match->assigned;
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
            /* hidden = completed + cleared on Google (tasks.clear): a
             * locally purged Clear-Completed victim must not resurrect.     */
            if (r->matched || r->deleted || r->hidden ||
                is_child != (pass == 1))
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
            nt.id           = id;
            nt.title        = r->title;
            nt.notes        = r->notes;
            nt.due          = r->due;
            nt.done         = r->done;
            nt.updated_at   = r->updated;
            nt.completed_at = r->completed;
            nt.etag         = r->etag;
            nt.web_link     = r->web_link;
            nt.glinks       = r->glinks;
            nt.assigned     = r->assigned;
            bt_db_task_apply_remote(db, &nt);
            r->matched = TRUE;
            stats->pulled++;
            if (!is_child) {
                /* Make the new row findable for pass 2's children.          */
                BtTask *row = bt_db_task_get(db, id);
                if (row != NULL) {
                    g_hash_table_insert(local_by_gid, row->gtasks_id, row);
                    g_ptr_array_add(local, row);   /* owned by `local`      */
                }
            }
        }
    }

    g_hash_table_destroy(by_gid);
    g_hash_table_destroy(local_by_gid);
    bt_ptr_array_free_tasks(local);
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

/* post_status() — queue a status-bar message from the worker thread
 * (marshalled to the main thread; `msg` is copied).                         */
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
    bt_app_notify_changed(job->app);
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
    gboolean ok = sync_lists(job->app, db, token, last_sync, pairs,
                             &stats, &err);
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
    if (!bt_app_config_get_bool("google_sync_enabled", TRUE)) {
        bt_app_status(app, "Google Tasks sync is disabled \xe2\x80\x94 "
                      "enable it in File \xe2\x86\x92 Settings\xe2\x80\xa6");
        if (done != NULL)
            done(app, FALSE, "sync disabled", user_data);
        return;
    }
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
 * bt_sync_signin_done() — shared tail of a browser sign-in that was
 * started to sync (see gtasks.h).
 * ------------------------------------------------------------------------- */
void
bt_sync_signin_done(BtApp *app, GtkWindow *parent, const gchar *db_path,
                    gboolean ok, const gchar *error, BtSyncDoneFn done)
{
    if (ok) {
        bt_sync_start(app, db_path, done, NULL);
    } else {
        bt_app_notice(parent, GTK_MESSAGE_ERROR,
                      "Hacienda - Google Sign-In",
                      "Could not sign in: %s",
                      error != NULL ? error : "unknown error");
    }
}

/* ===========================================================================
 * Cross-list move (tasks.move + destinationTasklist) and Clear Completed
 * (tasks.clear) — one-shot worker jobs following the sync pattern:
 * network on a thread, results marshalled back via g_idle_add.
 * =========================================================================== */

typedef struct {
    BtApp     *app;
    gchar     *src_gid;              /* source list gid                     */
    gchar     *dest_gid;             /* destination list gid                */
    gchar     *task_gid;             /* the moved task's gid                */
    GPtrArray *child_gids;           /* its subtasks' gids (owned)          */
    gint64     task_id;              /* local ids for the fallback          */
    gint64     src_list_id;
    gboolean   ok;
    gchar     *error;
} MoveJob;

/* move_job_free() — free a MoveJob and everything it owns.                  */
static void
move_job_free(MoveJob *job)
{
    g_free(job->src_gid);
    g_free(job->dest_gid);
    g_free(job->task_gid);
    for (guint i = 0; i < job->child_gids->len; i++)
        g_free(g_ptr_array_index(job->child_gids, i));
    g_ptr_array_free(job->child_gids, TRUE);
    g_free(job->error);
    g_free(job);
}

/* move_fallback() — the offline/failed-move recovery (main thread): the
 * moved rows lose their Google identity (they will push as NEW tasks in
 * the destination list) and stub tombstones in the SOURCE list delete
 * the old remote copies on the next sync.                                   */
static void
move_fallback(BtApp *app, MoveJob *job)
{
    if (job->task_gid == NULL)
        return;                      /* never synced: nothing to unlink     */
    bt_db_insert_remote_tombstone(app->db, job->src_list_id,
                                  job->task_gid);
    bt_db_task_set_gtasks_id(app->db, job->task_id, NULL);
    GPtrArray *subs = bt_db_subtasks(app->db, job->task_id);
    for (guint i = 0; i < subs->len; i++) {
        BtTask *s = g_ptr_array_index(subs, i);
        if (s->gtasks_id != NULL) {
            bt_db_insert_remote_tombstone(app->db, job->src_list_id,
                                          s->gtasks_id);
            bt_db_task_set_gtasks_id(app->db, s->id, NULL);
        }
    }
    bt_ptr_array_free_tasks(subs);
}

/* move_apply() — main-thread completion of the remote move.                 */
static gboolean
move_apply(gpointer data)
{
    MoveJob *job = data;
    if (!job->ok) {
        move_fallback(job->app, job);
        bt_app_status(job->app, "Move will finish on the next sync (%s)",
                      job->error != NULL ? job->error : "remote move failed");
    } else {
        bt_app_status(job->app, "Moved in Google Tasks");
    }
    bt_app_notify_changed(job->app);
    move_job_free(job);
    return G_SOURCE_REMOVE;
}

/* move_call() — one tasks.move POST.                                        */
static gboolean
move_call(const gchar *token, const gchar *src_gid, const gchar *task_gid,
          const gchar *dest_gid, const gchar *parent_gid, gchar **err)
{
    gchar *base = task_url(src_gid, task_gid);
    gchar *dest_esc = escaped(dest_gid);
    GString *url = g_string_new(base);
    g_string_append_printf(url, "/move?destinationTasklist=%s", dest_esc);
    if (parent_gid != NULL) {
        gchar *parent_esc = escaped(parent_gid);
        g_string_append_printf(url, "&parent=%s", parent_esc);
        g_free(parent_esc);
    }
    gboolean ok = api_call("POST", url->str, token, NULL, NULL,
                           NULL, err);
    g_string_free(url, TRUE);
    g_free(dest_esc);
    g_free(base);
    return ok;
}

/* move_thread() — worker: move the parent, then each subtask under it.      */
static gpointer
move_thread(gpointer data)
{
    MoveJob *job = data;
    gchar *token = bt_oauth_access_token(&job->error);
    if (token == NULL) {
        job->ok = FALSE;
        g_idle_add(move_apply, job);
        return NULL;
    }
    job->ok = move_call(token, job->src_gid, job->task_gid,
                        job->dest_gid, NULL, &job->error);
    for (guint i = 0; job->ok && i < job->child_gids->len; i++)
        job->ok = move_call(token, job->src_gid,
                            g_ptr_array_index(job->child_gids, i),
                            job->dest_gid, job->task_gid, &job->error);
    g_free(token);
    g_idle_add(move_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * bt_gtasks_move_task() — move a top-level task to another list (see
 * gtasks.h).  The local move happens immediately; the remote side uses
 * tasks.move with destinationTasklist when possible, else the
 * delete-and-recreate fallback.
 * ------------------------------------------------------------------------- */
void
bt_gtasks_move_task(BtApp *app, gint64 task_id, gint64 dest_list_id)
{
    BtTask *t = bt_db_task_get(app->db, task_id);
    if (t == NULL || t->parent_id != 0 || t->list_id == dest_list_id) {
        bt_task_free(t);
        return;
    }
    BtList *src  = bt_db_list_get(app->db, t->list_id);
    BtList *dest = bt_db_list_get(app->db, dest_list_id);

    MoveJob *job = g_new0(MoveJob, 1);
    job->app         = app;
    job->task_id     = task_id;
    job->src_list_id = t->list_id;
    job->src_gid     = src != NULL ? g_strdup(src->gtasks_id) : NULL;
    job->dest_gid    = dest != NULL ? g_strdup(dest->gtasks_id) : NULL;
    job->task_gid    = g_strdup(t->gtasks_id);
    job->child_gids  = g_ptr_array_new();
    GPtrArray *subs = bt_db_subtasks(app->db, task_id);
    for (guint i = 0; i < subs->len; i++) {
        BtTask *s = g_ptr_array_index(subs, i);
        if (s->gtasks_id != NULL)
            g_ptr_array_add(job->child_gids, g_strdup(s->gtasks_id));
    }
    bt_ptr_array_free_tasks(subs);
    bt_list_free(src);
    bt_list_free(dest);
    bt_task_free(t);

    /* Local move first — the UI reflects it immediately.                    */
    bt_db_task_move_list(app->db, task_id, dest_list_id);

    if (job->task_gid != NULL && job->src_gid != NULL &&
        job->dest_gid != NULL && bt_oauth_authenticated()) {
        GThread *th = g_thread_new("bt-move", move_thread, job);
        g_thread_unref(th);
    } else {
        move_fallback(app, job);     /* offline / unsynced endpoints        */
        bt_app_notify_changed(app);
        move_job_free(job);
    }
}

/* --------------------------------------------------------------------------
 * Clear Completed.
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp   *app;
    gchar   *list_gid;
    gint64   list_id;
    gboolean ok;
    gchar   *error;
} ClearJob;

/* clear_apply() — main-thread completion: purge the local rows (Google
 * hid its copies, and the hidden guard keeps them from resurrecting).       */
static gboolean
clear_apply(gpointer data)
{
    ClearJob *job = data;
    if (job->ok) {
        bt_db_purge_done(job->app->db, job->list_id);
        bt_app_status(job->app, "Completed tasks cleared");
    } else {
        bt_app_status(job->app, "Clear failed: %s",
                      job->error != NULL ? job->error : "unknown error");
    }
    bt_app_notify_changed(job->app);
    g_free(job->list_gid);
    g_free(job->error);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* clear_thread() — worker: POST tasks.clear.                                */
static gpointer
clear_thread(gpointer data)
{
    ClearJob *job = data;
    gchar *token = bt_oauth_access_token(&job->error);
    if (token == NULL) {
        job->ok = FALSE;
        g_idle_add(clear_apply, job);
        return NULL;
    }
    gchar *gid_esc = escaped(job->list_gid);
    gchar *url = g_strdup_printf(BT_TASKS_API "/lists/%s/clear", gid_esc);
    job->ok = api_call("POST", url, token, NULL, NULL, NULL, &job->error);
    g_free(url);
    g_free(gid_esc);
    g_free(token);
    g_idle_add(clear_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * bt_gtasks_clear_completed() — archive a list's done tasks (see
 * gtasks.h).  Synced list + signed in → Google's tasks.clear (hides
 * them there) then a local purge; otherwise the done tasks are
 * tombstone-deleted so the removal still propagates.
 * ------------------------------------------------------------------------- */
void
bt_gtasks_clear_completed(BtApp *app, gint64 list_id)
{
    BtList *l = bt_db_list_get(app->db, list_id);
    if (l == NULL)
        return;
    if (l->gtasks_id != NULL && bt_oauth_authenticated()) {
        ClearJob *job = g_new0(ClearJob, 1);
        job->app      = app;
        job->list_id  = list_id;
        job->list_gid = g_strdup(l->gtasks_id);
        GThread *th = g_thread_new("bt-clear", clear_thread, job);
        g_thread_unref(th);
    } else {
        /* No remote side to archive on: delete instead (tombstones
         * propagate if the list ever syncs).                                */
        GPtrArray *tasks = bt_db_tasks_toplevel(app->db, list_id);
        guint n = 0;                 /* how many rows went                  */
        for (guint i = 0; i < tasks->len; i++) {
            BtTask *t = g_ptr_array_index(tasks, i);
            if (t->done) {
                bt_db_task_delete(app->db, t->id);
                n++;
            }
        }
        bt_ptr_array_free_tasks(tasks);
        bt_app_status(app, "Deleted %u completed task%s", n,
                      n == 1 ? "" : "s");
        bt_app_notify_changed(app);
    }
    bt_list_free(l);
}

/* ---------------------------------------------------------------------------
 * Periodic auto-sync (see gtasks.h) — the timer payload and callbacks.
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp *app;
    gchar *db_path;
} AutoSync;

/* auto_sync_tick() — the periodic timer body: start a pass when signed
 * in and idle.  `data` is the timer-owned AutoSync.                         */
static gboolean
auto_sync_tick(gpointer data)
{
    AutoSync *as = data;
    if (bt_oauth_authenticated() && !as->app->sync_running)
        bt_sync_start(as->app, as->db_path, NULL, NULL);
    return G_SOURCE_CONTINUE;
}

/* auto_sync_free() — GDestroyNotify for the timer payload: without it,
 * every re-arm (each Settings interval change) leaked the old struct.       */
static void
auto_sync_free(gpointer data)
{
    AutoSync *as = data;
    g_free(as->db_path);
    g_free(as);
}

/* bt_sync_auto_start() — (re)arm the auto-sync timer (see gtasks.h).        */
void
bt_sync_auto_start(BtApp *app, const gchar *db_path)
{
    if (app->sync_timer != 0) {
        g_source_remove(app->sync_timer);
        app->sync_timer = 0;
    }
    if (!bt_app_config_get_bool("google_sync_enabled", TRUE))
        return;                      /* master switch off: no timer, no
                                      * initial pass                        */
    gchar *v = bt_app_config_get("sync_interval_min");
    gint minutes = v != NULL ? atoi(v) : 5;
    g_free(v);
    if (minutes <= 0)
        return;

    AutoSync *as = g_new0(AutoSync, 1);
    as->app = app;
    as->db_path = g_strdup(db_path);
    app->sync_timer = g_timeout_add_seconds_full(G_PRIORITY_DEFAULT,
                                                 (guint)(minutes * 60),
                                                 auto_sync_tick, as,
                                                 auto_sync_free);
    if (bt_oauth_authenticated())
        bt_sync_start(app, db_path, NULL, NULL);
}
