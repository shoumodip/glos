#include "lexer.h"
#include "basic.h"
#include "token.h"
#include <ctype.h>
#include <errno.h>

bool lexer_open(Lexer *l, const char *path) {
    memset(l, 0, sizeof(*l));
    if (!read_file(path, &l->sv, &default_arena)) {
        return false;
    }

    l->pos.path = path;
    return true;
}

static bool isident(char ch) {
    return isalnum(ch) || ch == '_';
}

static void next_char(Lexer *l) {
    if (*l->sv.data == '\n') {
        if (l->sv.count > 1) {
            l->pos.row++;
            l->pos.col = 0;

            l->sv.data++;
            l->sv.count--;
            return;
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

static bool match_char(Lexer *l, char ch) {
    if (l->sv.count && *l->sv.data == ch) {
        next_char(l);
        return true;
    }
    return false;
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

        case '#':
            if (l->pos.col == 0 && l->pos.row == 0 && peek_char(l, 1) == '!') {
                while (l->sv.count && *l->sv.data != '\n') {
                    next_char(l);
                }
            } else {
                return;
            }
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

static void error_invalid(Pos pos, SV sv, const char *label) {
    if (isprint(*sv.data)) {
        fprintf(stderr, Pos_Fmt "ERROR: Invalid %s '%c'\n", Pos_Arg(pos), label, *sv.data);
    } else {
        fprintf(stderr, Pos_Fmt "ERROR: Invalid %s (%d)\n", Pos_Arg(pos), label, *sv.data);
    }
    exit(1);
}

static void error_unterminated(Pos pos, const char *label) {
    fprintf(stderr, Pos_Fmt "ERROR: Unterminated %s\n", Pos_Arg(pos), label);
    exit(1);
}

static bool escape_char(char *ch) {
    switch (*ch) {
    case 'e':
        *ch = '\033';
        break;

    case 'n':
        *ch = '\n';
        break;

    case 'r':
        *ch = '\r';
        break;

    case 't':
        *ch = '\t';
        break;

    case '0':
        *ch = '\0';
        break;

    case '\'':
        *ch = '\'';
        break;

    case '"':
        *ch = '\"';
        break;

    case '\\':
        *ch = '\\';
        break;

    default:
        return false;
    }

    return true;
}

static char next_char_with_parsed_escape(Lexer *l, const char *label) {
    if (!l->sv.count) {
        error_unterminated(l->pos, label);
    }

    char ch = read_char(l);
    if (ch != '\\') {
        return ch;
    }

    if (!l->sv.count) {
        error_unterminated(l->pos, label);
    }

    ch = *l->sv.data;
    if (!escape_char(&ch)) {
        error_invalid(l->pos, l->sv, "escape character");
    }

    next_char(l);
    return ch;
}

Token lexer_get_string(Lexer *l, Pos pos) {
    const size_t default_sb_count_save = default_sb.count;

    Token token = {.kind = TOKEN_STRING, .pos = pos};
    while (l->sv.count) {
        if (*l->sv.data == '"') {
            break;
        }

        if (*l->sv.data == '\\' && l->sv.count > 1 && l->sv.data[1] == '{') {
            next_char(l);
            token.kind = TOKEN_ISTRING;
            break;
        }
        sb_push(&default_sb, next_char_with_parsed_escape(l, "string"));
    }

    if (!l->sv.count) {
        error_unterminated(l->pos, "string");
    }
    next_char(l);

    token.sv.count = default_sb.count - default_sb_count_save;
    token.sv.data = arena_clone(&default_arena, default_sb.data + default_sb_count_save, token.sv.count);
    default_sb.count = default_sb_count_save;
    return token;
}

static_assert(COUNT_TOKENS == 77, "");
Token lexer_iter(Lexer *l) {
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
            error_invalid(l->pos, l->sv, "digit");
        }

        char *buffer = arena_sv_to_cstr(&temp_arena, token.sv);
        memcpy(buffer, token.sv.data, token.sv.count);

        errno = 0;
        token.as.integer = strtoul(buffer, NULL, 10);
        arena_reset(&temp_arena, buffer);

        if (!errno) {
            return token;
        }

        fprintf(stderr, Pos_Fmt "ERROR: Number '" SV_Fmt "' is too large\n", Pos_Arg(token.pos), SV_Arg(token.sv));
        exit(1);
    }

    if (isident(*l->sv.data)) {
        while (l->sv.count > 0 && isident(*l->sv.data)) {
            next_char(l);
        }
        token.sv.count -= l->sv.count;

        if (sv_match(token.sv, "true")) {
            token.kind = TOKEN_BOOL;
            token.as.integer = true;
        } else if (sv_match(token.sv, "false")) {
            token.kind = TOKEN_BOOL;
            token.as.integer = false;
        } else if (sv_match(token.sv, "null")) {
            token.kind = TOKEN_NULL;
        } else if (sv_match(token.sv, "enum")) {
            token.kind = TOKEN_ENUM;
        } else if (sv_match(token.sv, "trait")) {
            token.kind = TOKEN_TRAIT;
        } else if (sv_match(token.sv, "union")) {
            token.kind = TOKEN_UNION;
        } else if (sv_match(token.sv, "struct")) {
            token.kind = TOKEN_STRUCT;
        } else if (sv_match(token.sv, "sizeof")) {
            token.kind = TOKEN_SIZEOF;
        } else if (sv_match(token.sv, "typeof")) {
            token.kind = TOKEN_TYPEOF;
        } else if (sv_match(token.sv, "inline")) {
            token.kind = TOKEN_INLINE;
        } else if (sv_match(token.sv, "distinct")) {
            token.kind = TOKEN_DISTINCT;
        } else if (sv_match(token.sv, "if")) {
            token.kind = TOKEN_IF;
        } else if (sv_match(token.sv, "else")) {
            token.kind = TOKEN_ELSE;
        } else if (sv_match(token.sv, "for")) {
            token.kind = TOKEN_FOR;
        } else if (sv_match(token.sv, "case")) {
            token.kind = TOKEN_CASE;
        } else if (sv_match(token.sv, "defer")) {
            token.kind = TOKEN_DEFER;
        } else if (sv_match(token.sv, "break")) {
            token.kind = TOKEN_BREAK;
        } else if (sv_match(token.sv, "continue")) {
            token.kind = TOKEN_CONTINUE;
        } else if (sv_match(token.sv, "return")) {
            token.kind = TOKEN_RETURN;
        } else if (sv_match(token.sv, "extern")) {
            token.kind = TOKEN_EXTERN;
        } else {
            token.kind = TOKEN_IDENT;
        }

        return token;
    }

    switch (read_char(l)) {
    case ';':
        token.kind = TOKEN_EOL;
        break;

    case '.':
        if (match_char(l, '.')) {
            if (match_char(l, '.')) {
                token.kind = TOKEN_SPREAD;
            } else {
                token.kind = TOKEN_RANGE;
            }
        } else {
            token.kind = TOKEN_DOT;
        }
        break;

    case ':':
        token.kind = TOKEN_COLON;
        break;

    case ',':
        token.kind = TOKEN_COMMA;
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

    case '\'':
        token.kind = TOKEN_CHAR;
        token.as.integer = next_char_with_parsed_escape(l, "character");
        if (!match_char(l, '\'')) {
            error_unterminated(l->pos, "character");
        }
        break;

    case '"':
        return lexer_get_string(l, token.pos);

    case '+':
        token.kind = TOKEN_ADD;
        if (match_char(l, '=')) {
            token.kind = TOKEN_ADD_SET;
        }
        break;

    case '-':
        token.kind = TOKEN_SUB;
        if (match_char(l, '>')) {
            token.kind = TOKEN_ARROW;
        } else if (match_char(l, '=')) {
            token.kind = TOKEN_SUB_SET;
        }
        break;

    case '*':
        token.kind = TOKEN_MUL;
        if (match_char(l, '=')) {
            token.kind = TOKEN_MUL_SET;
        }
        break;

    case '/':
        token.kind = TOKEN_DIV;
        if (match_char(l, '=')) {
            token.kind = TOKEN_DIV_SET;
        }
        break;

    case '%':
        token.kind = TOKEN_MOD;
        if (match_char(l, '=')) {
            token.kind = TOKEN_MOD_SET;
        }
        break;

    case '|':
        token.kind = TOKEN_BOR;
        if (match_char(l, '|')) {
            token.kind = TOKEN_LOR;
        } else if (match_char(l, '=')) {
            token.kind = TOKEN_BOR_SET;
        }
        break;

    case '&':
        token.kind = TOKEN_BAND;
        if (match_char(l, '&')) {
            token.kind = TOKEN_LAND;
        } else if (match_char(l, '=')) {
            token.kind = TOKEN_BAND_SET;
        }
        break;

    case '~':
        token.kind = TOKEN_BNOT;
        break;

    case '!':
        if (match_char(l, '=')) {
            token.kind = TOKEN_NE;
        } else {
            token.kind = TOKEN_LNOT;
        }
        break;

    case '>':
        if (match_char(l, '>')) {
            token.kind = TOKEN_SHR;
            if (match_char(l, '=')) {
                token.kind = TOKEN_SHR_SET;
            }
        } else if (match_char(l, '=')) {
            token.kind = TOKEN_GE;
        } else {
            token.kind = TOKEN_GT;
        }
        break;

    case '<':
        if (match_char(l, '<')) {
            token.kind = TOKEN_SHL;
            if (match_char(l, '=')) {
                token.kind = TOKEN_SHL_SET;
            }
        } else if (match_char(l, '=')) {
            token.kind = TOKEN_LE;
        } else {
            token.kind = TOKEN_LT;
        }
        break;

    case '=':
        if (match_char(l, '=')) {
            token.kind = TOKEN_EQ;
        } else {
            token.kind = TOKEN_SET;
        }
        break;

    case '#':
        while (l->sv.count > 0 && isident(*l->sv.data)) {
            next_char(l);
        }
        token.sv.count -= l->sv.count;

        if (sv_match(token.sv, "#if")) {
            token.kind = TOKEN_DIRECTIVE_IF;
        } else if (sv_match(token.sv, "#assert")) {
            token.kind = TOKEN_DIRECTIVE_ASSERT;
        } else if (sv_match(token.sv, "#link")) {
            token.kind = TOKEN_DIRECTIVE_LINK;
        } else if (sv_match(token.sv, "#import")) {
            token.kind = TOKEN_DIRECTIVE_IMPORT;
        } else if (sv_match(token.sv, "#static")) {
            token.kind = TOKEN_DIRECTIVE_STATIC;
        } else if (sv_match(token.sv, "#private")) {
            token.kind = TOKEN_DIRECTIVE_PRIVATE;
        } else if (sv_match(token.sv, "#library")) {
            token.kind = TOKEN_DIRECTIVE_LIBRARY;
        } else if (sv_match(token.sv, "#main")) {
            token.kind = TOKEN_DIRECTIVE_MAIN;
        } else if (sv_match(token.sv, "#platform")) {
            token.kind = TOKEN_DIRECTIVE_PLATFORM;
        } else if (sv_match(token.sv, "#caller_location")) {
            token.kind = TOKEN_DIRECTIVE_CALLER_LOCATION;
        } else {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Invalid compile time directive '" SV_Fmt "'\n",
                Pos_Arg(token.pos),
                SV_Arg(token.sv));
            exit(1);
        }
        break;

    default:
        error_invalid(token.pos, token.sv, "character");
        break;
    }

    token.sv.count -= l->sv.count;
    return token;
}
