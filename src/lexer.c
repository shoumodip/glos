#include <ctype.h>
#include <errno.h>

#include "lexer.h"

bool lexer_open(Lexer *l, const char *path) {
    if (!read_file(&l->sv, path)) {
        return false;
    }

    l->pos.path = path;
    return true;
}

void lexer_buffer(Lexer *l, Token token) {
    l->peeked = true;
    l->buffer = token;
}

void lexer_unbuffer(Lexer *l) {
    l->peeked = false;
}

static bool isident(char ch) {
    return isalnum(ch) || ch == '_';
}

static void next_char(Lexer *l) {
    if (*l->sv.data == '\n') {
        if (l->sv.count > 1) {
            l->pos.row++;
            l->pos.col = 0;
        }
    } else {
        l->pos.col++;
    }

    l->sv.data++;
    l->sv.count--;
}

static char peek_char(Lexer *l, size_t n) {
    if (l->sv.count > n) {
        return l->sv.data[n];
    }

    return 0;
}

static char read_char(Lexer *l) {
    next_char(l);
    return l->sv.data[-1];
}

static void skip_whitespace(Lexer *l) {
    l->newline = false;
    while (l->sv.count) {
        switch (*l->sv.data) {
        case ' ':
        case '\t':
        case '\r':
            next_char(l);
            break;

        case '\n':
            next_char(l);
            l->newline = true;
            break;

        case '/':
            if (peek_char(l, 1) == '/') {
                while (l->sv.count && *l->sv.data != '\n') {
                    next_char(l);
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

static void error_invalid(Pos pos, char ch, const char *label) {
    if (isprint(ch)) {
        fprintf(stderr, PosFmt "ERROR: Invalid %s '%c'\n", PosArg(pos), label, ch);
    } else {
        fprintf(stderr, PosFmt "ERROR: Invalid %s (%d)\n", PosArg(pos), label, ch);
    }

    exit(1);
}

static_assert(COUNT_TOKENS == 19, "");
Token lexer_next(Lexer *l) {
    if (l->peeked) {
        lexer_unbuffer(l);
        return l->buffer;
    }
    skip_whitespace(l);

    Token token = {
        .pos = l->pos,
        .sv = l->sv,
        .newline = l->newline,
    };

    if (!l->sv.count) {
        return token;
    }

    if (isdigit(*l->sv.data)) {
        token.kind = TOKEN_INT;
        while (l->sv.count > 0 && isdigit(*l->sv.data)) {
            next_char(l);
        }
        token.sv.count -= l->sv.count;

        if (l->sv.count && isident(*l->sv.data)) {
            error_invalid(l->pos, *l->sv.data, "digit");
        }

        char buffer[32] = {0};
        if (token.sv.count < sizeof(buffer) - 1) {
            memcpy(buffer, token.sv.data, token.sv.count);

            errno = 0;
            token.as.integer = strtol(buffer, NULL, 10);

            if (!errno) {
                return token;
            }
        }

        fprintf(stderr, PosFmt "ERROR: Integer literal '" SVFmt "' is too large\n", PosArg(token.pos), SVArg(token.sv));
        exit(1);
    }

    if (isident(*l->sv.data)) {
        while (l->sv.count > 0 && isident(*l->sv.data)) {
            next_char(l);
        }
        token.sv.count -= l->sv.count;

        if (sv_match(token.sv, "true")) {
            token.kind = TOKEN_BOOL;
            token.as.boolean = 1;
        } else if (sv_match(token.sv, "false")) {
            token.kind = TOKEN_BOOL;
            token.as.boolean = 0;
        } else if (sv_match(token.sv, "fn")) {
            token.kind = TOKEN_FN;
        } else if (sv_match(token.sv, "var")) {
            token.kind = TOKEN_VAR;
        } else if (sv_match(token.sv, "if")) {
            token.kind = TOKEN_IF;
        } else if (sv_match(token.sv, "else")) {
            token.kind = TOKEN_ELSE;
        } else if (sv_match(token.sv, "print")) {
            token.kind = TOKEN_PRINT;
        } else {
            token.kind = TOKEN_IDENT;
        }

        return token;
    }

    switch (read_char(l)) {
    case ';':
        token.kind = TOKEN_EOL;
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

    case '=':
        token.kind = TOKEN_SET;
        break;

    default:
        error_invalid(token.pos, *token.sv.data, "character");
        break;
    }

    token.sv.count -= l->sv.count;
    return token;
}

Token lexer_peek(Lexer *l) {
    if (!l->peeked) {
        lexer_buffer(l, lexer_next(l));
    }
    return l->buffer;
}

bool lexer_read(Lexer *l, TokenKind kind) {
    lexer_peek(l);
    l->peeked = l->buffer.kind != kind;
    return !l->peeked;
}

Token lexer_expect_impl(Lexer *l, const TokenKind *kinds) {
    const Token token = lexer_next(l);
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

        fprintf(stderr, "%s", token_kind_to_cstr(*it));
    }

    fprintf(stderr, ", got %s\n", token_kind_to_cstr(token.kind));
    exit(1);
}
