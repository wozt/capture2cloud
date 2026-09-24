#include "../../app_config.c"

void video_capture_watch_for_change(void) {}

void gamepad_bridge_prepare_console_wake(void) {}
int gamepad_bridge_console_wake_ready(void) { return 1; }
void gamepad_bridge_reset(void) {}


#include "../../reset_method.c"
#include "test_util.h"

#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

static char env_path[PATH_MAX];

static void setup_env(void)
{
    char dir[PATH_MAX];

    app_path(dir, sizeof(dir), "scripts");
    mkdir(dir, 0700);
    app_config_path(env_path, sizeof(env_path));

    remove(env_path);
    app_config_invalidate();
}

static void write_env(const char *body)
{
    FILE *f = fopen(env_path, "w");

    if (!f) {
        return;
    }

    fputs(body, f);
    fclose(f);
    app_config_invalidate();
}

static void test_names(void)
{
    t_begin("reset method names");

    t_eq_str("script name",
             reset_method_name(RESET_METHOD_SCRIPT),
             "script");

    t_eq_str("bluetooth name",
             reset_method_name(RESET_METHOD_BLUETOOTH),
             "bluetooth");

    t_ok("invalid method has no name",
         reset_method_name((ResetMethod)99) == NULL);
}

static void test_config_selection(void)
{
    t_begin("reset method config selection");

    remove(env_path);
    app_config_invalidate();

    t_eq_int("missing config defaults to script",
             reset_method_current(),
             RESET_METHOD_SCRIPT);

    write_env("RESET_METHOD=script\n");
    t_eq_int("script selected",
             reset_method_current(),
             RESET_METHOD_SCRIPT);

    write_env("RESET_METHOD=bluetooth\n");
    t_eq_int("bluetooth selected",
             reset_method_current(),
             RESET_METHOD_BLUETOOTH);

    write_env("RESET_METHOD=nonsense\n");
    t_eq_int("unknown value falls back to script",
             reset_method_current(),
             RESET_METHOD_SCRIPT);
}

static void test_persistent_set(void)
{
    t_begin("reset method persistent save");

    write_env(
        "# keep me\n"
        "OTHER_SETTING=untouched\n"
        "RESET_METHOD=script\n");

    t_eq_int("save bluetooth succeeds",
             reset_method_set(RESET_METHOD_BLUETOOTH),
             0);

    t_eq_int("saved value is immediately active",
             reset_method_current(),
             RESET_METHOD_BLUETOOTH);

    FILE *f = fopen(env_path, "r");
    char body[1024] = {0};

    if (f) {
        fread(body, 1, sizeof(body) - 1, f);
        fclose(f);
    }

    t_ok("RESET_METHOD was replaced",
         strstr(body, "RESET_METHOD=bluetooth\n") != NULL);

    t_ok("other config survived",
         strstr(body, "OTHER_SETTING=untouched\n") != NULL);

    t_ok("comment survived",
         strstr(body, "# keep me\n") != NULL);

    t_eq_int("invalid enum is rejected",
             reset_method_set((ResetMethod)99),
             -1);
}

int main(void)
{
    setup_env();

    test_names();
    test_config_selection();
    test_persistent_set();

    remove(env_path);
    return t_report();
}
