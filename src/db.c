/* ===========================================================================
 * db.c — SQLite storage for Blue Tasks (see db.h)
 * =========================================================================== */

#include "db.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * exec() — run one or more statements with no results; failures are
 * logged, not fatal.  Returns TRUE when everything ran.
 * ------------------------------------------------------------------------- */
static gboolean
exec(BtDatabase *db, const gchar *sql)
{
    gchar *msg = NULL;               /* sqlite's error text                 */
    if (sqlite3_exec(db->sq, sql, NULL, NULL, &msg) != SQLITE_OK) {
        g_warning("db: %s: %s", sql, msg != NULL ? msg : "?");
        sqlite3_free(msg);
        return FALSE;
    }
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * exec_txn() — run `sql` (one or more statements) inside a transaction,
 * ROLLING BACK on failure.  A bare "BEGIN;…;COMMIT;" through exec()
 * would leave the connection stuck inside an open transaction when a
 * middle statement fails (e.g. SQLITE_BUSY against the sync worker) —
 * every later BEGIN would then fail and writes would silently vanish
 * until the connection closes.
 * ------------------------------------------------------------------------- */
static void
exec_txn(BtDatabase *db, const gchar *sql)
{
    if (!exec(db, "BEGIN IMMEDIATE"))
        return;
    if (exec(db, sql)) {
        if (!exec(db, "COMMIT"))
            exec(db, "ROLLBACK");
    } else {
        exec(db, "ROLLBACK");
    }
}

/* ---------------------------------------------------------------------------
 * step_done() — run a prepared WRITE statement to completion, logging
 * sqlite's message when it fails (SQLITE_BUSY against the sync worker,
 * constraint violations, I/O errors) — silent write loss is the one
 * unacceptable outcome.
 *   db  — the connection (for the error text).
 *   st  — the prepared, bound statement, or NULL when the PREPARE
 *         itself failed (also logged); the caller finalizes either way.
 *   ctx — short operation name for the log line.
 * Returns TRUE when the statement completed.
 * ------------------------------------------------------------------------- */
static gboolean
step_done(BtDatabase *db, sqlite3_stmt *st, const gchar *ctx)
{
    if (st == NULL) {
        g_warning("db: %s: prepare failed: %s", ctx,
                  sqlite3_errmsg(db->sq));
        return FALSE;
    }
    if (sqlite3_step(st) != SQLITE_DONE) {
        g_warning("db: %s: %s", ctx, sqlite3_errmsg(db->sq));
        return FALSE;
    }
    return TRUE;
}

/* column_text_dup() — g_strdup a TEXT column, NULL when SQL NULL.
 *   st/col — the row being read and the 0-based column index.               */
static gchar *
column_text_dup(sqlite3_stmt *st, int col)
{
    const unsigned char *s = sqlite3_column_text(st, col);
    return s != NULL ? g_strdup((const gchar *)s) : NULL;
}

/* now() — current unix time, the updated_at stamp.                          */
static gint64
now(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

/* ---------------------------------------------------------------------------
 * Struct free helpers.
 * ------------------------------------------------------------------------- */
void
bt_list_free(BtList *l)
{
    if (l == NULL)
        return;
    g_free(l->name);
    g_free(l->emoji);
    g_free(l->gtasks_id);
    g_free(l);
}

/* bt_task_free() — free one task and its owned strings.  NULL-safe.        */
void
bt_task_free(BtTask *t)
{
    if (t == NULL)
        return;
    g_free(t->title);
    g_free(t->notes);
    g_free(t->gtasks_id);
    g_free(t->etag);
    g_free(t->web_link);
    g_free(t->glinks);
    g_free(t->assigned);
    g_free(t);
}

/* bt_attachment_free() — free one attachment row.  NULL-safe.               */
static void
bt_attachment_free(BtAttachment *a)
{
    if (a == NULL)
        return;
    g_free(a->path);
    g_free(a);
}

/* bt_ptr_array_free_lists() — free an array of BtList*.  NULL-safe.         */
void
bt_ptr_array_free_lists(GPtrArray *a)
{
    if (a == NULL)
        return;
    for (guint i = 0; i < a->len; i++)
        bt_list_free(g_ptr_array_index(a, i));
    g_ptr_array_free(a, TRUE);
}

/* bt_ptr_array_free_tasks() — free an array of BtTask*.  NULL-safe.         */
void
bt_ptr_array_free_tasks(GPtrArray *a)
{
    if (a == NULL)
        return;
    for (guint i = 0; i < a->len; i++)
        bt_task_free(g_ptr_array_index(a, i));
    g_ptr_array_free(a, TRUE);
}

/* bt_ptr_array_free_attachments() — free BtAttachment*s.  NULL-safe.        */
void
bt_ptr_array_free_attachments(GPtrArray *a)
{
    if (a == NULL)
        return;
    for (guint i = 0; i < a->len; i++)
        bt_attachment_free(g_ptr_array_index(a, i));
    g_ptr_array_free(a, TRUE);
}

/* ---------------------------------------------------------------------------
 * bt_db_default_path() — the standard db location (see db.h).
 * ------------------------------------------------------------------------- */
gchar *
bt_db_default_path(void)
{
    gchar *dir = g_build_filename(g_get_user_data_dir(), "blue_tasks", NULL);
    g_mkdir_with_parents(dir, 0755);
    gchar *path = g_build_filename(dir, BT_DB_FILENAME, NULL);
    g_free(dir);
    return path;
}

/* ---------------------------------------------------------------------------
 * bt_db_open() — open + create/migrate the schema (see db.h).
 * ------------------------------------------------------------------------- */
BtDatabase *
bt_db_open(const gchar *path, GError **err)
{
    sqlite3 *sq = NULL;
    if (sqlite3_open(path, &sq) != SQLITE_OK) {
        g_set_error(err, g_quark_from_static_string("bt-db"), 1,
                    "cannot open %s: %s", path,
                    sq != NULL ? sqlite3_errmsg(sq) : "?");
        if (sq != NULL)
            sqlite3_close(sq);
        return NULL;
    }
    BtDatabase *db = g_new0(BtDatabase, 1);
    db->sq = sq;

    sqlite3_busy_timeout(sq, 5000);  /* GUI + sync worker share the file    */
    exec(db, "PRAGMA foreign_keys = ON");

    /* Schema v1.  CREATE IF NOT EXISTS keeps reopen cheap; user_version
     * gates future migrations.                                              */
    exec(db,
        "CREATE TABLE IF NOT EXISTS lists ("
        "  id         INTEGER PRIMARY KEY,"
        "  name       TEXT    NOT NULL DEFAULT '',"
        "  emoji      TEXT    NOT NULL DEFAULT '',"
        "  position   INTEGER NOT NULL DEFAULT 0,"
        "  gtasks_id  TEXT,"
        "  updated_at INTEGER NOT NULL DEFAULT 0,"
        "  deleted    INTEGER NOT NULL DEFAULT 0)");
    exec(db,
        "CREATE TABLE IF NOT EXISTS tasks ("
        "  id           INTEGER PRIMARY KEY,"
        "  list_id      INTEGER NOT NULL REFERENCES lists(id),"
        "  parent_id    INTEGER REFERENCES tasks(id),"
        "  title        TEXT    NOT NULL DEFAULT '',"
        "  notes        TEXT    NOT NULL DEFAULT '',"
        "  due          INTEGER NOT NULL DEFAULT 0,"
        "  done         INTEGER NOT NULL DEFAULT 0,"
        "  pinned       INTEGER NOT NULL DEFAULT 0,"
        "  position     INTEGER NOT NULL DEFAULT 0,"
        "  gtasks_id    TEXT,"
        "  updated_at   INTEGER NOT NULL DEFAULT 0,"
        "  deleted      INTEGER NOT NULL DEFAULT 0,"
        "  completed_at INTEGER NOT NULL DEFAULT 0,"
        "  etag         TEXT,"
        "  web_link     TEXT,"
        "  glinks       TEXT,"
        "  assigned     TEXT)");
    exec(db,
        "CREATE TABLE IF NOT EXISTS attachments ("
        "  id         INTEGER PRIMARY KEY,"
        "  task_id    INTEGER NOT NULL REFERENCES tasks(id)"
        "                     ON DELETE CASCADE,"
        "  path       TEXT    NOT NULL,"
        "  added_at   INTEGER NOT NULL DEFAULT 0)");
    exec(db,
        "CREATE TABLE IF NOT EXISTS sync_state ("
        "  key   TEXT PRIMARY KEY,"
        "  value TEXT)");
    exec(db, "CREATE INDEX IF NOT EXISTS idx_tasks_list "
             "ON tasks(list_id, parent_id, position)");

    /* Guarded migrations (on fresh files the ALTERs fail silently —
     * CREATE already has the columns): v2 = lists.emoji; v3 = the five
     * Google-mirror task columns.                                           */
    sqlite3_stmt *vst = NULL;
    gint uv = 0;                     /* the file's schema version           */
    if (sqlite3_prepare_v2(sq, "PRAGMA user_version", -1, &vst, NULL)
        == SQLITE_OK && sqlite3_step(vst) == SQLITE_ROW)
        uv = sqlite3_column_int(vst, 0);
    sqlite3_finalize(vst);
    if (uv < 2)
        sqlite3_exec(sq, "ALTER TABLE lists ADD COLUMN emoji TEXT "
                     "NOT NULL DEFAULT ''", NULL, NULL, NULL);
    if (uv < 3) {
        sqlite3_exec(sq, "ALTER TABLE tasks ADD COLUMN completed_at "
                     "INTEGER NOT NULL DEFAULT 0", NULL, NULL, NULL);
        sqlite3_exec(sq, "ALTER TABLE tasks ADD COLUMN etag TEXT",
                     NULL, NULL, NULL);
        sqlite3_exec(sq, "ALTER TABLE tasks ADD COLUMN web_link TEXT",
                     NULL, NULL, NULL);
        sqlite3_exec(sq, "ALTER TABLE tasks ADD COLUMN glinks TEXT",
                     NULL, NULL, NULL);
        sqlite3_exec(sq, "ALTER TABLE tasks ADD COLUMN assigned TEXT",
                     NULL, NULL, NULL);
    }
    exec(db, "PRAGMA user_version = 3");
    return db;
}

/* bt_db_close() — close the connection (see db.h).                          */
void
bt_db_close(BtDatabase *db)
{
    if (db == NULL)
        return;
    sqlite3_close(db->sq);
    g_free(db);
}

/* ---------------------------------------------------------------------------
 * read_list() — build a BtList from the standard lists SELECT
 * (id, name, position, gtasks_id, updated_at, deleted).
 * ------------------------------------------------------------------------- */
static BtList *
read_list(sqlite3_stmt *st)
{
    BtList *l = g_new0(BtList, 1);
    l->id         = sqlite3_column_int64(st, 0);
    l->name       = column_text_dup(st, 1);
    l->position   = sqlite3_column_int(st, 2);
    l->gtasks_id  = column_text_dup(st, 3);
    l->updated_at = sqlite3_column_int64(st, 4);
    l->deleted    = sqlite3_column_int(st, 5) != 0;
    l->emoji      = column_text_dup(st, 6);
    if (l->name == NULL)
        l->name = g_strdup("");
    if (l->emoji == NULL)
        l->emoji = g_strdup("");
    return l;
}

/* The shared column list for read_list().                                   */
#define LIST_COLS "id, name, position, gtasks_id, updated_at, deleted, " \
                  "emoji"

/* ---------------------------------------------------------------------------
 * bt_db_lists() — all (visible) lists (see db.h).
 * ------------------------------------------------------------------------- */
GPtrArray *
bt_db_lists(BtDatabase *db, gboolean include_deleted)
{
    GPtrArray *out = g_ptr_array_new();
    const gchar *sql = include_deleted
        ? "SELECT " LIST_COLS " FROM lists ORDER BY position, name"
        : "SELECT " LIST_COLS " FROM lists WHERE deleted = 0 "
          "ORDER BY position, name";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW)
            g_ptr_array_add(out, read_list(st));
    else
        step_done(db, NULL, "lists query");
    sqlite3_finalize(st);
    return out;
}

/* bt_db_list_get() — one list row, tombstoned or not; NULL if absent.       */
BtList *
bt_db_list_get(BtDatabase *db, gint64 id)
{
    sqlite3_stmt *st = NULL;
    BtList *l = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "SELECT " LIST_COLS " FROM lists WHERE id = ?", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW)
            l = read_list(st);
    }
    sqlite3_finalize(st);
    return l;
}

/* ---------------------------------------------------------------------------
 * bt_db_list_create() — append a new list (see db.h).
 * ------------------------------------------------------------------------- */
gint64
bt_db_list_create(BtDatabase *db, const gchar *name, const gchar *emoji)
{
    sqlite3_stmt *st = NULL;
    gint64 id = 0;                   /* the new rowid                       */
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO lists(name, emoji, position, updated_at) "
            "VALUES(?, ?, "
            "(SELECT COALESCE(MAX(position), 0) + 1 FROM lists), ?)", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, emoji != NULL ? emoji : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        if (step_done(db, st, "list create"))
            id = sqlite3_last_insert_rowid(db->sq);
    } else {
        step_done(db, NULL, "list create");
    }
    sqlite3_finalize(st);
    return id;
}

/* bt_db_list_update() — rename/re-emoji + stamp.                            */
void
bt_db_list_update(BtDatabase *db, gint64 id, const gchar *name,
                  const gchar *emoji)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE lists SET name = ?, emoji = ?, updated_at = ? "
            "WHERE id = ?", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, emoji != NULL ? emoji : "", -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        sqlite3_bind_int64(st, 4, id);
        step_done(db, st, "list update");
    } else {
        step_done(db, NULL, "list update");
    }
    sqlite3_finalize(st);
}

/* ---------------------------------------------------------------------------
 * bt_db_list_delete() — tombstone the list and every task in it.
 * ------------------------------------------------------------------------- */
void
bt_db_list_delete(BtDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET deleted = 1, updated_at = %lld "
        "  WHERE list_id = %lld;"
        "UPDATE lists SET deleted = 1, updated_at = %lld WHERE id = %lld;",
        (long long)now(), (long long)id, (long long)now(), (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* ---------------------------------------------------------------------------
 * read_task() — build a BtTask from the standard tasks SELECT.
 * ------------------------------------------------------------------------- */
#define TASK_COLS "id, list_id, COALESCE(parent_id, 0), title, notes, due, " \
                  "done, pinned, position, gtasks_id, updated_at, deleted, " \
                  "completed_at, etag, web_link, glinks, assigned"

static BtTask *
read_task(sqlite3_stmt *st)
{
    BtTask *t = g_new0(BtTask, 1);
    t->id           = sqlite3_column_int64(st, 0);
    t->list_id      = sqlite3_column_int64(st, 1);
    t->parent_id    = sqlite3_column_int64(st, 2);
    t->title        = column_text_dup(st, 3);
    t->notes        = column_text_dup(st, 4);
    t->due          = sqlite3_column_int64(st, 5);
    t->done         = sqlite3_column_int(st, 6) != 0;
    t->pinned       = sqlite3_column_int(st, 7) != 0;
    t->position     = sqlite3_column_int(st, 8);
    t->gtasks_id    = column_text_dup(st, 9);
    t->updated_at   = sqlite3_column_int64(st, 10);
    t->deleted      = sqlite3_column_int(st, 11) != 0;
    t->completed_at = sqlite3_column_int64(st, 12);
    t->etag         = column_text_dup(st, 13);
    t->web_link     = column_text_dup(st, 14);
    t->glinks       = column_text_dup(st, 15);
    t->assigned     = column_text_dup(st, 16);
    if (t->title == NULL) t->title = g_strdup("");
    if (t->notes == NULL) t->notes = g_strdup("");
    return t;
}

/* ---------------------------------------------------------------------------
 * task_query() — run a tasks SELECT (already using TASK_COLS) and
 * collect the rows.
 *   db    — the connection.
 *   sql   — the full statement text.
 *   nbind — how many of a/b to bind as int64 parameters ?1/?2 (0-2).
 * Returns a GPtrArray of BtTask* (possibly empty, never NULL); free
 * with bt_ptr_array_free_tasks.  Prepare failures are logged and yield
 * the empty array.
 * ------------------------------------------------------------------------- */
static GPtrArray *
task_query(BtDatabase *db, const gchar *sql, gint nbind, gint64 a, gint64 b)
{
    GPtrArray *out = g_ptr_array_new();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK) {
        if (nbind >= 1) sqlite3_bind_int64(st, 1, a);
        if (nbind >= 2) sqlite3_bind_int64(st, 2, b);
        while (sqlite3_step(st) == SQLITE_ROW)
            g_ptr_array_add(out, read_task(st));
    } else {
        step_done(db, NULL, "task query");
    }
    sqlite3_finalize(st);
    return out;
}

/* bt_db_task_get() — one task row; NULL if absent.                          */
BtTask *
bt_db_task_get(BtDatabase *db, gint64 id)
{
    GPtrArray *a = task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE id = ?", 1, id, 0);
    BtTask *t = a->len > 0 ? g_ptr_array_index(a, 0) : NULL;
    g_ptr_array_free(a, TRUE);
    return t;
}

/* bt_db_tasks_toplevel() — visible top-level tasks of a list.               */
GPtrArray *
bt_db_tasks_toplevel(BtDatabase *db, gint64 list_id)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE list_id = ? AND "
        "parent_id IS NULL AND deleted = 0 ORDER BY position, id",
        1, list_id, 0);
}

/* bt_db_subtasks() — visible subtasks of a task.                            */
GPtrArray *
bt_db_subtasks(BtDatabase *db, gint64 parent_id)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE parent_id = ? AND "
        "deleted = 0 ORDER BY position, id", 1, parent_id, 0);
}

/* bt_db_subtasks_all_visible() — every visible subtask, one query.          */
GPtrArray *
bt_db_subtasks_all_visible(BtDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE parent_id IS NOT NULL AND "
        "deleted = 0 ORDER BY parent_id, position, id", 0, 0, 0);
}

/* bt_db_tasks_pinned() — the Pinned Tasks meta list.                        */
GPtrArray *
bt_db_tasks_pinned(BtDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE pinned = 1 AND deleted = 0 "
        "ORDER BY list_id, position, id", 0, 0, 0);
}

/* bt_db_tasks_all_visible() — the All Tasks meta list (top-level tasks
 * of every list; their subtasks render inside the rows as usual).           */
GPtrArray *
bt_db_tasks_all_visible(BtDatabase *db)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE parent_id IS NULL AND "
        "deleted = 0 ORDER BY list_id, position, id", 0, 0, 0);
}

/* bt_db_tasks_due_between() — the Due Today / Due Tomorrow meta lists.      */
GPtrArray *
bt_db_tasks_due_between(BtDatabase *db, gint64 lo, gint64 hi)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE due >= ? AND due < ? AND "
        "deleted = 0 ORDER BY due, list_id, position", 2, lo, hi);
}

/* bt_db_tasks_in_list_all() — one list's rows incl. tombstones (sync),
 * parents before subtasks.                                                  */
GPtrArray *
bt_db_tasks_in_list_all(BtDatabase *db, gint64 list_id)
{
    return task_query(db,
        "SELECT " TASK_COLS " FROM tasks WHERE list_id = ? ORDER BY "
        "parent_id IS NOT NULL, position, id", 1, list_id, 0);
}

/* ---------------------------------------------------------------------------
 * bt_db_task_create() — append a task (see db.h).  One nesting level: a
 * non-zero parent must itself be a top-level, undeleted task in the same
 * list, or the insert is refused.
 * ------------------------------------------------------------------------- */
gint64
bt_db_task_create(BtDatabase *db, gint64 list_id, gint64 parent_id,
                  const gchar *title)
{
    if (parent_id != 0) {
        BtTask *p = bt_db_task_get(db, parent_id);
        gboolean ok = p != NULL && p->parent_id == 0 && !p->deleted &&
                      p->list_id == list_id;
        bt_task_free(p);
        if (!ok)
            return 0;
    }
    sqlite3_stmt *st = NULL;
    gint64 id = 0;                   /* the new rowid                       */
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO tasks(list_id, parent_id, title, position, "
            "updated_at) VALUES(?, ?, ?, (SELECT COALESCE(MAX(position), 0)"
            " + 1 FROM tasks WHERE list_id = ?1 AND "
            "COALESCE(parent_id, 0) = COALESCE(?2, 0)), ?)", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, list_id);
        if (parent_id != 0)
            sqlite3_bind_int64(st, 2, parent_id);
        else
            sqlite3_bind_null(st, 2);
        sqlite3_bind_text(st, 3, title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, now());
        if (step_done(db, st, "task create"))
            id = sqlite3_last_insert_rowid(db->sq);
    } else {
        step_done(db, NULL, "task create");
    }
    sqlite3_finalize(st);
    return id;
}

/* ---------------------------------------------------------------------------
 * bt_db_task_update() — write the editable fields back (see db.h).
 * ------------------------------------------------------------------------- */
void
bt_db_task_update(BtDatabase *db, const BtTask *t)
{
    /* completed_at follows done: stamped when done flips on (the CASE
     * reads the OLD row values), cleared when it flips off.                  */
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET title = ?1, notes = ?2, due = ?3, "
            "completed_at = CASE WHEN ?4 = 1 AND done = 0 THEN ?6 "
            "                    WHEN ?4 = 0 THEN 0 "
            "                    ELSE completed_at END, "
            "done = ?4, pinned = ?5, updated_at = ?6 WHERE id = ?7", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, t->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, t->notes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, t->due);
        sqlite3_bind_int(st, 4, t->done ? 1 : 0);
        sqlite3_bind_int(st, 5, t->pinned ? 1 : 0);
        sqlite3_bind_int64(st, 6, now());
        sqlite3_bind_int64(st, 7, t->id);
        step_done(db, st, "task update");
    } else {
        step_done(db, NULL, "task update");
    }
    sqlite3_finalize(st);
}

/* bt_db_task_set_done() — toggle done, stamping/clearing completed_at
 * and updated_at (see db.h).                                                */
void
bt_db_task_set_done(BtDatabase *db, gint64 id, gboolean done)
{
    gint64 ts = now();
    gchar *sql = g_strdup_printf(
        "UPDATE tasks SET completed_at = %lld, done = %d, "
        "updated_at = %lld WHERE id = %lld",
        done ? (long long)ts : 0LL, done ? 1 : 0,
        (long long)ts, (long long)id);
    exec(db, sql);
    g_free(sql);
}

/* bt_db_task_set_pinned() — toggle the local-only pin (see db.h).           */
void
bt_db_task_set_pinned(BtDatabase *db, gint64 id, gboolean pinned)
{
    gchar *sql = g_strdup_printf(
        "UPDATE tasks SET pinned = %d, updated_at = %lld WHERE id = %lld",
        pinned ? 1 : 0, (long long)now(), (long long)id);
    exec(db, sql);
    g_free(sql);
}

/* ---------------------------------------------------------------------------
 * bt_db_task_delete() — tombstone the task and its subtasks.
 * ------------------------------------------------------------------------- */
void
bt_db_task_delete(BtDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET deleted = 1, updated_at = %lld "
        "  WHERE parent_id = %lld;"
        "UPDATE tasks SET deleted = 1, updated_at = %lld WHERE id = %lld;",
        (long long)now(), (long long)id, (long long)now(), (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* ---------------------------------------------------------------------------
 * Attachments.
 * ------------------------------------------------------------------------- */
GPtrArray *
bt_db_attachments(BtDatabase *db, gint64 task_id)
{
    GPtrArray *out = g_ptr_array_new();
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "SELECT id, task_id, path FROM attachments WHERE task_id = ? "
            "ORDER BY added_at, id", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, task_id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            BtAttachment *a = g_new0(BtAttachment, 1);
            a->id      = sqlite3_column_int64(st, 0);
            a->task_id = sqlite3_column_int64(st, 1);
            a->path    = column_text_dup(st, 2);
            g_ptr_array_add(out, a);
        }
    }
    sqlite3_finalize(st);
    return out;
}

/* bt_db_attachment_add() — new attachment row; the new id, 0 on
 * failure (see db.h).                                                       */
gint64
bt_db_attachment_add(BtDatabase *db, gint64 task_id, const gchar *path)
{
    sqlite3_stmt *st = NULL;
    gint64 id = 0;                   /* the new rowid                       */
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO attachments(task_id, path, added_at) "
            "VALUES(?, ?, ?)", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, task_id);
        sqlite3_bind_text(st, 2, path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        if (step_done(db, st, "attachment add"))
            id = sqlite3_last_insert_rowid(db->sq);
    } else {
        step_done(db, NULL, "attachment add");
    }
    sqlite3_finalize(st);
    return id;
}

/* bt_db_attachment_remove() — drop one attachment row (see db.h).           */
void
bt_db_attachment_remove(BtDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "DELETE FROM attachments WHERE id = %lld", (long long)id);
    exec(db, sql);
    sqlite3_free(sql);
}

/* bt_db_attachment_counts() — task_id → count map, one query (see db.h).    */
GHashTable *
bt_db_attachment_counts(BtDatabase *db)
{
    GHashTable *map = g_hash_table_new(g_direct_hash, g_direct_equal);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "SELECT task_id, COUNT(*) FROM attachments GROUP BY task_id",
            -1, &st, NULL) == SQLITE_OK)
        while (sqlite3_step(st) == SQLITE_ROW)
            g_hash_table_insert(map,
                GINT_TO_POINTER((gint)sqlite3_column_int64(st, 0)),
                GINT_TO_POINTER(sqlite3_column_int(st, 1)));
    sqlite3_finalize(st);
    return map;
}

/* ---------------------------------------------------------------------------
 * Sync state + sync-side mutators (no updated_at stamp — see db.h).
 * ------------------------------------------------------------------------- */
gchar *
bt_db_state_get(BtDatabase *db, const gchar *key)
{
    sqlite3_stmt *st = NULL;
    gchar *val = NULL;               /* the fetched value                   */
    if (sqlite3_prepare_v2(db->sq,
            "SELECT value FROM sync_state WHERE key = ?", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW)
            val = column_text_dup(st, 0);
    }
    sqlite3_finalize(st);
    return val;
}

/* bt_db_state_set() — upsert one sync_state row (see db.h).                 */
void
bt_db_state_set(BtDatabase *db, const gchar *key, const gchar *value)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO sync_state(key, value) VALUES(?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value = excluded.value", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, value, -1, SQLITE_TRANSIENT);
        step_done(db, st, "sync state set");
    } else {
        step_done(db, NULL, "sync state set");
    }
    sqlite3_finalize(st);
}

/* set_gtasks_id() — shared body of the two id setters.                      */
static void
set_gtasks_id(BtDatabase *db, const gchar *table, gint64 id,
              const gchar *gid)
{
    gchar *sql = g_strdup_printf(
        "UPDATE %s SET gtasks_id = ? WHERE id = %lld",
        table, (long long)id);
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq, sql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, gid, -1, SQLITE_TRANSIENT);
        step_done(db, st, "gtasks id set");
    } else {
        step_done(db, NULL, "gtasks id set");
    }
    sqlite3_finalize(st);
    g_free(sql);
}

/* bt_db_list_set_gtasks_id() — bind a list to its Google id WITHOUT
 * stamping updated_at (see db.h).                                           */
void
bt_db_list_set_gtasks_id(BtDatabase *db, gint64 id, const gchar *gid)
{
    set_gtasks_id(db, "lists", id, gid);
}

/* bt_db_task_set_gtasks_id() — task variant of the above (see db.h).        */
void
bt_db_task_set_gtasks_id(BtDatabase *db, gint64 id, const gchar *gid)
{
    set_gtasks_id(db, "tasks", id, gid);
}

/* bt_db_list_apply_remote() — overwrite name with the remote's, stamping
 * the REMOTE updated time so the row is clean after the sync.               */
void
bt_db_list_apply_remote(BtDatabase *db, gint64 id, const gchar *name,
                        gint64 updated_at)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE lists SET name = ?, updated_at = ? WHERE id = ?", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, updated_at);
        sqlite3_bind_int64(st, 3, id);
        step_done(db, st, "list apply remote");
    } else {
        step_done(db, NULL, "list apply remote");
    }
    sqlite3_finalize(st);
}

/* bt_db_task_apply_remote() — overwrite the synced fields (title, notes,
 * due, done) plus the Google-mirror metadata (completed_at, etag,
 * web_link, glinks, assigned) from remote data; pinned is local-only
 * and untouched.                                                             */
void
bt_db_task_apply_remote(BtDatabase *db, const BtTask *t)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "UPDATE tasks SET title = ?, notes = ?, due = ?, done = ?, "
            "updated_at = ?, completed_at = ?, etag = ?, web_link = ?, "
            "glinks = ?, assigned = ? WHERE id = ?", -1, &st, NULL)
        == SQLITE_OK) {
        sqlite3_bind_text(st, 1, t->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, t->notes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, t->due);
        sqlite3_bind_int(st, 4, t->done ? 1 : 0);
        sqlite3_bind_int64(st, 5, t->updated_at);
        sqlite3_bind_int64(st, 6, t->completed_at);
        sqlite3_bind_text(st, 7, t->etag, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 8, t->web_link, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 9, t->glinks, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 10, t->assigned, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 11, t->id);
        step_done(db, st, "task apply remote");
    } else {
        step_done(db, NULL, "task apply remote");
    }
    sqlite3_finalize(st);
}

/* ---------------------------------------------------------------------------
 * bt_db_task_move_list() — cross-list move (see db.h).
 * ------------------------------------------------------------------------- */
void
bt_db_task_move_list(BtDatabase *db, gint64 id, gint64 dest_list)
{
    gchar *sql = sqlite3_mprintf(
        "UPDATE tasks SET list_id = %lld, updated_at = %lld, "
        "  position = (SELECT COALESCE(MAX(position), 0) + 1 FROM tasks "
        "              WHERE list_id = %lld AND parent_id IS NULL) "
        "  WHERE id = %lld;"
        "UPDATE tasks SET list_id = %lld, updated_at = %lld "
        "  WHERE parent_id = %lld;",
        (long long)dest_list, (long long)now(), (long long)dest_list,
        (long long)id,
        (long long)dest_list, (long long)now(), (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* ---------------------------------------------------------------------------
 * bt_db_purge_done() — remove a list's completed tasks (see db.h).
 * ------------------------------------------------------------------------- */
void
bt_db_purge_done(BtDatabase *db, gint64 list_id)
{
    gchar *sql = sqlite3_mprintf(
        "DELETE FROM tasks WHERE list_id = %lld AND parent_id IN "
        "  (SELECT id FROM tasks WHERE list_id = %lld AND done = 1);"
        "DELETE FROM tasks WHERE list_id = %lld AND done = 1;",
        (long long)list_id, (long long)list_id, (long long)list_id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* ---------------------------------------------------------------------------
 * bt_db_insert_remote_tombstone() — offline-move stub (see db.h).
 * ------------------------------------------------------------------------- */
void
bt_db_insert_remote_tombstone(BtDatabase *db, gint64 list_id,
                              const gchar *gtasks_id)
{
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db->sq,
            "INSERT INTO tasks(list_id, title, deleted, gtasks_id, "
            "updated_at) VALUES(?, '', 1, ?, ?)", -1,
            &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, list_id);
        sqlite3_bind_text(st, 2, gtasks_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, now());
        step_done(db, st, "remote tombstone insert");
    } else {
        step_done(db, NULL, "remote tombstone insert");
    }
    sqlite3_finalize(st);
}

/* bt_db_list_purge() — physically delete a list row + all its tasks'
 * rows (attachments cascade).                                               */
void
bt_db_list_purge(BtDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "DELETE FROM tasks WHERE list_id = %lld AND parent_id IS NOT NULL;"
        "DELETE FROM tasks WHERE list_id = %lld;"
        "DELETE FROM lists WHERE id = %lld;",
        (long long)id, (long long)id, (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}

/* bt_db_task_purge() — physically delete a task row + its subtasks.         */
void
bt_db_task_purge(BtDatabase *db, gint64 id)
{
    gchar *sql = sqlite3_mprintf(
        "DELETE FROM tasks WHERE parent_id = %lld;"
        "DELETE FROM tasks WHERE id = %lld;",
        (long long)id, (long long)id);
    exec_txn(db, sql);
    sqlite3_free(sql);
}
