#include "include/chunk.h"
#include "include/common.h"
#include "include/debug.h"
#include "include/io.h"
#include "include/vm.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include <unistd.h>

///
// Function to set terminal to non-canonical mode

static void repl()
{
    enableRawMode();
    char line[1024];
    Stack *line_stack = &(Stack){.current_line = NULL, .size = 0};
    init_stack(line_stack);
    printf("DotK Interactive Shell\n");
    for (;;)
    {
        printf("> ");

        handleInput(line, line_stack);

        printf("\n");
        interpret(line, "stdin", true);
    }
    disableRawMode();
}

static void runFile(char *path)
{
    char *source = readFileAndExit(path);
    InterpretResult result = interpret(source, path, false);
    free(source);
    if (result == INTERPRET_COMPILE_ERROR)
        exit(65);
    if (result == INTERPRET_RUNTIME_ERROR)
        exit(70);
}

static void printUsage()
{
    fprintf(stderr, "Usage: dotk optional <file.k>\n");
    exit(64);
}

int main(int argc, char *argv[])
{
    initVM();

    if (argc == 1)
        repl();
    else if (argc == 2)
        runFile(argv[1]);
    else
        printUsage();

    freeVM();
    return 0;
}