#include "../src/include/native_api.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const DotKNativeApi *g_api = NULL;

typedef struct
{
    float x;
    float y;
    float z;
} ObjVert;

typedef struct
{
    int a;
    int b;
} ObjEdge;

typedef struct ObjModelCache
{
    char *path;
    ObjVert *verts;
    int vertCount;
    ObjEdge *edges;
    int edgeCount;
    struct ObjModelCache *next;
} ObjModelCache;

static ObjModelCache *g_objCache = NULL;

static Display *g_display = NULL;
static Window g_window = 0;
static GC g_gc = 0;
static bool g_shouldClose = false;
static int g_targetFps = 60;
static int g_mouseX = 0;
static int g_mouseY = 0;
static bool g_mouseDown[8] = {false};
static bool g_mouseClicked[8] = {false};
static char *g_lastRunOutput = NULL;
static int g_lastRunExitCode = -1;
static char g_runFlags[256] = "";
static pid_t g_debugPid = -1;
static int g_debugInFd = -1;
static int g_debugOutFd = -1;
static bool g_debugRunning = false;

static void set_last_run_output(const char *text)
{
    if (g_lastRunOutput != NULL)
    {
        free(g_lastRunOutput);
        g_lastRunOutput = NULL;
    }

    if (text == NULL)
        text = "";

    size_t len = strlen(text);
    g_lastRunOutput = (char *)malloc(len + 1);
    if (g_lastRunOutput == NULL)
        return;
    memcpy(g_lastRunOutput, text, len + 1);
}

static void append_last_run_output(const char *text)
{
    if (text == NULL || text[0] == '\0')
        return;

    if (g_lastRunOutput == NULL)
    {
        set_last_run_output(text);
        return;
    }

    size_t oldLen = strlen(g_lastRunOutput);
    size_t addLen = strlen(text);
    char *grown = (char *)realloc(g_lastRunOutput, oldLen + addLen + 1);
    if (grown == NULL)
        return;
    g_lastRunOutput = grown;
    memcpy(g_lastRunOutput + oldLen, text, addLen + 1);
}

static void debug_close_pipes(void)
{
    if (g_debugInFd >= 0)
    {
        close(g_debugInFd);
        g_debugInFd = -1;
    }
    if (g_debugOutFd >= 0)
    {
        close(g_debugOutFd);
        g_debugOutFd = -1;
    }
}

static void debug_poll_output(void);

static void debug_refresh_state(void)
{
    if (!g_debugRunning || g_debugPid <= 0)
        return;

    int status = 0;
    pid_t r = waitpid(g_debugPid, &status, WNOHANG);
    if (r == 0)
        return;

    if (r == g_debugPid)
    {
        if (WIFEXITED(status))
            g_lastRunExitCode = WEXITSTATUS(status);
        else if (WIFSIGNALED(status))
            g_lastRunExitCode = 128 + WTERMSIG(status);
        else
            g_lastRunExitCode = -1;
    }

    g_debugRunning = false;
    g_debugPid = -1;
    debug_close_pipes();
}

static bool debug_wait_for_exit(int timeoutMs)
{
    if (!g_debugRunning || g_debugPid <= 0)
        return true;

    int waited = 0;
    while (waited <= timeoutMs)
    {
        debug_poll_output();
        debug_refresh_state();
        if (!g_debugRunning || g_debugPid <= 0)
            return true;

        usleep(10000);
        waited += 10;
    }

    return false;
}

static void debug_poll_output(void)
{
    if (g_debugOutFd < 0)
        return;

    char buf[1024];
    for (;;)
    {
        ssize_t n = read(g_debugOutFd, buf, sizeof(buf) - 1);
        if (n > 0)
        {
            buf[n] = '\0';
            append_last_run_output(buf);
            continue;
        }
        if (n == 0)
        {
            debug_refresh_state();
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;

        debug_refresh_state();
        return;
    }
}

static void debug_stop_process(void)
{
    if (!g_debugRunning || g_debugPid <= 0)
        return;

    if (g_debugInFd >= 0)
    {
        ssize_t wrote = write(g_debugInFd, "q\n", 2);
        (void)wrote;

        close(g_debugInFd);
        g_debugInFd = -1;
    }

    if (debug_wait_for_exit(300))
        return;

    if (g_debugRunning && g_debugPid > 0)
    {
        kill(g_debugPid, SIGTERM);
        if (debug_wait_for_exit(700))
            return;
    }

    if (g_debugRunning && g_debugPid > 0)
    {
        kill(g_debugPid, SIGKILL);
        if (debug_wait_for_exit(700))
            return;

        int status = 0;
        pid_t reaped = waitpid(g_debugPid, &status, WNOHANG);
        if (reaped == g_debugPid)
        {
            if (WIFEXITED(status))
                g_lastRunExitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                g_lastRunExitCode = 128 + WTERMSIG(status);
            else
                g_lastRunExitCode = -1;
        }

        g_debugRunning = false;
        g_debugPid = -1;
        debug_close_pipes();
    }
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

static unsigned long rgb_to_pixel(int r, int g, int b)
{
    return ((unsigned long)(r & 0xff) << 16) | ((unsigned long)(g & 0xff) << 8) | (unsigned long)(b & 0xff);
}

static bool ensure_window(const char *name, bool *hasError)
{
    if (g_display != NULL && g_window != 0 && g_gc != 0)
        return true;

    g_api->raiseError("%s requires an initialized X11 window. Call x11_init_window(...) first.", name);
    *hasError = true;
    return false;
}

static void pump_events(void)
{
    if (g_display == NULL)
        return;

    while (XPending(g_display) > 0)
    {
        XEvent event;
        XNextEvent(g_display, &event);
        if (event.type == ClientMessage || event.type == DestroyNotify)
            g_shouldClose = true;
        else if (event.type == MotionNotify)
        {
            g_mouseX = event.xmotion.x;
            g_mouseY = event.xmotion.y;
        }
        else if (event.type == ButtonPress)
        {
            g_mouseX = event.xbutton.x;
            g_mouseY = event.xbutton.y;
            int b = event.xbutton.button;
            if (b >= 0 && b < 8)
            {
                g_mouseDown[b] = true;
                g_mouseClicked[b] = true;
            }
        }
        else if (event.type == ButtonRelease)
        {
            g_mouseX = event.xbutton.x;
            g_mouseY = event.xbutton.y;
            int b = event.xbutton.button;
            if (b >= 0 && b < 8)
                g_mouseDown[b] = false;
        }
    }
}

static Value x11_init_window_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_init_window(width, height, title)", argc, 3, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "x11_init_window(width, height, title)", 1, hasError) ||
        !expect_number(argv[1], "x11_init_window(width, height, title)", 2, hasError) ||
        !expect_string(argv[2], "x11_init_window(width, height, title)", 3, hasError))
        return NIL_VAL;

    g_display = XOpenDisplay(NULL);
    if (g_display == NULL)
    {
        g_api->raiseError("x11_init_window failed: could not open DISPLAY");
        *hasError = true;
        return BOOL_VAL(false);
    }

    int screen = DefaultScreen(g_display);
    int width = as_int(argv[0]);
    int height = as_int(argv[1]);

    g_window = XCreateSimpleWindow(
        g_display,
        RootWindow(g_display, screen),
        100,
        100,
        (unsigned int)width,
        (unsigned int)height,
        1,
        BlackPixel(g_display, screen),
        WhitePixel(g_display, screen));

    if (g_window == 0)
    {
        XCloseDisplay(g_display);
        g_display = NULL;
        g_api->raiseError("x11_init_window failed: could not create X11 window");
        *hasError = true;
        return BOOL_VAL(false);
    }

    XStoreName(g_display, g_window, AS_CSTR(argv[2]));
    XSelectInput(g_display,
                 g_window,
                 ExposureMask |
                     KeyPressMask |
                     KeyReleaseMask |
                     StructureNotifyMask |
                     ButtonPressMask |
                     ButtonReleaseMask |
                     PointerMotionMask);

    Atom wmDelete = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(g_display, g_window, &wmDelete, 1);

    g_gc = XCreateGC(g_display, g_window, 0, NULL);
    if (g_gc == 0)
    {
        XDestroyWindow(g_display, g_window);
        XCloseDisplay(g_display);
        g_window = 0;
        g_display = NULL;
        g_api->raiseError("x11_init_window failed: could not create graphics context");
        *hasError = true;
        return BOOL_VAL(false);
    }

    XMapWindow(g_display, g_window);
    XFlush(g_display);

    g_shouldClose = false;
    g_mouseX = 0;
    g_mouseY = 0;
    for (int i = 0; i < 8; i++)
    {
        g_mouseDown[i] = false;
        g_mouseClicked[i] = false;
    }
    return BOOL_VAL(true);
}

static Value x11_close_window_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_close_window()", argc, 0, hasError))
        return NIL_VAL;

    if (g_display != NULL)
    {
        if (g_gc != 0)
        {
            XFreeGC(g_display, g_gc);
            g_gc = 0;
        }
        if (g_window != 0)
        {
            XDestroyWindow(g_display, g_window);
            g_window = 0;
        }
        XCloseDisplay(g_display);
        g_display = NULL;
    }

    g_shouldClose = true;
    g_mouseX = 0;
    g_mouseY = 0;
    for (int i = 0; i < 8; i++)
    {
        g_mouseDown[i] = false;
        g_mouseClicked[i] = false;
    }
    if (g_lastRunOutput != NULL)
    {
        free(g_lastRunOutput);
        g_lastRunOutput = NULL;
    }
    if (g_objCache != NULL)
    {
        ObjModelCache *cache = g_objCache;
        while (cache != NULL)
        {
            ObjModelCache *next = cache->next;
            free(cache->path);
            free(cache->verts);
            free(cache->edges);
            free(cache);
            cache = next;
        }
        g_objCache = NULL;
    }
    return NIL_VAL;
}

static Value x11_set_target_fps_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_set_target_fps(fps)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "x11_set_target_fps(fps)", 1, hasError))
        return NIL_VAL;

    int fps = as_int(argv[0]);
    if (fps < 1)
        fps = 1;
    g_targetFps = fps;
    return NIL_VAL;
}

static Value x11_window_should_close_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_window_should_close()", argc, 0, hasError))
        return NIL_VAL;

    pump_events();
    return BOOL_VAL(g_shouldClose);
}

static Value x11_begin_frame_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_begin_frame(r, g, b)", argc, 3, hasError))
        return NIL_VAL;
    for (int i = 0; i < 3; i++)
    {
        if (!expect_number(argv[i], "x11_begin_frame(r, g, b)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window("x11_begin_frame", hasError))
        return NIL_VAL;

    pump_events();

    XSetForeground(g_display, g_gc, rgb_to_pixel(as_int(argv[0]), as_int(argv[1]), as_int(argv[2])));

    XWindowAttributes attrs;
    XGetWindowAttributes(g_display, g_window, &attrs);
    XFillRectangle(g_display, g_window, g_gc, 0, 0, (unsigned int)attrs.width, (unsigned int)attrs.height);

    return NIL_VAL;
}

static Value x11_end_frame_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_end_frame()", argc, 0, hasError))
        return NIL_VAL;
    if (!ensure_window("x11_end_frame", hasError))
        return NIL_VAL;

    XFlush(g_display);
    if (g_targetFps > 0)
        usleep((useconds_t)(1000000 / g_targetFps));

    return NIL_VAL;
}

static Value x11_draw_rect_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_draw_rect(x, y, w, h, r, g, b)", argc, 7, hasError))
        return NIL_VAL;
    for (int i = 0; i < 7; i++)
    {
        if (!expect_number(argv[i], "x11_draw_rect(x, y, w, h, r, g, b)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window("x11_draw_rect", hasError))
        return NIL_VAL;

    XSetForeground(g_display, g_gc, rgb_to_pixel(as_int(argv[4]), as_int(argv[5]), as_int(argv[6])));
    XFillRectangle(
        g_display,
        g_window,
        g_gc,
        as_int(argv[0]),
        as_int(argv[1]),
        (unsigned int)as_int(argv[2]),
        (unsigned int)as_int(argv[3]));

    return NIL_VAL;
}

static Value x11_draw_line_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_draw_line(x0, y0, x1, y1, r, g, b)", argc, 7, hasError))
        return NIL_VAL;
    for (int i = 0; i < 7; i++)
    {
        if (!expect_number(argv[i], "x11_draw_line(x0, y0, x1, y1, r, g, b)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window("x11_draw_line", hasError))
        return NIL_VAL;

    XSetForeground(g_display, g_gc, rgb_to_pixel(as_int(argv[4]), as_int(argv[5]), as_int(argv[6])));
    XDrawLine(g_display,
              g_window,
              g_gc,
              as_int(argv[0]),
              as_int(argv[1]),
              as_int(argv[2]),
              as_int(argv[3]));
    return NIL_VAL;
}

static int edge_cmp(const void *lhs, const void *rhs)
{
    const ObjEdge *a = (const ObjEdge *)lhs;
    const ObjEdge *b = (const ObjEdge *)rhs;
    if (a->a != b->a)
        return a->a < b->a ? -1 : 1;
    if (a->b != b->b)
        return a->b < b->b ? -1 : 1;
    return 0;
}

static bool grow_verts(ObjVert **arr, int *cap, int minCap)
{
    if (*cap >= minCap)
        return true;
    int next = *cap == 0 ? 256 : *cap;
    while (next < minCap)
        next *= 2;
    ObjVert *grown = (ObjVert *)realloc(*arr, (size_t)next * sizeof(ObjVert));
    if (grown == NULL)
        return false;
    *arr = grown;
    *cap = next;
    return true;
}

static bool grow_edges(ObjEdge **arr, int *cap, int minCap)
{
    if (*cap >= minCap)
        return true;
    int next = *cap == 0 ? 512 : *cap;
    while (next < minCap)
        next *= 2;
    ObjEdge *grown = (ObjEdge *)realloc(*arr, (size_t)next * sizeof(ObjEdge));
    if (grown == NULL)
        return false;
    *arr = grown;
    *cap = next;
    return true;
}

static int parse_obj_index_token(const char *tok, int vertCount)
{
    if (tok == NULL || *tok == '\0')
        return -1;

    char *end = NULL;
    long idx = strtol(tok, &end, 10);
    if (end == tok)
        return -1;

    if (idx < 0)
        idx = (long)vertCount + idx;
    else
        idx = idx - 1;

    if (idx < 0 || idx >= vertCount)
        return -1;
    return (int)idx;
}

static ObjModelCache *find_obj_cache(const char *path)
{
    for (ObjModelCache *it = g_objCache; it != NULL; it = it->next)
    {
        if (strcmp(it->path, path) == 0)
            return it;
    }
    return NULL;
}

static ObjModelCache *load_obj_cache(const char *path, bool *hasError)
{
    ObjModelCache *cached = find_obj_cache(path);
    if (cached != NULL)
        return cached;

    FILE *f = fopen(path, "r");
    if (f == NULL)
    {
        g_api->raiseError("x11 OBJ load failed: cannot open '%s'", path);
        *hasError = true;
        return NULL;
    }

    ObjVert *verts = NULL;
    ObjEdge *edges = NULL;
    int vertCount = 0;
    int vertCap = 0;
    int edgeCount = 0;
    int edgeCap = 0;
    char line[4096];

    bool oom = false;
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *s = line;
        while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
            s++;
        if (*s == '\0' || *s == '#')
            continue;

        if (s[0] == 'v' && (s[1] == ' ' || s[1] == '\t'))
        {
            float x, y, z;
            if (sscanf(s + 1, "%f %f %f", &x, &y, &z) == 3)
            {
                if (!grow_verts(&verts, &vertCap, vertCount + 1))
                {
                    oom = true;
                    break;
                }
                verts[vertCount].x = x;
                verts[vertCount].y = y;
                verts[vertCount].z = z;
                vertCount++;
            }
            continue;
        }

        if (s[0] == 'f' && (s[1] == ' ' || s[1] == '\t'))
        {
            int faceCap = 16;
            int faceCount = 0;
            int *face = (int *)malloc((size_t)faceCap * sizeof(int));
            if (face == NULL)
            {
                oom = true;
                break;
            }

            char *save = NULL;
            char *tok = strtok_r(s + 1, " \t\r\n", &save);
            while (tok != NULL)
            {
                char *slash = strchr(tok, '/');
                if (slash != NULL)
                    *slash = '\0';

                int idx = parse_obj_index_token(tok, vertCount);
                if (idx >= 0)
                {
                    if (faceCount >= faceCap)
                    {
                        faceCap *= 2;
                        int *grown = (int *)realloc(face, (size_t)faceCap * sizeof(int));
                        if (grown == NULL)
                        {
                            free(face);
                            oom = true;
                            face = NULL;
                            break;
                        }
                        face = grown;
                    }
                    face[faceCount++] = idx;
                }
                tok = strtok_r(NULL, " \t\r\n", &save);
            }

            if (oom)
                break;

            if (faceCount >= 2)
            {
                for (int i = 0; i < faceCount; i++)
                {
                    int a = face[i];
                    int b = face[(i + 1) % faceCount];
                    if (a == b)
                        continue;
                    if (a > b)
                    {
                        int t = a;
                        a = b;
                        b = t;
                    }

                    if (!grow_edges(&edges, &edgeCap, edgeCount + 1))
                    {
                        free(face);
                        oom = true;
                        break;
                    }

                    edges[edgeCount].a = a;
                    edges[edgeCount].b = b;
                    edgeCount++;
                }
            }

            if (face != NULL)
                free(face);
            if (oom)
                break;
        }
    }
    fclose(f);

    if (oom)
    {
        free(verts);
        free(edges);
        g_api->raiseError("x11 OBJ load failed: out of memory");
        *hasError = true;
        return NULL;
    }

    if (vertCount <= 0 || edgeCount <= 0)
    {
        free(verts);
        free(edges);
        g_api->raiseError("x11 OBJ load failed: no usable geometry in '%s'", path);
        *hasError = true;
        return NULL;
    }

    qsort(edges, (size_t)edgeCount, sizeof(ObjEdge), edge_cmp);
    int uniq = 0;
    for (int i = 0; i < edgeCount; i++)
    {
        if (i == 0 || edge_cmp(&edges[i - 1], &edges[i]) != 0)
            edges[uniq++] = edges[i];
    }
    edgeCount = uniq;

    ObjModelCache *node = (ObjModelCache *)malloc(sizeof(ObjModelCache));
    if (node == NULL)
    {
        free(verts);
        free(edges);
        g_api->raiseError("x11 OBJ cache failed: out of memory");
        *hasError = true;
        return NULL;
    }

    size_t n = strlen(path);
    node->path = (char *)malloc(n + 1);
    if (node->path == NULL)
    {
        free(node);
        free(verts);
        free(edges);
        g_api->raiseError("x11 OBJ cache failed: out of memory");
        *hasError = true;
        return NULL;
    }
    memcpy(node->path, path, n + 1);
    node->verts = verts;
    node->vertCount = vertCount;
    node->edges = edges;
    node->edgeCount = edgeCount;
    node->next = g_objCache;
    g_objCache = node;
    return node;
}

static int screen_out_code(double x, double y, int width, int height)
{
    int code = 0;
    if (x < 0)
        code |= 1;
    if (x >= width)
        code |= 2;
    if (y < 0)
        code |= 4;
    if (y >= height)
        code |= 8;
    return code;
}

static Value x11_draw_obj_wire_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_draw_obj_wire(path, px, py, pz, scale, rx, ry, camX, camY, camZ, camRotX, camRotY, fov, r, g, b)", argc, 16, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_draw_obj_wire(path,...)", 1, hasError))
        return NIL_VAL;
    for (int i = 1; i < 16; i++)
    {
        if (!expect_number(argv[i], "x11_draw_obj_wire(path,...)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window("x11_draw_obj_wire", hasError))
        return NIL_VAL;

    const char *path = AS_CSTR(argv[0]);
    ObjModelCache *model = load_obj_cache(path, hasError);
    if (model == NULL)
        return NIL_VAL;

    double posX = AS_NUM(argv[1]);
    double posY = AS_NUM(argv[2]);
    double posZ = AS_NUM(argv[3]);
    double scale = AS_NUM(argv[4]);
    double rotX = AS_NUM(argv[5]);
    double rotY = AS_NUM(argv[6]);
    double camX = AS_NUM(argv[7]);
    double camY = AS_NUM(argv[8]);
    double camZ = AS_NUM(argv[9]);
    double camRotX = AS_NUM(argv[10]);
    double camRotY = AS_NUM(argv[11]);
    double fov = AS_NUM(argv[12]);
    int cr = as_int(argv[13]);
    int cg = as_int(argv[14]);
    int cb = as_int(argv[15]);

    XWindowAttributes attrs;
    XGetWindowAttributes(g_display, g_window, &attrs);
    int width = attrs.width;
    int height = attrs.height;

    double cy = cos(rotY);
    double sy = sin(rotY);
    double cx = cos(rotX);
    double sx = sin(rotX);
    double ccy = cos(camRotY);
    double csy = sin(camRotY);
    double ccx = cos(camRotX);
    double csx = sin(camRotX);

    double *sxv = (double *)malloc((size_t)model->vertCount * sizeof(double));
    double *syv = (double *)malloc((size_t)model->vertCount * sizeof(double));
    double *szv = (double *)malloc((size_t)model->vertCount * sizeof(double));
    uint8_t *okv = (uint8_t *)malloc((size_t)model->vertCount);
    if (sxv == NULL || syv == NULL || szv == NULL || okv == NULL)
    {
        free(sxv);
        free(syv);
        free(szv);
        free(okv);
        g_api->raiseError("x11_draw_obj_wire failed: out of memory");
        *hasError = true;
        return NIL_VAL;
    }

    for (int i = 0; i < model->vertCount; i++)
    {
        ObjVert v = model->verts[i];
        double lx = (double)v.x * scale;
        double ly = (double)v.y * scale;
        double lz = (double)v.z * scale;

        double x1 = (lx * cy) - (lz * sy);
        double z1 = (lx * sy) + (lz * cy);
        double y2 = (ly * cx) - (z1 * sx);
        double z2 = (ly * sx) + (z1 * cx);

        double wx = posX + x1;
        double wy = posY + y2;
        double wz = posZ + z2;

        double vx = wx - camX;
        double vy = wy - camY;
        double vz = wz - camZ;

        double vx1 = (vx * ccy) + (vz * csy);
        double vz1 = (-vx * csy) + (vz * ccy);
        double vy2 = (vy * ccx) + (vz1 * csx);
        double vz2 = (-vy * csx) + (vz1 * ccx);

        if (vz2 <= 0.2)
        {
            okv[i] = 0;
            continue;
        }

        sxv[i] = (width / 2.0) + ((vx1 * fov) / vz2);
        syv[i] = (height / 2.0) - ((vy2 * fov) / vz2);
        szv[i] = vz2;
        okv[i] = 1;
    }

    XSetForeground(g_display, g_gc, rgb_to_pixel(cr, cg, cb));
    for (int i = 0; i < model->edgeCount; i++)
    {
        int a = model->edges[i].a;
        int b = model->edges[i].b;
        if (!okv[a] || !okv[b])
            continue;

        int codeA = screen_out_code(sxv[a], syv[a], width, height);
        int codeB = screen_out_code(sxv[b], syv[b], width, height);
        if ((codeA & codeB) != 0)
            continue;

        XDrawLine(g_display,
                  g_window,
                  g_gc,
                  (int)sxv[a],
                  (int)syv[a],
                  (int)sxv[b],
                  (int)syv[b]);
    }

    free(sxv);
    free(syv);
    free(szv);
    free(okv);
    return NIL_VAL;
}

static Value x11_load_obj_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_load_obj(path)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_load_obj(path)", 1, hasError))
        return NIL_VAL;

    ObjModelCache *modelData = load_obj_cache(AS_CSTR(argv[0]), hasError);
    if (modelData == NULL)
        return NIL_VAL;

    Value vertsList = g_api->makeList();
    g_api->pushValue(vertsList);
    for (int i = 0; i < modelData->vertCount; i++)
    {
        Value triple = g_api->makeList();
        g_api->pushValue(triple);
        g_api->listAppend(triple, NUM_VAL((double)modelData->verts[i].x));
        g_api->listAppend(triple, NUM_VAL((double)modelData->verts[i].y));
        g_api->listAppend(triple, NUM_VAL((double)modelData->verts[i].z));
        g_api->listAppend(vertsList, triple);
        g_api->popValue();
    }

    Value edgesList = g_api->makeList();
    g_api->pushValue(edgesList);
    for (int i = 0; i < modelData->edgeCount; i++)
    {
        Value pair = g_api->makeList();
        g_api->pushValue(pair);
        g_api->listAppend(pair, NUM_VAL((double)modelData->edges[i].a));
        g_api->listAppend(pair, NUM_VAL((double)modelData->edges[i].b));
        g_api->listAppend(edgesList, pair);
        g_api->popValue();
    }

    Value model = g_api->makeMap();
    g_api->pushValue(model);
    Value keyVerts = g_api->makeString("verts", 5, true);
    Value keyEdges = g_api->makeString("edges", 5, true);
    g_api->mapSet(model, keyVerts, vertsList);
    g_api->mapSet(model, keyEdges, edgesList);

    g_api->popValue(); // model
    g_api->popValue(); // edgesList
    g_api->popValue(); // vertsList
    return model;
}

static Value x11_draw_circle_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_draw_circle(cx, cy, radius, r, g, b)", argc, 6, hasError))
        return NIL_VAL;
    for (int i = 0; i < 6; i++)
    {
        if (!expect_number(argv[i], "x11_draw_circle(cx, cy, radius, r, g, b)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window("x11_draw_circle", hasError))
        return NIL_VAL;

    int radius = as_int(argv[2]);
    if (radius < 1)
        radius = 1;

    int x = as_int(argv[0]) - radius;
    int y = as_int(argv[1]) - radius;
    unsigned int d = (unsigned int)(radius * 2);

    XSetForeground(g_display, g_gc, rgb_to_pixel(as_int(argv[3]), as_int(argv[4]), as_int(argv[5])));
    XFillArc(g_display, g_window, g_gc, x, y, d, d, 0, 360 * 64);

    return NIL_VAL;
}

static Value x11_draw_text_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_draw_text(text, x, y, r, g, b)", argc, 6, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_draw_text(text, x, y, r, g, b)", 1, hasError))
        return NIL_VAL;
    for (int i = 1; i < 6; i++)
    {
        if (!expect_number(argv[i], "x11_draw_text(text, x, y, r, g, b)", i + 1, hasError))
            return NIL_VAL;
    }
    if (!ensure_window("x11_draw_text", hasError))
        return NIL_VAL;

    const char *text = AS_CSTR(argv[0]);
    int len = (int)strlen(text);

    XSetForeground(g_display, g_gc, rgb_to_pixel(as_int(argv[3]), as_int(argv[4]), as_int(argv[5])));
    XDrawString(g_display, g_window, g_gc, as_int(argv[1]), as_int(argv[2]), text, len);

    return NIL_VAL;
}

static Value x11_is_key_down_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_is_key_down(keysym)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "x11_is_key_down(keysym)", 1, hasError))
        return NIL_VAL;
    if (!ensure_window("x11_is_key_down", hasError))
        return NIL_VAL;

    pump_events();

    char keymap[32];
    XQueryKeymap(g_display, keymap);

    KeySym ks = (KeySym)as_int(argv[0]);
    KeyCode code = XKeysymToKeycode(g_display, ks);
    if (code == 0)
        return BOOL_VAL(false);

    int idx = code / 8;
    int bit = code % 8;
    bool down = (keymap[idx] & (1 << bit)) != 0;
    return BOOL_VAL(down);
}

static Value x11_mouse_x_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_mouse_x()", argc, 0, hasError))
        return NIL_VAL;
    if (!ensure_window("x11_mouse_x", hasError))
        return NIL_VAL;

    pump_events();
    return NUM_VAL((double)g_mouseX);
}

static Value x11_mouse_y_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_mouse_y()", argc, 0, hasError))
        return NIL_VAL;
    if (!ensure_window("x11_mouse_y", hasError))
        return NIL_VAL;

    pump_events();
    return NUM_VAL((double)g_mouseY);
}

static Value x11_mouse_down_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_mouse_down(button)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "x11_mouse_down(button)", 1, hasError))
        return NIL_VAL;
    if (!ensure_window("x11_mouse_down", hasError))
        return NIL_VAL;

    pump_events();

    int b = as_int(argv[0]);
    if (b < 0 || b >= 8)
        return BOOL_VAL(false);
    return BOOL_VAL(g_mouseDown[b]);
}

static Value x11_mouse_clicked_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_mouse_clicked(button)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_number(argv[0], "x11_mouse_clicked(button)", 1, hasError))
        return NIL_VAL;
    if (!ensure_window("x11_mouse_clicked", hasError))
        return NIL_VAL;

    pump_events();

    int b = as_int(argv[0]);
    if (b < 0 || b >= 8)
        return BOOL_VAL(false);

    bool clicked = g_mouseClicked[b];
    g_mouseClicked[b] = false;
    return BOOL_VAL(clicked);
}

static Value x11_time_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_time()", argc, 0, hasError))
        return NIL_VAL;

    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0)
        return NUM_VAL(0);

    double t = (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0);
    return NUM_VAL(t);
}

static Value x11_list_dotk_files_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_list_dotk_files(dir)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_list_dotk_files(dir)", 1, hasError))
        return NIL_VAL;

    const char *dirPath = AS_CSTR(argv[0]);
    DIR *dir = opendir(dirPath);
    if (dir == NULL)
    {
        g_api->raiseError("x11_list_dotk_files failed: could not open directory '%s'", dirPath);
        *hasError = true;
        return NIL_VAL;
    }

    Value list = g_api->makeList();
    g_api->pushValue(list);

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        const char *name = ent->d_name;
        size_t len = strlen(name);
        if (len < 3)
            continue;
        if (!(name[len - 2] == '.' && name[len - 1] == 'k'))
            continue;

        Value item = g_api->makeString(name, (int)len, false);
        g_api->pushValue(item);
        g_api->listAppend(list, item);
        g_api->popValue();
    }
    closedir(dir);

    return g_api->popValue();
}

static Value x11_run_dotk_file_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_run_dotk_file(path)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_run_dotk_file(path)", 1, hasError))
        return NIL_VAL;

    const char *path = AS_CSTR(argv[0]);
    char command[4096];
    if (g_runFlags[0] != '\0')
        snprintf(command, sizeof(command), "./dotk.out %s \"%s\" 2>&1", g_runFlags, path);
    else
        snprintf(command, sizeof(command), "./dotk.out \"%s\" 2>&1", path);

    FILE *pipe = popen(command, "r");
    if (pipe == NULL)
    {
        g_api->raiseError("x11_run_dotk_file failed: could not execute command");
        *hasError = true;
        return NIL_VAL;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buffer = (char *)malloc(cap);
    if (buffer == NULL)
    {
        pclose(pipe);
        g_api->raiseError("x11_run_dotk_file failed: out of memory");
        *hasError = true;
        return NIL_VAL;
    }
    buffer[0] = '\0';

    char chunk[1024];
    while (fgets(chunk, sizeof(chunk), pipe) != NULL)
    {
        size_t n = strlen(chunk);
        if (len + n + 1 > cap)
        {
            while (len + n + 1 > cap)
                cap *= 2;
            char *grown = (char *)realloc(buffer, cap);
            if (grown == NULL)
                break;
            buffer = grown;
        }
        memcpy(buffer + len, chunk, n);
        len += n;
        buffer[len] = '\0';
    }

    int status = pclose(pipe);
    if (WIFEXITED(status))
        g_lastRunExitCode = WEXITSTATUS(status);
    else
        g_lastRunExitCode = -1;

    set_last_run_output(buffer);
    free(buffer);

    return BOOL_VAL(g_lastRunExitCode == 0);
}

static void escape_for_dquote(const char *src, char *dst, size_t dstCap)
{
    if (dstCap == 0)
        return;
    size_t j = 0;
    for (size_t i = 0; src[i] != '\0' && j + 1 < dstCap; i++)
    {
        char c = src[i];
        if ((c == '\\' || c == '"' || c == '$' || c == '`') && j + 2 < dstCap)
            dst[j++] = '\\';
        dst[j++] = c;
    }
    dst[j] = '\0';
}

static Value x11_run_dotk_debug_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_run_dotk_debug(path, commands)", argc, 2, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_run_dotk_debug(path, commands)", 1, hasError) ||
        !expect_string(argv[1], "x11_run_dotk_debug(path, commands)", 2, hasError))
        return NIL_VAL;

    const char *path = AS_CSTR(argv[0]);
    const char *commands = AS_CSTR(argv[1]);

    char escPath[2048];
    char escCmds[2048];
    escape_for_dquote(path, escPath, sizeof(escPath));
    escape_for_dquote(commands, escCmds, sizeof(escCmds));

    char command[8192];
    if (g_runFlags[0] != '\0')
        snprintf(command,
                 sizeof(command),
                 "DOTK_DEBUG_CMDS=\"%s\" ./dotk.out --debug %s \"%s\" 2>&1",
                 escCmds,
                 g_runFlags,
                 escPath);
    else
        snprintf(command,
                 sizeof(command),
                 "DOTK_DEBUG_CMDS=\"%s\" ./dotk.out --debug \"%s\" 2>&1",
                 escCmds,
                 escPath);

    FILE *pipe = popen(command, "r");
    if (pipe == NULL)
    {
        g_api->raiseError("x11_run_dotk_debug failed: could not execute command");
        *hasError = true;
        return NIL_VAL;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *buffer = (char *)malloc(cap);
    if (buffer == NULL)
    {
        pclose(pipe);
        g_api->raiseError("x11_run_dotk_debug failed: out of memory");
        *hasError = true;
        return NIL_VAL;
    }
    buffer[0] = '\0';

    char chunk[1024];
    while (fgets(chunk, sizeof(chunk), pipe) != NULL)
    {
        size_t n = strlen(chunk);
        if (len + n + 1 > cap)
        {
            while (len + n + 1 > cap)
                cap *= 2;
            char *grown = (char *)realloc(buffer, cap);
            if (grown == NULL)
                break;
            buffer = grown;
        }
        memcpy(buffer + len, chunk, n);
        len += n;
        buffer[len] = '\0';
    }

    int status = pclose(pipe);
    if (WIFEXITED(status))
        g_lastRunExitCode = WEXITSTATUS(status);
    else
        g_lastRunExitCode = -1;

    set_last_run_output(buffer);
    free(buffer);

    return BOOL_VAL(g_lastRunExitCode == 0);
}

static Value x11_last_run_output_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_last_run_output()", argc, 0, hasError))
        return NIL_VAL;

    const char *out = g_lastRunOutput != NULL ? g_lastRunOutput : "";
    return g_api->makeString(out, (int)strlen(out), false);
}

static Value x11_last_run_exit_code_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_last_run_exit_code()", argc, 0, hasError))
        return NIL_VAL;

    return NUM_VAL((double)g_lastRunExitCode);
}

static Value x11_set_run_flags_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_set_run_flags(flags)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_set_run_flags(flags)", 1, hasError))
        return NIL_VAL;

    const char *flags = AS_CSTR(argv[0]);
    strncpy(g_runFlags, flags, sizeof(g_runFlags) - 1);
    g_runFlags[sizeof(g_runFlags) - 1] = '\0';
    return NIL_VAL;
}

static Value x11_get_run_flags_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_get_run_flags()", argc, 0, hasError))
        return NIL_VAL;
    return g_api->makeString(g_runFlags, (int)strlen(g_runFlags), false);
}

static Value x11_read_text_file_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_read_text_file(path)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_read_text_file(path)", 1, hasError))
        return NIL_VAL;

    const char *path = AS_CSTR(argv[0]);
    FILE *f = fopen(path, "rb");
    if (f == NULL)
    {
        g_api->raiseError("x11_read_text_file failed: cannot open '%s'", path);
        *hasError = true;
        return NIL_VAL;
    }

    fseek(f, 0L, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size < 0)
        size = 0;

    char *buffer = (char *)malloc((size_t)size + 1);
    if (buffer == NULL)
    {
        fclose(f);
        g_api->raiseError("x11_read_text_file failed: out of memory");
        *hasError = true;
        return NIL_VAL;
    }

    size_t readN = fread(buffer, 1, (size_t)size, f);
    buffer[readN] = '\0';
    fclose(f);

    Value out = g_api->makeString(buffer, (int)readN, false);
    free(buffer);
    return out;
}

static Value x11_debug_start_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_debug_start(path)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_debug_start(path)", 1, hasError))
        return NIL_VAL;

    debug_stop_process();

    int inPipe[2];
    int outPipe[2];
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0)
    {
        g_api->raiseError("x11_debug_start failed: could not create pipes");
        *hasError = true;
        return NIL_VAL;
    }

    const char *path = AS_CSTR(argv[0]);
    char escPath[2048];
    escape_for_dquote(path, escPath, sizeof(escPath));

    char command[4096];
    if (g_runFlags[0] != '\0')
        snprintf(command, sizeof(command), "./dotk.out --debug %s \"%s\"", g_runFlags, escPath);
    else
        snprintf(command, sizeof(command), "./dotk.out --debug \"%s\"", escPath);

    pid_t pid = fork();
    if (pid < 0)
    {
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);
        g_api->raiseError("x11_debug_start failed: fork error");
        *hasError = true;
        return NIL_VAL;
    }

    if (pid == 0)
    {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(outPipe[1], STDERR_FILENO);

        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    close(inPipe[0]);
    close(outPipe[1]);

    int flags = fcntl(outPipe[0], F_GETFL, 0);
    if (flags >= 0)
        fcntl(outPipe[0], F_SETFL, flags | O_NONBLOCK);

    g_debugPid = pid;
    g_debugInFd = inPipe[1];
    g_debugOutFd = outPipe[0];
    g_debugRunning = true;

    g_lastRunExitCode = -1;
    set_last_run_output("");
    usleep(20000);
    debug_poll_output();

    return BOOL_VAL(true);
}

static Value x11_debug_send_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;

    if (!expect_argc("x11_debug_send(command)", argc, 1, hasError))
        return NIL_VAL;
    if (!expect_string(argv[0], "x11_debug_send(command)", 1, hasError))
        return NIL_VAL;

    debug_refresh_state();
    if (!g_debugRunning || g_debugInFd < 0)
    {
        g_api->raiseError("x11_debug_send failed: no active debug session");
        *hasError = true;
        return NIL_VAL;
    }

    const char *cmd = AS_CSTR(argv[0]);
    size_t len = strlen(cmd);
    if (len > 0)
    {
        ssize_t wrote = write(g_debugInFd, cmd, len);
        (void)wrote;
    }
    {
        ssize_t wrote = write(g_debugInFd, "\n", 1);
        (void)wrote;
    }

    usleep(30000);
    debug_poll_output();
    debug_refresh_state();
    return BOOL_VAL(true);
}

static Value x11_debug_poll_output_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_debug_poll_output()", argc, 0, hasError))
        return NIL_VAL;

    debug_poll_output();
    debug_refresh_state();

    const char *out = g_lastRunOutput != NULL ? g_lastRunOutput : "";
    return g_api->makeString(out, (int)strlen(out), false);
}

static Value x11_debug_stop_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_debug_stop()", argc, 0, hasError))
        return NIL_VAL;

    debug_stop_process();
    return BOOL_VAL(true);
}

static Value x11_debug_is_running_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)argv;
    (void)pushedValue;

    if (!expect_argc("x11_debug_is_running()", argc, 0, hasError))
        return NIL_VAL;

    debug_refresh_state();
    return BOOL_VAL(g_debugRunning);
}

bool dotk_init_module(const DotKNativeApi *api)
{
    if (api == NULL || api->version != DOTK_NATIVE_API_VERSION)
        return false;

    g_api = api;

    api->defineNative("x11_init_window", x11_init_window_native);
    api->defineNative("x11_close_window", x11_close_window_native);
    api->defineNative("x11_set_target_fps", x11_set_target_fps_native);
    api->defineNative("x11_window_should_close", x11_window_should_close_native);
    api->defineNative("x11_begin_frame", x11_begin_frame_native);
    api->defineNative("x11_end_frame", x11_end_frame_native);
    api->defineNative("x11_draw_rect", x11_draw_rect_native);
    api->defineNative("x11_draw_line", x11_draw_line_native);
    api->defineNative("x11_draw_circle", x11_draw_circle_native);
    api->defineNative("x11_draw_text", x11_draw_text_native);
    api->defineNative("x11_is_key_down", x11_is_key_down_native);
    api->defineNative("x11_mouse_x", x11_mouse_x_native);
    api->defineNative("x11_mouse_y", x11_mouse_y_native);
    api->defineNative("x11_mouse_down", x11_mouse_down_native);
    api->defineNative("x11_mouse_clicked", x11_mouse_clicked_native);
    api->defineNative("x11_time", x11_time_native);
    api->defineNative("x11_list_dotk_files", x11_list_dotk_files_native);
    api->defineNative("x11_run_dotk_file", x11_run_dotk_file_native);
    api->defineNative("x11_run_dotk_debug", x11_run_dotk_debug_native);
    api->defineNative("x11_debug_start", x11_debug_start_native);
    api->defineNative("x11_debug_send", x11_debug_send_native);
    api->defineNative("x11_debug_poll_output", x11_debug_poll_output_native);
    api->defineNative("x11_debug_stop", x11_debug_stop_native);
    api->defineNative("x11_debug_is_running", x11_debug_is_running_native);
    api->defineNative("x11_last_run_output", x11_last_run_output_native);
    api->defineNative("x11_last_run_exit_code", x11_last_run_exit_code_native);
    api->defineNative("x11_set_run_flags", x11_set_run_flags_native);
    api->defineNative("x11_get_run_flags", x11_get_run_flags_native);
    api->defineNative("x11_read_text_file", x11_read_text_file_native);
    api->defineNative("x11_load_obj", x11_load_obj_native);
    api->defineNative("x11_draw_obj_wire", x11_draw_obj_wire_native);

    api->defineGlobalValue("X11_KEY_LEFT", NUM_VAL((double)XK_Left));
    api->defineGlobalValue("X11_KEY_RIGHT", NUM_VAL((double)XK_Right));
    api->defineGlobalValue("X11_KEY_UP", NUM_VAL((double)XK_Up));
    api->defineGlobalValue("X11_KEY_DOWN", NUM_VAL((double)XK_Down));
    api->defineGlobalValue("X11_KEY_W", NUM_VAL((double)XK_w));
    api->defineGlobalValue("X11_KEY_A", NUM_VAL((double)XK_a));
    api->defineGlobalValue("X11_KEY_S", NUM_VAL((double)XK_s));
    api->defineGlobalValue("X11_KEY_D", NUM_VAL((double)XK_d));
    api->defineGlobalValue("X11_KEY_Q", NUM_VAL((double)XK_q));
    api->defineGlobalValue("X11_KEY_E", NUM_VAL((double)XK_e));
    api->defineGlobalValue("X11_KEY_ESCAPE", NUM_VAL((double)XK_Escape));
    api->defineGlobalValue("X11_KEY_SPACE", NUM_VAL((double)XK_space));
    api->defineGlobalValue("X11_MOUSE_LEFT", NUM_VAL(1));
    api->defineGlobalValue("X11_MOUSE_MIDDLE", NUM_VAL(2));
    api->defineGlobalValue("X11_MOUSE_RIGHT", NUM_VAL(3));
    api->defineGlobalValue("X11_MOUSE_WHEEL_UP", NUM_VAL(4));
    api->defineGlobalValue("X11_MOUSE_WHEEL_DOWN", NUM_VAL(5));

    return true;
}
