/* Smoke-test that installed GTK symbolic icons are actually decodable. */
#include <gtk/gtk.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    gtk_init(&argc, &argv);
    GtkIconTheme *theme = gtk_icon_theme_get_default();
    const char *names[] = {
        "window-close-symbolic", "window-minimize-symbolic",
        "preferences-system-symbolic", "document-open-symbolic"
    };
    for (gsize i = 0; i < G_N_ELEMENTS(names); ++i) {
        GError *error = NULL;
        GdkPixbuf *icon = gtk_icon_theme_load_icon(theme, names[i], 16,
                                                   GTK_ICON_LOOKUP_FORCE_SIZE, &error);
        if (!icon) {
            fprintf(stderr, "Missing GTK runtime icon %s: %s\n", names[i],
                    error ? error->message : "unknown error");
            g_clear_error(&error);
            return 1;
        }
        g_object_unref(icon);
    }
    puts("GTK symbolic icons loaded successfully");
    return 0;
}
