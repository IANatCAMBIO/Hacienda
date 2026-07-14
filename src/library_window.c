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
    gint          sel_kind;
    gint64        sel_id;
    gboolean      populating;
    gboolean      sb_populated;      /* first population expands Lists      */
    gint          win_w, win_h;      /* live client size (persisted at
                                      * close as the next launch's size)    */
} BtLibrary;

static void refresh_sidebar(BtLibrary *lw);
static void refresh_tasks(BtLibrary *lw);

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
 * ------------------------------------------------------------------------- */
static gchar *
task_desc_markup(const BtTask *t, const gchar *list_name, gint att_count,
                 GPtrArray *subs)
{
    GString *s = g_string_new(NULL);
    gchar *title = g_markup_escape_text(
        *t->title != '\0' ? t->title : "Untitled Task", -1);
    gchar *line = t->done
        ? g_strdup_printf("<b><s>%s</s></b>", title)
        : g_strdup_printf("<b>%s</b>", title);
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
    } metas[] = {
        { SB_KIND_PINNED,   "Pinned Tasks" },
        { SB_KIND_ALL,      "All Tasks" },
        { SB_KIND_TODAY,    "Due Today" },
        { SB_KIND_TOMORROW, "Due Tomorrow" },
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
        /* The optional emoji prefixes the name, set off by two spaces.      */
        gchar *label = *l->emoji != '\0'
            ? g_strdup_printf("%s  %s", l->emoji, l->name)
            : g_strdup(l->name);
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
                           SB_LABEL, "Action Items (from Blue Notes)",
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
 * refresh_bn_actions() — fill the task pane with Blue Notes action items
 * (fetched through the blue_notes CLI; see bnotes.h).  Rows carry their
 * "NOTEID:ORD" address in TL_REF and 0 in TL_ID.
 * ------------------------------------------------------------------------- */
static void
refresh_bn_actions(BtLibrary *lw)
{
    gchar *err = NULL;
    GPtrArray *acts = bt_bnotes_actions(&err);
    if (acts == NULL) {
        gtk_label_set_text(GTK_LABEL(lw->status_left),
                           "Action Items (Blue Notes)");
        bt_app_status(lw->app, "%s", err);
        g_free(err);
        return;
    }
    for (guint i = 0; i < acts->len; i++) {
        BtNoteAction *na = g_ptr_array_index(acts, i);
        gchar *esc = g_markup_escape_text(
            *na->text != '\0' ? na->text : "(empty item)", -1);
        gchar *note = g_strndup(na->ref, strcspn(na->ref, ":"));
        gchar *desc = g_strdup_printf(
            "%s%s%s\n<small><span alpha=\"60%%\">Blue Notes \xc2\xb7 "
            "note %s</span></small>",
            na->done ? "<b><s>" : "<b>", esc,
            na->done ? "</s></b>" : "</b>", note);
        gchar *due = bt_due_format(na->due);
        GtkTreeIter iter;
        gtk_list_store_append(lw->task_store, &iter);
        gtk_list_store_set(lw->task_store, &iter,
                           TL_ID, (gint64)0,
                           TL_DONE, na->done,
                           TL_DESC, desc,
                           TL_DUE, due,
                           TL_DUE_RAW, na->due,
                           TL_PINNED, FALSE,
                           TL_REF, na->ref,
                           -1);
        g_free(desc);
        g_free(due);
        g_free(esc);
        g_free(note);
    }
    gchar *loc = g_strdup_printf(
        "Action Items (from Blue Notes) \xe2\x80\x94 %u item%s",
        acts->len, acts->len == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(lw->status_left), loc);
    g_free(loc);
    bt_bnotes_actions_free(acts);
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

    /* One pass of shared lookups (avoid per-row queries where easy).        */
    GHashTable *att_counts = bt_db_attachment_counts(lw->app->db);
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

    for (guint i = 0; i < tasks->len; i++) {
        BtTask *t = g_ptr_array_index(tasks, i);
        GPtrArray *subs = t->parent_id == 0
            ? bt_db_subtasks(lw->app->db, t->id) : NULL;
        const gchar *list_name = virtual_view && list_names != NULL
            ? g_hash_table_lookup(list_names,
                                  GINT_TO_POINTER((gint)t->list_id))
            : NULL;
        gint att_count = GPOINTER_TO_INT(g_hash_table_lookup(att_counts,
            GINT_TO_POINTER((gint)t->id)));
        gchar *desc = task_desc_markup(t, list_name, att_count, subs);
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
        if (subs != NULL)
            bt_ptr_array_free_tasks(subs);
    }

    /* Status bar left: where we are + how many rows.                        */
    if (virtual_view) {
        gchar *loc = g_strdup_printf("%s \xe2\x80\x94 %u task%s",
                                     view_name, tasks->len,
                                     tasks->len == 1 ? "" : "s");
        gtk_label_set_text(GTK_LABEL(lw->status_left), loc);
        g_free(loc);
    } else {
        BtList *l = bt_db_list_get(lw->app->db, lw->sel_id);
        gchar *loc = g_strdup_printf("%s \xe2\x80\x94 %u task%s",
                                     l != NULL ? l->name : "?",
                                     tasks->len,
                                     tasks->len == 1 ? "" : "s");
        gtk_label_set_text(GTK_LABEL(lw->status_left), loc);
        g_free(loc);
        bt_list_free(l);
    }

    g_hash_table_destroy(att_counts);
    if (list_names != NULL)
        g_hash_table_destroy(list_names);
    bt_ptr_array_free_tasks(tasks);
}

/* full_refresh() — sidebar + task pane + open editors.                      */
static void
full_refresh(BtLibrary *lw)
{
    refresh_sidebar(lw);
    refresh_tasks(lw);
    bt_editor_refresh_all(lw->app);
}

/* notify_changed_hook() / notify_status_hook() — the BtApp hooks.           */
static void
notify_changed_hook(BtApp *app)
{
    BtLibrary *lw = lib_of(app);
    if (lw != NULL)
        full_refresh(lw);
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
 * Task pane behavior.
 * =========================================================================== */

/* selected_task_id() — id of the selected task row, or 0.                   */
static gint64
selected_task_id(BtLibrary *lw)
{
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->task_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return 0;
    gint64 id;
    gtk_tree_model_get(model, &iter, TL_ID, &id, -1);
    return id;
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
    if (lw->sel_kind == SB_KIND_BN_ACTIONS) {
        gchar *ref = NULL;
        gtk_tree_model_get(model, &iter, TL_REF, &ref, -1);
        if (ref != NULL)
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

    /* Blue Notes rows write back through the blue_notes CLI, which
     * strikes/un-strikes the '!' line in the note itself.                   */
    if (lw->sel_kind == SB_KIND_BN_ACTIONS) {
        gchar *ref = NULL;
        gtk_tree_model_get(model, &iter, TL_REF, &ref, -1);
        gchar *err = NULL;
        if (ref != NULL &&
            bt_bnotes_action_set_done(ref, !done, &err)) {
            bt_app_status(lw->app, "Updated in Blue Notes");
            refresh_tasks(lw);
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
    if (lw->sel_kind == SB_KIND_BN_ACTIONS) {
        bt_app_status(lw->app,
                      "Blue Notes items cannot be pinned here");
        return;
    }
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
    if (!gtk_tree_model_get_iter_from_string(model, &iter, path_str))
        return;
    gint64 id;
    gboolean pinned;
    gtk_tree_model_get(model, &iter, TL_ID, &id, TL_PINNED, &pinned, -1);
    bt_db_task_set_pinned(lw->app->db, id, !pinned);
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
        lw->sel_kind = SB_KIND_LIST;
        lw->sel_id = id;
        full_refresh(lw);
        bt_app_status(lw->app,
                      "Created list \xe2\x80\x9c%s\xe2\x80\x9d", name);
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
    if (id == 0)
        return;
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
    gint64 id = selected_task_id(lw);
    if (id == 0) {
        bt_app_status(lw->app, "Select a task to delete");
        return;
    }
    BtTask *t = bt_db_task_get(lw->app->db, id);
    if (t == NULL)
        return;
    gboolean yes = bt_app_confirm(GTK_WINDOW(lw->window), "Delete Task",
        "Delete \xe2\x80\x9c%s\xe2\x80\x9d%s?",
        *t->title != '\0' ? t->title : "Untitled Task",
        t->parent_id == 0 ? " and its subtasks" : "");
    if (yes) {
        GtkWindow *editor = g_hash_table_lookup(lw->app->editors, &id);
        if (editor != NULL)
            gtk_widget_destroy(GTK_WIDGET(editor));
        bt_db_task_delete(lw->app->db, id);
        full_refresh(lw);
        bt_app_status(lw->app, "Deleted \xe2\x80\x9c%s\xe2\x80\x9d",
                      t->title);
    }
    bt_task_free(t);
}

/* sync_done() — re-enable the Sync menu item after a run.                   */
static void
sync_done(BtApp *app, gboolean ok, const gchar *message, gpointer data)
{
    (void)app; (void)ok; (void)message;
    BtLibrary *lw = data;
    gtk_widget_set_sensitive(lw->sync_item, TRUE);
}

/* sync_after_signin() — the Sync button's browser flow finished: run the
 * actual sync, or report why not.                                           */
static void
sync_after_signin(gboolean ok, const gchar *error, gpointer data)
{
    BtLibrary *lw = data;
    if (ok) {
        bt_sync_start(lw->app, lw->db_path, sync_done, lw);
    } else {
        gtk_widget_set_sensitive(lw->sync_item, TRUE);
        bt_app_notice(GTK_WINDOW(lw->window), GTK_MESSAGE_ERROR,
                      "Blue Tasks - Google Sign-In",
                      "Could not sign in: %s",
                      error != NULL ? error : "unknown error");
    }
}

/* on_sync() — the toolbar Sync button.  Sign-in is per session: when the
 * in-memory token is missing/expired this re-runs the browser flow first
 * (usually a silent redirect), then syncs.                                  */
static void
on_sync(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    if (!bt_oauth_have_client()) {
        bt_app_status(lw->app, "Google sync is not configured \xe2\x80\x94 "
                      "see File \xe2\x86\x92 Settings\xe2\x80\xa6");
        bt_settings_window_open(lw->app, GTK_WINDOW(lw->window),
                                lw->db_path);
        return;
    }
    gtk_widget_set_sensitive(lw->sync_item, FALSE);
    if (bt_oauth_authenticated()) {
        bt_sync_start(lw->app, lw->db_path, sync_done, lw);
    } else {
        bt_app_status(lw->app,
                      "Opening browser for Google sign-in\xe2\x80\xa6");
        bt_oauth_begin(lw->app, GTK_WINDOW(lw->window),
                       sync_after_signin, lw);
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

/* on_menu_sync() — File → Sync Now.                                         */
static void
on_menu_sync(GtkWidget *w, gpointer data)
{
    on_sync(w, data);
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
    bt_editor_close_all(lw->app);
    lw->app->notify_changed = NULL;
    lw->app->notify_status  = NULL;
    lw->app->library_window = NULL;
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
    menu_item(file_menu, "Sync Now", G_CALLBACK(on_menu_sync), lw);
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
     * icons/Selected/ (case-exact for Linux).  Layout: sidebar toggle +
     * the "Lists" menu button (New/Delete List, Sync — the Blue Notes
     * compact pattern) on the left, the task buttons pushed to the RIGHT
     * end by an invisible expanding separator.                              */
    GtkWidget *toolbar = gtk_toolbar_new();
    /* Small-toolbar metrics — the Blue Notes bar height.                    */
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);
    tool_button(lw, GTK_TOOLBAR(toolbar), "Selected/sidebar",
                "\xe2\x97\xa7", "Sidebar", "Show or hide the lists pane",
                G_CALLBACK(on_toggle_sidebar));

    lw->sync_item = GTK_WIDGET(tool_button(lw, GTK_TOOLBAR(toolbar),
        "Selected/cycle", "\xe2\x9f\xb3", "Sync",
        "Sync with Google Tasks now", G_CALLBACK(on_sync)));

    GtkToolItem *spring = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(spring),
                                     FALSE);
    gtk_tool_item_set_expand(spring, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), spring, -1);

    tool_button(lw, GTK_TOOLBAR(toolbar), "Selected/add2", NULL,
                "New Task", "Create a task in the selected list",
                G_CALLBACK(on_new_task));
    tool_button(lw, GTK_TOOLBAR(toolbar), "Selected/remove", NULL,
                "Delete Task", "Delete the selected task",
                G_CALLBACK(on_delete_task));
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
    g_signal_connect(lw->task_view, "row-activated",
                     G_CALLBACK(on_task_activated), lw);

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
    return lw->window;
}
