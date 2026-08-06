/* ===========================================================================
 * editor_window.h — the per-task editor window for Lists
 *
 * One window per task (double-click in the library), tracked in
 * app->editors keyed by task id.  Edits all task properties — title,
 * notes, due date, done, pinned — plus the task's attachments and (for
 * top-level tasks) its subtasks.  Subtasks cannot have subtasks: editing
 * a subtask shows a "part of" note instead of a subtask section.
 *
 * Saves are write-through with a short debounce (like the Records
 * editor autosave): every change lands in the database within ~600 ms
 * and the library refreshes.  Closing the window flushes a pending save.
 * A task mirroring a Records action item is no different here — its
 * done and due reach Records with the next mirror pass (bnsync.h),
 * never from this window.
 *
 * Subtasks and Attachments are folded away behind an "Advanced" link at
 * the foot of the window: collapsed for a task that has neither, expanded
 * on open for one that already does.  Expanding grows the window by that
 * block's own height and collapsing gives the same pixels back.
 *
 * The foot row also carries a Save button (flush the pending save and
 * close) at the right of every editor; the New Task variant adds Cancel
 * to its right — see bt_editor_open_new().
 * =========================================================================== */

#ifndef BT_EDITOR_WINDOW_H
#define BT_EDITOR_WINDOW_H

#include "app.h"

/* ---------------------------------------------------------------------------
 * bt_editor_open() — open (or raise) the editor for `task_id`.
 * ------------------------------------------------------------------------- */
void bt_editor_open(BtApp *app, gint64 task_id);

/* ---------------------------------------------------------------------------
 * bt_editor_open_new() — bt_editor_open() for the row the New Task action
 * has just created: the same window plus a Cancel button beside the Save
 * every editor has.  Cancel closes and DELETES the task again (tombstoned
 * with its subtasks, so the delete syncs), which is why this entry point
 * exists at all — Cancel must never do that to a pre-existing task.
 * Opens folded even if the row somehow has subtasks or attachments.
 * ------------------------------------------------------------------------- */
void bt_editor_open_new(BtApp *app, gint64 task_id);

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
