/* ===========================================================================
 * editor_window.c — the per-task editor window (see editor_window.h)
 * =========================================================================== */

#include "editor_window.h"
#include "bnotes.h"
#include "json.h"
#include <string.h>

/* Columns of the subtasks list store.                                       */
enum {
    SUB_ID = 0,                      /* gint64: subtask id                  */
    SUB_DONE,                        /* gboolean                            */
    SUB_TITLE,                       /* gchar*                              */
    SUB_N_COLS
};

/* Columns of the attachments list store.                                    */
enum {
    ATT_ID = 0,                      /* gint64: attachment id               */
    ATT_PATH,                        /* gchar*: full path                   */
    ATT_NAME,                        /* gchar*: basename shown              */
    ATT_N_COLS
};

/* ---------------------------------------------------------------------------
 * BtEditor — one open editor window's state.
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp        *app;
    gint64        task_id;           /* 0 for Blue Notes item editors       */
    gchar        *bn_ref;            /* Blue Notes "NOTEID:ORD", or NULL    */
    gint64        parent_id;         /* 0 when the task is top-level        */
    GtkWidget    *window;
    GtkWidget    *title_entry;
    GtkWidget    *done_check;
    GtkWidget    *pinned_check;
    GtkWidget    *due_entry;
    GtkTextBuffer *notes_buf;
    GtkListStore *sub_store;         /* NULL for subtask editors            */
    GtkWidget    *sub_view;
    GtkListStore *att_store;
    GtkWidget    *att_view;
    GtkWidget    *google_box;        /* "From Google" section, or NULL      */
    GtkWidget    *google_info;       /* completed/assignment label          */
    GtkWidget    *glinks_box;        /* link buttons container              */
    guint         save_source;       /* pending debounce save, or 0         */
    gboolean      loading;           /* suppress change handlers            */
    gboolean      bn_done;           /* Blue Notes editors: last loaded     */
    gint64        bn_due;            /* state, so saves only shell the CLI  */
                                     /* for fields that actually changed    */
} BtEditor;

/* editor_notify() — tell the library something changed.  Editor saves
 * use the LIGHT hook (task pane only): they can never change the
 * sidebar, and the saving editor is itself the source of truth — the
 * full notify would reload every open editor (and re-run the Blue Notes
 * CLI) per autosave.                                                        */
static void
editor_notify(BtEditor *ed)
{
    if (ed->app->notify_tasks != NULL)
        ed->app->notify_tasks(ed->app);
    else if (ed->app->notify_changed != NULL)
        ed->app->notify_changed(ed->app);
}

/* editor_due_entry_parse() — the due entry's text as a timestamp, with
 * the mid-typing guard: blank clears (0), a valid date parses, and
 * PARTIAL/invalid text keeps `current` — a debounced save firing while
 * the user is still typing must not wipe the stored date.                   */
static gint64
editor_due_entry_parse(BtEditor *ed, gint64 current)
{
    gchar *trim = g_strstrip(
        g_strdup(gtk_entry_get_text(GTK_ENTRY(ed->due_entry))));
    gint64 due;                      /* the value to store                  */
    if (*trim == '\0')
        due = 0;
    else {
        gint64 parsed = bt_due_parse(trim);
        due = parsed != 0 ? parsed : current;
    }
    g_free(trim);
    return due;
}

static void editor_load(BtEditor *ed);

/* ---------------------------------------------------------------------------
 * editor_title_refresh() — window title "Blue Tasks - <task title>".
 * ------------------------------------------------------------------------- */
static void
editor_title_refresh(BtEditor *ed)
{
    const gchar *t = gtk_entry_get_text(GTK_ENTRY(ed->title_entry));
    gchar *title = g_strdup_printf("Blue Tasks - %s",
                                   *t != '\0' ? t : "Untitled Task");
    gtk_window_set_title(GTK_WINDOW(ed->window), title);
    g_free(title);
}

/* ---------------------------------------------------------------------------
 * editor_save_now() — write every editable field through to the row and
 * notify the library.  The debounce timer funnels here.  Blue Notes
 * items write done + due through the blue_notes CLI (the only fields it
 * can change); everything else in that editor is insensitive.
 * ------------------------------------------------------------------------- */
static void
editor_save_now(BtEditor *ed)
{
    if (ed->save_source != 0) {
        g_source_remove(ed->save_source);
        ed->save_source = 0;
    }
    if (ed->bn_ref != NULL) {
        gboolean done = gtk_toggle_button_get_active(
                            GTK_TOGGLE_BUTTON(ed->done_check));
        gint64 due = editor_due_entry_parse(ed, ed->bn_due);
        gchar *err = NULL;
        gboolean ok = TRUE;
        if (done != ed->bn_done) {   /* only shell the CLI for real changes */
            ok = bt_bnotes_action_set_done(ed->bn_ref, done, &err);
            if (ok)
                ed->bn_done = done;
        }
        if (ok && due != ed->bn_due) {
            g_clear_pointer(&err, g_free);
            ok = bt_bnotes_action_set_due(ed->bn_ref, due, &err);
            if (ok)
                ed->bn_due = due;
        }
        if (!ok)
            bt_app_status(ed->app, "%s",
                          err != NULL ? err : "Blue Notes update failed");
        g_free(err);
        editor_title_refresh(ed);
        editor_notify(ed);
        return;
    }
    BtTask *t = bt_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL)
        return;
    g_free(t->title);
    g_free(t->notes);
    t->title = g_strdup(gtk_entry_get_text(GTK_ENTRY(ed->title_entry)));
    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(ed->notes_buf, &a, &b);
    t->notes = gtk_text_buffer_get_text(ed->notes_buf, &a, &b, FALSE);
    t->done   = gtk_toggle_button_get_active(
                    GTK_TOGGLE_BUTTON(ed->done_check));
    t->pinned = gtk_toggle_button_get_active(
                    GTK_TOGGLE_BUTTON(ed->pinned_check));
    t->due    = editor_due_entry_parse(ed, t->due);
    bt_db_task_update(ed->app->db, t);
    bt_task_free(t);
    editor_title_refresh(ed);
    editor_notify(ed);
}

/* save_timeout() — the debounce timer body.                                 */
static gboolean
save_timeout(gpointer data)
{
    BtEditor *ed = data;
    ed->save_source = 0;
    editor_save_now(ed);
    return G_SOURCE_REMOVE;
}

/* editor_queue_save() — (re)arm the ~600 ms debounce.                       */
static void
editor_queue_save(BtEditor *ed)
{
    if (ed->loading)
        return;
    if (ed->save_source != 0)
        g_source_remove(ed->save_source);
    ed->save_source = g_timeout_add(600, save_timeout, ed);
}

/* on_field_changed() — any text/toggle edit → debounce a save.              */
static void
on_field_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    editor_queue_save(data);
}

/* on_toggle_changed() — done/pinned change → save immediately (these
 * drive the library's meta lists).                                          */
static void
on_toggle_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    BtEditor *ed = data;
    if (!ed->loading)
        editor_save_now(ed);
}

/* ---------------------------------------------------------------------------
 * on_due_calendar() — the 📅 button: modal GtkCalendar dialog writing an
 * ISO date into the due entry (Clear empties it).
 * ------------------------------------------------------------------------- */
static void
on_due_calendar(GtkWidget *w, gpointer data)
{
    (void)w;
    BtEditor *ed = data;
    GtkWidget *dlg = gtk_dialog_new_with_buttons("Due Date",
        GTK_WINDOW(ed->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Clear", GTK_RESPONSE_REJECT, "_Cancel", GTK_RESPONSE_CANCEL,
        "_OK", GTK_RESPONSE_OK, NULL);
    GtkWidget *cal = gtk_calendar_new();

    /* Preselect the current due date, if any.                               */
    gint64 due = bt_due_parse(gtk_entry_get_text(GTK_ENTRY(ed->due_entry)));
    if (due != 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(due);
        gtk_calendar_select_month(GTK_CALENDAR(cal),
                                  (guint)g_date_time_get_month(dt) - 1,
                                  (guint)g_date_time_get_year(dt));
        gtk_calendar_select_day(GTK_CALENDAR(cal),
                                (guint)g_date_time_get_day_of_month(dt));
        g_date_time_unref(dt);
    }
    gtk_box_pack_start(
        GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dlg))),
        cal, TRUE, TRUE, 6);
    gtk_widget_show_all(dlg);

    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    if (resp == GTK_RESPONSE_OK) {
        guint y, m, d;               /* the picked date                     */
        gtk_calendar_get_date(GTK_CALENDAR(cal), &y, &m, &d);
        gchar *iso = g_strdup_printf("%04u-%02u-%02u", y, m + 1, d);
        gtk_entry_set_text(GTK_ENTRY(ed->due_entry), iso);
        g_free(iso);
        editor_save_now(ed);
    } else if (resp == GTK_RESPONSE_REJECT) {
        gtk_entry_set_text(GTK_ENTRY(ed->due_entry), "");
        editor_save_now(ed);
    }
    gtk_widget_destroy(dlg);
}

/* ===========================================================================
 * Subtasks section (top-level tasks only).
 * =========================================================================== */

/* sub_selected_id() — id of the selected subtask row, or 0.                 */
static gint64
sub_selected_id(BtEditor *ed)
{
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ed->sub_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return 0;
    gint64 id;
    gtk_tree_model_get(model, &iter, SUB_ID, &id, -1);
    return id;
}

/* sub_refresh() — repopulate the subtasks store from the database.          */
static void
sub_refresh(BtEditor *ed)
{
    if (ed->sub_store == NULL)
        return;
    gtk_list_store_clear(ed->sub_store);
    GPtrArray *subs = bt_db_subtasks(ed->app->db, ed->task_id);
    for (guint i = 0; i < subs->len; i++) {
        BtTask *s = g_ptr_array_index(subs, i);
        GtkTreeIter iter;
        gtk_list_store_append(ed->sub_store, &iter);
        gtk_list_store_set(ed->sub_store, &iter,
                           SUB_ID, s->id,
                           SUB_DONE, s->done,
                           SUB_TITLE, s->title,
                           -1);
    }
    bt_ptr_array_free_tasks(subs);
}

/* on_sub_add() — create a subtask and start editing its title in place.     */
static void
on_sub_add(GtkWidget *w, gpointer data)
{
    (void)w;
    BtEditor *ed = data;
    BtTask *t = bt_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL)
        return;
    gint64 id = bt_db_task_create(ed->app->db, t->list_id, ed->task_id,
                                  "New subtask");
    bt_task_free(t);
    if (id == 0) {                   /* refused (nesting) or write failed   */
        bt_app_status(ed->app, "Could not create the subtask");
        return;
    }
    sub_refresh(ed);
    editor_notify(ed);

    /* Put the fresh row's title straight into edit mode.                    */
    GtkTreeModel *model = GTK_TREE_MODEL(ed->sub_store);
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);
    while (valid) {
        gint64 rid;
        gtk_tree_model_get(model, &iter, SUB_ID, &rid, -1);
        if (rid == id) {
            GtkTreePath *path = gtk_tree_model_get_path(model, &iter);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(ed->sub_view), path,
                gtk_tree_view_get_column(GTK_TREE_VIEW(ed->sub_view), 1),
                TRUE);
            gtk_tree_path_free(path);
            break;
        }
        valid = gtk_tree_model_iter_next(model, &iter);
    }
}

/* on_sub_remove() — delete the selected subtask (no confirm — it is one
 * line of text; the delete propagates to Google on the next sync).          */
static void
on_sub_remove(GtkWidget *w, gpointer data)
{
    (void)w;
    BtEditor *ed = data;
    gint64 id = sub_selected_id(ed);
    if (id == 0)
        return;
    bt_db_task_delete(ed->app->db, id);
    sub_refresh(ed);
    editor_notify(ed);
}

/* on_sub_toggled() — the subtask done checkbox in the list.                 */
static void
on_sub_toggled(GtkCellRendererToggle *cell, gchar *path_str, gpointer data)
{
    (void)cell;
    BtEditor *ed = data;
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(ed->sub_store);
    if (!gtk_tree_model_get_iter_from_string(model, &iter, path_str))
        return;
    gint64 id;
    gboolean done;
    gtk_tree_model_get(model, &iter, SUB_ID, &id, SUB_DONE, &done, -1);
    bt_db_task_set_done(ed->app->db, id, !done);
    gtk_list_store_set(ed->sub_store, &iter, SUB_DONE, !done, -1);
    editor_notify(ed);
}

/* on_sub_title_edited() — in-place subtask rename.                          */
static void
on_sub_title_edited(GtkCellRendererText *cell, gchar *path_str,
                    gchar *new_text, gpointer data)
{
    (void)cell;
    BtEditor *ed = data;
    GtkTreeIter iter;
    GtkTreeModel *model = GTK_TREE_MODEL(ed->sub_store);
    if (!gtk_tree_model_get_iter_from_string(model, &iter, path_str))
        return;
    gint64 id;
    gtk_tree_model_get(model, &iter, SUB_ID, &id, -1);
    BtTask *t = bt_db_task_get(ed->app->db, id);
    if (t == NULL)
        return;
    g_free(t->title);
    t->title = g_strdup(new_text);
    bt_db_task_update(ed->app->db, t);
    bt_task_free(t);
    gtk_list_store_set(ed->sub_store, &iter, SUB_TITLE, new_text, -1);
    editor_notify(ed);
}

/* ===========================================================================
 * Attachments section.
 * =========================================================================== */

/* att_refresh() — repopulate the attachments store.                         */
static void
att_refresh(BtEditor *ed)
{
    gtk_list_store_clear(ed->att_store);
    GPtrArray *atts = bt_db_attachments(ed->app->db, ed->task_id);
    for (guint i = 0; i < atts->len; i++) {
        BtAttachment *a = g_ptr_array_index(atts, i);
        gchar *name = g_path_get_basename(a->path);
        GtkTreeIter iter;
        gtk_list_store_append(ed->att_store, &iter);
        gtk_list_store_set(ed->att_store, &iter,
                           ATT_ID, a->id,
                           ATT_PATH, a->path,
                           ATT_NAME, name,
                           -1);
        g_free(name);
    }
    bt_ptr_array_free_attachments(atts);
}

/* att_selected() — id and (optionally) path of the selected row.            */
static gint64
att_selected(BtEditor *ed, gchar **path_out)
{
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(ed->att_view));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return 0;
    gint64 id;
    gtk_tree_model_get(model, &iter, ATT_ID, &id, -1);
    if (path_out != NULL)
        gtk_tree_model_get(model, &iter, ATT_PATH, path_out, -1);
    return id;
}

/* on_att_add() — file chooser → new attachment row.                         */
static void
on_att_add(GtkWidget *w, gpointer data)
{
    (void)w;
    BtEditor *ed = data;
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Attach File",
        GTK_WINDOW(ed->window), GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Attach", GTK_RESPONSE_ACCEPT,
        NULL);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        gchar *path =
            gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path != NULL) {
            bt_db_attachment_add(ed->app->db, ed->task_id, path);
            g_free(path);
            att_refresh(ed);
            editor_notify(ed);
        }
    }
    gtk_widget_destroy(dlg);
}

/* on_att_remove() — drop the selected attachment (the file itself is
 * never touched — attachments are references).                              */
static void
on_att_remove(GtkWidget *w, gpointer data)
{
    (void)w;
    BtEditor *ed = data;
    gint64 id = att_selected(ed, NULL);
    if (id == 0)
        return;
    bt_db_attachment_remove(ed->app->db, id);
    att_refresh(ed);
    editor_notify(ed);
}

/* att_open_path() — hand a path to the platform's default opener.           */
static void
att_open_path(BtEditor *ed, const gchar *path)
{
    gchar *uri = g_filename_to_uri(path, NULL, NULL);
    if (uri == NULL)
        return;
    GError *gerr = NULL;
    if (!gtk_show_uri_on_window(GTK_WINDOW(ed->window), uri,
                                GDK_CURRENT_TIME, &gerr)) {
        bt_app_notice(GTK_WINDOW(ed->window), GTK_MESSAGE_ERROR, NULL,
                      "Cannot open %s: %s", path,
                      gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
    }
    g_free(uri);
}

/* on_att_open() — the Open button.                                          */
static void
on_att_open(GtkWidget *w, gpointer data)
{
    (void)w;
    BtEditor *ed = data;
    gchar *path = NULL;
    if (att_selected(ed, &path) != 0 && path != NULL)
        att_open_path(ed, path);
    g_free(path);
}

/* on_att_activated() — double-click a row = open it.                        */
static void
on_att_activated(GtkTreeView *view, GtkTreePath *tp,
                 GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    BtEditor *ed = data;
    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, tp))
        return;
    gchar *path = NULL;
    gtk_tree_model_get(model, &iter, ATT_PATH, &path, -1);
    if (path != NULL)
        att_open_path(ed, path);
    g_free(path);
}

/* ===========================================================================
 * Load / lifetime.
 * =========================================================================== */

/* set_entry_if_differs() — rewrite an entry only when the text really
 * changed, so refreshes never move a cursor needlessly.                     */
static void
set_entry_if_differs(GtkWidget *entry, const gchar *text)
{
    if (strcmp(gtk_entry_get_text(GTK_ENTRY(entry)), text) != 0)
        gtk_entry_set_text(GTK_ENTRY(entry), text);
}

/* editor_load_bnote() — (re)load a Blue Notes item editor from the CLI
 * listing; the window closes when the item disappeared.                     */
static void
editor_load_bnote(BtEditor *ed)
{
    gchar *err = NULL;
    GPtrArray *acts = bt_bnotes_actions(&err);
    g_free(err);
    BtNoteAction *found = NULL;      /* our item in the fresh listing       */
    for (guint i = 0; acts != NULL && i < acts->len; i++) {
        BtNoteAction *na = g_ptr_array_index(acts, i);
        if (strcmp(na->ref, ed->bn_ref) == 0) {
            found = na;
            break;
        }
    }
    if (found == NULL) {             /* CLI failed or item gone             */
        bt_bnotes_actions_free(acts);
        gtk_widget_destroy(ed->window);
        return;
    }
    ed->loading = TRUE;
    set_entry_if_differs(ed->title_entry, found->text);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->done_check),
                                 found->done);
    /* Never rewrite the due entry while the user is typing in it — the
     * stored canonical form would replace their half-typed text.            */
    if (!gtk_widget_has_focus(ed->due_entry)) {
        gchar *due = g_strdup("");
        if (found->due != 0) {
            GDateTime *dt = g_date_time_new_from_unix_local(found->due);
            g_free(due);
            due = g_date_time_format(dt, "%Y-%m-%d");
            g_date_time_unref(dt);
        }
        set_entry_if_differs(ed->due_entry, due);
        g_free(due);
    }
    ed->bn_done = found->done;       /* the change-detection baseline       */
    ed->bn_due  = found->due;
    editor_title_refresh(ed);
    ed->loading = FALSE;
    bt_bnotes_actions_free(acts);
}

/* clear_children() — empty a container.                                     */
static void
clear_children(GtkWidget *box)
{
    GList *kids = gtk_container_get_children(GTK_CONTAINER(box));
    for (GList *l = kids; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(kids);
}

/* add_link_button() — a left-aligned GtkLinkButton row.                      */
static void
add_link_button(GtkWidget *box, const gchar *uri, const gchar *label)
{
    GtkWidget *btn = gtk_link_button_new_with_label(uri,
        label != NULL && *label != '\0' ? label : uri);
    gtk_widget_set_halign(btn, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
}

/* ---------------------------------------------------------------------------
 * google_section_load() — fill the read-only "From Google" section from
 * the task's synced metadata; hidden when there is nothing to show.
 *   completed time · assignment origin (Docs/Chat) · Google-attached
 *   links[] · the webViewLink deep link.
 * ------------------------------------------------------------------------- */
static void
google_section_load(BtEditor *ed, const BtTask *t)
{
    if (ed->google_box == NULL)
        return;
    GString *info = g_string_new(NULL);
    if (t->done && t->completed_at != 0) {
        GDateTime *dt = g_date_time_new_from_unix_local(t->completed_at);
        gchar *when = g_date_time_format(dt, "%b %-e, %Y at %H:%M");
        g_string_append_printf(info, "Completed %s", when);
        g_free(when);
        g_date_time_unref(dt);
    }
    if (t->assigned != NULL) {
        BtJson *ai = bt_json_parse(t->assigned, -1);
        const gchar *surface = bt_json_str(ai, "surfaceType");
        if (info->len > 0)
            g_string_append_c(info, '\n');
        g_string_append_printf(info, "Assigned task (from %s)",
            g_strcmp0(surface, "DOCUMENT") == 0 ? "Google Docs"
            : g_strcmp0(surface, "SPACE") == 0  ? "Google Chat"
                                                : "Google Workspace");
        bt_json_free(ai);
    }
    gtk_label_set_text(GTK_LABEL(ed->google_info), info->str);
    gtk_widget_set_visible(ed->google_info, info->len > 0);

    clear_children(ed->glinks_box);
    if (t->glinks != NULL) {
        BtJson *links = bt_json_parse(t->glinks, -1);
        for (guint i = 0; i < bt_json_len(links); i++) {
            BtJson *lk = bt_json_at(links, i);
            const gchar *uri = bt_json_str(lk, "link");
            if (uri != NULL)
                add_link_button(ed->glinks_box, uri,
                                bt_json_str(lk, "description"));
        }
        bt_json_free(links);
    }
    if (t->web_link != NULL)
        add_link_button(ed->glinks_box, t->web_link,
                        "Open in Google Tasks");

    gboolean any = info->len > 0 || t->web_link != NULL ||
                   t->glinks != NULL;
    g_string_free(info, TRUE);
    if (any)
        gtk_widget_show_all(ed->google_box);
    else
        gtk_widget_hide(ed->google_box);
    gtk_widget_set_visible(ed->google_info,
        gtk_widget_get_visible(ed->google_box) &&
        *gtk_label_get_text(GTK_LABEL(ed->google_info)) != '\0');
}

/* ---------------------------------------------------------------------------
 * editor_load() — (re)load every widget from the database row.
 * ------------------------------------------------------------------------- */
static void
editor_load(BtEditor *ed)
{
    if (ed->bn_ref != NULL) {
        editor_load_bnote(ed);
        return;
    }
    BtTask *t = bt_db_task_get(ed->app->db, ed->task_id);
    if (t == NULL || t->deleted) {
        bt_task_free(t);
        gtk_widget_destroy(ed->window);
        return;
    }
    ed->loading = TRUE;
    set_entry_if_differs(ed->title_entry, t->title);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->done_check),
                                 t->done);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ed->pinned_check),
                                 t->pinned);
    /* Same mid-typing guard as the Blue Notes loader.                       */
    if (!gtk_widget_has_focus(ed->due_entry)) {
        gchar *due = g_strdup("");
        if (t->due != 0) {
            GDateTime *dt = g_date_time_new_from_unix_local(t->due);
            g_free(due);
            due = g_strdup_printf("%04d-%02d-%02d",
                                  g_date_time_get_year(dt),
                                  g_date_time_get_month(dt),
                                  g_date_time_get_day_of_month(dt));
            g_date_time_unref(dt);
        }
        set_entry_if_differs(ed->due_entry, due);
        g_free(due);
    }

    GtkTextIter a, b;
    gtk_text_buffer_get_bounds(ed->notes_buf, &a, &b);
    gchar *cur = gtk_text_buffer_get_text(ed->notes_buf, &a, &b, FALSE);
    if (strcmp(cur, t->notes) != 0)
        gtk_text_buffer_set_text(ed->notes_buf, t->notes, -1);
    g_free(cur);

    sub_refresh(ed);
    att_refresh(ed);
    google_section_load(ed, t);
    editor_title_refresh(ed);
    ed->loading = FALSE;
    bt_task_free(t);
}

/* on_editor_destroy() — flush a pending save and unregister.                */
static void
on_editor_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    BtEditor *ed = data;
    if (ed->save_source != 0)
        editor_save_now(ed);         /* also clears the source              */
    if (ed->bn_ref != NULL)
        g_hash_table_remove(ed->app->bn_editors, ed->bn_ref);
    else
        g_hash_table_remove(ed->app->editors, &ed->task_id);
    g_free(ed->bn_ref);
    g_free(ed);
}

/* ---------------------------------------------------------------------------
 * make_list_section() — the shared "label + scrolled tree view + button
 * column" layout of the subtasks and attachments sections.  Returns the
 * outer widget; *view_out receives the tree view.
 * ------------------------------------------------------------------------- */
static GtkWidget *
make_list_section(const gchar *heading, GtkWidget *view,
                  GtkWidget *btn_box)
{
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", heading);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(outer), label, FALSE, FALSE, 0);

    GtkWidget *hbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll),
                                        GTK_SHADOW_IN);
    gtk_widget_set_size_request(scroll, -1, 110);
    gtk_container_add(GTK_CONTAINER(scroll), view);
    gtk_box_pack_start(GTK_BOX(hbox), scroll, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(hbox), btn_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), hbox, TRUE, TRUE, 0);
    return outer;
}

/* small_button() — a compact labelled button wired to `cb`.                 */
static GtkWidget *
small_button(const gchar *label, GCallback cb, gpointer data)
{
    GtkWidget *b = gtk_button_new_with_label(label);
    g_signal_connect(b, "clicked", cb, data);
    return b;
}

/* ---------------------------------------------------------------------------
 * editor_open_common() — build an editor window for a task (bn_ref NULL)
 * or a Blue Notes action item (task_id 0).  The Blue Notes variant uses
 * the same layout with title/notes/subtasks/attachments/pinned disabled
 * — done and due are the CLI-writable fields.
 * ------------------------------------------------------------------------- */
static void
editor_open_common(BtApp *app, gint64 task_id, const gchar *bn_ref)
{
    GtkWindow *existing = bn_ref != NULL
        ? g_hash_table_lookup(app->bn_editors, bn_ref)
        : g_hash_table_lookup(app->editors, &task_id);
    if (existing != NULL) {
        gtk_window_present(existing);
        return;
    }
    BtTask *t = NULL;                /* the task row (task editors only)    */
    if (bn_ref == NULL) {
        t = bt_db_task_get(app->db, task_id);
        if (t == NULL || t->deleted) {
            bt_task_free(t);
            return;
        }
    }

    BtEditor *ed = g_new0(BtEditor, 1);
    ed->app       = app;
    ed->task_id   = task_id;
    ed->bn_ref    = g_strdup(bn_ref);
    ed->parent_id = t != NULL ? t->parent_id : 0;
    gboolean bn   = bn_ref != NULL;  /* the reduced Blue Notes editor       */

    ed->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(ed->window), 520, 620);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 12);
    gtk_container_add(GTK_CONTAINER(ed->window), vbox);

    /* Title.                                                                */
    ed->title_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed->title_entry),
                                   "Task title");
    g_signal_connect(ed->title_entry, "changed",
                     G_CALLBACK(on_field_changed), ed);
    if (bn) {                        /* the '!' line's text lives in the
                                      * note — no CLI rename verb           */
        gtk_widget_set_sensitive(ed->title_entry, FALSE);
        gtk_widget_set_tooltip_text(ed->title_entry,
            "Edit the item text in its Blue Notes note");
    }
    gtk_box_pack_start(GTK_BOX(vbox), ed->title_entry, FALSE, FALSE, 0);

    /* Done / Pinned / Due row.                                              */
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    ed->done_check = gtk_check_button_new_with_label("Done");
    g_signal_connect(ed->done_check, "toggled",
                     G_CALLBACK(on_toggle_changed), ed);
    gtk_box_pack_start(GTK_BOX(row), ed->done_check, FALSE, FALSE, 0);
    ed->pinned_check = gtk_check_button_new_with_label("Pinned");
    g_signal_connect(ed->pinned_check, "toggled",
                     G_CALLBACK(on_toggle_changed), ed);
    if (bn)                          /* pinning is a Blue Tasks concept     */
        gtk_widget_set_sensitive(ed->pinned_check, FALSE);
    gtk_box_pack_start(GTK_BOX(row), ed->pinned_check, FALSE, FALSE, 0);

    GtkWidget *due_btn = small_button("\xf0\x9f\x93\x85",
                                      G_CALLBACK(on_due_calendar), ed);
    gtk_widget_set_tooltip_text(due_btn, "Pick a due date");
    gtk_box_pack_end(GTK_BOX(row), due_btn, FALSE, FALSE, 0);
    ed->due_entry = gtk_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(ed->due_entry), 12);
    gtk_entry_set_placeholder_text(GTK_ENTRY(ed->due_entry),
                                   "YYYY-MM-DD");
    g_signal_connect(ed->due_entry, "changed",
                     G_CALLBACK(on_field_changed), ed);
    gtk_box_pack_end(GTK_BOX(row), ed->due_entry, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(row), gtk_label_new("Due:"),
                     FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    /* Notes.                                                                */
    GtkWidget *notes_label = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(notes_label), "<b>Notes</b>");
    gtk_widget_set_halign(notes_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), notes_label, FALSE, FALSE, 0);
    GtkWidget *notes_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(notes_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(notes_scroll),
                                        GTK_SHADOW_IN);
    GtkWidget *notes_view = gtk_text_view_new();
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(notes_view), GTK_WRAP_WORD);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(notes_view), 6);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(notes_view), 6);
    ed->notes_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(notes_view));
    g_signal_connect(ed->notes_buf, "changed",
                     G_CALLBACK(on_field_changed), ed);
    gtk_container_add(GTK_CONTAINER(notes_scroll), notes_view);
    if (bn)
        gtk_widget_set_sensitive(notes_scroll, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), notes_scroll, TRUE, TRUE, 0);

    /* Subtasks — only for top-level tasks (no nested subtasks).             */
    if (ed->parent_id == 0) {
        ed->sub_store = gtk_list_store_new(SUB_N_COLS, G_TYPE_INT64,
                                           G_TYPE_BOOLEAN, G_TYPE_STRING);
        ed->sub_view = gtk_tree_view_new_with_model(
            GTK_TREE_MODEL(ed->sub_store));
        g_object_unref(ed->sub_store);
        gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ed->sub_view),
                                          FALSE);
        gtk_tree_view_set_enable_search(GTK_TREE_VIEW(ed->sub_view),
                                        FALSE);

        GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
        g_signal_connect(toggle, "toggled",
                         G_CALLBACK(on_sub_toggled), ed);
        gtk_tree_view_append_column(GTK_TREE_VIEW(ed->sub_view),
            gtk_tree_view_column_new_with_attributes("", toggle,
                "active", SUB_DONE, NULL));
        GtkCellRenderer *text = gtk_cell_renderer_text_new();
        g_object_set(text, "editable", TRUE, NULL);
        g_signal_connect(text, "edited",
                         G_CALLBACK(on_sub_title_edited), ed);
        gtk_tree_view_append_column(GTK_TREE_VIEW(ed->sub_view),
            gtk_tree_view_column_new_with_attributes("Subtask", text,
                "text", SUB_TITLE, NULL));

        GtkWidget *btns = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_box_pack_start(GTK_BOX(btns),
            small_button("Add", G_CALLBACK(on_sub_add), ed),
            FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(btns),
            small_button("Remove", G_CALLBACK(on_sub_remove), ed),
            FALSE, FALSE, 0);
        GtkWidget *sub_section =
            make_list_section("Subtasks", ed->sub_view, btns);
        if (bn)
            gtk_widget_set_sensitive(sub_section, FALSE);
        gtk_box_pack_start(GTK_BOX(vbox), sub_section, FALSE, FALSE, 0);
    } else {
        BtTask *parent = bt_db_task_get(app->db, ed->parent_id);
        gchar *txt = g_strdup_printf(
            "This is a subtask of \xe2\x80\x9c%s\xe2\x80\x9d "
            "\xe2\x80\x94 subtasks cannot have their own subtasks.",
            parent != NULL ? parent->title : "?");
        GtkWidget *note = gtk_label_new(txt);
        gtk_label_set_line_wrap(GTK_LABEL(note), TRUE);
        gtk_widget_set_halign(note, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(vbox), note, FALSE, FALSE, 0);
        g_free(txt);
        bt_task_free(parent);
    }

    /* Attachments.                                                          */
    ed->att_store = gtk_list_store_new(ATT_N_COLS, G_TYPE_INT64,
                                       G_TYPE_STRING, G_TYPE_STRING);
    ed->att_view = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(ed->att_store));
    g_object_unref(ed->att_store);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(ed->att_view), FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(ed->att_view), FALSE);
    GtkCellRenderer *att_text = gtk_cell_renderer_text_new();
    g_object_set(att_text, "ellipsize", PANGO_ELLIPSIZE_MIDDLE, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(ed->att_view),
        gtk_tree_view_column_new_with_attributes("File", att_text,
            "text", ATT_NAME, NULL));
    g_signal_connect(ed->att_view, "row-activated",
                     G_CALLBACK(on_att_activated), ed);

    GtkWidget *att_btns = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_pack_start(GTK_BOX(att_btns),
        small_button("Add\xe2\x80\xa6", G_CALLBACK(on_att_add), ed),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(att_btns),
        small_button("Remove", G_CALLBACK(on_att_remove), ed),
        FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(att_btns),
        small_button("Open", G_CALLBACK(on_att_open), ed),
        FALSE, FALSE, 0);
    GtkWidget *att_section =
        make_list_section("Attachments", ed->att_view, att_btns);
    if (bn)
        gtk_widget_set_sensitive(att_section, FALSE);
    gtk_box_pack_start(GTK_BOX(vbox), att_section, FALSE, FALSE, 0);

    /* "From Google" — read-only metadata pulled by the sync (completed
     * time, Docs/Chat assignment, Google-attached links, the deep link
     * into Google Tasks).  Task editors only; shown only when present.      */
    if (!bn) {
        ed->google_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        GtkWidget *heading = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(heading), "<b>From Google</b>");
        gtk_widget_set_halign(heading, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(ed->google_box), heading,
                           FALSE, FALSE, 0);
        ed->google_info = gtk_label_new("");
        gtk_label_set_line_wrap(GTK_LABEL(ed->google_info), TRUE);
        gtk_widget_set_halign(ed->google_info, GTK_ALIGN_START);
        bt_app_widget_add_css(ed->google_info,
                              "label { font-size: 85%; }");
        gtk_box_pack_start(GTK_BOX(ed->google_box), ed->google_info,
                           FALSE, FALSE, 0);
        ed->glinks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_box_pack_start(GTK_BOX(ed->google_box), ed->glinks_box,
                           FALSE, FALSE, 0);
        gtk_widget_set_no_show_all(ed->google_box, TRUE);
        gtk_box_pack_start(GTK_BOX(vbox), ed->google_box,
                           FALSE, FALSE, 0);
    }

    g_signal_connect(ed->window, "destroy",
                     G_CALLBACK(on_editor_destroy), ed);

    /* Register + load.                                                      */
    if (bn) {
        g_hash_table_insert(app->bn_editors, g_strdup(bn_ref),
                            ed->window);
    } else {
        gint64 *key = g_new(gint64, 1);
        *key = task_id;
        g_hash_table_insert(app->editors, key, ed->window);
    }
    g_object_set_data(G_OBJECT(ed->window), "bt-editor", ed);
    bt_task_free(t);
    editor_load(ed);
    gtk_widget_show_all(ed->window);
}

/* ---------------------------------------------------------------------------
 * bt_editor_open() / bt_editor_open_bnote() — the public entry points
 * (see header).
 * ------------------------------------------------------------------------- */
void
bt_editor_open(BtApp *app, gint64 task_id)
{
    editor_open_common(app, task_id, NULL);
}

void
bt_editor_open_bnote(BtApp *app, const gchar *ref)
{
    editor_open_common(app, 0, ref);
}

/* ---------------------------------------------------------------------------
 * bt_editor_refresh_all() — reload every open editor (see header).
 * ------------------------------------------------------------------------- */
void
bt_editor_refresh_all(BtApp *app)
{
    GList *windows = g_hash_table_get_values(app->editors);
    windows = g_list_concat(windows,
                            g_hash_table_get_values(app->bn_editors));
    for (GList *l = windows; l != NULL; l = l->next) {
        BtEditor *ed = g_object_get_data(G_OBJECT(l->data), "bt-editor");
        if (ed == NULL || ed->save_source != 0)
            continue;                /* mid-edit: their version wins        */
        editor_load(ed);
    }
    g_list_free(windows);
}

/* bt_editor_close_all() — destroy every open editor (flushing saves).       */
void
bt_editor_close_all(BtApp *app)
{
    GList *windows = g_hash_table_get_values(app->editors);
    windows = g_list_concat(windows,
                            g_hash_table_get_values(app->bn_editors));
    for (GList *l = windows; l != NULL; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(windows);
}
