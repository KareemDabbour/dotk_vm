#include "../src/include/native_api.h"

#include <raylib.h>
#include <setjmp.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

static const DotKNativeApi *g_api = NULL;
static sigjmp_buf g_initWindowJmp;
static volatile sig_atomic_t g_inInitWindow = 0;

static void init_window_signal_handler(int signo)
{
    if (g_inInitWindow)
        siglongjmp(g_initWindowJmp, 1);

    signal(signo, SIG_DFL);
    raise(signo);
}

static bool expect_argc(const char *name, int got, int expected, bool *hasError)
{
    if (got == expected)
        return true;
    g_api->raiseError("%s expects %d argument(s) but got %d", name, expected, got);
    *hasError = true;
    return false;
}

static bool expect_number(Value v, const char *name, int index, bool *hasError)
{
    if (IS_NUM(v))
        return true;
    g_api->raiseError("%s expects argument %d to be Number", name, index);
    *hasError = true;
    return false;
}

static bool expect_string(Value v, const char *name, int index, bool *hasError)
{
    if (IS_STR(v))
        return true;
    g_api->raiseError("%s expects argument %d to be String", name, index);
    *hasError = true;
    return false;
}

static int as_int(Value v)
{
    return (int)AS_NUM(v);
}

static Color color_from_args(Value *argv, int start)
{
    Color c;
    c.r = (unsigned char)as_int(argv[start + 0]);
    c.g = (unsigned char)as_int(argv[start + 1]);
    c.b = (unsigned char)as_int(argv[start + 2]);
    c.a = (unsigned char)as_int(argv[start + 3]);
    return c;
}

static bool ensure_window_ready(const char *name, bool *hasError)
{
    if (IsWindowReady())
        return true;
    g_api->raiseError("%s requires a ready raylib window/context. Call rl_init_window(...) and ensure it succeeds.", name);
    *hasError = true;
    return false;
}

static void ensure_wsl_display_env(void)
{
    const char *wslDistro = getenv("WSL_DISTRO_NAME");
    if (wslDistro == NULL || *wslDistro == '\0')
        return;

    const char *display = getenv("DISPLAY");
    if (display == NULL || *display == '\0')
    {
        setenv("DISPLAY", ":0", 1);
        return;
    }

    if (strcmp(display, ":0") == 0)
        return;

    if (strchr(display, '.') != NULL)
        setenv("DISPLAY", ":0", 1);
}

static Value rl_init_window_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("rl_init_window(width, height, title)", argc, 3, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "rl_init_window(width, height, title)", 1, hasError) ||
        !expect_number(argv[1], "rl_init_window(width, height, title)", 2, hasError) ||
        !expect_string(argv[2], "rl_init_window(width, height, title)", 3, hasError))
        return NIL_VAL;

    ensure_wsl_display_env();

    struct sigaction oldSegv;
    struct sigaction oldBus;
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = init_window_signal_handler;
    sigemptyset(&action.sa_mask);

    sigaction(SIGSEGV, &action, &oldSegv);
    sigaction(SIGBUS, &action, &oldBus);

    if (sigsetjmp(g_initWindowJmp, 1) != 0)
    {
        g_inInitWindow = 0;
        sigaction(SIGSEGV, &oldSegv, NULL);
        sigaction(SIGBUS, &oldBus, NULL);

        g_api->raiseError("rl_init_window crashed inside raylib/GLX while creating context (WSL graphics stack issue)");
        *hasError = true;
        return BOOL_VAL(false);
    }

    g_inInitWindow = 1;
    InitWindow(as_int(argv[0]), as_int(argv[1]), AS_CSTR(argv[2]));
    g_inInitWindow = 0;

    sigaction(SIGSEGV, &oldSegv, NULL);
    sigaction(SIGBUS, &oldBus, NULL);

    if (!IsWindowReady())
    {
        g_api->raiseError("rl_init_window failed: raylib could not create a window/context (check WSLg/OpenGL setup)");
        *hasError = true;
        return BOOL_VAL(false);
    }

    return BOOL_VAL(true);
}

static Value rl_close_window_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("rl_close_window()", argc, 0, hasError))
        return NIL_VAL;

    CloseWindow();
    return NIL_VAL;
}

static Value rl_set_target_fps_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("rl_set_target_fps(fps)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "rl_set_target_fps(fps)", 1, hasError))
        return NIL_VAL;
    if (!ensure_window_ready("rl_set_target_fps", hasError))
        return NIL_VAL;

    SetTargetFPS(as_int(argv[0]));
    return NIL_VAL;
}

static Value rl_window_should_close_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("rl_window_should_close()", argc, 0, hasError))
        return NIL_VAL;

    return BOOL_VAL(WindowShouldClose());
}

static Value rl_begin_drawing_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("rl_begin_drawing()", argc, 0, hasError))
        return NIL_VAL;
    if (!ensure_window_ready("rl_begin_drawing", hasError))
        return NIL_VAL;

    BeginDrawing();
    return NIL_VAL;
}

static Value rl_end_drawing_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("rl_end_drawing()", argc, 0, hasError))
        return NIL_VAL;
    if (!ensure_window_ready("rl_end_drawing", hasError))
        return NIL_VAL;

    EndDrawing();
    return NIL_VAL;
}

static Value rl_clear_background_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("rl_clear_background(r, g, b, a)", argc, 4, hasError))
        return NIL_VAL;
    for (int i = 0; i < 4; i++)
    {
        if (!expect_number(argv[i], "rl_clear_background(r, g, b, a)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window_ready("rl_clear_background", hasError))
        return NIL_VAL;

    ClearBackground(color_from_args(argv, 0));
    return NIL_VAL;
}

static Value rl_draw_text_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("rl_draw_text(text, x, y, size, r, g, b, a)", argc, 8, hasError))
        return NIL_VAL;

    if (!expect_string(argv[0], "rl_draw_text(text, x, y, size, r, g, b, a)", 1, hasError))
        return NIL_VAL;
    for (int i = 1; i < 8; i++)
    {
        if (!expect_number(argv[i], "rl_draw_text(text, x, y, size, r, g, b, a)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window_ready("rl_draw_text", hasError))
        return NIL_VAL;

    DrawText(
        AS_CSTR(argv[0]),
        as_int(argv[1]),
        as_int(argv[2]),
        as_int(argv[3]),
        color_from_args(argv, 4));

    return NIL_VAL;
}

static Value rl_draw_rectangle_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("rl_draw_rectangle(x, y, w, h, r, g, b, a)", argc, 8, hasError))
        return NIL_VAL;
    for (int i = 0; i < 8; i++)
    {
        if (!expect_number(argv[i], "rl_draw_rectangle(x, y, w, h, r, g, b, a)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window_ready("rl_draw_rectangle", hasError))
        return NIL_VAL;

    DrawRectangle(
        as_int(argv[0]),
        as_int(argv[1]),
        as_int(argv[2]),
        as_int(argv[3]),
        color_from_args(argv, 4));

    return NIL_VAL;
}

static Value rl_draw_circle_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("rl_draw_circle(x, y, radius, r, g, b, a)", argc, 7, hasError))
        return NIL_VAL;
    for (int i = 0; i < 7; i++)
    {
        if (!expect_number(argv[i], "rl_draw_circle(x, y, radius, r, g, b, a)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window_ready("rl_draw_circle", hasError))
        return NIL_VAL;

    DrawCircle(
        as_int(argv[0]),
        as_int(argv[1]),
        as_int(argv[2]),
        color_from_args(argv, 3));

    return NIL_VAL;
}

static Value rl_get_frame_time_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("rl_get_frame_time()", argc, 0, hasError))
        return NIL_VAL;
    if (!ensure_window_ready("rl_get_frame_time", hasError))
        return NIL_VAL;

    return NUM_VAL((double)GetFrameTime());
}

static Value rl_is_key_down_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("rl_is_key_down(key)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "rl_is_key_down(key)", 1, hasError))
        return NIL_VAL;
    if (!ensure_window_ready("rl_is_key_down", hasError))
        return NIL_VAL;

    return BOOL_VAL(IsKeyDown(as_int(argv[0])));
}

bool dotk_init_module(const DotKNativeApi *api)
{
    if (api == NULL || api->version != DOTK_NATIVE_API_VERSION)
        return false;

    g_api = api;

    api->defineNative("rl_init_window", rl_init_window_native);
    api->defineNative("rl_close_window", rl_close_window_native);
    api->defineNative("rl_set_target_fps", rl_set_target_fps_native);
    api->defineNative("rl_window_should_close", rl_window_should_close_native);
    api->defineNative("rl_begin_drawing", rl_begin_drawing_native);
    api->defineNative("rl_end_drawing", rl_end_drawing_native);
    api->defineNative("rl_clear_background", rl_clear_background_native);
    api->defineNative("rl_draw_text", rl_draw_text_native);
    api->defineNative("rl_draw_rectangle", rl_draw_rectangle_native);
    api->defineNative("rl_draw_circle", rl_draw_circle_native);
    api->defineNative("rl_get_frame_time", rl_get_frame_time_native);
    api->defineNative("rl_is_key_down", rl_is_key_down_native);

    api->defineGlobalValue("RL_KEY_ESCAPE", NUM_VAL((double)KEY_ESCAPE));
    api->defineGlobalValue("RL_KEY_SPACE", NUM_VAL((double)KEY_SPACE));
    api->defineGlobalValue("RL_KEY_ENTER", NUM_VAL((double)KEY_ENTER));
    api->defineGlobalValue("RL_KEY_LEFT", NUM_VAL((double)KEY_LEFT));
    api->defineGlobalValue("RL_KEY_RIGHT", NUM_VAL((double)KEY_RIGHT));
    api->defineGlobalValue("RL_KEY_UP", NUM_VAL((double)KEY_UP));
    api->defineGlobalValue("RL_KEY_DOWN", NUM_VAL((double)KEY_DOWN));

    return true;
}
