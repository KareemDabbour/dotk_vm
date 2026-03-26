#ifndef dotk_io_h
#define dotk_io_h
#include <unistd.h>

#include "stack.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>

static char *readFile(const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL)
    {
        // fprintf(stderr, "Could not open file \"%s\".\n", path);
        return NULL;
    }
    fseek(file, 0L, SEEK_END);
    size_t fileSize = ftell(file);
    rewind(file);

    char *buffer = (char *)malloc(fileSize + 1);
    if (buffer == NULL)
    {
        fprintf(stderr, "Not enough memory to read file \"%s\".", path);
        fclose(file);
        return NULL;
    }
    size_t bytesRead = fread(buffer, sizeof(char), fileSize, file);
    if (bytesRead < fileSize)
    {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        free(buffer);
        fclose(file);
        return NULL;
    }

    buffer[bytesRead] = '\0';

    fclose(file);
    return buffer;
}

static char *readFileAndExit(const char *path)
{
    char *source = readFile(path);
    if (source == NULL)
        exit(74);

    return source;
}

// Function to set terminal to non-canonical mode
static void enableRawMode()
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &raw);
    raw.c_lflag &= ~(ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void move_cursor_right(int n)
{
    printf("\033[%dC", n);
}

static void move_cursor_left(int n)
{
    printf("\033[%dD", n);
}

// Function to disable raw mode and restore terminal settings
static void disableRawMode()
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &raw);
    raw.c_lflag |= (ECHO | ICANON);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static bool is_ctrl_arrow_char_term(char c)
{
    return c == ' ' || c == '.' || c == '_' || c == '-' || c == '+' || c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' || c == '<' || c == '>';
}

// Function to handle user input and editing
static void handleInput(char *line, Stack *stack, bool piped)
{
    int position = 0;
    int lineLen = 0;
    line[1] = '\0';
    bool cachedLine = false;
    for (;;)
    {
        int c = getchar();

        if (c == EOF)
        {
            disableRawMode();
            exit(0);
        }

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
                        prevLine = get_prev_line(stack);
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
                        nextLine = get_next_line(stack);
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
                else if (c == '1' && (c = getchar() == ';')) // ctrl + arrow keys
                {
                    c = getchar();

                    if (c == '5')
                    {
                        c = getchar();
                        if (c == 'D') // left arrow
                        {
                            int spaces = 0;
                            while ((position - spaces) > 0 && is_ctrl_arrow_char_term(line[(position - spaces) - 1]))
                                spaces++;
                            while ((position - spaces) > 0 && !is_ctrl_arrow_char_term(line[(position - spaces) - 1]))
                                spaces++;
                            position -= spaces;
                            if (spaces > 0)
                                move_cursor_left(spaces);
                        }
                        else if (c == 'C') // right arrow
                        {
                            int spaces = 0;
                            while ((position + spaces) < lineLen && is_ctrl_arrow_char_term(line[position + spaces]))
                                spaces++;
                            while ((position + spaces) < lineLen && !is_ctrl_arrow_char_term(line[position + spaces]))
                                spaces++;
                            position += spaces;
                            if (spaces > 0)
                                move_cursor_right(spaces);
                        }
                    }
                }
            }
            else if (c == 'd') // ctrl + del
            {
                if (position == lineLen)
                    continue;
                int spaces = 0;
                while ((position + spaces) < lineLen && is_ctrl_arrow_char_term(line[position + spaces]))
                    spaces++;
                while ((position + spaces) < lineLen && !is_ctrl_arrow_char_term(line[position + spaces]))
                    spaces++;
                if (spaces <= 0)
                    continue;
                memmove(line + position, line + position + spaces, lineLen - position + spaces);
                // position += spaces;
                lineLen -= spaces;
                printf("\033[K"); // Clear line from cursor
                printf("%s", line + position);
                if (position != lineLen)
                    move_cursor_left(lineLen - position);
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
        else if (c >= 32 && c <= 126) // Printable characters
        {
            // Shift characters to the right to simulate insertion
            if (lineLen > position)
                memmove(line + position + 1, line + position, lineLen - position);
            line[position++] = c;
            line[++lineLen] = '\0';
            cachedLine = false;
            if (!piped)
            {
                // Print updated line and move cursor
                printf("\033[K"); // Clear line from cursor
                printf("%s", line + position - 1);
                if (lineLen > position)
                    printf("\033[%dD", lineLen - position); // Move cursor left
            }
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
        else if (c == 23) // ctrl - backspace
        {
            if (position <= 0)
                continue;

            int spaces = 0;
            while ((position - spaces) > 0 && is_ctrl_arrow_char_term(line[(position - spaces) - 1]))
                spaces++;
            while ((position - spaces) > 0 && !is_ctrl_arrow_char_term(line[(position - spaces) - 1]))
                spaces++;
            if (spaces <= 0)
                continue;

            cachedLine = false;
            if (position != lineLen)
                memmove(line + position - spaces, line + position, lineLen - position + spaces);
            else
                line[position - spaces] = '\0';
            // position--;
            position -= spaces;
            lineLen -= spaces;
            printf("\033[%dD", spaces);
            printf("\033[K"); // Clear line from cursor
            printf("%s", line + position);
            if (position != lineLen)
                move_cursor_left(lineLen - position);
        }
        else if (c == 1) // ctrl - a
        {
            if (position != 0)
                move_cursor_left(position);
            position = 0;
        }
        else if (c == 4) // ctrl - d
        {
            if (lineLen == 0)
            {
                printf("\n");
                disableRawMode();
                exit(0);
            }
            else
            {
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
        else if (c == 9) // tab
        {
            if (lineLen > position)
                memmove(line + position + 1, line + position, lineLen - position);
            line[position++] = ' ';
            line[++lineLen] = '\0';
            cachedLine = false;

            // Print updated line and move cursor
            printf("\033[K"); // Clear line from cursor
            printf("%s", line + position - 1);
            if (lineLen > position)
                printf("\033[%dD", lineLen - position);
        }
    }
}

static int kbhit(void)
{
    static bool initflag = false;
    static const int STDIN = 0;

    if (!initflag)
    {
        // Use termios to turn off line buffering
        struct termios term;
        tcgetattr(STDIN, &term);
        term.c_lflag &= ~ICANON;
        tcsetattr(STDIN, TCSANOW, &term);
        setbuf(stdin, NULL);
        initflag = true;
    }

    int nbbytes;
    ioctl(STDIN, FIONREAD, &nbbytes); // 0 is STDIN
    return nbbytes;
}

static int _kbhit()
{
    static const int STDIN = 0;
    static bool initialized = false;

    if (!initialized)
    {
        // Use termios to turn off line buffering
        struct termios term;
        tcgetattr(STDIN, &term);
        term.c_lflag &= ~ICANON;
        tcsetattr(STDIN, TCSANOW, &term);
        setbuf(stdin, NULL);
        initialized = true;
    }

    int bytesWaiting;
    ioctl(STDIN, FIONREAD, &bytesWaiting);
    return bytesWaiting;
}

static char getch()
{
    char buf = 0;
    struct termios old = {0};
    if (tcgetattr(0, &old) < 0)
        perror("tcsetattr()");
    old.c_lflag &= ~ICANON;
    old.c_lflag &= ~ECHO;
    old.c_cc[VMIN] = 1;
    old.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &old) < 0)
        perror("tcsetattr ICANON");
    if (read(0, &buf, 1) < 0)
        perror("read()");
    old.c_lflag |= ICANON;
    old.c_lflag |= ECHO;
    if (tcsetattr(0, TCSADRAIN, &old) < 0)
        perror("tcsetattr ~ICANON");
    return (buf);
}

#endif