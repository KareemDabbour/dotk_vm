#ifndef DOTK_AOT_MODULE_API_H
#define DOTK_AOT_MODULE_API_H

#ifdef __cplusplus
extern "C"
{
#endif

#define DOTK_AOT_API_VERSION 1

    typedef struct DotkAotValue
    {
        int t;
        double n;
        const char *s;
        int b;
        void *p;
    } DotkAotValue;

    typedef int (*DotkAotFunction)(int argc, const DotkAotValue *argv, DotkAotValue *out, const char **lastErr);

    typedef struct DotkAotApi
    {
        int version;
        void (*registerFunction)(const char *name, DotkAotFunction fn);
        void (*registerNumberConstant)(const char *name, double value);
        void (*registerStringConstant)(const char *name, const char *value);
        void *(*listNew)(void);
        int (*listPush)(void *list, DotkAotValue value);
        void *(*mapNew)(void);
        int (*mapSet)(void *map, DotkAotValue key, DotkAotValue value);
    } DotkAotApi;

    typedef int (*DotkAotInitFn)(const DotkAotApi *api);

#ifdef __cplusplus
}
#endif

#endif
