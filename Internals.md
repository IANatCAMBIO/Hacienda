# Hacienda — Internals

How Hacienda is put together: the source layout, the database schema,
and the sync engine. For everyday use see the
[User Guide](User_Guide.md); for build instructions see the
[README](README.md).

## Code layout

| File                       | Purpose                                            |
|----------------------------|----------------------------------------------------|
| `src/main.c`               | GtkApplication entry point; config, database and OAuth init, auto-sync timer |
| `src/app.[ch]`             | Shared `BtApp` context: ini config, dialogs, toolbar styles, icon loading, date helpers |
| `src/db.[ch]`              | SQLite layer: lists, tasks, subtasks, attachments; tombstones and `updated_at` for sync |
| `src/library_window.[ch]`  | Sidebar, tall task rows, Weekly Forecast panel, toolbar, context menus, status bar |
| `src/editor_window.[ch]`   | Per-task editor (and the reduced Blue Notes variant); debounced write-through saves |
| `src/settings_window.[ch]` | The Settings window                                |
| `src/oauth.[ch]`           | OAuth 2.0 installed-app flow: PKCE, loopback redirect |
| `src/gtasks.[ch]`          | Two-way Google Tasks sync engine + move/clear jobs |
| `src/bnotes.[ch]`          | Blue Notes integration (via its CLI, never its database) |
| `src/http.[ch]`            | Small libcurl wrapper (blocking; worker threads only) |
| `src/json.[ch]`            | Minimal JSON parser/serializer (no external JSON dependency) |
| `icons/`                   | Bundled PNG toolbar icons + app logo               |

## Database format

Everything lives in one ordinary SQLite file (see *Storage* in the
[User Guide](User_Guide.md)), so any standard SQLite tool can read it:

```sql
CREATE TABLE lists (
  id         INTEGER PRIMARY KEY,
  name       TEXT    NOT NULL DEFAULT '',
  emoji      TEXT    NOT NULL DEFAULT '',   -- local-only, never synced
  position   INTEGER NOT NULL DEFAULT 0,    -- local-only display order
  gtasks_id  TEXT,                          -- bound Google tasklist
  updated_at INTEGER NOT NULL DEFAULT 0,    -- UNIX seconds
  deleted    INTEGER NOT NULL DEFAULT 0     -- tombstone until pushed
);

CREATE TABLE tasks (
  id           INTEGER PRIMARY KEY,
  list_id      INTEGER NOT NULL REFERENCES lists(id),
  parent_id    INTEGER REFERENCES tasks(id),  -- NULL = top level; one
                                              -- level only (no sub-subtasks)
  title        TEXT    NOT NULL DEFAULT '',
  notes        TEXT    NOT NULL DEFAULT '',
  due          INTEGER NOT NULL DEFAULT 0,    -- UNIX local midnight; 0 = none
  done         INTEGER NOT NULL DEFAULT 0,
  pinned       INTEGER NOT NULL DEFAULT 0,    -- local-only, never synced
  priority     INTEGER NOT NULL DEFAULT 0,    -- high-priority flag;
                                              -- local-only, never synced
  position     INTEGER NOT NULL DEFAULT 0,
  gtasks_id    TEXT,                          -- bound Google task
  updated_at   INTEGER NOT NULL DEFAULT 0,    -- UNIX seconds
  deleted      INTEGER NOT NULL DEFAULT 0,    -- tombstone until pushed
  completed_at INTEGER NOT NULL DEFAULT 0,
  etag         TEXT,                          -- push guard (If-Match)
  web_link     TEXT,                          -- Google mirror fields...
  glinks       TEXT,                          --   links[] as JSON
  assigned     TEXT                           --   assignmentInfo origin
);

CREATE TABLE attachments (
  id         INTEGER PRIMARY KEY,
  task_id    INTEGER NOT NULL REFERENCES tasks(id) ON DELETE CASCADE,
  path       TEXT    NOT NULL,               -- a reference, not a copy
  added_at   INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE sync_state  (key TEXT PRIMARY KEY, value TEXT);
CREATE TABLE bn_pins     (ref TEXT PRIMARY KEY); -- pinned Blue Notes items
CREATE TABLE bn_priority (ref TEXT PRIMARY KEY); -- high-priority BN items

CREATE INDEX idx_tasks_list ON tasks(list_id, parent_id, position);
```

The schema version rides in `PRAGMA user_version` (currently 4);
older files are migrated in place at open.

Semantics worth knowing when querying directly:

- **Tombstones**: `deleted = 1` rows are pending remote deletes — the
  app hides them and purges them once the delete is pushed (or
  immediately when the row never synced). Filter them out of any
  direct query.
- `due` is midnight *local time* on the due day, as UNIX seconds;
  0 means no due date. Google's side is date-only, so this loses
  nothing in sync.
- `gtasks_id`, `etag` and `updated_at` are the sync identity: a row
  with a NULL `gtasks_id` has never been pushed; `updated_at` newer
  than the remote copy means locally dirty.
- `position` (both tables), `lists.emoji`, `tasks.pinned` and
  `tasks.priority` are local-only — Google Tasks has neither pinning
  nor priority, so none of them ever sync.
  `tasks.web_link`, `glinks` and `assigned` are read-only mirrors of
  Google fields, shown in the editor's From Google section.
- `sync_state` is a key/value scratchpad: `last_sync` (start time of
  the last successful pass), `default_list_gid` (Google's undeletable
  default tasklist), `lists_custom_order` (set once the user
  drag-reorders lists).
- `bn_pins` and `bn_priority` keys are Blue Notes `NOTEID:ORD` refs —
  pinning and the high-priority flag for action items live entirely
  on this side (Blue Notes knows neither concept).

Two practical cautions: the app sets a 5-second busy timeout (the GUI
and the sync worker share the file), so brief external readers coexist
fine, but long write transactions from other tools will stall it; and
prefer backing up while the app is closed — a copy taken mid-sync can
catch a transaction in flight.

## Sync engine

The design goal is to be **non-destructive by default**: absence on
one side never deletes on the other; only explicit deletes propagate.

- Sync runs on a worker thread with its **own SQLite connection** (a
  connection never crosses threads); progress and completion are
  marshalled back to the main loop. `curl_global_init` happens in
  `main()` before any thread exists — libcurl's implicit init is not
  thread-safe.
- After the first full pass, task fetches are incremental
  (`updatedMin = last_sync − 300` — the overlap absorbs clock skew)
  with deleted/completed/hidden items included. With a partial
  listing, an absent item means *unchanged*, never deleted — remote
  deletions arrive as explicit `deleted: true` items.
- Local tombstones DELETE remotely, then purge. A local task whose
  `gtasks_id` is missing from a **full** listing (deleted on Google
  with no local tombstone) drops its stale identity and is pushed
  back as a new remote task; a local list whose remote list vanished
  is re-created and all its tasks re-pushed.
- Pushes: creates POST (parents before subtasks), edits PATCH with
  `If-Match: etag` — a 412 skips the push (remote wins; the next pull
  reconciles). Otherwise conflicts resolve newest-wins per item, and
  a deletion beats a concurrent edit. Every push reply stamps the row
  clean (fresh etag, remote update time).
- `hidden` remote tasks (completed and cleared) are never re-created
  locally — that keeps Clear Completed from resurrecting rows.
- Cross-list moves use `tasks.move` with `destinationTasklist` on a
  worker job, children re-parented afterwards; when offline the
  fallback is a tombstone in the source list plus stripped ids so the
  rows push as new. Clear Completed uses `tasks.clear` when signed
  in, tombstone deletes otherwise.
- Google's default tasklist cannot be deleted by any client (their
  API returns 400). Every sync stores its id in `sync_state`, the
  delete action refuses it up front, and a stale tombstone for it is
  restored rather than retried forever.

## OAuth

Installed-app flow per RFC 8252: PKCE (S256, GLib SHA-256), a
loopback `GSocketService` on an ephemeral port for the redirect, and
`access_type=offline` for a refresh token. The client credentials
resolve in order: a `client_secret….json` next to the binary (or in
the user config dir) → legacy `google_client_id`/`google_client_secret`
ini keys → a baked-in default from `client_credentials.mk`. The
refresh token persists in `hacienda.ini` (`gtasks_refresh_token`);
access tokens live in memory only. The redirect listener redeems the
authorization code exactly once — browsers sometimes replay the
redirect GET, and a second exchange would revoke the first grant.
