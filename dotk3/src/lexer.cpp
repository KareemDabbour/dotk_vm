#include "syntax.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace dk3
{

    std::vector<size_t> utf8Offsets(const std::string &text, const Span &span)
    {
        std::vector<size_t> offsets;
        for (size_t position = 0; position < text.size();)
        {
            offsets.push_back(position);
            unsigned char first = static_cast<unsigned char>(text[position]);
            size_t width = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2
                                          : first >= 0xe0 && first <= 0xef   ? 3
                                          : first >= 0xf0 && first <= 0xf4   ? 4
                                                                             : 0;
            if (!width || position + width > text.size())
                throw Error(span, "Invalid UTF-8 text", "value");
            uint32_t code = first & (width == 1 ? 0x7f : width == 2 ? 0x1f
                                                     : width == 3   ? 0x0f
                                                                    : 0x07);
            for (size_t continuation = 1; continuation < width; continuation++)
            {
                unsigned char byte = static_cast<unsigned char>(text[position + continuation]);
                if ((byte & 0xc0) != 0x80)
                    throw Error(span, "Invalid UTF-8 continuation", "value");
                code = (code << 6) | (byte & 0x3f);
            }
            if ((width == 2 && code < 0x80) || (width == 3 && code < 0x800) ||
                (width == 4 && code < 0x10000) || (code >= 0xd800 && code <= 0xdfff) || code > 0x10ffff)
                throw Error(span, "Invalid UTF-8 code point", "value");
            position += width;
        }
        offsets.push_back(text.size());
        return offsets;
    }

    std::vector<Token> lex(std::shared_ptr<const Source> source)
    {
        std::vector<Token> tokens;
        size_t position = 0;
        int line = 1;
        int column = 1;
        auto advance = [&]()
        {
            char byte = source->text[position++];
            if (byte == '\n')
            {
                line++;
                column = 1;
            }
            else
                column++;
            return byte;
        };
        auto peek = [&](size_t distance = 0)
        { return position + distance < source->text.size() ? source->text[position + distance] : '\0'; };
        while (position < source->text.size())
        {
            if (peek() == ' ' || peek() == '\t' || peek() == '\r')
            {
                advance();
                continue;
            }
            if (peek() == '#')
            {
                while (position < source->text.size() && peek() != '\n')
                    advance();
                continue;
            }
            Span span{source, position, 0, line, column};
            TokenKind kind = TokenKind::Symbol;
            std::string text;
            if (peek() == '\n')
            {
                text += advance();
                kind = TokenKind::Newline;
            }
            else if (peek() == '"' || peek() == '\'' || ((peek() == 'b' || peek() == 'f') && (peek(1) == '"' || peek(1) == '\'')))
            {
                bool bytes = peek() == 'b';
                bool formatted = peek() == 'f';
                if (bytes || formatted)
                    advance();
                char quote = advance();
                bool triple = peek() == quote && peek(1) == quote;
                if (triple)
                {
                    advance();
                    advance();
                }
                bool closed = false;
                size_t braces = 0;
                char innerQuote = 0;
                bool innerEscape = false;
                while (position < source->text.size())
                {
                    char byte = advance();
                    if (formatted && braces)
                    {
                        text += byte;
                        if (innerQuote)
                        {
                            if (innerEscape)
                                innerEscape = false;
                            else if (byte == '\\')
                                innerEscape = true;
                            else if (byte == innerQuote)
                                innerQuote = 0;
                        }
                        else if (byte == '\'' || byte == '"')
                            innerQuote = byte;
                        else if (byte == '{')
                            braces++;
                        else if (byte == '}')
                            braces--;
                        continue;
                    }
                    if (formatted && byte == '{')
                    {
                        text += byte;
                        if (peek() == '{')
                            text += advance();
                        else
                            braces = 1;
                        continue;
                    }
                    if (byte == quote && (!triple || (peek() == quote && peek(1) == quote)))
                    {
                        if (triple)
                        {
                            advance();
                            advance();
                        }
                        closed = true;
                        break;
                    }
                    if (byte == '\n' && !triple)
                        throw Error(span, "Unterminated string literal");
                    if (byte != '\\')
                    {
                        text += byte;
                        continue;
                    }
                    if (position == source->text.size())
                        break;
                    char escape = advance();
                    switch (escape)
                    {
                    case 'n':
                        text += '\n';
                        break;
                    case 'r':
                        text += '\r';
                        break;
                    case 't':
                        text += '\t';
                        break;
                    case '0':
                        text += '\0';
                        break;
                    case '\\':
                        text += '\\';
                        break;
                    case '\'':
                        text += '\'';
                        break;
                    case '"':
                        text += '"';
                        break;
                    case 'x':
                    {
                        int value = 0;
                        for (int digit = 0; digit < 2; digit++)
                        {
                            unsigned char hex = static_cast<unsigned char>(peek());
                            if (!std::isxdigit(hex))
                                throw Error(span, "Expected two hexadecimal digits after \\x");
                            advance();
                            value = value * 16 + (hex <= '9' ? hex - '0' : std::tolower(hex) - 'a' + 10);
                        }
                        text += static_cast<char>(value);
                        if (formatted && (value == '{' || value == '}'))
                            text += static_cast<char>(value);
                        break;
                    }
                    default:
                        throw Error(span, "Unknown string escape");
                    }
                }
                if (!closed)
                    throw Error(span, "Unterminated string literal", "syntax", triple);
                kind = formatted ? TokenKind::FormatString : bytes ? TokenKind::Bytes
                                                                   : TokenKind::String;
                if (!bytes)
                    utf8Offsets(text, span);
            }
            else if (std::isalpha(static_cast<unsigned char>(peek())) || peek() == '_')
            {
                kind = TokenKind::Name;
                while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
                    text += advance();
            }
            else if (std::isdigit(static_cast<unsigned char>(peek())))
            {
                kind = TokenKind::Integer;
                if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'b'))
                {
                    text += advance();
                    text += advance();
                    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')
                        text += advance();
                }
                else
                {
                    while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')
                        text += advance();
                    if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1))))
                    {
                        kind = TokenKind::Float;
                        text += advance();
                        while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')
                            text += advance();
                    }
                    if (peek() == 'e' || peek() == 'E')
                    {
                        kind = TokenKind::Float;
                        text += advance();
                        if (peek() == '+' || peek() == '-')
                            text += advance();
                        if (!std::isdigit(static_cast<unsigned char>(peek())))
                            throw Error(span, "Expected exponent digits");
                        while (std::isdigit(static_cast<unsigned char>(peek())) || peek() == '_')
                            text += advance();
                    }
                }
            }
            else
            {
                text += advance();
                std::string pair = text + peek();
                for (const char *compound : {"==", "!=", "<=", ">=", "->", "=>", "&&", "||", "//", "**", "?.", "+=", "-=", "*=", "/=", "??", "++", "--"})
                {
                    if (pair == compound)
                    {
                        text += advance();
                        break;
                    }
                }
                if (std::string("(){}[],:;.?+-*/%!=<>&|").find(text[0]) == std::string::npos)
                    throw Error(span, "Unexpected character");
            }
            span.length = position - span.offset;
            tokens.push_back({kind, std::move(text), span});
        }
        tokens.push_back({TokenKind::End, "", {source, position, 0, line, column}});
        return tokens;
    }

    std::string diagnose(const Error &error, bool color)
    {
        std::ostringstream output;
        const char *red = color ? "\x1b[1;31m" : "", *cyan = color ? "\x1b[36m" : "", *muted = color ? "\x1b[90m" : "", *reset = color ? "\x1b[0m" : "";
        std::string category = error.category.empty() ? "Runtime" : error.category;
        if (category == "io")
            category = "IO";
        else if (category == "throw")
            category = "Thrown";
        else
            category[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(category[0])));
        output << red << category << (error.category == "warning" ? "" : "Error") << reset << ": " << error.what() << '\n';
        const Span &span = error.span;
        output << cyan << "  --> " << (span.source ? span.source->file : "<host>") << ':' << span.line << ':' << span.column << reset << '\n';
        if (span.source)
        {
            const std::string &text = span.source->text;
            size_t offset = std::min(span.offset, text.size()), start = offset;
            while (start && text[start - 1] != '\n')
                start--;
            size_t end = text.find('\n', start);
            if (end == std::string::npos)
                end = text.size();
            size_t before = start;
            if (start)
            {
                before = start - 1;
                while (before && text[before - 1] != '\n')
                    before--;
            }
            size_t after = end < text.size() ? end + 1 : end;
            size_t afterEnd = text.find('\n', after);
            if (afterEnd == std::string::npos)
                afterEnd = text.size();
            int width = static_cast<int>(std::to_string(std::max(1, span.line + 1)).size());
            auto columns = [](const std::string &line, size_t count)
            {
                size_t column = 0;
                for (size_t index = 0; index < std::min(count, line.size()); index++)
                {
                    unsigned char byte = static_cast<unsigned char>(line[index]);
                    if (byte == '\t')
                        column += 4 - column % 4;
                    else if ((byte & 0xc0) != 0x80 && byte != '\r')
                        column++;
                }
                return column;
            };
            auto sourceLine = [&](size_t begin, size_t finish, int number, bool failing)
            {
                std::string line = text.substr(begin, finish - begin);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                std::string expanded;
                size_t column = 0;
                for (unsigned char byte : line)
                {
                    if (byte == '\t')
                    {
                        size_t spaces = 4 - column % 4;
                        expanded.append(spaces, ' ');
                        column += spaces;
                    }
                    else
                    {
                        expanded += static_cast<char>(byte);
                        if ((byte & 0xc0) != 0x80)
                            column++;
                    }
                }
                output << "\n"
                       << muted << "  " << std::setw(width) << number << " | " << reset << expanded;
                if (failing)
                {
                    size_t marker = columns(line, offset - begin);
                    size_t markedEnd = std::min(line.size(), offset - begin + std::min<size_t>(span.length, 80));
                    size_t marked = std::max<size_t>(1, columns(line, markedEnd) - marker);
                    output << '\n'
                           << muted << std::string(static_cast<size_t>(width) + 2, ' ') << " | " << reset << std::string(marker, ' ') << red << '^' << std::string(marked - 1, '~') << reset;
                }
            };
            if (start && span.line > 1)
                sourceLine(before, start - 1, span.line - 1, false);
            sourceLine(start, end, span.line, true);
            if (after < text.size())
                sourceLine(after, afterEnd, span.line + 1, false);
        }
        if (!error.trace.empty())
        {
            output << "\n\n"
                   << cyan << "Call stack:" << reset;
            for (const auto &frame : error.trace)
                output << "\n  " << frame.name << (frame.name.empty() || frame.name[0] == '<' ? "" : "()") << "  " << muted << (frame.span.source ? frame.span.source->file : "<host>") << ':' << frame.span.line << ':' << frame.span.column << reset;
        }
        return output.str();
    }

}