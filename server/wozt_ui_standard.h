#ifndef CAPTURE2CLOUD_WOZT_UI_STANDARD_H
#define CAPTURE2CLOUD_WOZT_UI_STANDARD_H
#include <gtk/gtk.h>

typedef struct WoztUiStandard WoztUiStandard;

/* All functions are called by Capture2Cloud's dedicated GTK thread. */
WoztUiStandard *wozt_ui_standard_new(const char *data_dir,
                                     const char *saved_theme,
                                     const char *saved_language);
void wozt_ui_standard_free(WoztUiStandard *ui);
GtkWidget *wozt_ui_standard_theme_combo(WoztUiStandard *ui);
GtkWidget *wozt_ui_standard_language_combo(WoztUiStandard *ui);
const char *wozt_ui_standard_theme_id(WoztUiStandard *ui);
const char *wozt_ui_standard_language_id(WoztUiStandard *ui);
void wozt_ui_standard_apply_theme(WoztUiStandard *ui, const char *id);
void wozt_ui_standard_apply_language(WoztUiStandard *ui, const char *id,
                                      GtkWidget *root);
void wozt_ui_standard_reload(WoztUiStandard *ui, GtkWidget *root);

#endif
