/* ===========================================================================
 * gtasks.h — two-way Google Tasks sync for Hacienda
 *
 * Sync model
 * ----------
 * Every local list/task carries a `gtasks_id` (its Google-side identity)
 * and an `updated_at` stamp; sync_state.last_sync records when the last
 * successful sync STARTED.  One sync pass:
 *
 *   pull the remote state → match rows by gtasks_id →
 *     local tombstone            → DELETE remote, purge local
 *     local row, no gtasks_id    → POST (create remote), adopt the new id
 *     remote row, no local match → create local (clean, remote stamp)
 *     remote deleted             → purge local (deletion wins)
 *     both present               → newer `updated` wins; the loser is
 *                                  overwritten (local applies keep the
 *                                  REMOTE stamp so the row ends clean)
 *
 * Only the FIRST pass pulls the full remote task state; later passes
 * fetch incrementally (updatedMin = last_sync - 300s skew overlap), and
 * a task absent from such a partial listing means UNCHANGED — it is
 * still pushed if locally dirty, and never treated as deleted
 * (deletions arrive as deleted:true items).
 *
 * First-sync dedup: an unmatched local list adopts an unmatched remote
 * list with the same name (and likewise tasks by title within a list)
 * instead of creating a duplicate; the task-title dedup only runs
 * against a full listing.
 *
 * Google Tasks maps cleanly onto the schema: tasklists ↔ lists, tasks ↔
 * tasks, `parent` ↔ parent_id (the API supports exactly the one nesting
 * level Hacienda allows).  `pinned` and attachments have no Google
 * counterpart and stay local-only.  Task order is not synced.
 *
 * Threading: the whole pass runs on a worker thread with its OWN SQLite
 * connection (a connection must not cross threads); status/completion
 * are marshalled to the main thread with g_idle_add.  The GUI keeps
 * writing during a sync — SQLite's busy timeout serializes the writes,
 * and anything changed mid-sync is simply picked up by the next pass.
 * =========================================================================== */

#ifndef BT_GTASKS_H
#define BT_GTASKS_H

#include "app.h"

/* Completion callback; runs on the main thread.  `message` is a short
 * human-readable summary or error (not owned by the callee).                */
typedef void (*BtSyncDoneFn)(BtApp *app, gboolean ok, const gchar *message,
                             gpointer user_data);

/* ---------------------------------------------------------------------------
 * bt_sync_start() — kick off one sync pass on a worker thread.  Three
 * early-outs, each with a status message: sync disabled in Settings and
 * "not signed in" also fire `done` (FALSE + a short reason); "already
 * running" does not.  `done` may be NULL.  Main thread only.
 * ------------------------------------------------------------------------- */
void bt_sync_start(BtApp *app, const gchar *db_path,
                   BtSyncDoneFn done, gpointer user_data);

/* ---------------------------------------------------------------------------
 * bt_sync_signin_done() — shared tail of a browser sign-in that was
 * started in order to sync: on success kick off a pass (`done` may be
 * NULL), on failure show the standard sign-in-failed dialog over
 * `parent`.  Main thread only.
 * ------------------------------------------------------------------------- */
void bt_sync_signin_done(BtApp *app, GtkWindow *parent,
                         const gchar *db_path, gboolean ok,
                         const gchar *error, BtSyncDoneFn done);

/* ---------------------------------------------------------------------------
 * bt_sync_auto_start() — install the periodic auto-sync timer from the
 * "sync_interval_min" config key (default 5; 0 disables) and run one
 * initial pass when an account is connected.  Safe to call again after
 * the setting changes.
 * ------------------------------------------------------------------------- */
void bt_sync_auto_start(BtApp *app, const gchar *db_path);

/* ---------------------------------------------------------------------------
 * bt_gtasks_move_task() — move a TOP-LEVEL task (and its subtasks) to
 * another list.  Local move is immediate; the Google side moves via
 * tasks.move with destinationTasklist on a worker thread when both
 * lists are synced and the user is signed in, else falls back to
 * delete-old + create-new on the next sync.  Main thread only.
 * ------------------------------------------------------------------------- */
void bt_gtasks_move_task(BtApp *app, gint64 task_id, gint64 dest_list_id);

/* ---------------------------------------------------------------------------
 * bt_gtasks_clear_completed() — archive a list's completed tasks:
 * Google's tasks.clear (hides them in Google Tasks) plus a local purge
 * when possible, tombstone deletion otherwise.  Main thread only.
 * ------------------------------------------------------------------------- */
void bt_gtasks_clear_completed(BtApp *app, gint64 list_id);

#endif /* BT_GTASKS_H */
