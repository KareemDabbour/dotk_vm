#include "../src/include/native_api.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const char *src;
    int len;
    int i;
    bool failed;
    const char *error;
} JsonParser;

static const DotKNativeApi *g_api = NULL;

static void set_error(JsonParser *p, const char *msg)
{
    if (!p->failed)
    {
        p->failed = true;
        p->error = msg;
    }
}

static void skip_ws(JsonParser *p)
{
    while (p->i < p->len && isspace((unsigned char)p->src[p->i]))
        p->i++;
}

static bool match(JsonParser *p, const char *token)
{
    int n = (int)strlen(token);
    if (p->i + n > p->len)
        return false;
    if (strncmp(p->src + p->i, token, (size_t)n) != 0)
        return false;
    p->i += n;
    return true;
}

static Value parse_value(JsonParser *p);

static Value make_string_from_heap(char *buf, int len)
{
    Value s = g_api->makeString(buf, len, false);
    free(buf);
    return s;
}

static Value parse_string(JsonParser *p)
{
    if (p->i >= p->len || p->src[p->i] != '"')
    {
        set_error(p, "Expected string");
        return NIL_VAL;
    }
    p->i++;

    int cap = 64;
    int outLen = 0;
    char *out = (char *)malloc((size_t)cap);

    while (p->i < p->len)
    {
        char c = p->src[p->i++];
        if (c == '"')
        {
            Value s = make_string_from_heap(out, outLen);
            return s;
        }

        if (c == '\\')
        {
            if (p->i >= p->len)
            {
                free(out);
                set_error(p, "Invalid escape");
                return NIL_VAL;
            }
            char e = p->src[p->i++];
            switch (e)
            {
            case '"':
                c = '"';
                break;
            case '\\':
                c = '\\';
                break;
            case '/':
                c = '/';
                break;
            case 'b':
                c = '\b';
                break;
            case 'f':
                c = '\f';
                break;
            case 'n':
                c = '\n';
                break;
            case 'r':
                c = '\r';
                break;
            case 't':
                c = '\t';
                break;
            case 'u':
                if (p->i + 4 <= p->len)
                    p->i += 4;
                c = '?';
                break;
            default:
                free(out);
                set_error(p, "Unknown string escape");
                return NIL_VAL;
            }
        }

        if (outLen + 1 >= cap)
        {
            cap *= 2;
            out = (char *)realloc(out, (size_t)cap);
        }
        out[outLen++] = c;
    }

    free(out);
    set_error(p, "Unterminated string");
    return NIL_VAL;
}

static Value parse_number(JsonParser *p)
{
    int start = p->i;

    if (p->i < p->len && p->src[p->i] == '-')
        p->i++;

    if (p->i >= p->len || !isdigit((unsigned char)p->src[p->i]))
    {
        set_error(p, "Invalid number");
        return NIL_VAL;
    }

    if (p->src[p->i] == '0')
    {
        p->i++;
    }
    else
    {
        while (p->i < p->len && isdigit((unsigned char)p->src[p->i]))
            p->i++;
    }

    if (p->i < p->len && p->src[p->i] == '.')
    {
        p->i++;
        if (p->i >= p->len || !isdigit((unsigned char)p->src[p->i]))
        {
            set_error(p, "Invalid number fractional part");
            return NIL_VAL;
        }
        while (p->i < p->len && isdigit((unsigned char)p->src[p->i]))
            p->i++;
    }

    if (p->i < p->len && (p->src[p->i] == 'e' || p->src[p->i] == 'E'))
    {
        p->i++;
        if (p->i < p->len && (p->src[p->i] == '+' || p->src[p->i] == '-'))
            p->i++;
        if (p->i >= p->len || !isdigit((unsigned char)p->src[p->i]))
        {
            set_error(p, "Invalid number exponent");
            return NIL_VAL;
        }
        while (p->i < p->len && isdigit((unsigned char)p->src[p->i]))
            p->i++;
    }

    int n = p->i - start;
    char *tmp = (char *)malloc((size_t)n + 1);
    memcpy(tmp, p->src + start, (size_t)n);
    tmp[n] = '\0';

    double val = strtod(tmp, NULL);
    free(tmp);
    return NUM_VAL(val);
}

static Value parse_array(JsonParser *p)
{
    if (p->i >= p->len || p->src[p->i] != '[')
    {
        set_error(p, "Expected array");
        return NIL_VAL;
    }
    p->i++;

    Value list = g_api->makeList();
    g_api->pushValue(list);

    skip_ws(p);
    if (p->i < p->len && p->src[p->i] == ']')
    {
        p->i++;
        g_api->popValue();
        return list;
    }

    while (p->i < p->len && !p->failed)
    {
        skip_ws(p);
        Value item = parse_value(p);
        if (p->failed)
            break;

        g_api->pushValue(item);
        if (!g_api->listAppend(list, item))
        {
            g_api->popValue();
            set_error(p, "Failed to append array item");
            break;
        }
        g_api->popValue();

        skip_ws(p);
        if (p->i < p->len && p->src[p->i] == ',')
        {
            p->i++;
            continue;
        }
        if (p->i < p->len && p->src[p->i] == ']')
        {
            p->i++;
            g_api->popValue();
            return list;
        }

        set_error(p, "Expected ',' or ']' in array");
        break;
    }

    g_api->popValue();
    if (!p->failed)
        set_error(p, "Unterminated array");
    return NIL_VAL;
}

static Value parse_object(JsonParser *p)
{
    if (p->i >= p->len || p->src[p->i] != '{')
    {
        set_error(p, "Expected object");
        return NIL_VAL;
    }
    p->i++;

    Value map = g_api->makeMap();
    g_api->pushValue(map);

    skip_ws(p);
    if (p->i < p->len && p->src[p->i] == '}')
    {
        p->i++;
        g_api->popValue();
        return map;
    }

    while (p->i < p->len && !p->failed)
    {
        skip_ws(p);
        Value key = parse_string(p);
        if (p->failed)
            break;

        skip_ws(p);
        if (p->i >= p->len || p->src[p->i] != ':')
        {
            set_error(p, "Expected ':' in object");
            break;
        }
        p->i++;

        skip_ws(p);
        Value value = parse_value(p);
        if (p->failed)
            break;

        g_api->pushValue(key);
        g_api->pushValue(value);
        if (!g_api->mapSet(map, key, value))
        {
            g_api->popValue();
            g_api->popValue();
            set_error(p, "Failed setting object key");
            break;
        }
        g_api->popValue();
        g_api->popValue();

        skip_ws(p);
        if (p->i < p->len && p->src[p->i] == ',')
        {
            p->i++;
            continue;
        }
        if (p->i < p->len && p->src[p->i] == '}')
        {
            p->i++;
            g_api->popValue();
            return map;
        }

        set_error(p, "Expected ',' or '}' in object");
        break;
    }

    g_api->popValue();
    if (!p->failed)
        set_error(p, "Unterminated object");
    return NIL_VAL;
}

static Value parse_value(JsonParser *p)
{
    skip_ws(p);
    if (p->i >= p->len)
    {
        set_error(p, "Unexpected end of input");
        return NIL_VAL;
    }

    char c = p->src[p->i];
    if (c == '"')
        return parse_string(p);
    if (c == '{')
        return parse_object(p);
    if (c == '[')
        return parse_array(p);
    if (c == 't')
    {
        if (match(p, "true"))
            return BOOL_VAL(true);
        set_error(p, "Invalid token near 't'");
        return NIL_VAL;
    }
    if (c == 'f')
    {
        if (match(p, "false"))
            return BOOL_VAL(false);
        set_error(p, "Invalid token near 'f'");
        return NIL_VAL;
    }
    if (c == 'n')
    {
        if (match(p, "null"))
            return NIL_VAL;
        set_error(p, "Invalid token near 'n'");
        return NIL_VAL;
    }

    if (c == '-' || isdigit((unsigned char)c))
        return parse_number(p);

    set_error(p, "Unexpected token");
    return NIL_VAL;
}

typedef struct
{
    char *buf;
    int len;
    int cap;
    bool failed;
} StrBuilder;

static void sb_init(StrBuilder *sb)
{
    sb->cap = 256;
    sb->len = 0;
    sb->failed = false;
    sb->buf = (char *)malloc((size_t)sb->cap);
}

static void sb_need(StrBuilder *sb, int extra)
{
    if (sb->failed)
        return;
    int needed = sb->len + extra + 1;
    if (needed <= sb->cap)
        return;
    while (sb->cap < needed)
        sb->cap *= 2;
    sb->buf = (char *)realloc(sb->buf, (size_t)sb->cap);
}

static void sb_append_char(StrBuilder *sb, char c)
{
    if (sb->failed)
        return;
    sb_need(sb, 1);
    sb->buf[sb->len++] = c;
}

static void sb_append(StrBuilder *sb, const char *s, int n)
{
    if (sb->failed)
        return;
    sb_need(sb, n);
    memcpy(sb->buf + sb->len, s, (size_t)n);
    sb->len += n;
}

static void stringify_value(StrBuilder *sb, Value v);

static void stringify_string(StrBuilder *sb, const char *s, int n)
{
    sb_append_char(sb, '"');
    for (int i = 0; i < n; i++)
    {
        char c = s[i];
        switch (c)
        {
        case '"':
            sb_append(sb, "\\\"", 2);
            break;
        case '\\':
            sb_append(sb, "\\\\", 2);
            break;
        case '\b':
            sb_append(sb, "\\b", 2);
            break;
        case '\f':
            sb_append(sb, "\\f", 2);
            break;
        case '\n':
            sb_append(sb, "\\n", 2);
            break;
        case '\r':
            sb_append(sb, "\\r", 2);
            break;
        case '\t':
            sb_append(sb, "\\t", 2);
            break;
        default:
            if ((unsigned char)c < 0x20)
            {
                char tmp[7];
                snprintf(tmp, sizeof(tmp), "\\u%04x", (unsigned char)c);
                sb_append(sb, tmp, 6);
            }
            else
            {
                sb_append_char(sb, c);
            }
            break;
        }
    }
    sb_append_char(sb, '"');
}

static void stringify_map(StrBuilder *sb, ObjMap *map)
{
    sb_append_char(sb, '{');
    bool first = true;
    for (int i = 0; i < map->map.capacity; i++)
    {
        MapEntry *e = &map->map.entries[i];
        if (!e->isUsed)
            continue;

        if (!first)
            sb_append_char(sb, ',');
        first = false;

        if (!IS_STR(e->key))
            stringify_string(sb, "<non_string_key>", 16);
        else
            stringify_string(sb, AS_STR(e->key)->chars, AS_STR(e->key)->len);

        sb_append_char(sb, ':');
        stringify_value(sb, e->value);
    }
    sb_append_char(sb, '}');
}

static void stringify_list(StrBuilder *sb, ObjList *list)
{
    sb_append_char(sb, '[');
    for (int i = 0; i < list->count; i++)
    {
        if (i > 0)
            sb_append_char(sb, ',');
        stringify_value(sb, list->items[i]);
    }
    sb_append_char(sb, ']');
}

static void stringify_value(StrBuilder *sb, Value v)
{
    if (IS_NIL(v))
    {
        sb_append(sb, "null", 4);
        return;
    }
    if (IS_BOOL(v))
    {
        if (AS_BOOL(v))
            sb_append(sb, "true", 4);
        else
            sb_append(sb, "false", 5);
        return;
    }
    if (IS_NUM(v))
    {
        char num[64];
        int n = snprintf(num, sizeof(num), "%.15g", AS_NUM(v));
        if (n < 0)
            n = 0;
        sb_append(sb, num, n);
        return;
    }
    if (IS_STR(v))
    {
        stringify_string(sb, AS_STR(v)->chars, AS_STR(v)->len);
        return;
    }
    if (IS_LIST(v))
    {
        stringify_list(sb, AS_LIST(v));
        return;
    }
    if (IS_MAP(v))
    {
        stringify_map(sb, AS_MAP(v));
        return;
    }

    sb_append(sb, "null", 4);
}

static Value json_parse_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;
    if (argc != 1 || !IS_STR(argv[0]))
    {
        g_api->raiseError("JSON.parse(string) expects a single string argument");
        *hasError = true;
        return NIL_VAL;
    }

    JsonParser p;
    p.src = AS_STR(argv[0])->chars;
    p.len = AS_STR(argv[0])->len;
    p.i = 0;
    p.failed = false;
    p.error = NULL;

    Value out = parse_value(&p);
    if (!p.failed)
    {
        skip_ws(&p);
        if (p.i != p.len)
            set_error(&p, "Trailing characters after JSON value");
    }

    if (p.failed)
    {
        g_api->raiseError("JSON.parse failed near index %d: %s", p.i, p.error ? p.error : "invalid json");
        *hasError = true;
        return NIL_VAL;
    }

    return out;
}

static Value json_stringify_native(int argc, Value *argv, bool *hasError, bool *pushedValue)
{
    (void)pushedValue;
    if (argc != 1)
    {
        g_api->raiseError("JSON.stringify(value) expects 1 argument");
        *hasError = true;
        return NIL_VAL;
    }

    StrBuilder sb;
    sb_init(&sb);
    stringify_value(&sb, argv[0]);

    sb_append_char(&sb, '\0');
    Value s = g_api->makeString(sb.buf, sb.len - 1, false);
    free(sb.buf);
    return s;
}

bool dotk_init_module(const DotKNativeApi *api)
{
    if (api == NULL || api->version != DOTK_NATIVE_API_VERSION)
        return false;

    g_api = api;

    ObjClass *jsonClass = api->defineClass("JSON");
    api->defineClassStaticMethod(jsonClass, "parse", json_parse_native);
    api->defineClassStaticMethod(jsonClass, "stringify", json_stringify_native);

    api->defineNative("json_parse", json_parse_native);
    api->defineNative("json_stringify", json_stringify_native);

    return true;
}
