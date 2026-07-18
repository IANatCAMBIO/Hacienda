/* ===========================================================================
 * db.h — SQLite storage for Hacienda
 *
 * Schema (PRAGMA user_version = 5; v2 added lists.emoji, v3 the five
 * Google-mirror task columns, v4 tasks.priority, v5 list_groups +
 * lists.group_id):
 *
 *   list_groups  id, name, position              (local-only; never synced)
 *   lists        id, name, emoji, position, gtasks_id, updated_at, deleted,
 *                group_id (FK → list_groups.id; NULL = ungrouped)
 *   tasks        id, list_id, parent_id (NULL = top-level; ONE level of
 *                nesting only — a subtask can never be a parent),
 *                title, notes, due (unix local midnight; 0 = none), done,
 *                pinned, priority (local-only; sorts first in every
 *                view), position, gtasks_id, updated_at, deleted,
 *                completed_at, etag, web_link, glinks, assigned
 *   attachments  id, task_id, path, added_at   (local-only; never synced)
 *   sync_state   key, value                    (e.g. "last_sync")
 *   bn_pins      ref                           (pinned Blue Notes action
 *                                               items; local-only)
 *   bn_priority  ref                           (high-priority Blue Notes
 *                                               action items; local-only)
 *
 * Deletion is a SOFT flag everywhere (`deleted` = tombstone): the Google
 * Tasks sync needs to see "this existed and was deleted locally" to
 * propagate the delete, after which the row is purged for real.  Every
 * mutation stamps `updated_at` (unix seconds) — rows whose stamp is newer
 * than sync_state.last_sync are the local dirty set.
 * =========================================================================== */

#ifndef BT_DB_H
#define BT_DB_H

#include <glib.h>
#include <sqlite3.h>

/* Database filename, under g_get_user_data_dir()/hacienda/.               */
#define BT_DB_FILENAME "hacienda.db"

/* ---------------------------------------------------------------------------
 * BtDatabase — one open connection.  A connection must not cross threads:
 * the sync worker opens its own on the same path (bt_db_open).
 * ------------------------------------------------------------------------- */
typedef struct {
    sqlite3 *sq;
} BtDatabase;

/* One task list.  Strings are owned by the struct.                          */
typedef struct {
    gint64    id;
    gchar    *name;
    gchar    *emoji;                 /* optional display prefix ("")        */
    gchar    *gtasks_id;             /* Google tasklist id, or NULL         */
    gint64    updated_at;
    gint      position;
    gint64    group_id;              /* 0 = ungrouped                       */
    gboolean  deleted;
} BtList;

/* One list group (local-only; never synced to Google).  Strings owned.     */
typedef struct {
    gint64  id;
    gchar  *name;
    gint    position;
} BtGroup;

/* One task or subtask.  Strings are owned by the struct.  The last five
 * fields mirror read-only (or Google-managed) Task resource data pulled
 * by the sync: completion time, the concurrency etag, the deep link
 * into Google's own UI, and the links[]/assignmentInfo substructures
 * kept as their raw JSON.                                                   */
typedef struct {
    gint64    id;
    gint64    list_id;
    gint64    parent_id;             /* 0 = top-level task                  */
    gchar    *title;
    gchar    *notes;
    gint64    due;                   /* unix local midnight; 0 = no date    */
    gboolean  done;
    gboolean  pinned;
    gboolean  priority;              /* high priority — local-only, like
                                      * pinned (Google has no priority);
                                      * sorts to the top of every view      */
    gint      position;
    gchar    *gtasks_id;             /* Google task id, or NULL             */
    gint64    updated_at;
    gboolean  deleted;
    gint64    completed_at;          /* unix; 0 = never / not done         */
    gchar    *etag;                  /* Google etag, or NULL                */
    gchar    *web_link;              /* Google Tasks UI URL, or NULL        */
    gchar    *glinks;                /* links[] as raw JSON, or NULL        */
    gchar    *assigned;              /* assignmentInfo as raw JSON, / NULL  */
} BtTask;

/* One file attachment on a task.                                            */
typedef struct {
    gint64    id;
    gint64    task_id;
    gchar    *path;                  /* absolute file path (owned)          */
} BtAttachment;

void bt_list_free(BtList *l);
void bt_task_free(BtTask *t);

/* ---------------------------------------------------------------------------
 * bt_db_open() — open (creating/migrating as needed) the database at
 * `path`.  Returns the handle, or NULL with `err` set.
 * ------------------------------------------------------------------------- */
BtDatabase *bt_db_open(const gchar *path, GError **err);

/* bt_db_close() — close the connection and free the handle.  NULL-safe.     */
void bt_db_close(BtDatabase *db);

/* bt_db_default_path() — "<user data dir>/hacienda/hacienda.db",
 * creating the directory.  Returns a new string (g_free it).                */
gchar *bt_db_default_path(void);

/* --------------------------------- lists --------------------------------- */

/* All lists — alphabetical (case-insensitive) by DEFAULT; once the user
 * drag-reorders the sidebar (sync_state "lists_custom_order" set by
 * bt_db_lists_reorder) the stored positions rule, name-tiebroken.
 * include_deleted also returns tombstoned rows (sync).  Returns BtList*
 * elements; free the array with g_ptr_array_free after bt_list_free-ing
 * elements (or use bt_ptr_array_free_lists).                                */
GPtrArray *bt_db_lists(BtDatabase *db, gboolean include_deleted);

/* Persist a sidebar drag-reorder: position = index of each id in `ids`
 * (one transaction) and switch bt_db_lists to custom-order mode.  The
 * order is local-only — Google tasklists have no ordering — so rows are
 * NOT dirtied for sync.                                                     */
void bt_db_lists_reorder(BtDatabase *db, const gint64 *ids, gsize n);

BtList  *bt_db_list_get(BtDatabase *db, gint64 id);

/* Seed the emoji of the list bound to `gtasks_id` — ONLY while its
 * emoji is empty, so a later user edit sticks.  Used by the sync to
 * mark Google's undeletable default list.  No updated_at bump (the
 * emoji is local-only; this must not dirty the row for sync).               */
void bt_db_list_emoji_if_empty(BtDatabase *db, const gchar *gtasks_id,
                               const gchar *emoji);

/* Create a list.  `emoji` is the optional local-only display prefix
 * (NULL/"" for none) — it is never part of the synced name.                 */
gint64   bt_db_list_create(BtDatabase *db, const gchar *name,
                           const gchar *emoji);

/* Update a list's name + emoji (stamps updated_at; a changed name syncs
 * to Google, the emoji never does).                                         */
void     bt_db_list_update(BtDatabase *db, gint64 id, const gchar *name,
                           const gchar *emoji);

/* Tombstone the list AND every task in it (they must disappear from the
 * remote side too).                                                         */
void     bt_db_list_delete(BtDatabase *db, gint64 id);

/* Undo a list tombstone: restore the list and its still-tombstoned
 * tasks (used when Google refuses the deletion — its default tasklist
 * cannot be deleted; remote remains the source of truth).                   */
void     bt_db_list_restore(BtDatabase *db, gint64 id);

/* --------------------------------- tasks --------------------------------- */

BtTask    *bt_db_task_get(BtDatabase *db, gint64 id);

/* Visible top-level tasks of one list, ordered by position.                 */
GPtrArray *bt_db_tasks_toplevel(BtDatabase *db, gint64 list_id);

/* Visible subtasks of one task, ordered by position.                        */
GPtrArray *bt_db_subtasks(BtDatabase *db, gint64 parent_id);

/* ALL visible subtasks (every list), ordered by parent then position —
 * one query for the task pane instead of one per top-level row.             */
GPtrArray *bt_db_subtasks_all_visible(BtDatabase *db);

/* Visible pinned tasks across all lists (any level), pinned order = list
 * then position.                                                            */
GPtrArray *bt_db_tasks_pinned(BtDatabase *db);

/* TRUE when any non-tombstoned task is pinned; with `with_bn_pins`,
 * pinned Blue Notes action items (bn_pins rows, which may be stale —
 * refs whose items vanished still count) do too.  Drives the sidebar's
 * Pinned Tasks row visibility.                                              */
gboolean bt_db_has_pinned(BtDatabase *db, gboolean with_bn_pins);

/* Visible top-level tasks across ALL lists (the "All Tasks" meta list),
 * ordered by list then position.                                            */
GPtrArray *bt_db_tasks_all_visible(BtDatabase *db);

/* Visible tasks (any level) with lo <= due < hi, soonest first.             */
GPtrArray *bt_db_tasks_due_between(BtDatabase *db, gint64 lo, gint64 hi);

/* Every task row of ONE list, including subtasks and tombstones, parents
 * before subtasks (sync — a new parent must own a gtasks_id before its
 * children push).                                                           */
GPtrArray *bt_db_tasks_in_list_all(BtDatabase *db, gint64 list_id);

/* Create a task ('' title allowed).  parent_id = 0 for top-level; the
 * parent must itself be top-level (one nesting level — enforced here).
 * Returns the new id, or 0 on constraint failure.                           */
gint64 bt_db_task_create(BtDatabase *db, gint64 list_id, gint64 parent_id,
                         const gchar *title);

/* Write the editable fields (title/notes/due/done/pinned/priority) from
 * `t` back to its row and stamp updated_at.                                 */
void bt_db_task_update(BtDatabase *db, const BtTask *t);

/* Field setters used by the list-view toggles (stamp updated_at).           */
void bt_db_task_set_done(BtDatabase *db, gint64 id, gboolean done);
void bt_db_task_set_pinned(BtDatabase *db, gint64 id, gboolean pinned);
void bt_db_task_set_priority(BtDatabase *db, gint64 id, gboolean priority);

/* Tombstone the task and its subtasks.                                      */
void bt_db_task_delete(BtDatabase *db, gint64 id);

/* Move a top-level task (and its subtasks) to another list, appended at
 * the end; stamps updated_at.                                               */
void bt_db_task_move_list(BtDatabase *db, gint64 id, gint64 dest_list);

/* Physically remove every DONE task of a list (and their subtasks) —
 * the local half of "Clear Completed" (the remote half is tasks.clear,
 * which hides them on Google's side, so no tombstones are needed).          */
void bt_db_purge_done(BtDatabase *db, gint64 list_id);

/* Insert a bare tombstone carrying a Google task id — the offline
 * fallback for cross-list moves: the moved row starts a NEW remote
 * task while this stub deletes the old remote copy on the next sync.        */
void bt_db_insert_remote_tombstone(BtDatabase *db, gint64 list_id,
                                   const gchar *gtasks_id);

/* ------------------------------ attachments ------------------------------ */

GPtrArray *bt_db_attachments(BtDatabase *db, gint64 task_id);
gint64     bt_db_attachment_add(BtDatabase *db, gint64 task_id,
                                const gchar *path);
void       bt_db_attachment_remove(BtDatabase *db, gint64 id);

/* task_id → attachment count for every task, as one query.  Keys/values
 * are packed into the pointers (GINT_TO_POINTER); free with
 * g_hash_table_destroy.                                                     */
GHashTable *bt_db_attachment_counts(BtDatabase *db);

/* -------------------------------- vitals --------------------------------- */

/* Live totals for the About dialog: non-tombstoned task and list counts.
 * Either out-pointer may be NULL; a failed query leaves 0.                  */
void bt_db_totals(BtDatabase *db, gint *n_tasks, gint *n_lists);

/* -------------------- Blue Notes pins and priorities --------------------- */

/* Pinned state for Blue Notes action items (pinning is a Hacienda
 * concept; Blue Notes has none, so membership lives in the local
 * bn_pins table keyed by the item's "NOTEID:ORD" address).                  */
gboolean    bt_db_bn_pin_get(BtDatabase *db, const gchar *ref);
void        bt_db_bn_pin_set(BtDatabase *db, const gchar *ref,
                             gboolean pinned);

/* All pinned refs as a set (string keys, owned by the table); free with
 * g_hash_table_destroy.                                                     */
GHashTable *bt_db_bn_pins(BtDatabase *db);

/* High-priority state for Blue Notes action items — the same local
 * design as pins (bn_priority table, "NOTEID:ORD" keys): Blue Notes
 * has no priority concept, so the flag only affects where the items
 * appear in Hacienda's views.                                               */
gboolean    bt_db_bn_priority_get(BtDatabase *db, const gchar *ref);
void        bt_db_bn_priority_set(BtDatabase *db, const gchar *ref,
                                  gboolean priority);

/* All high-priority refs as a set; free with g_hash_table_destroy.          */
GHashTable *bt_db_bn_priorities(BtDatabase *db);

/* Drop every gtasks_id/etag of one list's tasks — used when the bound
 * remote list vanished and the list is re-created remotely: its tasks
 * must push as NEW remote tasks (non-destructive sync).                     */
void        bt_db_tasks_clear_gtasks_ids(BtDatabase *db, gint64 list_id);

/* ------------------------------- sync state ------------------------------ */

/* Get/set one sync_state row.  Getter returns a new string or NULL.         */
gchar *bt_db_state_get(BtDatabase *db, const gchar *key);
void   bt_db_state_set(BtDatabase *db, const gchar *key, const gchar *value);

/* Record the Google-side id of a row WITHOUT stamping updated_at (used
 * right after a successful push — the row is not "newly dirty").            */
void bt_db_list_set_gtasks_id(BtDatabase *db, gint64 id, const gchar *gid);
void bt_db_task_set_gtasks_id(BtDatabase *db, gint64 id, const gchar *gid);

/* Overwrite a row from remote data WITHOUT the usual now() stamp — the
 * caller passes the remote updated time so the row is clean afterwards.    */
void bt_db_list_apply_remote(BtDatabase *db, gint64 id, const gchar *name,
                             gint64 updated_at);
void bt_db_task_apply_remote(BtDatabase *db, const BtTask *t);

/* Physically remove tombstoned/remotely-deleted rows.                       */
void bt_db_list_purge(BtDatabase *db, gint64 id);   /* + its tasks          */
void bt_db_task_purge(BtDatabase *db, gint64 id);   /* + its subtasks       */

/* Free helper for the GPtrArrays above.                                     */
void bt_ptr_array_free_lists(GPtrArray *a);
void bt_ptr_array_free_tasks(GPtrArray *a);
void bt_ptr_array_free_attachments(GPtrArray *a);

/* --------------------------------- groups -------------------------------- */

void      bt_group_free(BtGroup *g);
void      bt_ptr_array_free_groups(GPtrArray *a);

/* All groups, ordered by position then name.                                */
GPtrArray *bt_db_groups(BtDatabase *db);

/* Create a group.  Returns the new id, or 0 on failure.                     */
gint64    bt_db_group_create(BtDatabase *db, const gchar *name);

/* Delete a group: un-groups all its lists (sets group_id = NULL).           */
void      bt_db_group_delete(BtDatabase *db, gint64 id);

/* Rename a group.                                                           */
void      bt_db_group_rename(BtDatabase *db, gint64 id, const gchar *name);

/* Move a list into a group (group_id 0 = ungrouped → sets NULL).           */
void      bt_db_list_set_group(BtDatabase *db, gint64 list_id,
                               gint64 group_id);

#endif /* BT_DB_H */
