/* ===========================================================================
 * settings_window.c — the Lists settings window (see header)
 * =========================================================================== */

#include "settings_window.h"
#include "db.h"
#include "oauth.h"
#include "gtasks.h"
#include "bnsync.h"
#include "library_window.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * BtSettings — the singleton window's state.
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp     *app;
    gchar     *db_path;
    GtkWidget *window;
    GtkWidget *sync_check;           /* Google Tasks master switch          */
    GtkWidget *sync_toolbar_check;   /* show Sync button in toolbar         */
    GtkWidget *interval_spin;
    GtkWidget *signin_btn;
    GtkWidget *signout_btn;
    GtkWidget *state_label;          /* "Signed in" / "Not signed in"       */
    GtkWidget *bn_check;             /* Records mirror master switch        */
    GtkWidget *bn_cli_entry;         /* Records command path                */
    GtkWidget *bn_embed_combo;       /* list mirrored items are filed into  */
    GArray    *bn_embed_ids;         /* combo index → list id (0 = managed) */
    GtkWidget *bn_interval_spin;     /* mirror pass interval, minutes       */
    GtkWidget *bn_meta_check;        /* sidebar Action Items view toggle    */
    gboolean   loading;              /* suppress write-through on load      */
} BtSettings;

static BtSettings *settings = NULL;  /* the singleton, or NULL              */

/* ---------------------------------------------------------------------------
 * state_refresh() — reflect the master switch + sign-in state: with the
 * switch off, Sign In / Sign Out / the auto-sync interval all grey out.
 * ------------------------------------------------------------------------- */
static void
state_refresh(BtSettings *sw)
{
    gboolean enabled = bt_app_config_get_bool("google_sync_enabled",
                                              TRUE);
    gboolean in = bt_oauth_authenticated();
    gtk_label_set_markup(GTK_LABEL(sw->state_label),
        !enabled ? "<span foreground=\"#888888\">Sync disabled</span>"
        : in     ? "<span foreground=\"#26a269\">Signed in</span>"
                 : "<span foreground=\"#888888\">Not signed in</span>");
    gtk_widget_set_sensitive(sw->signout_btn, enabled && in);
    gtk_widget_set_sensitive(sw->signin_btn,
                             enabled && bt_oauth_have_client() && !in);
    gtk_widget_set_sensitive(sw->interval_spin, enabled);
    gtk_widget_set_sensitive(sw->sync_toolbar_check, enabled);
}

/* ---------------------------------------------------------------------------
 * on_sync_enabled_toggled() — the Google Tasks master switch: persist,
 * re-grey the section, and arm/disarm the auto-sync timer.
 * ------------------------------------------------------------------------- */
static void
on_sync_enabled_toggled(GtkWidget *w, gpointer data)
{
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    bt_app_config_set("google_sync_enabled", on ? "1" : "0");
    state_refresh(sw);
    bt_sync_auto_start(sw->app, sw->app->db->path);
    /* Full notify: the library hides/shows its Sync button with this.       */
    bt_app_notify_changed(sw->app);
    bt_app_status(sw->app, on ? "Google Tasks sync enabled"
                              : "Google Tasks sync disabled");
}

/* on_interval_changed() — write-through + restart the auto-sync timer.      */
static void
on_interval_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gint minutes = gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(sw->interval_spin));
    gchar *v = g_strdup_printf("%d", minutes);
    bt_app_config_set("sync_interval_min", v);
    g_free(v);
    bt_sync_auto_start(sw->app, sw->app->db->path);
}

/* on_sync_toolbar_toggled() — persist the toolbar-button visibility pref
 * and tell the library window to show or hide its Sync button live.         */
static void
on_sync_toolbar_toggled(GtkWidget *w, gpointer data)
{
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    bt_app_config_set("sync_toolbar_button", on ? "1" : "0");
    bt_app_notify_changed(sw->app);
}

/* signin_done() — completion of the browser flow started here.              */
static void
signin_done(gboolean ok, const gchar *error, gpointer data)
{
    BtSettings *sw = data;
    if (settings != sw)              /* window closed mid-flow              */
        return;
    state_refresh(sw);
    if (ok)
        bt_app_status(sw->app, "Signed in to Google");
    bt_sync_signin_done(sw->app, GTK_WINDOW(sw->window), sw->db_path,
                        ok, error, NULL);
}

/* on_signin() — the Sign In button.                                         */
static void
on_signin(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    bt_app_status(sw->app, "Opening browser for Google sign-in\xe2\x80\xa6");
    bt_oauth_begin(GTK_WINDOW(sw->window), signin_done, sw);
}

/* on_signout() — the Sign Out button.                                       */
static void
on_signout(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    bt_oauth_signout();
    state_refresh(sw);
    bt_app_status(sw->app, "Signed out \xe2\x80\x94 the stored sign-in "
                  "was removed and syncing stopped");
}

/* on_bn_toggled() — the Records enable checkbox: persist, then re-arm
 * the mirror timer (switching ON runs a pass immediately, which is what
 * populates the mirror; switching OFF stops the timer).                     */
static void
on_bn_toggled(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(
        GTK_TOGGLE_BUTTON(sw->bn_check));
    bt_app_config_set("blue_notes_sync", on ? "1" : "0");
    bt_bnsync_auto_start(sw->app, sw->db_path);
    bt_app_notify_changed(sw->app);
}

/* on_bn_interval_changed() — write-through + re-arm the mirror timer.       */
static void
on_bn_interval_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gchar *v = g_strdup_printf("%d", gtk_spin_button_get_value_as_int(
        GTK_SPIN_BUTTON(sw->bn_interval_spin)));
    bt_app_config_set("records_sync_interval_min", v);
    g_free(v);
    bt_bnsync_auto_start(sw->app, sw->db_path);
}

/* on_bn_meta_toggled() — show/hide the sidebar's Action Items view.         */
static void
on_bn_meta_toggled(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (sw->loading)
        return;
    bt_app_config_set("records_meta_row",
        gtk_toggle_button_get_active(
            GTK_TOGGLE_BUTTON(sw->bn_meta_check)) ? "1" : "0");
    bt_app_notify_changed(sw->app);
}

/* on_bn_embed_changed() — where Records action items appear: their
 * own sidebar list (index 0) or embedded inside a chosen real list.
 * Persist the list id and refresh (the sidebar row and the target
 * list's view both change).                                                 */
static void
on_bn_embed_changed(GtkComboBox *combo, gpointer data)
{
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gint active = gtk_combo_box_get_active(combo);
    if (active < 0 || active >= (gint)sw->bn_embed_ids->len)
        return;
    gint64 id = g_array_index(sw->bn_embed_ids, gint64, active);
    if (id == 0) {
        bt_app_config_set("blue_notes_embed_list", NULL);
    } else {
        gchar *v = g_strdup_printf("%" G_GINT64_FORMAT, id);
        bt_app_config_set("blue_notes_embed_list", v);
        g_free(v);
    }
    bt_app_notify_changed(sw->app);
}

/* on_bn_cli_changed() — the CLI path entry: persist ONLY.  The mirror
 * pass happens on commit (focus-out/Enter) — running it per keystroke
 * would spawn the half-typed command over and over.                        */
static void
on_bn_cli_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (sw->loading)
        return;
    const gchar *cli = gtk_entry_get_text(GTK_ENTRY(sw->bn_cli_entry));
    bt_app_config_set("blue_notes_cli", *cli != '\0' ? cli : NULL);
}

/* on_bn_cli_commit() — Enter in the CLI path entry: run a mirror pass
 * against the newly named binary so a wrong path reports itself now
 * rather than at the next tick.                                             */
static void
on_bn_cli_commit(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (sw->loading)
        return;
    if (bt_app_config_get_bool("blue_notes_sync", FALSE))
        bt_bnsync_start(sw->app, sw->db_path, NULL, NULL);
    bt_app_notify_changed(sw->app);
}

/* on_bn_cli_focus_out() — leaving the CLI path entry: commit now.           */
static gboolean
on_bn_cli_focus_out(GtkWidget *w, GdkEventFocus *event, gpointer data)
{
    (void)event;
    on_bn_cli_commit(w, data);
    return FALSE;                    /* propagate                           */
}

/* on_forecast_toggled() — Appearance: show or hide the sidebar's Weekly
 * Forecast view.  The full notify rebuilds the sidebar; a hidden view
 * that was selected falls back to the first list there.                     */
static void
on_forecast_toggled(GtkWidget *w, gpointer data)
{
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    bt_app_config_set("weekly_forecast", on ? "1" : "0");
    bt_app_notify_changed(sw->app);
}

/* on_bold_titles_toggled() — Appearance: bold task titles on/off,
 * applied live (the task pane re-renders its markup).                       */
static void
on_due_today_overdue_toggled(GtkWidget *w, gpointer data)
{
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    bt_app_config_set("due_today_show_overdue", on ? "1" : "0");
    bt_app_notify_changed(sw->app);
}

static void
on_bold_titles_toggled(GtkWidget *w, gpointer data)
{
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gboolean bold = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    bt_app_config_set("bold_task_titles", bold ? "1" : "0");
    bt_app_notify_changed(sw->app);
}

/* on_toolbar_style_changed() — the Appearance combo: apply the chosen
 * toolbar style live (icons / text below icons / text only).                */
static void
on_toolbar_style_changed(GtkComboBox *combo, gpointer data)
{
    BtSettings *sw = data;
    if (sw->loading)
        return;
    static const GtkToolbarStyle STYLES[] = {
        GTK_TOOLBAR_ICONS, GTK_TOOLBAR_BOTH, GTK_TOOLBAR_TEXT
    };
    gint active = gtk_combo_box_get_active(combo);
    if (active >= 0 && active < (gint)G_N_ELEMENTS(STYLES))
        bt_app_set_toolbar_style(sw->app, STYLES[active]);
}

#ifdef HAVE_GTKOSX
/* on_native_menubar_toggled() — move the library menu into (or out of)
 * the native macOS menu bar, live, and persist the choice.                  */
static void
on_native_menubar_toggled(GtkToggleButton *check, gpointer data)
{
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gboolean native = gtk_toggle_button_get_active(check);
    bt_app_config_set("native_menubar", native ? "1" : "0");
    bt_library_apply_native_menubar(sw->app, native);
}
#endif /* HAVE_GTKOSX */

/* on_integrity_check_toggled() — persist the db_integrity_check pref and
 * update the in-memory flag so the setting takes effect next launch.         */
static void
on_integrity_check_toggled(GtkWidget *w, gpointer data)
{
    BtSettings *sw = data;
    if (sw->loading)
        return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(w));
    bt_app_config_set("db_integrity_check", on ? "1" : "0");
    sw->app->db_integrity_check = on;
}

/* ---------------------------------------------------------------------------
 * DbSection — widgets of the Database settings block, kept alive so
 * handlers can update them after a location switch.
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp     *app;
    GtkWidget *check;                /* "custom folder" checkbox             */
    GtkWidget *choose_btn;           /* "Choose Folder…" (sensitive = custom)*/
    GtkWidget *path_label;           /* shows the active db file path        */
} DbSection;

/* db_section_refresh() — sync the widgets with the current app state.       */
static void
db_section_refresh(DbSection *s)
{
    gchar *markup = g_markup_printf_escaped(
        "<small>Current database: %s</small>", s->app->db->path);
    gtk_label_set_markup(GTK_LABEL(s->path_label), markup);
    g_free(markup);
    gtk_widget_set_sensitive(s->choose_btn, s->app->db_dir != NULL);
}

static void on_db_custom_toggled(GtkToggleButton *check, gpointer user_data);

/* db_switch_report() — run the switch and re-sync widgets with whatever
 * actually happened (a cancelled or failed switch leaves the old db active).*/
static void
db_switch_report(DbSection *s, const gchar *new_dir)
{
    bt_app_switch_database(s->app, new_dir);
    g_signal_handlers_block_by_func(s->check, on_db_custom_toggled, s);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(s->check),
                                 s->app->db_dir != NULL);
    g_signal_handlers_unblock_by_func(s->check, on_db_custom_toggled, s);
    db_section_refresh(s);
}

/* db_pick_folder() — run a folder-chooser dialog; returns new path or NULL. */
static gchar *
db_pick_folder(DbSection *s)
{
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Choose Database Folder",
        GTK_WINDOW(gtk_widget_get_toplevel(s->check)),
        GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Select", GTK_RESPONSE_ACCEPT,
        NULL);
    if (s->app->db_dir != NULL)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                            s->app->db_dir);
    gchar *dir = NULL;
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT)
        dir = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);
    return dir;
}

/* on_db_custom_toggled() — checkbox toggled: switch to custom or default.   */
static void
on_db_custom_toggled(GtkToggleButton *check, gpointer user_data)
{
    DbSection *s = user_data;
    gboolean want_custom = gtk_toggle_button_get_active(check);

    if (want_custom && s->app->db_dir == NULL) {
        gchar *dir = db_pick_folder(s);
        if (dir == NULL) {
            g_signal_handlers_block_by_func(check, on_db_custom_toggled, s);
            gtk_toggle_button_set_active(check, FALSE);
            g_signal_handlers_unblock_by_func(check, on_db_custom_toggled, s);
            return;
        }
        db_switch_report(s, dir);
        g_free(dir);
    } else if (!want_custom && s->app->db_dir != NULL) {
        db_switch_report(s, NULL);   /* back to the default location         */
    }
}

/* on_db_choose_clicked() — "Choose Folder…" button: re-pick the folder.    */
static void
on_db_choose_clicked(GtkButton *btn, gpointer user_data)
{
    (void)btn;
    DbSection *s = user_data;
    gchar *dir = db_pick_folder(s);
    if (dir != NULL) {
        db_switch_report(s, dir);
        g_free(dir);
    }
}

/* on_settings_destroy() — clear the singleton.                              */
static void
on_settings_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (settings == sw)
        settings = NULL;
    if (sw->bn_embed_ids != NULL)
        g_array_free(sw->bn_embed_ids, TRUE);
    g_free(sw->db_path);
    g_free(sw);
}

/* section_label() — a bold section heading, left-aligned.                   */
static GtkWidget *
section_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(NULL);
    gchar *markup = g_markup_printf_escaped("<b>%s</b>", text);
    gtk_label_set_markup(GTK_LABEL(label), markup);
    g_free(markup);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

/* wrapped_label() — a wrapping, left-aligned explanatory label.             */
static GtkWidget *
wrapped_label(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_halign(label, GTK_ALIGN_START);
    return label;
}

/* ---------------------------------------------------------------------------
 * bt_settings_window_open() — show (or raise) the window (see header).
 * ------------------------------------------------------------------------- */
void
bt_settings_window_open(BtApp *app, GtkWindow *parent,
                        const gchar *db_path)
{
    if (settings != NULL) {
        gtk_window_present(GTK_WINDOW(settings->window));
        return;
    }
    BtSettings *sw = g_new0(BtSettings, 1);
    settings = sw;
    sw->app = app;
    sw->db_path = g_strdup(db_path);
    sw->loading = TRUE;

    sw->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(sw->window), "Lists - Settings");
    gtk_window_set_transient_for(GTK_WINDOW(sw->window), parent);
    gtk_window_set_default_size(GTK_WINDOW(sw->window), 470, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_container_add(GTK_CONTAINER(sw->window), vbox);

    /* --- Appearance --------------------------------------------------------- */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Appearance"),
                       FALSE, FALSE, 0);

    GtkWidget *bold_check = gtk_check_button_new_with_label(
        "Show task titles in bold");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(bold_check),
        bt_app_config_get_bool("bold_task_titles", FALSE));
    g_signal_connect(bold_check, "toggled",
                     G_CALLBACK(on_bold_titles_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), bold_check, FALSE, FALSE, 0);

    GtkWidget *forecast_check = gtk_check_button_new_with_label(
        "Show the Weekly Forecast view (this week, day by day)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(forecast_check),
        bt_app_config_get_bool("weekly_forecast", TRUE));
    g_signal_connect(forecast_check, "toggled",
                     G_CALLBACK(on_forecast_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), forecast_check, FALSE, FALSE, 0);

    GtkWidget *overdue_check = gtk_check_button_new_with_label(
        "Include all past-due tasks in the Due Today view");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(overdue_check),
        bt_app_config_get_bool("due_today_show_overdue", FALSE));
    g_signal_connect(overdue_check, "toggled",
                     G_CALLBACK(on_due_today_overdue_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), overdue_check, FALSE, FALSE, 0);

    /* Toolbar style: icons / text below icons / text only.  Applies live
     * to every registered toolbar; also reachable by right-clicking any
     * toolbar (like Records).                                               */
    GtkWidget *style_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(style_row),
                       gtk_label_new("Toolbar style:"), FALSE, FALSE, 0);
    GtkWidget *style_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(style_combo),
                                   "Icons");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(style_combo),
                                   "Text below icons");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(style_combo),
                                   "Text only");
    gtk_combo_box_set_active(GTK_COMBO_BOX(style_combo),
        app->toolbar_style == GTK_TOOLBAR_BOTH ? 1
        : app->toolbar_style == GTK_TOOLBAR_TEXT ? 2 : 0);
    g_signal_connect(style_combo, "changed",
                     G_CALLBACK(on_toolbar_style_changed), sw);
    gtk_box_pack_start(GTK_BOX(style_row), style_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), style_row, FALSE, FALSE, 0);

#ifdef __APPLE__
    GtkWidget *mac_check = gtk_check_button_new_with_label(
        "Use the native macOS menu bar (hide the in-window menu)");
#ifdef HAVE_GTKOSX
    gtk_toggle_button_set_active(
        GTK_TOGGLE_BUTTON(mac_check),
        bt_app_config_get_bool("native_menubar", FALSE));
    g_signal_connect(mac_check, "toggled",
                     G_CALLBACK(on_native_menubar_toggled), sw);
#else
    gtk_widget_set_sensitive(mac_check, FALSE);
    gtk_widget_set_tooltip_text(mac_check,
        "Requires the gtk-mac-integration library:\n"
        "sudo port install gtk-osx-application-gtk3, then rebuild "
        "(make clean && make)");
#endif
    gtk_box_pack_start(GTK_BOX(vbox), mac_check, FALSE, FALSE, 0);
#endif /* __APPLE__ */
    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* --- Database ---------------------------------------------------------- */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Database"),
                       FALSE, FALSE, 0);

    DbSection *dbs = g_new0(DbSection, 1);
    dbs->app = app;
    g_object_set_data_full(G_OBJECT(sw->window), "bt-db-section",
                           dbs, g_free);

    dbs->check = gtk_check_button_new_with_label(
        "Store the database in a custom folder (e.g. a shared drive)");
    gtk_widget_set_margin_start(dbs->check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dbs->check),
                                 app->db_dir != NULL);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->check, FALSE, FALSE, 0);

    GtkWidget *db_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(db_row, 12);
    dbs->choose_btn = gtk_button_new_with_label(
        "Choose Folder\xe2\x80\xa6");
    gtk_box_pack_start(GTK_BOX(db_row), dbs->choose_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), db_row, FALSE, FALSE, 0);

    dbs->path_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(dbs->path_label), 0.0);
    gtk_label_set_line_wrap(GTK_LABEL(dbs->path_label), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(dbs->path_label), 40);
    gtk_widget_set_margin_start(dbs->path_label, 12);
    gtk_box_pack_start(GTK_BOX(vbox), dbs->path_label, FALSE, FALSE, 0);

    db_section_refresh(dbs);
    g_signal_connect(dbs->check, "toggled",
                     G_CALLBACK(on_db_custom_toggled), dbs);
    g_signal_connect(dbs->choose_btn, "clicked",
                     G_CALLBACK(on_db_choose_clicked), dbs);

    GtkWidget *integrity_check = gtk_check_button_new_with_label(
        "Check database integrity on startup (PRAGMA integrity_check)");
    gtk_widget_set_margin_start(integrity_check, 12);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(integrity_check),
        bt_app_config_get_bool("db_integrity_check", TRUE));
    g_signal_connect(integrity_check, "toggled",
                     G_CALLBACK(on_integrity_check_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), integrity_check, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* --- Records ------------------------------------------------------------ */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Records"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), wrapped_label(
        "Mirror the action items from Records as ordinary tasks, with "
        "their own notes, subtasks and attachments. Ticking one off or "
        "changing its due date is sent back to Records on the interval "
        "below; the item's text is owned by the note in Records."),
        FALSE, FALSE, 0);
    sw->bn_check = gtk_check_button_new_with_label(
        "Mirror Records action items");
    g_signal_connect(sw->bn_check, "toggled",
                     G_CALLBACK(on_bn_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), sw->bn_check, FALSE, FALSE, 0);

    GtkWidget *bn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(bn_row),
                       gtk_label_new("Records command:"),
                       FALSE, FALSE, 0);
    sw->bn_cli_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sw->bn_cli_entry),
                                   "records (searched on PATH)");
    gtk_widget_set_hexpand(sw->bn_cli_entry, TRUE);
    g_signal_connect(sw->bn_cli_entry, "changed",
                     G_CALLBACK(on_bn_cli_changed), sw);
    g_signal_connect(sw->bn_cli_entry, "activate",
                     G_CALLBACK(on_bn_cli_commit), sw);
    g_signal_connect(sw->bn_cli_entry, "focus-out-event",
                     G_CALLBACK(on_bn_cli_focus_out), sw);
    gtk_box_pack_start(GTK_BOX(bn_row), sw->bn_cli_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bn_row, FALSE, FALSE, 0);

    /* Which real list the mirrored tasks are filed into.  0 = let the
     * mirror manage its own "Action Items" list, created on first use.     */
    GtkWidget *embed_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(embed_row),
                       gtk_label_new("Mirror action items into:"),
                       FALSE, FALSE, 0);
    sw->bn_embed_combo = gtk_combo_box_text_new();
    sw->bn_embed_ids = g_array_new(FALSE, FALSE, sizeof(gint64));
    gint64 own = 0;
    g_array_append_val(sw->bn_embed_ids, own);
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(sw->bn_embed_combo),
                                   "Action Items (managed list)");
    gchar *embed_v = bt_app_config_get("blue_notes_embed_list");
    gint64 embed_id = embed_v != NULL
                      ? g_ascii_strtoll(embed_v, NULL, 10) : 0;
    g_free(embed_v);
    gint embed_active = 0;           /* combo index to preselect            */
    GPtrArray *lists = bt_db_lists(app->db, FALSE);
    for (guint i = 0; i < lists->len; i++) {
        BtList *l = g_ptr_array_index(lists, i);
        gchar *label = *l->emoji != '\0'
            ? g_strdup_printf("%s  %s", l->emoji, l->name)
            : g_strdup(l->name);
        gtk_combo_box_text_append_text(
            GTK_COMBO_BOX_TEXT(sw->bn_embed_combo), label);
        g_free(label);
        g_array_append_val(sw->bn_embed_ids, l->id);
        if (l->id == embed_id)
            embed_active = (gint)sw->bn_embed_ids->len - 1;
    }
    bt_ptr_array_free_lists(lists);
    gtk_combo_box_set_active(GTK_COMBO_BOX(sw->bn_embed_combo),
                             embed_active);
    gtk_widget_set_tooltip_text(sw->bn_embed_combo,
        "New action items are filed here; moving one to another list "
        "afterwards is fine and sticks");
    g_signal_connect(sw->bn_embed_combo, "changed",
                     G_CALLBACK(on_bn_embed_changed), sw);
    gtk_box_pack_start(GTK_BOX(embed_row), sw->bn_embed_combo,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), embed_row, FALSE, FALSE, 0);

    /* How often the mirror runs — the same shape as the Google interval
     * (0 = only when Sync is pressed).                                     */
    GtkWidget *bn_iv_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(bn_iv_row),
                       gtk_label_new("Sync action items every"),
                       FALSE, FALSE, 0);
    sw->bn_interval_spin = gtk_spin_button_new_with_range(0, 720, 1);
    gtk_widget_set_tooltip_text(sw->bn_interval_spin,
        "0 = only when you press Sync");
    g_signal_connect(sw->bn_interval_spin, "value-changed",
                     G_CALLBACK(on_bn_interval_changed), sw);
    gtk_box_pack_start(GTK_BOX(bn_iv_row), sw->bn_interval_spin,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(bn_iv_row), gtk_label_new("minutes"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bn_iv_row, FALSE, FALSE, 0);

    sw->bn_meta_check = gtk_check_button_new_with_label(
        "Show the Action Items view in the sidebar");
    gtk_widget_set_tooltip_text(sw->bn_meta_check,
        "Lists every mirrored action item in one place, whichever list "
        "each one lives in");
    g_signal_connect(sw->bn_meta_check, "toggled",
                     G_CALLBACK(on_bn_meta_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), sw->bn_meta_check, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* --- Google Tasks ------------------------------------------------------ */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Google Tasks"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), wrapped_label(
        "Two-way non-destructive sync with Google Tasks.  Sign in will "
        "open a browser window for authentication; Sign out will remove "
        "the local stored token."), FALSE, FALSE, 0);

    sw->sync_check = gtk_check_button_new_with_label(
        "Enable Google Tasks sync");
    g_signal_connect(sw->sync_check, "toggled",
                     G_CALLBACK(on_sync_enabled_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), sw->sync_check, FALSE, FALSE, 0);

    GtkWidget *btn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    sw->signin_btn = gtk_button_new_with_label(
        "Sign In to Google\xe2\x80\xa6");
    g_signal_connect(sw->signin_btn, "clicked",
                     G_CALLBACK(on_signin), sw);
    gtk_box_pack_start(GTK_BOX(btn_row), sw->signin_btn, FALSE, FALSE, 0);
    sw->signout_btn = gtk_button_new_with_label("Sign Out");
    g_signal_connect(sw->signout_btn, "clicked",
                     G_CALLBACK(on_signout), sw);
    gtk_box_pack_start(GTK_BOX(btn_row), sw->signout_btn,
                       FALSE, FALSE, 0);
    sw->state_label = gtk_label_new("");
    gtk_box_pack_end(GTK_BOX(btn_row), sw->state_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), btn_row, FALSE, FALSE, 0);

    GtkWidget *interval_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(interval_row),
                       gtk_label_new("Auto-sync every"), FALSE, FALSE, 0);
    sw->interval_spin = gtk_spin_button_new_with_range(0, 720, 1);
    gtk_widget_set_tooltip_text(sw->interval_spin,
        "Minutes between automatic syncs while signed in; 0 disables "
        "the timer (the Sync button always works)");
    g_signal_connect(sw->interval_spin, "value-changed",
                     G_CALLBACK(on_interval_changed), sw);
    gtk_box_pack_start(GTK_BOX(interval_row), sw->interval_spin,
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(interval_row),
                       gtk_label_new("minutes (0 = off)"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), interval_row, FALSE, FALSE, 0);

    sw->sync_toolbar_check = gtk_check_button_new_with_label(
        "Show Sync button in toolbar");
    g_signal_connect(sw->sync_toolbar_check, "toggled",
                     G_CALLBACK(on_sync_toolbar_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), sw->sync_toolbar_check,
                       FALSE, FALSE, 0);

    /* --- Load current values ------------------------------------------------ */
    gchar *iv  = bt_app_config_get("sync_interval_min");
    gchar *bnc = bt_app_config_get("blue_notes_cli");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw->sync_check),
        bt_app_config_get_bool("google_sync_enabled", TRUE));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw->sync_toolbar_check),
        bt_app_config_get_bool("sync_toolbar_button", TRUE));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sw->interval_spin),
                              iv != NULL ? g_ascii_strtod(iv, NULL) : 5);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw->bn_check),
        bt_app_config_get_bool("blue_notes_sync", FALSE));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw->bn_meta_check),
        bt_app_config_get_bool("records_meta_row", TRUE));
    gchar *bniv = bt_app_config_get("records_sync_interval_min");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sw->bn_interval_spin),
                              bniv != NULL ? g_ascii_strtod(bniv, NULL) : 5);
    g_free(bniv);
    if (bnc != NULL)
        gtk_entry_set_text(GTK_ENTRY(sw->bn_cli_entry), bnc);
    g_free(iv);
    g_free(bnc);
    sw->loading = FALSE;

    state_refresh(sw);
    g_signal_connect(sw->window, "destroy",
                     G_CALLBACK(on_settings_destroy), sw);
    gtk_widget_show_all(sw->window);
}
