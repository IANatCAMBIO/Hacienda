# Lists — User Guide

Everyday use of Lists: the library, the task editor, settings,
storage, Google Tasks sync, and the Records integration. For build
instructions and OAuth client setup see the [README](README.md); for
the database schema and the sync engine see [Internals](Internals.md).

## Library window

- **Sidebar** — bold rolled-up views at the top: **⭐️ Favorites**
  (present only while something is favorited), **🔮 All Tasks**,
  **☀️ Due Today** (rolling over at local midnight; Settings can widen
  it to include everything past due) and **🌤️ Weekly Forecast** —
  the current week, Sunday through Saturday, as seven day lists
  stacked down the page, each headed by its day and date (today is
  marked) and showing the tasks due that day; check off tasks in
  place or double-click to edit. The Weekly Forecast view can be
  switched off in Settings → Appearance, which removes it from the
  sidebar entirely. Your
  real lists nest under a collapsible **Lists** header, each shown
  with its emoji when one is set. Lists sort alphabetically until you
  drag one into place — from then on your custom order sticks (it is
  local-only; Google Tasks has no list order to sync). One honest
  caveat if you launch from a terminal on macOS: dragging a list may
  print a `Gdk-CRITICAL … gdk_atom_intern` line on the console. It is
  harmless noise from a bug in the Mac build of the GTK library
  itself, not in Lists — the drag works fine. Right-click to manage
  lists: the **Lists** header offers *New List* and *New Group*, and a
  list offers *Edit List* (name plus an emoji picker), *Delete List*,
  *Move to Group* and *Remove from Group*; double-clicking a list opens
  that same Edit dialog. *File → New List…* does the same thing.
  The sidebar starts hidden; the toolbar's **Sidebar** button (or
  *View → Show Sidebar*) toggles it and the choice persists.
- **List groups** — lists can be filed under named groups, which show
  as their own expandable rows under the **Lists** header. Right-click
  a group to *Rename Group* or *Remove Group* (removing the group keeps
  its lists — they just move back up to the top level). Selecting a
  group row doesn't change the task pane; it is only a container.
  Expansion state, for the Lists header and each group, is remembered
  across refreshes and forced open when your selected list is inside.
- **Task rows** are tall: title (optionally bold — see Settings),
  notes preview, attachment count, and up to four subtask lines with
  their own checkboxes rendered inline. Tasks marked **High
  Priority** (a checkbox in the editor, or the right-click menu) sort
  to the top of every list they appear in and wear a 🚨 beside the
  title. The flag is local to Lists — Google Tasks has no priority, so
  it never syncs. Columns: a done checkbox, the task, and the due date;
  right-click any column header to hide or show the Done and Due Date
  columns (the Task column always stays). Favoriting is done from the
  editor window's **Favorite** checkbox or the task's right-click menu.
  Rows alternate white/light-blue; click the Due header to sort
  (soonest first, undated rows last). Due dates are color-coded: green
  while the date is still ahead, gold on the day itself, red once it
  has passed.
- **Manual Sort** — the toolbar's sort-mode toggle (or *View → Manual
  Sort*) switches the task pane to hand ordering: a ⠿ handle column
  appears and you drag rows into the order you want. The order is
  remembered per view — each list, All Tasks, Favorites, Due Today and
  the Records section keep their own — and is local to Lists.
- **Toolbar** — the Sidebar toggle, then Sync, a visibility toggle that
  shows or hides completed tasks (it applies to every view, Records
  items included), the Manual Sort toggle, then New Task and Delete
  Task. At the far right, the logo button opens the About dialog
  (program info plus live database statistics). Button style —
  icons, icons + text, or text — is set in Settings or by
  right-clicking an empty spot on the toolbar. The Sync button can be
  hidden in Settings → Google Tasks.
- **Status bar** — the left side describes the current view and
  selection; the right side shows the latest event message (a sync
  result, a save failure), which fades out after a few seconds.
- **View menu** — *Show Completed* and *Manual Sort* mirror their
  toolbar buttons, *Show Sidebar* mirrors the Sidebar toggle, and
  *Compact Layout* strips the window down to the task list: the whole
  toolbar and the sidebar go away, and a small floating bar with just
  **New Task** and **Delete Task** sits 20 px in from the bottom-right
  corner. Turning Compact Layout back off restores the toolbar and
  puts the sidebar back the way you had it; *Show Sidebar* still works
  while compact if you want the lists pane over the task list. The
  choice persists between launches.
- **Multi-select** (Cmd/Shift-click) for bulk actions via the
  right-click menu: mark complete or incomplete, favorite or
  unfavorite, set or clear **High Priority**, **Move to List**, Delete.
  With a single task selected the favorite and priority items show only
  the direction that applies. **Open in Google Tasks** opens a single
  selected task in the browser (for tasks that have synced).
- **Double-click a task** to open its editor window.
- Menus: *File → New Task*, *New List…*, *Sync Now*, *Clear Completed
  Tasks*, *Open Database File…*, *Settings…*, *About*, and *Quit*. With
  gtk-mac-integration built in, the menu moves into the native macOS
  menu bar.

## Editor windows

Every task opens in its own window, centered on the screen, with a
standard titlebar (no GNOME header bars anywhere) — and only one
window per task: opening it again focuses the one you already have.

- **Fields** — title, **Done**, **Favorite**, **High Priority**, and a
  due date you can type (`YYYY-MM-DD`) or pick from the calendar button.
  The entry is forgiving: while it holds a partial or invalid date
  nothing is clobbered — the stored date only changes once the text
  parses.
- **Notes** — free multiline text below the field row, eight lines tall
  to start with; it scrolls past that, and grows if you enlarge the
  window.
- **Advanced** — Subtasks and Attachments start folded away behind the
  **Advanced ▾** link at the bottom-left; clicking it drops the window
  open to show both, and clicking again folds it back to the size it
  was. A task that already has subtasks or attachments opens expanded,
  so you never have to click to see what is already there.
- **Save** sits under the notes box, at its right edge, in every editor:
  it closes the window (your edits are already saved as you type, so it
  is a "done here" button, not the only way to persist). Closing the
  window with its close box does exactly the same thing.
- **New Task** windows add **Cancel** to the right of Save. **Cancel
  closes the window and deletes the task again** — the "never mind"
  button for a task you just created. Only new tasks get it; nothing in
  an existing task's editor can delete it.
- **Subtasks** — add, rename, toggle and remove; exactly one level
  (subtasks cannot have subtasks, and Google Tasks agrees).
- **Attachments** — file references (add/remove/open); the files stay
  where they are, and the references never leave the machine.
- **From Google** — a read-only section for synced tasks showing
  what Google knows and the app doesn't edit: the completion time,
  a Docs/Chat assignment origin, and any Google-attached links (for
  example the Gmail message a task was created from).
- **Autosave** — edits persist about half a second after you stop
  typing; the Done, Favorite and High Priority checkboxes save
  immediately.

## Settings (*File → Settings…*)

- **Appearance** — toolbar button style (icons / icons + text /
  text), bold task titles in the list, show/hide the Weekly Forecast
  sidebar row, whether **Due Today** also includes everything past due,
  and — when built with gtk-mac-integration — a native macOS menu bar
  option.
- **Database** — shows the current database file path and lets you
  move it to a different folder. Switching always removes the old
  file: if the target folder is empty the current database is copied
  there; if it already contains a database you choose whether to use
  the existing one or overwrite it with your current data.
- **Records** — mirror its action items as ordinary tasks, point the
  app at the `records` command (a path or a name on PATH), choose which
  list new items are filed into, set how often changes are sent back,
  and show or hide the sidebar's Action Items view.
- **Google Tasks** — the sync master switch, Sign In / Sign Out, the
  auto-sync interval in minutes (default 5; 0 turns the timer off while
  the toolbar Sync button always works), and whether the Sync button
  appears in the toolbar at all.

All changes apply live and persist (in `lists.ini` next to the
binary). Toolbar icons are PNGs bundled in `icons/` — replaceable by
dropping in files.

## Storage

Everything lives in a single SQLite database:

- `~/.local/share/lists/lists.db` (GLib's user-data directory).
  Any standard SQLite tool can read it — the schema is documented in
  [Internals](Internals.md). Back it up by copying the file while the
  app is closed.
- Settings live in `lists.ini` next to the binary (portable mode),
  falling back to `~/.config/lists/` when that directory is not
  writable; it is seeded from `lists.ini.defaults` on first launch
  and rewritten by the app as you change things.

## Google Tasks sync

Sign in once (see the [README](README.md) for the OAuth client setup
if you built the app yourself); after that a periodic auto-sync runs
while signed in, and *File → Sync Now* or the toolbar button run one
on demand. The GUI stays live throughout — sync happens on a worker
thread.

What maps: tasklists ↔ lists, tasks ↔ tasks (with the same single
level of subtasks), due date ↔ due date, done ↔ completed. Four
things are **local-only** and never leave the machine: Favorite flags,
High Priority flags, list emoji, and attachments — Google Tasks has no
equivalent. Your list order and any manual task order are local-only
too. Two honest API caveats, confirmed against the docs: Google's
`due` field is date-only (a time of day would be discarded), and there
is no starring or priority field to map Favorite/High Priority onto.

How it behaves:

- After the first full pass, syncs are **incremental** — only items
  changed since the last pass transfer.
- **Absence never deletes.** Only explicit deletes propagate: delete
  a task here and it disappears from Google; delete it on Google and
  it disappears here. A task deleted on Google *without* a trace
  (say, by another client while you were offline) is pushed back
  rather than silently dropped.
- Conflicts resolve **newest-wins** per item; a deletion beats a
  concurrent edit. Edits are etag-guarded, so a push that lost the
  race defers to the remote copy and the next pull reconciles.
- **Clear Completed Tasks** uses the server-side clear when signed
  in; **Move to List** uses the server-side move (falling back to
  delete-and-recreate when offline). Tasks cleared on Google's side
  stay cleared — they are never resurrected here.
- Google's default tasklist cannot be deleted (their rule, enforced
  by their API); Lists refuses up front rather than failing
  mid-sync.

Signing out drops the tokens; the grant can also be revoked at
myaccount.google.com/permissions at any time.

## Records action items

If you keep meeting notes in
[Records](https://github.com/IANatCAMBIO/records), its `!`
action items can live here as **ordinary tasks**. Enable the
integration in Settings and Lists mirrors each item into a real list —
the managed **Action Items** list by default, or any list you pick
under "Mirror action items into".

Because they are ordinary tasks, they behave like everything else:
notes, subtasks, attachments, Favorites, High Priority, moving them to
another list, and Google Tasks sync all work. Nothing is locked.

- **Where they are** — the sidebar's **Action Items** row is a view,
  not a list: it gathers every mirrored item wherever you have filed
  it, so moving one into your working list doesn't lose track of it.
  Turn the row off in Settings → Records if you don't want it.
- **What flows back to Records** — ticking an item done and changing
  its due date, because those are the only two things the Records
  command line can write. The item's **text belongs to the note**: edit
  it in Records, not here, or your change will be overwritten the next
  time the two sync.
- **When it flows back** — not instantly. Changes are cached and sent
  in a batch on the interval in Settings → Records ("Sync action items
  every N minutes", 5 by default; set 0 to only sync when you press
  Sync). If Records can't be reached, your change simply waits and goes
  out on a later pass — it is never dropped.
- **Deleting** — an item deleted in Records disappears from Lists on
  the next pass, along with any notes or subtasks you attached to it.
  Deleting the task in Lists keeps it deleted: it will not come back on
  the next sync, even though the item still exists in Records.
- Everything goes through the `records` command-line interface —
  never its database file — so a running Records GUI and Lists
  cooperate safely (the CLI forwards to the GUI over its socket).
  This needs a current version of Records; against an older one Lists
  says so and leaves your tasks alone rather than guessing which item
  is which.
