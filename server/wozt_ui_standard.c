/* WOZT GTK Standard v1.2 resources and live styling for Capture2Cloud.
 * UI-only: never touches capture threads, codecs, Bluetooth or network state.
 * The repository ships JSON palettes and translations, including AppImage. */
#include "wozt_ui_standard.h"
#include <json-glib/json-glib.h>
#include <string.h>

#define WOZT_COLORS 11
static const char *const wozt_color_keys[WOZT_COLORS] = {
    "background", "panel", "entry", "border", "text", "muted",
    "accent", "button", "hover", "active", "negative"
};

typedef struct {
    char *id;
    char *name;
    char *path;
} WoztResource;

struct WoztUiStandard {
    char *data_dir;
    GPtrArray *themes;
    GPtrArray *languages;
    char *theme_id;
    char *language_id;
    GHashTable *translations;
    GtkCssProvider *css;
    GdkRGBA colors[WOZT_COLORS];
    gboolean colors_ready;
    GtkWidget *theme_combo, *language_combo; /* owned by the GTK window */
};

static void wozt_resource_free(gpointer data)
{
    WoztResource *r = data;
    if (!r) return;
    g_free(r->id);
    g_free(r->name);
    g_free(r->path);
    g_free(r);
}

static gboolean wozt_valid_id(const char *s)
{
    if (!s || !*s) return FALSE;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p)
        if (!(g_ascii_islower(*p) || g_ascii_isdigit(*p) || *p == '-' || *p == '_'))
            return FALSE;
    return TRUE;
}

static gboolean wozt_hex_color(const char *s)
{
    if (!s || strlen(s) != 7 || s[0] != '#') return FALSE;
    for (int i = 1; i < 7; ++i)
        if (!g_ascii_isxdigit(s[i])) return FALSE;
    return TRUE;
}

static JsonObject *wozt_json_object(const char *path, JsonParser **parser_out)
{
    GError *error = NULL;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, path, &error)) {
        g_warning("wozt-ui: cannot load %s: %s", path,
                  error ? error->message : "invalid JSON");
        g_clear_error(&error);
        g_object_unref(parser);
        return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    if (!root || !JSON_NODE_HOLDS_OBJECT(root)) {
        g_warning("wozt-ui: JSON root must be an object: %s", path);
        g_object_unref(parser);
        return NULL;
    }
    *parser_out = parser;
    return json_node_get_object(root);
}

static const char *wozt_string(JsonObject *obj, const char *key)
{
    if (!json_object_has_member(obj, key)) return NULL;
    JsonNode *node = json_object_get_member(obj, key);
    return node && JSON_NODE_HOLDS_VALUE(node) &&
           json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : NULL;
}

/* Expose the validated, active JSON palette to Cairo-drawn widgets.
 * Called only from the GTK thread (like the CSS/theme API). */
gboolean wozt_ui_standard_get_color(WoztUiStandard *ui, const char *key,
                                    GdkRGBA *out)
{
    if (!ui || !ui->colors_ready || !key || !out) return FALSE;
    for (int i = 0; i < WOZT_COLORS; ++i) {
        if (g_strcmp0(key, wozt_color_keys[i]) == 0) {
            *out = ui->colors[i];
            return TRUE;
        }
    }
    return FALSE;
}

static void wozt_scan_resources(WoztUiStandard *ui, const char *folder, GPtrArray *list)
{
    char *dir = g_build_filename(ui->data_dir, folder, NULL);
    GDir *handle = g_dir_open(dir, 0, NULL);
    if (!handle) {
        g_warning("wozt-ui: missing resource directory: %s", dir);
        g_free(dir);
        return;
    }
    const char *entry;
    while ((entry = g_dir_read_name(handle))) {
        if (!g_str_has_suffix(entry, ".json")) continue;
        char *path = g_build_filename(dir, entry, NULL);
        JsonParser *parser = NULL;
        JsonObject *obj = wozt_json_object(path, &parser);
        if (obj) {
            const char *id = wozt_string(obj, "id");
            const char *name = wozt_string(obj, "name");
            if (wozt_valid_id(id) && name && *name) {
                WoztResource *r = g_new0(WoztResource, 1);
                r->id = g_strdup(id);
                r->name = g_strdup(name);
                r->path = g_strdup(path);
                g_ptr_array_add(list, r);
            } else {
                g_warning("wozt-ui: invalid id/name in %s", path);
            }
            g_object_unref(parser);
        }
        g_free(path);
    }
    g_dir_close(handle);
    g_free(dir);
}

static gint wozt_resource_sort(gconstpointer aa, gconstpointer bb)
{
    const WoztResource *a = *(WoztResource *const *)aa;
    const WoztResource *b = *(WoztResource *const *)bb;
    return g_strcmp0(a->path, b->path);
}

static WoztResource *wozt_find(GPtrArray *list, const char *id)
{
    for (guint i = 0; i < list->len; ++i) {
        WoztResource *r = g_ptr_array_index(list, i);
        if (!g_strcmp0(r->id, id)) return r;
    }
    return NULL;
}

static void wozt_load_language(WoztUiStandard *ui, const char *id)
{
    g_hash_table_remove_all(ui->translations);
    g_free(ui->language_id);
    ui->language_id = g_strdup(id && *id ? id : "en");
    if (!g_strcmp0(ui->language_id, "en")) return;
    WoztResource *r = wozt_find(ui->languages, ui->language_id);
    if (!r) return;
    JsonParser *parser = NULL;
    JsonObject *obj = wozt_json_object(r->path, &parser);
    if (obj && json_object_has_member(obj, "translations") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(obj, "translations"))) {
        JsonObject *translations = json_object_get_object_member(obj, "translations");
        GList *keys = json_object_get_members(translations);
        for (GList *p = keys; p; p = p->next) {
            const char *key = p->data;
            const char *value = wozt_string(translations, key);
            if (key && value) g_hash_table_insert(ui->translations,
                                                  g_strdup(key), g_strdup(value));
        }
        g_list_free(keys);
    }
    if (parser) g_object_unref(parser);
}

static const char *wozt_tr(WoztUiStandard *ui, const char *english)
{
    if (!english) return "";
    const char *localized = g_hash_table_lookup(ui->translations, english);
    return localized && *localized ? localized : english;
}

/* Translate a stable English source, never a previously translated value. */
static void wozt_translate_label(WoztUiStandard *ui, GtkWidget *widget)
{
    GtkLabel *label = GTK_LABEL(widget);
    if (gtk_label_get_selectable(label)) return; /* live status/diagnostics */
    const char *english = g_object_get_data(G_OBJECT(widget), "wozt-source-label");
    if (!english) {
        const char *shown = gtk_label_get_text(label);
        if (!shown || !*shown) return;
        english = g_strdup(shown);
        g_object_set_data_full(G_OBJECT(widget), "wozt-source-label", (gpointer)english, g_free);
        if (gtk_label_get_use_markup(label))
            g_object_set_data_full(G_OBJECT(widget), "wozt-source-markup",
                                   g_strdup(gtk_label_get_label(label)), g_free);
    }
    const char *localized = wozt_tr(ui, english);
    const char *markup = g_object_get_data(G_OBJECT(widget), "wozt-source-markup");
    if (markup) {
        const char *pos = strstr(markup, english);
        if (pos) {
            char *escaped = g_markup_escape_text(localized, -1);
            GString *next = g_string_new_len(markup, (gssize)(pos - markup));
            g_string_append(next, escaped);
            g_string_append(next, pos + strlen(english));
            gtk_label_set_markup(label, next->str);
            g_string_free(next, TRUE);
            g_free(escaped);
        }
    } else {
        gtk_label_set_text(label, localized);
    }
}

typedef struct {
    GtkTreeModel *model; /* weak: combo owns the model */
    GPtrArray *english;
} WoztComboText;

static void wozt_combo_text_free(gpointer ptr)
{
    WoztComboText *data = ptr;
    if (!data) return;
    g_ptr_array_unref(data->english);
    g_free(data);
}

static void wozt_translate_combo(WoztUiStandard *ui, GtkWidget *widget)
{
    if (g_object_get_data(G_OBJECT(widget), "wozt-no-translate")) return;
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget));
    if (!model || !GTK_IS_LIST_STORE(model) ||
        gtk_tree_model_get_n_columns(model) < 1 ||
        gtk_tree_model_get_column_type(model, 0) != G_TYPE_STRING) return;

    WoztComboText *data = g_object_get_data(G_OBJECT(widget), "wozt-combo-source");
    if (!data || data->model != model) {
        data = g_new0(WoztComboText, 1);
        data->model = model;
        data->english = g_ptr_array_new_with_free_func(g_free);
        GtkTreeIter it;
        if (gtk_tree_model_get_iter_first(model, &it)) {
            do {
                gchar *str = NULL;
                gtk_tree_model_get(model, &it, 0, &str, -1);
                g_ptr_array_add(data->english, str ? str : g_strdup(""));
            } while (gtk_tree_model_iter_next(model, &it));
        }
        g_object_set_data_full(G_OBJECT(widget), "wozt-combo-source", data,
                               wozt_combo_text_free);
    }
    GtkTreeIter it;
    guint i = 0;
    if (gtk_tree_model_get_iter_first(model, &it)) {
        do {
            if (i < data->english->len) {
                const char *english = g_ptr_array_index(data->english, i);
                gtk_list_store_set(GTK_LIST_STORE(model), &it, 0,
                                   wozt_tr(ui, english), -1);
            }
            ++i;
        } while (gtk_tree_model_iter_next(model, &it));
    }
}

static void wozt_translate_tree(WoztUiStandard *ui, GtkWidget *widget);
static void wozt_translate_child(GtkWidget *child, gpointer data)
{
    WoztUiStandard *ui = data;
    wozt_translate_tree(ui, child);
}

static void wozt_translate_tree(WoztUiStandard *ui, GtkWidget *widget)
{
    if (GTK_IS_LABEL(widget)) wozt_translate_label(ui, widget);
    if (GTK_IS_COMBO_BOX(widget)) wozt_translate_combo(ui, widget);
    const char *tooltip = g_object_get_data(G_OBJECT(widget), "wozt-source-tooltip");
    if (!tooltip) {
        const char *shown = gtk_widget_get_tooltip_text(widget);
        if (shown && *shown) {
            tooltip = g_strdup(shown);
            g_object_set_data_full(G_OBJECT(widget), "wozt-source-tooltip", (gpointer)tooltip, g_free);
        }
    }
    if (tooltip) gtk_widget_set_tooltip_text(widget, wozt_tr(ui, tooltip));
    if (GTK_IS_CONTAINER(widget))
        gtk_container_foreach(GTK_CONTAINER(widget), wozt_translate_child, ui);
    if (GTK_IS_FRAME(widget)) {
        GtkWidget *heading = gtk_frame_get_label_widget(GTK_FRAME(widget));
        if (heading) wozt_translate_tree(ui, heading);
    }
}

static void wozt_apply_palette(WoztUiStandard *ui, const char *id)
{
    WoztResource *r = wozt_find(ui->themes, id);
    if (!r) {
        g_warning("wozt-ui: unknown theme %s; using graphite", id ? id : "(null)");
        r = wozt_find(ui->themes, "graphite");
    }
    if (!r) return;
    JsonParser *parser = NULL;
    JsonObject *obj = wozt_json_object(r->path, &parser);
    if (!obj) return;
    if (!json_object_has_member(obj, "colors") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(obj, "colors"))) {
        g_warning("wozt-ui: theme lacks colors: %s", r->path);
        g_object_unref(parser);
        return;
    }
    JsonObject *colors = json_object_get_object_member(obj, "colors");
    const char *v[WOZT_COLORS];
    for (int i = 0; i < WOZT_COLORS; ++i) {
        v[i] = wozt_string(colors, wozt_color_keys[i]);
        if (!wozt_hex_color(v[i])) {
            g_warning("wozt-ui: invalid palette key %s in %s", wozt_color_keys[i], r->path);
            g_object_unref(parser);
            return;
        }
    }
    /* WUP Stream reference density: 11px text, 17px controls, squared borders.
     * Keep selectors and popup surfaces themed too; no animation or repeated repaint. */
    char *css = g_strdup_printf(
        "window.wozt-window, window.wozt-window box.wozt-dashboard, "
        "window.wozt-window scrolledwindow.wozt-dashboard {background-color:%s;color:%s;}"
        "window.wozt-window {font-size:11px;border-radius:0; }"
        "window.wozt-window frame.wozt-section {background-color:%s; border:1px solid %s; margin:0; padding:1px;}"
        "window.wozt-window frame.wozt-section > border {border:1px solid %s;border-radius:0;}"
        "window.wozt-window .wozt-section-title {font-size:12px;font-weight:bold;color:%s;}"
        "window.wozt-window .wozt-subsection {font-size:11px;font-weight:bold;color:%s;}"
        "window.wozt-window .wozt-titlebar {background-color:%s;color:%s;border-bottom:1px solid %s;}"
        "window.wozt-window .wozt-title {font-size:12px;font-weight:bold;color:%s;}"
        "window.wozt-window button,window.wozt-window combobox button,dialog button,messagedialog button,popover button {background:%s;color:%s;min-height:19px;padding:0 5px;border:1px solid %s;border-radius:0;}"
        "window.wozt-window button:hover,window.wozt-window combobox button:hover,dialog button:hover,messagedialog button:hover,popover button:hover {background:%s;}"
        "window.wozt-window button:active,window.wozt-window button:checked,dialog button:active,messagedialog button:active,popover button:active {background:%s;}"
        "window.wozt-window button:focus,window.wozt-window entry:focus,dialog button:focus,dialog entry:focus,messagedialog button:focus,messagedialog entry:focus,popover button:focus,popover entry:focus {outline-color:%s;}"
        "window.wozt-window entry,window.wozt-window spinbutton,dialog entry,dialog spinbutton,messagedialog entry,messagedialog spinbutton,popover entry,popover spinbutton {background:%s;color:%s;min-height:19px;padding:0 4px;border:1px solid %s;}"
        "window.wozt-window checkbutton check {min-height:14px;min-width:14px;border-radius:0;}"
        "window.wozt-window .wozt-window-button {min-width:20px;min-height:19px;padding:0;}"
        "window.wozt-window .wozt-danger:hover {background:%s;}"
        "window.wozt-window .wozt-resize {background:transparent;border:0;}"
        "window.wozt-window label.wozt-muted {color:%s;}"
        "window.wozt-window notebook tab {padding:1px 4px;}"
        "window.wozt-window notebook header {margin-bottom:2px;}"
        "window.wozt-window scale slider {min-width:10px;min-height:10px;}"
        "window.wozt-window notebook tab:checked {background:%s;color:%s;}"
        "menu,tooltip,dialog {border-radius:0;} menu,dialog {background:%s;color:%s;}"
        "tooltip {background:%s;color:%s;}"
        , v[0],v[4],v[1],v[3],v[3],v[6],v[6],v[1],v[4],v[3],v[6],
          v[7],v[4],v[3],v[8],v[9],v[6],v[2],v[4],v[3],v[10],v[5],
          v[9],v[4],v[1],v[4],v[1],v[4]);
    /* Recomputed for EVERY theme, just like the rest of the CSS.
     * Keep the native scale value and callback behavior intact. */
    char *slider_css = g_strdup_printf(
        "window.wozt-window scale trough {background-image:none;"
        "background-color:%s;border:1px solid %s;"
        "border-radius:0;min-height:5px;box-shadow:none;}"
        "window.wozt-window scale trough highlight,"
        "window.wozt-window scale.horizontal trough highlight,"
        "window.wozt-window scale.vertical trough highlight {"
        "background-image:none;background-color:%s;"
        "border:0;border-radius:0;box-shadow:none;}"
        "window.wozt-window scale trough slider,"
        "window.wozt-window scale slider {"
        "background-image:none;background-color:%s;"
        "border:1px solid %s;border-radius:0;box-shadow:none;}",
        v[2], v[3], v[6], v[6], v[3]);
    char *complete_css = g_strconcat(css, slider_css, NULL);
    g_free(slider_css);
    GError *error = NULL;
    gtk_css_provider_load_from_data(ui->css, complete_css, -1, &error);
    if (error) {
        g_warning("wozt-ui: invalid generated CSS: %s", error->message);
        g_clear_error(&error);
    }
    g_free(complete_css);
    g_free(css);
    /* Colors are already validated as #RRGGBB; parsing is deterministic. */
    for (int i = 0; i < WOZT_COLORS; ++i)
        gdk_rgba_parse(&ui->colors[i], v[i]);
    ui->colors_ready = TRUE;
    g_free(ui->theme_id);
    ui->theme_id = g_strdup(r->id);
    g_object_unref(parser);
}

WoztUiStandard *wozt_ui_standard_new(const char *data_dir,
                                     const char *saved_theme,
                                     const char *saved_language)
{
    WoztUiStandard *ui = g_new0(WoztUiStandard, 1);
    ui->data_dir = g_strdup(data_dir);
    ui->themes = g_ptr_array_new_with_free_func(wozt_resource_free);
    ui->languages = g_ptr_array_new_with_free_func(wozt_resource_free);
    ui->translations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    ui->css = gtk_css_provider_new();
    wozt_scan_resources(ui, "themes", ui->themes);
    wozt_scan_resources(ui, "languages", ui->languages);
    g_ptr_array_sort(ui->themes, wozt_resource_sort);
    g_ptr_array_sort(ui->languages, wozt_resource_sort);
    /* Parse the first complete palette BEFORE registering this provider.
     * Attaching an empty provider and then loading it caused needless GTK3
     * style invalidation while the window hierarchy was being created. */
    wozt_apply_palette(ui, saved_theme && *saved_theme ? saved_theme : "graphite");
    GdkScreen *screen = gdk_screen_get_default();
    if (screen) gtk_style_context_add_provider_for_screen(
        screen, GTK_STYLE_PROVIDER(ui->css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    wozt_load_language(ui, saved_language && *saved_language ? saved_language : "en");
    return ui;
}

void wozt_ui_standard_free(WoztUiStandard *ui)
{
    if (!ui) return;
    GdkScreen *screen = gdk_screen_get_default();
    if (screen) gtk_style_context_remove_provider_for_screen(screen, GTK_STYLE_PROVIDER(ui->css));
    g_object_unref(ui->css);
    g_hash_table_destroy(ui->translations);
    g_ptr_array_unref(ui->themes);
    g_ptr_array_unref(ui->languages);
    g_free(ui->theme_id);
    g_free(ui->language_id);
    g_free(ui->data_dir);
    g_free(ui);
}

GtkWidget *wozt_ui_standard_theme_combo(WoztUiStandard *ui)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    ui->theme_combo = combo;
    for (guint i = 0; i < ui->themes->len; ++i) {
        WoztResource *r = g_ptr_array_index(ui->themes, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), r->id, r->name);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), ui->theme_id);
    g_object_set_data(G_OBJECT(combo), "wozt-no-translate", GINT_TO_POINTER(1));
    return combo;
}

GtkWidget *wozt_ui_standard_language_combo(WoztUiStandard *ui)
{
    GtkWidget *combo = gtk_combo_box_text_new();
    ui->language_combo = combo;
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "en", "English");
    for (guint i = 0; i < ui->languages->len; ++i) {
        WoztResource *r = g_ptr_array_index(ui->languages, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), r->id, r->name);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), ui->language_id);
    g_object_set_data(G_OBJECT(combo), "wozt-no-translate", GINT_TO_POINTER(1));
    return combo;
}

const char *wozt_ui_standard_theme_id(WoztUiStandard *ui)
{ return ui && ui->theme_id ? ui->theme_id : "graphite"; }
const char *wozt_ui_standard_language_id(WoztUiStandard *ui)
{ return ui && ui->language_id ? ui->language_id : "en"; }

void wozt_ui_standard_apply_theme(WoztUiStandard *ui, const char *id)
{
    if (ui) wozt_apply_palette(ui, id);
}

void wozt_ui_standard_apply_language(WoztUiStandard *ui, const char *id,
                                      GtkWidget *root)
{
    if (!ui) return;
    wozt_load_language(ui, id);
    if (root) wozt_translate_tree(ui, root);
}

void wozt_ui_standard_reload(WoztUiStandard *ui, GtkWidget *root)
{
    if (!ui) return;
    char *theme = g_strdup(wozt_ui_standard_theme_id(ui));
    char *language = g_strdup(wozt_ui_standard_language_id(ui));
    /* Re-enumerate the directories: a user may have added a custom JSON
     * palette/translation since the last reload. Existing ID stays selected. */
    g_ptr_array_set_size(ui->themes, 0);
    g_ptr_array_set_size(ui->languages, 0);
    wozt_scan_resources(ui, "themes", ui->themes);
    wozt_scan_resources(ui, "languages", ui->languages);
    g_ptr_array_sort(ui->themes, wozt_resource_sort);
    g_ptr_array_sort(ui->languages, wozt_resource_sort);
    if (!wozt_find(ui->themes, theme)) {
        g_free(theme);
        theme = g_strdup("graphite");
    }
    if (g_strcmp0(language, "en") && !wozt_find(ui->languages, language)) {
        g_free(language);
        language = g_strdup("en");
    }
    wozt_apply_palette(ui, theme);
    wozt_load_language(ui, language);
    if (ui->theme_combo) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui->theme_combo));
        for (guint i = 0; i < ui->themes->len; ++i) {
            WoztResource *r = g_ptr_array_index(ui->themes, i);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->theme_combo), r->id, r->name);
        }
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->theme_combo),
                                    wozt_ui_standard_theme_id(ui));
    }
    if (ui->language_combo) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui->language_combo));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->language_combo), "en", "English");
        for (guint i = 0; i < ui->languages->len; ++i) {
            WoztResource *r = g_ptr_array_index(ui->languages, i);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->language_combo), r->id, r->name);
        }
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->language_combo),
                                    wozt_ui_standard_language_id(ui));
    }
    if (root) wozt_translate_tree(ui, root);
    g_free(theme);
    g_free(language);
}
