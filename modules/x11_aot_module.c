#include "../src/include/aot_module_api.h"

#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct
{
    double x;
    double y;
    double z;
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

static Display *g_display = NULL;
static Window g_window = 0;
static GC g_gc = 0;
static int g_targetFps = 60;
static int g_mouseX = 0;
static int g_mouseY = 0;
#define X11_MOUSE_BUTTON_MAX 256
static unsigned char g_mouseDown[X11_MOUSE_BUTTON_MAX] = {0};
static unsigned char g_mouseClicked[X11_MOUSE_BUTTON_MAX] = {0};
static unsigned char g_keyDown[65536] = {0};
static int g_shouldClose = 0;
static Atom g_wmDeleteAtom = None;
static double g_lastFrame = 0.0;
static const DotkAotApi *g_aot_api = NULL;
static ObjModelCache *g_objCache = NULL;
static unsigned long g_clearPixel = 0;

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

static int grow_verts(ObjVert **arr, int *cap, int minCap)
{
    if (*cap >= minCap)
        return 1;
    int next = *cap == 0 ? 256 : *cap;
    while (next < minCap)
        next *= 2;
    ObjVert *grown = (ObjVert *)realloc(*arr, (size_t)next * sizeof(ObjVert));
    if (!grown)
        return 0;
    *arr = grown;
    *cap = next;
    return 1;
}

static int grow_edges(ObjEdge **arr, int *cap, int minCap)
{
    if (*cap >= minCap)
        return 1;
    int next = *cap == 0 ? 512 : *cap;
    while (next < minCap)
        next *= 2;
    ObjEdge *grown = (ObjEdge *)realloc(*arr, (size_t)next * sizeof(ObjEdge));
    if (!grown)
        return 0;
    *arr = grown;
    *cap = next;
    return 1;
}

static int parse_obj_index_token(const char *tok, int vertCount)
{
    if (!tok || *tok == '\0')
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

static ObjModelCache *load_obj_cache(const char *path)
{
    if (!path || !*path)
        return NULL;

    for (ObjModelCache *it = g_objCache; it != NULL; it = it->next)
    {
        if (strcmp(it->path, path) == 0)
            return it;
    }

    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    ObjVert *verts = NULL;
    ObjEdge *edges = NULL;
    int vertCap = 0;
    int edgeCap = 0;
    int vertCount = 0;
    int edgeCount = 0;
    int oom = 0;

    char line[4096];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;

        if (s[0] == 'v' && (s[1] == ' ' || s[1] == '\t'))
        {
            double x = 0.0, y = 0.0, z = 0.0;
            if (sscanf(s + 1, "%lf %lf %lf", &x, &y, &z) == 3)
            {
                if (!grow_verts(&verts, &vertCap, vertCount + 1))
                {
                    oom = 1;
                    break;
                }
                verts[vertCount++] = (ObjVert){x, y, z};
            }
            continue;
        }

        if (s[0] == 'f' && (s[1] == ' ' || s[1] == '\t'))
        {
            int faceCap = 16;
            int faceCount = 0;
            int *face = (int *)malloc((size_t)faceCap * sizeof(int));
            if (!face)
            {
                oom = 1;
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
                        if (!grown)
                        {
                            free(face);
                            face = NULL;
                            oom = 1;
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
                        oom = 1;
                        break;
                    }

                    edges[edgeCount++] = (ObjEdge){a, b};
                }
            }

            free(face);
            if (oom)
                break;
        }
    }
    fclose(f);

    if (oom || vertCount <= 0 || edgeCount <= 0)
    {
        free(verts);
        free(edges);
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
    if (!node)
    {
        free(verts);
        free(edges);
        return NULL;
    }

    size_t n = strlen(path);
    node->path = (char *)malloc(n + 1);
    if (!node->path)
    {
        free(node);
        free(verts);
        free(edges);
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

static unsigned long rgb(int r, int g, int b)
{
    if (r < 0)
        r = 0;
    if (r > 255)
        r = 255;
    if (g < 0)
        g = 0;
    if (g > 255)
        g = 255;
    if (b < 0)
        b = 0;
    if (b > 255)
        b = 255;
    return ((unsigned long)(r & 0xff) << 16) |
           ((unsigned long)(g & 0xff) << 8) |
           (unsigned long)(b & 0xff);
}

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

static void pump_events(void)
{
    if (!g_display)
        return;
    for (int i = 0; i < X11_MOUSE_BUTTON_MAX; i++)
        g_mouseClicked[i] = 0;
    while (XPending(g_display) > 0)
    {
        XEvent e;
        XNextEvent(g_display, &e);
        if (e.type == DestroyNotify)
        {
            g_shouldClose = 1;
        }
        else if (e.type == ClientMessage)
        {
            if (g_wmDeleteAtom != None && (Atom)e.xclient.data.l[0] == g_wmDeleteAtom)
                g_shouldClose = 1;
        }
        else if (e.type == MotionNotify)
        {
            g_mouseX = e.xmotion.x;
            g_mouseY = e.xmotion.y;
        }
        else if (e.type == ButtonPress)
        {
            int b = e.xbutton.button;
            g_mouseX = e.xbutton.x;
            g_mouseY = e.xbutton.y;
            if (b >= 0 && b < X11_MOUSE_BUTTON_MAX)
            {
                g_mouseDown[b] = 1;
                g_mouseClicked[b] = 1;
            }
        }
        else if (e.type == ButtonRelease)
        {
            int b = e.xbutton.button;
            g_mouseX = e.xbutton.x;
            g_mouseY = e.xbutton.y;
            if (b >= 0 && b < X11_MOUSE_BUTTON_MAX)
                g_mouseDown[b] = 0;
        }
        else if (e.type == KeyPress)
        {
            KeySym k = XLookupKeysym(&e.xkey, 0);
            if ((unsigned long)k < 65536UL)
                g_keyDown[(unsigned long)k] = 1;
            if (k == XK_Escape)
                g_shouldClose = 1;
        }
        else if (e.type == KeyRelease)
        {
            KeySym k = XLookupKeysym(&e.xkey, 0);
            if ((unsigned long)k < 65536UL)
                g_keyDown[(unsigned long)k] = 0;
        }
    }
}

static DotkAotValue v_num(double x)
{
    DotkAotValue v = {0};
    v.t = 0;
    v.n = x;
    return v;
}

static DotkAotValue v_str(const char *s)
{
    DotkAotValue v = {0};
    v.t = 1;
    v.s = s;
    return v;
}

static DotkAotValue v_bool(int b)
{
    DotkAotValue v = {0};
    v.t = 2;
    v.b = b ? 1 : 0;
    return v;
}

static DotkAotValue v_nil(void)
{
    DotkAotValue v = {0};
    v.t = 3;
    return v;
}

static DotkAotValue v_list(void *p)
{
    DotkAotValue v = {0};
    v.t = 5;
    v.p = p;
    return v;
}

static DotkAotValue v_map(void *p)
{
    DotkAotValue v = {0};
    v.t = 6;
    v.p = p;
    return v;
}

static int key_from_value(const DotkAotValue *v, int *out)
{
    if (!v || !out)
        return 0;
    if (v->t == 0)
    {
        *out = (int)v->n;
        return 1;
    }
    if (v->t == 1 && v->s)
    {
        if (strcmp(v->s, "X11_KEY_LEFT") == 0)
            *out = XK_Left;
        else if (strcmp(v->s, "X11_KEY_RIGHT") == 0)
            *out = XK_Right;
        else if (strcmp(v->s, "X11_KEY_UP") == 0)
            *out = XK_Up;
        else if (strcmp(v->s, "X11_KEY_DOWN") == 0)
            *out = XK_Down;
        else if (strcmp(v->s, "X11_KEY_A") == 0)
            *out = XK_A;
        else if (strcmp(v->s, "X11_KEY_D") == 0)
            *out = XK_D;
        else if (strcmp(v->s, "X11_KEY_W") == 0)
            *out = XK_W;
        else if (strcmp(v->s, "X11_KEY_S") == 0)
            *out = XK_S;
        else if (strcmp(v->s, "X11_KEY_Q") == 0)
            *out = XK_Q;
        else if (strcmp(v->s, "X11_KEY_E") == 0)
            *out = XK_E;
        else if (strcmp(v->s, "X11_KEY_ESCAPE") == 0)
            *out = XK_Escape;
        else if (strcmp(v->s, "X11_KEY_SPACE") == 0)
            *out = XK_space;
        else if (strcmp(v->s, "X11_MOUSE_LEFT") == 0)
            *out = 1;
        else if (strcmp(v->s, "X11_MOUSE_MIDDLE") == 0)
            *out = 2;
        else if (strcmp(v->s, "X11_MOUSE_RIGHT") == 0)
            *out = 3;
        else if (strcmp(v->s, "X11_MOUSE_WHEEL_UP") == 0)
            *out = 4;
        else if (strcmp(v->s, "X11_MOUSE_WHEEL_DOWN") == 0)
            *out = 5;
        else
            return 0;
        return 1;
    }
    return 0;
}

static int fn_x11Init(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    if (argc != 3 || argv[0].t != 0 || argv[1].t != 0 || argv[2].t != 1)
    {
        if (err)
            *err = "x11Init arity/type mismatch";
        return 1;
    }
    if (!g_display)
    {
        g_display = XOpenDisplay(NULL);
        if (!g_display)
        {
            if (err)
                *err = "x11 open display failed";
            return 1;
        }
        int s = DefaultScreen(g_display);
        int w = (int)argv[0].n;
        int h = (int)argv[1].n;
        g_window = XCreateSimpleWindow(g_display, RootWindow(g_display, s), 0, 0, (unsigned)w, (unsigned)h, 1, BlackPixel(g_display, s), BlackPixel(g_display, s));
        XSelectInput(g_display, g_window, ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | StructureNotifyMask);
        g_wmDeleteAtom = XInternAtom(g_display, "WM_DELETE_WINDOW", False);
        if (g_wmDeleteAtom != None)
            XSetWMProtocols(g_display, g_window, &g_wmDeleteAtom, 1);
        XStoreName(g_display, g_window, argv[2].s ? argv[2].s : "DotK");
        XMapWindow(g_display, g_window);
        g_gc = XCreateGC(g_display, g_window, 0, NULL);
        g_lastFrame = now_sec();
        g_clearPixel = rgb(0, 0, 0);
        g_shouldClose = 0;
        g_mouseX = 0;
        g_mouseY = 0;
        memset(g_mouseDown, 0, sizeof(g_mouseDown));
        memset(g_mouseClicked, 0, sizeof(g_mouseClicked));
        memset(g_keyDown, 0, sizeof(g_keyDown));
    }
    *out = v_bool(1);
    return 0;
}

static int fn_x11Close(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    (void)argv;
    if (argc != 0)
    {
        if (err)
            *err = "x11Close arity mismatch";
        return 1;
    }
    if (g_display)
    {
        if (g_gc)
            XFreeGC(g_display, g_gc);
        if (g_window)
            XDestroyWindow(g_display, g_window);
        XCloseDisplay(g_display);
        g_display = NULL;
        g_window = 0;
        g_gc = 0;
        g_wmDeleteAtom = None;
    }
    while (g_objCache != NULL)
    {
        ObjModelCache *next = g_objCache->next;
        free(g_objCache->path);
        free(g_objCache->verts);
        free(g_objCache->edges);
        free(g_objCache);
        g_objCache = next;
    }
    *out = v_nil();
    return 0;
}

static int fn_x11SetTargetFps(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    if (argc != 1 || argv[0].t != 0)
    {
        if (err)
            *err = "x11SetTargetFps arity/type mismatch";
        return 1;
    }
    g_targetFps = (int)argv[0].n;
    if (g_targetFps < 1)
        g_targetFps = 1;
    *out = v_nil();
    return 0;
}

static int fn_x11ShouldClose(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    (void)argv;
    if (argc != 0)
    {
        if (err)
            *err = "x11ShouldClose arity mismatch";
        return 1;
    }
    pump_events();
    *out = v_bool(g_shouldClose);
    return 0;
}

static int fn_x11Begin(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    if (argc != 3)
    {
        if (err)
            *err = "x11Begin arity mismatch";
        return 1;
    }
    if (g_display)
    {
        pump_events();
        g_clearPixel = rgb((int)argv[0].n, (int)argv[1].n, (int)argv[2].n);
        XSetForeground(g_display, g_gc, g_clearPixel);
        XWindowAttributes attrs;
        XGetWindowAttributes(g_display, g_window, &attrs);
        XFillRectangle(g_display, g_window, g_gc, 0, 0, (unsigned int)attrs.width, (unsigned int)attrs.height);
    }
    *out = v_nil();
    return 0;
}

static int fn_x11End(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    (void)argv;
    if (argc != 0)
    {
        if (err)
            *err = "x11End arity mismatch";
        return 1;
    }
    if (g_display)
    {
        XFlush(g_display);
        double now = now_sec();
        double target = 1.0 / (double)g_targetFps;
        double dt = now - g_lastFrame;
        if (dt < target)
        {
            int us = (int)((target - dt) * 1000000.0);
            if (us > 0)
                usleep((useconds_t)us);
            now = now_sec();
        }
        g_lastFrame = now;
    }
    *out = v_nil();
    return 0;
}

static int fn_x11Rect(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    if (argc != 7)
    {
        if (err)
            *err = "x11Rect arity mismatch";
        return 1;
    }
    if (g_display)
    {
        XSetForeground(g_display, g_gc, rgb((int)argv[4].n, (int)argv[5].n, (int)argv[6].n));
        XFillRectangle(g_display, g_window, g_gc, (int)argv[0].n, (int)argv[1].n, (unsigned int)((int)argv[2].n), (unsigned int)((int)argv[3].n));
    }
    *out = v_nil();
    return 0;
}

static int fn_x11Line(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    if (argc != 7)
    {
        if (err)
            *err = "x11Line arity mismatch";
        return 1;
    }
    if (g_display)
    {
        XSetForeground(g_display, g_gc, rgb((int)argv[4].n, (int)argv[5].n, (int)argv[6].n));
        XDrawLine(g_display, g_window, g_gc, (int)argv[0].n, (int)argv[1].n, (int)argv[2].n, (int)argv[3].n);
    }
    *out = v_nil();
    return 0;
}

static int fn_x11Text(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    if (argc != 6 || argv[0].t != 1)
    {
        if (err)
            *err = "x11Text arity/type mismatch";
        return 1;
    }
    if (g_display)
    {
        const char *t = argv[0].s ? argv[0].s : "";
        size_t len = strnlen(t, 4096);
        int x = (int)argv[1].n;
        int y = (int)argv[2].n;
        int tw = (int)len * 8;
        int th = 14;

        XSetForeground(g_display, g_gc, g_clearPixel);
        XFillRectangle(g_display, g_window, g_gc, x - 1, y - 12, (unsigned int)(tw + 2), (unsigned int)th);

        XSetForeground(g_display, g_gc, rgb((int)argv[3].n, (int)argv[4].n, (int)argv[5].n));
        XDrawString(g_display, g_window, g_gc, x, y, t, (int)len);
    }
    *out = v_nil();
    return 0;
}

static int fn_x11KeyDown(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    int key = 0;
    if (argc != 1 || !key_from_value(&argv[0], &key))
    {
        if (err)
            *err = "x11KeyDown arity/type mismatch";
        return 1;
    }
    pump_events();
    *out = v_bool((key >= 0 && key < 65536) ? g_keyDown[key] : 0);
    return 0;
}

static int fn_x11MouseX(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    (void)argv;
    if (argc != 0)
    {
        if (err)
            *err = "x11MouseX arity mismatch";
        return 1;
    }
    pump_events();
    *out = v_num((double)g_mouseX);
    return 0;
}

static int fn_x11MouseY(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    (void)argv;
    if (argc != 0)
    {
        if (err)
            *err = "x11MouseY arity mismatch";
        return 1;
    }
    pump_events();
    *out = v_num((double)g_mouseY);
    return 0;
}

static int fn_x11MouseDown(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    int b = 0;
    if (argc != 1 || !key_from_value(&argv[0], &b))
    {
        if (err)
            *err = "x11MouseDown arity/type mismatch";
        return 1;
    }
    pump_events();
    *out = v_bool((b >= 0 && b < X11_MOUSE_BUTTON_MAX) ? g_mouseDown[b] : 0);
    return 0;
}

static int fn_x11MouseClicked(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    int b = 0;
    if (argc != 1 || !key_from_value(&argv[0], &b))
    {
        if (err)
            *err = "x11MouseClicked arity/type mismatch";
        return 1;
    }
    pump_events();
    int c = (b >= 0 && b < X11_MOUSE_BUTTON_MAX) ? g_mouseClicked[b] : 0;
    if (b >= 0 && b < X11_MOUSE_BUTTON_MAX)
        g_mouseClicked[b] = 0;
    *out = v_bool(c);
    return 0;
}

static int fn_x11Time(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    (void)argv;
    if (argc != 0)
    {
        if (err)
            *err = "x11Time arity mismatch";
        return 1;
    }
    *out = v_num(now_sec());
    return 0;
}

static int parse_obj(const DotkAotApi *api, const char *path, DotkAotValue *out)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;

    int vCap = 256;
    int vCount = 0;
    ObjVert *verts = (ObjVert *)malloc(sizeof(ObjVert) * (size_t)vCap);
    int eCap = 512;
    int eCount = 0;
    ObjEdge *edges = (ObjEdge *)malloc(sizeof(ObjEdge) * (size_t)eCap);

    char line[2048];
    while (fgets(line, sizeof(line), f) != NULL)
    {
        if (line[0] == 'v' && line[1] == ' ')
        {
            double x = 0, y = 0, z = 0;
            if (sscanf(line + 2, "%lf %lf %lf", &x, &y, &z) == 3)
            {
                if (vCount + 1 > vCap)
                {
                    vCap *= 2;
                    verts = (ObjVert *)realloc(verts, sizeof(ObjVert) * (size_t)vCap);
                }
                verts[vCount++] = (ObjVert){x, y, z};
            }
        }
        else if (line[0] == 'f' && line[1] == ' ')
        {
            int idx[64];
            int n = 0;
            char *tok = strtok(line + 2, " \t\r\n");
            while (tok && n < 64)
            {
                int vi = atoi(tok);
                if (vi > 0)
                    vi -= 1;
                else if (vi < 0)
                    vi = vCount + vi;
                idx[n++] = vi;
                tok = strtok(NULL, " \t\r\n");
            }
            for (int i = 0; i < n; i++)
            {
                int a = idx[i];
                int b = idx[(i + 1) % n];
                if (a < 0 || b < 0 || a >= vCount || b >= vCount || a == b)
                    continue;
                if (eCount + 1 > eCap)
                {
                    eCap *= 2;
                    edges = (ObjEdge *)realloc(edges, sizeof(ObjEdge) * (size_t)eCap);
                }
                edges[eCount++] = (ObjEdge){a, b};
            }
        }
    }
    fclose(f);

    void *vertsList = api->listNew();
    void *edgesList = api->listNew();
    if (!vertsList || !edgesList)
    {
        free(verts);
        free(edges);
        return 0;
    }

    for (int i = 0; i < vCount; i++)
    {
        void *row = api->listNew();
        if (!row)
            continue;
        api->listPush(row, v_num(verts[i].x));
        api->listPush(row, v_num(verts[i].y));
        api->listPush(row, v_num(verts[i].z));
        api->listPush(vertsList, v_list(row));
    }

    for (int i = 0; i < eCount; i++)
    {
        void *row = api->listNew();
        if (!row)
            continue;
        api->listPush(row, v_num((double)edges[i].a));
        api->listPush(row, v_num((double)edges[i].b));
        api->listPush(edgesList, v_list(row));
    }

    void *m = api->mapNew();
    if (!m)
    {
        free(verts);
        free(edges);
        return 0;
    }
    api->mapSet(m, v_str("verts"), v_list(vertsList));
    api->mapSet(m, v_str("edges"), v_list(edgesList));

    free(verts);
    free(edges);
    *out = v_map(m);
    return 1;
}

static int fn_x11LoadObj(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    (void)err;
    if (argc != 1 || argv[0].t != 1)
        return 1;

    if (g_aot_api == NULL)
    {
        *out = v_nil();
        return 0;
    }
    if (!parse_obj(g_aot_api, argv[0].s ? argv[0].s : "", out))
    {
        *out = v_nil();
    }
    return 0;
}

static int fn_x11DrawObjWire(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **err)
{
    if (argc != 16 || argv[0].t != 1)
    {
        if (err)
            *err = "x11DrawObjWire arity/type mismatch";
        return 1;
    }
    for (int i = 1; i < 16; i++)
    {
        if (argv[i].t != 0)
        {
            if (err)
                *err = "x11DrawObjWire expects numeric args";
            return 1;
        }
    }

    if (!g_display || !g_window || !g_gc)
    {
        *out = v_nil();
        return 0;
    }

    const char *path = argv[0].s ? argv[0].s : "";
    ObjModelCache *model = load_obj_cache(path);
    if (!model)
    {
        if (err)
            *err = "x11DrawObjWire failed to load OBJ";
        return 1;
    }

    double posX = argv[1].n;
    double posY = argv[2].n;
    double posZ = argv[3].n;
    double scale = argv[4].n;
    double rotX = argv[5].n;
    double rotY = argv[6].n;
    double camX = argv[7].n;
    double camY = argv[8].n;
    double camZ = argv[9].n;
    double camRotX = argv[10].n;
    double camRotY = argv[11].n;
    double fov = argv[12].n;
    int cr = (int)argv[13].n;
    int cg = (int)argv[14].n;
    int cb = (int)argv[15].n;

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
    unsigned char *okv = (unsigned char *)malloc((size_t)model->vertCount);
    if (!sxv || !syv || !okv)
    {
        free(sxv);
        free(syv);
        free(okv);
        if (err)
            *err = "x11DrawObjWire out of memory";
        return 1;
    }

    for (int i = 0; i < model->vertCount; i++)
    {
        ObjVert v = model->verts[i];
        double lx = v.x * scale;
        double ly = v.y * scale;
        double lz = v.z * scale;

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
        okv[i] = 1;
    }

    XSetForeground(g_display, g_gc, rgb(cr, cg, cb));
    for (int i = 0; i < model->edgeCount; i++)
    {
        int a = model->edges[i].a;
        int b = model->edges[i].b;
        if (a < 0 || b < 0 || a >= model->vertCount || b >= model->vertCount)
            continue;
        if (!okv[a] || !okv[b])
            continue;

        int codeA = screen_out_code(sxv[a], syv[a], width, height);
        int codeB = screen_out_code(sxv[b], syv[b], width, height);
        if ((codeA & codeB) != 0)
            continue;

        XDrawLine(g_display, g_window, g_gc, (int)sxv[a], (int)syv[a], (int)sxv[b], (int)syv[b]);
    }

    free(sxv);
    free(syv);
    free(okv);
    *out = v_nil();
    return 0;
}

int dotk_aot_init(const DotkAotApi *api)
{
    if (!api || api->version != DOTK_AOT_API_VERSION)
        return 0;

    g_aot_api = api;

    api->registerFunction("x11Init", fn_x11Init);
    api->registerFunction("x11Close", fn_x11Close);
    api->registerFunction("x11SetTargetFps", fn_x11SetTargetFps);
    api->registerFunction("x11ShouldClose", fn_x11ShouldClose);
    api->registerFunction("x11Begin", fn_x11Begin);
    api->registerFunction("x11End", fn_x11End);
    api->registerFunction("x11Rect", fn_x11Rect);
    api->registerFunction("x11Line", fn_x11Line);
    api->registerFunction("x11Text", fn_x11Text);
    api->registerFunction("x11KeyDown", fn_x11KeyDown);
    api->registerFunction("x11MouseX", fn_x11MouseX);
    api->registerFunction("x11MouseY", fn_x11MouseY);
    api->registerFunction("x11MouseDown", fn_x11MouseDown);
    api->registerFunction("x11MouseClicked", fn_x11MouseClicked);
    api->registerFunction("x11Time", fn_x11Time);
    api->registerFunction("x11LoadObj", fn_x11LoadObj);
    api->registerFunction("x11DrawObjWire", fn_x11DrawObjWire);

    api->registerNumberConstant("X11_KEY_LEFT", (double)XK_Left);
    api->registerNumberConstant("X11_KEY_RIGHT", (double)XK_Right);
    api->registerNumberConstant("X11_KEY_UP", (double)XK_Up);
    api->registerNumberConstant("X11_KEY_DOWN", (double)XK_Down);
    api->registerNumberConstant("X11_KEY_A", (double)XK_A);
    api->registerNumberConstant("X11_KEY_D", (double)XK_D);
    api->registerNumberConstant("X11_KEY_W", (double)XK_W);
    api->registerNumberConstant("X11_KEY_S", (double)XK_S);
    api->registerNumberConstant("X11_KEY_Q", (double)XK_Q);
    api->registerNumberConstant("X11_KEY_E", (double)XK_E);
    api->registerNumberConstant("X11_KEY_SPACE", (double)XK_space);
    api->registerNumberConstant("X11_KEY_ESCAPE", (double)XK_Escape);
    api->registerNumberConstant("X11_MOUSE_LEFT", 1.0);
    api->registerNumberConstant("X11_MOUSE_MIDDLE", 2.0);
    api->registerNumberConstant("X11_MOUSE_RIGHT", 3.0);
    api->registerNumberConstant("X11_MOUSE_WHEEL_UP", 4.0);
    api->registerNumberConstant("X11_MOUSE_WHEEL_DOWN", 5.0);

    return 1;
}
