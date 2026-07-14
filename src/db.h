/* ===========================================================================
 * db.h — SQLite storage for Blue Tasks
 *
 * Schema (PRAGMA user_version = 1):
 *
 *   lists        id, name, position, gtasks_id, updated_at, deleted
 *   tasks        id, list_id, parent_id (NULL = top-level; ONE level of
 *                nesting only — a subtask can never be a parent),
 *                title, notes, due (unix local midnight; 0 = none), done,
 *                pinned, position, gtasks_id, updated_at, deleted
 *   attachments  id, task_id, path, added_at   (local-only; never synced)
 *   sync_state   key, value                    (e.g. "last_sync")
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

/* Database filename, under g_get_user_data_dir()/blue_tasks/.               */
#define BT_DB_FILENAME "blue_tasks.db"

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
    gboolean  deleted;
} BtList;

/* One task or subtask.  Strings are owned by the struct.                    */
typedef struct {
    gint64    id;
    gint64    list_id;
    gint64    parent_id;             /* 0 = top-level task                  */
    gchar    *title;
    gchar    *notes;
    gint64    due;                   /* unix local midnight; 0 = no date    */
    gboolean  done;
    gboolean  pinned;
    gint      position;
    gchar    *gtasks_id;             /* Google task id, or NULL             */
    gint64    updated_at;
    gboolean  deleted;
} BtTask;

/* One file attachment on a task.                                            */
typedef struct {
    gint64    id;
    gint64    task_id;
    gchar    *path;                  /* absolute file path (owned)          */
} BtAttachment;

void bt_list_free(BtList *l);
void bt_task_free(BtTask *t);
void bt_attachment_free(BtAttachment *a);

/* ---------------------------------------------------------------------------
 * bt_db_open() — open (creating/migrating as needed) the database at
 * `path`.  Returns the handle, or NULL with `err` set.
 * ------------------------------------------------------------------------- */
BtDatabase *bt_db_open(const gchar *path, GError **err);

/* bt_db_close() — close the connection and free the handle.  NULL-safe.     */
void bt_db_close(BtDatabase *db);

/* bt_db_default_path() — "<user data dir>/blue_tasks/blue_tasks.db",
 * creating the directory.  Returns a new string (g_free it).                */
gchar *bt_db_default_path(void);

/* --------------------------------- lists --------------------------------- */

/* All lists ordered by position then name.  include_deleted also returns
 * tombstoned rows (sync).  Returns BtList* elements; free the array with
 * g_ptr_array_free after bt_list_free-ing elements (or use
 * bt_ptr_array_free_lists).                                                 */
GPtrArray *bt_db_lists(BtDatabase *db, gboolean include_deleted);

BtList  *bt_db_list_get(BtDatabase *db, gint64 id);

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

/* --------------------------------- tasks --------------------------------- */

BtTask    *bt_db_task_get(BtDatabase *db, gint64 id);

/* Visible top-level tasks of one list, ordered by position.                 */
GPtrArray *bt_db_tasks_toplevel(BtDatabase *db, gint64 list_id);

/* Visible subtasks of one task, ordered by position.                        */
GPtrArray *bt_db_subtasks(BtDatabase *db, gint64 parent_id);

/* Visible pinned tasks across all lists (any level), pinned order = list
 * then position.                                                            */
GPtrArray *bt_db_tasks_pinned(BtDatabase *db);

/* Visible top-level tasks across ALL lists (the "All Tasks" meta list),
 * ordered by list then position.                                            */
GPtrArray *bt_db_tasks_all_visible(BtDatabase *db);

/* Visible tasks (any level) with lo <= due < hi, soonest first.             */
GPtrArray *bt_db_tasks_due_between(BtDatabase *db, gint64 lo, gint64 hi);

/* Every task row including subtasks and tombstones (sync).                  */
GPtrArray *bt_db_tasks_all(BtDatabase *db);

/* Create a task ('' title allowed).  parent_id = 0 for top-level; the
 * parent must itself be top-level (one nesting level — enforced here).
 * Returns the new id, or 0 on constraint failure.                           */
gint64 bt_db_task_create(BtDatabase *db, gint64 list_id, gint64 parent_id,
                         const gchar *title);

/* Write the editable fields (title/notes/due/done/pinned) from `t` back
 * to its row and stamp updated_at.                                          */
void bt_db_task_update(BtDatabase *db, const BtTask *t);

/* Field setters used by the list-view toggles (stamp updated_at).           */
void bt_db_task_set_done(BtDatabase *db, gint64 id, gboolean done);
void bt_db_task_set_pinned(BtDatabase *db, gint64 id, gboolean pinned);

/* Tombstone the task and its subtasks.                                      */
void bt_db_task_delete(BtDatabase *db, gint64 id);

/* ------------------------------ attachments ------------------------------ */

GPtrArray *bt_db_attachments(BtDatabase *db, gint64 task_id);
gint64     bt_db_attachment_add(BtDatabase *db, gint64 task_id,
                                const gchar *path);
void       bt_db_attachment_remove(BtDatabase *db, gint64 id);

/* task_id → attachment count for every task, as one query.  Keys/values
 * are packed into the pointers (GINT_TO_POINTER); free with
 * g_hash_table_destroy.                                                     */
GHashTable *bt_db_attachment_counts(BtDatabase *db);

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

#endif /* BT_DB_H */
