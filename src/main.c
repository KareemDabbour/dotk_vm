#include "include/chunk.h"
#include "include/common.h"
#include "include/debug.h"
#include "include/io.h"
#include "include/vm.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

struct stat statbuf;

static void repl(int argC, char **argV, bool piped)
{
    enableRawMode();
    char line[1024];
    Stack *line_stack = &(Stack){.current_line = NULL, .size = 0};
    init_stack(line_stack);
    if (!piped)
        printf("DotK Interactive Shell\n");
    for (;;)
    {
        if (__glibc_likely(!piped))
            printf("> ");

        handleInput(line, line_stack, piped);
        if (__glibc_likely(!piped))
            printf("\n");
        interpret(line, "stdin", true, argC, argV);
    }
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

static void printUsage()
{
    fprintf(stderr,
            "Usage: dotk <file.k>\n"
            "Flags: \n"
            "   --help:      Print this message (if set, all other flags will be ignored)\n"
            "   --pbytecode: Print the bytecode\n"
            "   --pexec:     Print the execution stack\n"
            "Anything after the last flag / file will be passed to the program\n");
    exit(64);
}

int main(int argc, char *argv[])
{

    int printBytecode = -1;
    int printExecStack = -1;
    int file = -1;
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
        else if (ex != NULL && strcmp(ex, ".k") == 0)
            file = i;
    }
    initVM(printBytecode != -1, printExecStack != -1);
    int last = 0;

    // get the max of the three
    if (printBytecode > last)
        last = printBytecode;
    if (printExecStack > last)
        last = printExecStack;
    if (file > last)
        last = file;
    last++;
    if (file == -1)
        repl(argc - last, argv + last, piped);
    else
        runFile(argv[file], argc - last, argv + last);

    freeVM();
    return 0;
}