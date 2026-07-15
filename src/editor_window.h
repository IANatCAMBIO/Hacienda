/* ===========================================================================
 * editor_window.h — the per-task editor window for Hacienda
 *
 * One window per task (double-click in the library), tracked in
 * app->editors keyed by task id.  Edits all task properties — title,
 * notes, due date, done, pinned — plus the task's attachments and (for
 * top-level tasks) its subtasks.  Subtasks cannot have subtasks: editing
 * a subtask shows a "part of" note instead of a subtask section.
 *
 * Saves are write-through with a short debounce (like the Blue Notes
 * editor autosave): every change lands in the database within ~600 ms
 * and the library refreshes.  Closing the window flushes a pending save.
 * =========================================================================== */

#ifndef BT_EDITOR_WINDOW_H
#define BT_EDITOR_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * bt_editor_open() — open (or raise) the editor for `task_id`.
 * ------------------------------------------------------------------------- */
void bt_editor_open(BtApp *app, gint64 task_id);

/* ---------------------------------------------------------------------------
 * bt_editor_open_bnote() — open (or raise) the reduced editor for a Blue
 * Notes action item addressed by `ref` ("NOTEID:ORD").  Done and due
 * date are editable (written back through the blue_notes CLI), and
 * pinned works too — it is local-only state in the bn_pins table; title,
 * notes, subtasks and attachments are disabled — the note in Blue Notes
 * owns those and the CLI has no verbs for them.
 * ------------------------------------------------------------------------- */
void bt_editor_open_bnote(BtApp *app, const gchar *ref);

/* ---------------------------------------------------------------------------
 * bt_editor_refresh_all() — reload every open editor from the database
 * (called after a sync or library-side change).  Editors with a pending
 * unsaved edit are skipped; text widgets are only rewritten when the
 * stored content actually differs, so a cursor never jumps mid-typing.
 * Editors whose task disappeared are closed.
 * ------------------------------------------------------------------------- */
void bt_editor_refresh_all(BtApp *app);

/* bt_editor_close_all() — destroy every open editor (flushing saves).       */
void bt_editor_close_all(BtApp *app);

#endif /* BT_EDITOR_WINDOW_H */
