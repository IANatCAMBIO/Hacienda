# Blue Tasks

A task-list desktop app in **plain C + GTK3 + SQLite** — the companion
app to [Blue Notes](../orange_notes/).  Same design language: plain
`GtkWindow` titlebars ("Blue Tasks - <thing>"), a sidebar + content
library window, one editor window per item.

![layout](#) <!-- run `make run` and see for yourself -->

## Features

- **Lists** in the sidebar tree view, nested under a collapsible
  **Lists** section (lists themselves cannot nest), plus three
  **virtual lists** aggregated across every real list:
  - 📌 **Pinned Tasks** — anything with the pinned flag
  - **Due Today** / **Due Tomorrow** — by due date, rolling over at
    local midnight
- **Tall task rows** in the list view: bold title, notes preview,
  attachment count, and up to four subtask lines with their own
  checkboxes rendered inline.  Columns: done checkbox, task, due date
  (urgency-tinted: overdue red / today gold / ahead green, sortable
  with undated rows last), pinned checkbox.
- **Double-click a task** to open its editor window: title, done,
  pinned, due date (typed ISO date or a calendar picker), notes, file
  attachments (references — add/remove/open), and subtasks
  (add/remove/rename/toggle).  **Subtasks cannot have subtasks** — one
  level only, enforced in the schema layer.
- **Toolbar**: New List, Delete List | New Task, Delete Task | Sync.
- Local storage in SQLite:
  `~/.local/share/blue_tasks/blue_tasks.db` (GLib user-data dir).
- **Two-way Google Tasks sync** (see below).

## Build

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make          # builds ./blue_tasks
make run
```

Dependencies (MacPorts): `gtk3 +quartz`, `sqlite3`, `curl`, `pkgconf`.
On Debian/Ubuntu: `libgtk-3-dev libsqlite3-dev libcurl4-openssl-dev`.

## Google Tasks sync

Two-way sync against the Google Tasks REST API.  Google's data model
maps cleanly: tasklists ↔ lists, tasks ↔ tasks, `parent` ↔ subtask
(the API supports exactly the one nesting level Blue Tasks allows),
`due` ↔ due date, `status` ↔ done.  **Pinned and attachments are
local-only** — Google Tasks has no equivalent, so they never leave the
machine.

Setup (once): in the Google Cloud console, enable the **Google Tasks
API** and create an OAuth client of type **Desktop app**; enter its
client id/secret in **File → Settings…** (stored in `blue_tasks.ini` —
they identify the app to Google and grant nothing by themselves).

Alternatively, a distributor can bake a default client into the binary
so end users skip that step: copy `client_credentials.mk.example` to
`client_credentials.mk` (gitignored) and rebuild.  Settings values
override the built-ins.

Sign in **once**: the Sync button (or Settings → Sign In) opens your
browser for Google's consent page and the app receives its tokens over
a localhost redirect (RFC 8252 + PKCE).  The refresh token is stored in
`blue_tasks.ini` (`gtasks_refresh_token`, scoped to Google Tasks only)
and silently exchanged for fresh access tokens as they expire, so the
browser never reappears unless you Sign Out or revoke the grant at
myaccount.google.com/permissions.

Sync mechanics: every row carries its Google id, etag and an
`updated_at` stamp; deletes are tombstones until pushed.  After the
first full pass syncs run **incrementally** (`updatedMin` — only items
changed since the last pass transfer).  Pushes are etag-guarded
(`If-Match`; a 412 defers to the remote copy).  Conflicts resolve
newest-wins per item; deletions win over concurrent edits.  A periodic
auto-sync runs while signed in (`sync_interval_min`, default 5 minutes,
0 = off).  The sync runs on a worker thread with its own SQLite
connection; the GUI stays live throughout.

Beyond field sync, the full API surface is wired up: right-click a task
for **Open in Google Tasks** (`webViewLink`) and **Move to List**
(server-side `tasks.move` with `destinationTasklist`, with an offline
delete-and-recreate fallback); File → **Clear Completed Tasks** uses
`tasks.clear`; and the editor's read-only **From Google** section shows
the completion time, Docs/Chat assignment origin (`assignmentInfo`),
and any Google-attached `links[]` (e.g. the Gmail message a task was
created from).

## Configuration

`blue_tasks.ini` next to the binary (portable mode), falling back to
`~/.config/blue_tasks/blue_tasks.ini` when that directory is not
writable.  Seeded from `blue_tasks.ini.defaults` on first launch.  All
keys are editable in-app via File → Settings…:
`google_client_id`, `google_client_secret`, `sync_interval_min`.

## File map

| File | Purpose |
|---|---|
| `src/main.c` | GtkApplication entry; config + db init |
| `src/app.[ch]` | Shared `BtApp` context, dialogs, ini config, date helpers |
| `src/db.[ch]` | SQLite schema + CRUD; tombstones + `updated_at` for sync |
| `src/library_window.[ch]` | Sidebar (virtual + real lists), tall task rows, toolbar, status bar |
| `src/editor_window.[ch]` | Per-task editor: fields, attachments, subtasks; debounced write-through saves |
| `src/settings_window.[ch]` | Google sync credentials, sign in/out, auto-sync interval |
| `src/oauth.[ch]` | OAuth 2.0 installed-app flow (PKCE, loopback redirect, session-only token) |
| `src/gtasks.[ch]` | Two-way sync engine (worker thread, own db connection) |
| `src/http.[ch]` | Small libcurl wrapper |
| `src/json.[ch]` | Minimal JSON parser/escaper (no external JSON dependency) |
