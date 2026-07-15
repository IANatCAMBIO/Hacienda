# Blue Tasks — project guide

Task-list app in **plain C + GTK3 + SQLite**, the companion app to Blue
Notes (`~/salt_development/orange_notes` — NOT `../orange_notes`).  Two
window types: a Library (lists sidebar + tall task rows) and one editor
window per task.  Two-way Google Tasks sync.  No GNOME HeaderBars
anywhere — plain `GtkWindow` titlebars, formatted `"Blue Tasks - <thing>"`.

## Build & run

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make          # builds ./blue_tasks  (-Wall -Wextra must stay clean)
make run
```

Dependencies (MacPorts): `gtk3 +quartz`, `sqlite3`, `curl`, `pkgconf`,
optionally `gtk-osx-application-gtk3` (native macOS menubar — pkg-config
module is **`gtk-mac-integration-gtk3`**; the Makefile auto-detects it
and defines `HAVE_GTKOSX`).  After toggling a dependency run
`make clean && make`.

Launch for testing: `pkill -f './blue_tasks'; nohup ./blue_tasks
>/tmp/bt_launch.log 2>&1 &` then `screencapture -x` for screenshots.
Do NOT drive the GUI with osascript accessibility clicks (rejected by
the user).  A logic test harness lives in the session scratchpad
(`test_bt.c`, links against `build/{json,db,app}.o`) — keep it passing.

## File map

| File | Purpose |
|---|---|
| `src/main.c` | GtkApplication entry; config → curl_global_init → db → oauth snapshot → window → auto-sync; icon-theme path for HiDPI expanders |
| `src/app.[ch]` | Shared `BtApp` context; ini config; dialogs; toolbar style system (icons/both/text + right-click menu); HiDPI icon loader; CSS helper; date helpers |
| `src/db.[ch]` | SQLite schema (user_version 3) + CRUD; tombstones + `updated_at` for sync; `step_done`/`exec_txn` error discipline |
| `src/library_window.[ch]` | Sidebar (virtual lists + collapsible Lists section), tall task rows, toolbar, mini +/✎/− bar, multi-select context menu, status bar |
| `src/editor_window.[ch]` | Per-task editor (debounced write-through saves); reduced Blue Notes variant; read-only "From Google" section |
| `src/settings_window.[ch]` | Singleton settings: Google OAuth client, sign in/out, auto-sync interval, Blue Notes integration, toolbar style, native menubar |
| `src/oauth.[ch]` | OAuth 2.0 installed-app flow: PKCE + loopback listener; refresh token in ini; access tokens in memory |
| `src/gtasks.[ch]` | Two-way sync engine + move/clear worker jobs |
| `src/bnotes.[ch]` | Blue Notes integration — CLI ONLY, never its database |
| `src/http.[ch]` | libcurl wrapper (blocking; worker threads only) |
| `src/json.[ch]` | Minimal JSON parser/serializer (no external JSON dep) |
| `icons/Selected/` | Curated toolbar PNGs (icon names are `icons/`-relative paths, case-exact for Linux) |
| `icons/theme/hicolor/` | Bundled SVG `pan-*-symbolic` arrows → crisp HiDPI tree expanders (needs librsvg loader) |

## Conventions

- `bt_` prefix for public symbols, `Bt` for types; every function gets a
  banner comment.  **Headers carry the full public contract** (purpose,
  params, returns, ownership, failure behavior); `.c` banners say "see
  x.h" plus the how.  Non-obvious variables get column-aligned trailing
  comments; ~78-col lines.  UTF-8 escapes (`\xe2\x80\xa6`) for …/—/✓ in
  source strings.
- Config: `blue_tasks.ini` NEXT TO THE BINARY (portable mode), fallback
  `~/.config/blue_tasks/` when unwritable; seeded from
  `blue_tasks.ini.defaults`; loaded ONCE, written through on change,
  never re-read.  All settings editable in File → Settings….
- **Error discipline**: every prepared WRITE goes through `step_done()`
  (logs sqlite's message on prepare/step failure — silent write loss is
  the unacceptable outcome); multi-statement writes go through
  `exec_txn()` (BEGIN IMMEDIATE + ROLLBACK on failure — a bare
  `BEGIN;…;COMMIT` via sqlite3_exec wedges the connection in an open
  transaction on SQLITE_BUSY).  Create failures (id 0) must surface a
  status-bar message at the call site.
- Notify hooks on BtApp: `notify_changed` = FULL refresh (sidebar +
  tasks + reload all editors) for structural changes; `notify_tasks` =
  task pane only — editor saves and subtask/attachment edits use this
  (the full path would re-run the Blue Notes CLI per autosave).
  `bt_app_status()` for events.  Teardown: NULL the hooks BEFORE
  `bt_editor_close_all` (a closing editor's flush otherwise cascades
  refreshes into destroyed windows).
- Async callback lifetime: never capture the BtLibrary pointer in a
  worker/idle callback — re-resolve via `lib_of(app)` and no-op when
  NULL (the window may close mid-flight).  The settings window guards
  the same way (`settings != sw`).

## GUI rules (visual parity with Blue Notes)

- Toolbar: `GTK_ICON_SIZE_SMALL_TOOLBAR` metrics; buttons via
  `bt_app_tool_item_new` (local PNG at 24 px logical, Pango-markup glyph
  fallback); registered with `bt_app_register_toolbar` so the
  icons/both/text style applies live (Settings combo + right-click
  radio menu).  Task buttons sit at the RIGHT end behind an invisible
  expanding separator; Sync stays on the toolbar.
- Thin `gtk_separator_new` rules under the toolbar and above the status
  bar.  Status bar: margins 8/8/3/3 (NOT border_width) and
  `label { font-size: 85%; }` on both labels — measured pixel-identical
  to Blue Notes.
- Task list: alternating white/`ROW_TINT` (#e8f2fb) stripes via a
  cell-background data func on EVERY column's renderer (the Due column's
  func does stripe + urgency tint in one, since a renderer gets one data
  func).  Dimmed markup uses Pango `alpha`, NEVER a fixed gray —
  hardcoded grays are unreadable on the blue selection.  Due tint:
  overdue #c01c28, today #d19a00, ahead #26a269, computed at draw time.
- Sidebar: gray backdrop CSS (rgb 230,230,230 / text 65,65,65 /
  selection rgb 86,131,224 white); meta rows bold (Pinned, All Tasks,
  Due Today, Due Tomorrow); real lists nest under a collapsible bold
  "Lists" header whose expansion is SNAPSHOTTED before every rebuild
  (first population expands; force-open when the selection lives
  inside).  List labels: `emoji + two spaces + name` when an emoji is
  set.  Mini action bar at the bottom: flat compact + ✎ − buttons,
  right-aligned.  Sidebar starts HIDDEN by default
  (`sidebar_visible`, write-through on toggle).
- Window size: tracked via configure-event, persisted as `win_w/win_h`
  on clean close, restored at launch (980×640 fallback).
- Model rebuilds: capture the scrolled window's vadjustment and restore
  it idle-deferred (`scroll_keep_queue`) — clearing a store zeroes the
  scrollbar.
- Context menus built per popup MUST self-destroy via
  `g_signal_connect(menu, "selection-done", gtk_widget_destroy)` —
  attached menus otherwise live until the widget dies (fires after the
  chosen item's activate, so it is safe).
- Task view is `GTK_SELECTION_MULTIPLE`; right-click INSIDE an existing
  selection keeps it, outside collapses to the clicked row; context
  actions (mark complete/incomplete, move, delete) apply to the whole
  selection; "Open in Google Tasks" is single-row only.  Bulk data
  rides on menu items as `g_array_ref`'d id arrays with destroy
  notifies.
- Editor: 600 ms debounced write-through saves; done/pinned save
  immediately.  NEVER rewrite the due entry while it has focus, and a
  save must not clobber the stored date when the entry holds partial/
  invalid text (`editor_due_entry_parse`).  Editors are singletons per
  task (`app->editors` gint64 keys) / per Blue Notes ref
  (`app->bn_editors` string keys).
- Emoji picking: a bare 18 px single-char entry; click clears it and
  emits `insert-emoji` (GTK stashes its chooser on the entry as
  `"gtk-emoji-chooser"` object data).  GTK3 popovers render INSIDE
  their toplevel — the dialog grows to 440×470 while the chooser is
  open and shrinks back on its "closed" signal.

## Sync architecture (Google Tasks)

- Worker thread with its OWN SQLite connection (a connection never
  crosses threads); status/completion marshalled with g_idle_add;
  `curl_global_init` happens in main() BEFORE any thread exists.
- Identity: rows carry `gtasks_id` + `etag` + `updated_at`; deletes are
  tombstones until pushed, then purged.  `sync_state.last_sync` = the
  START time of the last successful pass.
- Incremental: after the first full pass, task fetches use
  `updatedMin = last_sync - 300` (overlap for clock skew) with
  showDeleted/showCompleted/showHidden.  CRITICAL: with a partial
  listing, "absent" means UNCHANGED (push if locally dirty), never
  deleted — deletions arrive as `deleted:true` items.  First-sync
  title dedup only runs against a full listing.
- NON-DESTRUCTIVE by default: ABSENCE NEVER DELETES on either side.
  Only explicit deletes propagate — a local tombstone DELETEs
  remotely; a remote `deleted:true` purges locally.  A local task
  whose gtasks_id is missing from a FULL listing (deleted on Google
  with no local tombstone) drops its stale Google identity and is
  pushed back as a NEW remote task; a local list whose bound remote
  list vanished is re-created remotely and ALL its tasks' gtasks_ids/
  etags are cleared (`bt_db_tasks_clear_gtasks_ids`) so the task pass
  re-pushes them.  Remote items unknown locally are pulled (created)
  as before.
- Pushes: creates POST (parents before subtasks — the per-list query
  orders `parent_id IS NOT NULL` last); edits PATCH with `If-Match:
  etag`, and a 412 SKIPS the push (remote wins; next pull reconciles).
  Conflicts otherwise resolve newest-wins; deletion beats concurrent
  edit.  Replies stamp rows clean (remote updated + fresh etag).
- LOCAL-ONLY fields (never sent): `pinned`, list `emoji`, attachments.
  The API has NO starring and `due` is DATE-ONLY (time is documented as
  discarded and unreadable) — both confirmed against the docs; don't
  re-attempt.
- `hidden` remote tasks (completed + cleared) are never re-created
  locally — that's what keeps Clear Completed from resurrecting rows.
- Cross-list move: `tasks.move` + `destinationTasklist` on a worker
  (children moved under the parent afterwards); offline/failure
  fallback = stub tombstone in the source list + strip gtasks_ids so
  the rows push as new.  Clear Completed: `tasks.clear` + local purge
  when synced/signed-in, tombstone deletes otherwise.
- OAuth: installed-app flow, PKCE (GLib SHA-256), loopback
  GSocketService on an ephemeral port, `access_type=offline` +
  `prompt=consent`; refresh token persisted in the ini
  (`gtasks_refresh_token`), access tokens in memory only.  Client
  id/secret are entered in Settings (ini) with an optional baked-in
  default via gitignored `client_credentials.mk`.  Races handled: a
  sign-out mustn't be resurrected by an in-flight refresh (re-check
  `cred_refresh_token` before caching); a timed-out flow discards a
  late exchange result.  The consent screen's app name comes from the
  Google Cloud OAuth registration, not from this app.

## Blue Notes integration

- ALL access via the `blue_notes` CLI (`action list/done/undone/due`),
  NEVER its database file — Blue Notes' GUI/CLI coexistence is a
  single-writer design (CLI routes through the running GUI's socket).
  Row format: `NOTEID:ORD \t [x]|[ ] \t YYYY-MM-DD|- \t text`.
- Shown as "Action Items (from Blue Notes)" under Lists while
  `blue_notes_sync=1`; not deletable/editable as a list; items open the
  reduced editor (done + due writable via CLI; title/notes/subtasks/
  attachments insensitive — no CLI verbs for them).  PINNING works:
  it is local-only state in the `bn_pins` table keyed by the item's
  "NOTEID:ORD" ref (Blue Notes has no pin concept); pinned action
  items also appear in the Pinned Tasks view (`append_bn_rows` with
  only_pinned).  BN rows are detected by TL_REF != NULL (id 0), NOT
  by the selected sidebar kind — they can appear in Pinned too.  The Blue
  Notes editor tracks loaded done/due and only shells the CLI for
  fields that actually changed.  The Settings CLI-path entry persists
  per keystroke but only refreshes on Enter/focus-out (a per-keystroke
  refresh would spawn the half-typed command).

## Hard-won gotchas (do not re-learn)

1. GTK3 popovers cannot escape their toplevel window — size the window,
   don't fight the popover (emoji chooser).
2. `g_clear_pointer` is a statement-style macro; it cannot sit inside
   an expression.
3. A `GtkMenu` built per right-click and attached to a widget leaks
   until that widget dies unless destroyed on "selection-done".
4. blue_notes gotcha inherited: clearing a tree/list store zeroes the
   view's scrollbar (restore idle-deferred) and collapses expansion
   state (snapshot before clear).
5. `gtk_tree_view_set_enable_search(view, FALSE)` on every tree view —
   the auto search column is the int64 id and matches nothing.
6. Status-bar height parity needs margins (8/8/3/3), not
   `border_width` — border pads every edge.
7. libcurl's implicit global init is not thread-safe; init in main().
8. sqlite `UPDATE` SET expressions read the OLD row values — the
   `completed_at CASE WHEN ?done=1 AND done=0` transition relies on it.
9. Toolbar right-click style menu fires on EMPTY toolbar area only
   ("popup-context-menu").
10. macOS AX geometry (osascript) reports frame incl. titlebar;
    `gtk_window_get_size` is the client area (~28 pt difference).
11. The live ini rewrites drop comments and carry per-machine values —
    it stays gitignored; document defaults in `blue_tasks.ini.defaults`.
12. Google's DEFAULT tasklist cannot be deleted — `tasklists.delete`
    returns 400 "Invalid Value" from any client, and an unhandled
    failure there aborts the whole sync pass (blocking every later
    push).  Handled ACCURATELY (no hidden state): every sync GETs
    `…/lists/@default` and stores its id as
    `sync_state.default_list_gid`; on_delete_list refuses that list up
    front (like the Blue Notes list), and if a stale tombstone for it
    exists anyway, sync_lists RESTORES the list + its same-moment task
    tombstones (`bt_db_list_restore`) instead of deleting or hiding
    anything.
