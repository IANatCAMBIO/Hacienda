/* ===========================================================================
 * bnotes.h — Blue Notes integration for Blue Tasks
 *
 * Blue Tasks can show the companion Blue Notes app's action items ('!'
 * lines) as a special read/write list.  ALL access goes through the
 * blue_notes CLI ("action list" / "action done|undone"), never the
 * Blue Notes database file: Blue Notes' GUI/CLI coexistence is a
 * single-writer design — CLI invocations route through a running GUI's
 * unix socket — so the CLI is the one safe automation surface.
 *
 * Output format parsed ("action list", one row per item):
 *
 *     NOTEID:ORD <TAB> [x]|[ ] <TAB> YYYY-MM-DD|- <TAB> text
 *
 * The binary is resolved from the "blue_notes_cli" ini key (set in
 * File → Settings…), falling back to `blue_notes` on PATH.
 * =========================================================================== */

#ifndef BT_BNOTES_H
#define BT_BNOTES_H

#include <glib.h>

/* One Blue Notes action item.  Strings are owned.                           */
typedef struct {
    gchar    *ref;                   /* "NOTEID:ORD" — the CLI address      */
    gchar    *text;                  /* the item text                       */
    gint64    due;                   /* unix local midnight; 0 = none       */
    gboolean  done;
} BtNoteAction;

/* ---------------------------------------------------------------------------
 * bt_bnotes_actions() — run `blue_notes action list` and parse the rows
 * (list order preserved: newest note first, like Blue Notes prints it).
 * Returns BtNoteAction* elements (free with bt_bnotes_actions_free), or
 * NULL with *err set (g_free) — CLI missing, spawn failure, non-zero
 * exit.  BLOCKING for the CLI round trip (fast: local socket or local
 * file), so callers keep it to user-triggered refreshes.
 * ------------------------------------------------------------------------- */
GPtrArray *bt_bnotes_actions(gchar **err);

void bt_bnotes_actions_free(GPtrArray *a);

/* ---------------------------------------------------------------------------
 * bt_bnotes_action_set_done() — run `blue_notes action done|undone REF`.
 * The write lands in the Blue Notes note itself (striking/un-striking
 * the '!' line), routed through its GUI when one is running.  TRUE on
 * success; FALSE with *err set (g_free).
 * ------------------------------------------------------------------------- */
gboolean bt_bnotes_action_set_done(const gchar *ref, gboolean done,
                                   gchar **err);

/* ---------------------------------------------------------------------------
 * bt_bnotes_action_set_due() — run `blue_notes action due REF DATE|-`
 * (due == 0 clears).  Rewrites the item's trailing "due <date>" suffix
 * in the note text.  TRUE on success; FALSE with *err set (g_free).
 * ------------------------------------------------------------------------- */
gboolean bt_bnotes_action_set_due(const gchar *ref, gint64 due,
                                  gchar **err);

#endif /* BT_BNOTES_H */
