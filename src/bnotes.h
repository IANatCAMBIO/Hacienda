/* ===========================================================================
 * bnotes.h — Records CLI access for Lists
 *
 * Lists MIRRORS the companion Records app's action items ('!' lines) as
 * ordinary tasks (see bnsync.h for the sync itself); this module is the
 * thin CLI wrapper underneath it.  ALL access goes through the records
 * CLI ("action list --uid" / "action done|undone" / "action due"), never
 * the Records database file: Records' GUI/CLI coexistence is a
 * single-writer design — CLI invocations route through a running GUI's
 * unix socket — so the CLI is the one safe automation surface.
 *
 * Output format parsed ("action list --uid", one row per item):
 *
 *     UID <TAB> NOTEID:ORD <TAB> [x]|[ ] <TAB> YYYY-MM-DD|- <TAB> text
 *
 * UID is the item's STABLE identity: a bare positive integer, assigned
 * once by Records, never reused, and unchanged when the item is
 * reworded or when other lines are inserted above it.  NOTEID:ORD is
 * the older POSITIONAL address — it renumbers whenever a note gains or
 * loses a '!' line, so it is never used as identity here; it survives
 * only to key the legacy bn_pins/bn_priority tables while the first
 * mirror pass drains them.
 *
 * The binary is resolved from the "blue_notes_cli" ini key (set in
 * File → Settings…), falling back to `records` on PATH.
 * =========================================================================== */

#ifndef BT_BNOTES_H
#define BT_BNOTES_H

#include <glib.h>

/* One Records action item.  Strings are owned.                              */
typedef struct {
    gint64    uid;                   /* stable identity (> 0)               */
    gchar    *ref;                   /* legacy "NOTEID:ORD" address         */
    gchar    *text;                  /* the item text                       */
    gint64    due;                   /* unix local midnight; 0 = none       */
    gboolean  done;
} BtNoteAction;

/* ---------------------------------------------------------------------------
 * bt_bnotes_actions() — run `records action list --uid` and parse the
 * rows (list order preserved: newest note first, like Records prints
 * it).  Returns BtNoteAction* elements (free with
 * bt_bnotes_actions_free), or NULL with *err set (g_free) — CLI
 * missing, spawn failure, non-zero exit.  A Records too old to know
 * --uid also fails here; call bt_bnotes_supports_uid() on the failure
 * path to tell that case apart and report it usefully.
 *
 * BLOCKING for the CLI round trip, so callers keep it to the sync
 * worker thread or a user-triggered refresh.
 * ------------------------------------------------------------------------- */
GPtrArray *bt_bnotes_actions(gchar **err);

void bt_bnotes_actions_free(GPtrArray *a);

/* ---------------------------------------------------------------------------
 * bt_bnotes_supports_uid() — does the installed Records understand
 * stable uids?  Runs `action list --uid` and reports whether it was
 * accepted (an older build answers an unknown flag with usage and exit
 * 1).  Diagnostic only: the happy path never calls this, so a normal
 * sync pass costs ONE CLI round trip.
 * ------------------------------------------------------------------------- */
gboolean bt_bnotes_supports_uid(void);

/* ---------------------------------------------------------------------------
 * bt_bnotes_action_set_done() — run `records action done|undone UID`.
 * The write lands in the Records note itself (striking/un-striking
 * the '!' line), routed through its GUI when one is running.  TRUE on
 * success; FALSE with *err set (g_free).
 * ------------------------------------------------------------------------- */
gboolean bt_bnotes_action_set_done(gint64 uid, gboolean done, gchar **err);

/* ---------------------------------------------------------------------------
 * bt_bnotes_action_set_due() — run `records action due UID DATE|-`
 * (due == 0 clears).  Rewrites the item's trailing "due <date>" suffix
 * in the note text.  TRUE on success; FALSE with *err set (g_free).
 * ------------------------------------------------------------------------- */
gboolean bt_bnotes_action_set_due(gint64 uid, gint64 due, gchar **err);

#endif /* BT_BNOTES_H */
