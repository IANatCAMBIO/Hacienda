/* ===========================================================================
 * library_window.c — the main Blue Tasks window (see library_window.h)
 * =========================================================================== */

#include "library_window.h"
#include "bnotes.h"
#include "editor_window.h"
#include "gtasks.h"
#include "oauth.h"
#include "settings_window.h"
#include <stdlib.h>
#include <string.h>
#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

/* Odd-row stripe tint of the task list (the Blue Notes list palette).       */
#define ROW_TINT "#e8f2fb"

/* Sidebar row kinds (SB_KIND column).                                       */
enum {
    SB_KIND_PINNED = 0,              /* "Pinned Tasks" virtual list         */
    SB_KIND_ALL,                     /* "All Tasks" virtual list            */
    SB_KIND_BN_ACTIONS,              /* Blue Notes "Action Items" list      */
    SB_KIND_TODAY,                   /* "Due Today" virtual list            */
    SB_KIND_TOMORROW,                /* "Due Tomorrow" virtual list         */
    SB_KIND_HEADER,                  /* the "Lists" section header          */
    SB_KIND_LIST                     /* a real list                         */
};

/* Sidebar store columns.                                                    */
enum {
    SB_KIND = 0,                     /* gint: one of SB_KIND_*              */
    SB_ID,                           /* gint64: list id (SB_KIND_LIST)      */
    SB_LABEL,                        /* gchar*: display text                */
    SB_WEIGHT,                       /* gint: Pango weight (bold metas)     */
    SB_N_COLS
};

/* Task pane store columns.                                                  */
enum {
    TL_ID = 0,                       /* gint64: task id (0 for Blue Notes
                                      * action rows)                        */
    TL_DONE,                         /* gboolean                            */
    TL_DESC,                         /* gchar*: the tall markup cell        */
    TL_DUE,                          /* gchar*: formatted due date ("")     */
    TL_DUE_RAW,                      /* gint64: due timestamp (sort/tint)   */
    TL_PINNED,                       /* gboolean                            */
    TL_REF,                          /* gchar*: Blue Notes "NOTEID:ORD"
                                      * address, NULL for real tasks        */
    TL_N_COLS
};

/* ---------------------------------------------------------------------------
 * BtLibrary — the window's state.
 *   sel_kind/sel_id — current sidebar selection (survives refreshes).
 *   populating      — guards the sidebar changed handler during rebuilds.
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp        *app;
    gchar        *db_path;           /* for the sync worker                 */
    GtkWidget    *window;
    GtkTreeStore *sb_store;
    GtkWidget    *sb_view;
    GtkListStore *task_store;
    GtkWidget    *task_view;
    GtkWidget    *sidebar_box;       /* for the toolbar show/hide toggle    */
    GtkWidget    *status_left;       /* selection info label                */
    GtkWidget    *status_right;      /* latest event message label          */
    GtkWidget    *sync_item;         /* the Lists-menu Sync item            */
    GtkWidget    *hide_done_item;    /* completed-visibility toggle button  */
    gint          sel_kind;
    gint64        sel_id;
    gboolean      populating;
    gboolean      sb_populated;      /* first population expands Lists      */
    gint          win_w, win_h;      /* live client size (persisted at
                                      * close as the next launch's size)    */
} BtLibrary;

/* list_label() — a list's display label: the optional emoji prefixes
 * the name, set off by two spaces.  New string (g_free).                    */
static gchar *
list_label(const BtList *l)
{
    return *l->emoji != '\0'
        ? g_strdup_printf("%s  %s", l->emoji, l->name)
        : g_strdup(l->name);
}

/* lib_of() — the BtLibrary behind app->library_window.                      */
static BtLibrary *
lib_of(BtApp *app)
{
    if (app->library_window == NULL)
        return NULL;
    return g_object_get_data(G_OBJECT(app->library_window), "bt-library");
}

/* ===========================================================================
 * Task description markup — the tall cell.
 * =========================================================================== */

/* append_line() — add a `\n`-separated markup line.                         */
static void
append_line(GString *s, const gchar *markup)
{
    if (s->len > 0)
        g_string_append_c(s, '\n');
    g_string_append(s, markup);
}

/* ---------------------------------------------------------------------------
 * task_desc_markup() — build the Task cell: bold title (struck when
 * done), an "in <list>" line in the virtual views, a dimmed notes
 * preview, an attachment count, and up to four subtask lines.  This is
 * what makes the rows "extra tall".
 *   list_name  — the owning list's name, or NULL when the view IS that
 *                list (no need to repeat it).
 *   att_count  — the task's attachment count.
 *   subs       — the task's visible subtasks (may be NULL).
 *   bold       — render the title in bold (the "bold_task_titles"
 *                setting, read once per refresh by the caller).
 * ------------------------------------------------------------------------- */
static gchar *
task_desc_markup(const BtTask *t, const gchar *list_name, gint att_count,
                 GPtrArray *subs, gboolean bold)
{
    GString *s = g_string_new(NULL);
    gchar *title = g_markup_escape_text(
        *t->title != '\0' ? t->title : "Untitled Task", -1);
    const gchar *open  = bold ? "<b>" : "";
    const gchar *close = bold ? "</b>" : "";
    gchar *line = t->done
        ? g_strdup_printf("%s<s>%s</s>%s", open, title, close)
        : g_strdup_printf("%s%s%s", open, title, close);
    if (t->parent_id != 0) {         /* a subtask row in a virtual view     */
        gchar *sub = g_strdup_printf("\xe2\x86\xb3 %s", line);
        g_free(line);
        line = sub;
    }
    append_line(s, line);
    g_free(line);
    g_free(title);

    /* Dimmed lines use Pango ALPHA, never a fixed gray: a hardcoded
     * foreground stays gray on the selection's blue background and is
     * unreadable — alpha dims whatever color the theme picks, so the
     * text follows the row's selected/unselected state.                     */
    if (list_name != NULL) {
        gchar *esc = g_markup_escape_text(list_name, -1);
        gchar *l = g_strdup_printf(
            "<small><i><span alpha=\"60%%\">in %s</span></i>"
            "</small>", esc);
        append_line(s, l);
        g_free(l);
        g_free(esc);
    }

    if (*t->notes != '\0') {
        /* First line of the notes, capped, as a dimmed preview.             */
        gchar *preview = g_strndup(t->notes, 120);
        gchar *nl = strchr(preview, '\n');
        if (nl != NULL)
            *nl = '\0';
        gchar *esc = g_markup_escape_text(preview, -1);
        gchar *l = g_strdup_printf(
            "<small><span alpha=\"65%%\">%s%s</span></small>",
            esc, strlen(t->notes) > strlen(preview) ? "\xe2\x80\xa6" : "");
        append_line(s, l);
        g_free(l);
        g_free(esc);
        g_free(preview);
    }

    if (att_count > 0) {
        gchar *l = g_strdup_printf(
            "<small><span alpha=\"65%%\">\xf0\x9f\x93\x8e "
            "%d attachment%s</span></small>",
            att_count, att_count == 1 ? "" : "s");
        append_line(s, l);
        g_free(l);
    }

    guint nsubs = subs != NULL ? subs->len : 0;
    for (guint i = 0; i < MIN(nsubs, 4u); i++) {
        BtTask *sub = g_ptr_array_index(subs, i);
        gchar *esc = g_markup_escape_text(
            *sub->title != '\0' ? sub->title : "Untitled", -1);
        gchar *l = sub->done
            ? g_strdup_printf("<small>\xe2\x98\x91 <span "
                              "alpha=\"55%%\"><s>%s</s></span>"
                              "</small>", esc)
            : g_strdup_printf("<small>\xe2\x98\x90 %s</small>", esc);
        append_line(s, l);
        g_free(l);
        g_free(esc);
    }
    if (nsubs > 4) {
        gchar *l = g_strdup_printf(
            "<small><span alpha=\"65%%\">\xe2\x80\xa6 +%u more "
            "subtask%s</span></small>", nsubs - 4,
            nsubs - 4 == 1 ? "" : "s");
        append_line(s, l);
        g_free(l);
    }
    return g_string_free(s, FALSE);
}

/* ===========================================================================
 * Refreshes.
 * =========================================================================== */

/* scroll_keep_queue() — restore a scrolled window's vertical position
 * after a model rebuild (idle-deferred so the rebuilt view re-validates
 * its height first — Blue Notes gotcha #11).                                */
typedef struct {
    GtkAdjustment *vadj;
    gdouble        value;
} ScrollKeep;

static gboolean
scroll_keep_apply(gpointer data)
{
    ScrollKeep *sk = data;
    gtk_adjustment_set_value(sk->vadj,
        MIN(sk->value, gtk_adjustment_get_upper(sk->vadj) -
                       gtk_adjustment_get_page_size(sk->vadj)));
    g_object_unref(sk->vadj);
    g_free(sk);
    return G_SOURCE_REMOVE;
}

static void
scroll_keep_queue(GtkWidget *view)
{
    GtkWidget *scroll = gtk_widget_get_parent(view);
    if (!GTK_IS_SCROLLED_WINDOW(scroll))
        return;
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(scroll));
    ScrollKeep *sk = g_new0(ScrollKeep, 1);
    sk->vadj  = g_object_ref(vadj);
    sk->value = gtk_adjustment_get_value(vadj);
    g_idle_add(scroll_keep_apply, sk);
}

/* ---------------------------------------------------------------------------
 * refresh_sidebar() — rebuild the sidebar and restore the selection.
 * ------------------------------------------------------------------------- */
static void
refresh_sidebar(BtLibrary *lw)
{
    lw->populating = TRUE;
    scroll_keep_queue(lw->sb_view);

    /* Snapshot the Lists section's expansion BEFORE the clear — every
     * model rebuild collapses it otherwise (Blue Notes gotcha #14).
     * The first population expands it; after that the user's choice
     * is preserved.                                                         */
    GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
    gboolean lists_expanded = TRUE;
    GtkTreeIter iter;
    if (lw->sb_populated) {
        lists_expanded = FALSE;
        if (gtk_tree_model_get_iter_first(model, &iter)) {
            do {
                gint kind;
                gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
                if (kind == SB_KIND_HEADER) {
                    GtkTreePath *p = gtk_tree_model_get_path(model,
                                                             &iter);
                    lists_expanded = gtk_tree_view_row_expanded(
                        GTK_TREE_VIEW(lw->sb_view), p);
                    gtk_tree_path_free(p);
                    break;
                }
            } while (gtk_tree_model_iter_next(model, &iter));
        }
    }
    gtk_tree_store_clear(lw->sb_store);
    struct {
        gint kind;
        const gchar *label;
    } metas[] = {                    /* emoji + two spaces, like lists      */
        { SB_KIND_PINNED,   "\xf0\x9f\x93\x8d  Pinned Tasks" },
        { SB_KIND_ALL,      "\xf0\x9f\x94\xae  All Tasks" },
        { SB_KIND_TODAY,    "\xf0\x9f\x8c\x9e  Due Today" },
        { SB_KIND_TOMORROW, "\xf0\x9f\x8c\x99  Due Tomorrow" },
    };
    for (gsize i = 0; i < G_N_ELEMENTS(metas); i++) {
        gtk_tree_store_append(lw->sb_store, &iter, NULL);
        gtk_tree_store_set(lw->sb_store, &iter,
                           SB_KIND, metas[i].kind,
                           SB_ID, (gint64)0,
                           SB_LABEL, metas[i].label,
                           SB_WEIGHT, PANGO_WEIGHT_BOLD,
                           -1);
    }
    GtkTreeIter header;              /* the collapsible "Lists" section     */
    gtk_tree_store_append(lw->sb_store, &header, NULL);
    gtk_tree_store_set(lw->sb_store, &header,
                       SB_KIND, SB_KIND_HEADER,
                       SB_ID, (gint64)0,
                       SB_LABEL, "Lists",
                       SB_WEIGHT, PANGO_WEIGHT_BOLD,
                       -1);

    /* Real lists nest UNDER the header (like Blue Notes' folder tree).      */
    GPtrArray *lists = bt_db_lists(lw->app->db, FALSE);
    GtkTreeIter selected;            /* the row to reselect                 */
    gboolean have_selected = FALSE;
    GtkTreeIter first_list;          /* fallback selection                  */
    gboolean have_first = FALSE;
    for (guint i = 0; i < lists->len; i++) {
        BtList *l = g_ptr_array_index(lists, i);
        gchar *label = list_label(l);
        gtk_tree_store_append(lw->sb_store, &iter, &header);
        gtk_tree_store_set(lw->sb_store, &iter,
                           SB_KIND, SB_KIND_LIST,
                           SB_ID, l->id,
                           SB_LABEL, label,
                           SB_WEIGHT, PANGO_WEIGHT_NORMAL,
                           -1);
        g_free(label);
        if (!have_first) {
            first_list = iter;
            have_first = TRUE;
        }
        if (lw->sel_kind == SB_KIND_LIST && lw->sel_id == l->id) {
            selected = iter;
            have_selected = TRUE;
        }
    }
    bt_ptr_array_free_lists(lists);

    /* The Blue Notes Action Items list sits among the real lists (but
     * cannot be deleted or hold new tasks); it exists only while the
     * integration is enabled in Settings.                                   */
    if (bt_app_config_get_bool("blue_notes_sync", FALSE)) {
        gtk_tree_store_append(lw->sb_store, &iter, &header);
        gtk_tree_store_set(lw->sb_store, &iter,
                           SB_KIND, SB_KIND_BN_ACTIONS,
                           SB_ID, (gint64)0,
                           SB_LABEL, "\xe2\x9d\x97\xef\xb8\x8f  "
                                     "Action Items (from Blue Notes)",
                           SB_WEIGHT, PANGO_WEIGHT_NORMAL,
                           -1);
        if (lw->sel_kind == SB_KIND_BN_ACTIONS) {
            selected = iter;
            have_selected = TRUE;
        }
    }

    /* Reselect: same list, or same meta row, or the first list.             */
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->sb_view));
    if (!have_selected && lw->sel_kind != SB_KIND_LIST &&
        gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gint kind;
            gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
            if (kind == lw->sel_kind) {
                selected = iter;
                have_selected = TRUE;
                break;
            }
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    if (!have_selected && have_first) {
        selected = first_list;
        have_selected = TRUE;
        lw->sel_kind = SB_KIND_LIST;
        gtk_tree_model_get(model, &first_list, SB_ID, &lw->sel_id, -1);
    }
    if (!have_selected &&            /* no lists at all: fall back to the   */
        gtk_tree_model_get_iter_first(model, &iter)) {
        selected = iter;             /* Pinned Tasks row                    */
        have_selected = TRUE;
        lw->sel_kind = SB_KIND_PINNED;
        lw->sel_id = 0;
    }

    /* Restore the section's expansion — and force it open when the row
     * to select lives inside it (a selection must be visible).              */
    if (lists_expanded ||
        (have_selected && (lw->sel_kind == SB_KIND_LIST ||
                           lw->sel_kind == SB_KIND_BN_ACTIONS))) {
        GtkTreePath *p = gtk_tree_model_get_path(model, &header);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(lw->sb_view), p, FALSE);
        gtk_tree_path_free(p);
    }
    if (have_selected)
        gtk_tree_selection_select_iter(sel, &selected);
    lw->sb_populated = TRUE;
    lw->populating = FALSE;
}

/* ---------------------------------------------------------------------------
 * append_bn_rows() — append Blue Notes action items to the task pane
 * (fetched through the blue_notes CLI; see bnotes.h).  Rows carry their
 * "NOTEID:ORD" address in TL_REF and 0 in TL_ID; TL_PINNED comes from
 * the local bn_pins table (pinning is a Blue Tasks concept).  With
 * only_pinned, unpinned items are skipped (the Pinned Tasks view).
 * Returns the number of rows appended, or -1 when the CLI failed (the
 * error is posted to the status bar).
 * ------------------------------------------------------------------------- */
static gint
append_bn_rows(BtLibrary *lw, gboolean only_pinned)
{
    gchar *err = NULL;
    GPtrArray *acts = bt_bnotes_actions(&err);
    if (acts == NULL) {
        bt_app_status(lw->app, "%s", err);
        g_free(err);
        return -1;
    }
    GHashTable *pins = bt_db_bn_pins(lw->app->db);
    gboolean bold = bt_app_config_get_bool("bold_task_titles", FALSE);
    gboolean show_done = bt_app_config_get_bool("show_completed", TRUE);
    gint shown = 0;
    for (guint i = 0; i < acts->len; i++) {
        BtNoteAction *na = g_ptr_array_index(acts, i);
        gboolean pinned = g_hash_table_contains(pins, na->ref);
        if (only_pinned && !pinned)
            continue;
        if (!show_done && na->done)
            continue;
        gchar *esc = g_markup_escape_text(
            *na->text != '\0' ? na->text : "(empty item)", -1);
        gchar *note = g_strndup(na->ref, strcspn(na->ref, ":"));
        const gchar *open  = bold ? (na->done ? "<b><s>" : "<b>")
                                  : (na->done ? "<s>" : "");
        const gchar *close = bold ? (na->done ? "</s></b>" : "</b>")
                                  : (na->done ? "</s>" : "");
        gchar *desc = g_strdup_printf(
            "%s%s%s\n<small><span alpha=\"60%%\">Blue Notes \xc2\xb7 "
            "note %s</span></small>",
            open, esc, close, note);
        gchar *due = bt_due_format(na->due);
        GtkTreeIter iter;
        gtk_list_store_append(lw->task_store, &iter);
        gtk_list_store_set(lw->task_store, &iter,
                           TL_ID, (gint64)0,
                           TL_DONE, na->done,
                           TL_DESC, desc,
                           TL_DUE, due,
                           TL_DUE_RAW, na->due,
                           TL_PINNED, pinned,
                           TL_REF, na->ref,
                           -1);
        g_free(desc);
        g_free(due);
        g_free(esc);
        g_free(note);
        shown++;
    }
    g_hash_table_destroy(pins);
    bt_bnotes_actions_free(acts);
    return shown;
}

/* refresh_bn_actions() — the Action Items list view: every item, plus
 * the status-bar location line.                                             */
static void
refresh_bn_actions(BtLibrary *lw)
{
    gint n = append_bn_rows(lw, FALSE);
    if (n < 0) {
        gtk_label_set_text(GTK_LABEL(lw->status_left),
                           "Action Items (Blue Notes)");
        return;
    }
    gchar *loc = g_strdup_printf(
        "Action Items (from Blue Notes) \xe2\x80\x94 %d item%s",
        n, n == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(lw->status_left), loc);
    g_free(loc);
}

/* ---------------------------------------------------------------------------
 * refresh_tasks() — rebuild the task pane for the current selection.
 * ------------------------------------------------------------------------- */
static void
refresh_tasks(BtLibrary *lw)
{
    scroll_keep_queue(lw->task_view);
    gtk_list_store_clear(lw->task_store);

    if (lw->sel_kind == SB_KIND_BN_ACTIONS) {
        refresh_bn_actions(lw);
        return;
    }

    /* Collect the tasks of the current view.                                */
    GPtrArray *tasks;                /* BtTask* rows to show                */
    gboolean virtual_view = TRUE;    /* show the "in <list>" line           */
    const gchar *view_name = "";
    switch (lw->sel_kind) {
    case SB_KIND_PINNED:
        tasks = bt_db_tasks_pinned(lw->app->db);
        view_name = "Pinned Tasks";
        break;
    case SB_KIND_ALL:
        tasks = bt_db_tasks_all_visible(lw->app->db);
        view_name = "All Tasks";
        break;
    case SB_KIND_TODAY: {
        gint64 lo, hi;
        bt_day_bounds(0, &lo, &hi);
        tasks = bt_db_tasks_due_between(lw->app->db, lo, hi);
        view_name = "Due Today";
        break;
    }
    case SB_KIND_TOMORROW: {
        gint64 lo, hi;
        bt_day_bounds(1, &lo, &hi);
        tasks = bt_db_tasks_due_between(lw->app->db, lo, hi);
        view_name = "Due Tomorrow";
        break;
    }
    case SB_KIND_LIST:
    default:
        tasks = bt_db_tasks_toplevel(lw->app->db, lw->sel_id);
        virtual_view = FALSE;
        break;
    }

    /* One pass of shared lookups (avoid per-row queries).  Subtasks come
     * as ONE query grouped in memory, not one query per top-level row.      */
    GHashTable *att_counts = bt_db_attachment_counts(lw->app->db);
    GPtrArray *all_subs = bt_db_subtasks_all_visible(lw->app->db);
    GHashTable *subs_by_parent =     /* parent id → GPtrArray of borrowed   */
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                              (GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < all_subs->len; i++) {
        BtTask *s = g_ptr_array_index(all_subs, i);
        GPtrArray *bucket = g_hash_table_lookup(subs_by_parent,
            GINT_TO_POINTER((gint)s->parent_id));
        if (bucket == NULL) {
            bucket = g_ptr_array_new();
            g_hash_table_insert(subs_by_parent,
                GINT_TO_POINTER((gint)s->parent_id), bucket);
        }
        g_ptr_array_add(bucket, s);
    }
    GHashTable *list_names = NULL;   /* list id → name (virtual views)      */
    if (virtual_view) {
        list_names = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                           NULL, g_free);
        GPtrArray *lists = bt_db_lists(lw->app->db, FALSE);
        for (guint i = 0; i < lists->len; i++) {
            BtList *l = g_ptr_array_index(lists, i);
            g_hash_table_insert(list_names,
                                GINT_TO_POINTER((gint)l->id),
                                g_strdup(l->name));
        }
        bt_ptr_array_free_lists(lists);
    }

    gboolean bold = bt_app_config_get_bool("bold_task_titles", FALSE);
    gboolean show_done = bt_app_config_get_bool("show_completed", TRUE);
    guint appended = 0;              /* rows actually in the pane           */
    for (guint i = 0; i < tasks->len; i++) {
        BtTask *t = g_ptr_array_index(tasks, i);
        if (!show_done && t->done)   /* toolbar completed-visibility toggle */
            continue;
        GPtrArray *subs = t->parent_id == 0
            ? g_hash_table_lookup(subs_by_parent,
                                  GINT_TO_POINTER((gint)t->id))
            : NULL;
        const gchar *list_name = virtual_view && list_names != NULL
            ? g_hash_table_lookup(list_names,
                                  GINT_TO_POINTER((gint)t->list_id))
            : NULL;
        gint att_count = GPOINTER_TO_INT(g_hash_table_lookup(att_counts,
            GINT_TO_POINTER((gint)t->id)));
        gchar *desc = task_desc_markup(t, list_name, att_count, subs,
                                       bold);
        gchar *due  = bt_due_format(t->due);
        GtkTreeIter iter;
        gtk_list_store_append(lw->task_store, &iter);
        gtk_list_store_set(lw->task_store, &iter,
                           TL_ID, t->id,
                           TL_DONE, t->done,
                           TL_DESC, desc,
                           TL_DUE, due,
                           TL_DUE_RAW, t->due,
                           TL_PINNED, t->pinned,
                           -1);
        g_free(desc);
        g_free(due);
        appended++;
    }

    /* Pinned Tasks also gathers pinned Blue Notes action items (their
     * pin state is local — the bn_pins table).                              */
    guint shown = appended;          /* rows in the pane (for the status)   */
    if (lw->sel_kind == SB_KIND_PINNED &&
        bt_app_config_get_bool("blue_notes_sync", FALSE)) {
        gint bn = append_bn_rows(lw, TRUE);
        if (bn > 0)
            shown += (guint)bn;
    }

    /* Status bar left: where we are + how many rows.                        */
    BtList *sel_list = virtual_view ? NULL
                       : bt_db_list_get(lw->app->db, lw->sel_id);
    gchar *loc = g_strdup_printf("%s \xe2\x80\x94 %u task%s",
                                 virtual_view    ? view_name
                                 : sel_list != NULL ? sel_list->name : "?",
                                 shown, shown == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(lw->status_left), loc);
    g_free(loc);
    bt_list_free(sel_list);

    g_hash_table_destroy(att_counts);
    g_hash_table_destroy(subs_by_parent);
    bt_ptr_array_free_tasks(all_subs);
    if (list_names != NULL)
        g_hash_table_destroy(list_names);
    bt_ptr_array_free_tasks(tasks);
}

/* full_refresh() — sidebar + task pane + open editors, plus the Sync
 * button's visibility (hidden while the Google master switch is off —
 * Settings fires a full notify when it flips).                              */
static void
full_refresh(BtLibrary *lw)
{
    refresh_sidebar(lw);
    refresh_tasks(lw);
    gtk_widget_set_visible(lw->sync_item,
        bt_app_config_get_bool("google_sync_enabled", TRUE));
    bt_editor_refresh_all(lw->app);
}

/* hide_done_icon_refresh() — point the completed-visibility toggle's
 * icon + tooltip at the ACTION it offers: hidden.png while completed
 * tasks are visible (click to hide them), visible.png while they are
 * hidden (click to bring them back).                                        */
static void
hide_done_icon_refresh(BtLibrary *lw)
{
    gboolean show = bt_app_config_get_bool("show_completed", TRUE);
    GtkWidget *icon = bt_app_icon_image_sized(lw->app,
        show ? "hidden" : "visible", 24);
    if (icon != NULL) {
        gtk_widget_show(icon);
        gtk_tool_button_set_icon_widget(
            GTK_TOOL_BUTTON(lw->hide_done_item), icon);
    }
    gtk_tool_item_set_tooltip_text(GTK_TOOL_ITEM(lw->hide_done_item),
        show ? "Hide completed tasks" : "Show completed tasks");
}

/* on_toggle_done_visible() — the toolbar toggle behind it: flip the
 * persisted show_completed flag and rebuild the task pane.                  */
static void
on_toggle_done_visible(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    gboolean show = !bt_app_config_get_bool("show_completed", TRUE);
    bt_app_config_set("show_completed", show ? "1" : "0");
    hide_done_icon_refresh(lw);
    refresh_tasks(lw);
}

/* notify_changed_hook() / notify_tasks_hook() / notify_status_hook() —
 * the BtApp hooks.                                                          */
static void
notify_changed_hook(BtApp *app)
{
    BtLibrary *lw = lib_of(app);
    if (lw != NULL)
        full_refresh(lw);
}

/* The light variant: task pane only (editor saves — see editor_notify).     */
static void
notify_tasks_hook(BtApp *app)
{
    BtLibrary *lw = lib_of(app);
    if (lw != NULL)
        refresh_tasks(lw);
}

static void
notify_status_hook(BtApp *app, const gchar *message)
{
    BtLibrary *lw = lib_of(app);
    if (lw != NULL)
        gtk_label_set_text(GTK_LABEL(lw->status_right), message);
}

/* ===========================================================================
 * Sidebar behavior.
 * =========================================================================== */

/* sb_row_selectable() — the "Lists" header row cannot be selected.          */
static gboolean
sb_row_selectable(GtkTreeSelection *sel, GtkTreeModel *model,
                  GtkTreePath *path, gboolean selected, gpointer data)
{
    (void)sel; (void)selected; (void)data;
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return FALSE;
    gint kind;
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
    return kind != SB_KIND_HEADER;
}

/* on_sidebar_changed() — selection drives the task pane.                    */
static void
on_sidebar_changed(GtkTreeSelection *sel, gpointer data)
{
    BtLibrary *lw = data;
    if (lw->populating)
        return;
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;
    gtk_tree_model_get(model, &iter,
                       SB_KIND, &lw->sel_kind,
                       SB_ID, &lw->sel_id,
                       -1);
    refresh_tasks(lw);
}

/* selected_list_id() — the currently selected REAL list, or 0.              */
static gint64
selected_list_id(BtLibrary *lw)
{
    return lw->sel_kind == SB_KIND_LIST ? lw->sel_id : 0;
}

/* ===========================================================================
 * Sidebar drag & drop — reordering the real lists.
 *
 * The dest protocol is fully custom, mirroring Blue Notes (its quirk
 * #13): GtkTreeView's default drag-motion handler requests the row DATA
 * on every motion to validate the drop, and on quartz those replies
 * arrive before the release — a received handler treating every
 * delivery as a drop would finish the drag mid-air.  So the motion
 * handler answers gdk_drag_status() itself, ONLY the drop requests the
 * data, and the received handler is the one place the move happens.
 * =========================================================================== */

/* The one drag flavor: GtkTreeView rows within this app.                    */
static const GtkTargetEntry SB_ROW_TARGET =
    { (gchar *)"GTK_TREE_MODEL_ROW", GTK_TARGET_SAME_APP, 0 };

/* sb_drop_target() — resolve and validate the drop target under the
 * pointer.  Only a real list row may move (the dragged row is the
 * sidebar's selected row — a press always settles the single-mode
 * selection before the drag threshold), and only BEFORE/AFTER another
 * real list row; meta rows, the header, and the Blue Notes row take
 * part in neither end.  Lists cannot nest, so INTO positions are
 * coerced to the nearer edge.  Returns TRUE and fills `path_out`
 * (caller frees) + `pos_out` when the drop is legal.                        */
static gboolean
sb_drop_target(BtLibrary *lw, GdkDragContext *context, gint x, gint y,
               GtkTreePath **path_out, GtkTreeViewDropPosition *pos_out)
{
    *path_out = NULL;
    *pos_out  = GTK_TREE_VIEW_DROP_BEFORE;

    if (gtk_drag_get_source_widget(context) != lw->sb_view)
        return FALSE;                /* only the sidebar's own rows         */
    if (gtk_drag_dest_find_target(lw->sb_view, context, NULL) == GDK_NONE)
        return FALSE;                /* not a GTK_TREE_MODEL_ROW drag       */

    GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
    GtkTreeIter iter;

    /* The dragged row must be a real list.                                  */
    gint src_kind = -1;
    GtkTreePath *src_path = NULL;
    GtkTreeModel *m;
    if (gtk_tree_selection_get_selected(
            gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->sb_view)),
            &m, &iter)) {
        gtk_tree_model_get(m, &iter, SB_KIND, &src_kind, -1);
        src_path = gtk_tree_model_get_path(m, &iter);
    }
    if (src_kind != SB_KIND_LIST || src_path == NULL) {
        if (src_path != NULL)
            gtk_tree_path_free(src_path);
        return FALSE;
    }

    /* ... and the row under the pointer another real list.                  */
    GtkTreePath *path = NULL;        /* row under the pointer               */
    GtkTreeViewDropPosition pos = GTK_TREE_VIEW_DROP_BEFORE;
    gboolean ok = FALSE;
    if (gtk_tree_view_get_dest_row_at_pos(GTK_TREE_VIEW(lw->sb_view),
                                          x, y, &path, &pos)) {
        gint kind = -1;
        if (gtk_tree_model_get_iter(model, &iter, path))
            gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
        ok = kind == SB_KIND_LIST &&
             gtk_tree_path_compare(src_path, path) != 0;
        if (pos == GTK_TREE_VIEW_DROP_INTO_OR_BEFORE)
            pos = GTK_TREE_VIEW_DROP_BEFORE;
        else if (pos == GTK_TREE_VIEW_DROP_INTO_OR_AFTER)
            pos = GTK_TREE_VIEW_DROP_AFTER;
    }
    gtk_tree_path_free(src_path);
    if (ok) {
        *path_out = path;
        *pos_out  = pos;
    } else if (path != NULL) {
        gtk_tree_path_free(path);
    }
    return ok;
}

/* on_sb_drag_motion() — answer the status ourselves and draw the drop
 * indicator; returning TRUE keeps the data-requesting default handler
 * out (the quartz hazard above).                                            */
static gboolean
on_sb_drag_motion(GtkWidget *widget, GdkDragContext *context,
                  gint x, gint y, guint time, gpointer data)
{
    BtLibrary *lw = data;
    GtkTreePath *path = NULL;        /* legal target row (or NULL)          */
    GtkTreeViewDropPosition pos;     /* indicator position                  */
    gboolean ok = sb_drop_target(lw, context, x, y, &path, &pos);
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget),
                                    ok ? path : NULL, pos);
    gdk_drag_status(context, ok ? GDK_ACTION_MOVE : 0, time);
    if (path != NULL)
        gtk_tree_path_free(path);
    return TRUE;
}

/* on_sb_drag_leave() — clear the drop indicator (also fires right
 * before every drop).                                                       */
static void
on_sb_drag_leave(GtkWidget *widget, GdkDragContext *context, guint time,
                 gpointer data)
{
    (void)context; (void)time; (void)data;
    gtk_tree_view_set_drag_dest_row(GTK_TREE_VIEW(widget), NULL,
                                    GTK_TREE_VIEW_DROP_BEFORE);
}

/* on_sb_drag_drop() — the button was released on a legal target:
 * request the row data (the move itself runs in on_sb_drag_received,
 * the only place the dragged row can be decoded).  TRUE keeps the
 * default handler out; FALSE cancels a targetless drop cleanly.             */
static gboolean
on_sb_drag_drop(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
                guint time, gpointer data)
{
    BtLibrary *lw = data;
    GtkTreePath *path = NULL;        /* legal target row (or NULL)          */
    GtkTreeViewDropPosition pos;     /* unused here                         */
    gboolean ok = sb_drop_target(lw, context, x, y, &path, &pos);
    if (path != NULL)
        gtk_tree_path_free(path);
    if (!ok)
        return FALSE;
    gtk_drag_get_data(widget, context,
                      gdk_atom_intern_static_string("GTK_TREE_MODEL_ROW"),
                      time);
    return TRUE;
}

/* on_sb_drag_received() — the drop: splice the dragged list before/
 * after the anchor in the CURRENT display order and persist the whole
 * sequence (bt_db_lists_reorder — also flips the ordering from the
 * alphabetical default to custom).  Fires exactly once per drop — only
 * on_sb_drag_drop requests the data, so x/y are real drop coordinates.
 * The default handler is stopped: it would try to splice the dragged
 * row into the tree store itself.                                           */
static void
on_sb_drag_received(GtkWidget *widget, GdkDragContext *context,
                    gint x, gint y, GtkSelectionData *seldata, guint info,
                    guint time, gpointer data)
{
    (void)info;
    BtLibrary *lw = data;
    g_signal_stop_emission_by_name(widget, "drag-data-received");

    GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
    GtkTreeModel *src_model = NULL;  /* model the drag started in           */
    GtkTreePath *src_path = NULL;    /* dragged row's path                  */
    GtkTreePath *dest_path = NULL;   /* target row's path                   */
    GtkTreeViewDropPosition pos = GTK_TREE_VIEW_DROP_BEFORE;
    GtkTreeIter iter;
    gint64 dragged = 0;              /* dragged list id                     */
    gint64 anchor = 0;               /* target list id                      */

    if (gtk_tree_get_row_drag_data(seldata, &src_model, &src_path) &&
        src_model == model &&
        sb_drop_target(lw, context, x, y, &dest_path, &pos)) {
        if (gtk_tree_model_get_iter(model, &iter, src_path))
            gtk_tree_model_get(model, &iter, SB_ID, &dragged, -1);
        if (gtk_tree_model_get_iter(model, &iter, dest_path))
            gtk_tree_model_get(model, &iter, SB_ID, &anchor, -1);
    }

    gboolean success = dragged != 0 && anchor != 0 && dragged != anchor;
    if (success) {
        gboolean after = pos == GTK_TREE_VIEW_DROP_AFTER;
        /* Current display order minus the dragged id, re-inserted at
         * the anchor.                                                       */
        GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
        GPtrArray *lists = bt_db_lists(lw->app->db, FALSE);
        for (guint i = 0; i < lists->len; i++) {
            BtList *l = g_ptr_array_index(lists, i);
            if (l->id == dragged)
                continue;            /* re-inserted at the anchor below     */
            if (l->id == anchor && !after)
                g_array_append_val(ids, dragged);
            g_array_append_val(ids, l->id);
            if (l->id == anchor && after)
                g_array_append_val(ids, dragged);
        }
        bt_ptr_array_free_lists(lists);
        bt_db_lists_reorder(lw->app->db, (const gint64 *)ids->data,
                            ids->len);
        g_array_free(ids, TRUE);
        refresh_sidebar(lw);         /* reselects the dragged list          */
    }

    if (src_path != NULL)
        gtk_tree_path_free(src_path);
    if (dest_path != NULL)
        gtk_tree_path_free(dest_path);
    gtk_drag_finish(context, success, FALSE, time);
}

/* ===========================================================================
 * Task pane behavior.
 * =========================================================================== */

/* selected_task_ids() — ids of every selected task row (the view is
 * multi-select: Ctrl/Cmd-click and Shift-click extend).  Free with
 * g_array_unref.  Blue Notes rows (id 0) are excluded.                      */
static GArray *
selected_task_ids(BtLibrary *lw)
{
    GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->task_view));
    GtkTreeModel *model = NULL;
    GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
    for (GList *l = rows; l != NULL; l = l->next) {
        GtkTreeIter iter;
        if (gtk_tree_model_get_iter(model, &iter, l->data)) {
            gint64 id;
            gtk_tree_model_get(model, &iter, TL_ID, &id, -1);
            if (id != 0)
                g_array_append_val(ids, id);
        }
    }
    g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);
    return ids;
}

/* on_task_activated() — double-click opens the editor window; Blue
 * Notes rows open the reduced Blue Notes editor (done + due editable).      */
static void
on_task_activated(GtkTreeView *view, GtkTreePath *path,
                  GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    BtLibrary *lw = data;
    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;
    /* A Blue Notes row (they carry a ref and id 0 — the Action Items
     * list AND pinned items in the Pinned Tasks view) opens the reduced
     * Blue Notes editor.                                                    */
    gchar *ref = NULL;
    gtk_tree_model_get(model, &iter, TL_REF, &ref, -1);
    if (ref != NULL) {
        bt_editor_open_bnote(lw->app, ref);
        g_free(ref);
        return;
    }
    gint64 id;
    gtk_tree_model_get(model, &iter, TL_ID, &id, -1);
    bt_editor_open(lw->app, id);
}

/* on_task_done_toggled() / on_task_pinned_toggled() — the two toggle
 * columns write straight through to the row.                                */
static void
on_task_done_toggled(GtkCellRendererToggle *cell, gchar *path_str,
                     gpointer data)
{
    (void)cell;
    BtLibrary *lw = data;
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
    if (!gtk_tree_model_get_iter_from_string(model, &iter, path_str))
        return;
    gint64 id;
    gboolean done;
    gtk_tree_model_get(model, &iter, TL_ID, &id, TL_DONE, &done, -1);

    /* Blue Notes rows (any view they appear in) write back through the
     * blue_notes CLI, which strikes/un-strikes the '!' line in the note
     * itself.                                                               */
    gchar *ref = NULL;
    gtk_tree_model_get(model, &iter, TL_REF, &ref, -1);
    if (ref != NULL) {
        gchar *err = NULL;
        if (bt_bnotes_action_set_done(ref, !done, &err)) {
            bt_app_status(lw->app, "Updated in Blue Notes");
            full_refresh(lw);        /* incl. an open editor of this ref    */
        } else {
            bt_app_status(lw->app, "%s",
                          err != NULL ? err : "update failed");
        }
        g_free(err);
        g_free(ref);
        return;
    }
    bt_db_task_set_done(lw->app->db, id, !done);
    full_refresh(lw);
}

static void
on_task_pinned_toggled(GtkCellRendererToggle *cell, gchar *path_str,
                       gpointer data)
{
    (void)cell;
    BtLibrary *lw = data;
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
    if (!gtk_tree_model_get_iter_from_string(model, &iter, path_str))
        return;
    gint64 id;
    gboolean pinned;
    gchar *ref = NULL;
    gtk_tree_model_get(model, &iter, TL_ID, &id, TL_PINNED, &pinned,
                       TL_REF, &ref, -1);
    if (ref != NULL) {
        /* Blue Notes row: the pin lives in the local bn_pins table
         * (Blue Notes itself has no pin concept).                          */
        bt_db_bn_pin_set(lw->app->db, ref, !pinned);
        g_free(ref);
    } else {
        bt_db_task_set_pinned(lw->app->db, id, !pinned);
    }
    full_refresh(lw);
}

/* task_row_bg_func() — cell data function giving list rows alternating
 * white / light-blue backgrounds regardless of theme (the Blue Notes
 * notes-list stripes).                                                      */
static void
task_row_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                 GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col; (void)data;
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gboolean even =                  /* row parity drives the tint          */
        (gtk_tree_path_get_indices(path)[0] % 2) == 0;
    gtk_tree_path_free(path);
    g_object_set(cell,
                 "cell-background", even ? NULL : ROW_TINT,
                 NULL);
}

/* due_color_func() — tint the Due cell by urgency at draw time (rolls
 * over at midnight).  Undated rows must reset foreground-set — the
 * renderer is shared.  Also applies the row stripe: a column gets ONE
 * cell data func per renderer, so this one does both jobs.                  */
static void
due_color_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
               GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    task_row_bg_func(col, cell, model, iter, data);
    gint64 due;
    gtk_tree_model_get(model, iter, TL_DUE_RAW, &due, -1);
    const gchar *color = bt_due_color(due);
    if (color == NULL)
        g_object_set(cell, "foreground-set", FALSE, NULL);
    else
        g_object_set(cell, "foreground", color, NULL);
}

/* sort_by_due() — soonest first; undated rows always last.                  */
static gint
sort_by_due(GtkTreeModel *model, GtkTreeIter *a, GtkTreeIter *b,
            gpointer data)
{
    (void)data;
    gint64 da, db;
    gtk_tree_model_get(model, a, TL_DUE_RAW, &da, -1);
    gtk_tree_model_get(model, b, TL_DUE_RAW, &db, -1);
    if (da == 0) da = G_MAXINT64;
    if (db == 0) db = G_MAXINT64;
    return (da > db) - (da < db);
}

/* ===========================================================================
 * Toolbar actions.
 * =========================================================================== */

/* on_toggle_sidebar() — toolbar show/hide button for the lists pane:
 * the task view takes the whole window while it is hidden (mirrors the
 * Blue Notes "Folders" toggle).                                             */
static void
on_toggle_sidebar(GtkWidget *widget, gpointer data)
{
    (void)widget;
    BtLibrary *lw = data;
    gboolean show = !gtk_widget_get_visible(lw->sidebar_box);
    gtk_widget_set_visible(lw->sidebar_box, show);
    bt_app_config_set("sidebar_visible", show ? "1" : "0");
}

/* on_emoji_chooser_closed() — picker dismissed: shrink the dialog back
 * to its natural size (see on_emoji_box_pressed).                           */
static void
on_emoji_chooser_closed(GtkPopover *chooser, gpointer dlg)
{
    (void)chooser;
    gtk_window_resize(GTK_WINDOW(dlg), 1, 1);
}

/* emoji_open_idle() — open the chooser AFTER the dialog's grow-resize
 * has landed, so the popover measures against the enlarged window.          */
static gboolean
emoji_open_idle(gpointer entry)
{
    g_signal_emit_by_name(entry, "insert-emoji");

    /* GtkEntry keeps its chooser as "gtk-emoji-chooser" object data;
     * hook its close (once) to give the dialog its size back.               */
    GtkWidget *chooser =
        g_object_get_data(G_OBJECT(entry), "gtk-emoji-chooser");
    GtkWidget *dlg = g_object_get_data(G_OBJECT(entry), "bt-dialog");
    if (chooser != NULL && dlg != NULL &&
        g_object_get_data(G_OBJECT(chooser), "bt-close-hooked") == NULL) {
        g_signal_connect(chooser, "closed",
                         G_CALLBACK(on_emoji_chooser_closed), dlg);
        g_object_set_data(G_OBJECT(chooser), "bt-close-hooked",
                          GINT_TO_POINTER(1));
    }
    return G_SOURCE_REMOVE;
}

/* on_emoji_box_pressed() — clicking the emoji box opens GTK's emoji
 * chooser on the entry (clearing any previous pick, so choosing always
 * replaces).  GTK3 popovers render INSIDE their toplevel and clip at
 * its edges, so the dialog is grown first to give the chooser room; it
 * shrinks back to natural size when the chooser closes.                     */
static gboolean
on_emoji_box_pressed(GtkWidget *entry, GdkEventButton *event,
                     gpointer data)
{
    (void)event; (void)data;
    gtk_entry_set_text(GTK_ENTRY(entry), "");
    GtkWidget *dlg = g_object_get_data(G_OBJECT(entry), "bt-dialog");
    if (dlg != NULL) {
        gint w, h;                   /* current dialog frame                */
        gtk_window_get_size(GTK_WINDOW(dlg), &w, &h);
        gtk_window_resize(GTK_WINDOW(dlg), MAX(w, 440), 470);
    }
    g_idle_add(emoji_open_idle, entry);
    return TRUE;                     /* the chooser owns this click         */
}

/* ---------------------------------------------------------------------------
 * run_list_dialog() — the shared New List / Edit List dialog: an emoji
 * box (click opens the picker) and a name entry, prefilled from the
 * name/emoji in-out parameters when editing.  On OK with a non-empty
 * name the trimmed values replace them (caller g_frees) and TRUE
 * returns.
 * ------------------------------------------------------------------------- */
static gboolean
run_list_dialog(BtLibrary *lw, const gchar *title,
                gchar **name, gchar **emoji)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(title,
        GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL, "_OK", GTK_RESPONSE_OK, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_OK);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);

    GtkWidget *emoji_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(emoji_row),
                       gtk_label_new("List Emoji:"), FALSE, FALSE, 0);
    GtkWidget *emoji_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(emoji_entry), 2);
    gtk_entry_set_max_length(GTK_ENTRY(emoji_entry), 4);
    gtk_entry_set_alignment(GTK_ENTRY(emoji_entry), 0.5f);
    gtk_widget_set_halign(emoji_entry, GTK_ALIGN_START);
    bt_app_widget_add_css(emoji_entry, "entry { font-size: 18px; }");
    gtk_widget_set_tooltip_text(emoji_entry,
        "Optional emoji \xe2\x80\x94 click to pick");
    if (*emoji != NULL)
        gtk_entry_set_text(GTK_ENTRY(emoji_entry), *emoji);
    g_signal_connect(emoji_entry, "button-press-event",
                     G_CALLBACK(on_emoji_box_pressed), NULL);
    gtk_box_pack_start(GTK_BOX(emoji_row), emoji_entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), emoji_row, FALSE, FALSE, 0);

    GtkWidget *name_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(name_row), gtk_label_new("List name:"),
                       FALSE, FALSE, 0);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(name_entry), 28);
    gtk_entry_set_activates_default(GTK_ENTRY(name_entry), TRUE);
    if (*name != NULL)
        gtk_entry_set_text(GTK_ENTRY(name_entry), *name);
    gtk_box_pack_start(GTK_BOX(name_row), name_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), name_row, FALSE, FALSE, 0);

    /* The click handler grows the dialog so the chooser popover fits.       */
    g_object_set_data(G_OBJECT(emoji_entry), "bt-dialog", dlg);

    gtk_box_pack_start(
        GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
        box, TRUE, TRUE, 0);
    gtk_widget_grab_focus(name_entry);
    gtk_widget_show_all(dlg);

    gboolean ok = FALSE;             /* accepted with a usable name         */
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_OK) {
        gchar *new_name = g_strstrip(
            g_strdup(gtk_entry_get_text(GTK_ENTRY(name_entry))));
        gchar *new_emoji = g_strstrip(
            g_strdup(gtk_entry_get_text(GTK_ENTRY(emoji_entry))));
        if (*new_name != '\0') {
            g_free(*name);
            g_free(*emoji);
            *name = new_name;
            *emoji = new_emoji;
            ok = TRUE;
        } else {
            g_free(new_name);
            g_free(new_emoji);
        }
    }
    gtk_widget_destroy(dlg);
    return ok;
}

/* on_new_list() — prompt (name + optional emoji), create, select.           */
static void
on_new_list(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    gchar *name = NULL;              /* dialog in/out values                */
    gchar *emoji = NULL;
    if (run_list_dialog(lw, "New List", &name, &emoji)) {
        gint64 id = bt_db_list_create(lw->app->db, name, emoji);
        if (id == 0) {               /* write failed (logged by the db)     */
            bt_app_status(lw->app, "Could not create the list \xe2\x80\x94 "
                          "database write failed");
        } else {
            lw->sel_kind = SB_KIND_LIST;
            lw->sel_id = id;
            full_refresh(lw);
            bt_app_status(lw->app,
                          "Created list \xe2\x80\x9c%s\xe2\x80\x9d", name);
        }
    }
    g_free(name);
    g_free(emoji);
}

/* on_edit_list() — change the selected list's name and/or emoji.            */
static void
on_edit_list(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    if (lw->sel_kind == SB_KIND_BN_ACTIONS) {
        bt_app_status(lw->app,
                      "This list mirrors Blue Notes and cannot be edited");
        return;
    }
    gint64 id = selected_list_id(lw);
    if (id == 0) {
        bt_app_status(lw->app, "Select a list to edit");
        return;
    }
    BtList *l = bt_db_list_get(lw->app->db, id);
    if (l == NULL)
        return;
    gchar *name  = g_strdup(l->name);
    gchar *emoji = g_strdup(l->emoji);
    bt_list_free(l);
    if (run_list_dialog(lw, "Edit List", &name, &emoji)) {
        bt_db_list_update(lw->app->db, id, name, emoji);
        full_refresh(lw);
        bt_app_status(lw->app,
                      "Updated list \xe2\x80\x9c%s\xe2\x80\x9d", name);
    }
    g_free(name);
    g_free(emoji);
}

/* on_delete_list() — confirm + tombstone the selected real list.            */
static void
on_delete_list(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    if (lw->sel_kind == SB_KIND_BN_ACTIONS) {
        bt_app_status(lw->app, "This list mirrors Blue Notes and cannot "
                      "be deleted \xe2\x80\x94 disable it in File \xe2\x86"
                      "\x92 Settings\xe2\x80\xa6");
        return;
    }
    gint64 id = selected_list_id(lw);
    if (id == 0) {
        bt_app_status(lw->app, "Select a list to delete");
        return;
    }
    BtList *l = bt_db_list_get(lw->app->db, id);
    if (l == NULL)
        return;
    /* Google's default tasklist cannot be deleted (the API refuses with
     * 400 from any client) — block it here, like the Blue Notes list.       */
    gchar *default_gid = bt_db_state_get(lw->app->db, "default_list_gid");
    if (l->gtasks_id != NULL && default_gid != NULL &&
        strcmp(l->gtasks_id, default_gid) == 0) {
        bt_app_status(lw->app, "\xe2\x80\x9c%s\xe2\x80\x9d is Google's "
                      "default list and cannot be deleted", l->name);
        g_free(default_gid);
        bt_list_free(l);
        return;
    }
    g_free(default_gid);
    gboolean yes = bt_app_confirm(GTK_WINDOW(lw->window), "Delete List",
        "Delete the list \xe2\x80\x9c%s\xe2\x80\x9d and all of its "
        "tasks?", l->name);
    if (yes) {
        bt_db_list_delete(lw->app->db, id);
        lw->sel_kind = SB_KIND_LIST;
        lw->sel_id = 0;              /* falls back to the first list        */
        full_refresh(lw);
        bt_app_status(lw->app,
                      "Deleted list \xe2\x80\x9c%s\xe2\x80\x9d", l->name);
    }
    bt_list_free(l);
}

/* on_new_task() — create an empty task in the selected list and open its
 * editor.  The virtual views cannot hold new tasks.                         */
static void
on_new_task(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    gint64 list_id = selected_list_id(lw);
    if (list_id == 0) {
        bt_app_status(lw->app,
                      "Select a list first \xe2\x80\x94 tasks cannot be "
                      "created in the virtual views");
        return;
    }
    gint64 id = bt_db_task_create(lw->app->db, list_id, 0, "New Task");
    if (id == 0) {                   /* write failed (logged by the db)     */
        bt_app_status(lw->app, "Could not create the task \xe2\x80\x94 "
                      "database write failed");
        return;
    }
    full_refresh(lw);
    bt_editor_open(lw->app, id);
}

/* on_delete_task() — confirm + tombstone the selected task.                 */
static void
on_delete_task(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    if (lw->sel_kind == SB_KIND_BN_ACTIONS) {
        bt_app_status(lw->app, "Blue Notes items are deleted by editing "
                      "the note in Blue Notes");
        return;
    }
    GArray *ids = selected_task_ids(lw);
    if (ids->len == 0) {
        bt_app_status(lw->app, "Select a task to delete");
        g_array_unref(ids);
        return;
    }

    gboolean yes;                    /* confirmed?                          */
    if (ids->len == 1) {
        BtTask *t = bt_db_task_get(lw->app->db,
                                   g_array_index(ids, gint64, 0));
        if (t == NULL) {
            g_array_unref(ids);
            return;
        }
        yes = bt_app_confirm(GTK_WINDOW(lw->window), "Delete Task",
            "Delete \xe2\x80\x9c%s\xe2\x80\x9d%s?",
            *t->title != '\0' ? t->title : "Untitled Task",
            t->parent_id == 0 ? " and its subtasks" : "");
        bt_task_free(t);
    } else {
        yes = bt_app_confirm(GTK_WINDOW(lw->window), "Delete Tasks",
            "Delete the %u selected tasks (and their subtasks)?",
            ids->len);
    }
    if (yes) {
        for (guint i = 0; i < ids->len; i++) {
            gint64 id = g_array_index(ids, gint64, i);
            GtkWindow *editor =
                g_hash_table_lookup(lw->app->editors, &id);
            if (editor != NULL)
                gtk_widget_destroy(GTK_WIDGET(editor));
            bt_db_task_delete(lw->app->db, id);
        }
        full_refresh(lw);
        bt_app_status(lw->app, "Deleted %u task%s", ids->len,
                      ids->len == 1 ? "" : "s");
    }
    g_array_unref(ids);
}

/* ===========================================================================
 * Task context menu — Open in Google Tasks, Move to List, Delete.
 * =========================================================================== */

static GtkWidget *menu_item(GtkWidget *menu, const gchar *label,
                            GCallback cb, gpointer data);

/* on_ctx_open_google() — open the row's webViewLink in the browser.         */
static void
on_ctx_open_google(GtkWidget *item, gpointer data)
{
    BtLibrary *lw = data;
    const gchar *url = g_object_get_data(G_OBJECT(item), "bt-url");
    if (url == NULL)
        return;
    GError *gerr = NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(lw->window), url,
                                GDK_CURRENT_TIME, &gerr)) {
        bt_app_status(lw->app, "Cannot open browser: %s",
                      gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
    }
}

/* item_ids() — the gint64 id array stashed on a context-menu item.          */
static GArray *
item_ids(GtkWidget *item)
{
    return g_object_get_data(G_OBJECT(item), "bt-ids");
}

/* on_ctx_set_done() — Mark Complete / Mark Incomplete on the selection.     */
static void
on_ctx_set_done(GtkWidget *item, gpointer data)
{
    BtLibrary *lw = data;
    GArray *ids = item_ids(item);
    gboolean done = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "bt-done"));
    for (guint i = 0; i < ids->len; i++)
        bt_db_task_set_done(lw->app->db,
                            g_array_index(ids, gint64, i), done);
    full_refresh(lw);
    bt_app_status(lw->app, "Marked %u task%s %s", ids->len,
                  ids->len == 1 ? "" : "s",
                  done ? "complete" : "incomplete");
}

/* ctx_done_item() — one Mark (All) Complete / Incomplete context-menu
 * item: the selection rides on the item as its own g_array_ref, the
 * complete/incomplete flag as "bt-done".                                    */
static void
ctx_done_item(BtLibrary *lw, GtkWidget *menu, GArray *ids,
              gboolean single, gboolean done)
{
    GtkWidget *item = gtk_menu_item_new_with_label(
        done ? (single ? "Mark Complete"   : "Mark All Complete")
             : (single ? "Mark Incomplete" : "Mark All Incomplete"));
    g_object_set_data_full(G_OBJECT(item), "bt-ids", g_array_ref(ids),
                           (GDestroyNotify)g_array_unref);
    g_object_set_data(G_OBJECT(item), "bt-done", GINT_TO_POINTER(done));
    g_signal_connect(item, "activate", G_CALLBACK(on_ctx_set_done), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* on_ctx_move() — a destination picked in the Move to List menu: move
 * every selected TOP-LEVEL task not already there (subtasks travel with
 * their parents; a selected subtask on its own cannot move).                */
static void
on_ctx_move(GtkWidget *item, gpointer data)
{
    BtLibrary *lw = data;
    GArray *ids = item_ids(item);
    gint64 dest_id = *(gint64 *)g_object_get_data(G_OBJECT(item),
                                                  "bt-dest-id");
    guint moved = 0;                 /* how many actually went              */
    for (guint i = 0; i < ids->len; i++) {
        gint64 id = g_array_index(ids, gint64, i);
        BtTask *t = bt_db_task_get(lw->app->db, id);
        if (t != NULL && t->parent_id == 0 && t->list_id != dest_id) {
            bt_gtasks_move_task(lw->app, id, dest_id);
            moved++;
        }
        bt_task_free(t);
    }
    if (moved > 0) {
        full_refresh(lw);
        bt_app_status(lw->app, "Moved %u task%s", moved,
                      moved == 1 ? "" : "s");
    } else {
        bt_app_status(lw->app, "Nothing to move (subtasks move with "
                      "their parent task)");
    }
}

/* ---------------------------------------------------------------------------
 * on_task_button_press() — right-click on a task row: keep an existing
 * multi-selection when clicked inside it (else select just that row)
 * and show the context menu, whose actions apply to the whole
 * selection.  Not offered in the Blue Notes view.
 * ------------------------------------------------------------------------- */
static gboolean
on_task_button_press(GtkWidget *view, GdkEventButton *event, gpointer data)
{
    BtLibrary *lw = data;
    if (event->button != 3 || lw->sel_kind == SB_KIND_BN_ACTIONS)
        return FALSE;

    GtkTreePath *path = NULL;
    if (!gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view),
                                       (gint)event->x, (gint)event->y,
                                       &path, NULL, NULL, NULL))
        return FALSE;
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(view));
    if (!gtk_tree_selection_path_is_selected(sel, path)) {
        gtk_tree_selection_unselect_all(sel);
        gtk_tree_selection_select_path(sel, path);
    }
    gtk_tree_path_free(path);

    GArray *ids = selected_task_ids(lw);
    if (ids->len == 0) {
        g_array_unref(ids);
        return FALSE;
    }
    gboolean single = ids->len == 1;
    BtTask *t = single
        ? bt_db_task_get(lw->app->db, g_array_index(ids, gint64, 0))
        : NULL;

    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), view, NULL);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    /* Mark Complete / Mark Incomplete — the bulk staples.                   */
    ctx_done_item(lw, menu, ids, single, TRUE);
    ctx_done_item(lw, menu, ids, single, FALSE);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    /* Open in Google Tasks — single, synced task only.                      */
    GtkWidget *open_item =
        gtk_menu_item_new_with_label("Open in Google Tasks");
    if (single && t != NULL && t->web_link != NULL) {
        g_object_set_data_full(G_OBJECT(open_item), "bt-url",
                               g_strdup(t->web_link), g_free);
        g_signal_connect(open_item, "activate",
                         G_CALLBACK(on_ctx_open_google), lw);
    } else {
        gtk_widget_set_sensitive(open_item, FALSE);
    }
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), open_item);

    /* Move to List — applies to the selection's top-level tasks.            */
    GtkWidget *move_item = gtk_menu_item_new_with_label("Move to List");
    GtkWidget *submenu = gtk_menu_new();
    GPtrArray *lists = bt_db_lists(lw->app->db, FALSE);
    guint added = 0;                 /* destinations offered                */
    for (guint i = 0; i < lists->len; i++) {
        BtList *l = g_ptr_array_index(lists, i);
        /* For a single selection its own list is pointless; keep every
         * destination for multi (rows may span lists in virtual views).     */
        if (single && t != NULL && l->id == t->list_id)
            continue;
        gchar *label = list_label(l);
        GtkWidget *dest = gtk_menu_item_new_with_label(label);
        g_free(label);
        gint64 *did = g_new(gint64, 1);
        *did = l->id;
        g_object_set_data_full(G_OBJECT(dest), "bt-dest-id", did,
                               g_free);
        g_object_set_data_full(G_OBJECT(dest), "bt-ids",
                               g_array_ref(ids),
                               (GDestroyNotify)g_array_unref);
        g_signal_connect(dest, "activate",
                         G_CALLBACK(on_ctx_move), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(submenu), dest);
        added++;
    }
    bt_ptr_array_free_lists(lists);
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(move_item), submenu);
    gtk_widget_set_sensitive(move_item, added > 0 &&
        !(single && t != NULL && t->parent_id != 0));
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), move_item);

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());
    gchar *del_label = single
        ? g_strdup("Delete Task")
        : g_strdup_printf("Delete %u Tasks", ids->len);
    menu_item(menu, del_label, G_CALLBACK(on_delete_task), lw);
    g_free(del_label);

    bt_task_free(t);
    g_array_unref(ids);
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

/* sync_done() — re-enable the Sync button after a run.  The library is
 * re-resolved through the app context: the completion idle can fire
 * AFTER the window was closed and its BtLibrary freed, so a captured lw
 * pointer would dangle (the settings window guards the same way).           */
static void
sync_done(BtApp *app, gboolean ok, const gchar *message, gpointer data)
{
    (void)ok; (void)message; (void)data;
    BtLibrary *lw = lib_of(app);
    if (lw != NULL)
        gtk_widget_set_sensitive(lw->sync_item, TRUE);
}

/* sync_after_signin() — the Sync button's browser flow finished: run the
 * actual sync, or report why not.  Same lifetime rule as sync_done.         */
static void
sync_after_signin(gboolean ok, const gchar *error, gpointer data)
{
    BtApp *app = data;
    BtLibrary *lw = lib_of(app);
    if (lw == NULL)
        return;                      /* window closed mid-flow              */
    if (!ok)
        gtk_widget_set_sensitive(lw->sync_item, TRUE);
    bt_sync_signin_done(app, GTK_WINDOW(lw->window), lw->db_path,
                        ok, error, sync_done);
}

/* on_sync() — the toolbar Sync button.  Sign-in is per session: when the
 * in-memory token is missing/expired this re-runs the browser flow first
 * (usually a silent redirect), then syncs.                                  */
static void
on_sync(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    if (!bt_app_config_get_bool("google_sync_enabled", TRUE)) {
        bt_app_status(lw->app, "Google Tasks sync is disabled \xe2\x80\x94 "
                      "enable it in File \xe2\x86\x92 Settings\xe2\x80\xa6");
        return;
    }
    if (!bt_oauth_have_client()) {
        bt_app_status(lw->app, "Google sync is not configured \xe2\x80\x94 "
                      "see File \xe2\x86\x92 Settings\xe2\x80\xa6");
        bt_settings_window_open(lw->app, GTK_WINDOW(lw->window),
                                lw->db_path);
        return;
    }
    gtk_widget_set_sensitive(lw->sync_item, FALSE);
    if (bt_oauth_authenticated()) {
        bt_sync_start(lw->app, lw->db_path, sync_done, NULL);
    } else {
        bt_app_status(lw->app,
                      "Opening browser for Google sign-in\xe2\x80\xa6");
        bt_oauth_begin(GTK_WINDOW(lw->window), sync_after_signin,
                       lw->app);
    }
}

/* ===========================================================================
 * Menu actions.
 * =========================================================================== */

/* on_menu_settings() — File → Settings…                                     */
static void
on_menu_settings(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    bt_settings_window_open(lw->app, GTK_WINDOW(lw->window), lw->db_path);
}

/* on_menu_clear_completed() — File → Clear Completed Tasks: archive the
 * selected list's done tasks (Google's tasks.clear when synced).            */
static void
on_menu_clear_completed(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    gint64 id = selected_list_id(lw);
    if (id == 0) {
        bt_app_status(lw->app,
                      "Select a list to clear its completed tasks");
        return;
    }
    BtList *l = bt_db_list_get(lw->app->db, id);
    if (l == NULL)
        return;
    if (bt_app_confirm(GTK_WINDOW(lw->window), "Clear Completed",
                       "Remove all completed tasks from \xe2\x80\x9c%s"
                       "\xe2\x80\x9d?", l->name))
        bt_gtasks_clear_completed(lw->app, id);
    bt_list_free(l);
}

/* on_menu_about() — Help → About.                                           */
static void
on_menu_about(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    gtk_show_about_dialog(GTK_WINDOW(lw->window),
        "program-name", "Blue Tasks",
        "version", BT_VERSION,
        "comments", "Task lists with subtasks, due dates and Google "
                    "Tasks sync.\nCompanion app to Blue Notes.",
        "license-type", GTK_LICENSE_BSD_3,
        NULL);
}

/* on_menu_quit() — File → Quit.                                             */
static void
on_menu_quit(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    gtk_widget_destroy(lw->window);
}

/* menu_item() — build one wired menu item.                                  */
static GtkWidget *
menu_item(GtkWidget *menu, const gchar *label, GCallback cb, gpointer data)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_signal_connect(item, "activate", cb, data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    return item;
}

/* ---------------------------------------------------------------------------
 * bt_library_apply_native_menubar() — move the library menu into (or out
 * of) the native macOS menu bar (see header).  Mirrors Blue Notes: the
 * SAME menu shell drives the macOS bar — the in-window widget just has
 * to be hidden; leaving native mode hands macOS an empty bar so the app
 * menu stays functional.
 * ------------------------------------------------------------------------- */
void
bt_library_apply_native_menubar(BtApp *app, gboolean native)
{
#ifdef HAVE_GTKOSX
    if (app->library_window == NULL)
        return;
    GtkWidget *menubar =             /* the in-window GtkMenuBar            */
        g_object_get_data(G_OBJECT(app->library_window), "bt-menubar");
    if (menubar == NULL)
        return;

    GtkosxApplication *osx = gtkosx_application_get();
    if (native) {
        gtk_widget_hide(menubar);
        gtkosx_application_set_menu_bar(osx, GTK_MENU_SHELL(menubar));
    } else {
        gtk_widget_show(menubar);
        GtkWidget *empty = gtk_menu_bar_new();
        gtkosx_application_set_menu_bar(osx, GTK_MENU_SHELL(empty));
    }
    gtkosx_application_sync_menubar(osx);
#else
    (void)app; (void)native;
#endif
}

/* ===========================================================================
 * Construction.
 * =========================================================================== */

/* tool_button() — a style-aware toolbar button (local icon + label)
 * wired to `cb` and appended to `bar`.                                      */
static GtkToolItem *
tool_button(BtLibrary *lw, GtkToolbar *bar, const gchar *icon,
            const gchar *fallback_markup, const gchar *label,
            const gchar *tooltip, GCallback cb)
{
    GtkToolItem *item = bt_app_tool_item_new(lw->app, icon,
                                             fallback_markup, label,
                                             tooltip);
    g_signal_connect(item, "clicked", cb, lw);
    gtk_toolbar_insert(bar, item, -1);
    return item;
}

/* on_library_configure() — track the live client size for persistence.      */
static gboolean
on_library_configure(GtkWidget *w, GdkEventConfigure *event, gpointer data)
{
    (void)w; (void)event;
    BtLibrary *lw = data;
    gtk_window_get_size(GTK_WINDOW(lw->window), &lw->win_w, &lw->win_h);
    return FALSE;                    /* propagate                           */
}

/* on_library_destroy() — tear down: editors first (flushing saves).         */
static void
on_library_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    /* The closing size becomes the next launch's window size.               */
    if (lw->win_w > 0 && lw->win_h > 0) {
        gchar *v = g_strdup_printf("%d", lw->win_w);
        bt_app_config_set("win_w", v);
        g_free(v);
        v = g_strdup_printf("%d", lw->win_h);
        bt_app_config_set("win_h", v);
        g_free(v);
    }
    /* Hooks come down BEFORE the editors: a closing editor's final save
     * would otherwise fire notify_changed → bt_editor_refresh_all, which
     * can destroy sibling editors mid-teardown (a failing Blue Notes CLI
     * closes its editors on reload) and leave close_all's snapshot list
     * holding freed windows.                                                */
    lw->app->notify_changed = NULL;
    lw->app->notify_tasks   = NULL;
    lw->app->notify_status  = NULL;
    lw->app->library_window = NULL;
    bt_editor_close_all(lw->app);
    g_free(lw->db_path);
    g_free(lw);
}

/* ---------------------------------------------------------------------------
 * bt_library_window_new() — build the library window (see header).
 * ------------------------------------------------------------------------- */
GtkWidget *
bt_library_window_new(BtApp *app, const gchar *db_path)
{
    BtLibrary *lw = g_new0(BtLibrary, 1);
    lw->app = app;
    lw->db_path = g_strdup(db_path);
    lw->sel_kind = SB_KIND_LIST;     /* refresh falls back to first list    */

    lw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(lw->window), "Blue Tasks - Library");
    /* The last session's closing size (win_w/win_h), else the default.      */
    gchar *ww = bt_app_config_get("win_w");
    gchar *wh = bt_app_config_get("win_h");
    gint w = ww != NULL ? atoi(ww) : 0;
    gint hgt = wh != NULL ? atoi(wh) : 0;
    gtk_window_set_default_size(GTK_WINDOW(lw->window),
                                w > 0 ? w : 980, hgt > 0 ? hgt : 640);
    g_free(ww);
    g_free(wh);
    g_signal_connect(lw->window, "configure-event",
                     G_CALLBACK(on_library_configure), lw);
    gtk_application_add_window(app->gtk_app, GTK_WINDOW(lw->window));

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(lw->window), vbox);

    /* --- Menubar ---------------------------------------------------------- */
    GtkWidget *menubar = gtk_menu_bar_new();
    GtkWidget *file_menu = gtk_menu_new();
    GtkWidget *file_item = gtk_menu_item_new_with_label("File");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(file_item), file_menu);
    menu_item(file_menu, "Sync Now", G_CALLBACK(on_sync), lw);
    menu_item(file_menu, "Clear Completed Tasks",
              G_CALLBACK(on_menu_clear_completed), lw);
    menu_item(file_menu, "Settings\xe2\x80\xa6",
              G_CALLBACK(on_menu_settings), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    menu_item(file_menu, "Quit", G_CALLBACK(on_menu_quit), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);

    GtkWidget *help_menu = gtk_menu_new();
    GtkWidget *help_item = gtk_menu_item_new_with_label("Help");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(help_item), help_menu);
    menu_item(help_menu, "About Blue Tasks",
              G_CALLBACK(on_menu_about), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), help_item);
    /* Remembered so the menu can be moved into the native macOS menu
     * bar (see bt_library_apply_native_menubar).                            */
    g_object_set_data(G_OBJECT(lw->window), "bt-menubar", menubar);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    /* --- Toolbar ---------------------------------------------------------- */
    /* Icon names are icons/-relative paths; the curated set lives in
     * icons/ (case-exact for Linux).  Layout: sidebar toggle,
     * a drawn divider, then the task buttons and Sync.                      */
    GtkWidget *toolbar = gtk_toolbar_new();
    /* Small-toolbar metrics — the Blue Notes bar height.                    */
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);
    tool_button(lw, GTK_TOOLBAR(toolbar), "sidebar",
                "\xe2\x97\xa7", "Sidebar", "Show or hide the lists pane",
                G_CALLBACK(on_toggle_sidebar));
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    tool_button(lw, GTK_TOOLBAR(toolbar), "add2", NULL,
                "New Task", "Create a task in the selected list",
                G_CALLBACK(on_new_task));
    tool_button(lw, GTK_TOOLBAR(toolbar), "remove", NULL,
                "Delete Task", "Delete the selected task",
                G_CALLBACK(on_delete_task));

    lw->sync_item = GTK_WIDGET(tool_button(lw, GTK_TOOLBAR(toolbar),
        "google-symbol", "\xe2\x9f\xb3", "Sync",
        "Sync with Google Tasks now", G_CALLBACK(on_sync)));
    lw->hide_done_item = GTK_WIDGET(tool_button(lw, GTK_TOOLBAR(toolbar),
        "hidden", "\xf0\x9f\x91\x81", "Completed",
        "Hide completed tasks", G_CALLBACK(on_toggle_done_visible)));
    hide_done_icon_refresh(lw);      /* the persisted state's icon          */
    bt_app_register_toolbar(app, toolbar);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);
    /* Thin rule between the toolbar and the panes (Blue Notes look).        */
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 0);

    /* --- Paned: sidebar | tasks ------------------------------------------ */
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_position(GTK_PANED(paned), 220);
    gtk_box_pack_start(GTK_BOX(vbox), paned, TRUE, TRUE, 0);

    /* Sidebar.                                                              */
    lw->sb_store = gtk_tree_store_new(SB_N_COLS, G_TYPE_INT,
                                      G_TYPE_INT64, G_TYPE_STRING,
                                      G_TYPE_INT);
    lw->sb_view = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(lw->sb_store));
    g_object_unref(lw->sb_store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(lw->sb_view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(lw->sb_view), FALSE);
    GtkCellRenderer *sb_cell = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->sb_view),
        gtk_tree_view_column_new_with_attributes("Lists", sb_cell,
            "text", SB_LABEL, "weight", SB_WEIGHT, NULL));
    /* Sidebar palette (Blue Notes): light grey backdrop (rows and the
     * empty area below them — the tree view paints the whole widget),
     * muted grey text, and a blue selection bar with white text.            */
    bt_app_widget_add_css(lw->sb_view,
        "treeview.view {"
        "  background-color: rgb(230,230,230);"
        "  color: rgb(65,65,65);"
        "}"
        "treeview.view:selected {"
        "  background-color: rgb(86,131,224);"
        "  color: white;"
        "}");
    GtkTreeSelection *sb_sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->sb_view));
    gtk_tree_selection_set_select_function(sb_sel, sb_row_selectable,
                                           lw, NULL);
    g_signal_connect(sb_sel, "changed",
                     G_CALLBACK(on_sidebar_changed), lw);
    /* Let list rows be dragged to reorder them.  The dest protocol is
     * fully custom (motion answers the status itself; only the drop
     * requests the row data) — see the sidebar DnD banner.                  */
    gtk_tree_view_enable_model_drag_source(GTK_TREE_VIEW(lw->sb_view),
        GDK_BUTTON1_MASK, &SB_ROW_TARGET, 1, GDK_ACTION_MOVE);
    gtk_tree_view_enable_model_drag_dest(GTK_TREE_VIEW(lw->sb_view),
        &SB_ROW_TARGET, 1, GDK_ACTION_MOVE);
    g_signal_connect(lw->sb_view, "drag-motion",
                     G_CALLBACK(on_sb_drag_motion), lw);
    g_signal_connect(lw->sb_view, "drag-leave",
                     G_CALLBACK(on_sb_drag_leave), NULL);
    g_signal_connect(lw->sb_view, "drag-drop",
                     G_CALLBACK(on_sb_drag_drop), lw);
    g_signal_connect(lw->sb_view, "drag-data-received",
                     G_CALLBACK(on_sb_drag_received), lw);
    GtkWidget *sb_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sb_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sb_scroll), lw->sb_view);

    /* Mini action bar under the tree: compact create (+), edit (pencil)
     * and delete (minus) list buttons, right-aligned, blending into the
     * sidebar grey.                                                         */
    GtkWidget *sb_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    bt_app_widget_add_css(sb_actions,
        "box { background-color: rgb(230,230,230); }");
    GtkWidget *sb_add = gtk_button_new_with_label("+");
    gtk_widget_set_tooltip_text(sb_add, "Create a new task list");
    g_signal_connect(sb_add, "clicked", G_CALLBACK(on_new_list), lw);
    GtkWidget *sb_edit = gtk_button_new_with_label("\xe2\x9c\x8e");
    gtk_widget_set_tooltip_text(sb_edit,
        "Edit the selected list's name or emoji");
    g_signal_connect(sb_edit, "clicked", G_CALLBACK(on_edit_list), lw);
    GtkWidget *sb_del = gtk_button_new_with_label("\xe2\x88\x92");
    gtk_widget_set_tooltip_text(sb_del, "Delete the selected list");
    g_signal_connect(sb_del, "clicked", G_CALLBACK(on_delete_list), lw);
    const gchar *mini_css =
        "button { padding: 0 10px; min-height: 22px; "
        "border-radius: 0; }";
    bt_app_widget_add_css(sb_add, mini_css);
    bt_app_widget_add_css(sb_edit, mini_css);
    bt_app_widget_add_css(sb_del, mini_css);
    gtk_button_set_relief(GTK_BUTTON(sb_add), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(sb_edit), GTK_RELIEF_NONE);
    gtk_button_set_relief(GTK_BUTTON(sb_del), GTK_RELIEF_NONE);
    gtk_box_pack_end(GTK_BOX(sb_actions), sb_del, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(sb_actions), sb_edit, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(sb_actions), sb_add, FALSE, FALSE, 0);

    GtkWidget *sb_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(sb_box), sb_scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(sb_box), sb_actions, FALSE, FALSE, 0);
    gtk_paned_pack1(GTK_PANED(paned), sb_box, FALSE, FALSE);
    lw->sidebar_box = sb_box;        /* for the toolbar show/hide toggle    */

    /* Task pane.                                                            */
    lw->task_store = gtk_list_store_new(TL_N_COLS, G_TYPE_INT64,
                                        G_TYPE_BOOLEAN, G_TYPE_STRING,
                                        G_TYPE_STRING, G_TYPE_INT64,
                                        G_TYPE_BOOLEAN, G_TYPE_STRING);
    lw->task_view = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(lw->task_store));
    g_object_unref(lw->task_store);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(lw->task_view), FALSE);
    /* Multi-select: Ctrl-click (Cmd on macOS — GTK maps the platform's
     * modify-selection modifier) and Shift-click extend; the context
     * menu's actions apply to the whole selection.                          */
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->task_view)),
        GTK_SELECTION_MULTIPLE);
    g_signal_connect(lw->task_view, "row-activated",
                     G_CALLBACK(on_task_activated), lw);
    g_signal_connect(lw->task_view, "button-press-event",
                     G_CALLBACK(on_task_button_press), lw);

    /* Done checkbox column.  Every column's renderer also runs the
     * stripe data func — the alternating background must span the row.     */
    GtkCellRenderer *done_cell = gtk_cell_renderer_toggle_new();
    g_signal_connect(done_cell, "toggled",
                     G_CALLBACK(on_task_done_toggled), lw);
    GtkTreeViewColumn *cdone =
        gtk_tree_view_column_new_with_attributes("\xe2\x9c\x93",
            done_cell, "active", TL_DONE, NULL);
    gtk_tree_view_column_set_cell_data_func(cdone, done_cell,
                                            task_row_bg_func, NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdone);

    /* Task description column — the tall multi-line markup cell.            */
    GtkCellRenderer *desc_cell = gtk_cell_renderer_text_new();
    g_object_set(desc_cell,
                 "ypad", 8,
                 "ellipsize", PANGO_ELLIPSIZE_END,
                 NULL);
    GtkTreeViewColumn *cdesc =
        gtk_tree_view_column_new_with_attributes("Task", desc_cell,
            "markup", TL_DESC, NULL);
    gtk_tree_view_column_set_cell_data_func(cdesc, desc_cell,
                                            task_row_bg_func, NULL, NULL);
    gtk_tree_view_column_set_expand(cdesc, TRUE);
    gtk_tree_view_column_set_resizable(cdesc, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdesc);

    /* Due Date column, urgency-tinted, sortable (undated last).             */
    GtkCellRenderer *due_cell = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *cdue =
        gtk_tree_view_column_new_with_attributes("Due Date", due_cell,
            "text", TL_DUE, NULL);
    gtk_tree_view_column_set_cell_data_func(cdue, due_cell,
                                            due_color_func, NULL, NULL);
    gtk_tree_view_column_set_resizable(cdue, TRUE);
    gtk_tree_sortable_set_sort_func(
        GTK_TREE_SORTABLE(lw->task_store), TL_DUE_RAW,
        sort_by_due, NULL, NULL);
    gtk_tree_view_column_set_sort_column_id(cdue, TL_DUE_RAW);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdue);

    /* Pinned checkbox column.                                               */
    GtkCellRenderer *pin_cell = gtk_cell_renderer_toggle_new();
    g_signal_connect(pin_cell, "toggled",
                     G_CALLBACK(on_task_pinned_toggled), lw);
    GtkTreeViewColumn *cpin =
        gtk_tree_view_column_new_with_attributes("Pinned", pin_cell,
            "active", TL_PINNED, NULL);
    gtk_tree_view_column_set_cell_data_func(cpin, pin_cell,
                                            task_row_bg_func, NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cpin);

    GtkWidget *task_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(task_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(task_scroll), lw->task_view);
    gtk_paned_pack2(GTK_PANED(paned), task_scroll, TRUE, FALSE);

    /* --- Status bar -------------------------------------------------------- */
    /* Same geometry as the Blue Notes status bar: 8 px side margins,
     * 3 px top/bottom (a border_width would add a pixel more on every
     * edge and read visibly taller).                                        */
    GtkWidget *status = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_set_margin_start(status, 8);
    gtk_widget_set_margin_end(status, 8);
    gtk_widget_set_margin_top(status, 3);
    gtk_widget_set_margin_bottom(status, 3);
    lw->status_left = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_left),
                            PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(lw->status_left, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(status), lw->status_left, TRUE, TRUE, 0);
    lw->status_right = gtk_label_new("");
    gtk_label_set_ellipsize(GTK_LABEL(lw->status_right),
                            PANGO_ELLIPSIZE_END);
    gtk_widget_set_halign(lw->status_right, GTK_ALIGN_END);
    gtk_box_pack_end(GTK_BOX(status), lw->status_right, FALSE, FALSE, 0);
    /* Both labels a step smaller than the UI font (Blue Notes size).        */
    bt_app_widget_add_css(lw->status_left,  "label { font-size: 85%; }");
    bt_app_widget_add_css(lw->status_right, "label { font-size: 85%; }");
    gtk_box_pack_end(GTK_BOX(vbox), status, FALSE, FALSE, 0);
    /* Matching thin rule above the status bar.                              */
    gtk_box_pack_end(GTK_BOX(vbox),
                     gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                     FALSE, FALSE, 0);

    /* --- Hooks + first population ------------------------------------------ */
    app->library_window = lw->window;
    g_object_set_data(G_OBJECT(lw->window), "bt-library", lw);
    app->notify_changed = notify_changed_hook;
    app->notify_tasks   = notify_tasks_hook;
    app->notify_status  = notify_status_hook;
    g_signal_connect(lw->window, "destroy",
                     G_CALLBACK(on_library_destroy), lw);

    refresh_sidebar(lw);
    refresh_tasks(lw);
    gtk_widget_show_all(lw->window);
    /* The lists pane starts HIDDEN by default (toolbar Sidebar button
     * brings it back); the toggle persists the user's last choice.          */
    if (!bt_app_config_get_bool("sidebar_visible", FALSE))
        gtk_widget_hide(lw->sidebar_box);
    /* No Sync button while the Google master switch is off.                 */
    if (!bt_app_config_get_bool("google_sync_enabled", TRUE))
        gtk_widget_hide(lw->sync_item);
    return lw->window;
}
