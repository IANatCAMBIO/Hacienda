/* ===========================================================================
 * bnsync.c — Records action-item mirror (see bnsync.h)
 * =========================================================================== */

#include "bnsync.h"
#include "bnotes.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The list mirrored items land in when Settings names none.  Created on
 * first use; the emoji marks it the way the sync marks Google's default
 * list.                                                                     */
#define BN_LIST_NAME  "Action Items"
#define BN_LIST_EMOJI "\xe2\x9d\x97"          /* ❗                          */

/* ---------------------------------------------------------------------------
 * One mirror pass in flight.  Built on the main thread, handed to the
 * worker, freed by the completion callback.
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp          *app;
    gchar          *db_path;         /* worker opens its OWN connection     */
    gint64          configured_list; /* Settings choice; 0 = auto           */
    BtBnSyncDoneFn  done;
    gpointer        user_data;

    gboolean        ok;
    gchar          *message;         /* summary or error (owned)            */
    gint            n_created;
    gint            n_updated;
    gint            n_removed;
    gint            n_pushed;
    gint            n_failed;        /* pushes Records refused              */
} BnJob;

/* ---------------------------------------------------------------------------
 * bt_bnsync_target_list() — resolve the destination list (see bnsync.h).
 * ------------------------------------------------------------------------- */
gint64
bt_bnsync_target_list(BtDatabase *db, gint64 configured)
{
    if (configured > 0) {
        BtList *l = bt_db_list_get(db, configured);
        if (l != NULL) {
            gboolean alive = !l->deleted;
            bt_list_free(l);
            if (alive)
                return configured;
        }
        /* A deleted or vanished target falls through to the managed
         * list rather than stranding every item (bn_embed_list did the
         * same before the mirror existed).                                  */
    }
    GPtrArray *lists = bt_db_lists(db, FALSE);
    gint64 found = 0;                /* the managed list, if it exists      */
    for (guint i = 0; i < lists->len && found == 0; i++) {
        BtList *l = g_ptr_array_index(lists, i);
        if (g_strcmp0(l->name, BN_LIST_NAME) == 0)
            found = l->id;
    }
    bt_ptr_array_free_lists(lists);
    if (found != 0)
        return found;
    return bt_db_list_create(db, BN_LIST_NAME, BN_LIST_EMOJI);
}

/* ---------------------------------------------------------------------------
 * drain_legacy_flags() — carry a pre-mirror pin/priority onto the task
 * that now represents the item, then clear the legacy row.  Those
 * tables were keyed by the POSITIONAL "NOTEID:ORD" address, which is
 * why this runs once per item, at the moment its mirror task is
 * created, and never again.
 * ------------------------------------------------------------------------- */
static void
drain_legacy_flags(BtDatabase *db, gint64 task_id, const gchar *ref)
{
    if (ref == NULL)
        return;
    if (bt_db_bn_pin_get(db, ref)) {
        bt_db_task_set_pinned(db, task_id, TRUE);
        bt_db_bn_pin_set(db, ref, FALSE);
    }
    if (bt_db_bn_priority_get(db, ref)) {
        bt_db_task_set_priority(db, task_id, TRUE);
        bt_db_bn_priority_set(db, ref, FALSE);
    }
}

/* ---------------------------------------------------------------------------
 * sync_item() — reconcile ONE listed action item with its mirror task.
 * Creates the task when it is new, otherwise pushes any cached local
 * done/due change and then applies whatever Records holds.  Counts land
 * in `job`.
 * ------------------------------------------------------------------------- */
static void
sync_item(BnJob *job, BtDatabase *db, const BtNoteAction *it, gint64 target)
{
    BtTask *t = bt_db_task_by_bn_uid(db, it->uid);

    if (t == NULL) {                 /* new item → new mirror task          */
        gint64 id = bt_db_task_create(db, target, 0, it->text);
        if (id == 0) {
            job->n_failed++;         /* create failures must not be silent  */
            return;
        }
        bt_db_task_apply_records(db, id, it->text, it->done, it->due,
                                 it->done, it->due);
        bt_db_task_set_bn(db, id, it->uid, it->done, it->due);
        drain_legacy_flags(db, id, it->ref);
        job->n_created++;
        return;
    }

    /* The pending-write set: fields that drifted from the baseline since
     * the last successful push.                                             */
    gboolean done_dirty = t->done != t->bn_done;
    gboolean due_dirty  = t->due  != t->bn_due;
    gboolean done_sent  = FALSE;     /* did Records accept the push?        */
    gboolean due_sent   = FALSE;

    if (done_dirty) {
        gchar *err = NULL;
        done_sent = bt_bnotes_action_set_done(it->uid, t->done, &err);
        if (done_sent) job->n_pushed++; else job->n_failed++;
        g_free(err);
    }
    if (due_dirty) {
        gchar *err = NULL;
        due_sent = bt_bnotes_action_set_due(it->uid, t->due, &err);
        if (due_sent) job->n_pushed++; else job->n_failed++;
        g_free(err);
    }

    /* What the task should hold: a local change stands whether or not
     * the push landed (a refused push must never discard the user's
     * edit), otherwise Records wins.                                        */
    gboolean new_done = done_dirty ? t->done : it->done;
    gint64   new_due  = due_dirty  ? t->due  : it->due;

    /* What Records now holds: the pushed value only if it was accepted;
     * an unsent change keeps the old baseline so it is retried.             */
    gboolean base_done = done_dirty ? (done_sent ? t->done : t->bn_done)
                                    : it->done;
    gint64   base_due  = due_dirty  ? (due_sent  ? t->due  : t->bn_due)
                                    : it->due;

    gboolean content = g_strcmp0(t->title, it->text) != 0 ||
                       new_done != t->done || new_due != t->due;

    if (content) {
        /* Stamps updated_at, so the change reaches Google too.              */
        bt_db_task_apply_records(db, t->id, it->text, new_done, new_due,
                                 base_done, base_due);
        job->n_updated++;
    } else if (base_done != t->bn_done || base_due != t->bn_due) {
        /* Nothing the user can see changed — only the push baseline —
         * so this must NOT stamp updated_at, or every pass would dirty
         * the row and buy a no-op Google PATCH.                             */
        bt_db_task_set_bn(db, t->id, it->uid, base_done, base_due);
    }
    bt_task_free(t);
}

/* ---------------------------------------------------------------------------
 * reap_missing() — tombstone mirror tasks whose item has left Records
 * (Records is authoritative for existence), and forget suppressions for
 * uids that are gone for good.
 *
 * `present` holds every uid in the listing.  The listing is always FULL
 * — Records has no incremental form — so absence really does mean gone,
 * unlike the Google pass where a partial listing makes absence
 * meaningless.
 * ------------------------------------------------------------------------- */
static void
reap_missing(BnJob *job, BtDatabase *db, GHashTable *present,
             GHashTable *suppressed)
{
    GPtrArray *mirror = bt_db_tasks_bn_mirror(db);
    for (guint i = 0; i < mirror->len; i++) {
        BtTask *t = g_ptr_array_index(mirror, i);
        if (g_hash_table_contains(present, GSIZE_TO_POINTER(t->bn_uid)))
            continue;
        bt_db_task_delete(db, t->id);
        /* task_delete parks the uid in bn_deleted so a live item is not
         * re-created after a local delete; here the item is gone from
         * Records, so that suppression has nothing left to suppress.       */
        bt_db_bn_deleted_forget(db, t->bn_uid);
        job->n_removed++;
    }
    bt_ptr_array_free_tasks(mirror);

    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, suppressed);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        if (!g_hash_table_contains(present, key))
            bt_db_bn_deleted_forget(db, (gint64)GPOINTER_TO_SIZE(key));
    }
}

/* ---------------------------------------------------------------------------
 * bn_apply() — main-thread completion: clear the guard, report, refresh.
 * ------------------------------------------------------------------------- */
static gboolean
bn_apply(gpointer data)
{
    BnJob *job = data;
    job->app->bn_sync_running = FALSE;
    if (job->message != NULL)
        bt_app_status(job->app, "%s", job->message);
    /* Structural: tasks appeared or vanished, and a first pass may have
     * created the list they live in.                                        */
    bt_app_notify_changed(job->app);
    if (job->done != NULL)
        job->done(job->app, job->ok, job->message, job->user_data);
    g_free(job->db_path);
    g_free(job->message);
    g_free(job);
    return G_SOURCE_REMOVE;
}

/* ---------------------------------------------------------------------------
 * bn_thread() — the worker: list, reconcile, reap.  Owns its SQLite
 * connection for the whole pass.
 * ------------------------------------------------------------------------- */
static gpointer
bn_thread(gpointer data)
{
    BnJob *job = data;
    GError *gerr = NULL;
    BtDatabase *db = bt_db_open(job->db_path, &gerr);
    if (db == NULL) {
        job->message = g_strdup_printf("Records sync failed: %s",
                                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    gchar *err = NULL;
    GPtrArray *items = bt_bnotes_actions(&err);
    if (items == NULL) {
        /* Tell "Records is too old" apart from every other failure —
         * the fix is entirely different, and positional addressing is
         * NOT an acceptable fallback (it would bind tasks to whichever
         * item happens to sit at that position).                            */
        if (!bt_bnotes_supports_uid())
            job->message = g_strdup("Records is too old for action-item "
                                    "sync \xe2\x80\x94 update Records");
        else
            job->message = g_strdup_printf("Records sync failed: %s",
                                           err != NULL ? err : "unknown");
        g_free(err);
        bt_db_close(db);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    gint64 target = bt_bnsync_target_list(db, job->configured_list);
    if (target == 0) {
        job->message = g_strdup("Records sync failed: cannot create the "
                                "Action Items list");
        bt_bnotes_actions_free(items);
        bt_db_close(db);
        g_idle_add(bn_apply, job);
        return NULL;
    }

    GHashTable *suppressed = bt_db_bn_deleted(db);
    GHashTable *present = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < items->len; i++) {
        BtNoteAction *it = g_ptr_array_index(items, i);
        g_hash_table_add(present, GSIZE_TO_POINTER(it->uid));
    }
    for (guint i = 0; i < items->len; i++) {
        BtNoteAction *it = g_ptr_array_index(items, i);
        if (g_hash_table_contains(suppressed, GSIZE_TO_POINTER(it->uid)))
            continue;            /* deleted in Lists; do not re-create      */
        sync_item(job, db, it, target);
    }
    reap_missing(job, db, present, suppressed);

    gchar *stamp = g_strdup_printf("%lld", (long long)time(NULL));
    bt_db_state_set(db, "bn_last_sync", stamp);
    g_free(stamp);

    job->ok = job->n_failed == 0;
    if (job->n_created == 0 && job->n_updated == 0 && job->n_removed == 0 &&
        job->n_pushed == 0 && job->n_failed == 0) {
        job->message = g_strdup("Action items up to date");
    } else {
        GString *s = g_string_new("Action items:");
        if (job->n_created > 0)
            g_string_append_printf(s, " %d added,", job->n_created);
        if (job->n_updated > 0)
            g_string_append_printf(s, " %d updated,", job->n_updated);
        if (job->n_removed > 0)
            g_string_append_printf(s, " %d removed,", job->n_removed);
        if (job->n_pushed > 0)
            g_string_append_printf(s, " %d sent to Records,", job->n_pushed);
        if (job->n_failed > 0)
            g_string_append_printf(s, " %d failed,", job->n_failed);
        if (s->len > 0 && s->str[s->len - 1] == ',')
            g_string_truncate(s, s->len - 1);
        job->message = g_string_free(s, FALSE);
    }

    g_hash_table_destroy(present);
    g_hash_table_destroy(suppressed);
    bt_bnotes_actions_free(items);
    bt_db_close(db);
    g_idle_add(bn_apply, job);
    return NULL;
}

/* ---------------------------------------------------------------------------
 * bt_bnsync_start() — kick off one pass (see bnsync.h).
 * ------------------------------------------------------------------------- */
void
bt_bnsync_start(BtApp *app, const gchar *db_path, BtBnSyncDoneFn done,
                gpointer user_data)
{
    if (app->bn_sync_running)
        return;                      /* silent: a pass is already running   */
    if (!bt_app_config_get_bool("blue_notes_sync", FALSE)) {
        bt_app_status(app, "Records integration is off");
        if (done != NULL)
            done(app, FALSE, "Records integration is off", user_data);
        return;
    }

    BnJob *job = g_new0(BnJob, 1);
    job->app       = app;
    job->db_path   = g_strdup(db_path);
    job->done      = done;
    job->user_data = user_data;
    /* Config is read HERE, on the main thread — the worker must not
     * touch the shared GKeyFile.                                            */
    gchar *v = bt_app_config_get("blue_notes_embed_list");
    job->configured_list = v != NULL ? g_ascii_strtoll(v, NULL, 10) : 0;
    g_free(v);

    app->bn_sync_running = TRUE;
    bt_app_status(app, "Syncing action items\xe2\x80\xa6");
    GThread *th = g_thread_new("bt-bnsync", bn_thread, job);
    g_thread_unref(th);
}

/* ---------------------------------------------------------------------------
 * Periodic mirror pass — timer payload and callbacks (see bnsync.h).
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp *app;
    gchar *db_path;
} BnAuto;

/* bn_auto_tick() — the periodic timer body.                                 */
static gboolean
bn_auto_tick(gpointer data)
{
    BnAuto *au = data;
    if (!au->app->bn_sync_running)
        bt_bnsync_start(au->app, au->db_path, NULL, NULL);
    return G_SOURCE_CONTINUE;
}

/* bn_auto_free() — GDestroyNotify for the timer payload (re-arming on
 * every Settings change would otherwise leak the old struct).               */
static void
bn_auto_free(gpointer data)
{
    BnAuto *au = data;
    g_free(au->db_path);
    g_free(au);
}

/* ---------------------------------------------------------------------------
 * bt_bnsync_auto_start() — (re)arm the mirror timer (see bnsync.h).
 * ------------------------------------------------------------------------- */
void
bt_bnsync_auto_start(BtApp *app, const gchar *db_path)
{
    if (app->bn_sync_timer != 0) {
        g_source_remove(app->bn_sync_timer);
        app->bn_sync_timer = 0;
    }
    if (!bt_app_config_get_bool("blue_notes_sync", FALSE))
        return;                      /* integration off: no timer, no pass  */

    gchar *v = bt_app_config_get("records_sync_interval_min");
    gint minutes = v != NULL ? atoi(v) : 5;
    g_free(v);
    if (minutes > 0) {
        BnAuto *au = g_new0(BnAuto, 1);
        au->app     = app;
        au->db_path = g_strdup(db_path);
        app->bn_sync_timer = g_timeout_add_seconds_full(
            G_PRIORITY_DEFAULT, (guint)(minutes * 60), bn_auto_tick, au,
            bn_auto_free);
    }
    /* One pass now even when the interval is 0 (manual): the mirror has
     * to be populated before the user can act on it.                        */
    bt_bnsync_start(app, db_path, NULL, NULL);
}
