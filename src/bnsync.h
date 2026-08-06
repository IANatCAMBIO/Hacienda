/* ===========================================================================
 * bnsync.h — Records action-item mirror for Lists
 *
 * Sync model
 * ----------
 * Every '!' action item in Records is mirrored as an ORDINARY Lists
 * task, so it carries notes, subtasks, attachments, a pin and a
 * priority like any other task — and, living in a real list, it syncs
 * on to Google Tasks as well.  Identity is the item's STABLE uid from
 * `records action list --uid`, stored in tasks.bn_uid; it survives
 * rewording and the renumbering of a note's lines, which the older
 * NOTEID:ORD address did not.
 *
 *   remote item, no local task  → create the mirror task
 *   both present                → push cached local done/due changes,
 *                                 then take Records' title/done/due
 *   local task, item gone       → tombstone the task (Records is
 *                                 authoritative for existence)
 *   task deleted in Lists       → uid parked in bn_deleted so the next
 *                                 pass does not re-create it (Records
 *                                 has no CLI verb to delete an item)
 *
 * Field ownership.  Records owns TITLE, DONE and DUE.  Everything else
 * — notes, subtasks, attachments, pin, priority, which list the task
 * lives in — is Lists-only and never leaves.  The title is one-way by
 * necessity: the CLI has no verb that rewrites an item's text, so a
 * title edited in Lists is overwritten on the next pass.
 *
 * Writes are CACHED, not live.  bn_done/bn_due record the state Records
 * was last known to hold; a row where done/due differ from that
 * baseline IS the pending-write set, so the queue survives a crash and
 * cannot drift out of step with the tasks. Each pass pushes those
 * deltas in bulk, on the interval in "records_sync_interval_min".  A
 * local change therefore WINS over a concurrent Records-side change to
 * the same field: it is pushed first, and the listing that follows
 * reads back what was just written.
 *
 * Threading: the pass runs on a worker thread with its OWN SQLite
 * connection (a connection must not cross threads); the CLI is spawned
 * there too, so a slow Records never blocks the UI.  Status and
 * completion are marshalled back with g_idle_add.
 *
 * Requires a Records new enough to understand `action list --uid`.
 * Against an older build the pass refuses to run and says so rather
 * than falling back to positional addressing, which would silently
 * bind tasks to the wrong items.
 * =========================================================================== */

#ifndef BT_BNSYNC_H
#define BT_BNSYNC_H

#include "app.h"

/* Completion callback; runs on the main thread.  `message` is a short
 * human-readable summary or error (not owned by the callee).                */
typedef void (*BtBnSyncDoneFn)(BtApp *app, gboolean ok, const gchar *message,
                               gpointer user_data);

/* ---------------------------------------------------------------------------
 * bt_bnsync_start() — kick off one mirror pass on a worker thread.
 * Early-outs, each with a status message: the Records integration
 * switched off in Settings, and "already running" (which does not fire
 * `done`).  `done` may be NULL.  Main thread only.
 * ------------------------------------------------------------------------- */
void bt_bnsync_start(BtApp *app, const gchar *db_path,
                     BtBnSyncDoneFn done, gpointer user_data);

/* ---------------------------------------------------------------------------
 * bt_bnsync_auto_start() — install the periodic mirror timer from the
 * "records_sync_interval_min" config key (default 5; 0 disables) and
 * run one initial pass.  Safe to call again after the setting changes.
 * ------------------------------------------------------------------------- */
void bt_bnsync_auto_start(BtApp *app, const gchar *db_path);

/* ---------------------------------------------------------------------------
 * bt_bnsync_target_list() — the list mirrored items are filed into: the
 * one named by "blue_notes_embed_list" when it still exists, else the
 * app-managed "Action Items" list, created on first use.  Returns 0
 * only when the list could not be created.  The config key keeps its
 * pre-rename name — it sits in users' ini files.
 * ------------------------------------------------------------------------- */
gint64 bt_bnsync_target_list(BtDatabase *db, gint64 configured);

#endif /* BT_BNSYNC_H */
