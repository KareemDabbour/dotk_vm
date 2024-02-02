#include "include/scanner.h"
#include "include/common.h"
#include <stdio.h>
#include <string.h>

typedef struct _Scanner
{
    const char *start;
    const char *current;
    int line;
    int col;
} Scanner;

Scanner scanner;

void initScanner(const char *source)
{
    scanner.start = source;
    scanner.current = source;
    scanner.line = 1;
    scanner.col = 1;
}

static bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

static bool isAlpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool isAtEnd()
{
    return *scanner.current == '\0';
}

static char advance()
{
    scanner.current++;
    scanner.col++;
    return scanner.current[-1];
}

static inline char peek()
{
    return *scanner.current;
}

static char peekNext()
{
    if (isAtEnd())
        return '\0';
    return scanner.current[1];
}

static bool match(char expected)
{
    if (isAtEnd())
        return false;
    if (*scanner.current != expected)
        return false;
    scanner.current++;
    return true;
}

static Token makeToken(TokenType type)
{
    Token token = {
        .type = type,
        .start = scanner.start,
        .len = (int)(scanner.current - scanner.start),
        .line = scanner.line,
        .col = scanner.col - token.len};
    return token;
}

static Token errorToken(const char *message)
{
    Token token = {
        .type = TOKEN_ERROR,
        .start = message,
        .len = (int)(strlen(message)),
        .line = scanner.line,
        .col = scanner.col - token.len};
    return token;
}

static int skipWhitespace()
{
    for (;;)
    {
        char c = peek();
        switch (c)
        {
        case ' ':
        case '\r':
        case '\t':
            advance();
            break;
        case 10:
            scanner.line++;
            advance();
            scanner.col = 1;
            break;
        case '#':
            while (peek() != '\n' && !isAtEnd())
                advance();
            break;
        case '~':
        {
            advance();
            while (peek() != '~' && !isAtEnd())
            {
                if (peek() == '\n')
                {
                    scanner.line++;
                    scanner.col = 1;
                }
                advance();
            }
            if (isAtEnd())
                return -1;
            advance();
            break;
        }
        default:
            return 0;
        }
    }
    return 0;
}

static TokenType checkKeyword(int start, int len, const char *rest, TokenType type)
{
    if (scanner.current - scanner.start == start + len && memcmp(scanner.start + start, rest, len) == 0)
        return type;
    return TOKEN_IDENTIFIER;
}

static TokenType identifierType()
{
    switch (*scanner.start)
    {
    case 'b':
        return checkKeyword(1, 4, "reak", TOKEN_BREAK);
    case 'c':
        if (scanner.current - scanner.start > 1)
        {
            switch (scanner.start[1])
            {
            case 'l':
                return checkKeyword(2, 3, "ass", TOKEN_CLASS);

            case 'a':
                return checkKeyword(2, 2, "se", TOKEN_CASE);
            case 'o':
            {

                if (scanner.current - scanner.start > 2 && scanner.start[2] == 'n')
                {
                    switch (scanner.start[3])
                    {
                    case 's':
                        return checkKeyword(4, 1, "t", TOKEN_CONST);
                    case 't':
                        return checkKeyword(4, 4, "inue", TOKEN_CONTINUE);
                    }
                }
            }
            }
        }
        break;
    case 'd':
        return checkKeyword(1, 6, "efault", TOKEN_DEFAULT);
    case 'e':
        return checkKeyword(1, 3, "lse", TOKEN_ELSE);
    case 'i':
    {
        if (scanner.current - scanner.start > 1)
        {
            switch (scanner.start[1])
            {
            case 'f':
                return checkKeyword(2, 0, "", TOKEN_IF);
            case 'm':
                return checkKeyword(2, 4, "port", TOKEN_IMPORT);
            }
        }
    }
    case 'n':
        return checkKeyword(1, 3, "ull", TOKEN_NIL);
    case 'p':
        return checkKeyword(1, 4, "rint", TOKEN_PRINT);
    case 'r':
        return checkKeyword(1, 5, "eturn", TOKEN_RETURN);
    case 's':
    {
        if (scanner.current - scanner.start > 1)
        {
            switch (scanner.start[1])
            {
            case 'w':
                return checkKeyword(2, 4, "itch", TOKEN_SWITCH);
            case 'u':
                return checkKeyword(2, 3, "per", TOKEN_SUPER);
            }
        }
        break;
    }
    case 't':
        if (scanner.current - scanner.start > 1)
        {
            switch (scanner.start[1])
            {
            case 'h':
                return checkKeyword(2, 2, "is", TOKEN_THIS);
            case 'r':
                return checkKeyword(2, 2, "ue", TOKEN_TRUE);
            }
        }
        break;
    case 'v':
        return checkKeyword(1, 2, "ar", TOKEN_VAR);
    case 'w':
        return checkKeyword(1, 4, "hile", TOKEN_WHILE);
    case 'f':
        if (scanner.current - scanner.start > 1)
        {
            switch (scanner.start[1])
            {
            case 'a':
                return checkKeyword(2, 3, "lse", TOKEN_FALSE);
            case 'o':
                return checkKeyword(2, 1, "r", TOKEN_FOR);
            case 'n':
                return checkKeyword(2, 0, "", TOKEN_FUN);
            }
        }
        break;
    default:
        break;
    }
    return TOKEN_IDENTIFIER;
}

static Token number()
{
    while (isDigit(peek()))
        advance();
    if (peek() == '.' && isDigit(peekNext()))
    {
        advance(); // eat the '.'
        while (isDigit(peek()))
            advance();
    }

    return makeToken(TOKEN_NUMBER);
}

static Token identifier()
{
    while (isAlpha(peek()) || isDigit(peek()))
        advance();
    return makeToken(identifierType());
}

static Token rawString()
{
    while (peek() != '`' && !isAtEnd())
    {
        if (peek() == '\n')
        {
            scanner.line++;
            scanner.col = 1;
        }
        advance();
    }
    if (isAtEnd())
        return errorToken("Unterminated raw string.");
    advance();
    return makeToken(TOKEN_RAW_STRING);
}

static Token templateString()
{
    while (peek() != '"' && peek() != '\n' && !isAtEnd())
    {
        if (peek() == '\n')
        {
            scanner.line++;
            scanner.col = 1;
        }
        else if (peek() == '\\' && peekNext() == '\"')
            advance();
        else if (peek() == '\\' && peekNext() == '\n')
        {
            advance();
            scanner.line++;
            scanner.col = 1;
        }

        else if (peek() == '\\' && peekNext() == '\\')
            advance();
        advance();
    }

    if (isAtEnd())
        return errorToken("Unterminated template string.");
    advance();
    return makeToken(TOKEN_TEMPLATE_STRING);
}

static Token basicString()
{
    while (peek() != '\'' && peek() != '\n' && !isAtEnd())
    {
        if (peek() == '\n')
        {
            scanner.line++;
            scanner.col = 1;
        }
        else if (peek() == '\\' && peekNext() == '\"')
            advance();
        else if (peek() == '\\' && peekNext() == '\n')
        {
            advance();
            scanner.line++;
            scanner.col = 1;
        }
        else if (peek() == '\\' && peekNext() == '\\')
            advance();
        advance();
    }
    if (isAtEnd())
        return errorToken("Unterminated basic string.");
    advance();
    return makeToken(TOKEN_BASIC_STRING);
}

Token scanToken()
{
    int line = scanner.line;
    int col = scanner.col;
    if (skipWhitespace() < 0)
    {
        Token blockCommentErr = errorToken("Unterminted Block Comment");
        blockCommentErr.line = line;
        blockCommentErr.col = col;
        return blockCommentErr;
    }
    scanner.start = scanner.current;
    if (isAtEnd())
        return makeToken(TOKEN_EOF);

    char c = advance();
    if (isAlpha(c))
        return identifier();
    if (isDigit(c))
        return number();
    switch (c)
    {
    case '(':
        return makeToken(TOKEN_LEFT_PAREN);
    case ')':
        return makeToken(TOKEN_RIGHT_PAREN);
    case '[':
        return makeToken(TOKEN_LEFT_BRACKET);
    case ']':
        return makeToken(TOKEN_RIGHT_BRACKET);
    case '{':
        return makeToken(TOKEN_LEFT_BRACE);
    case '}':
        return makeToken(TOKEN_RIGHT_BRACE);
    case ';':
        return makeToken(TOKEN_SEMICOLON);
    case ':':
        return makeToken(
            match('@') ? TOKEN_COLON_AT : TOKEN_COLON);
    case ',':
        return makeToken(TOKEN_COMMA);
    case '.':
        return makeToken(TOKEN_DOT);
    case '%':
        return makeToken(TOKEN_PERCENT);
    case '@':
        return makeToken(TOKEN_AT);
    case '?':
        return makeToken(TOKEN_QUESTION);
    case '-':
        return makeToken(
            match('=') ? TOKEN_MINUS_EQUAL : TOKEN_MINUS);
    case '+':
        return makeToken(
            match('=') ? TOKEN_PLUS_EQUAL : TOKEN_PLUS);
    case '&':
        return makeToken(
            match('&') ? TOKEN_AND : TOKEN_AMP);
    case '|':
        return makeToken(
            match('|') ? TOKEN_OR : TOKEN_PIPE);
    case '/':
    {
        if (match('/'))
            return makeToken(match('=') ? TOKEN_SLASH_SLASH_EQUAL : TOKEN_SLASH_SLASH);
        else if (match('='))
            return makeToken(TOKEN_SLASH_EQUAL);
        else
            return makeToken(TOKEN_SLASH);
    }
        return makeToken(
            match('=') ? TOKEN_SLASH_EQUAL : TOKEN_SLASH);
    case '*':
    {
        if (match('='))
            return makeToken(TOKEN_STAR_EQUAL);
        else if (match('*'))
            return makeToken(TOKEN_STAR_STAR);
        else
            return makeToken(TOKEN_STAR);
    }
    case '!':
        return makeToken(
            match('=') ? TOKEN_BANG_EQUAL : TOKEN_BANG);
    case '=':
    {
        if (match('='))
            return makeToken(TOKEN_EQUAL_EQUAL);
        else if (match('>'))
            return makeToken(TOKEN_ARROW);
        else
            return makeToken(TOKEN_EQUAL);
    }
    case '<':
        return makeToken(
            match('=') ? TOKEN_LESS_EQUAL : TOKEN_LESS);
    case '>':
        return makeToken(
            match('=') ? TOKEN_GREATER_EQUAL : TOKEN_GREATER);
    case '`':
        return rawString();
    case '"':
        return templateString();
    case '\'':
        return basicString();

    default:
        break;
    }
    return errorToken("Unexpected Character");
}