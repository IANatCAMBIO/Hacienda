# Hacienda — User Guide

Everyday use of Hacienda: the library, the task editor, settings,
storage, Google Tasks sync, and the Blue Notes integration. For build
instructions and OAuth client setup see the [README](README.md); for
the database schema and the sync engine see [Internals](Internals.md).

## Library window

- **Sidebar** — bold rolled-up views at the top: **Pinned Tasks**
  (present only while something is pinned), **All Tasks**, **Due
  Today** and **Due Tomorrow** (rolling over at local midnight). Your
  real lists nest under a collapsible **Lists** header, each shown
  with its emoji when one is set. Lists sort alphabetically until you
  drag one into place — from then on your custom order sticks (it is
  local-only; Google Tasks has no list order to sync). The compact
  **+ ✎ −** bar at the bottom creates, renames (with an emoji picker)
  and deletes lists. The sidebar starts hidden; the toolbar's
  **Sidebar** button toggles it and the choice persists.
- **Task rows** are tall: title (optionally bold — see Settings),
  notes preview, attachment count, and up to four subtask lines with
  their own checkboxes rendered inline. Columns: a done checkbox, the
  task, the due date, and a pinned checkbox. Rows alternate
  white/light-blue; click the Due header to sort (soonest first,
  undated rows last). Due dates are color-coded: green while the date
  is still ahead, gold on the day itself, red once it has passed.
- **Toolbar** — the Sidebar toggle, then New List, Delete List, New
  Task, Delete Task and Sync, then a visibility toggle that shows or
  hides completed tasks (it applies to every view, Blue Notes items
  included). At the far right, the logo button opens the About dialog
  (program info plus live database statistics). Button style —
  icons, icons + text, or text — is set in Settings or by
  right-clicking an empty spot on the toolbar.
- **Multi-select** (Cmd/Shift-click) for bulk actions via the
  right-click menu: mark complete or incomplete, **Move to List**,
  Delete. **Open in Google Tasks** opens a single selected task in
  the browser (for tasks that have synced).
- **Double-click a task** to open its editor window.
- Menus: *File → Settings…*, *Clear Completed Tasks*, *Sync Now* and
  *Quit*; *Help → About Hacienda*. With gtk-mac-integration built in,
  Settings can move the menu into the native macOS menu bar.

## Editor windows

Every task opens in its own window, centered on the screen, with a
standard titlebar (no GNOME header bars anywhere) — and only one
window per task: opening it again focuses the one you already have.

- **Fields** — title, Done, Pinned, and a due date you can type
  (`YYYY-MM-DD`) or pick from the calendar button. The entry is
  forgiving: while it holds a partial or invalid date nothing is
  clobbered — the stored date only changes once the text parses.
- **Notes** — free multiline text below the field row.
- **Subtasks** — add, rename, toggle and remove; exactly one level
  (subtasks cannot have subtasks, and Google Tasks agrees).
- **Attachments** — file references (add/remove/open); the files stay
  where they are, and the references never leave the machine.
- **From Google** — a read-only section for synced tasks showing
  what Google knows and the app doesn't edit: the completion time,
  a Docs/Chat assignment origin, and any Google-attached links (for
  example the Gmail message a task was created from).
- **Autosave** — edits persist about half a second after you stop
  typing; Done and Pinned save immediately.

## Settings (*File → Settings…*)

- **Sync** — the Google sync master switch, Sign In / Sign Out, and
  the auto-sync interval in minutes (default 5, 0 turns the timer
  off; the toolbar Sync button always works).
- **Appearance** — toolbar button style (icons / icons + text /
  text), bold task titles in the list, and — when built with
  gtk-mac-integration — a native macOS menu bar option.
- **Blue Notes** — enable the Action Items list and point the app at
  the `blue_notes` command (a path or a name on PATH).

All changes apply live and persist (in `hacienda.ini` next to the
binary). Toolbar icons are PNGs bundled in `icons/` — replaceable by
dropping in files.

## Storage

Everything lives in a single SQLite database:

- `~/.local/share/hacienda/hacienda.db` (GLib's user-data directory).
  Any standard SQLite tool can read it — the schema is documented in
  [Internals](Internals.md). Back it up by copying the file while the
  app is closed.
- Settings live in `hacienda.ini` next to the binary (portable mode),
  falling back to `~/.config/hacienda/` when that directory is not
  writable; it is seeded from `hacienda.ini.defaults` on first launch
  and rewritten by the app as you change things.

## Google Tasks sync

Sign in once (see the [README](README.md) for the OAuth client setup
if you built the app yourself); after that a periodic auto-sync runs
while signed in, and *File → Sync Now* or the toolbar button run one
on demand. The GUI stays live throughout — sync happens on a worker
thread.

What maps: tasklists ↔ lists, tasks ↔ tasks (with the same single
level of subtasks), due date ↔ due date, done ↔ completed. Three
things are **local-only** and never leave the machine: pinned flags,
list emoji, and attachments — Google Tasks has no equivalent. Two
honest API caveats, confirmed against the docs: Google's `due` field
is date-only (a time of day would be discarded), and there is no
starring/priority to map pinned onto.

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
  by their API); Hacienda refuses up front rather than failing
  mid-sync.

Signing out drops the tokens; the grant can also be revoked at
myaccount.google.com/permissions at any time.

## Blue Notes action items

If you keep meeting notes in [Blue Notes](../orange_notes/), its `!`
action items can show up here: enable the integration in Settings and
an **Action Items (from Blue Notes)** section appears under Lists
while it's on.

- Tick the checkbox to mark an item done — the line strikes through
  in the note itself. Double-click to open the reduced editor: done
  and due date are editable; the text lives in the note, so title,
  notes, subtasks and attachments are shown but locked.
- **Pinning works** and is Hacienda-local (Blue Notes has no pin
  concept); pinned action items appear in the Pinned Tasks view
  alongside your tasks.
- Everything goes through the `blue_notes` command-line interface —
  never its database file — so a running Blue Notes GUI and Hacienda
  cooperate safely (the CLI forwards to the GUI over its socket).
  Action items are not part of Google Tasks sync.
