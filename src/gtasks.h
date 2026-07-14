/* ===========================================================================
 * gtasks.h — two-way Google Tasks sync for Blue Tasks
 *
 * Sync model
 * ----------
 * Every local list/task carries a `gtasks_id` (its Google-side identity)
 * and an `updated_at` stamp; sync_state.last_sync records when the last
 * successful sync STARTED.  One sync pass:
 *
 *   pull the full remote state → match rows by gtasks_id →
 *     local tombstone            → DELETE remote, purge local
 *     local row, no gtasks_id    → POST (create remote), adopt the new id
 *     remote row, no local match → create local (clean, remote stamp)
 *     remote deleted             → purge local (deletion wins)
 *     both present               → newer `updated` wins; the loser is
 *                                  overwritten (local applies keep the
 *                                  REMOTE stamp so the row ends clean)
 *
 * First-sync dedup: an unmatched local list adopts an unmatched remote
 * list with the same name (and likewise tasks by title within a list)
 * instead of creating a duplicate.
 *
 * Google Tasks maps cleanly onto the schema: tasklists ↔ lists, tasks ↔
 * tasks, `parent` ↔ parent_id (the API supports exactly the one nesting
 * level Blue Tasks allows).  `pinned` and attachments have no Google
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
 * bt_sync_start() — kick off one sync pass on a worker thread.  No-op
 * (with a status message) when a sync is already running or no Google
 * account is connected.  `done` may be NULL.  Main thread only.
 * ------------------------------------------------------------------------- */
void bt_sync_start(BtApp *app, const gchar *db_path,
                   BtSyncDoneFn done, gpointer user_data);

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
