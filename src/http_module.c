#include "include/builtin_module.h"
#include "include/memory.h"
#include "include/vm.h"

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

typedef struct
{
    char host[512];
    int port;
    char path[2048];
} HttpUrlParts;

static bool parseHttpUrl(const char *url, HttpUrlParts *parts, char *err, size_t errCap)
{
    if (url == NULL || parts == NULL)
    {
        snprintf(err, errCap, "Invalid URL");
        return false;
    }

    const char *p = url;
    if (strncmp(p, "http://", 7) == 0)
        p += 7;
    else if (strncmp(p, "https://", 8) == 0)
    {
        snprintf(err, errCap, "https is not supported by builtin http module yet");
        return false;
    }

    if (*p == '\0')
    {
        snprintf(err, errCap, "Invalid URL: missing host");
        return false;
    }

    const char *slash = strchr(p, '/');
    size_t hostPortLen = slash ? (size_t)(slash - p) : strlen(p);
    if (hostPortLen == 0 || hostPortLen >= sizeof(parts->host))
    {
        snprintf(err, errCap, "Invalid URL host");
        return false;
    }

    char hostPort[512];
    memcpy(hostPort, p, hostPortLen);
    hostPort[hostPortLen] = '\0';

    parts->port = 80;
    const char *colon = strrchr(hostPort, ':');
    if (colon != NULL)
    {
        size_t hostLen = (size_t)(colon - hostPort);
        if (hostLen == 0 || hostLen >= sizeof(parts->host))
        {
            snprintf(err, errCap, "Invalid URL host");
            return false;
        }

        const char *portStr = colon + 1;
        if (*portStr == '\0')
        {
            snprintf(err, errCap, "Invalid URL port");
            return false;
        }

        char *endPtr = NULL;
        long port = strtol(portStr, &endPtr, 10);
        if (*endPtr != '\0' || port <= 0 || port > 65535)
        {
            snprintf(err, errCap, "Invalid URL port");
            return false;
        }

        memcpy(parts->host, hostPort, hostLen);
        parts->host[hostLen] = '\0';
        parts->port = (int)port;
    }
    else
    {
        strncpy(parts->host, hostPort, sizeof(parts->host) - 1);
        parts->host[sizeof(parts->host) - 1] = '\0';
    }

    if (slash == NULL)
    {
        parts->path[0] = '/';
        parts->path[1] = '\0';
    }
    else
    {
        size_t pathLen = strlen(slash);
        if (pathLen >= sizeof(parts->path))
        {
            snprintf(err, errCap, "URL path too long");
            return false;
        }
        memcpy(parts->path, slash, pathLen + 1);
    }

    return true;
}

static bool sendAll(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n <= 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

static bool toLowerCopy(const char *src, int len, char **out)
{
    char *lower = ALLOCATE(char, len + 1);
    for (int i = 0; i < len; i++)
        lower[i] = (char)tolower((unsigned char)src[i]);
    lower[len] = '\0';
    *out = lower;
    return true;
}

static bool headerContainsChunked(const char *headers, int headersLen)
{
    char *lower = NULL;
    if (!toLowerCopy(headers, headersLen, &lower))
        return false;

    bool chunked = strstr(lower, "transfer-encoding:") != NULL && strstr(lower, "chunked") != NULL;
    FREE_ARRAY(char, lower, headersLen + 1);
    return chunked;
}

static bool decodeChunkedBody(const char *input, int inputLen, char **decodedOut, int *decodedLenOut)
{
    int outCap = inputLen > 0 ? inputLen : 1;
    int outLen = 0;
    char *out = ALLOCATE(char, outCap + 1);

    const char *p = input;
    const char *end = input + inputLen;

    while (p < end)
    {
        const char *lineEnd = strstr(p, "\r\n");
        if (lineEnd == NULL)
            lineEnd = strstr(p, "\n");
        if (lineEnd == NULL)
            break;

        int lineLen = (int)(lineEnd - p);
        if (lineLen <= 0)
            break;

        char sizeBuf[32];
        int sizeLen = lineLen < (int)sizeof(sizeBuf) - 1 ? lineLen : (int)sizeof(sizeBuf) - 1;
        memcpy(sizeBuf, p, sizeLen);
        sizeBuf[sizeLen] = '\0';

        char *semi = strchr(sizeBuf, ';');
        if (semi != NULL)
            *semi = '\0';

        char *sizeEnd = NULL;
        long chunkSize = strtol(sizeBuf, &sizeEnd, 16);
        if (sizeEnd == sizeBuf || chunkSize < 0)
            break;

        p = lineEnd;
        if (p < end && *p == '\r')
            p++;
        if (p < end && *p == '\n')
            p++;

        if (chunkSize == 0)
        {
            out[outLen] = '\0';
            *decodedOut = out;
            *decodedLenOut = outLen;
            return true;
        }

        if (p + chunkSize > end)
            break;

        int needed = outLen + (int)chunkSize;
        if (needed + 1 > outCap)
        {
            int oldCap = outCap;
            while (needed + 1 > outCap)
                outCap = GROW_CAPACITY(outCap);
            out = GROW_ARRAY(char, out, oldCap + 1, outCap + 1);
        }

        memcpy(out + outLen, p, (size_t)chunkSize);
        outLen += (int)chunkSize;
        p += chunkSize;

        if (p < end && *p == '\r')
            p++;
        if (p < end && *p == '\n')
            p++;
    }

    FREE_ARRAY(char, out, outCap + 1);
    return false;
}

static Value buildHttpResponseObject(const char *url,
                                     int status,
                                     const char *statusText,
                                     int statusTextLen,
                                     const char *headers,
                                     int headersLen,
                                     const char *body,
                                     int bodyLen)
{
    ObjInstance *result = newInstance(vm.baseObj);
    push(OBJ_VAL(result));

    tableSet(&result->fields, copyString("url", 3), OBJ_VAL(copyString(url, (int)strlen(url))));
    tableSet(&result->fields, copyString("status", 6), NUM_VAL((double)status));
    tableSet(&result->fields, copyString("ok", 2), BOOL_VAL(status >= 200 && status < 300));

    tableSet(&result->fields,
             copyString("statusText", 10),
             OBJ_VAL(copyString(statusText, statusTextLen)));

    tableSet(&result->fields,
             copyString("headers", 7),
             OBJ_VAL(copyString(headers, headersLen)));

    tableSet(&result->fields,
             copyString("body", 4),
             OBJ_VAL(copyString(body, bodyLen)));

    return pop();
}

static Value httpRequestInternal(const char *method,
                                 const char *url,
                                 const char *body,
                                 int bodyLen,
                                 bool *hasError)
{
    HttpUrlParts parts;
    char err[256] = {0};
    if (!parseHttpUrl(url, &parts, err, sizeof(err)))
    {
        runtimeError("http_request failed: %s", err);
        *hasError = true;
        return NIL_VAL;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", parts.port);

    struct addrinfo *res = NULL;
    DotKThread *saved = vmBeginBlockingIO();
    int gai = getaddrinfo(parts.host, portStr, &hints, &res);
    if (gai != 0)
    {
        vmEndBlockingIO(saved);
        runtimeError("http_request failed: DNS lookup failed for '%s'", parts.host);
        *hasError = true;
        return NIL_VAL;
    }

    int fd = -1;
    for (struct addrinfo *it = res; it != NULL; it = it->ai_next)
    {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    vmEndBlockingIO(saved);
    freeaddrinfo(res);

    if (fd < 0)
    {
        runtimeError("http_request failed: could not connect to %s:%d", parts.host, parts.port);
        *hasError = true;
        return NIL_VAL;
    }

    const char *bodyPtr = body ? body : "";
    int contentLen = body ? bodyLen : 0;

    int reqCap = 1024 + (int)strlen(parts.path) + (int)strlen(parts.host) + contentLen;
    char *req = ALLOCATE(char, reqCap);
    int reqLen = snprintf(req,
                          reqCap,
                          "%s %s HTTP/1.1\r\n"
                          "Host: %s\r\n"
                          "User-Agent: dotk-http/1.0\r\n"
                          "Connection: close\r\n"
                          "Content-Length: %d\r\n"
                          "\r\n",
                          method,
                          parts.path,
                          parts.host,
                          contentLen);

    if (contentLen > 0)
    {
        if (reqLen + contentLen + 1 > reqCap)
        {
            int oldCap = reqCap;
            reqCap = reqLen + contentLen + 1;
            req = GROW_ARRAY(char, req, oldCap, reqCap);
        }
        memcpy(req + reqLen, bodyPtr, contentLen);
        reqLen += contentLen;
        req[reqLen] = '\0';
    }

    saved = vmBeginBlockingIO();
    bool sendOk = sendAll(fd, req, (size_t)reqLen);
    vmEndBlockingIO(saved);
    FREE_ARRAY(char, req, reqCap);
    if (!sendOk)
    {
        close(fd);
        runtimeError("http_request failed: send error");
        *hasError = true;
        return NIL_VAL;
    }

    int resCap = 8192;
    int resLen = 0;
    char *resBuf = ALLOCATE(char, resCap + 1);

    while (true)
    {
        char chunk[4096];
        saved = vmBeginBlockingIO();
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        vmEndBlockingIO(saved);
        if (n < 0)
        {
            FREE_ARRAY(char, resBuf, resCap + 1);
            close(fd);
            runtimeError("http_request failed: receive error");
            *hasError = true;
            return NIL_VAL;
        }
        if (n == 0)
            break;

        if (resLen + (int)n + 1 > resCap)
        {
            int oldCap = resCap;
            while (resLen + (int)n + 1 > resCap)
                resCap = GROW_CAPACITY(resCap);
            resBuf = GROW_ARRAY(char, resBuf, oldCap + 1, resCap + 1);
        }
        memcpy(resBuf + resLen, chunk, (size_t)n);
        resLen += (int)n;
    }

    close(fd);
    resBuf[resLen] = '\0';

    const char *lineEnd = strstr(resBuf, "\r\n");
    if (lineEnd == NULL)
        lineEnd = strstr(resBuf, "\n");
    if (lineEnd == NULL)
    {
        FREE_ARRAY(char, resBuf, resCap + 1);
        runtimeError("http_request failed: invalid HTTP response");
        *hasError = true;
        return NIL_VAL;
    }

    int status = 0;
    char statusText[256] = {0};
    if (sscanf(resBuf, "HTTP/%*s %d %255[^\r\n]", &status, statusText) < 1)
    {
        FREE_ARRAY(char, resBuf, resCap + 1);
        runtimeError("http_request failed: could not parse HTTP status");
        *hasError = true;
        return NIL_VAL;
    }

    const char *sep = strstr(resBuf, "\r\n\r\n");
    int sepLen = 4;
    if (sep == NULL)
    {
        sep = strstr(resBuf, "\n\n");
        sepLen = 2;
    }

    const char *headersStart = lineEnd;
    if (*headersStart == '\r')
        headersStart++;
    if (*headersStart == '\n')
        headersStart++;

    const char *bodyStart = sep ? (sep + sepLen) : resBuf + resLen;
    int headersLen = (int)(sep ? (sep - headersStart) : 0);
    int bodyRawLen = (int)(resBuf + resLen - bodyStart);

    char *decodedBody = NULL;
    int decodedBodyLen = 0;
    bool chunked = headersLen > 0 && headerContainsChunked(headersStart, headersLen);
    if (chunked)
    {
        if (decodeChunkedBody(bodyStart, bodyRawLen, &decodedBody, &decodedBodyLen))
        {
            Value v = buildHttpResponseObject(url,
                                              status,
                                              statusText,
                                              (int)strlen(statusText),
                                              headersStart,
                                              headersLen,
                                              decodedBody,
                                              decodedBodyLen);
            FREE_ARRAY(char, decodedBody, decodedBodyLen + 1);
            FREE_ARRAY(char, resBuf, resCap + 1);
            return v;
        }
    }

    Value v = buildHttpResponseObject(url,
                                      status,
                                      statusText,
                                      (int)strlen(statusText),
                                      headersStart,
                                      headersLen,
                                      bodyStart,
                                      bodyRawLen);
    FREE_ARRAY(char, resBuf, resCap + 1);
    return v;
}

NATIVE_FN(httpRequestNative)
{
    if (argc != 2 && argc != 3)
    {
        runtimeError("'http_request(method, url, <optional> body)' expects 2 or 3 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]) || !IS_STR(argv[1]))
    {
        runtimeError("'http_request(method, url, <optional> body)' expects method and url as strings");
        *hasError = true;
        return NIL_VAL;
    }

    const char *method = AS_CSTR(argv[0]);
    const char *url = AS_CSTR(argv[1]);

    const char *body = NULL;
    int bodyLen = 0;
    if (argc == 3)
    {
        if (!IS_STR(argv[2]))
        {
            runtimeError("'http_request(method, url, <optional> body)' body must be a string");
            *hasError = true;
            return NIL_VAL;
        }
        body = AS_CSTR(argv[2]);
        bodyLen = AS_STR(argv[2])->len;
    }

    return httpRequestInternal(method, url, body, bodyLen, hasError);
}

NATIVE_FN(httpGetNative)
{
    if (argc != 1)
    {
        runtimeError("'http_get(url)' expects 1 argument but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]))
    {
        runtimeError("'http_get(url)' expects url as string");
        *hasError = true;
        return NIL_VAL;
    }

    return httpRequestInternal("GET", AS_CSTR(argv[0]), NULL, 0, hasError);
}

NATIVE_FN(httpPostNative)
{
    if (argc != 2)
    {
        runtimeError("'http_post(url, body)' expects 2 arguments but %d were passed in", argc);
        *hasError = true;
        return NIL_VAL;
    }
    if (!IS_STR(argv[0]) || !IS_STR(argv[1]))
    {
        runtimeError("'http_post(url, body)' expects url and body as strings");
        *hasError = true;
        return NIL_VAL;
    }

    return httpRequestInternal("POST", AS_CSTR(argv[0]), AS_CSTR(argv[1]), AS_STR(argv[1])->len, hasError);
}

static void initHttpModule()
{
    vmDefineNative("http_request", httpRequestNative);
    vmDefineNative("http_get", httpGetNative);
    vmDefineNative("http_post", httpPostNative);
}

void registerBuiltinHttpModule()
{
    registerBuiltinModule("http", initHttpModule);
}
