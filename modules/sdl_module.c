#include "../src/include/native_api.h"

#include <SDL2/SDL.h>
#include <stdbool.h>

static const DotKNativeApi *g_api = NULL;
static SDL_Window *g_window = NULL;
static SDL_Renderer *g_renderer = NULL;
static bool g_shouldClose = false;
static int g_targetFps = 60;

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

static bool ensure_renderer(const char *name, bool *hasError)
{
    if (g_renderer != NULL)
        return true;

    g_api->raiseError("%s requires an initialized SDL window. Call sdl_init_window(...) first.", name);
    *hasError = true;
    return false;
}

static void pump_events(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_QUIT)
            g_shouldClose = true;
    }
}

static Value sdl_init_window_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("sdl_init_window(width, height, title)", argc, 3, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "sdl_init_window(width, height, title)", 1, hasError) ||
        !expect_number(argv[1], "sdl_init_window(width, height, title)", 2, hasError) ||
        !expect_string(argv[2], "sdl_init_window(width, height, title)", 3, hasError))
        return NIL_VAL;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0)
    {
        g_api->raiseError("sdl_init_window failed: %s", SDL_GetError());
        *hasError = true;
        return BOOL_VAL(false);
    }

    g_window = SDL_CreateWindow(
        AS_CSTR(argv[2]),
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        as_int(argv[0]),
        as_int(argv[1]),
        SDL_WINDOW_SHOWN);

    if (g_window == NULL)
    {
        g_api->raiseError("sdl_init_window failed (window): %s", SDL_GetError());
        SDL_Quit();
        *hasError = true;
        return BOOL_VAL(false);
    }

    g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (g_renderer == NULL)
    {
        g_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
    }

    if (g_renderer == NULL)
    {
        g_api->raiseError("sdl_init_window failed (renderer): %s", SDL_GetError());
        SDL_DestroyWindow(g_window);
        g_window = NULL;
        SDL_Quit();
        *hasError = true;
        return BOOL_VAL(false);
    }

    g_shouldClose = false;
    return BOOL_VAL(true);
}

static Value sdl_close_window_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("sdl_close_window()", argc, 0, hasError))
        return NIL_VAL;

    if (g_renderer != NULL)
    {
        SDL_DestroyRenderer(g_renderer);
        g_renderer = NULL;
    }
    if (g_window != NULL)
    {
        SDL_DestroyWindow(g_window);
        g_window = NULL;
    }
    SDL_Quit();

    g_shouldClose = true;
    return NIL_VAL;
}

static Value sdl_set_target_fps_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("sdl_set_target_fps(fps)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "sdl_set_target_fps(fps)", 1, hasError))
        return NIL_VAL;

    int fps = as_int(argv[0]);
    if (fps < 1)
        fps = 1;
    g_targetFps = fps;
    return NIL_VAL;
}

static Value sdl_window_should_close_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("sdl_window_should_close()", argc, 0, hasError))
        return NIL_VAL;

    pump_events();
    return BOOL_VAL(g_shouldClose);
}

static Value sdl_begin_frame_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("sdl_begin_frame(r, g, b, a)", argc, 4, hasError))
        return NIL_VAL;
    for (int i = 0; i < 4; i++)
    {
        if (!expect_number(argv[i], "sdl_begin_frame(r, g, b, a)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_renderer("sdl_begin_frame", hasError))
        return NIL_VAL;

    pump_events();

    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(
        g_renderer,
        (Uint8)as_int(argv[0]),
        (Uint8)as_int(argv[1]),
        (Uint8)as_int(argv[2]),
        (Uint8)as_int(argv[3]));
    SDL_RenderClear(g_renderer);

    return NIL_VAL;
}

static Value sdl_end_frame_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("sdl_end_frame()", argc, 0, hasError))
        return NIL_VAL;
    if (!ensure_renderer("sdl_end_frame", hasError))
        return NIL_VAL;

    SDL_RenderPresent(g_renderer);
    if (g_targetFps > 0)
        SDL_Delay((Uint32)(1000 / g_targetFps));
    return NIL_VAL;
}

static Value sdl_draw_rect_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("sdl_draw_rect(x, y, w, h, r, g, b, a)", argc, 8, hasError))
        return NIL_VAL;
    for (int i = 0; i < 8; i++)
    {
        if (!expect_number(argv[i], "sdl_draw_rect(x, y, w, h, r, g, b, a)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_renderer("sdl_draw_rect", hasError))
        return NIL_VAL;

    SDL_Rect rect;
    rect.x = as_int(argv[0]);
    rect.y = as_int(argv[1]);
    rect.w = as_int(argv[2]);
    rect.h = as_int(argv[3]);

    SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(
        g_renderer,
        (Uint8)as_int(argv[4]),
        (Uint8)as_int(argv[5]),
        (Uint8)as_int(argv[6]),
        (Uint8)as_int(argv[7]));
    SDL_RenderFillRect(g_renderer, &rect);

    return NIL_VAL;
}

static Value sdl_is_key_down_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("sdl_is_key_down(scancode)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "sdl_is_key_down(scancode)", 1, hasError))
        return NIL_VAL;

    pump_events();
    const Uint8 *keyboard = SDL_GetKeyboardState(NULL);
    int scan = as_int(argv[0]);
    if (scan < 0 || scan >= SDL_NUM_SCANCODES)
        return BOOL_VAL(false);

    return BOOL_VAL(keyboard[scan] != 0);
}

bool dotk_init_module(const DotKNativeApi *api)
{
    if (api == NULL || api->version != DOTK_NATIVE_API_VERSION)
        return false;

    g_api = api;

    api->defineNative("sdl_init_window", sdl_init_window_native);
    api->defineNative("sdl_close_window", sdl_close_window_native);
    api->defineNative("sdl_set_target_fps", sdl_set_target_fps_native);
    api->defineNative("sdl_window_should_close", sdl_window_should_close_native);
    api->defineNative("sdl_begin_frame", sdl_begin_frame_native);
    api->defineNative("sdl_end_frame", sdl_end_frame_native);
    api->defineNative("sdl_draw_rect", sdl_draw_rect_native);
    api->defineNative("sdl_is_key_down", sdl_is_key_down_native);

    api->defineGlobalValue("SDL_SCANCODE_LEFT", NUM_VAL((double)SDL_SCANCODE_LEFT));
    api->defineGlobalValue("SDL_SCANCODE_RIGHT", NUM_VAL((double)SDL_SCANCODE_RIGHT));
    api->defineGlobalValue("SDL_SCANCODE_UP", NUM_VAL((double)SDL_SCANCODE_UP));
    api->defineGlobalValue("SDL_SCANCODE_DOWN", NUM_VAL((double)SDL_SCANCODE_DOWN));
    api->defineGlobalValue("SDL_SCANCODE_ESCAPE", NUM_VAL((double)SDL_SCANCODE_ESCAPE));
    api->defineGlobalValue("SDL_SCANCODE_SPACE", NUM_VAL((double)SDL_SCANCODE_SPACE));

    return true;
}
