#include "include/chunk.h"
#include "include/common.h"
#include "include/debug.h"
#include "include/io.h"
#include "include/stack.h"
#include "include/vm.h"
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

///
// Function to set terminal to non-canonical mode
void enableRawMode()
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &raw);
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

void move_cursor_right(int n)
{
    printf("\033[%dC", n);
}

void move_cursor_left(int n)
{
    printf("\033[%dD", n);
}

// Function to disable raw mode and restore terminal settings
void disableRawMode()
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &raw);
    raw.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

// Function to handle user input and editing
void handleInput(char *line, Stack *stack)
{
    int position = 0;
    int lineLen = 0;
    line[1] = '\0';
    bool cachedLine = false;
    for (;;)
    {
        int c = getchar();

        if (c == '\033')
        { // Escape key (arrow keys)
            c = getchar();
            if (c == '[')
            {
                c = getchar();
                if (c == 'D' && position > 0) // Left arrow
                {
                    move_cursor_left(1);
                    position--;
                }
                else if (c == 'C' && position < lineLen) // Right arrow
                {
                    move_cursor_right(1); // Move cursor right
                    position++;
                }
                else if (c == 'A') // Up arrow
                {
                    char *prevLine;
                    if (lineLen != 0)
                    {
                        // if (!cachedLine)
                        // {
                        //     line[lineLen] = '\0';
                        //     push_line(stack, line);
                        //     cachedLine = true;
                        // }
                        prevLine = get_prev_line(stack);
                    }
                    else
                        prevLine = get_curr_line(stack);
                    if (prevLine != NULL)
                    {
                        lineLen = strlen(prevLine);
                        if (position != 0)
                            move_cursor_left(position);
                        printf("\033[K");
                        memcpy(line, prevLine, lineLen + 1);
                        printf("%s", line);
                        position = lineLen;
                    }
                }
                else if (c == 'B') // Down arrow
                {
                    char *nextLine;
                    if (lineLen != 0)
                    {
                        nextLine = get_next_line(stack);
                        // if (!cachedLine)
                        // {
                        //     line[lineLen] = '\0';
                        //     push_line(stack, line); // TODO make a backwards push
                        //     cachedLine = true;
                        // }
                    }
                    else
                        nextLine = get_curr_line(stack);
                    if (nextLine != NULL)
                    {
                        lineLen = strlen(nextLine);
                        if (position != 0)
                            move_cursor_left(position);
                        printf("\033[K");
                        memcpy(line, nextLine, lineLen + 1);
                        printf("%s", line);
                        // printf("\033[%dC", lineLen);
                        position = lineLen;
                    }
                    else
                    {
                        if (position != 0)
                            move_cursor_left(position);
                        printf("\033[K");
                        memset(line, '\0', 10);
                        position = 0;
                        lineLen = 0;
                    }
                }
                else if (c == '3') // Del key
                {
                    c = getchar();
                    if (position == lineLen)
                        continue;
                    memmove(line + position, line + position + 1, lineLen - position + 1);
                    lineLen--;
                    printf("\033[K"); // Clear line from cursor
                    printf("%s", line + position);
                    if (position != lineLen)
                        move_cursor_left(lineLen - position);
                }
            }
        }
        else if (c == 127 || c == 8)
        {
            if (position <= 0)
                continue;
            cachedLine = false;
            if (position != lineLen)
                memmove(line + position - 1, line + position, lineLen - position + 1);
            else
                line[position - 1] = '\0';
            position--;
            lineLen--;
            printf("\033[1D");
            printf("\033[K"); // Clear line from cursor
            printf("%s", line + position);
            if (position != lineLen)
                move_cursor_left(lineLen - position);
        }
        else if (c >= 32 && c <= 126)
        { // Printable characters
            // Shift characters to the right to simulate insertion
            if (lineLen > position)
                memmove(line + position + 1, line + position, lineLen - position);
            line[position++] = c;
            line[++lineLen] = '\0';
            cachedLine = false;

            // Print updated line and move cursor
            // if (lineLen > position)
            // {
            printf("\033[K"); // Clear line from cursor
            printf("%s", line + position - 1);
            if (lineLen > position)
                printf("\033[%dD", lineLen - position); // Move cursor left
            // }
        }
        else if (c == '\n')
        {
            if (lineLen != 0)
            {
                // memcpy(saved_line, line, lineLen);
                // saved_line[lineLen] = '\0';
                line[lineLen] = '\0';
                push_line(stack, line);
            }
            line[lineLen + 1] = '\0';
            line[lineLen] = '\n';
            break;
        }
    }
}

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

        // printf("\n");
        // if (!fgets(line, sizeof(line), stdin))
        // {
        //     printf("\n");
        //     break;
        // }

        printf("\n");
        interpret(line, "stdin");
    }
    disableRawMode();
}

static void runFile(char *path)
{
    char *source = readFileAndExit(path);
    InterpretResult result = interpret(source, path);
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