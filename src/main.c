#include "include/ast.h"
#include "include/chunk.h"
#include "include/common.h"
#include "include/compiler.h"
#include "include/debug.h"
#include "include/io.h"
#include "include/native_exec.h"
#include "include/vm.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <string.h>

// #include <X11/Xlib.h>
#include <sys/stat.h>
#include <unistd.h>

struct stat statbuf;
pthread_mutex_t GVL;

typedef struct
{
    char **items;
    int count;
    int capacity;
} StrList;

static bool replNeedsMoreInput(const char *src)
{
    int paren = 0;
    int bracket = 0;
    int brace = 0;
    bool inSingle = false;
    bool inDouble = false;
    bool inRaw = false;
    bool inLineComment = false;
    bool inBlockComment = false;
    bool escaped = false;

    for (const char *p = src; *p != '\0'; p++)
    {
        char c = *p;

        if (inLineComment)
        {
            if (c == '\n')
                inLineComment = false;
            continue;
        }

        if (inBlockComment)
        {
            if (c == '~')
                inBlockComment = false;
            continue;
        }

        if (inSingle)
        {
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (c == '\\')
            {
                escaped = true;
                continue;
            }
            if (c == '\'')
                inSingle = false;
            continue;
        }

        if (inDouble)
        {
            if (escaped)
            {
                escaped = false;
                continue;
            }
            if (c == '\\')
            {
                escaped = true;
                continue;
            }
            if (c == '"')
                inDouble = false;
            continue;
        }

        if (inRaw)
        {
            if (c == '`')
                inRaw = false;
            continue;
        }

        if (c == '#')
        {
            inLineComment = true;
            continue;
        }
        if (c == '~')
        {
            inBlockComment = true;
            continue;
        }

        if (c == '\'')
        {
            inSingle = true;
            continue;
        }
        if (c == '"')
        {
            inDouble = true;
            continue;
        }
        if (c == '`')
        {
            inRaw = true;
            continue;
        }

        switch (c)
        {
        case '(':
            paren++;
            break;
        case ')':
            if (paren > 0)
                paren--;
            break;
        case '[':
            bracket++;
            break;
        case ']':
            if (bracket > 0)
                bracket--;
            break;
        case '{':
            brace++;
            break;
        case '}':
            if (brace > 0)
                brace--;
            break;
        default:
            break;
        }
    }

    return paren > 0 || bracket > 0 || brace > 0 || inSingle || inDouble || inRaw || inBlockComment;
}

typedef struct
{
    char name[256];
    int passed;
    int failed;
    int total;
} ModuleSummary;

static void repl(int argC, char **argV, bool piped, bool runShellAfter)
{
    enableRawMode();
    char line[1024];
    Stack *line_stack = &(Stack){.current_line = NULL, .size = 0};
    init_stack(line_stack);

    size_t sourceCap = 1024;
    char *source = (char *)malloc(sourceCap);
    source[0] = '\0';
    size_t sourceLen = 0;
    bool awaitingMore = false;

    if (!piped && !runShellAfter)
        printf("DotK Interactive Shell\n");
    for (;;)
    {
        if (likely(!piped))
            printf(awaitingMore ? "... " : "> ");

        handleInput(line, line_stack, piped);
        if (likely(!piped))
            printf("\n");

        size_t lineLen = strlen(line);
        if (sourceLen + lineLen + 1 > sourceCap)
        {
            while (sourceLen + lineLen + 1 > sourceCap)
                sourceCap *= 2;
            source = (char *)realloc(source, sourceCap);
        }
        memcpy(source + sourceLen, line, lineLen + 1);
        sourceLen += lineLen;

        if (sourceLen == 0 || (sourceLen == 1 && source[0] == '\n'))
        {
            awaitingMore = false;
            source[0] = '\0';
            sourceLen = 0;
            continue;
        }

        if (replNeedsMoreInput(source))
        {
            awaitingMore = true;
            continue;
        }

        interpret(source, "stdin", true, argC, argV);
        awaitingMore = false;
        source[0] = '\0';
        sourceLen = 0;
    }
    free(source);
    disableRawMode();
}

static void runFile(char *path, int argC, char **argV)
{
    char *source = readFileAndExit(path);
    InterpretResult result = interpret(source, path, false, argC, argV);
    free(source);
    if (result == INTERPRET_COMPILE_ERROR)
        exit(65);
    if (result == INTERPRET_RUNTIME_ERROR)
        exit(70);
}

static InterpretResult runFileResult(char *path, int argC, char **argV)
{
    char *source = readFileAndExit(path);
    InterpretResult result = interpret(source, path, false, argC, argV);
    free(source);
    return result;
}

static bool hasKExt(const char *name)
{
    const char *dot = strrchr(name, '.');
    return dot != NULL && strcmp(dot, ".k") == 0;
}

static char *trimTrailingNewLinesDup(const char *s)
{
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r'))
        len--;
    char *out = (char *)malloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static bool lineEquals(const char *lineStart, int lineLen, const char *text)
{
    int textLen = (int)strlen(text);
    if (lineLen != textLen)
        return false;
    return memcmp(lineStart, text, (size_t)lineLen) == 0;
}

static char *extractExpectedOutput(const char *source)
{
    const char *start = NULL;
    const char *end = NULL;

    const char *line = source;
    while (*line)
    {
        const char *nl = strchr(line, '\n');
        int lineLen = nl ? (int)(nl - line) : (int)strlen(line);

        if (lineEquals(line, lineLen, "# EXPECTED") || lineEquals(line, lineLen, "# --- EXPECTED ---"))
        {
            start = nl ? nl + 1 : line + lineLen;
            break;
        }
        if (!nl)
            break;
        line = nl + 1;
    }

    if (start == NULL)
        return NULL;

    line = start;
    while (*line)
    {
        const char *nl = strchr(line, '\n');
        int lineLen = nl ? (int)(nl - line) : (int)strlen(line);

        if (lineEquals(line, lineLen, "# END_EXPECTED") || lineEquals(line, lineLen, "# --- END EXPECTED ---"))
        {
            end = line;
            break;
        }
        if (!nl)
            break;
        line = nl + 1;
    }

    if (end == NULL)
        return NULL;

    size_t cap = (size_t)(end - start) + 1;
    char *result = (char *)malloc(cap);
    size_t outLen = 0;

    line = start;
    while (line < end)
    {
        const char *nl = strchr(line, '\n');
        if (nl == NULL || nl > end)
            nl = end;
        int lineLen = (int)(nl - line);

        if (lineLen >= 1 && line[0] == '#')
        {
            int offset = 1;
            if (lineLen >= 2 && line[1] == ' ')
                offset = 2;
            int copyLen = lineLen - offset;
            if (copyLen > 0)
            {
                memcpy(result + outLen, line + offset, (size_t)copyLen);
                outLen += (size_t)copyLen;
            }
            result[outLen++] = '\n';
        }
        else if (lineLen == 0)
        {
            result[outLen++] = '\n';
        }

        if (nl == end)
            break;
        line = nl + 1;
    }

    result[outLen] = '\0';
    return result;
}

static int runTestFile(const char *path, const char *prefix)
{
    if (prefix == NULL)
        prefix = "";
    char *source = readFile(path);
    if (source == NULL)
    {
        fprintf(stderr, "%s[FAIL] %s (unable to read file)\n", prefix, path);
        return 1;
    }

    char *expectedRaw = extractExpectedOutput(source);
    if (expectedRaw == NULL)
    {
        fprintf(stderr, "%s[FAIL] %s (missing expected block)\n", prefix, path);
        fprintf(stderr, "%s       Add:\n%s       # EXPECTED\n%s       # ...\n%s       # END_EXPECTED\n", prefix, prefix, prefix, prefix);
        free(source);
        return 1;
    }
    free(source);

    fflush(stdout);
    int originalStdout = dup(STDOUT_FILENO);
    FILE *capture = tmpfile();
    if (capture == NULL || originalStdout < 0)
    {
        fprintf(stderr, "%s[FAIL] %s (failed to capture stdout)\n", prefix, path);
        free(expectedRaw);
        if (capture)
            fclose(capture);
        if (originalStdout >= 0)
            close(originalStdout);
        return 1;
    }

    dup2(fileno(capture), STDOUT_FILENO);
    InterpretResult result = runFileResult((char *)path, 0, NULL);
    fflush(stdout);
    dup2(originalStdout, STDOUT_FILENO);
    close(originalStdout);

    fseek(capture, 0L, SEEK_END);
    long sz = ftell(capture);
    rewind(capture);
    char *actualRaw = (char *)malloc((size_t)sz + 1);
    size_t readN = fread(actualRaw, 1, (size_t)sz, capture);
    actualRaw[readN] = '\0';
    fclose(capture);

    char *expected = trimTrailingNewLinesDup(expectedRaw);
    char *actual = trimTrailingNewLinesDup(actualRaw);
    free(expectedRaw);
    free(actualRaw);

    bool passed = (result == INTERPRET_OK) && strcmp(expected, actual) == 0;
    if (passed)
    {
        fprintf(stderr, "%s[PASS] %s\n", prefix, path);
        free(expected);
        free(actual);
        return 0;
    }

    fprintf(stderr, "%s[FAIL] %s\n", prefix, path);
    if (result == INTERPRET_COMPILE_ERROR)
        fprintf(stderr, "%s       Interpreter result: COMPILE_ERROR\n", prefix);
    else if (result == INTERPRET_RUNTIME_ERROR)
        fprintf(stderr, "%s       Interpreter result: RUNTIME_ERROR\n", prefix);

    fprintf(stderr, "%s------- EXPECTED -------\n%s\n", prefix, expected);
    fprintf(stderr, "%s--------- ACTUAL --------\n%s\n", prefix, actual);

    free(expected);
    free(actual);
    return 1;
}

static void strListInit(StrList *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void strListAdd(StrList *list, const char *item)
{
    if (list->count + 1 > list->capacity)
    {
        int oldCap = list->capacity;
        list->capacity = oldCap < 8 ? 8 : oldCap * 2;
        list->items = (char **)realloc(list->items, sizeof(char *) * (size_t)list->capacity);
    }
    list->items[list->count++] = strdup(item);
}

static int cmpStrPtr(const void *a, const void *b)
{
    const char *sa = *(const char *const *)a;
    const char *sb = *(const char *const *)b;
    return strcmp(sa, sb);
}

static void collectTestsRecursive(const char *dirPath, StrList *files)
{
    DIR *dir = opendir(dirPath);
    if (dir == NULL)
        return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dirPath, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
        {
            collectTestsRecursive(path, files);
            continue;
        }

        if (S_ISREG(st.st_mode) && hasKExt(entry->d_name))
            strListAdd(files, path);
    }
    closedir(dir);
}

static void moduleNameFromPath(const char *root, const char *fullPath, char *out, size_t outSize)
{
    if (outSize == 0)
        return;

    out[0] = '\0';
    size_t rootLen = strlen(root);
    const char *rel = fullPath;
    if (strncmp(fullPath, root, rootLen) == 0)
    {
        rel = fullPath + rootLen;
        if (*rel == '/')
            rel++;
    }

    const char *slash = strchr(rel, '/');
    if (slash == NULL)
    {
        snprintf(out, outSize, "ROOT");
        return;
    }

    size_t len = (size_t)(slash - rel);
    if (len >= outSize)
        len = outSize - 1;

    memcpy(out, rel, len);
    out[len] = '\0';
    for (size_t i = 0; i < len; i++)
        out[i] = (char)toupper((unsigned char)out[i]);
}

static void strListFree(StrList *list)
{
    for (int i = 0; i < list->count; i++)
        free(list->items[i]);
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int runTestFileSubprocess(const char *exePath, const char *path)
{
    pid_t pid = fork();
    if (pid < 0)
        return 2;

    if (pid == 0)
    {
        execl(exePath, exePath, "--test", path, (char *)NULL);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return 2;

    if (WIFEXITED(status))
        return WEXITSTATUS(status) == 0 ? 0 : 1;
    return 1;
}

static int runAllTestsInDir(const char *dirPath, bool stopOnFail, const char *exePath)
{
    DIR *dir = opendir(dirPath);
    if (dir == NULL)
    {
        fprintf(stderr, "Failed to open test directory '%s': %s\n", dirPath, strerror(errno));
        return 2;
    }

    StrList files;
    strListInit(&files);
    collectTestsRecursive(dirPath, &files);
    closedir(dir);

    qsort(files.items, (size_t)files.count, sizeof(char *), cmpStrPtr);

    int failed = 0;
    int passed = 0;

    ModuleSummary *moduleSummaries = files.count > 0 ? (ModuleSummary *)calloc((size_t)files.count, sizeof(ModuleSummary)) : NULL;
    int moduleSummaryCount = 0;

    char currentModule[256] = {0};
    int modulePassed = 0;
    int moduleFailed = 0;
    int moduleTotal = 0;

    for (int i = 0; i < files.count; i++)
    {
        char module[256] = {0};
        moduleNameFromPath(dirPath, files.items[i], module, sizeof(module));

        if (i == 0 || strcmp(module, currentModule) != 0)
        {
            if (i != 0)
            {
                fprintf(stderr, "Summary: %d passed, %d failed (%d total)\n\n", modulePassed, moduleFailed, moduleTotal);

                if (moduleSummaries != NULL)
                {
                    ModuleSummary *ms = &moduleSummaries[moduleSummaryCount++];
                    snprintf(ms->name, sizeof(ms->name), "%s", currentModule);
                    ms->passed = modulePassed;
                    ms->failed = moduleFailed;
                    ms->total = moduleTotal;
                }
            }

            strcpy(currentModule, module);
            modulePassed = 0;
            moduleFailed = 0;
            moduleTotal = 0;

            fprintf(stderr, "%s:\n", currentModule);
        }

        int testFailed = runTestFileSubprocess(exePath, files.items[i]);

        moduleTotal++;
        if (testFailed)
        {
            failed++;
            moduleFailed++;
        }
        else
        {
            passed++;
            modulePassed++;
        }

        if (stopOnFail && testFailed)
            break;
    }

    if (files.count > 0)
    {
        fprintf(stderr, "Summary: %d passed, %d failed (%d total)\n", modulePassed, moduleFailed, moduleTotal);

        if (moduleSummaries != NULL)
        {
            ModuleSummary *ms = &moduleSummaries[moduleSummaryCount++];
            snprintf(ms->name, sizeof(ms->name), "%s", currentModule);
            ms->passed = modulePassed;
            ms->failed = moduleFailed;
            ms->total = moduleTotal;
        }
    }

    if (moduleSummaryCount > 0)
    {
        fprintf(stderr, "\nAggregated module summary:\n");
        for (int i = 0; i < moduleSummaryCount; i++)
        {
            ModuleSummary *ms = &moduleSummaries[i];
            fprintf(stderr, "  %-12s %4d passed, %4d failed, %4d total\n", ms->name, ms->passed, ms->failed, ms->total);
        }
    }

    fprintf(stderr, "\nTotal summary: %d passed, %d failed (%d total)\n", passed, failed, passed + failed);
    free(moduleSummaries);
    strListFree(&files);
    return failed == 0 ? 0 : 1;
}

static void printUsage()
{
    fprintf(stderr,
            "Usage: dotk <file.k>\n"
            "Flags: \n"
            "   -i:          Run the file and start the interactive shell\n"
            "   --help:      Print this message (if set, all other flags will be ignored)\n"
            "   --pbytecode: Print the bytecode\n"
            "   --pexec:     Print the execution stack\n"
            "   --debug:     Start interactive VM debugger\n"
            "   --dump-ast:  Parse file and print bootstrap AST tree\n"
            "   --dump-sema: Parse file and print typed scope/symbol summary\n"
            "   --dump-sema-json: Parse file and print semantic model as JSON\n"
            "   --type-check: Enable optional AST-based type warnings\n"
            "   --type-check-strict: Fail compile when type checker finds issues\n"
            "   --native:    Try native machine-code execution first, fallback to VM\n"
            "   --native-out <bin>: AOT compile file.k to standalone native executable\n"
            "   --native-keep-tmp: Keep generated AOT C source file in /tmp\n"
            "   --pnative-ir: Print native backend IR for each compiled unit\n"
            "   -O0/-O1/-O2: Set compiler optimization level (default: -O1)\n"
            "   --opt <n>:   Set compiler optimization level [0..2]\n"
            "   --test <file.k>: Run one test file and compare output with # EXPECTED block\n"
            "   --test-all [dir]: Run all .k tests in directory (default: tests)\n"
            "   --stop-on-fail: Stop test suite at first failure (with --test-all)\n"
            "Anything after the last flag / file will be passed to the program\n");
    exit(64);
}

int main(int argc, char *argv[])
{
    // TODO: get gui to work with WSL
    // XEvent event = {0};
    // Display *display = XOpenDisplay(NULL);
    // if (display == NULL)
    // {
    //     fprintf(stderr, "Cannot open display\n");
    //     exit(1);
    // }
    // Window w = XCreateSimpleWindow(display, DefaultRootWindow(display), 50, 50, 250, 250, 1, BlackPixel(display, 0), WhitePixel(display, 0));

    // XSelectInput(display, w, ExposureMask | KeyPressMask);

    // XMapWindow(display, w);

    // XSync(display, 0);

    // for (;;)
    // {
    //     XNextEvent(display, &event);
    //     if (event.type == Expose)
    //     {
    //         XDrawString(display, w, DefaultGC(display, 0), 10, 20, "Hello World", 11);
    //     }
    //     else if (event.type == KeyPress)
    //     {
    //         printf("Key Pressed\n");
    //     }
    // }
    // XDestroyWindow(display, w);
    // XCloseDisplay(display);

    int printBytecode = -1;
    int printExecStack = -1;
    int file = -1;
    int runShellAfter = -1;
    int debugFlag = -1;
    int optFlag = -1;
    int optValueFlag = -1;
    int optimizeLevel = 1;
    int testFlag = -1;
    int testAllFlag = -1;
    int stopOnFailFlag = -1;
    int dumpAstFlag = -1;
    int dumpSemaFlag = -1;
    int dumpSemaJsonFlag = -1;
    int typeCheckFlag = -1;
    int typeCheckStrictFlag = -1;
    int nativeFlag = -1;
    int nativeOutFlag = -1;
    int nativeOutValueFlag = -1;
    int nativeKeepTmpFlag = -1;
    int printNativeIrFlag = -1;
    bool piped = ((fstat(0, &statbuf) == 0) && (S_ISFIFO(statbuf.st_mode) || S_ISREG(statbuf.st_mode)));

    for (int i = 1; i < argc; i++)
    {
        if (strncmp(argv[i], "--help", 7) == 0)
        {
            printUsage();
            exit(0);
        }
        char *ex = strrchr(argv[i], '.');
        if (strncmp(argv[i], "--pbytecode", 12) == 0)
            printBytecode = i;
        else if (strncmp(argv[i], "--pexec", 8) == 0)
            printExecStack = i;
        else if (strncmp(argv[i], "--debug", 8) == 0)
            debugFlag = i;
        else if (strncmp(argv[i], "--dump-ast", 11) == 0)
            dumpAstFlag = i;
        else if (strncmp(argv[i], "--dump-sema", 12) == 0)
            dumpSemaFlag = i;
        else if (strncmp(argv[i], "--dump-sema-json", 17) == 0)
            dumpSemaJsonFlag = i;
        else if (strncmp(argv[i], "--type-check-strict", 20) == 0)
            typeCheckStrictFlag = i;
        else if (strncmp(argv[i], "--type-check", 13) == 0)
            typeCheckFlag = i;
        else if (strncmp(argv[i], "--native-out=", 13) == 0)
        {
            nativeOutFlag = i;
            nativeOutValueFlag = i;
        }
        else if (strncmp(argv[i], "--native-out", 13) == 0)
        {
            nativeOutFlag = i;
            if (i + 1 < argc && argv[i + 1][0] != '-')
                nativeOutValueFlag = i + 1;
        }
        else if (strncmp(argv[i], "--native-keep-tmp", 18) == 0)
            nativeKeepTmpFlag = i;
        else if (strcmp(argv[i], "--native") == 0)
            nativeFlag = i;
        else if (strncmp(argv[i], "--pnative-ir", 13) == 0)
            printNativeIrFlag = i;
        else if (strncmp(argv[i], "-O", 2) == 0 && strlen(argv[i]) == 3 && isdigit((unsigned char)argv[i][2]))
        {
            optimizeLevel = argv[i][2] - '0';
            optFlag = i;
        }
        else if (strncmp(argv[i], "--opt=", 6) == 0)
        {
            optimizeLevel = atoi(argv[i] + 6);
            optFlag = i;
        }
        else if (strncmp(argv[i], "--opt", 6) == 0)
        {
            optFlag = i;
            if (i + 1 < argc && argv[i + 1][0] != '-')
            {
                optimizeLevel = atoi(argv[i + 1]);
                optValueFlag = i + 1;
            }
        }
        else if (strncmp(argv[i], "--test", 7) == 0)
            testFlag = i;
        else if (strncmp(argv[i], "--test-all", 11) == 0)
            testAllFlag = i;
        else if (strncmp(argv[i], "--stop-on-fail", 15) == 0)
            stopOnFailFlag = i;
        else if (strncmp(argv[i], "-i", 3) == 0)
            runShellAfter = i;
        else if (ex != NULL && strcmp(ex, ".k") == 0)
            file = i;
    }
    pthread_mutex_init(&GVL, NULL);

    compileSetOptimizationLevel(optimizeLevel);
    if (typeCheckStrictFlag != -1)
        compileSetTypeCheckMode(2);
    else if (typeCheckFlag != -1)
        compileSetTypeCheckMode(1);
    else
        compileSetTypeCheckMode(0);

    if (testAllFlag != -1)
    {
        const char *dir = "tests";
        if (testAllFlag + 1 < argc && argv[testAllFlag + 1][0] != '-')
            dir = argv[testAllFlag + 1];

        int status = runAllTestsInDir(dir, stopOnFailFlag != -1, argv[0]);
        pthread_mutex_destroy(&GVL);
        return status;
    }

    if (dumpAstFlag != -1 || dumpSemaFlag != -1 || dumpSemaJsonFlag != -1)
    {
        if (file == -1)
        {
            fprintf(stderr, "--dump-ast/--dump-sema/--dump-sema-json requires a .k file path\n");
            pthread_mutex_destroy(&GVL);
            return 64;
        }

        char *source = readFileAndExit(argv[file]);
        char errBuf[256] = {0};
        bool ok = false;
        if (dumpSemaJsonFlag != -1)
            ok = astDumpSemanticJsonFromSource(source, argv[file], stdout, errBuf, sizeof(errBuf));
        else if (dumpSemaFlag != -1)
            ok = astDumpSemanticFromSource(source, argv[file], stdout, errBuf, sizeof(errBuf));
        else
            ok = astDumpFromSource(source, argv[file], stdout, errBuf, sizeof(errBuf));
        free(source);

        if (!ok)
        {
            fprintf(stderr, "%s:0:0 Error: AST build failed: %s\n", argv[file], errBuf[0] == '\0' ? "unknown error" : errBuf);
            pthread_mutex_destroy(&GVL);
            return 65;
        }

        pthread_mutex_destroy(&GVL);
        return 0;
    }

    initVM(printBytecode != -1, printExecStack != -1);
    vmSetNativeExecution(nativeFlag != -1);
    vmSetNativePrintIr(printNativeIrFlag != -1);
    if (nativeFlag != -1 && !vmNativeExecutionAvailable())
        fprintf(stderr, "[dotk] --native requested but unsupported on this platform; using VM fallback.\n");

    if (nativeOutFlag != -1)
    {
        if (file == -1)
        {
            fprintf(stderr, "--native-out requires a .k source file\n");
            freeVM();
            pthread_mutex_destroy(&GVL);
            return 64;
        }

        const char *outPath = NULL;
        if (nativeOutValueFlag == nativeOutFlag)
            outPath = argv[nativeOutFlag] + 13;
        else if (nativeOutValueFlag != -1)
            outPath = argv[nativeOutValueFlag];

        if (outPath == NULL || outPath[0] == '\0')
        {
            fprintf(stderr, "--native-out requires an output binary path\n");
            freeVM();
            pthread_mutex_destroy(&GVL);
            return 64;
        }

        char *source = readFileAndExit(argv[file]);
        ObjFunction *function = compile(source, argv[file], false, printBytecode != -1);
        free(source);
        if (function == NULL)
        {
            freeVM();
            pthread_mutex_destroy(&GVL);
            return 65;
        }

        if (printNativeIrFlag != -1)
            nativeDumpFunctionIr(function, stderr);

        char errBuf[256] = {0};
        char keptPath[PATH_MAX] = {0};
        if (!nativeAotWriteExecutable(function,
                                      outPath,
                                      nativeKeepTmpFlag != -1,
                                      keptPath,
                                      sizeof(keptPath),
                                      errBuf,
                                      sizeof(errBuf)))
        {
            fprintf(stderr, "[dotk] native AOT failed: %s\n", errBuf[0] == '\0' ? "unknown error" : errBuf);
            freeVM();
            pthread_mutex_destroy(&GVL);
            return 70;
        }

        fprintf(stderr, "[dotk] native AOT executable written to '%s'\n", outPath);
        if (nativeKeepTmpFlag != -1 && keptPath[0] != '\0')
            fprintf(stderr, "[dotk] kept AOT source at '%s'\n", keptPath);
        freeVM();
        pthread_mutex_destroy(&GVL);
        return 0;
    }

    if (debugFlag != -1)
        vmEnableDebugger(true);

    if (testFlag != -1)
    {
        if (testFlag + 1 >= argc)
        {
            fprintf(stderr, "--test expects a .k file path\n");
            freeVM();
            pthread_mutex_destroy(&GVL);
            return 64;
        }
        int status = runTestFile(argv[testFlag + 1], "") == 0 ? 0 : 1;
        freeVM();
        pthread_mutex_destroy(&GVL);
        return status;
    }

    int last = 0;

    // get the max of the three
    if (printBytecode > last)
        last = printBytecode;
    if (printExecStack > last)
        last = printExecStack;
    if (debugFlag > last)
        last = debugFlag;
    if (dumpAstFlag > last)
        last = dumpAstFlag;
    if (dumpSemaFlag > last)
        last = dumpSemaFlag;
    if (dumpSemaJsonFlag > last)
        last = dumpSemaJsonFlag;
    if (typeCheckFlag > last)
        last = typeCheckFlag;
    if (typeCheckStrictFlag > last)
        last = typeCheckStrictFlag;
    if (nativeFlag > last)
        last = nativeFlag;
    if (nativeOutFlag > last)
        last = nativeOutFlag;
    if (nativeOutValueFlag > last)
        last = nativeOutValueFlag;
    if (nativeKeepTmpFlag > last)
        last = nativeKeepTmpFlag;
    if (printNativeIrFlag > last)
        last = printNativeIrFlag;
    if (optFlag > last)
        last = optFlag;
    if (optValueFlag > last)
        last = optValueFlag;
    if (runShellAfter > last)
        last = runShellAfter;
    if (file > last)
        last = file;
    last++;
    if (file == -1)
        repl(argc - last, argv + last, piped, false);
    else
    {
        runFile(argv[file], argc - last, argv + last);
        if (runShellAfter != -1)
            repl(argc - last, argv + last, piped, true);
    }
    freeVM();
    pthread_mutex_destroy(&GVL);

    return 0;
}