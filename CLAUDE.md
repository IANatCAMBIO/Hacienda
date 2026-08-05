# Lists — project guide

Task-list app in **plain C + GTK3 + SQLite**, the companion app to Blue
Notes (`~/salt_development/orange_notes` — NOT `../orange_notes`).  Two
window types: a Library (lists sidebar + tall task rows) and one editor
window per task.  Two-way Google Tasks sync.  No GNOME HeaderBars
anywhere — plain `GtkWindow` titlebars, formatted `"Lists - <thing>"`.

## Build & run

```sh
export PATH=/opt/local/bin:$PATH   # MacPorts pkg-config
make          # builds ./lists  (-Wall -Wextra must stay clean)
make run
```

Dependencies (MacPorts): `gtk3 +quartz`, `sqlite3`, `curl`, `pkgconf`,
optionally `gtk-osx-application-gtk3` (native macOS menubar — pkg-config
module is **`gtk-mac-integration-gtk3`**; the Makefile auto-detects it
and defines `HAVE_GTKOSX`).  After toggling a dependency run
`make clean && make`.

Launch for testing: `pkill -f './lists'; nohup ./lists
>/tmp/bt_launch.log 2>&1 &` then `screencapture -x` for screenshots.
Do NOT drive the GUI with osascript accessibility clicks (rejected by
the user).  A logic test harness lives in the session scratchpad
(`test_bt.c`, links against `build/{json,db,app}.o`) — keep it passing.

## File map

| File | Purpose |
|---|---|
| `src/main.c` | GtkApplication entry; config → curl_global_init → db → oauth snapshot → window → auto-sync; icon-theme path for HiDPI expanders |
| `src/app.[ch]` | Shared `BtApp` context; ini config; dialogs; toolbar style system (icons/both/text + right-click menu); HiDPI icon loader; CSS helper; date helpers |
| `src/db.[ch]` | SQLite schema (user_version 5) + CRUD; tombstones + `updated_at` for sync; `step_done`/`exec_txn` error discipline |
| `src/library_window.[ch]` | Sidebar (virtual lists + collapsible Lists section with list groups), tall task rows, toolbar, multi-select context menu, status bar |
| `src/editor_window.[ch]` | Per-task editor (debounced write-through saves); reduced Blue Notes variant; read-only "From Google" section |
| `src/settings_window.[ch]` | Singleton settings: sync master switch, sign in/out, auto-sync interval, Blue Notes integration, toolbar style, native menubar |
| `src/oauth.[ch]` | OAuth 2.0 installed-app flow: PKCE + loopback listener; refresh token in ini; access tokens in memory |
| `src/gtasks.[ch]` | Two-way sync engine + move/clear worker jobs |
| `src/bnotes.[ch]` | Blue Notes integration — CLI ONLY, never its database |
| `src/http.[ch]` | libcurl wrapper (blocking; worker threads only) |
| `src/json.[ch]` | Minimal JSON parser/serializer (no external JSON dep) |
| `icons/` | Curated toolbar images directly in icons/ (icon names are extension-less basenames — the loader tries `.png` then `.svg`, case-exact for Linux; spares live in `icons/Unused/`) |
| `icons/theme/hicolor/` | Bundled SVG `pan-*-symbolic` arrows → crisp HiDPI tree expanders (needs librsvg loader) |

## Conventions

- `bt_` prefix for public symbols, `Bt` for types; every function gets a
  banner comment.  **Headers carry the full public contract** (purpose,
  params, returns, ownership, failure behavior); `.c` banners say "see
  x.h" plus the how.  Non-obvious variables get column-aligned trailing
  comments; ~78-col lines.  UTF-8 escapes (`\xe2\x80\xa6`) for …/—/✓ in
  source strings.
- Config: `lists.ini` NEXT TO THE BINARY (portable mode), fallback
  `~/.config/lists/` when unwritable; seeded from
  `lists.ini.defaults`; loaded ONCE, written through on change,
  never re-read.  Everything except the OAuth client keys and the
  window geometry is editable in File → Settings….
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
  radio menu).  Layout (all left-packed): the Sidebar toggle, a drawn
  divider, Sync, the completed-visibility toggle, the Manual Sort
  toggle, a second divider, then New Task and Delete Task.
  The completed-visibility toggle (`show_completed`, default 1) shows
  hidden.png while completed tasks are visible and visible.png while
  hidden (the icon names the ACTION), swapped live via
  `hide_done_icon_refresh`; the filter applies to every task-pane view
  including Blue Notes rows.  The Manual Sort toggle
  (`task_list_manual_sort`, default 0) enables drag-reorder of the task
  pane: a ⠿ drag-handle column (26 px wide, `cdrag`) appears; order is
  persisted per view in config keys `manual_order_list_<id>`,
  `manual_order_all`, `manual_order_pinned`, `manual_order_today`,
  `manual_order_bn_actions`.  A drag-lock (`drag_lock_ref`) prevents
  rapid row flicker at boundaries.  Far right (after an expanding blank
  separator): the About button — document.png logo in every style
  except text-only, which swaps in an "About" label
  (`about_button_fit_style` on "style-changed"); it opens the Blue
  Notes-style about dialog (`on_menu_about`: HiDPI logo, compile
  stamp, `bt_db_totals` vitals), shared with File → About Lists.
  document.png is also the .app bundle icon (Makefile `app` target).
- View menu: Show Completed, Manual Sort, then Show Sidebar and Compact
  Layout.  Show Sidebar is the menu twin of the toolbar Sidebar button —
  both route through `sidebar_set_visible` (write-through
  `sidebar_visible` + `sidebar_menu_sync`, which blocks the item's
  handler around its own set_active, like `hide_done_icon_refresh`).
  **Compact Layout** (`compact_layout`, default 0) hides the whole
  toolbar and its rule plus the sidebar, and shows `float_bar` instead:
  a two-button pill (New Task + Delete Task, icons only, never
  registered with `bt_app_register_toolbar`) added as a `GtkOverlay`
  child over the paned with halign/valign END and 20 px end/bottom
  margins.  The overlay wraps the PANED, not the whole vbox, so the
  float never covers the status bar's event messages.
  `compact_layout_apply` is the single applier (also called after the
  construction-time `show_all`, where it replaces the old
  `sidebar_visible` hide); it uses `gtk_widget_show`, never `show_all`,
  on the toolbar so a hidden Sync button stays hidden.  Compact does NOT
  touch `sidebar_visible`, so leaving it restores the pane as the user
  left it — and Show Sidebar still works while compact (explicit
  override).
- Thin `gtk_separator_new` rules under the toolbar and above the status
  bar.  Status bar: margins 8/8/3/3 (NOT border_width) and
  `label { font-size: 85%; }` on both labels — measured pixel-identical
  to Blue Notes.  Event messages posted via `bt_app_status()` hold for
  3 s then fade out over 1 s (20 × 50 ms alpha steps via Pango markup);
  a new message resets the timer.
- Task list: alternating white/`ROW_TINT` (#e8f2fb) stripes via a
  cell-background data func on EVERY column's renderer (the Due column's
  func does stripe + urgency tint in one, since a renderer gets one data
  func).  Dimmed markup uses Pango `alpha`, NEVER a fixed gray —
  hardcoded grays are unreadable on the blue selection.  Due tint:
  overdue #c01c28, today #d19a00, ahead #26a269, computed at draw time.
  Right-clicking any column header shows a hide/show menu for the Done
  and Due Date columns; visibility persists in `col_done_visible` /
  `col_due_visible`.
- Sidebar: gray backdrop CSS (rgb 230,230,230 / text 65,65,65 /
  selection rgb 86,131,224 white); meta rows bold (Favorites ⭐️, All
  Tasks 🔮, Due Today ☀️, Weekly Forecast 🌤️).  Due Today optionally
  includes all past-due tasks via `due_today_show_overdue` (Settings →
  Appearance; default off).  Weekly Forecast is its OWN
  panel, not rows in the task store: seven full-width day sections
  (Sunday–Saturday) stacked vertically, 6 px apart, each a heading
  label + framed (NOT individually scrolled) two-column (done ✓ +
  markup) tree view with its own store at natural full-content
  height, selection mode NONE (seven views would each keep their own
  selection; double-click activation works without one) — the whole
  week scrolls together in one outer scroller
  (position restored via scroll_keep_queue_win); refresh_tasks swaps
  task_scroll ↔ forecast_box visibility (re-applied after the
  construction-time show_all, which shows both) and clears the hidden
  regular store (a stale selection there would feed the toolbar's
  Delete Task).  Day headings are set per refresh (today: blue ● + "— Today"); the
  day checkbox resolves its store via "bt-model" object data on the
  renderer.  Empty days show an inert dimmed "No tasks due" row
  (id 0 — checkbox hidden by forecast_toggle_bg_func, activation
  ignored).
  In-list day-section headers and side-by-side day columns were both
  tried and rejected (2026-07-16) — don't reintroduce.
  The row exists only while `weekly_forecast`=1 (Settings →
  Appearance; default on).  The Favorites row exists ONLY while
  something is pinned (`bt_db_has_pinned`, counting bn_pins when the
  Blue Notes integration is on); editor pin flips arrive via
  notify_tasks, so `notify_tasks_hook` rebuilds the sidebar on the
  0 ↔ nonzero transition (tracked in `pinned_row_shown`) — the
  context-menu pin actions already full_refresh.  There is NO pinned
  column in the task list (removed 2026-07-16): pinning happens in the
  editor or the right-click menu (which BN rows never get — BN items
  pin via their editor only).  Real lists nest under a collapsible bold
  "Lists" header whose expansion is SNAPSHOTTED before every rebuild
  (first population expands; force-open when the selection lives
  inside).  Lists can be organized into named **list groups**
  (`SB_KIND_GROUP` rows, stored in `list_groups` table): right-click
  the Lists header or a group → New Group / Rename Group / Remove Group;
  right-click a list → Move to Group / Remove from Group.  Group
  expansion state is snapshotted separately in `lw->group_expanded`
  (GHashTable of group_id → bool); groups expand by default on first
  population and force-open when the selected list is inside.  Selecting
  a group row does NOT refresh the task pane — it just tracks
  `sel_kind`/`sel_id`.  List labels: `emoji + two spaces + name` when
  an emoji is set.  Double-clicking a list row opens the Edit List
  dialog (metas/header/BN row: no-op).  Sidebar starts HIDDEN by default
  (`sidebar_visible`, write-through on toggle).  Lists are ALPHABETICAL
  by default and drag-reorderable: `bt_db_lists` sorts by lower(name)
  until sync_state `lists_custom_order` exists (set by
  `bt_db_lists_reorder`, which writes position = display index; order
  is LOCAL-ONLY — Google tasklists have none — so reorders never dirty
  rows for sync).  The DnD dest protocol is fully custom, copied from
  Blue Notes' quirk #13: GtkTreeView's default drag-motion handler
  requests row data per motion, and on quartz the replies land before
  the release, finishing the drag mid-air — so on_sb_drag_motion
  answers gdk_drag_status itself (returning TRUE), only
  on_sb_drag_drop requests the data, and on_sb_drag_received stops the
  default emission and performs the move.  Only SB_KIND_LIST rows may
  drag or anchor a drop (never the metas, header, group, or Blue Notes
  row).
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
  actions (mark complete/incomplete, pin/unpin, set/clear high
  priority, move, delete) apply to the whole selection — single rows
  get only the pin/priority direction that applies; "Open in Google
  Tasks" is single-row only.  Bulk data
  rides on menu items as `g_array_ref`'d id arrays with destroy
  notifies.
- Editor: 600 ms debounced write-through saves; done/pinned save
  immediately.  NEVER rewrite the due entry while it has focus, and a
  save must not clobber the stored date when the entry holds partial/
  invalid text (`editor_due_entry_parse`).  Editors are singletons per
  task (`app->editors` gint64 keys) / per Blue Notes ref
  (`app->bn_editors` string keys).
- Editor foot row (packed LAST so it stays at the window's bottom in both
  fold states): an "Advanced ▾/▴" link at the left, then Save at the
  right in EVERY editor, with Cancel to its right only in the
  `bt_editor_open_new` variant — so the order reads Save, Cancel
  left-to-right, which means Cancel is `pack_end`ed FIRST (pack_end puts
  the first-packed child rightmost).  Save flushes the write-through save
  and closes;
  **Cancel closes and tombstones the task** (`bt_db_task_delete`, so the
  delete syncs), which is why New Task uses its own entry point —
  `bt_editor_open` must never offer that.  Cancel drops the pending
  debounce and destroys the window BEFORE deleting: `on_editor_destroy`
  would otherwise flush a save into the row being tombstoned.  It then
  notifies through `notify_changed` (a vanishing task is structural), and
  `ed` is dead by then, so it captures app/task_id first.
- Advanced disclosure (`editor_advanced_set`, the single applier):
  Subtasks + Attachments live in `adv_box`, folded by default and
  expanded on open when the task already HAS either
  (`editor_has_advanced_content`, read off the loaded stores, so it runs
  after `editor_load` + `show_all`).  Expanding measures the block's
  preferred height and grows the window by it, remembering `adv_height`
  so the collapse gives the same pixels back — measured round trip
  307 → 581 → 307.
- Editor geometry: default size is **490 × -1** — the -1 means the
  layout's NATURAL height, which is only correct because `adv_box`
  carries `no_show_all` and so is absent from that measurement (gotcha
  15).  A fixed window height was the bug: the notes box is packed
  expand=TRUE, so it swallowed every pixel of slack and opened huge.
  The notes scroller is pinned to **8 lines** via BOTH
  `min_content_height` and `max_content_height`, measured by laying out
  eight "X\n" lines in the view's own Pango context (+12 px: the view's
  4 px top/bottom margins and 4 px slack so the caret on the 8th line
  is not flush against the frame).  Font metrics' ascent+descent is NOT
  the right measure — it omits Pango's inter-line gap and lands about
  half a line short.  The max is what keeps a task with 50 lines of
  notes from opening a screen-tall window; it caps the size REQUEST
  only, so a hand-resized window still stretches the box.
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
- LOCAL-ONLY fields (never sent): `pinned`, task `priority` (binary
  high-priority flag; every view query sorts `priority DESC` first so
  flagged tasks top any list they appear in, 🚨 (siren emoji) in the task cell,
  "High Priority" editor checkbox), list `emoji`, attachments.
  A write to a local-only field must NOT bump `updated_at`
  (`bt_db_task_set_pinned` / `_set_priority` deliberately don't): the
  bump marks the row sync-dirty, so every Favorite/priority toggle buys
  a no-op PATCH, and a concurrent remote edit can be starved behind a
  412 skip for as long as the flag keeps changing.
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
  (`gtasks_refresh_token`), access tokens in memory only.  The OAuth
  client resolves as: client-secret JSON file next to the binary (or
  user config dir) → legacy ini keys `google_client_id`/`_secret` (no
  UI writes them) → baked-in default via gitignored
  `client_credentials.mk`.  THIS machine bakes via
  client_credentials.mk ONLY — the JSON was deliberately deleted
  (2026-07-15); don't look for it or recreate it.  Objects and
  compile_commands.json depend on the mk file, so a plain `make`
  picks up credential edits.  Races handled: a
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
  `blue_notes_sync=1` — unless `blue_notes_embed_list` (Settings'
  "Show action items in" combo) names a real list: then the sidebar
  row disappears and the items ride in that list's view instead,
  tagged "❗ Action Items · note N" (a stale/deleted embed id falls
  back to the own-section mode — see `bn_embed_list`).  Not
  deletable/editable as a list; items open the reduced editor (done +
  due writable via CLI; title/notes/subtasks/attachments insensitive
  — no CLI verbs for them).  PINNING and HIGH PRIORITY work: both are
  local-only state (`bn_pins` / `bn_priority` tables) keyed by the
  item's "NOTEID:ORD" ref (Blue Notes knows neither concept); pinned
  action items also appear in the Favorites view.  BN rows are
  fetched ONCE per refresh (`bn_rows_fetch` — the CLI is expensive)
  and appended via `append_bn_items` in a priority-then-rest pass
  pair; in the embedded list the priority pass lands ABOVE the tasks,
  the rest after them.  BN rows are detected by TL_REF != NULL
  (id 0), NOT by the selected sidebar kind — they can appear in
  Pinned and the embed list too.  The Blue
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
    it stays gitignored; document defaults in `lists.ini.defaults`.
12. `Gdk-CRITICAL … gdk_atom_intern: assertion 'atom_name != NULL'` on
    the console during a sidebar list drag (or exotic clipboard
    flavors) is a GTK-quartz bug, NOT ours — do not re-investigate:
    `gdkselection-quartz.c:199` (3.24.52) interns
    `uti.preferredMIMEType.UTF8String`, which is NULL for the `dyn.*`
    UTIs macOS wraps around custom pasteboard types like
    GTK_TREE_MODEL_ROW.  Harmless (that flavor is skipped, the row
    flavor still resolves); documented in User_Guide.md.  Don't
    suppress with a log filter.
14. `bt_app_switch_database` always **removes the old database file**
    after a successful switch — it is a move, not a copy.  This holds
    even when the user picks "Use Existing Database" (no copy was made,
    but the old file is still removed so no orphan is left behind).
    Do not add logic that skips the delete based on whether a copy was
    performed; that was the bug that caused orphaned files.
13. Google's DEFAULT tasklist cannot be deleted — `tasklists.delete`
    returns 400 "Invalid Value" from any client, and an unhandled
    failure there aborts the whole sync pass (blocking every later
    push).  Handled ACCURATELY (no hidden state): every sync GETs
    `…/lists/@default` and stores its id as
    `sync_state.default_list_gid`; on_delete_list refuses that list up
    front (like the Blue Notes list), and if a stale tombstone for it
    exists anyway, sync_lists RESTORES the list + its same-moment task
    tombstones (`bt_db_list_restore`) instead of deleting or hiding
    anything.  sync_lists also seeds the default list's emoji to 🔴
    (`bt_db_list_emoji_if_empty` — only while empty, no updated_at
    bump) so it is visibly marked; a user's later emoji edit sticks.
15. `gtk_widget_show_all(w)` returns EARLY when `w`'s OWN `no_show_all`
    flag is set — the flag is not just "skip me when a parent recurses".
    So the "hide it, then show it explicitly later" pattern silently
    never shows anything.  Both deferred sections in the editor keep the
    flag and LIFT it across their own show_all
    (`set_no_show_all(FALSE)` → `show_all` → `set_no_show_all(TRUE)`):
    the Advanced block (`adv_box`, in `editor_advanced_set`) and the
    "From Google" box (`google_section_load` — without the lift that
    section could never appear at all; found and fixed 2026-08-05).
    Keeping the flag is load-bearing, not just tidy: it is what keeps
    both boxes out of the window's NATURAL height, which the editor
    passes -1 to use.  Related: a GtkBox's preferred height counts only
    VISIBLE children, so measure a disclosure block AFTER showing it or
    the grow-the-window arithmetic reads 0.
