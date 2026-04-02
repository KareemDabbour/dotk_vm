#include "include/scanner.h"
#include "include/common.h"
#include <string.h>

typedef struct _Scanner
{
    const char *start;
    const char *current;
    int interpolationDepth;
    int line;
    int col;
} Scanner;

Scanner scanner;

void initScanner(const char *source)
{
    scanner.start = source;
    scanner.current = source;
    scanner.interpolationDepth = 0;
    scanner.line = 1;
    scanner.col = 1;
}

static bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

static bool isHexDigit(char c)
{
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
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

static char peekN(int offset)
{
    const char *p = scanner.current;
    for (int i = 0; i < offset; i++)
    {
        if (*p == '\0')
            return '\0';
        p++;
    }
    return *p;
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
        .col = scanner.col};
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
    case 'a':
        if (scanner.current - scanner.start > 1)
        {
            switch (scanner.start[1])
            {
            case 'n':
                return checkKeyword(2, 1, "d", TOKEN_AND);
            case 's':
                if (scanner.current - scanner.start > 2)
                    return checkKeyword(2, 3, "ync", TOKEN_ASYNC);
                return checkKeyword(2, 0, "", TOKEN_AS);
            case 'w':
                return checkKeyword(2, 3, "ait", TOKEN_AWAIT);
            }
        }
        break;
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
            {
                if (scanner.current - scanner.start > 2)
                {
                    switch (scanner.start[2])
                    {
                    case 't':
                        return checkKeyword(3, 2, "ch", TOKEN_CATCH);
                    case 's':
                        return checkKeyword(3, 1, "e", TOKEN_CASE);
                    }
                }
            }
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
        if (scanner.current - scanner.start > 1)
        {
            switch (scanner.start[1])
            {
            case 'l':
                return checkKeyword(2, 2, "se", TOKEN_ELSE);
            case 'x':
                return checkKeyword(2, 4, "port", TOKEN_EXPORT);
            }
        }
        break;
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
            case 'n':
                return checkKeyword(2, 0, "", TOKEN_IN);
            }
        }
        break;
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
            {
                if (scanner.current - scanner.start > 2)
                {
                    switch (scanner.start[2])
                    {
                    case 'y':
                        return checkKeyword(3, 0, "", TOKEN_TRY);
                    case 'u':
                        return checkKeyword(3, 1, "e", TOKEN_TRUE);
                    }
                }
            }
            }
        }
        break;
    case 'v':
        return checkKeyword(1, 2, "ar", TOKEN_VAR);
    case 'w':
        return checkKeyword(1, 4, "hile", TOKEN_WHILE);
    case 'y':
        return checkKeyword(1, 4, "ield", TOKEN_YIELD);
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
            case 'r':
                return checkKeyword(2, 2, "om", TOKEN_FROM);
            }
        }
        break;
    case 'm':
        return checkKeyword(1, 5, "odule", TOKEN_MODULE);
    case 'o':
        return checkKeyword(1, 1, "r", TOKEN_OR);
    default:
        break;
    }
    return TOKEN_IDENTIFIER;
}

static Token number()
{
    if (scanner.start[0] == '0' && (peek() == 'x' || peek() == 'X'))
    {
        advance();
        while (isHexDigit(peek()))
            advance();
        return makeToken(TOKEN_NUMBER);
    }

    if (scanner.start[0] == '0' && (peek() == 'b' || peek() == 'B'))
    {
        advance();
        while (peek() == '0' || peek() == '1')
            advance();
        return makeToken(TOKEN_NUMBER);
    }

    if (scanner.start[0] == '0' && (peek() == 'y' || peek() == 'Y'))
    {
        advance();
        while (isDigit(peek()))
            advance();
        return makeToken(TOKEN_NUMBER);
    }

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

static Token interpolateString()
{
    while (peek() != '"' && peek() != '\n' && !isAtEnd())
    {
        if (peek() == '\n')
        {
            scanner.line++;
            scanner.col = 1;
        }
        else if (peek() == '\\' && peekNext() == '{')
            advance();
        else if (peek() == '\\' && peekNext() == '}')
            advance();

        else if (peek() == '{')
        {
            if (scanner.interpolationDepth >= UINT4_MAX)
                return errorToken("Interpolation depth exceeded");

            if (peekNext() == '}') // empty interpolation
            {
                advance(); // eat the '{'
                while (peek() != '"' && !isAtEnd())
                {
                    if (peek() == '\n')
                    {
                        scanner.line++;
                        scanner.col = 1;
                    }
                    advance();
                }
                if (isAtEnd())
                    return errorToken("Unterminated template string.");
                return errorToken("Empty interpolation");
            }
            scanner.interpolationDepth++;
            advance();
            Token tok = makeToken(TOKEN_INTERPOLATION);
            return tok;
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
    if (peek() == '"')
        advance();
    else
        return errorToken("Unterminated template string.");
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
        // else if (peek() == '%' && peekNext() == '{')
        // {
        //     if (scanner.interpolationDepth >= UINT4_MAX)
        //         return errorToken("Interpolation depth exceeded");

        //     advance();             // eat the '%'
        //     if (peekNext() == '}') // empty interpolation
        //     {
        //         advance(); // eat the '{'
        //         while (peek() != '"' && !isAtEnd())
        //         {
        //             if (peek() == '\n')
        //             {
        //                 scanner.line++;
        //                 scanner.col = 1;
        //             }
        //             advance();
        //         }
        //         if (isAtEnd())
        //             return errorToken("Unterminated template string.");
        //         return errorToken("Empty interpolation");
        //     }
        //     scanner.interpolationDepth++;
        //     Token tok = makeToken(TOKEN_INTERPOLATION);
        //     advance();
        //     return tok;
        // }
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
    if (peek() == '"')
        advance();
    else
        return errorToken("Unterminated template string.");
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

    if (c == 'f' && peek() == '"')
    {
        advance();
        // advance();
        scanner.start = scanner.current - 1;
        return interpolateString();
    }

    if ((c == 'b' || c == 'B') && peek() == '"')
    {
        advance();
        scanner.start = scanner.current - 1;
        Token tok = templateString();
        tok.type = TOKEN_BYTE_STRING;
        return tok;
    }

    if ((c == 'x' || c == 'X') && peek() == '"')
    {
        advance();
        scanner.start = scanner.current - 1;
        Token tok = templateString();
        tok.type = TOKEN_HEX_STRING;
        return tok;
    }

    if ((c == 'b' || c == 'B') && peek() == 'i' && peekN(1) == 'n' && peekN(2) == '"')
    {
        advance();
        advance();
        advance();
        scanner.start = scanner.current - 1;
        Token tok = templateString();
        tok.type = TOKEN_BINARY_STRING;
        return tok;
    }

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
    {
        if (scanner.interpolationDepth > 0)
        {
            scanner.interpolationDepth--;
            // advance();
            // scanner.start = scanner.current;
            return interpolateString();
        }
        return makeToken(TOKEN_RIGHT_BRACE);
    }
    case ';':
        return makeToken(TOKEN_SEMICOLON);
    case ':':
    {
        if (match('@'))
            return makeToken(TOKEN_COLON_AT);
        else if (match(':'))
            return makeToken(TOKEN_DOUBLE_COLON);
        else
            return makeToken(TOKEN_COLON);
    }
    case '^':
        return makeToken(TOKEN_CARET);
    case ',':
        return makeToken(TOKEN_COMMA);
    case '.':
        return makeToken(match('{') ? TOKEN_DOT_BRACKET : TOKEN_DOT);
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
    {
        if (match('<'))
            return makeToken(TOKEN_LESS_LESS);
        else if (match('='))
            return makeToken(TOKEN_LESS_EQUAL);

        return makeToken(TOKEN_LESS);
    }
    case '>':
    {
        if (match('>'))
            return makeToken(TOKEN_GREATER_GREATER);
        else if (match('='))
            return makeToken(TOKEN_GREATER_EQUAL);

        return makeToken(TOKEN_GREATER);
    }
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