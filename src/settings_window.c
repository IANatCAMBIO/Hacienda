/* ===========================================================================
 * settings_window.c — the Blue Tasks settings window (see header)
 * =========================================================================== */

#include "settings_window.h"
#include "oauth.h"
#include "gtasks.h"
#include "library_window.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * BtSettings — the singleton window's state.
 * ------------------------------------------------------------------------- */
typedef struct {
    BtApp     *app;
    gchar     *db_path;
    GtkWidget *window;
    GtkWidget *id_entry;
    GtkWidget *secret_entry;
    GtkWidget *interval_spin;
    GtkWidget *signin_btn;
    GtkWidget *signout_btn;
    GtkWidget *state_label;          /* "Signed in" / "Not signed in"       */
    GtkWidget *bn_check;             /* Blue Notes action items toggle      */
    GtkWidget *bn_cli_entry;         /* blue_notes command path             */
    gboolean   loading;              /* suppress write-through on load      */
} BtSettings;

static BtSettings *settings = NULL;  /* the singleton, or NULL              */

/* ---------------------------------------------------------------------------
 * state_refresh() — reflect the current sign-in state in the label and
 * button sensitivity.
 * ------------------------------------------------------------------------- */
static void
state_refresh(BtSettings *sw)
{
    gboolean in = bt_oauth_authenticated();
    gtk_label_set_markup(GTK_LABEL(sw->state_label),
        in ? "<span foreground=\"#26a269\">Signed in</span>"
           : "<span foreground=\"#888888\">Not signed in</span>");
    gtk_widget_set_sensitive(sw->signout_btn, in);
    gtk_widget_set_sensitive(sw->signin_btn,
                             bt_oauth_have_client() && !in);
}

/* ---------------------------------------------------------------------------
 * on_cred_changed() — write-through for the two credential entries; the
 * OAuth module re-snapshots so the next sign-in uses the new client.
 * ------------------------------------------------------------------------- */
static void
on_cred_changed(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (sw->loading)
        return;
    const gchar *id  = gtk_entry_get_text(GTK_ENTRY(sw->id_entry));
    const gchar *sec = gtk_entry_get_text(GTK_ENTRY(sw->secret_entry));
    bt_app_config_set("google_client_id", *id != '\0' ? id : NULL);
    bt_app_config_set("google_client_secret", *sec != '\0' ? sec : NULL);
    bt_oauth_init();
    state_refresh(sw);
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
    bt_sync_auto_start(sw->app, sw->db_path);
}

/* signin_done() — completion of the browser flow started here.              */
static void
signin_done(gboolean ok, const gchar *error, gpointer data)
{
    BtSettings *sw = data;
    if (settings != sw)              /* window closed mid-flow              */
        return;
    state_refresh(sw);
    if (ok) {
        bt_app_status(sw->app, "Signed in to Google");
        bt_sync_start(sw->app, sw->db_path, NULL, NULL);
    } else {
        bt_app_notice(GTK_WINDOW(sw->window), GTK_MESSAGE_ERROR,
                      "Blue Tasks - Google Sign-In",
                      "Could not sign in: %s",
                      error != NULL ? error : "unknown error");
    }
}

/* on_signin() — the Sign In button.                                         */
static void
on_signin(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    bt_app_status(sw->app, "Opening browser for Google sign-in\xe2\x80\xa6");
    bt_oauth_begin(sw->app, GTK_WINDOW(sw->window), signin_done, sw);
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

/* on_bn_toggled() — the Blue Notes enable checkbox: persist + refresh
 * (the Action Items row appears/disappears immediately).                    */
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
    if (sw->app->notify_changed != NULL)
        sw->app->notify_changed(sw->app);
}

/* on_bn_cli_changed() — the CLI path entry: persist ONLY.  The library
 * refresh happens on commit (focus-out/Enter) — refreshing per
 * keystroke would synchronously spawn the half-typed command with the
 * Blue Notes view visible.                                                  */
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

/* on_bn_cli_commit() — Enter in the CLI path entry: refresh now.            */
static void
on_bn_cli_commit(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (!sw->loading && sw->app->notify_changed != NULL)
        sw->app->notify_changed(sw->app);
}

/* on_bn_cli_focus_out() — leaving the CLI path entry: refresh now.          */
static gboolean
on_bn_cli_focus_out(GtkWidget *w, GdkEventFocus *event, gpointer data)
{
    (void)event;
    on_bn_cli_commit(w, data);
    return FALSE;                    /* propagate                           */
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

/* on_settings_destroy() — clear the singleton.                              */
static void
on_settings_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    BtSettings *sw = data;
    if (settings == sw)
        settings = NULL;
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

/* grid_row() — attach "label: widget" as row `row` of `grid`.               */
static void
grid_row(GtkWidget *grid, gint row, const gchar *text, GtkWidget *widget)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_widget_set_halign(label, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), label, 0, row, 1, 1);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_grid_attach(GTK_GRID(grid), widget, 1, row, 1, 1);
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
    gtk_window_set_title(GTK_WINDOW(sw->window), "Blue Tasks - Settings");
    gtk_window_set_transient_for(GTK_WINDOW(sw->window), parent);
    gtk_window_set_default_size(GTK_WINDOW(sw->window), 470, -1);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_container_add(GTK_CONTAINER(sw->window), vbox);

    /* --- Google Tasks Sync ------------------------------------------------ */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Google Tasks Sync"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), wrapped_label(
        "Blue Tasks syncs with Google Tasks using an OAuth client: in "
        "the Google Cloud console, enable the Google Tasks API, create "
        "a \xe2\x80\x9c""Desktop app\xe2\x80\x9d OAuth client, and enter "
        "its credentials below.  Sign in once in your browser and Blue "
        "Tasks stays signed in (a sync-only token is kept in the ini; "
        "Sign Out removes it, and it can be revoked any time from your "
        "Google account's security page)."), FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

    sw->id_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sw->id_entry),
        "\xe2\x80\xa6.apps.googleusercontent.com");
    g_signal_connect(sw->id_entry, "changed",
                     G_CALLBACK(on_cred_changed), sw);
    grid_row(grid, 0, "Client ID:", sw->id_entry);

    sw->secret_entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(sw->secret_entry), FALSE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(sw->secret_entry),
                                   "GOCSPX-\xe2\x80\xa6");
    g_signal_connect(sw->secret_entry, "changed",
                     G_CALLBACK(on_cred_changed), sw);
    grid_row(grid, 1, "Client secret:", sw->secret_entry);
    gtk_box_pack_start(GTK_BOX(vbox), grid, FALSE, FALSE, 0);

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

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* --- Blue Notes --------------------------------------------------------- */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Blue Notes"),
                       FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), wrapped_label(
        "Show the companion Blue Notes app's action items ('!' lines) "
        "as an \xe2\x80\x9c""Action Items (from Blue Notes)\xe2\x80\x9d "
        "list under Lists.  Reads and writes go through the blue_notes "
        "CLI \xe2\x80\x94 safe alongside a running Blue Notes \xe2\x80\x94 "
        "so checking an item off or changing its due date here rewrites "
        "the line in the note itself."), FALSE, FALSE, 0);
    sw->bn_check = gtk_check_button_new_with_label(
        "Show Blue Notes action items");
    g_signal_connect(sw->bn_check, "toggled",
                     G_CALLBACK(on_bn_toggled), sw);
    gtk_box_pack_start(GTK_BOX(vbox), sw->bn_check, FALSE, FALSE, 0);

    GtkWidget *bn_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(bn_row),
                       gtk_label_new("blue_notes command:"),
                       FALSE, FALSE, 0);
    sw->bn_cli_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(sw->bn_cli_entry),
                                   "blue_notes (searched on PATH)");
    gtk_widget_set_hexpand(sw->bn_cli_entry, TRUE);
    g_signal_connect(sw->bn_cli_entry, "changed",
                     G_CALLBACK(on_bn_cli_changed), sw);
    g_signal_connect(sw->bn_cli_entry, "activate",
                     G_CALLBACK(on_bn_cli_commit), sw);
    g_signal_connect(sw->bn_cli_entry, "focus-out-event",
                     G_CALLBACK(on_bn_cli_focus_out), sw);
    gtk_box_pack_start(GTK_BOX(bn_row), sw->bn_cli_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), bn_row, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox),
                       gtk_separator_new(GTK_ORIENTATION_HORIZONTAL),
                       FALSE, FALSE, 2);

    /* --- Appearance --------------------------------------------------------- */
    gtk_box_pack_start(GTK_BOX(vbox), section_label("Appearance"),
                       FALSE, FALSE, 0);

    /* Toolbar style: icons / text below icons / text only.  Applies live
     * to every registered toolbar; also reachable by right-clicking any
     * toolbar (like Blue Notes).                                            */
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
    GtkWidget *db_label = gtk_label_new(db_path);
    gtk_label_set_ellipsize(GTK_LABEL(db_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(db_label), TRUE);
    gtk_widget_set_halign(db_label, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), db_label, FALSE, FALSE, 0);

    /* --- Load current values ------------------------------------------------ */
    gchar *id  = bt_app_config_get("google_client_id");
    gchar *sec = bt_app_config_get("google_client_secret");
    gchar *iv  = bt_app_config_get("sync_interval_min");
    gchar *bnc = bt_app_config_get("blue_notes_cli");
    if (id != NULL)
        gtk_entry_set_text(GTK_ENTRY(sw->id_entry), id);
    if (sec != NULL)
        gtk_entry_set_text(GTK_ENTRY(sw->secret_entry), sec);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(sw->interval_spin),
                              iv != NULL ? g_ascii_strtod(iv, NULL) : 5);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sw->bn_check),
        bt_app_config_get_bool("blue_notes_sync", FALSE));
    if (bnc != NULL)
        gtk_entry_set_text(GTK_ENTRY(sw->bn_cli_entry), bnc);
    g_free(id);
    g_free(sec);
    g_free(iv);
    g_free(bnc);
    sw->loading = FALSE;

    state_refresh(sw);
    g_signal_connect(sw->window, "destroy",
                     G_CALLBACK(on_settings_destroy), sw);
    gtk_widget_show_all(sw->window);
}
