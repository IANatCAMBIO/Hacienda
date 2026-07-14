/* ===========================================================================
 * app.c — shared application context for Blue Tasks (see app.h)
 * =========================================================================== */

#include "app.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---------------------------------------------------------------------------
 * bt_app_status() — post an event message to the library status bar.
 * ------------------------------------------------------------------------- */
void
bt_app_status(BtApp *app, const gchar *fmt, ...)
{
    if (app == NULL || app->notify_status == NULL)
        return;
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    app->notify_status(app, msg);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * bt_app_notice() — modal OK message dialog.
 * ------------------------------------------------------------------------- */
void
bt_app_notice(GtkWindow *parent, GtkMessageType type,
              const gchar *title, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    GtkWidget *dlg = gtk_message_dialog_new(parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, type,
        GTK_BUTTONS_OK, "%s", msg);
    if (title != NULL)
        gtk_window_set_title(GTK_WINDOW(dlg), title);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(msg);
}

/* ---------------------------------------------------------------------------
 * bt_app_confirm() — modal Yes/No question; TRUE on Yes.
 * ------------------------------------------------------------------------- */
gboolean
bt_app_confirm(GtkWindow *parent, const gchar *title, const gchar *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    gchar *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    GtkWidget *dlg = gtk_message_dialog_new(parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        GTK_MESSAGE_QUESTION, GTK_BUTTONS_YES_NO, "%s", msg);
    if (title != NULL)
        gtk_window_set_title(GTK_WINDOW(dlg), title);
    gint resp = gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
    g_free(msg);
    return resp == GTK_RESPONSE_YES;
}

/* ---------------------------------------------------------------------------
 * bt_app_widget_add_css() — one-off CSS on a single widget (see app.h).
 * ------------------------------------------------------------------------- */
void
bt_app_widget_add_css(GtkWidget *widget, const gchar *css_text)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, css_text, -1, NULL);
    gtk_style_context_add_provider(
        gtk_widget_get_style_context(widget),
        GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ===========================================================================
 * Toolbar icons + style (see app.h).
 * =========================================================================== */

/* exe_dir_from_argv0() — the directory holding the binary (new string).     */
static gchar *
exe_dir_from_argv0(const gchar *argv0)
{
    if (argv0 != NULL && strchr(argv0, '/') != NULL) {
        gchar *abs = g_canonicalize_filename(argv0, NULL);
        gchar *dir = g_path_get_dirname(abs);
        g_free(abs);
        return dir;
    }
    return g_get_current_dir();
}

/* ---------------------------------------------------------------------------
 * bt_app_init_icons_dir() — icons/ next to the executable (see app.h).
 * ------------------------------------------------------------------------- */
void
bt_app_init_icons_dir(BtApp *app, const gchar *argv0)
{
    gchar *exe_dir = exe_dir_from_argv0(argv0);
    app->icons_dir = g_build_filename(exe_dir, "icons", NULL);
    g_free(exe_dir);
}

/* ---------------------------------------------------------------------------
 * bt_app_icon_image_sized() — HiDPI-sharp GtkImage for a local icon
 * (see app.h).  Rasterizes at the display's scale factor: `size` is the
 * LOGICAL size, the backing pixels are size × sf, and the cairo
 * surface's device scale maps between the two (raw pixbufs render
 * 1 buffer-pixel = 1 logical px and blur on Retina — Blue Notes
 * gotcha #5).
 * ------------------------------------------------------------------------- */
GtkWidget *
bt_app_icon_image_sized(BtApp *app, const gchar *name, gint size)
{
    static const gchar *EXTS[] = { "png", "svg" };

    gint sf = 1;                     /* display scale factor                */
    GdkDisplay *display = gdk_display_get_default();
    if (display != NULL) {
        GdkMonitor *monitor = gdk_display_get_primary_monitor(display);
        if (monitor == NULL)
            monitor = gdk_display_get_monitor(display, 0);
        if (monitor != NULL)
            sf = gdk_monitor_get_scale_factor(monitor);
    }

    for (gsize i = 0; i < G_N_ELEMENTS(EXTS); i++) {
        gchar *path = g_strdup_printf("%s%c%s.%s",
                                      app->icons_dir, G_DIR_SEPARATOR,
                                      name, EXTS[i]);
        GdkPixbuf *pix = NULL;       /* decoded at backing resolution       */
        if (g_file_test(path, G_FILE_TEST_EXISTS))
            pix = gdk_pixbuf_new_from_file_at_size(path, size * sf,
                                                   size * sf, NULL);
        g_free(path);
        if (pix != NULL) {
            cairo_surface_t *surface =
                gdk_cairo_surface_create_from_pixbuf(pix, sf, NULL);
            g_object_unref(pix);
            GtkWidget *image = gtk_image_new_from_surface(surface);
            cairo_surface_destroy(surface);
            return image;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------
 * bt_app_tool_item_new() — style-aware toolbar button (see app.h).
 * ------------------------------------------------------------------------- */
GtkToolItem *
bt_app_tool_item_new(BtApp *app, const gchar *icon_name,
                     const gchar *fallback_markup, const gchar *label,
                     const gchar *tooltip)
{
    GtkToolItem *item = gtk_tool_button_new(NULL, NULL);
    gtk_tool_button_set_label(GTK_TOOL_BUTTON(item), label);

    /* Icon: the local PNG if present, else the fallback markup rendered
     * as a label standing in for the icon.                                 */
    GtkWidget *icon = (icon_name != NULL)
                      ? bt_app_icon_image_sized(app, icon_name, 24) : NULL;
    if (icon == NULL) {
        icon = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(icon),
                             fallback_markup != NULL ? fallback_markup
                                                     : label);
    }
    gtk_widget_show(icon);
    gtk_tool_button_set_icon_widget(GTK_TOOL_BUTTON(item), icon);

    gtk_tool_item_set_tooltip_text(item, tooltip);
    gtk_tool_item_set_is_important(item, TRUE);
    return item;
}

/* style_name()/style_from_name() — the persisted spelling of a style.       */
static const gchar *
style_name(GtkToolbarStyle style)
{
    return style == GTK_TOOLBAR_TEXT ? "text"
         : style == GTK_TOOLBAR_BOTH ? "both" : "icons";
}

static GtkToolbarStyle
style_from_name(const gchar *name)
{
    if (g_strcmp0(name, "text") == 0) return GTK_TOOLBAR_TEXT;
    if (g_strcmp0(name, "both") == 0) return GTK_TOOLBAR_BOTH;
    return GTK_TOOLBAR_ICONS;
}

/* bt_app_load_toolbar_style() — the persisted style (default icons).        */
void
bt_app_load_toolbar_style(BtApp *app)
{
    gchar *v = bt_app_config_get("toolbar_style");
    app->toolbar_style = style_from_name(v);
    g_free(v);
}

/* ---------------------------------------------------------------------------
 * bt_app_set_toolbar_style() — apply + persist a style change (see app.h).
 * ------------------------------------------------------------------------- */
void
bt_app_set_toolbar_style(BtApp *app, GtkToolbarStyle style)
{
    app->toolbar_style = style;
    bt_app_config_set("toolbar_style", style_name(style));
    if (app->toolbars != NULL)
        for (guint i = 0; i < app->toolbars->len; i++)
            gtk_toolbar_set_style(
                GTK_TOOLBAR(g_ptr_array_index(app->toolbars, i)), style);
}

/* toolbar_destroyed() — drop a dying toolbar from the registry.             */
static void
toolbar_destroyed(GtkWidget *toolbar, gpointer data)
{
    BtApp *app = data;
    if (app->toolbars != NULL)
        g_ptr_array_remove(app->toolbars, toolbar);
}

/* style_menu_toggled() — a radio item in the right-click menu.              */
static void
style_menu_toggled(GtkCheckMenuItem *item, gpointer data)
{
    BtApp *app = data;
    if (!gtk_check_menu_item_get_active(item))
        return;                      /* ignore the deactivating item        */
    bt_app_set_toolbar_style(app, (GtkToolbarStyle)GPOINTER_TO_INT(
        g_object_get_data(G_OBJECT(item), "bt-style")));
}

/* ---------------------------------------------------------------------------
 * toolbar_context_menu() — "popup-context-menu": right-clicking a
 * toolbar offers the icons/both/text radio choices (fires on empty
 * toolbar area only, like Blue Notes).
 * ------------------------------------------------------------------------- */
static gboolean
toolbar_context_menu(GtkToolbar *toolbar, gint x, gint y, gint button,
                     gpointer data)
{
    (void)x; (void)y; (void)button;
    BtApp *app = data;
    static const struct {
        const gchar    *label;
        GtkToolbarStyle style;
    } CHOICES[] = {
        { "Icons",            GTK_TOOLBAR_ICONS },
        { "Text Below Icons", GTK_TOOLBAR_BOTH  },
        { "Text Only",        GTK_TOOLBAR_TEXT  },
    };
    GtkWidget *menu = gtk_menu_new();
    gtk_menu_attach_to_widget(GTK_MENU(menu), GTK_WIDGET(toolbar), NULL);
    /* One menu is built per right-click; without this it would stay
     * attached (= alive) until the toolbar dies.  selection-done fires
     * after the chosen item's activate, so destroying there is safe.        */
    g_signal_connect(menu, "selection-done",
                     G_CALLBACK(gtk_widget_destroy), NULL);
    GSList *group = NULL;            /* the radio group                     */
    for (gsize i = 0; i < G_N_ELEMENTS(CHOICES); i++) {
        GtkWidget *item =
            gtk_radio_menu_item_new_with_label(group, CHOICES[i].label);
        group = gtk_radio_menu_item_get_group(GTK_RADIO_MENU_ITEM(item));
        g_object_set_data(G_OBJECT(item), "bt-style",
                          GINT_TO_POINTER(CHOICES[i].style));
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item),
                                       app->toolbar_style ==
                                       CHOICES[i].style);
        g_signal_connect(item, "toggled",
                         G_CALLBACK(style_menu_toggled), app);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), NULL);
    return TRUE;
}

/* ---------------------------------------------------------------------------
 * bt_app_register_toolbar() — track + style a toolbar (see app.h).
 * ------------------------------------------------------------------------- */
void
bt_app_register_toolbar(BtApp *app, GtkWidget *toolbar)
{
    gtk_toolbar_set_style(GTK_TOOLBAR(toolbar), app->toolbar_style);
    if (app->toolbars != NULL)
        g_ptr_array_add(app->toolbars, toolbar);
    g_signal_connect(toolbar, "destroy",
                     G_CALLBACK(toolbar_destroyed), app);
    g_signal_connect(toolbar, "popup-context-menu",
                     G_CALLBACK(toolbar_context_menu), app);
}

/* ===========================================================================
 * Config — ini next to the binary, ~/.config fallback (see app.h).
 * =========================================================================== */

#define BT_INI_GROUP "blue-tasks"

static GKeyFile *config_kf   = NULL; /* the in-memory config                */
static gchar    *config_path = NULL; /* where it is written                 */

/* ---------------------------------------------------------------------------
 * bt_app_config_init() — resolve + load the config file once.  Portable
 * mode: blue_tasks.ini next to the binary; when none exists there AND the
 * directory is unwritable, ~/.config/blue_tasks/blue_tasks.ini.  On first
 * run it is seeded from blue_tasks.ini.defaults next to the binary.
 * ------------------------------------------------------------------------- */
void
bt_app_config_init(const gchar *argv0)
{
    if (config_kf != NULL)
        return;

    gchar *exe_dir = exe_dir_from_argv0(argv0);
    gchar *local = g_build_filename(exe_dir, "blue_tasks.ini", NULL);
    if (g_file_test(local, G_FILE_TEST_EXISTS) ||
        g_access(exe_dir, W_OK) == 0) {
        config_path = local;         /* portable mode                       */
    } else {
        g_free(local);
        gchar *dir = g_build_filename(g_get_user_config_dir(),
                                      "blue_tasks", NULL);
        g_mkdir_with_parents(dir, 0700);
        config_path = g_build_filename(dir, "blue_tasks.ini", NULL);
        g_free(dir);
    }

    config_kf = g_key_file_new();
    if (!g_key_file_load_from_file(config_kf, config_path,
                                   G_KEY_FILE_NONE, NULL)) {
        /* First launch: seed from the committed defaults, if present.       */
        gchar *defaults = g_build_filename(exe_dir,
                                           "blue_tasks.ini.defaults", NULL);
        g_key_file_load_from_file(config_kf, defaults,
                                  G_KEY_FILE_NONE, NULL);
        g_free(defaults);
    }
    g_free(exe_dir);
}

/* ---------------------------------------------------------------------------
 * bt_app_config_get() — read one setting; NULL when unset/empty.
 * ------------------------------------------------------------------------- */
gchar *
bt_app_config_get(const gchar *key)
{
    if (config_kf == NULL)
        return NULL;
    gchar *v = g_key_file_get_string(config_kf, BT_INI_GROUP, key, NULL);
    if (v != NULL && *v == '\0') {
        g_free(v);
        v = NULL;
    }
    return v;
}

/* ---------------------------------------------------------------------------
 * bt_app_config_get_bool() — read a 0/1 setting (see app.h).
 * ------------------------------------------------------------------------- */
gboolean
bt_app_config_get_bool(const gchar *key, gboolean def)
{
    gchar *v = bt_app_config_get(key);
    if (v == NULL)
        return def;
    gboolean b = strcmp(v, "0") != 0;
    g_free(v);
    return b;
}

/* ---------------------------------------------------------------------------
 * bt_app_config_set() — change one setting and write the ini through.
 * NULL removes the key.  Unchanged values skip the rewrite.
 * ------------------------------------------------------------------------- */
void
bt_app_config_set(const gchar *key, const gchar *value)
{
    if (config_kf == NULL)
        return;
    gchar *old = g_key_file_get_string(config_kf, BT_INI_GROUP, key, NULL);
    gboolean same = (old == NULL && value == NULL) ||
                    (old != NULL && value != NULL &&
                     strcmp(old, value) == 0);
    g_free(old);
    if (same)
        return;
    if (value != NULL)
        g_key_file_set_string(config_kf, BT_INI_GROUP, key, value);
    else
        g_key_file_remove_key(config_kf, BT_INI_GROUP, key, NULL);
    g_key_file_save_to_file(config_kf, config_path, NULL);
}

/* ===========================================================================
 * Date helpers (see app.h).
 * =========================================================================== */

/* ---------------------------------------------------------------------------
 * bt_day_bounds() — local midnight bounds of "today + offset_days".
 * ------------------------------------------------------------------------- */
void
bt_day_bounds(gint offset_days, gint64 *lo, gint64 *hi)
{
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *day = g_date_time_add_days(now, offset_days);
    GDateTime *mid = g_date_time_new_local(g_date_time_get_year(day),
                                           g_date_time_get_month(day),
                                           g_date_time_get_day_of_month(day),
                                           0, 0, 0);
    GDateTime *nxt = g_date_time_add_days(mid, 1);
    *lo = g_date_time_to_unix(mid);
    *hi = g_date_time_to_unix(nxt);
    g_date_time_unref(now);
    g_date_time_unref(day);
    g_date_time_unref(mid);
    g_date_time_unref(nxt);
}

/* ---------------------------------------------------------------------------
 * bt_due_format() — human-readable due date ("" for none).
 * ------------------------------------------------------------------------- */
gchar *
bt_due_format(gint64 due)
{
    if (due == 0)
        return g_strdup("");
    GDateTime *dt = g_date_time_new_from_unix_local(due);
    gchar *s = g_date_time_format(dt, "%b %-e, %Y");
    g_date_time_unref(dt);
    return s != NULL ? s : g_strdup("");
}

/* ---------------------------------------------------------------------------
 * bt_due_color() — urgency tint (see app.h).  Compares calendar DAYS in
 * local time so the colors roll over at midnight.
 * ------------------------------------------------------------------------- */
const gchar *
bt_due_color(gint64 due)
{
    if (due == 0)
        return NULL;
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *dt  = g_date_time_new_from_unix_local(due);
    gint today = g_date_time_get_year(now) * 10000 +
                 g_date_time_get_month(now) * 100 +
                 g_date_time_get_day_of_month(now);
    gint day   = g_date_time_get_year(dt) * 10000 +
                 g_date_time_get_month(dt) * 100 +
                 g_date_time_get_day_of_month(dt);
    g_date_time_unref(now);
    g_date_time_unref(dt);
    return day < today  ? "#c01c28"          /* overdue: red                */
         : day == today ? "#d19a00"          /* today: gold                 */
                        : "#26a269";         /* ahead: green                */
}

/* ---------------------------------------------------------------------------
 * bt_due_parse() — "YYYY-MM-DD" or "M/D/YY[YY]" → local midnight unix.
 * ------------------------------------------------------------------------- */
gint64
bt_due_parse(const gchar *text)
{
    if (text == NULL)
        return 0;
    gchar *t = g_strstrip(g_strdup(text));
    gint y = 0, m = 0, d = 0;        /* parsed calendar fields              */
    gboolean ok = FALSE;
    if (sscanf(t, "%d-%d-%d", &y, &m, &d) == 3) {
        ok = TRUE;
    } else if (sscanf(t, "%d/%d/%d", &m, &d, &y) == 3) {
        if (y < 100)
            y += 2000;
        ok = TRUE;
    }
    g_free(t);
    if (!ok || m < 1 || m > 12 || d < 1 || d > 31 || y < 1970 || y > 9999)
        return 0;
    GDateTime *dt = g_date_time_new_local(y, m, d, 0, 0, 0);
    if (dt == NULL)
        return 0;
    gint64 u = g_date_time_to_unix(dt);
    g_date_time_unref(dt);
    return u;
}
