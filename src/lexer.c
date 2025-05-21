#include <ctype.h>
#include <errno.h>

#include "lexer.h"
#undef lexerExpect

bool lexerNew(Lexer *l, const char *path) {
    if (!readFile(&l->str, path)) {
        return false;
    }

    l->pos.path = path;
    return true;
}

void lexerBuffer(Lexer *l, Token token) {
    l->peeked = true;
    l->buffer = token;
}

void lexerUnbuffer(Lexer *l) {
    l->peeked = false;
}

static bool isIdent(char ch) {
    return isalnum(ch) || ch == '_';
}

static void nextChar(Lexer *l) {
    if (*l->str.data == '\n') {
        if (l->str.length > 1) {
            l->pos.row++;
            l->pos.col = 0;
        }
    } else {
        l->pos.col++;
    }

    l->str.data++;
    l->str.length--;
}

static char peekChar(Lexer *l, size_t n) {
    if (l->str.length > n) {
        return l->str.data[n];
    }

    return 0;
}

static char readChar(Lexer *l) {
    nextChar(l);
    return l->str.data[-1];
}

static bool matchChar(Lexer *l, char ch) {
    if (l->str.length > 0 && *l->str.data == ch) {
        nextChar(l);
        return true;
    }
    return false;
}

static void skipWhitespace(Lexer *l) {
    l->onNewline = false;
    while (l->str.length) {
        switch (*l->str.data) {
        case ' ':
        case '\t':
        case '\r':
            nextChar(l);
            break;

        case '\n':
            nextChar(l);
            l->onNewline = true;
            break;

        case '/':
            if (peekChar(l, 1) == '/') {
                while (l->str.length && *l->str.data != '\n') {
                    nextChar(l);
                }
            } else {
                return;
            }
            break;

        default:
            return;
        }
    }
}

static void errorInvalid(Pos pos, char ch, const char *label) {
    if (isprint(ch)) {
        fprintf(stderr, PosFmt "ERROR: Invalid %s '%c'\n", PosArg(pos), label, ch);
    } else {
        fprintf(stderr, PosFmt "ERROR: Invalid %s (%d)\n", PosArg(pos), label, ch);
    }

    exit(1);
}

static void errorUnterminated(Pos pos, const char *label) {
    fprintf(stderr, PosFmt "ERROR: Unterminated %s\n", PosArg(pos), label);
    exit(1);
}

static char lexChar(Lexer *l, const char *label) {
    if (!l->str.length) {
        errorUnterminated(l->pos, label);
    }

    char ch = readChar(l);
    if (ch == '\\') {
        if (!l->str.length) {
            errorUnterminated(l->pos, "escape character");
        }

        ch = *l->str.data;
        if (!resolveEscapeChar(&ch)) {
            errorInvalid(l->pos, ch, "escape character");
        }

        nextChar(l);
    }

    return ch;
}

static_assert(COUNT_TOKENS == 49, "");
Token lexerNext(Lexer *l) {
    if (l->peeked) {
        lexerUnbuffer(l);
        return l->buffer;
    }
    skipWhitespace(l);

    Token token = {
        .pos = l->pos,
        .str = l->str,
        .onNewline = l->onNewline,
    };

    if (l->str.length == 0) {
        return token;
    }

    if (isdigit(*l->str.data)) {
        token.kind = TOKEN_INT;
        while (l->str.length > 0 && isdigit(*l->str.data)) {
            nextChar(l);
        }
        token.str.length -= l->str.length;

        if (l->str.length && isIdent(*l->str.data)) {
            errorInvalid(l->pos, *l->str.data, "digit");
        }

        char buffer[32] = {0};
        if (token.str.length < sizeof(buffer) - 1) {
            memcpy(buffer, token.str.data, token.str.length);

            errno = 0;
            token.as.integer = strtol(buffer, NULL, 10);

            if (!errno) {
                return token;
            }
        }

        fprintf(
            stderr,
            PosFmt "ERROR: Integer literal '" StrFmt "' is too large\n",
            PosArg(token.pos),
            StrArg(token.str));

        exit(1);
    }

    if (*l->str.data == 'c' && peekChar(l, 1) == '"') {
        token.kind = TOKEN_CSTR;
        nextChar(l);
    }

    if (isIdent(*l->str.data)) {
        while (l->str.length > 0 && isIdent(*l->str.data)) {
            nextChar(l);
        }
        token.str.length -= l->str.length;

        if (strMatch(token.str, "true")) {
            token.kind = TOKEN_BOOL;
            token.as.boolean = 1;
        } else if (strMatch(token.str, "false")) {
            token.kind = TOKEN_BOOL;
            token.as.boolean = 0;
        } else if (strMatch(token.str, "sizeof")) {
            token.kind = TOKEN_SIZEOF;
        } else if (strMatch(token.str, "if")) {
            token.kind = TOKEN_IF;
        } else if (strMatch(token.str, "else")) {
            token.kind = TOKEN_ELSE;
        } else if (strMatch(token.str, "for")) {
            token.kind = TOKEN_FOR;
        } else if (strMatch(token.str, "return")) {
            token.kind = TOKEN_RETURN;
        } else if (strMatch(token.str, "fn")) {
            token.kind = TOKEN_FN;
        } else if (strMatch(token.str, "var")) {
            token.kind = TOKEN_VAR;
        } else if (strMatch(token.str, "type")) {
            token.kind = TOKEN_TYPE;
        } else if (strMatch(token.str, "struct")) {
            token.kind = TOKEN_STRUCT;
        } else if (strMatch(token.str, "extern")) {
            token.kind = TOKEN_EXTERN;
        } else if (strMatch(token.str, "print")) {
            token.kind = TOKEN_PRINT;
        } else {
            token.kind = TOKEN_IDENT;
        }

        return token;
    }

    switch (readChar(l)) {
    case ';':
        token.kind = TOKEN_EOL;
        break;

    case '.':
        if (matchChar(l, '.')) {
            token.kind = TOKEN_RANGE;
        } else {
            token.kind = TOKEN_DOT;
        }
        break;

    case ',':
        token.kind = TOKEN_COMMA;
        break;

    case ':':
        if (matchChar(l, '=')) {
            token.kind = TOKEN_WALRUS;
        } else {
            token.kind = TOKEN_COLON;
        }
        break;

    case '"':
        token.as.integer = 0;
        while (peekChar(l, 0) != '"') {
            lexChar(l, "string");
            token.as.integer++;
        }
        nextChar(l); // The terminating quote

        if (token.kind != TOKEN_CSTR) {
            token.kind = TOKEN_STR;
        }
        break;

    case '\'':
        token.kind = TOKEN_CHAR;
        token.as.integer = lexChar(l, "character");
        if (!matchChar(l, '\'')) {
            errorUnterminated(l->pos, "character");
        }
        break;

    case '(':
        token.kind = TOKEN_LPAREN;
        break;

    case ')':
        token.kind = TOKEN_RPAREN;
        break;

    case '{':
        token.kind = TOKEN_LBRACE;
        break;

    case '}':
        token.kind = TOKEN_RBRACE;
        break;

    case '[':
        token.kind = TOKEN_LBRACKET;
        break;

    case ']':
        token.kind = TOKEN_RBRACKET;
        break;

    case '+':
        token.kind = TOKEN_ADD;
        break;

    case '-':
        token.kind = TOKEN_SUB;
        break;

    case '*':
        token.kind = TOKEN_MUL;
        break;

    case '/':
        token.kind = TOKEN_DIV;
        break;

    case '|':
        if (matchChar(l, '|')) {
            token.kind = TOKEN_LOR;
        } else {
            token.kind = TOKEN_BOR;
        }
        break;

    case '&':
        if (matchChar(l, '&')) {
            token.kind = TOKEN_LAND;
        } else {
            token.kind = TOKEN_BAND;
        }
        break;

    case '~':
        token.kind = TOKEN_BNOT;
        break;

    case '!':
        if (matchChar(l, '=')) {
            token.kind = TOKEN_NE;
        } else {
            token.kind = TOKEN_LNOT;
        }
        break;

    case '>':
        if (matchChar(l, '>')) {
            token.kind = TOKEN_SHR;
        } else if (matchChar(l, '=')) {
            token.kind = TOKEN_GE;
        } else {
            token.kind = TOKEN_GT;
        }
        break;

    case '<':
        if (matchChar(l, '<')) {
            token.kind = TOKEN_SHL;
        } else if (matchChar(l, '=')) {
            token.kind = TOKEN_LE;
        } else {
            token.kind = TOKEN_LT;
        }
        break;

    case '=':
        if (matchChar(l, '=')) {
            token.kind = TOKEN_EQ;
        } else {
            token.kind = TOKEN_SET;
        }
        break;

    default:
        errorInvalid(token.pos, *token.str.data, "character");
    }

    token.str.length -= l->str.length;
    return token;
}

Token lexerPeek(Lexer *l) {
    if (!l->peeked) {
        lexerBuffer(l, lexerNext(l));
    }
    return l->buffer;
}

bool lexerRead(Lexer *l, TokenKind kind) {
    lexerPeek(l);
    l->peeked = l->buffer.kind != kind;
    return !l->peeked;
}

Token lexerExpect(Lexer *l, const TokenKind *kinds) {
    const Token token = lexerNext(l);
    for (const TokenKind *it = kinds; *it != TOKEN_EOF; it++) {
        if (token.kind == *it) {
            return token;
        }
    }

    fprintf(stderr, PosFmt "ERROR: Expected ", PosArg(token.pos));
    for (const TokenKind *it = kinds; *it != TOKEN_EOF; it++) {
        if (it != kinds) {
            fprintf(stderr, " or ");
        }

        fprintf(stderr, "%s", tokenKindName(*it));
    }

    fprintf(stderr, ", got %s\n", tokenKindName(token.kind));
    exit(1);
}

Token lexerSplitToken(Lexer *l, Token token) {
    switch (token.kind) {
    case TOKEN_LAND:
        token.kind = TOKEN_BAND;
        break;

    default:
        unreachable();
    }

    token.str.length = 1;
    Token remainder = token;
    remainder.pos.col++;
    lexerBuffer(l, remainder);
    return token;
}
