/* ===========================================================================
 * library_window.c — the main Lists window (see library_window.h)
 * =========================================================================== */

#include "library_window.h"
#include "bnsync.h"
#include "editor_window.h"
#include "gtasks.h"
#include "oauth.h"
#include "settings_window.h"
#include <stdlib.h>
#include <string.h>
#include <glib/gstdio.h>
#ifdef HAVE_GTKOSX
#include <gtkosxapplication.h>
#endif

/* Odd-row stripe tint of the task list (the Records list palette).          */
#define ROW_TINT      "#e8f2fb"
/* Background applied to the row currently held during a manual drag.        */
#define DRAG_ROW_TINT "#fde68a"

/* Sidebar row kinds (SB_KIND column).                                       */
enum {
    SB_KIND_PINNED = 0,              /* "Pinned Tasks" virtual list         */
    SB_KIND_ALL,                     /* "All Tasks" virtual list            */
    SB_KIND_BN_ACTIONS,              /* "Action Items" — a filtered view
                                      * of every mirrored Records item      */
    SB_KIND_TODAY,                   /* "Due Today" virtual list            */
    SB_KIND_FORECAST,                /* "Weekly Forecast" virtual list      */
    SB_KIND_HEADER,                  /* the "Lists" section header          */
    SB_KIND_LIST,                    /* a real list                         */
    SB_KIND_GROUP                    /* a list-group sub-header             */
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
    TL_ID = 0,                       /* gint64: task id (0 only for the
                                      * forecast's placeholder rows)        */
    TL_DONE,                         /* gboolean                            */
    TL_DESC,                         /* gchar*: the tall markup cell        */
    TL_DUE,                          /* gchar*: formatted due date ("")     */
    TL_DUE_RAW,                      /* gint64: due timestamp (sort/tint)   */
    TL_TITLE,                        /* gchar*: raw task title (sort key)   */
    TL_N_COLS
};

/* ---------------------------------------------------------------------------
 * BtLibrary — the window's state.
 *   sel_kind/sel_id — current sidebar selection (survives refreshes).
 *   populating      — guards the sidebar changed handler during rebuilds.
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp        *app;
    GtkWidget    *window;
    GtkTreeStore *sb_store;
    GtkWidget    *sb_view;
    GtkListStore *task_store;
    GtkWidget    *task_view;
    GtkWidget    *task_scroll;       /* the regular task pane; swapped
                                      * with forecast_box (visibility)      */
    GtkWidget    *forecast_box;      /* Weekly Forecast: the scroller
                                      * holding the 7 stacked day lists     */
    GtkWidget    *day_labels[7];     /* the day headings, Sunday first      */
    GtkListStore *day_stores[7];     /* one store per day view              */
    GtkWidget    *day_views[7];      /* the per-day tree views              */
    GtkWidget    *sidebar_box;       /* for the toolbar show/hide toggle    */
    GtkWidget    *toolbar;           /* hidden by Compact Layout            */
    GtkWidget    *toolbar_rule;      /* the thin rule under the toolbar     */
    GtkWidget    *float_bar;         /* Compact Layout's floating New /
                                      * Delete Task pair (overlay child)    */
    GtkWidget    *status_left;       /* selection info label                */
    GtkWidget    *status_right;      /* latest event message label          */
    GtkWidget    *sync_item;         /* the Lists-menu Sync item            */
    GtkWidget    *hide_done_item;    /* completed-visibility toggle button  */
    GtkWidget    *manual_sort_item;  /* manual-sort mode toggle button      */
    GtkWidget    *view_show_done_item;  /* View menu: Show Completed check  */
    GtkWidget    *view_manual_sort_item;/* View menu: Manual Sort check     */
    GtkWidget    *view_compact_item;    /* View menu: Compact Layout check  */
    GtkWidget    *view_sidebar_item;    /* View menu: Show Sidebar check    */
    gint          sel_kind;
    gint64        sel_id;
    gboolean      populating;
    gboolean      sb_populated;      /* first population expands Lists      */
    gboolean      pinned_row_shown;  /* Pinned Tasks row exists (hidden
                                      * while nothing is pinned)            */
    GHashTable   *group_expanded;    /* group id (ptr) → expanded gboolean  */
    gint          sb_width;          /* live divider position (persisted
                                      * at close as sidebar_width)          */
    gint          win_w, win_h;      /* live client size (persisted at
                                      * close as the next launch's size)    */
    gboolean             manual_sort;    /* task_list_manual_sort, cached:
                                          * read per motion event and per
                                          * refresh, so it must not cost a
                                          * GKeyFile lookup + strdup each
                                          * time.  task_manual_sort_apply
                                          * is the single writer.          */
    gboolean             drag_active;    /* live task-row drag in progress  */
    GtkTreeRowReference *drag_row_ref;   /* auto-updating ref to drag row  */
    GtkTreeRowReference *drag_lock_ref;  /* row just swapped; locked until
                                          * cursor re-enters drag row      */
    GdkCursor           *drag_cursor;    /* the "ns-resize" cursor, made
                                          * once (owned; the motion path
                                          * would otherwise allocate one
                                          * per event)                     */
    gint                 pending_fades;       /* active fade-out animations  */
    guint                status_fade_source;  /* delay before fade starts    */
    guint                status_fade_step_source; /* per-step fade timer     */
    gint                 status_fade_step;    /* current step                */
    gchar               *status_fade_text;   /* plain text being faded      */
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
 * Chrome that has to match the window background (@theme_bg_color).
 * =========================================================================== */

/* ThemedCssFunc — build a widget's CSS from the resolved background color.
 * New string (the caller g_frees it).                                       */
typedef gchar *(*ThemedCssFunc)(const GdkRGBA *bg);

/* ---------------------------------------------------------------------------
 * themed_bg_css_apply() — style `w` from the theme's @theme_bg_color, and
 * keep it in step when the theme changes (a macOS light/dark switch, or a
 * GTK theme swap on Linux).
 *
 * Resolving the NAMED color rather than hardcoding a gray is the whole
 * point: it is what makes these widgets match the window and the status
 * bar, whatever the theme paints them.  A theme that doesn't name the
 * color is the one case we leave alone rather than guess — the widget
 * keeps its default look.
 *
 * The provider is created once and RELOADED in place, kept on the widget as
 * object data: bt_app_widget_add_css would stack a fresh provider on every
 * theme change.  The last color written is stored alongside it, which is
 * also what stops the recursion — our own reload re-emits "style-updated",
 * and the second pass resolves the same color and returns without writing.
 * (@theme_bg_color comes from the theme's provider, not ours, so the
 * resolved value really is stable across our own reload.)
 * ------------------------------------------------------------------------- */
static void themed_bg_css_apply(GtkWidget *w, ThemedCssFunc build);

/* on_themed_style_updated() — the theme moved: recompute from the builder
 * stashed on the widget.                                                    */
static void
on_themed_style_updated(GtkWidget *w, gpointer data)
{
    (void)data;
    themed_bg_css_apply(w, (ThemedCssFunc)g_object_get_data(
                               G_OBJECT(w), "bt-themed-build"));
}

static void
themed_bg_css_apply(GtkWidget *w, ThemedCssFunc build)
{
    if (build == NULL)
        return;
    GdkRGBA bg;                      /* the window/status-bar background    */
    if (!gtk_style_context_lookup_color(gtk_widget_get_style_context(w),
                                        "theme_bg_color", &bg))
        return;

    GdkRGBA *last = g_object_get_data(G_OBJECT(w), "bt-themed-rgba");
    if (last != NULL && gdk_rgba_equal(last, &bg))
        return;                      /* unchanged — and ends the recursion  */

    GtkCssProvider *provider =
        g_object_get_data(G_OBJECT(w), "bt-themed-css");
    if (provider == NULL) {
        provider = gtk_css_provider_new();
        gtk_style_context_add_provider(gtk_widget_get_style_context(w),
            GTK_STYLE_PROVIDER(provider),
            GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_set_data_full(G_OBJECT(w), "bt-themed-css", provider,
                               g_object_unref);
        g_object_set_data(G_OBJECT(w), "bt-themed-build", (gpointer)build);
        g_signal_connect(w, "style-updated",
                         G_CALLBACK(on_themed_style_updated), NULL);
    }
    g_object_set_data_full(G_OBJECT(w), "bt-themed-rgba",
                           gdk_rgba_copy(&bg),
                           (GDestroyNotify)gdk_rgba_free);
    gchar *css = build(&bg);
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    g_free(css);
}

/* rgb_of() — a GdkRGBA as a CSS "rgb(r,g,b)" literal (new string).          */
static gchar *
rgb_of(const GdkRGBA *c)
{
    return g_strdup_printf("rgb(%d,%d,%d)",
                           (gint)(c->red   * 255 + 0.5),
                           (gint)(c->green * 255 + 0.5),
                           (gint)(c->blue  * 255 + 0.5));
}

/* header_flatten_css() — the column-header CSS: the flat background plus
 * shades of it for :hover / :active, so a sortable header still gives
 * feedback instead of jumping back to the theme's button color.  Quartz
 * only, like its one caller — otherwise it is an unused static.             */
#ifdef GDK_WINDOWING_QUARTZ
static gchar *
header_flatten_css(const GdkRGBA *bg)
{
    gchar *c   = rgb_of(bg);
    gchar *css = g_strdup_printf(
        "button {"
        "  background-image: none;"
        "  background-color: %s;"
        "}"
        "button:hover {"
        "  background-image: none;"
        "  background-color: shade(%s, 0.94);"
        "}"
        "button:active {"
        "  background-image: none;"
        "  background-color: shade(%s, 0.88);"
        "}",
        c, c, c);
    g_free(c);
    return css;
}
#endif /* GDK_WINDOWING_QUARTZ */

/* ---------------------------------------------------------------------------
 * header_button_flatten() — paint a tree-view column header the same color
 * as the status bar.  macOS (quartz) ONLY: elsewhere the platform theme
 * owns the header's look and we leave it completely alone.
 *
 * The status bar sets no background of its own: it shows the window's,
 * which the theme paints from @theme_bg_color.  Headers, by contrast, are
 * real GtkButtons and come with the quartz theme's button gradient, so
 * they read lighter than the rest of the chrome.
 *
 * The provider goes on the header BUTTON, not the tree view: a provider
 * added to a widget's style context styles that widget only, and the
 * header buttons are separate widgets from the view.
 *
 * Gated on GDK_WINDOWING_QUARTZ rather than __APPLE__: the reason to
 * restyle is how the quartz backend draws buttons, so an X11 build on a
 * Mac correctly keeps its GTK theme.
 * ------------------------------------------------------------------------- */
static void
header_button_flatten(GtkWidget *hbtn)
{
#ifndef GDK_WINDOWING_QUARTZ
    (void)hbtn;                      /* Linux/X11: the GTK theme decides    */
#else
    themed_bg_css_apply(hbtn, header_flatten_css);
#endif
}

/* line_is_blank() — TRUE when [start, end) holds nothing but whitespace.
 * Unicode-aware on purpose: a stray U+00A0 pasted into a note is just as
 * invisible as a space and must not earn a preview line either.            */
static gboolean
line_is_blank(const gchar *start, const gchar *end)
{
    for (const gchar *p = start; p < end; p = g_utf8_next_char(p))
        if (!g_unichar_isspace(g_utf8_get_char(p)))
            return FALSE;
    return TRUE;
}

/* append_line() — add a `\n`-separated markup line.                         */
static void
append_line(GString *s, const gchar *markup)
{
    if (s->len > 0)
        g_string_append_c(s, '\n');
    g_string_append(s, markup);
}

/* ---------------------------------------------------------------------------
 * markup_escape_db() — g_markup_escape_text() for a string that came out of
 * the DATABASE, i.e. one whose UTF-8 validity we do not control.
 *
 * A whole task cell is ONE Pango markup string, so a single bad byte
 * anywhere in it makes pango_parse_markup reject the lot and the row draws
 * completely blank — title, list, notes and all.  g_markup_escape_text does
 * not validate (it escapes the markup metacharacters and copies the rest),
 * so invalid bytes pass straight through to Pango.  g_utf8_make_valid
 * substitutes U+FFFD for them, which shows the user a replacement glyph in
 * the one bad spot instead of silently losing the entire row.
 *
 * Text the app itself produced (GtkTextBuffer contents, our own literals)
 * is always valid; this is for anything a sync payload or a hand-edited
 * database could have put there.  New string (g_free).
 * ------------------------------------------------------------------------- */
static gchar *
markup_escape_db(const gchar *text)
{
    if (g_utf8_validate(text, -1, NULL))
        return g_markup_escape_text(text, -1);
    gchar *valid = g_utf8_make_valid(text, -1);
    gchar *esc   = g_markup_escape_text(valid, -1);
    g_free(valid);
    return esc;
}

/* ---------------------------------------------------------------------------
 * task_desc_markup() — build the Task cell: bold title (struck when
 * done), an "in <list>" line in the virtual views, a dimmed notes
 * preview, an attachment count, and up to four subtask lines.  This is
 * what makes the rows "extra tall".
 *
 * Prefix glyphs stack outwards from the title: ❗ marks a mirrored
 * Records action item, then ⭐️ a favorite, then 🚨 high priority, then
 * ↳ a subtask shown in a virtual view.  The ❗ sits INNERMOST (nearest
 * the title) because it describes what the row IS, not how it is
 * flagged — and unlike the pre-mirror tag it shows in every view,
 * including the item's own list.
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
    gchar *title = markup_escape_db(
        *t->title != '\0' ? t->title : "Untitled Task");
    const gchar *open  = bold ? "<b>" : "";
    const gchar *close = bold ? "</b>" : "";
    gchar *line = t->done
        ? g_strdup_printf("%s<s>%s</s>%s", open, title, close)
        : g_strdup_printf("%s%s%s", open, title, close);
    if (t->bn_uid != 0) {            /* mirrored Records action item        */
        gchar *p = g_strdup_printf("\xe2\x9d\x97  %s", line);
        g_free(line);
        line = p;
    }
    if (t->pinned) {                  /* favorite task wears a star           */
        gchar *p = g_strdup_printf("\xe2\xad\x90\xef\xb8\x8f  %s", line);
        g_free(line);
        line = p;
    }
    if (t->priority) {               /* high priority wears a siren          */
        gchar *p = g_strdup_printf("\xf0\x9f\x9a\xa8  %s", line);
        g_free(line);
        line = p;
    }
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
        gchar *esc = markup_escape_db(list_name);
        gchar *l = g_strdup_printf(
            "<small><i><span alpha=\"60%%\">in %s</span></i>"
            "</small>", esc);
        append_line(s, l);
        g_free(l);
        g_free(esc);
    }

    /* Notes preview: the first line that actually HAS content, capped,
     * dimmed.  Testing `*notes != '\0'` was not enough — a note holding
     * just a space (or a leading blank line) previewed as an empty line,
     * which reads as nothing at all while still making that one row a
     * whole line taller than every other row in the list.                  */
    const gchar *nline = t->notes;   /* candidate line, start …             */
    const gchar *nend  = nline;      /* … and one past its last byte        */
    while (*nline != '\0') {
        const gchar *eol = strchr(nline, '\n');
        nend = eol != NULL ? eol : nline + strlen(nline);
        if (!line_is_blank(nline, nend))
            break;
        if (eol == NULL) {           /* every line was blank                */
            nline = nend;
            break;
        }
        nline = eol + 1;
    }
    if (nline < nend) {
        gsize len   = (gsize)(nend - nline);
        gsize shown = MIN(len, (gsize)120);
        /* The cap is a BYTE cap, so walk it back to a character boundary:
         * a multi-byte character straddling byte 120 would leave a partial
         * sequence, and the whole cell is ONE Pango markup string — so
         * pango_parse_markup rejects it and the row renders completely
         * blank, title and all (not just the preview).  g_utf8_find_prev_char
         * from the cut point gives the last character that STARTS before it;
         * keep it only when it also ends at or before the cut.               */
        if (shown < len) {
            const gchar *cut  = nline + shown;
            const gchar *prev = g_utf8_find_prev_char(nline, cut);
            if (prev == NULL)            /* no boundary found: drop it all   */
                shown = 0;
            else if (g_utf8_next_char(prev) > cut)
                shown = (gsize)(prev - nline);   /* char is cut: exclude it  */
        }
        gchar *preview = g_strndup(nline, shown);
        /* Trim both ends: leading indentation reads as a stray gap in a
         * one-line preview, trailing space would sit before the ellipsis.
         * g_strstrip chugs in place, so `preview` stays the pointer to
         * free.                                                            */
        g_strstrip(preview);
        /* Nothing survived the cap (a single over-long character, or bytes
         * that were not valid UTF-8 to begin with): emit no line at all
         * rather than an empty one — an empty preview reads as nothing
         * while still making this row a line taller than its neighbours,
         * which is the bug the content-gating above exists to prevent.      */
        if (*preview != '\0') {
            gchar *esc = markup_escape_db(preview);
            gchar *l = g_strdup_printf(
                "<small><span alpha=\"65%%\">%s%s</span></small>", esc,
                /* more of THIS line, or any line after it                  */
                shown < len || *nend != '\0' ? "\xe2\x80\xa6" : "");
            append_line(s, l);
            g_free(l);
            g_free(esc);
        }
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
        gchar *esc = markup_escape_db(
            *sub->title != '\0' ? sub->title : "Untitled");
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
 * its height first — Records gotcha #11).                                   */
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

/* scroll_keep_queue_win() — the same, given the scrolled window itself
 * (the Weekly Forecast's one outer scroller wraps a box, not a view).       */
static void
scroll_keep_queue_win(GtkWidget *scroll)
{
    if (!GTK_IS_SCROLLED_WINDOW(scroll))
        return;
    GtkAdjustment *vadj = gtk_scrolled_window_get_vadjustment(
        GTK_SCROLLED_WINDOW(scroll));
    ScrollKeep *sk = g_new0(ScrollKeep, 1);
    sk->vadj  = g_object_ref(vadj);
    sk->value = gtk_adjustment_get_value(vadj);
    g_idle_add(scroll_keep_apply, sk);
}

static void
scroll_keep_queue(GtkWidget *view)
{
    scroll_keep_queue_win(gtk_widget_get_parent(view));
}

/* ---------------------------------------------------------------------------
 * refresh_sidebar() — rebuild the sidebar and restore the selection.
 * ------------------------------------------------------------------------- */

static void     task_view_apply_manual_order(BtLibrary *lw);
static void     task_manual_sort_apply(BtLibrary *lw);
static gchar   *list_order_key(gint64 list_id);
static gboolean on_column_header_press(GtkWidget *, GdkEventButton *, gpointer);
static void     on_menu_toggle_done_visible(GtkWidget *, gpointer);
static void     on_menu_toggle_manual_sort(GtkWidget *, gpointer);
static void     on_menu_toggle_sidebar(GtkWidget *, gpointer);

/* sidebar_show_pinned() — whether the Pinned Tasks meta row should
 * exist: any pinned task.  Mirrored Records items are ordinary tasks
 * carrying the ordinary `pinned` flag, so no separate check remains.       */
static gboolean
sidebar_show_pinned(BtLibrary *lw)
{
    return bt_db_has_pinned(lw->app->db, FALSE);
}

/* sidebar_show_actions() — whether the "Action Items" meta row should
 * exist: the Records integration on, and the row not switched off in
 * Settings.  It is a FILTERED VIEW over every mirrored task, wherever
 * each one actually lives, which is why it sits with the other meta
 * rows instead of among the real lists.                                    */
static gboolean
sidebar_show_actions(void)
{
    return bt_app_config_get_bool("blue_notes_sync", FALSE) &&
           bt_app_config_get_bool("records_meta_row", TRUE);
}

static void
refresh_sidebar(BtLibrary *lw)
{
    lw->populating = TRUE;
    scroll_keep_queue(lw->sb_view);

    /* Snapshot the Lists section's expansion BEFORE the clear — every
     * model rebuild collapses it otherwise (Records gotcha #14).
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
                    GtkTreePath *p = gtk_tree_model_get_path(model, &iter);
                    lists_expanded = gtk_tree_view_row_expanded(
                        GTK_TREE_VIEW(lw->sb_view), p);
                    gtk_tree_path_free(p);
                    /* Also snapshot group expansion states from children. */
                    GtkTreeIter child;
                    if (gtk_tree_model_iter_children(model, &child, &iter)) {
                        do {
                            gint ck; gint64 cid;
                            gtk_tree_model_get(model, &child,
                                               SB_KIND, &ck, SB_ID, &cid, -1);
                            if (ck == SB_KIND_GROUP) {
                                GtkTreePath *gp =
                                    gtk_tree_model_get_path(model, &child);
                                gboolean exp = gtk_tree_view_row_expanded(
                                    GTK_TREE_VIEW(lw->sb_view), gp);
                                gtk_tree_path_free(gp);
                                g_hash_table_insert(lw->group_expanded,
                                    GINT_TO_POINTER(cid),
                                    GINT_TO_POINTER(exp ? 1 : 0));
                            }
                        } while (gtk_tree_model_iter_next(model, &child));
                    }
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
        { SB_KIND_PINNED,     "\xe2\xad\x90\xef\xb8\x8f  Favorites" },
        { SB_KIND_ALL,        "\xf0\x9f\x94\xae  All Tasks" },
        { SB_KIND_BN_ACTIONS, "\xe2\x9d\x97\xef\xb8\x8f  Action Items" },
        { SB_KIND_TODAY,      "\xe2\x98\x80\xef\xb8\x8f  Due Today" },
        { SB_KIND_FORECAST, "\xf0\x9f\x8c\xa4\xef\xb8\x8f  Weekly Forecast" },
    };
    lw->pinned_row_shown = sidebar_show_pinned(lw);
    for (gsize i = 0; i < G_N_ELEMENTS(metas); i++) {
        if (metas[i].kind == SB_KIND_PINNED && !lw->pinned_row_shown)
            continue;                /* hidden while nothing is pinned      */
        if (metas[i].kind == SB_KIND_BN_ACTIONS && !sidebar_show_actions())
            continue;                /* integration or row off in Settings  */
        if (metas[i].kind == SB_KIND_FORECAST &&
            !bt_app_config_get_bool("weekly_forecast", TRUE))
            continue;                /* disabled in Settings                */
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

    GPtrArray *groups = bt_db_groups(lw->app->db);
    GPtrArray *lists  = bt_db_lists(lw->app->db, FALSE);
    GtkTreeIter selected;            /* the row to reselect                 */
    gboolean have_selected = FALSE;
    GtkTreeIter first_list;          /* fallback selection                  */
    gboolean have_first = FALSE;
    gboolean sel_in_group = FALSE;   /* selected list is inside a group     */

    /* First pass: groups and their lists.                                   */
    for (guint gi = 0; gi < groups->len; gi++) {
        BtGroup *grp = g_ptr_array_index(groups, gi);
        gchar *glabel = g_strdup(grp->name);
        GtkTreeIter grp_iter;
        gtk_tree_store_append(lw->sb_store, &grp_iter, &header);
        gtk_tree_store_set(lw->sb_store, &grp_iter,
                           SB_KIND, SB_KIND_GROUP,
                           SB_ID, grp->id,
                           SB_LABEL, glabel,
                           SB_WEIGHT, PANGO_WEIGHT_BOLD,
                           -1);
        g_free(glabel);

        gboolean grp_has_selected = FALSE;
        for (guint li = 0; li < lists->len; li++) {
            BtList *l = g_ptr_array_index(lists, li);
            if (l->group_id != grp->id) continue;
            gchar *label = list_label(l);
            gtk_tree_store_append(lw->sb_store, &iter, &grp_iter);
            gtk_tree_store_set(lw->sb_store, &iter,
                               SB_KIND, SB_KIND_LIST,
                               SB_ID, l->id,
                               SB_LABEL, label,
                               SB_WEIGHT, PANGO_WEIGHT_NORMAL,
                               -1);
            g_free(label);
            if (!have_first) { first_list = iter; have_first = TRUE; }
            if (lw->sel_kind == SB_KIND_LIST && lw->sel_id == l->id) {
                selected = iter;
                have_selected = TRUE;
                grp_has_selected = TRUE;
                sel_in_group = TRUE;
            }
        }
        if (lw->sel_kind == SB_KIND_GROUP && lw->sel_id == grp->id) {
            selected = grp_iter;
            have_selected = TRUE;
        }

        /* Expand the group: default TRUE on first population, then use the
         * snapshot; force open when the selected list lives inside.         */
        gpointer snap = g_hash_table_lookup(lw->group_expanded,
                                            GINT_TO_POINTER(grp->id));
        gboolean was_expanded = (snap == NULL) ? TRUE
                                               : GPOINTER_TO_INT(snap) != 0;
        if (was_expanded || grp_has_selected) {
            GtkTreePath *gp = gtk_tree_model_get_path(model, &grp_iter);
            gtk_tree_view_expand_row(GTK_TREE_VIEW(lw->sb_view), gp, FALSE);
            gtk_tree_path_free(gp);
        }
    }

    /* Second pass: ungrouped lists directly under the header.               */
    for (guint li = 0; li < lists->len; li++) {
        BtList *l = g_ptr_array_index(lists, li);
        if (l->group_id != 0) continue;
        gchar *label = list_label(l);
        gtk_tree_store_append(lw->sb_store, &iter, &header);
        gtk_tree_store_set(lw->sb_store, &iter,
                           SB_KIND, SB_KIND_LIST,
                           SB_ID, l->id,
                           SB_LABEL, label,
                           SB_WEIGHT, PANGO_WEIGHT_NORMAL,
                           -1);
        g_free(label);
        if (!have_first) { first_list = iter; have_first = TRUE; }
        if (lw->sel_kind == SB_KIND_LIST && lw->sel_id == l->id) {
            selected = iter;
            have_selected = TRUE;
        }
    }
    bt_ptr_array_free_groups(groups);
    bt_ptr_array_free_lists(lists);

    /* Reselect: same list/group, or same meta row, or the first list.      */
    GtkTreeSelection *sel =
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->sb_view));
    if (!have_selected && lw->sel_kind != SB_KIND_LIST &&
        lw->sel_kind != SB_KIND_GROUP &&
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
        selected = iter;             /* first meta row                      */
        have_selected = TRUE;
        gtk_tree_model_get(model, &iter, SB_KIND, &lw->sel_kind, -1);
        lw->sel_id = 0;
    }

    /* Restore the Lists section expansion and force it open when the
     * selection lives inside (a selection must be visible).                 */
    if (lists_expanded ||
        (have_selected && (lw->sel_kind == SB_KIND_LIST ||
                           lw->sel_kind == SB_KIND_GROUP))) {
        GtkTreePath *p = gtk_tree_model_get_path(model, &header);
        gtk_tree_view_expand_row(GTK_TREE_VIEW(lw->sb_view), p, FALSE);
        gtk_tree_path_free(p);
    }
    (void)sel_in_group;              /* groups expand themselves above      */
    if (have_selected) {
        gtk_tree_selection_select_iter(sel, &selected);
        GtkTreePath *sp = gtk_tree_model_get_path(model, &selected);
        gtk_tree_view_set_cursor(GTK_TREE_VIEW(lw->sb_view), sp, NULL, FALSE);
        gtk_tree_path_free(sp);
    }
    lw->sb_populated = TRUE;
    lw->populating = FALSE;
}

/* ---------------------------------------------------------------------------
 * TaskRowCtx — the shared lookups behind the task rows of one refresh
 * (avoid per-row queries).  Subtasks come as ONE query grouped in
 * memory, not one query per top-level row; list names are loaded only
 * for the virtual views (the "in <list>" line).
 * ------------------------------------------------------------------------- */
typedef struct {
    GHashTable *att_counts;          /* task id → attachment count          */
    GPtrArray  *all_subs;            /* owns the subtask rows below         */
    GHashTable *subs_by_parent;      /* parent id → GPtrArray of borrowed   */
    GHashTable *list_names;          /* list id → name, NULL for list views */
    gboolean    bold;                /* the bold_task_titles setting        */
    gboolean    show_done;           /* the show_completed toggle           */
} TaskRowCtx;

static void
task_row_ctx_init(BtLibrary *lw, TaskRowCtx *ctx, gboolean virtual_view)
{
    ctx->att_counts = bt_db_attachment_counts(lw->app->db);
    ctx->all_subs = bt_db_subtasks_all_visible(lw->app->db);
    ctx->subs_by_parent =
        g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
                              (GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < ctx->all_subs->len; i++) {
        BtTask *s = g_ptr_array_index(ctx->all_subs, i);
        GPtrArray *bucket = g_hash_table_lookup(ctx->subs_by_parent,
            GINT_TO_POINTER(s->parent_id));
        if (bucket == NULL) {
            bucket = g_ptr_array_new();
            g_hash_table_insert(ctx->subs_by_parent,
                GINT_TO_POINTER(s->parent_id), bucket);
        }
        g_ptr_array_add(bucket, s);
    }
    ctx->list_names = NULL;
    if (virtual_view) {
        ctx->list_names = g_hash_table_new_full(g_direct_hash,
                                                g_direct_equal,
                                                NULL, g_free);
        GPtrArray *lists = bt_db_lists(lw->app->db, FALSE);
        for (guint i = 0; i < lists->len; i++) {
            BtList *l = g_ptr_array_index(lists, i);
            g_hash_table_insert(ctx->list_names,
                                GINT_TO_POINTER(l->id),
                                g_strdup(l->name));
        }
        bt_ptr_array_free_lists(lists);
    }
    ctx->bold = bt_app_config_get_bool("bold_task_titles", FALSE);
    ctx->show_done = bt_app_config_get_bool("show_completed", TRUE);
}

static void
task_row_ctx_clear(TaskRowCtx *ctx)
{
    g_hash_table_destroy(ctx->att_counts);
    g_hash_table_destroy(ctx->subs_by_parent);
    bt_ptr_array_free_tasks(ctx->all_subs);
    if (ctx->list_names != NULL)
        g_hash_table_destroy(ctx->list_names);
}

/* append_task_rows() — append `tasks` to `store` through the shared-
 * lookup context, honoring the completed-visibility toggle.  Returns
 * the number of rows actually appended.                                     */
static guint
append_task_rows(GtkListStore *store, GPtrArray *tasks,
                 const TaskRowCtx *ctx)
{
    guint appended = 0;              /* rows actually in the pane           */
    for (guint i = 0; i < tasks->len; i++) {
        BtTask *t = g_ptr_array_index(tasks, i);
        if (!ctx->show_done && t->done)
            continue;                /* toolbar completed-visibility toggle */
        GPtrArray *subs = t->parent_id == 0
            ? g_hash_table_lookup(ctx->subs_by_parent,
                                  GINT_TO_POINTER(t->id))
            : NULL;
        const gchar *list_name = ctx->list_names != NULL
            ? g_hash_table_lookup(ctx->list_names,
                                  GINT_TO_POINTER(t->list_id))
            : NULL;
        gint att_count = GPOINTER_TO_INT(
            g_hash_table_lookup(ctx->att_counts,
                                GINT_TO_POINTER(t->id)));
        gchar *desc = task_desc_markup(t, list_name, att_count, subs,
                                       ctx->bold);
        gchar *due  = bt_due_format(t->due);
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                           TL_ID, t->id,
                           TL_DONE, t->done,
                           TL_DESC, desc,
                           TL_DUE, due,
                           TL_DUE_RAW, t->due,
                           TL_TITLE, t->title,
                           -1);
        g_free(desc);
        g_free(due);
        appended++;
    }
    return appended;
}

/* ---------------------------------------------------------------------------
 * refresh_forecast() — the Weekly Forecast view: seven per-day list
 * views stacked vertically (Sunday through Saturday), each under a
 * day-of-the-week heading, scrolling together as one page.  Rebuilds
 * every day's store and heading; the panel itself is swapped in by
 * refresh_tasks.
 * ------------------------------------------------------------------------- */
static void
refresh_forecast(BtLibrary *lw)
{
    /* Days elapsed since this week's Sunday: GDateTime weekdays run
     * 1 (Monday) through 7 (Sunday), so it is the weekday mod 7.            */
    GDateTime *now = g_date_time_new_now_local();
    gint since_sunday = g_date_time_get_day_of_week(now) % 7;

    /* One outer scroller wraps the whole week — restore its position
     * after the rebuild (clearing the stores collapses the page).           */
    scroll_keep_queue_win(lw->forecast_box);

    TaskRowCtx ctx;                  /* shared lookups (see above)          */
    task_row_ctx_init(lw, &ctx, TRUE);
    guint shown = 0;                 /* task rows across the week           */
    for (gint d = 0; d < 7; d++) {
        gint offset = d - since_sunday;      /* this day vs. today          */
        GDateTime *day = g_date_time_add_days(now, offset);
        gchar *name = g_date_time_format(day, "%A");
        gchar *date = g_date_time_format(day, "%b %-e");
        /* Today wears a small blue dot (the sidebar selection blue)
         * beside its name, plus the "— Today" tag on the date line.         */
        gchar *hdr = g_strdup_printf(
            "%s<b>%s</b>\n<small><span alpha=\"60%%\">%s%s"
            "</span></small>",
            offset == 0 ? "<small><span foreground=\"#5683e0\">"
                          "\xe2\x97\x8f</span></small> " : "",
            name, date,
            offset == 0 ? " \xe2\x80\x94 Today" : "");
        gtk_label_set_markup(GTK_LABEL(lw->day_labels[d]), hdr);
        g_free(hdr);
        g_free(name);
        g_free(date);
        g_date_time_unref(day);

        gtk_list_store_clear(lw->day_stores[d]);
        gint64 lo, hi;               /* the day's local midnight bounds     */
        bt_day_bounds(offset, &lo, &hi);
        GPtrArray *tasks = bt_db_tasks_due_between(lw->app->db, lo, hi);
        guint n = append_task_rows(lw->day_stores[d], tasks, &ctx);
        bt_ptr_array_free_tasks(tasks);
        if (n == 0) {
            /* An empty day still shows a one-row list: an inert dimmed
             * placeholder (id 0 — checkbox hidden, activation ignored).     */
            GtkTreeIter iter;
            gtk_list_store_append(lw->day_stores[d], &iter);
            gtk_list_store_set(lw->day_stores[d], &iter,
                               TL_ID, (gint64)0,
                               TL_DESC, "<i><span alpha=\"55%\">"
                                        "No tasks due</span></i>",
                               TL_DUE, "",
                               -1);
        }
        shown += n;
    }
    g_date_time_unref(now);
    task_row_ctx_clear(&ctx);

    gchar *loc = g_strdup_printf(
        "Weekly Forecast - %u task%s this week",
        shown, shown == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(lw->status_left), loc);
    g_free(loc);
}

/* ---------------------------------------------------------------------------
 * refresh_tasks() — rebuild the task pane for the current selection.
 * The Weekly Forecast has its own panel of seven day views; selecting
 * it swaps that panel in for the regular task list (and back).
 * ------------------------------------------------------------------------- */
static void
refresh_tasks(BtLibrary *lw)
{
    gboolean forecast = lw->sel_kind == SB_KIND_FORECAST;
    gtk_widget_set_visible(lw->task_scroll, !forecast);
    gtk_widget_set_visible(lw->forecast_box, forecast);
    if (forecast) {
        /* Drop the hidden regular pane's rows: a stale selection there
         * would still feed the toolbar's Delete Task.                       */
        gtk_list_store_clear(lw->task_store);
        refresh_forecast(lw);
        return;
    }

    scroll_keep_queue(lw->task_view);
    gtk_list_store_clear(lw->task_store);

    /* Collect the tasks of the current view.                                */
    GPtrArray *tasks;                /* BtTask* rows to show                */
    gboolean virtual_view = TRUE;    /* show the "in <list>" line           */
    const gchar *view_name = "";
    switch (lw->sel_kind) {
    case SB_KIND_PINNED:
        tasks = bt_db_tasks_pinned(lw->app->db);
        view_name = "Favorites";
        break;
    case SB_KIND_ALL:
        tasks = bt_db_tasks_all_visible(lw->app->db);
        view_name = "All Tasks";
        break;
    case SB_KIND_BN_ACTIONS:
        /* A FILTERED view over the mirrored Records items, wherever
         * each one lives — not a list of its own.  virtual_view stays
         * TRUE so every row keeps its "in <list>" line, the only thing
         * that says where the task actually sits.                          */
        tasks = bt_db_tasks_bn_mirror(lw->app->db);
        view_name = "Action Items";
        break;
    case SB_KIND_TODAY: {
        gint64 lo, hi;
        bt_day_bounds(0, &lo, &hi);
        if (bt_app_config_get_bool("due_today_show_overdue", FALSE))
            lo = 1;          /* include all past-due tasks (due > 0)        */
        tasks = bt_db_tasks_due_between(lw->app->db, lo, hi);
        view_name = "Due Today";
        break;
    }
    case SB_KIND_LIST:
    default:
        tasks = bt_db_tasks_toplevel(lw->app->db, lw->sel_id);
        virtual_view = FALSE;
        break;
    }

    TaskRowCtx ctx;                  /* shared lookups (see above)          */
    task_row_ctx_init(lw, &ctx, virtual_view);
    guint shown = append_task_rows(lw->task_store, tasks, &ctx);
    task_row_ctx_clear(&ctx);

    /* Reorder to match the saved manual order (no-op when mode is off).       */
    if (lw->manual_sort)
        task_view_apply_manual_order(lw);

    /* Status bar left: where we are + how many rows.                        */
    BtList *sel_list = virtual_view ? NULL
                       : bt_db_list_get(lw->app->db, lw->sel_id);
    const gchar *where = virtual_view    ? view_name
                       : sel_list != NULL ? sel_list->name : "?";
    gchar *loc = lw->sel_kind == SB_KIND_BN_ACTIONS
        ? g_strdup_printf("%s - %u action item%s",
                          where, shown, shown == 1 ? "" : "s")
        : g_strdup_printf("%s - %u task%s",
                          where, shown, shown == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(lw->status_left), loc);
    g_free(loc);
    bt_list_free(sel_list);

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
        bt_app_config_get_bool("google_sync_enabled", TRUE) &&
        bt_app_config_get_bool("sync_toolbar_button", TRUE));
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
    if (lw->view_show_done_item) {
        g_signal_handlers_block_by_func(lw->view_show_done_item,
                                        on_menu_toggle_done_visible, lw);
        gtk_check_menu_item_set_active(
            GTK_CHECK_MENU_ITEM(lw->view_show_done_item), show);
        g_signal_handlers_unblock_by_func(lw->view_show_done_item,
                                          on_menu_toggle_done_visible, lw);
    }
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

/* manual_sort_icon_refresh() — swap the sort-mode button's icon and tooltip
 * to reflect the current state: menu.png in manual mode (action = switch to
 * column sort), slide.png in column-sort mode (action = switch to manual).  */
static void
manual_sort_icon_refresh(BtLibrary *lw)
{
    gboolean manual = lw->manual_sort;   /* every caller applies first      */
    GtkWidget *icon = bt_app_icon_image_sized(lw->app,
        manual ? "menu" : "slide", 24);
    if (icon) {
        gtk_widget_show(icon);
        gtk_tool_button_set_icon_widget(
            GTK_TOOL_BUTTON(lw->manual_sort_item), icon);
    }
    gtk_tool_item_set_tooltip_text(GTK_TOOL_ITEM(lw->manual_sort_item),
        manual ? "Switch to column sorting"
               : "Switch to manual drag sorting");
    if (lw->view_manual_sort_item) {
        g_signal_handlers_block_by_func(lw->view_manual_sort_item,
                                        on_menu_toggle_manual_sort, lw);
        gtk_check_menu_item_set_active(
            GTK_CHECK_MENU_ITEM(lw->view_manual_sort_item), manual);
        g_signal_handlers_unblock_by_func(lw->view_manual_sort_item,
                                          on_menu_toggle_manual_sort, lw);
    }
}

/* on_toggle_manual_sort() — toolbar button that flips task_list_manual_sort
 * and refreshes the pane.                                                   */
static void
on_toggle_manual_sort(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    gboolean manual = !lw->manual_sort;
    bt_app_config_set("task_list_manual_sort", manual ? "1" : "0");
    task_manual_sort_apply(lw);
    manual_sort_icon_refresh(lw);
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

/* The light variant: task pane only (editor saves — see editor_notify).
 * One exception: an editor's pin flip can be the first/last pin, which
 * adds/removes the sidebar's Pinned Tasks row — rebuild the sidebar
 * only on that 0 <-> nonzero transition (it never runs the BN CLI).         */
static void
notify_tasks_hook(BtApp *app)
{
    BtLibrary *lw = lib_of(app);
    if (lw == NULL)
        return;
    if (sidebar_show_pinned(lw) != lw->pinned_row_shown)
        refresh_sidebar(lw);
    refresh_tasks(lw);
}

/* ---------------------------------------------------------------------------
 * Status-bar fade: 3 s hold then a 1 s fade-out (20 × 50 ms).
 * ------------------------------------------------------------------------- */
#define STATUS_FADE_STEPS    20
#define STATUS_FADE_INTERVAL 50   /* ms */
#define STATUS_FADE_HOLD     3000 /* ms before fade starts */

static void
status_fade_cancel(BtLibrary *lw)
{
    if (lw->status_fade_source != 0) {
        g_source_remove(lw->status_fade_source);
        lw->status_fade_source = 0;
    }
    if (lw->status_fade_step_source != 0) {
        g_source_remove(lw->status_fade_step_source);
        lw->status_fade_step_source = 0;
    }
    lw->status_fade_step = 0;
    g_clear_pointer(&lw->status_fade_text, g_free);
}

static gboolean
status_fade_step_cb(gpointer data)
{
    BtLibrary *lw = lib_of((BtApp *)data);
    if (lw == NULL || lw->status_fade_text == NULL) {
        if (lw != NULL)
            lw->status_fade_step_source = 0;
        return G_SOURCE_REMOVE;
    }
    lw->status_fade_step++;
    if (lw->status_fade_step >= STATUS_FADE_STEPS) {
        gtk_label_set_text(GTK_LABEL(lw->status_right), "");
        lw->status_fade_step_source = 0;
        g_clear_pointer(&lw->status_fade_text, g_free);
        return G_SOURCE_REMOVE;
    }
    gint alpha = 100 - (lw->status_fade_step * 100 / STATUS_FADE_STEPS);
    gchar *escaped = g_markup_escape_text(lw->status_fade_text, -1);
    gchar *markup  = g_strdup_printf("<span alpha=\"%d%%\">%s</span>",
                                     alpha, escaped);
    g_free(escaped);
    gtk_label_set_markup(GTK_LABEL(lw->status_right), markup);
    g_free(markup);
    return G_SOURCE_CONTINUE;
}

static gboolean
status_fade_start_cb(gpointer data)
{
    BtLibrary *lw = lib_of((BtApp *)data);
    if (lw == NULL)
        return G_SOURCE_REMOVE;
    lw->status_fade_source = 0;
    lw->status_fade_text   = g_strdup(gtk_label_get_text(
                                          GTK_LABEL(lw->status_right)));
    lw->status_fade_step   = 0;
    lw->status_fade_step_source =
        g_timeout_add(STATUS_FADE_INTERVAL, status_fade_step_cb, data);
    return G_SOURCE_REMOVE;
}

static void
notify_status_hook(BtApp *app, const gchar *message)
{
    BtLibrary *lw = lib_of(app);
    if (lw == NULL)
        return;
    status_fade_cancel(lw);
    gtk_label_set_text(GTK_LABEL(lw->status_right), message);
    lw->status_fade_source =
        g_timeout_add(STATUS_FADE_HOLD, status_fade_start_cb, app);
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

/* on_sidebar_changed() — selection drives the task pane.  With MULTIPLE
 * selection the cursor row (last pressed) drives sel_kind/sel_id; group
 * rows are tracked but don't switch the task pane.                          */
static void
on_sidebar_changed(GtkTreeSelection *sel, gpointer data)
{
    (void)sel;
    BtLibrary *lw = data;
    if (lw->populating)
        return;
    GtkTreePath *cursor = NULL;
    gtk_tree_view_get_cursor(GTK_TREE_VIEW(lw->sb_view), &cursor, NULL);
    if (cursor == NULL)
        return;
    GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
    GtkTreeIter iter;
    if (gtk_tree_model_get_iter(model, &iter, cursor))
        gtk_tree_model_get(model, &iter,
                           SB_KIND, &lw->sel_kind,
                           SB_ID,   &lw->sel_id,
                           -1);
    gtk_tree_path_free(cursor);
    if (lw->sel_kind != SB_KIND_GROUP)
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

/* selected_task_ids() — ids of every selected task row (the view is
 * multi-select: Ctrl/Cmd-click and Shift-click extend).  Free with
 * g_array_unref.  Records rows (id 0) are excluded.                         */
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

/* on_task_activated() — double-click opens the editor window.  Mirrored
 * Records items are ordinary tasks, so they open the ordinary editor.      */
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
    gint64 id;
    gtk_tree_model_get(model, &iter, TL_ID, &id, -1);
    if (id == 0)                     /* the forecast's "No tasks due"
                                      * placeholder rows                     */
        return;
    bt_editor_open(lw->app, id);
}

/* ---------------------------------------------------------------------------
 * Fade-out animation for tasks marked done while completeds are hidden.
 *
 * 20 steps × 50 ms = 1 s.  Each step wraps TL_DESC in a <span alpha="N%">
 * that decrements from 95 → 0.  At step 20 a full_refresh removes the row.
 * lib_of() / gtk_tree_row_reference_get_path() guard against a window close
 * or another refresh occurring mid-flight.
 * ------------------------------------------------------------------------- */
#define FADE_STEPS    20
#define FADE_INTERVAL 50   /* ms — 20 × 50 ms = 1 s                         */

typedef struct {
    BtApp              *app;
    GtkListStore       *store;
    GtkTreeRowReference *row_ref;
    gchar              *orig_desc;  /* TL_DESC value at fade-start           */
    gint                step;
} FadeCtx;

static void
fade_ctx_free(FadeCtx *ctx)
{
    gtk_tree_row_reference_free(ctx->row_ref);
    g_free(ctx->orig_desc);
    g_free(ctx);
}

/* fade_done() — shared terminal path: decrement lw->pending_fades and
 * call full_refresh only when the last active fade finishes.                */
static void
fade_done(BtLibrary *lw)
{
    if (--lw->pending_fades <= 0) {
        lw->pending_fades = 0;
        full_refresh(lw);
    }
}

static gboolean
fade_step_cb(gpointer data)
{
    FadeCtx   *ctx = data;
    BtLibrary *lw  = lib_of(ctx->app);
    ctx->step++;

    /* Window closed, or a new window opened at a different address — this
     * timer is stale.  Check store membership to detect the latter case.    */
    if (lw == NULL) {
        fade_ctx_free(ctx);
        return G_SOURCE_REMOVE;
    }
    gboolean store_ok = (ctx->store == lw->task_store);
    for (gint i = 0; i < 7 && !store_ok; i++)
        store_ok = (ctx->store == lw->day_stores[i]);
    if (!store_ok) {
        fade_ctx_free(ctx);
        return G_SOURCE_REMOVE;
    }

    GtkTreePath *path = gtk_tree_row_reference_get_path(ctx->row_ref);
    if (path == NULL) {              /* row already gone (external refresh)  */
        fade_done(lw);
        fade_ctx_free(ctx);
        return G_SOURCE_REMOVE;
    }

    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(GTK_TREE_MODEL(ctx->store), &iter, path)) {
        gtk_tree_path_free(path);
        fade_done(lw);
        fade_ctx_free(ctx);
        return G_SOURCE_REMOVE;
    }

    if (ctx->step >= FADE_STEPS) {   /* fade complete — remove this row      */
        gtk_list_store_remove(ctx->store, &iter);
        gtk_tree_path_free(path);
        fade_done(lw);               /* full_refresh fires on the last one   */
        fade_ctx_free(ctx);
        return G_SOURCE_REMOVE;
    }

    gtk_tree_path_free(path);

    /* alpha: 95 → 5 across FADE_STEPS steps (step 1 = 95%, step 19 = 5%)  */
    gint alpha = 100 - (ctx->step * 100 / FADE_STEPS);
    gchar *faded = g_strdup_printf("<span alpha=\"%d%%\">%s</span>",
                                   alpha, ctx->orig_desc);
    gtk_list_store_set(ctx->store, &iter, TL_DESC, faded, -1);
    g_free(faded);

    return G_SOURCE_CONTINUE;
}

/* start_fade() — kick off a fade-out for iter in store.  Reads orig_desc
 * from the store, updates TL_DONE=TRUE, posts a status, and fires the
 * repeating timer.                                                          */
static void
start_fade(BtLibrary *lw, GtkListStore *store, GtkTreeIter *iter,
           const gchar *title)
{
    gchar *orig_desc = NULL;
    gtk_tree_model_get(GTK_TREE_MODEL(store), iter, TL_DESC, &orig_desc, -1);

    gtk_list_store_set(store, iter, TL_DONE, TRUE, -1);

    /* The RAW title: the status bar is a plain-text label (set_text, no
     * markup), so escaping here put a literal "&amp;" on screen for any
     * task with an ampersand in its name.  The fade animation is the only
     * thing that needs markup, and it escapes what it reads back off the
     * label itself.                                                         */
    bt_app_status(lw->app,
                  "\xe2\x80\x9c%s\xe2\x80\x9d \xe2\x80\x94 Completed",
                  title != NULL && *title != '\0' ? title : "Untitled Task");

    GtkTreePath *path =
        gtk_tree_model_get_path(GTK_TREE_MODEL(store), iter);
    FadeCtx *ctx   = g_new0(FadeCtx, 1);
    ctx->app       = lw->app;
    ctx->store     = store;
    ctx->row_ref   = gtk_tree_row_reference_new(GTK_TREE_MODEL(store), path);
    ctx->orig_desc = orig_desc;          /* ownership transferred            */
    ctx->step      = 0;
    gtk_tree_path_free(path);

    lw->pending_fades++;
    g_timeout_add(FADE_INTERVAL, fade_step_cb, ctx);
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
    gchar *title = NULL;
    gtk_tree_model_get(model, &iter,
                       TL_ID, &id, TL_DONE, &done, TL_TITLE, &title, -1);

    /* A mirrored Records item is written like any other task: the tick
     * lands in the database now and rides to Records with the next
     * mirror pass (bnsync.h) — that is what makes the write-back bulk
     * rather than one CLI spawn per click.                                 */
    bt_db_task_set_done(lw->app->db, id, !done);

    /* When hiding completed tasks and a task is just being ticked done,
     * animate a 1 s fade-out before the full_refresh removes the row.      */
    gboolean hiding = !bt_app_config_get_bool("show_completed", TRUE);
    if (!done && hiding) {
        start_fade(lw, lw->task_store, &iter, title);
        g_free(title);
        return;
    }
    g_free(title);
    full_refresh(lw);
}

/* on_forecast_done_toggled() — the done checkbox of a Weekly Forecast
 * day view.  Each day view has its own store (the handler above is
 * bound to the main task store), stashed on the renderer as
 * "bt-model".  Day views hold real tasks only — no Records rows.            */
static void
on_forecast_done_toggled(GtkCellRendererToggle *cell, gchar *path_str,
                         gpointer data)
{
    BtLibrary *lw = data;
    GtkTreeModel *model = g_object_get_data(G_OBJECT(cell), "bt-model");
    GtkTreeIter iter;
    if (model == NULL ||
        !gtk_tree_model_get_iter_from_string(model, &iter, path_str))
        return;
    gint64 id;
    gboolean done;
    gchar *title = NULL;
    gtk_tree_model_get(model, &iter,
                       TL_ID, &id, TL_DONE, &done, TL_TITLE, &title, -1);
    if (id == 0) {
        g_free(title);
        return;
    }
    bt_db_task_set_done(lw->app->db, id, !done);

    gboolean hiding = !bt_app_config_get_bool("show_completed", TRUE);
    if (!done && hiding) {
        start_fade(lw, GTK_LIST_STORE(model), &iter, title);
        g_free(title);
        return;
    }
    g_free(title);
    full_refresh(lw);
}

/* task_row_bg_func() — cell data function giving list rows alternating
 * white / light-blue backgrounds regardless of theme (the Records
 * notes-list stripes).  data is BtLibrary * for the task pane columns so
 * the dragged row can be highlighted; NULL is safe (forecast day views).   */
static void
task_row_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                 GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    (void)col;
    BtLibrary *lw = data;            /* may be NULL for forecast day views  */
    GtkTreePath *path = gtk_tree_model_get_path(model, iter);
    gboolean even =                  /* row parity drives the tint          */
        (gtk_tree_path_get_indices(path)[0] % 2) == 0;
    const gchar *bg = even ? NULL : ROW_TINT;

    /* While dragging, paint the held row amber so it is easy to track.     */
    if (lw && lw->drag_active && lw->drag_row_ref) {
        GtkTreePath *drag_path =
            gtk_tree_row_reference_get_path(lw->drag_row_ref);
        if (drag_path) {
            if (gtk_tree_path_compare(path, drag_path) == 0)
                bg = DRAG_ROW_TINT;
            gtk_tree_path_free(drag_path);
        }
    }

    gtk_tree_path_free(path);
    g_object_set(cell, "cell-background", bg, NULL);
}

/* forecast_toggle_bg_func() — the day views' checkbox data func: the
 * row stripe, plus hiding the checkbox on the "No tasks due"
 * placeholder rows (id 0 — day stores never hold Records rows, so
 * the id alone identifies them).                                            */
static void
forecast_toggle_bg_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                        GtkTreeModel *model, GtkTreeIter *iter,
                        gpointer data)
{
    task_row_bg_func(col, cell, model, iter, data);
    gint64 id;
    gtk_tree_model_get(model, iter, TL_ID, &id, -1);
    g_object_set(cell, "visible", id != 0, NULL);
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

/* sidebar_menu_sync() — push the lists pane's live visibility into the
 * View menu's Show Sidebar check.  The handler is blocked around the
 * set_active so the toolbar → menu sync path cannot recurse (same
 * protocol as hide_done_icon_refresh).                                      */
static void
sidebar_menu_sync(BtLibrary *lw)
{
    if (lw->view_sidebar_item == NULL)
        return;
    g_signal_handlers_block_by_func(lw->view_sidebar_item,
                                    on_menu_toggle_sidebar, lw);
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(lw->view_sidebar_item),
        gtk_widget_get_visible(lw->sidebar_box));
    g_signal_handlers_unblock_by_func(lw->view_sidebar_item,
                                      on_menu_toggle_sidebar, lw);
}

/* sidebar_set_visible() — show or hide the lists pane, persist the
 * choice in `sidebar_visible` and keep the View menu check in step.
 * Both the toolbar button and the menu item route through here.             */
static void
sidebar_set_visible(BtLibrary *lw, gboolean show)
{
    gtk_widget_set_visible(lw->sidebar_box, show);
    bt_app_config_set("sidebar_visible", show ? "1" : "0");
    sidebar_menu_sync(lw);
}

/* ---------------------------------------------------------------------------
 * compact_layout_apply() — put the window in (or take it out of) Compact
 * Layout, per the persisted `compact_layout` flag.
 *
 * Compact hides the whole top toolbar (and its rule) plus the lists pane,
 * and shows the floating New/Delete Task pair pinned to the bottom-right
 * of the task area instead.  Leaving compact restores the toolbar and
 * puts the sidebar back to the user's own `sidebar_visible` preference —
 * compact never overwrites it, so the pane returns as the user left it.
 * Show Sidebar still works while compact (an explicit override).
 *
 * gtk_widget_show() — never show_all() — on the toolbar: its children
 * carry their own visibility (a hidden Sync button must stay hidden).
 * ------------------------------------------------------------------------- */
static void
compact_layout_apply(BtLibrary *lw)
{
    gboolean compact = bt_app_config_get_bool("compact_layout", FALSE);

    gtk_widget_set_visible(lw->toolbar,      !compact);
    gtk_widget_set_visible(lw->toolbar_rule, !compact);
    gtk_widget_set_visible(lw->float_bar,     compact);
    gtk_widget_set_visible(lw->sidebar_box,
        !compact && bt_app_config_get_bool("sidebar_visible", FALSE));
    sidebar_menu_sync(lw);
}

/* on_toggle_sidebar() — toolbar show/hide button for the lists pane:
 * the task view takes the whole window while it is hidden (mirrors the
 * Records "Folders" toggle).                                                */
static void
on_toggle_sidebar(GtkWidget *widget, gpointer data)
{
    (void)widget;
    BtLibrary *lw = data;
    sidebar_set_visible(lw, !gtk_widget_get_visible(lw->sidebar_box));
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

/* ---------------------------------------------------------------------------
 * Group context-menu actions (forward-declared; menu built in
 * on_sb_button_press below).
 * ------------------------------------------------------------------------- */
static void
on_sb_ctx_move_to_group(GtkWidget *item, gpointer data)
{
    BtLibrary *lw   = data;
    GArray    *ids  = g_object_get_data(G_OBJECT(item), "bt-ids");
    gint64 group_id = (gint64)(gintptr)
        g_object_get_data(G_OBJECT(item), "bt-group-id");
    for (guint i = 0; i < ids->len; i++)
        bt_db_list_set_group(lw->app->db,
                             g_array_index(ids, gint64, i), group_id);
    full_refresh(lw);
}

static void
on_sb_ctx_remove_from_group(GtkWidget *item, gpointer data)
{
    BtLibrary *lw = data;
    GArray   *ids = g_object_get_data(G_OBJECT(item), "bt-ids");
    for (guint i = 0; i < ids->len; i++)
        bt_db_list_set_group(lw->app->db,
                             g_array_index(ids, gint64, i), 0);
    full_refresh(lw);
}

/* run_group_name_dialog() — modal entry for a group name; fills *out and
 * returns TRUE on accept with non-empty text, FALSE otherwise.              */
static gboolean
run_group_name_dialog(BtLibrary *lw, const gchar *title, const gchar *button,
                      const gchar *initial, gchar **out)
{
    GtkWidget *dlg = gtk_dialog_new_with_buttons(
        title, GTK_WINDOW(lw->window),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "Cancel", GTK_RESPONSE_CANCEL,
        button, GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *entry = gtk_entry_new();
    if (initial && *initial)
        gtk_entry_set_text(GTK_ENTRY(entry), initial);
    else
        gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Group name");
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dlg));
    gtk_box_pack_start(GTK_BOX(box), gtk_label_new("Group name:"),
                       FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 4);
    gtk_widget_show_all(dlg);
    gboolean accepted = FALSE;
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        const gchar *name = gtk_entry_get_text(GTK_ENTRY(entry));
        if (name && *name) {
            *out     = g_strdup(name);
            accepted = TRUE;
        }
    }
    gtk_widget_destroy(dlg);
    return accepted;
}

static void
on_sb_ctx_rename_group(GtkWidget *item, gpointer data)
{
    BtLibrary *lw   = data;
    gint64 group_id = (gint64)(gintptr)
        g_object_get_data(G_OBJECT(item), "bt-group-id");
    GPtrArray *groups  = bt_db_groups(lw->app->db);
    gchar     *current = NULL;
    for (guint i = 0; i < groups->len; i++) {
        BtGroup *g = g_ptr_array_index(groups, i);
        if (g->id == group_id) { current = g_strdup(g->name); break; }
    }
    bt_ptr_array_free_groups(groups);
    gchar *name = NULL;
    if (run_group_name_dialog(lw, "Rename Group", "Rename",
                              current ? current : "", &name)) {
        bt_db_group_rename(lw->app->db, group_id, name);
        full_refresh(lw);
        g_free(name);
    }
    g_free(current);
}

static void
on_sb_ctx_delete_group(GtkWidget *item, gpointer data)
{
    BtLibrary *lw   = data;
    gint64 group_id = (gint64)(gintptr)
        g_object_get_data(G_OBJECT(item), "bt-group-id");
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
        "Remove this group? Its lists will become ungrouped.");
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    if (resp == GTK_RESPONSE_OK) {
        if (lw->sel_kind == SB_KIND_GROUP && lw->sel_id == group_id) {
            lw->sel_kind = SB_KIND_LIST;
            lw->sel_id   = 0;
        }
        bt_db_group_delete(lw->app->db, group_id);
        full_refresh(lw);
    }
}

static void on_new_list(GtkWidget *, gpointer);
static void on_new_group(GtkWidget *, gpointer);
static void on_edit_list(GtkWidget *, gpointer);
static void on_delete_list(GtkWidget *, gpointer);

/* on_sb_button_press() — right-click on the sidebar: always offers New List
 * and New Group; adds Edit/Delete for SB_KIND_LIST, Rename/Remove for
 * SB_KIND_GROUP, and group-assignment items when groups exist.  Right-clicking
 * inside an existing multi-selection keeps it; outside collapses to the
 * clicked row first.                                                         */
static gboolean
on_sb_button_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    if (event->button != 3) return FALSE;
    BtLibrary *lw = data;

    GtkTreePath *path = NULL;
    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
                                  (gint)event->x, (gint)event->y,
                                  &path, NULL, NULL, NULL);
    gint   kind = -1;
    gint64 id   = 0;
    if (path) {
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
        GtkTreeIter it;
        if (gtk_tree_model_get_iter(model, &it, path))
            gtk_tree_model_get(model, &it, SB_KIND, &kind, SB_ID, &id, -1);
        if (!gtk_tree_selection_path_is_selected(sel, path)) {
            gtk_tree_selection_unselect_all(sel);
            gtk_tree_selection_select_path(sel, path);
            gtk_tree_view_set_cursor(GTK_TREE_VIEW(widget), path, NULL, FALSE);
        }
        gtk_tree_path_free(path);
    }

    GtkWidget *menu = gtk_menu_new();
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);

    /* New List and New Group are always available. */
    GtkWidget *new_list = gtk_menu_item_new_with_label("New List");
    g_signal_connect(new_list, "activate", G_CALLBACK(on_new_list), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), new_list);

    GtkWidget *new_grp = gtk_menu_item_new_with_label("New Group");
    g_signal_connect(new_grp, "activate", G_CALLBACK(on_new_group), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), new_grp);

    if (kind == SB_KIND_LIST) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());

        GtkWidget *edit = gtk_menu_item_new_with_label("Edit List");
        g_signal_connect(edit, "activate", G_CALLBACK(on_edit_list), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), edit);

        GtkWidget *del = gtk_menu_item_new_with_label("Delete List");
        g_signal_connect(del, "activate", G_CALLBACK(on_delete_list), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), del);

        /* Collect selected list ids and group membership for move items. */
        GtkTreeSelection *sel =
            gtk_tree_view_get_selection(GTK_TREE_VIEW(widget));
        GtkTreeModel *model = GTK_TREE_MODEL(lw->sb_store);
        GList *rows = gtk_tree_selection_get_selected_rows(sel, &model);
        GArray *ids = g_array_new(FALSE, FALSE, sizeof(gint64));
        gboolean any_grouped = FALSE;
        for (GList *r = rows; r; r = r->next) {
            GtkTreeIter ri;
            if (!gtk_tree_model_get_iter(model, &ri, r->data)) continue;
            gint k; gint64 lid;
            gtk_tree_model_get(model, &ri, SB_KIND, &k, SB_ID, &lid, -1);
            if (k != SB_KIND_LIST) continue;
            g_array_append_val(ids, lid);
            BtList *l = bt_db_list_get(lw->app->db, lid);
            if (l) {
                if (l->group_id != 0) any_grouped = TRUE;
                bt_list_free(l);
            }
        }
        g_list_free_full(rows, (GDestroyNotify)gtk_tree_path_free);

        GPtrArray *groups = bt_db_groups(lw->app->db);
        if (groups->len > 0 || any_grouped) {
            gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                                  gtk_separator_menu_item_new());
            if (groups->len > 0) {
                GtkWidget *move = gtk_menu_item_new_with_label("Move to Group");
                GtkWidget *sub  = gtk_menu_new();
                for (guint i = 0; i < groups->len; i++) {
                    BtGroup *g = g_ptr_array_index(groups, i);
                    GtkWidget *gi = gtk_menu_item_new_with_label(g->name);
                    g_object_set_data_full(G_OBJECT(gi), "bt-ids",
                                           g_array_ref(ids),
                                           (GDestroyNotify)g_array_unref);
                    g_object_set_data(G_OBJECT(gi), "bt-group-id",
                                      (gpointer)(gintptr)g->id);
                    g_signal_connect(gi, "activate",
                                     G_CALLBACK(on_sb_ctx_move_to_group), lw);
                    gtk_menu_shell_append(GTK_MENU_SHELL(sub), gi);
                }
                gtk_menu_item_set_submenu(GTK_MENU_ITEM(move), sub);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), move);
            }
            if (any_grouped) {
                GtkWidget *rem =
                    gtk_menu_item_new_with_label("Remove from Group");
                g_object_set_data_full(G_OBJECT(rem), "bt-ids",
                                       g_array_ref(ids),
                                       (GDestroyNotify)g_array_unref);
                g_signal_connect(rem, "activate",
                                 G_CALLBACK(on_sb_ctx_remove_from_group), lw);
                gtk_menu_shell_append(GTK_MENU_SHELL(menu), rem);
            }
        }
        bt_ptr_array_free_groups(groups);
        g_array_unref(ids);

    } else if (kind == SB_KIND_GROUP) {
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());

        GtkWidget *rename = gtk_menu_item_new_with_label("Rename Group");
        g_object_set_data(G_OBJECT(rename), "bt-group-id",
                          (gpointer)(gintptr)id);
        g_signal_connect(rename, "activate",
                         G_CALLBACK(on_sb_ctx_rename_group), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), rename);

        GtkWidget *del = gtk_menu_item_new_with_label("Remove Group");
        g_object_set_data(G_OBJECT(del), "bt-group-id",
                          (gpointer)(gintptr)id);
        g_signal_connect(del, "activate",
                         G_CALLBACK(on_sb_ctx_delete_group), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), del);
    }

    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
    return TRUE;
}

/* on_new_group() — prompt for a name and create a new list group.           */
static void
on_new_group(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    gchar *name = NULL;
    if (run_group_name_dialog(lw, "New Group", "Create", NULL, &name)) {
        gint64 gid = bt_db_group_create(lw->app->db, name);
        if (gid == 0)
            bt_app_status(lw->app, "Failed to create group");
        else
            full_refresh(lw);
        g_free(name);
    }
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
        bt_app_status(lw->app, "Action Items is a view, not a list \xe2\x80\x94 "
                      "edit the list each item lives in");
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

/* on_sidebar_activated() — double-click on a real list opens the Edit
 * List dialog (the first click of the pair already settled the
 * selection on the row).  Metas, the Lists header (which keeps its
 * default expand/collapse) and the Records row do nothing.                  */
static void
on_sidebar_activated(GtkTreeView *view, GtkTreePath *path,
                     GtkTreeViewColumn *col, gpointer data)
{
    (void)col;
    BtLibrary *lw = data;
    GtkTreeModel *model = gtk_tree_view_get_model(view);
    GtkTreeIter iter;
    if (!gtk_tree_model_get_iter(model, &iter, path))
        return;
    gint kind;
    gtk_tree_model_get(model, &iter, SB_KIND, &kind, -1);
    if (kind == SB_KIND_LIST)
        on_edit_list(NULL, lw);
}

/* on_delete_list() — confirm + tombstone the selected real list; when a
 * group is selected, delegate to on_sb_ctx_delete_group.                    */
static void
on_delete_list(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    if (lw->sel_kind == SB_KIND_GROUP) {
        gint64 gid = lw->sel_id;
        GtkWidget *dlg = gtk_message_dialog_new(
            GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
            GTK_MESSAGE_QUESTION, GTK_BUTTONS_OK_CANCEL,
            "Remove this group? Its lists will become ungrouped.");
        gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
        gtk_widget_destroy(dlg);
        if (resp == GTK_RESPONSE_OK) {
            lw->sel_kind = SB_KIND_LIST;
            lw->sel_id   = 0;
            bt_db_group_delete(lw->app->db, gid);
            full_refresh(lw);
        }
        return;
    }
    if (lw->sel_kind == SB_KIND_BN_ACTIONS) {
        bt_app_status(lw->app, "Action Items is a view, not a list \xe2\x80\x94 "
                      "hide it in File \xe2\x86\x92 Settings\xe2\x80\xa6");
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
     * 400 from any client) — block it here, like the Records list.          */
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
        /* Drop the list's manual-order key with it — nothing else ever
         * would, so the ini otherwise grows a dead manual_order_list_<id>
         * entry for every list the user has ever deleted.                   */
        gchar *order_key = list_order_key(id);
        bt_app_config_set(order_key, NULL);   /* NULL removes the key       */
        g_free(order_key);
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
    bt_editor_open_new(lw->app, id);  /* the Save / Cancel variant          */
}

/* on_delete_task() — confirm + tombstone the selected task.                 */
static void
on_delete_task(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    /* Mirrored Records items delete like any other task: the row is
     * tombstoned and its uid parked in bn_deleted, so the next mirror
     * pass does not helpfully re-create what was just deleted.             */
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
static GArray    *item_ids(GtkWidget *item);

/* on_ctx_info() — open the task editor (same as double-clicking the row).   */
static void
on_ctx_info(GtkWidget *item, gpointer data)
{
    BtLibrary *lw = data;
    GArray *ids = item_ids(item);
    if (ids == NULL || ids->len == 0)
        return;
    bt_editor_open(lw->app, g_array_index(ids, gint64, 0));
}

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

/* on_ctx_set_pinned() — Pin / Unpin on the selection (local-only; the
 * sidebar's Pinned Tasks row follows via full_refresh).                     */
static void
on_ctx_set_pinned(GtkWidget *item, gpointer data)
{
    BtLibrary *lw = data;
    GArray *ids = item_ids(item);
    gboolean pinned = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "bt-flag"));
    for (guint i = 0; i < ids->len; i++)
        bt_db_task_set_pinned(lw->app->db,
                              g_array_index(ids, gint64, i), pinned);
    full_refresh(lw);
    bt_app_status(lw->app, "%s %u task%s",
                  pinned ? "Added to Favorites" : "Removed from Favorites",
                  ids->len, ids->len == 1 ? "" : "s");
}

/* on_ctx_set_priority() — Set / Clear High Priority on the selection
 * (local-only; the views re-sort via full_refresh).                         */
static void
on_ctx_set_priority(GtkWidget *item, gpointer data)
{
    BtLibrary *lw = data;
    GArray *ids = item_ids(item);
    gboolean priority = GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "bt-flag"));
    for (guint i = 0; i < ids->len; i++)
        bt_db_task_set_priority(lw->app->db,
                                g_array_index(ids, gint64, i), priority);
    full_refresh(lw);
    bt_app_status(lw->app, "%s high priority on %u task%s",
                  priority ? "Set" : "Cleared",
                  ids->len, ids->len == 1 ? "" : "s");
}

/* ctx_flag_item() — one bulk context-menu item: the selection rides on
 * the item as its own g_array_ref ("bt-ids"), the boolean to apply as
 * "bt-flag".                                                                */
static void
ctx_flag_item(BtLibrary *lw, GtkWidget *menu, GArray *ids,
              const gchar *label, gboolean flag, GCallback cb)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_object_set_data_full(G_OBJECT(item), "bt-ids", g_array_ref(ids),
                           (GDestroyNotify)g_array_unref);
    g_object_set_data(G_OBJECT(item), "bt-flag", GINT_TO_POINTER(flag));
    g_signal_connect(item, "activate", cb, lw);
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
 * selection.
 * ------------------------------------------------------------------------- */
static gboolean
on_task_button_press(GtkWidget *view, GdkEventButton *event, gpointer data)
{
    BtLibrary *lw = data;

    /* Left-click in the drag handle column starts a manual reorder. */
    if (event->button == 1 && lw->manual_sort) {
        GtkTreePath      *path = NULL;
        GtkTreeViewColumn *col = NULL;
        if (gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(view),
            (gint)event->x, (gint)event->y, &path, &col, NULL, NULL)) {
            GtkTreeViewColumn *cdrag =
                g_object_get_data(G_OBJECT(lw->task_view), "bt-cdrag");
            if (col == cdrag) {
                GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
                GtkTreeIter it;
                gint64 id = 0;
                if (gtk_tree_model_get_iter(model, &it, path))
                    gtk_tree_model_get(model, &it, TL_ID, &id, -1);
                if (id != 0) {
                    lw->drag_active = TRUE;
                    if (lw->drag_row_ref != NULL)
                        gtk_tree_row_reference_free(lw->drag_row_ref);
                    lw->drag_row_ref =
                        gtk_tree_row_reference_new(model, path);
                    gtk_widget_queue_draw(view); /* paint amber highlight    */
                    gtk_tree_path_free(path);
                    return TRUE;       /* consume — don't change selection   */
                }
            }
            gtk_tree_path_free(path);
        }
    }

    /* Right-click in the header area: event->window is the header GdkWindow,
     * not the bin_window, regardless of column clickability.  Detect this
     * by window identity and route to the column/sort menu.                 */
    if (event->button == 3 &&
        event->window != gtk_tree_view_get_bin_window(GTK_TREE_VIEW(view)))
        return on_column_header_press(view, event, lw);

    if (event->button != 3)
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

    /* Info… — single row only; opens the editor (same as double-click).     */
    if (single) {
        GtkWidget *info_item = gtk_menu_item_new_with_label("Info\xe2\x80\xa6");
        g_object_set_data_full(G_OBJECT(info_item), "bt-ids",
                               g_array_ref(ids),
                               (GDestroyNotify)g_array_unref);
        g_signal_connect(info_item, "activate",
                         G_CALLBACK(on_ctx_info), lw);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), info_item);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                              gtk_separator_menu_item_new());
    }

    /* Mark Complete / Mark Incomplete — single row: only the applicable
     * direction; multi: both (selection may be mixed).                      */
    if (single && t != NULL)
        ctx_done_item(lw, menu, ids, TRUE, !t->done);
    else {
        ctx_done_item(lw, menu, ids, FALSE, TRUE);
        ctx_done_item(lw, menu, ids, FALSE, FALSE);
    }

    gtk_menu_shell_append(GTK_MENU_SHELL(menu),
                          gtk_separator_menu_item_new());

    /* Pin / Unpin and High Priority: a single row gets just the action
     * that applies to it; a multi-selection (possibly mixed states)
     * gets both directions.                                                 */
    if (single && t != NULL) {
        ctx_flag_item(lw, menu, ids,
                      t->pinned ? "Remove from Favorites" : "Add to Favorites",
                      !t->pinned, G_CALLBACK(on_ctx_set_pinned));
        ctx_flag_item(lw, menu, ids,
                      t->priority ? "Clear High Priority"
                                  : "Set High Priority",
                      !t->priority, G_CALLBACK(on_ctx_set_priority));
    } else {
        ctx_flag_item(lw, menu, ids, "Add All to Favorites", TRUE,
                      G_CALLBACK(on_ctx_set_pinned));
        ctx_flag_item(lw, menu, ids, "Remove All from Favorites", FALSE,
                      G_CALLBACK(on_ctx_set_pinned));
        ctx_flag_item(lw, menu, ids, "Set All High Priority", TRUE,
                      G_CALLBACK(on_ctx_set_priority));
        ctx_flag_item(lw, menu, ids, "Clear All High Priority", FALSE,
                      G_CALLBACK(on_ctx_set_priority));
    }

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
    bt_sync_signin_done(app, GTK_WINDOW(lw->window), app->db->path,
                        ok, error, sync_done);
}

/* on_sync() — the toolbar Sync button.  Sign-in is per session: when the
 * in-memory token is missing/expired this re-runs the browser flow first
 * (usually a silent redirect), then syncs.
 *
 * The Records mirror runs FIRST and on its own worker: it is the cheap
 * local pass, and going first means anything it pulls in from Records
 * is already in the database when the Google pass reads it, so a new
 * action item reaches Google in one press rather than two.                 */
static void
on_sync(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;
    if (bt_app_config_get_bool("blue_notes_sync", FALSE))
        bt_bnsync_start(lw->app, lw->app->db->path, NULL, NULL);
    if (!bt_app_config_get_bool("google_sync_enabled", TRUE)) {
        bt_app_status(lw->app, "Google Tasks sync is disabled \xe2\x80\x94 "
                      "enable it in File \xe2\x86\x92 Settings\xe2\x80\xa6");
        return;
    }
    if (!bt_oauth_have_client()) {
        bt_app_status(lw->app, "Google sync is not configured \xe2\x80\x94 "
                      "see File \xe2\x86\x92 Settings\xe2\x80\xa6");
        bt_settings_window_open(lw->app, GTK_WINDOW(lw->window),
                                lw->app->db->path);
        return;
    }
    gtk_widget_set_sensitive(lw->sync_item, FALSE);
    if (bt_oauth_authenticated()) {
        bt_sync_start(lw->app, lw->app->db->path, sync_done, NULL);
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
    bt_settings_window_open(lw->app, GTK_WINDOW(lw->window), lw->app->db->path);
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

/* find_gtk_image() — first GtkImage in a widget subtree (depth-first).
 * Used to reach GtkAboutDialog's internal logo image, which the public
 * API only feeds with a plain (blurry-on-Retina) GdkPixbuf.                 */
static GtkWidget *
find_gtk_image(GtkWidget *widget)
{
    if (GTK_IS_IMAGE(widget))
        return widget;
    GtkWidget *hit = NULL;           /* first image found in the subtree    */
    if (GTK_IS_CONTAINER(widget)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *l = kids; l != NULL && hit == NULL; l = l->next)
            hit = find_gtk_image(l->data);
        g_list_free(kids);
    }
    return hit;
}

/* ---------------------------------------------------------------------------
 * on_open_db() — File → Open Database File…: pick a .db file and open it as
 * the new default or for this session only.
 * ------------------------------------------------------------------------- */
static void
on_open_db(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    BtLibrary *lw  = user_data;
    BtApp     *app = lw->app;

    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Open Database", GTK_WINDOW(lw->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Open",   GTK_RESPONSE_ACCEPT,
        NULL);
    GtkFileFilter *ff = gtk_file_filter_new();
    gtk_file_filter_set_name(ff, "SQLite Database (*.db)");
    gtk_file_filter_add_pattern(ff, "*.db");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), ff);

    gchar *file_path = NULL;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        file_path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    if (file_path == NULL)
        return;

    if (g_strcmp0(file_path, app->db->path) == 0) { /* already open         */
        g_free(file_path);
        return;
    }

    /* Ask: permanent default or this session only? */
    gchar *display = g_path_get_basename(file_path);
    GtkWidget *dlg = gtk_message_dialog_new(
        GTK_WINDOW(lw->window), GTK_DIALOG_MODAL,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_NONE,
        "Open \xe2\x80\x9c%s\xe2\x80\x9d as your new default database, "
        "or for this session only?", display);
    g_free(display);
    gtk_window_set_title(GTK_WINDOW(dlg), "Lists - Open Database");
    gtk_dialog_add_buttons(GTK_DIALOG(dlg),
        "_Cancel",         GTK_RESPONSE_CANCEL,
        "_Session Only",   1,
        "Set as _Default", 2,
        NULL);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);

    if (resp == GTK_RESPONSE_CANCEL || resp == GTK_RESPONSE_DELETE_EVENT) {
        g_free(file_path);
        return;
    }
    gboolean set_default = (resp == 2);

    bt_editor_close_all(app);
    gchar *old_path = g_strdup(app->db->path);
    bt_db_close(app->db);
    GError *gerr = NULL;
    app->db = bt_db_open(file_path, &gerr);

    if (app->db == NULL) {
        bt_app_notice(GTK_WINDOW(lw->window), GTK_MESSAGE_ERROR,
                      "Lists - Database Error",
                      "Could not open:\n%s\n\n%s",
                      file_path,
                      gerr != NULL ? gerr->message : "Unknown error");
        g_clear_error(&gerr);
        app->db = bt_db_open(old_path, &gerr); /* revert                   */
        if (app->db == NULL)
            g_critical("on_open_db: cannot revert to %s: %s", old_path,
                       gerr != NULL ? gerr->message : "?");
        g_clear_error(&gerr);
        g_free(old_path);
        g_free(file_path);
        return;
    }

    if (set_default) {
        gchar *dir = g_path_get_dirname(file_path);
        g_free(app->db_dir);
        app->db_dir = g_strdup(dir);
        bt_app_config_set("db_dir", dir);
        g_free(dir);
    }

    g_free(old_path);
    g_free(file_path);

    /* Both timers carry the db path they were armed with, so a switch
     * must re-arm BOTH or the mirror keeps writing to the old file.       */
    bt_sync_auto_start(app, app->db->path);
    bt_bnsync_auto_start(app, app->db->path);
    bt_app_notify_changed(app);
    bt_app_status(app, "Opened %s", app->db->path);
}

/* on_menu_toggle_done_visible() — View → Show Completed: read the new check
 * state (class handler has already toggled it), sync config + toolbar.
 * Connected via "toggled" so the handler runs after priv->active is settled;
 * hide_done_icon_refresh blocks this handler when it calls set_active so the
 * toolbar → menu sync path does not recurse.                                 */
static void
on_menu_toggle_done_visible(GtkWidget *w, gpointer data)
{
    BtLibrary *lw = data;
    gboolean show = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
    bt_app_config_set("show_completed", show ? "1" : "0");
    hide_done_icon_refresh(lw);
    refresh_tasks(lw);
}

/* on_menu_toggle_manual_sort() — View → Manual Sort: mirror of the above
 * for the task_list_manual_sort config key.  Same block/unblock protocol
 * with manual_sort_icon_refresh.                                             */
static void
on_menu_toggle_manual_sort(GtkWidget *w, gpointer data)
{
    BtLibrary *lw = data;
    gboolean manual = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
    bt_app_config_set("task_list_manual_sort", manual ? "1" : "0");
    task_manual_sort_apply(lw);
    manual_sort_icon_refresh(lw);
    refresh_tasks(lw);
}

/* on_menu_toggle_compact() — View → Compact Layout: persist the flag and
 * re-apply the layout (toolbar + sidebar out, floating buttons in).          */
static void
on_menu_toggle_compact(GtkWidget *w, gpointer data)
{
    BtLibrary *lw = data;
    gboolean compact = gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w));
    bt_app_config_set("compact_layout", compact ? "1" : "0");
    compact_layout_apply(lw);
}

/* on_menu_toggle_sidebar() — View → Show Sidebar: the menu twin of the
 * toolbar Sidebar button (sidebar_set_visible persists and re-syncs the
 * check, which is a no-op here since the item is already in that state).     */
static void
on_menu_toggle_sidebar(GtkWidget *w, gpointer data)
{
    BtLibrary *lw = data;
    sidebar_set_visible(lw,
        gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(w)));
}

/* on_menu_about() — File → About and the toolbar About button: the
 * standard about dialog with the app logo, version, database vitals and
 * a link to the BSD license (the Records About, retinted).                  */
static void
on_menu_about(GtkWidget *w, gpointer data)
{
    (void)w;
    BtLibrary *lw = data;

    /* 128x128-logical logo from document.png, decoded at the display's
     * scale factor so it stays sharp on Retina.                             */
    gint sf = gtk_widget_get_scale_factor(lw->window);
    gchar *icon_path = g_build_filename(lw->app->icons_dir,
                                        "document.png", NULL);
    GdkPixbuf *logo = gdk_pixbuf_new_from_file_at_size(icon_path,
                                                       128 * sf, 128 * sf,
                                                       NULL);
    g_free(icon_path);

    const gchar *authors[] = { "Ian Campbell", "Claude", NULL };

    GtkWidget *dialog = gtk_about_dialog_new();
    gtk_window_set_transient_for(GTK_WINDOW(dialog),
                                 GTK_WINDOW(lw->window));
    gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(dialog),
                                      "Lists");
    gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(dialog), BT_VERSION);
    if (logo != NULL) {
        /* set_logo() first (it makes the internal image visible and
         * sized), then swap that image's content for a cairo surface
         * with the device scale — the pixbuf API renders 1 buffer px
         * per logical px and looks soft on HiDPI.                          */
        GdkPixbuf *at_128 = (sf > 1)
            ? gdk_pixbuf_scale_simple(logo, 128, 128, GDK_INTERP_BILINEAR)
            : g_object_ref(logo);
        gtk_about_dialog_set_logo(GTK_ABOUT_DIALOG(dialog), at_128);
        g_object_unref(at_128);

        if (sf > 1) {
            GtkWidget *img = find_gtk_image(
                gtk_dialog_get_content_area(GTK_DIALOG(dialog)));
            if (img != NULL) {
                cairo_surface_t *surface =
                    gdk_cairo_surface_create_from_pixbuf(logo, sf, NULL);
                gtk_image_set_from_surface(GTK_IMAGE(img), surface);
                cairo_surface_destroy(surface);
            }
        }
        g_object_unref(logo);
    }
    gtk_about_dialog_set_authors(GTK_ABOUT_DIALOG(dialog), authors);

    /* Database vitals: task/list counts, location, on-disk size.           */
    gint n_tasks, n_lists;           /* totals across the database          */
    bt_db_totals(lw->app->db, &n_tasks, &n_lists);
    GStatBuf st;                     /* for the database file size          */
    const gchar *db_path = lw->app->db->path;
    gchar *size_str = (g_stat(db_path, &st) == 0)
                      ? g_format_size((guint64)st.st_size)
                      : g_strdup("unknown");

    /* __DATE__/__TIME__ expand when this file is compiled — the closest
     * portable thing to a "last compiled" stamp.                           */
    gchar *comments = g_strdup_printf(
        "Task lists with subtasks, due dates and Google Tasks sync.\n"
        "Companion app to Records.\n\n"
        "Compiled " __DATE__ " " __TIME__ "\n\n"
        "Database: %s\n"
        "%d tasks in %d lists \xe2\x80\x94 %s on disk",
        db_path, n_tasks, n_lists, size_str);
    gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(dialog), comments);
    g_free(comments);
    g_free(size_str);
    gtk_about_dialog_set_license_type(GTK_ABOUT_DIALOG(dialog),
                                      GTK_LICENSE_BSD_3);
    gtk_about_dialog_set_website(GTK_ABOUT_DIALOG(dialog),
                                 "https://opensource.org/license/bsd-3-clause");
    gtk_about_dialog_set_website_label(GTK_ABOUT_DIALOG(dialog),
                                       "BSD License");

    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* about_button_fit_style() — the About button shows the centered logo in
 * every toolbar style; the "About" text appears ONLY in text-only mode
 * (where there would otherwise be nothing to click).  The item is a plain
 * GtkToolItem wrapping a GtkButton whose single child gets swapped — a
 * GtkToolButton would reserve empty label space under the icon in
 * icons-above-text mode.  The logo and label widgets live as object data
 * ("bt-logo"/"bt-label", owning refs) so they survive being unparented.
 *   item  — the About tool item.
 *   style — the toolbar style being applied.                                */
static void
about_button_fit_style(GtkToolItem *item, GtkToolbarStyle style)
{
    GtkWidget *btn   = gtk_bin_get_child(GTK_BIN(item));
    GtkWidget *logo  = g_object_get_data(G_OBJECT(item), "bt-logo");
    GtkWidget *label = g_object_get_data(G_OBJECT(item), "bt-label");

    GtkWidget *want =                /* the child this style calls for      */
        (style == GTK_TOOLBAR_TEXT) ? label : logo;
    GtkWidget *cur = gtk_bin_get_child(GTK_BIN(btn));
    if (cur == want)
        return;
    if (cur != NULL)
        gtk_container_remove(GTK_CONTAINER(btn), cur);
    gtk_container_add(GTK_CONTAINER(btn), want);
    gtk_widget_show(want);
}

/* on_toolbar_style_changed() — keep the About button's label rule
 * applied when the toolbar style changes.                                   */
static void
on_toolbar_style_changed(GtkToolbar *toolbar, GtkToolbarStyle style,
                         gpointer user_data)
{
    (void)toolbar;
    about_button_fit_style(GTK_TOOL_ITEM(user_data), style);
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
 * of) the native macOS menu bar (see header).  Mirrors Records: the
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

/* compact_bar_button() — one floating-bar button: the 24 px local icon
 * (Pango-markup glyph when the PNG is missing, matching the toolbar's
 * fallback rule) wired to `cb`, appended to `box`.                          */
static void
compact_bar_button(BtLibrary *lw, GtkWidget *box, const gchar *icon,
                   const gchar *fallback_markup, const gchar *tooltip,
                   GCallback cb)
{
    GtkWidget *btn   = gtk_button_new();
    GtkWidget *image = bt_app_icon_image_sized(lw->app, icon, 24);
    if (image == NULL) {
        image = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(image), fallback_markup);
    }
    gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(btn), image);
    gtk_widget_set_tooltip_text(btn, tooltip);
    g_signal_connect(btn, "clicked", cb, lw);
    gtk_box_pack_start(GTK_BOX(box), btn, FALSE, FALSE, 0);
}

/* float_bar_css() — the floating pill's plate: the window background, with
 * a border shaded off the same color so it reads as a raised object in
 * either a light or a dark theme.                                           */
static gchar *
float_bar_css(const GdkRGBA *bg)
{
    /* Light themes want a DARKER border than the plate, dark themes a
     * lighter one — pick the direction from the plate's own luminance so
     * the edge stays visible either way.                                    */
    gdouble lum = 0.299 * bg->red + 0.587 * bg->green + 0.114 * bg->blue;
    gchar *c   = rgb_of(bg);
    gchar *css = g_strdup_printf(
        "box {"
        "  background-color: %s;"
        "  border: 1px solid shade(%s, %s);"
        "  border-radius: 8px;"
        "  padding: 2px;"
        "}",
        c, c, lum > 0.5 ? "0.80" : "1.35");
    g_free(c);
    return css;
}

/* ---------------------------------------------------------------------------
 * compact_bar_new() — Compact Layout's floating toolbar: New Task and
 * Delete Task as a two-button pill pinned 20 px in from the bottom and
 * right edges of the task area.  Same icons and actions as the top
 * toolbar's pair, so the compact window keeps both task verbs.
 *
 * Returned as an overlay child (halign/valign END + 20 px margins do the
 * pinning); also stored as lw->float_bar, which compact_layout_apply
 * shows and hides.  The bar is NOT registered with
 * bt_app_register_toolbar — it is icons-only by design and must not grow
 * labels when the toolbar style changes.
 * ------------------------------------------------------------------------- */
static GtkWidget *
compact_bar_new(BtLibrary *lw)
{
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    /* A rounded, bordered plate so the buttons read as one floating
     * object over the task rows rather than two loose glyphs.  Both colors
     * come from the theme's @theme_bg_color (the border a shade of it), the
     * same resolution the column headers use — hardcoding the light-theme
     * grays put a white slab over a dark theme's task rows.                 */
    themed_bg_css_apply(bar, float_bar_css);
    compact_bar_button(lw, bar, "add2", "+", "Create a task in the "
                       "selected list", G_CALLBACK(on_new_task));
    compact_bar_button(lw, bar, "remove", "\xe2\x88\x92",
                       "Delete the selected task",
                       G_CALLBACK(on_delete_task));

    gtk_widget_set_halign(bar, GTK_ALIGN_END);
    gtk_widget_set_valign(bar, GTK_ALIGN_END);
    gtk_widget_set_margin_end(bar, 20);
    gtk_widget_set_margin_bottom(bar, 20);
    lw->float_bar = bar;
    return bar;
}

/* ---------------------------------------------------------------------------
 * forecast_day_section() — build one Weekly Forecast day section: a
 * heading label (day of the week over the date, set per refresh) above
 * a two-column (done + task) list view with its own store, sharing the
 * main pane's stripes, activation handler and row markup.  The view is
 * framed but NOT scrolled — it takes its natural full-content height;
 * the whole week scrolls together in the panel's one outer scroller.
 * Fills lw->day_labels / day_stores / day_views [d].
 * ------------------------------------------------------------------------- */
static GtkWidget *
forecast_day_section(BtLibrary *lw, gint d)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    lw->day_labels[d] = gtk_label_new(NULL);
    gtk_label_set_justify(GTK_LABEL(lw->day_labels[d]),
                          GTK_JUSTIFY_CENTER);
    gtk_label_set_ellipsize(GTK_LABEL(lw->day_labels[d]),
                            PANGO_ELLIPSIZE_END);
    gtk_box_pack_start(GTK_BOX(box), lw->day_labels[d], FALSE, FALSE, 2);

    lw->day_stores[d] = gtk_list_store_new(TL_N_COLS, G_TYPE_INT64,
                                           G_TYPE_BOOLEAN, G_TYPE_STRING,
                                           G_TYPE_STRING, G_TYPE_INT64,
                                           G_TYPE_STRING);
    lw->day_views[d] = gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(lw->day_stores[d]));
    g_object_unref(lw->day_stores[d]);
    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(lw->day_views[d]),
                                      FALSE);
    gtk_tree_view_set_enable_search(GTK_TREE_VIEW(lw->day_views[d]),
                                    FALSE);
    /* No selection: seven views would each keep their own, leaving up
     * to seven "selected" rows on screen.  Double-click activation
     * (and the checkbox) work without one.                                   */
    gtk_tree_selection_set_mode(
        gtk_tree_view_get_selection(GTK_TREE_VIEW(lw->day_views[d])),
        GTK_SELECTION_NONE);
    g_signal_connect(lw->day_views[d], "row-activated",
                     G_CALLBACK(on_task_activated), lw);

    GtkCellRenderer *done_cell = gtk_cell_renderer_toggle_new();
    g_object_set_data(G_OBJECT(done_cell), "bt-model",
                      lw->day_stores[d]);
    g_signal_connect(done_cell, "toggled",
                     G_CALLBACK(on_forecast_done_toggled), lw);
    GtkTreeViewColumn *cdone =
        gtk_tree_view_column_new_with_attributes("\xe2\x9c\x93",
            done_cell, "active", TL_DONE, NULL);
    gtk_tree_view_column_set_cell_data_func(cdone, done_cell,
                                            forecast_toggle_bg_func,
                                            NULL, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->day_views[d]), cdone);

    GtkCellRenderer *desc_cell = gtk_cell_renderer_text_new();
    g_object_set(desc_cell,
                 "ypad", 6,
                 "ellipsize", PANGO_ELLIPSIZE_END,
                 NULL);
    GtkTreeViewColumn *cdesc =
        gtk_tree_view_column_new_with_attributes("Task", desc_cell,
            "markup", TL_DESC, NULL);
    gtk_tree_view_column_set_cell_data_func(cdesc, desc_cell,
                                            task_row_bg_func, NULL, NULL);
    gtk_tree_view_column_set_expand(cdesc, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->day_views[d]), cdesc);

    /* A frame so each day reads as its own list even where the white
     * rows meet the 6 px gaps.                                               */
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_IN);
    gtk_container_add(GTK_CONTAINER(frame), lw->day_views[d]);
    gtk_box_pack_start(GTK_BOX(box), frame, FALSE, FALSE, 0);
    return box;
}

/* on_paned_position() — track the divider for persistence.  Fires on
 * every step of a drag, so it only caches; on_library_destroy does the
 * single config write.                                                      */
static void
on_paned_position(GObject *paned, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    BtLibrary *lw = data;
    gint pos = gtk_paned_get_position(GTK_PANED(paned));
    /* Ignore the collapse the Sidebar toggle causes: hiding the pane
     * drives the position to 0, and storing that would reopen the next
     * session with an invisible sidebar and no obvious way back.           */
    if (pos > 0)
        lw->sb_width = pos;
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
    /* Likewise the divider: a sidebar narrowed by hand has to come back
     * that width, or every launch undoes the adjustment.  Written here,
     * not per drag step — "notify::position" fires continuously while
     * the handle moves and each write rewrites the ini.                    */
    if (lw->sb_width > 0) {
        gchar *v = g_strdup_printf("%d", lw->sb_width);
        bt_app_config_set("sidebar_width", v);
        g_free(v);
    }
    /* Hooks come down BEFORE the editors: a closing editor's final save
     * would otherwise fire notify_changed → bt_editor_refresh_all, which
     * can destroy sibling editors mid-teardown (a failing Records CLI
     * closes its editors on reload) and leave close_all's snapshot list
     * holding freed windows.                                                */
    lw->app->notify_changed = NULL;
    lw->app->notify_tasks   = NULL;
    lw->app->notify_status  = NULL;
    lw->app->library_window = NULL;
    status_fade_cancel(lw);
    bt_editor_close_all(lw->app);
    if (lw->drag_row_ref  != NULL)
        gtk_tree_row_reference_free(lw->drag_row_ref);
    if (lw->drag_lock_ref != NULL)
        gtk_tree_row_reference_free(lw->drag_lock_ref);
    g_clear_object(&lw->drag_cursor);
    if (lw->group_expanded != NULL)
        g_hash_table_destroy(lw->group_expanded);
    g_free(lw);
}

/* ===========================================================================
 * Manual sort: order persistence, drag handlers, mode toggle.
 * =========================================================================== */

/* list_order_key() — a real list's manual-order config key.  Its own
 * function so on_delete_list can name the key it has to remove without
 * repeating the format string.  New string (g_free).                        */
static gchar *
list_order_key(gint64 list_id)
{
    return g_strdup_printf("manual_order_list_%" G_GINT64_FORMAT, list_id);
}

/* view_order_key() — the config key for the current view's manual sort
 * order, or NULL if the view doesn't support it.  New string (g_free).       */
static gchar *
view_order_key(BtLibrary *lw)
{
    switch (lw->sel_kind) {
    case SB_KIND_LIST:
        return list_order_key(lw->sel_id);
    case SB_KIND_ALL:
        return g_strdup("manual_order_all");
    case SB_KIND_PINNED:
        return g_strdup("manual_order_pinned");
    case SB_KIND_TODAY:
        return g_strdup("manual_order_today");
    case SB_KIND_BN_ACTIONS:
        return g_strdup("manual_order_bn_actions");
    default:
        return NULL;
    }
}

/* task_view_save_manual_order() — serialize the task pane's current row
 * order to config as a comma-separated list of task ids.  Every row is a
 * real task now (mirrored Records items included), so the old
 * "NOTEID:ORD" token form is gone; a saved order still holding those
 * tokens simply finds no match and those entries drop out.                  */
static void
task_view_save_manual_order(BtLibrary *lw)
{
    gchar *key = view_order_key(lw);
    if (key == NULL) return;
    GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
    GString      *s     = g_string_new(NULL);
    GtkTreeIter   iter;
    if (gtk_tree_model_get_iter_first(model, &iter)) {
        do {
            gint64 id;
            gtk_tree_model_get(model, &iter, TL_ID, &id, -1);
            if (id != 0) {
                if (s->len > 0) g_string_append_c(s, ',');
                g_string_append_printf(s, "%" G_GINT64_FORMAT, id);
            }
        } while (gtk_tree_model_iter_next(model, &iter));
    }
    bt_app_config_set(key, s->str);
    g_string_free(s, TRUE);
    g_free(key);
}

/* task_view_apply_manual_order() — after refresh_tasks populates the store,
 * reorder rows to match the saved manual order for the current view.  Tasks
 * absent from the saved list appear at the tail; id=0 rows follow them.       */
static void
task_view_apply_manual_order(BtLibrary *lw)
{
    gchar *key = view_order_key(lw);
    if (key == NULL) return;
    gchar *saved = bt_app_config_get(key);
    g_free(key);
    if (saved == NULL || *saved == '\0') { g_free(saved); return; }
    GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
    gint n = gtk_tree_model_iter_n_children(model, NULL);
    if (n <= 1) { g_free(saved); return; }

    /* Snapshot current row IDs (in display order). */
    gint64  *ids  = g_new(gint64, n);
    GtkTreeIter  iter;
    gtk_tree_model_get_iter_first(model, &iter);
    for (gint i = 0; i < n; i++) {
        gtk_tree_model_get(model, &iter, TL_ID, &ids[i], -1);
        gtk_tree_model_iter_next(model, &iter);
    }

    /* Build new_order: saved entries first (in saved sequence), remainder
     * (new rows not yet in saved list) appended at tail.  A pre-mirror
     * order may still hold "NOTEID:ORD" tokens; they parse to 0, match
     * nothing, and are skipped.                                             */
    gint     *new_order = g_new(gint, n);
    gboolean *placed    = g_new0(gboolean, n);
    gint      fill      = 0;
    gchar   **parts     = g_strsplit(saved, ",", -1);
    g_free(saved);
    for (gint i = 0; parts[i] != NULL; i++) {
        gint64 id = g_ascii_strtoll(parts[i], NULL, 10);
        if (id == 0)
            continue;
        for (gint j = 0; j < n; j++) {
            if (ids[j] == id && !placed[j]) {
                new_order[fill++] = j;
                placed[j]         = TRUE;
                break;
            }
        }
    }
    g_strfreev(parts);
    for (gint i = 0; i < n; i++)
        if (!placed[i])
            new_order[fill++] = i;
    gtk_list_store_reorder(lw->task_store, new_order);
    g_free(new_order);
    g_free(placed);
    g_free(ids);
}

/* drag_handle_func() — cell data func for the drag handle column.  The row
 * stripe is all it does: the ⠿ glyph and its dimming are constants, so they
 * are set once on the renderer at construction instead of on every draw.
 * Kept as its own function (rather than pointing the column straight at
 * task_row_bg_func) because the column is where a per-row "this row cannot
 * move" state would land if one is ever added.                              */
static void
drag_handle_func(GtkTreeViewColumn *col, GtkCellRenderer *cell,
                 GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
    task_row_bg_func(col, cell, model, iter, data);
}

/* ---------------------------------------------------------------------------
 * task_drag_set_cursor() — update the cursor on the task view's GdkWindow:
 * "ns-resize" while over the drag handle column or while dragging, else
 * reset to the window default.
 *
 * Runs on EVERY motion event over the task view, so it holds no allocation:
 * the manual-sort flag comes from lw->manual_sort rather than the ini, and
 * the cursor is made once and kept on lw (created lazily — the display is
 * only reachable from a realized widget).
 * ------------------------------------------------------------------------- */
static void
task_drag_set_cursor(GtkWidget *widget, BtLibrary *lw, gdouble x, gdouble y)
{
    GdkWindow  *win = gtk_widget_get_window(widget);
    if (win == NULL) return;
    gboolean want_resize = lw->drag_active;
    if (!want_resize && lw->manual_sort) {
        GtkTreeViewColumn *over = NULL;
        gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
            (gint)x, (gint)y, NULL, &over, NULL, NULL);
        GtkTreeViewColumn *cdrag =
            g_object_get_data(G_OBJECT(lw->task_view), "bt-cdrag");
        want_resize = (over != NULL && over == cdrag);
    }
    if (want_resize && lw->drag_cursor == NULL)
        lw->drag_cursor = gdk_cursor_new_from_name(
            gtk_widget_get_display(widget), "ns-resize");
    /* NULL restores the window default — and is also what a display that
     * cannot supply "ns-resize" leaves us with, which is the right
     * fallback rather than a guessed stock cursor.                          */
    gdk_window_set_cursor(win, want_resize ? lw->drag_cursor : NULL);
}

/* on_task_leave_notify() — restore the default cursor when the pointer
 * leaves the task view (e.g. moving to another widget).                       */
static gboolean
on_task_leave_notify(GtkWidget *widget, GdkEventCrossing *ev, gpointer data)
{
    (void)ev; (void)data;
    GdkWindow *win = gtk_widget_get_window(widget);
    if (win) gdk_window_set_cursor(win, NULL);
    return FALSE;
}

/* on_task_drag_motion() — when the pointer enters a different row, swap
 * that row with the dragged row so the dragged item ends up under the
 * cursor.  Uses get_path_at_pos (no hysteresis) so the swap fires the
 * moment the pointer crosses a row boundary.                                  */
static gboolean
on_task_drag_motion(GtkWidget *widget, GdkEventMotion *ev, gpointer data)
{
    BtLibrary *lw = data;
    task_drag_set_cursor(widget, lw, ev->x, ev->y);
    if (!lw->drag_active || lw->drag_row_ref == NULL)
        return FALSE;

    GtkTreePath *at_path = NULL;
    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget),
        1, (gint)ev->y, &at_path, NULL, NULL, NULL);
    if (at_path == NULL)
        return FALSE;

    GtkTreePath *drag_path =
        gtk_tree_row_reference_get_path(lw->drag_row_ref);
    if (drag_path == NULL) { gtk_tree_path_free(at_path); return FALSE; }

    if (gtk_tree_path_compare(at_path, drag_path) == 0) {
        /* Cursor is back on the dragged row — clear the anti-flicker lock
         * so the next row the cursor enters will swap normally.              */
        if (lw->drag_lock_ref != NULL) {
            gtk_tree_row_reference_free(lw->drag_lock_ref);
            lw->drag_lock_ref = NULL;
        }
    } else {
        /* Check whether this is the row we just swapped with.  Row refs
         * auto-update through moves, so lock_path tracks the locked row
         * even after surrounding rows have shifted.                          */
        GtkTreePath *lock_path = lw->drag_lock_ref
            ? gtk_tree_row_reference_get_path(lw->drag_lock_ref) : NULL;
        gboolean locked = lock_path &&
            gtk_tree_path_compare(at_path, lock_path) == 0;
        if (lock_path) gtk_tree_path_free(lock_path);

        if (!locked) {
            GtkTreeIter  at_it, drag_it;
            GtkTreeModel *model = GTK_TREE_MODEL(lw->task_store);
            if (gtk_tree_model_get_iter(model, &at_it,   at_path) &&
                gtk_tree_model_get_iter(model, &drag_it, drag_path)) {
                gint64 at_id;
                gtk_tree_model_get(model, &at_it, TL_ID, &at_id, -1);

                /* Every row carries a real id now — mirrored Records
                 * items included — so the old "skip past the contiguous
                 * BN section" dance is gone: any row is a swap target.    */
                if (at_id != 0) {
                    gint drag_idx = gtk_tree_path_get_indices(drag_path)[0];
                    gint at_idx   = gtk_tree_path_get_indices(at_path)[0];
                    /* Lock the target BEFORE the move; the row ref will
                     * auto-update to track it at its new position.           */
                    if (lw->drag_lock_ref != NULL)
                        gtk_tree_row_reference_free(lw->drag_lock_ref);
                    lw->drag_lock_ref =
                        gtk_tree_row_reference_new(model, at_path);
                    if (at_idx < drag_idx)
                        gtk_list_store_move_before(lw->task_store,
                                                  &drag_it, &at_it);
                    else
                        gtk_list_store_move_after(lw->task_store,
                                                 &drag_it, &at_it);
                }
            }
        }
    }

    gtk_tree_path_free(at_path);
    gtk_tree_path_free(drag_path);
    return FALSE;
}

/* on_task_drag_release() — button released: end the drag and persist the
 * new row order.                                                              */
static gboolean
on_task_drag_release(GtkWidget *widget, GdkEventButton *ev, gpointer data)
{
    (void)widget; (void)ev;
    BtLibrary *lw = data;
    if (!lw->drag_active) return FALSE;
    lw->drag_active = FALSE;
    if (lw->drag_row_ref != NULL) {
        gtk_tree_row_reference_free(lw->drag_row_ref);
        lw->drag_row_ref = NULL;
    }
    if (lw->drag_lock_ref != NULL) {
        gtk_tree_row_reference_free(lw->drag_lock_ref);
        lw->drag_lock_ref = NULL;
    }
    task_view_save_manual_order(lw);
    gtk_widget_queue_draw(widget);   /* clear the amber highlight            */
    GdkWindow *win = gtk_widget_get_window(widget);
    if (win) gdk_window_set_cursor(win, NULL);
    return FALSE;
}

/* task_manual_sort_apply() — sync the task view to the current
 * task_list_manual_sort config: show/hide drag handle, enable/disable
 * column-header click-to-sort, and clear any active sort indicator.
 * ALSO the single writer of lw->manual_sort, the cached copy the
 * per-motion and per-refresh paths read instead of the ini — every writer
 * of the config key calls this straight afterwards, so the cache cannot
 * drift.                                                                    */
static void
task_manual_sort_apply(BtLibrary *lw)
{
    gboolean manual =
        bt_app_config_get_bool("task_list_manual_sort", FALSE);
    lw->manual_sort = manual;
    GtkTreeViewColumn *cdrag =
        g_object_get_data(G_OBJECT(lw->task_view), "bt-cdrag");
    GtkTreeViewColumn *cdone =
        g_object_get_data(G_OBJECT(lw->task_view), "bt-cdone");
    GtkTreeViewColumn *cdesc =
        g_object_get_data(G_OBJECT(lw->task_view), "bt-cdesc");
    GtkTreeViewColumn *cdue  =
        g_object_get_data(G_OBJECT(lw->task_view), "bt-cdue");
    if (cdrag) gtk_tree_view_column_set_visible(cdrag, manual);
    if (cdone) gtk_tree_view_column_set_clickable(cdone, !manual);
    if (cdesc) gtk_tree_view_column_set_clickable(cdesc, !manual);
    if (cdue)  gtk_tree_view_column_set_clickable(cdue,  !manual);
    if (manual)
        gtk_tree_sortable_set_sort_column_id(
            GTK_TREE_SORTABLE(lw->task_store),
            GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID,
            GTK_SORT_ASCENDING);
}

/* on_column_toggled() — a column visibility check item was clicked: update
 * the column visibility and persist in config.                              */
static void
on_column_toggled(GtkCheckMenuItem *item, gpointer data)
{
    (void)data;
    GtkTreeViewColumn *col = g_object_get_data(G_OBJECT(item), "bt-col");
    BtLibrary         *lw  = g_object_get_data(G_OBJECT(item), "bt-lw");
    if (!col || !lw) return;
    const gchar *key = g_object_get_data(G_OBJECT(col), "bt-colkey");
    gboolean vis = gtk_check_menu_item_get_active(item);
    gtk_tree_view_column_set_visible(col, vis);
    if (key) {
        gchar *cfg = g_strdup_printf("col_%s_visible", key);
        bt_app_config_set(cfg, vis ? "1" : "0");
        g_free(cfg);
    }
}

/* task_columns_apply() — restore persisted column visibility.               */
static void
task_columns_apply(BtLibrary *lw)
{
    GtkTreeViewColumn *cdone =
        g_object_get_data(G_OBJECT(lw->task_view), "bt-cdone");
    GtkTreeViewColumn *cdue  =
        g_object_get_data(G_OBJECT(lw->task_view), "bt-cdue");
    if (cdone)
        gtk_tree_view_column_set_visible(cdone,
            bt_app_config_get_bool("col_done_visible", TRUE));
    if (cdue)
        gtk_tree_view_column_set_visible(cdue,
            bt_app_config_get_bool("col_due_visible", TRUE));
}

/* on_column_header_press() — right-click on any column header pops a menu
 * of check items for the hidable columns (Done and Due Date; Task always
 * shows and has no entry).                                                  */
static gboolean
on_column_header_press(GtkWidget *btn, GdkEventButton *ev, gpointer data)
{
    (void)btn;
    if (ev->button != 3) return FALSE;
    BtLibrary *lw = data;
    GtkWidget *menu = gtk_menu_new();

    GList *cols = gtk_tree_view_get_columns(GTK_TREE_VIEW(lw->task_view));
    for (GList *l = cols; l; l = l->next) {
        GtkTreeViewColumn *col   = l->data;
        const gchar       *key   =
            g_object_get_data(G_OBJECT(col), "bt-colkey");
        const gchar       *label =
            g_object_get_data(G_OBJECT(col), "bt-collabel");
        if (!key) continue;
        GtkWidget *item = gtk_check_menu_item_new_with_label(label);
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
            gtk_tree_view_column_get_visible(col));
        g_object_set_data(G_OBJECT(item), "bt-col", col);
        g_object_set_data(G_OBJECT(item), "bt-lw",  lw);
        g_signal_connect(item, "toggled",
                         G_CALLBACK(on_column_toggled), NULL);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    g_list_free(cols);
    gtk_widget_show_all(menu);
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)ev);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * bt_library_window_new() — build the library window (see header).
 * ------------------------------------------------------------------------- */
GtkWidget *
bt_library_window_new(BtApp *app)
{
    BtLibrary *lw = g_new0(BtLibrary, 1);
    lw->app = app;
    /* Seeded here, not left to task_manual_sort_apply at the end of this
     * function: the toolbar icon, tooltip and View-menu check are all built
     * before that call and read the cache.                                  */
    lw->manual_sort =
        bt_app_config_get_bool("task_list_manual_sort", FALSE);
    lw->sel_kind = SB_KIND_LIST;     /* refresh falls back to first list    */
    lw->group_expanded = g_hash_table_new(g_direct_hash, g_direct_equal);

    lw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(lw->window), "Lists");
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
    menu_item(file_menu, "New Task", G_CALLBACK(on_new_task), lw);
    menu_item(file_menu, "New List\xe2\x80\xa6", G_CALLBACK(on_new_list), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    menu_item(file_menu, "Sync Now", G_CALLBACK(on_sync), lw);
    menu_item(file_menu, "Clear Completed Tasks",
              G_CALLBACK(on_menu_clear_completed), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    menu_item(file_menu, "Open Database File\xe2\x80\xa6",
              G_CALLBACK(on_open_db), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    menu_item(file_menu, "Settings\xe2\x80\xa6",
              G_CALLBACK(on_menu_settings), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    menu_item(file_menu, "About", G_CALLBACK(on_menu_about), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(file_menu),
                          gtk_separator_menu_item_new());
    menu_item(file_menu, "Quit", G_CALLBACK(on_menu_quit), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), file_item);

    GtkWidget *view_menu = gtk_menu_new();
    GtkWidget *view_item = gtk_menu_item_new_with_label("View");
    gtk_menu_item_set_submenu(GTK_MENU_ITEM(view_item), view_menu);
    lw->view_show_done_item =
        gtk_check_menu_item_new_with_label("Show Completed");
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(lw->view_show_done_item),
        bt_app_config_get_bool("show_completed", TRUE));
    g_signal_connect(lw->view_show_done_item, "toggled",
                     G_CALLBACK(on_menu_toggle_done_visible), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          lw->view_show_done_item);
    lw->view_manual_sort_item =
        gtk_check_menu_item_new_with_label("Manual Sort");
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(lw->view_manual_sort_item), lw->manual_sort);
    g_signal_connect(lw->view_manual_sort_item, "toggled",
                     G_CALLBACK(on_menu_toggle_manual_sort), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          lw->view_manual_sort_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          gtk_separator_menu_item_new());
    /* Show Sidebar mirrors the toolbar's Sidebar button (both write
     * `sidebar_visible`); Compact Layout strips the chrome down to the
     * task pane plus the floating New/Delete pair.  Both are applied
     * after the construction-time show_all, at the end of this function.    */
    lw->view_sidebar_item =
        gtk_check_menu_item_new_with_label("Show Sidebar");
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(lw->view_sidebar_item),
        bt_app_config_get_bool("sidebar_visible", FALSE));
    g_signal_connect(lw->view_sidebar_item, "toggled",
                     G_CALLBACK(on_menu_toggle_sidebar), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          lw->view_sidebar_item);
    lw->view_compact_item =
        gtk_check_menu_item_new_with_label("Compact Layout");
    gtk_check_menu_item_set_active(
        GTK_CHECK_MENU_ITEM(lw->view_compact_item),
        bt_app_config_get_bool("compact_layout", FALSE));
    g_signal_connect(lw->view_compact_item, "toggled",
                     G_CALLBACK(on_menu_toggle_compact), lw);
    gtk_menu_shell_append(GTK_MENU_SHELL(view_menu),
                          lw->view_compact_item);
    gtk_menu_shell_append(GTK_MENU_SHELL(menubar), view_item);

    /* Remembered so the menu can be moved into the native macOS menu
     * bar (see bt_library_apply_native_menubar).                            */
    g_object_set_data(G_OBJECT(lw->window), "bt-menubar", menubar);
    gtk_box_pack_start(GTK_BOX(vbox), menubar, FALSE, FALSE, 0);

    /* --- Toolbar ---------------------------------------------------------- */
    /* Icon names are icons/-relative paths; the curated set lives in
     * icons/ (case-exact for Linux).  Layout: sidebar toggle,
     * a drawn divider, then the task buttons and Sync.                      */
    GtkWidget *toolbar = gtk_toolbar_new();
    lw->toolbar = toolbar;           /* Compact Layout hides it whole       */
    /* Small-toolbar metrics — the Records bar height.                       */
    gtk_toolbar_set_icon_size(GTK_TOOLBAR(toolbar),
                              GTK_ICON_SIZE_SMALL_TOOLBAR);
    tool_button(lw, GTK_TOOLBAR(toolbar), "sidebar",
                "\xe2\x97\xa7", "Sidebar", "Show or hide the lists pane",
                G_CALLBACK(on_toggle_sidebar));
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    lw->sync_item = GTK_WIDGET(tool_button(lw, GTK_TOOLBAR(toolbar),
        "google-symbol", "\xe2\x9f\xb3", "Sync",
        "Sync with Google Tasks now", G_CALLBACK(on_sync)));
    lw->hide_done_item = GTK_WIDGET(tool_button(lw, GTK_TOOLBAR(toolbar),
        "hidden", "\xf0\x9f\x91\x81", "Completed",
        "Hide completed tasks", G_CALLBACK(on_toggle_done_visible)));
    hide_done_icon_refresh(lw);      /* the persisted state's icon          */
    lw->manual_sort_item = GTK_WIDGET(tool_button(lw, GTK_TOOLBAR(toolbar),
        "menu", "\xe2\x89\x8b", "Sort Mode",
        "Switch to manual drag sorting", G_CALLBACK(on_toggle_manual_sort)));
    manual_sort_icon_refresh(lw);    /* the persisted state's tooltip       */
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar),
                       gtk_separator_tool_item_new(), -1);

    tool_button(lw, GTK_TOOLBAR(toolbar), "add2", NULL,
                "New Task", "Create a task in the selected list",
                G_CALLBACK(on_new_task));
    tool_button(lw, GTK_TOOLBAR(toolbar), "remove", NULL,
                "Delete Task", "Delete the selected task",
                G_CALLBACK(on_delete_task));

    /* Expanding blank separator pushes the About button to the right
     * edge (the Records layout).                                            */
    GtkToolItem *spacer = gtk_separator_tool_item_new();
    gtk_separator_tool_item_set_draw(GTK_SEPARATOR_TOOL_ITEM(spacer),
                                     FALSE);
    gtk_tool_item_set_expand(spacer, TRUE);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), spacer, -1);

    /* The About button at the far right.  Built by hand because the
     * child must stay centered (see about_button_fit_style).                */
    GtkToolItem *about_item = gtk_tool_item_new();
    GtkWidget *about_btn = gtk_button_new();
    gtk_button_set_relief(GTK_BUTTON(about_btn), GTK_RELIEF_NONE);
    gtk_container_add(GTK_CONTAINER(about_item), about_btn);
    {
        GtkWidget *logo =            /* the icon-mode child                 */
            bt_app_icon_image_sized(app, "document", 24);
        if (logo == NULL)
            logo = gtk_label_new("\xf0\x9f\x8f\xa0");    /* 🏠 fallback     */
        GtkWidget *label = gtk_label_new("About");   /* text-mode child     */

        /* Keep owning refs so removal from the button never frees them.    */
        g_object_set_data_full(G_OBJECT(about_item), "bt-logo",
                               g_object_ref_sink(logo), g_object_unref);
        g_object_set_data_full(G_OBJECT(about_item), "bt-label",
                               g_object_ref_sink(label), g_object_unref);
    }
    gtk_tool_item_set_tooltip_text(about_item, "About Lists");
    g_signal_connect(about_btn, "clicked",
                     G_CALLBACK(on_menu_about), lw);
    gtk_toolbar_insert(GTK_TOOLBAR(toolbar), about_item, -1);

    /* Logo only, except in text-only mode — applied now and re-applied
     * on every style switch (register_toolbar sets the style below).        */
    about_button_fit_style(about_item, app->toolbar_style);
    g_signal_connect(toolbar, "style-changed",
                     G_CALLBACK(on_toolbar_style_changed), about_item);

    bt_app_register_toolbar(app, toolbar);
    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);
    /* Thin rule between the toolbar and the panes (Records look).           */
    lw->toolbar_rule = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(vbox), lw->toolbar_rule, FALSE, FALSE, 0);

    /* --- Paned: sidebar | tasks ------------------------------------------ */
    /* The panes sit in a GtkOverlay so Compact Layout's floating button
     * pair can hover over the bottom-right corner of the task area.  The
     * overlay wraps the panes rather than the whole window box so the
     * float never covers the status bar's event messages.                   */
    GtkWidget *overlay = gtk_overlay_new();
    gtk_box_pack_start(GTK_BOX(vbox), overlay, TRUE, TRUE, 0);
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gchar *sbw = bt_app_config_get("sidebar_width");
    lw->sb_width = sbw != NULL ? atoi(sbw) : 220;
    g_free(sbw);
    if (lw->sb_width <= 0)
        lw->sb_width = 220;
    gtk_paned_set_position(GTK_PANED(paned), lw->sb_width);
    g_signal_connect(paned, "notify::position",
                     G_CALLBACK(on_paned_position), lw);
    gtk_container_add(GTK_CONTAINER(overlay), paned);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), compact_bar_new(lw));

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
    /* Ellipsize so a narrowed sidebar reads "Weekly Fore…" rather than
     * slicing a label mid-glyph; the renderer needs a width to ellipsize
     * against, which the FIXED column below gives it.                      */
    g_object_set(sb_cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
    GtkTreeViewColumn *sb_col =
        gtk_tree_view_column_new_with_attributes("Lists", sb_cell,
            "text", SB_LABEL, "weight", SB_WEIGHT, NULL);
    /* FIXED, not the default GROW_ONLY: GROW_ONLY ratchets — once a long
     * name has been shown the column keeps that width even after the row
     * is gone, so the floor only ever went up.                             */
    gtk_tree_view_column_set_sizing(sb_col, GTK_TREE_VIEW_COLUMN_FIXED);
    /* FIXED sizing needs an explicit width or the column has none; keep
     * it small and let expand=TRUE fill whatever the pane actually is.
     * The 40 px is a floor on the TREE VIEW's request only — the
     * EXTERNAL scroller does not pass that up to the pane.                 */
    gtk_tree_view_column_set_fixed_width(sb_col, 40);
    gtk_tree_view_column_set_expand(sb_col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->sb_view), sb_col);
    /* Sidebar palette (Records): light grey backdrop (rows and the
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
    gtk_tree_selection_set_mode(sb_sel, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_set_select_function(sb_sel, sb_row_selectable,
                                           lw, NULL);
    g_signal_connect(sb_sel, "changed",
                     G_CALLBACK(on_sidebar_changed), lw);
    g_signal_connect(lw->sb_view, "row-activated",
                     G_CALLBACK(on_sidebar_activated), lw);
    g_signal_connect(lw->sb_view, "button-press-event",
                     G_CALLBACK(on_sb_button_press), lw);
    GtkWidget *sb_scroll = gtk_scrolled_window_new(NULL, NULL);
    /* EXTERNAL, not NEVER, horizontally: NEVER makes the scroller demand
     * its child's FULL width as a minimum, so the widest row (a long
     * list name) became a floor the divider could not be dragged past.
     * EXTERNAL scrolls without ever showing a scrollbar, which is what
     * lets the pane go narrower than the content.                          */
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sb_scroll),
                                   GTK_POLICY_EXTERNAL,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(sb_scroll), lw->sb_view);

    /* shrink=TRUE (4th arg): the pane may allocate the sidebar LESS than
     * its minimum.  With shrink=FALSE the divider stops at that minimum
     * no matter what the scroll policy says — both are needed.            */
    gtk_paned_pack1(GTK_PANED(paned), sb_scroll, FALSE, TRUE);
    lw->sidebar_box = sb_scroll;     /* for the toolbar show/hide toggle    */

    /* Task pane.                                                            */
    lw->task_store = gtk_list_store_new(TL_N_COLS, G_TYPE_INT64,
                                        G_TYPE_BOOLEAN, G_TYPE_STRING,
                                        G_TYPE_STRING, G_TYPE_INT64,
                                        G_TYPE_STRING);
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

    /* Drag handle column — shown only in manual sort mode.  The glyph comes
     * from the renderer itself, not the model and not the data func: it is
     * the same on every row, and a data func runs per DRAW, so setting it
     * there was two property notifications per visible row per redraw.
     * Dimming is Pango `alpha` on the markup, never a fixed gray — a gray
     * is unreadable on the blue selection, while alpha rides whatever
     * foreground the row already has.                                       */
    GtkCellRenderer   *drag_cell = gtk_cell_renderer_text_new();
    g_object_set(drag_cell, "ypad", 8, "xpad", 4,
                 "markup",                       /* ⠿ handle glyph          */
                 "<span alpha=\"55%\">\xe2\xa0\xbf</span>", NULL);
    GtkTreeViewColumn *cdrag     = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(cdrag, "");
    gtk_tree_view_column_pack_start(cdrag, drag_cell, FALSE);
    gtk_tree_view_column_set_cell_data_func(cdrag, drag_cell,
                                            drag_handle_func, lw, NULL);
    gtk_tree_view_column_set_clickable(cdrag, FALSE);
    gtk_tree_view_column_set_sizing(cdrag, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(cdrag, 26);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdrag);

    /* Done checkbox column.  Every column's renderer also runs the
     * stripe data func — the alternating background must span the row.     */
    GtkCellRenderer *done_cell = gtk_cell_renderer_toggle_new();
    g_signal_connect(done_cell, "toggled",
                     G_CALLBACK(on_task_done_toggled), lw);
    GtkTreeViewColumn *cdone =
        gtk_tree_view_column_new_with_attributes("\xe2\x9c\x93",
            done_cell, "active", TL_DONE, NULL);
    gtk_tree_view_column_set_cell_data_func(cdone, done_cell,
                                            task_row_bg_func, lw, NULL);
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
                                            task_row_bg_func, lw, NULL);
    gtk_tree_view_column_set_expand(cdesc, TRUE);
    gtk_tree_view_column_set_resizable(cdesc, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdesc);

    /* Due Date column, urgency-tinted, sortable (undated last).             */
    GtkCellRenderer *due_cell = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *cdue =
        gtk_tree_view_column_new_with_attributes("Due Date", due_cell,
            "text", TL_DUE, NULL);
    gtk_tree_view_column_set_cell_data_func(cdue, due_cell,
                                            due_color_func, lw, NULL);
    gtk_tree_view_column_set_resizable(cdue, TRUE);
    gtk_tree_sortable_set_sort_func(
        GTK_TREE_SORTABLE(lw->task_store), TL_DUE_RAW,
        sort_by_due, NULL, NULL);
    gtk_tree_view_column_set_sort_column_id(cdue, TL_DUE_RAW);
    gtk_tree_view_append_column(GTK_TREE_VIEW(lw->task_view), cdue);

    /* Make Done and Task columns sortable by header click.  Task sorts by
     * the raw title string (TL_TITLE), not the Pango markup (TL_DESC).    */
    gtk_tree_view_column_set_sort_column_id(cdone, TL_DONE);
    gtk_tree_view_column_set_sort_column_id(cdesc, TL_TITLE);

    /* Column hide/show via header right-click.  Done and Due Date are
     * hidable (Task always shows); bt-colkey/bt-collabel drive the menu.
     * Store column refs on the view for task_columns_apply and the
     * realize-time header-button connection.                               */
    g_object_set_data(G_OBJECT(lw->task_view), "bt-cdrag", cdrag);
    g_object_set_data(G_OBJECT(lw->task_view), "bt-cdone", cdone);
    g_object_set_data(G_OBJECT(lw->task_view), "bt-cdesc", cdesc);
    g_object_set_data(G_OBJECT(lw->task_view), "bt-cdue",  cdue);
    g_object_set_data(G_OBJECT(cdone), "bt-colkey",   (gpointer)"done");
    g_object_set_data(G_OBJECT(cdone), "bt-collabel", (gpointer)"Completed");
    g_object_set_data(G_OBJECT(cdue),  "bt-colkey",   (gpointer)"due");
    g_object_set_data(G_OBJECT(cdue),  "bt-collabel", (gpointer)"Due Date");
    GtkTreeViewColumn *header_cols[] = { cdrag, cdone, cdesc, cdue };
    for (gsize i = 0; i < G_N_ELEMENTS(header_cols); i++) {
        GtkWidget *hbtn = gtk_tree_view_column_get_button(header_cols[i]);
        if (hbtn) {
            g_signal_connect(hbtn, "button-press-event",
                             G_CALLBACK(on_column_header_press), lw);
            header_button_flatten(hbtn);   /* match the status bar          */
        }
    }
    task_columns_apply(lw);
    task_manual_sort_apply(lw);   /* show/hide cdrag per persisted setting  */

    /* Motion, release, and leave events for live-drag reorder + cursor. */
    gtk_widget_add_events(lw->task_view,
                          GDK_POINTER_MOTION_MASK | GDK_LEAVE_NOTIFY_MASK);
    g_signal_connect(lw->task_view, "motion-notify-event",
                     G_CALLBACK(on_task_drag_motion), lw);
    g_signal_connect(lw->task_view, "button-release-event",
                     G_CALLBACK(on_task_drag_release), lw);
    g_signal_connect(lw->task_view, "leave-notify-event",
                     G_CALLBACK(on_task_leave_notify), lw);

    lw->task_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lw->task_scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(lw->task_scroll), lw->task_view);

    /* The Weekly Forecast panel: seven full-width day sections stacked
     * vertically, 6 px apart so the lists never touch, scrolling
     * together in one outer scroller.  It shares the pane with the
     * regular task list; refresh_tasks swaps their visibility.               */
    GtkWidget *week_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(week_box), 6);
    for (gint d = 0; d < 7; d++)
        gtk_box_pack_start(GTK_BOX(week_box),
                           forecast_day_section(lw, d), FALSE, FALSE, 0);
    lw->forecast_box = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(lw->forecast_box),
                                   GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(lw->forecast_box), week_box);

    GtkWidget *task_pane = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(task_pane), lw->task_scroll,
                       TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(task_pane), lw->forecast_box,
                       TRUE, TRUE, 0);
    gtk_paned_pack2(GTK_PANED(paned), task_pane, TRUE, FALSE);

    /* --- Status bar -------------------------------------------------------- */
    /* Same geometry as the Records status bar: 8 px side margins,
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
    /* Both labels 85% of the UI font (Records size).  CSS font-size: 85%
     * can resolve to zero on Linux when the per-widget provider has no
     * explicit base size in scope; Pango scale attributes are always
     * relative to the actual rendered font and work on every platform.      */
    PangoAttrList *small_attrs = pango_attr_list_new();
    pango_attr_list_insert(small_attrs, pango_attr_scale_new(0.85));
    gtk_label_set_attributes(GTK_LABEL(lw->status_left),  small_attrs);
    gtk_label_set_attributes(GTK_LABEL(lw->status_right), small_attrs);
    pango_attr_list_unref(small_attrs);
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
    /* show_all made the whole chrome visible — apply the persisted
     * Compact Layout state, which also settles the lists pane (HIDDEN by
     * default; the Sidebar button and View → Show Sidebar bring it back)
     * and hides the floating button pair outside compact mode.              */
    compact_layout_apply(lw);
    /* show_all made BOTH task-pane variants visible — restore the
     * regular-list / Weekly Forecast split for the current selection.       */
    gtk_widget_set_visible(lw->task_scroll,
                           lw->sel_kind != SB_KIND_FORECAST);
    gtk_widget_set_visible(lw->forecast_box,
                           lw->sel_kind == SB_KIND_FORECAST);
    /* Hide Sync button when the master switch is off or user opted out.     */
    if (!bt_app_config_get_bool("google_sync_enabled", TRUE) ||
        !bt_app_config_get_bool("sync_toolbar_button", TRUE))
        gtk_widget_hide(lw->sync_item);
    return lw->window;
}
