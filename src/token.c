#include "token.h"

static_assert(COUNT_TOKENS == 21, "");
const char *token_kind_to_cstr(TokenKind kind) {
    switch (kind) {
    case TOKEN_EOF:
        return "end of file";

    case TOKEN_EOL:
        return "';'";

    case TOKEN_COMMA:
        return "','";

    case TOKEN_INT:
        return "integer";

    case TOKEN_BOOL:
        return "boolean";

    case TOKEN_IDENT:
        return "identifier";

    case TOKEN_LPAREN:
        return "'('";

    case TOKEN_RPAREN:
        return "')'";

    case TOKEN_LBRACE:
        return "'{'";

    case TOKEN_RBRACE:
        return "'}'";

    case TOKEN_ADD:
        return "'+'";

    case TOKEN_SUB:
        return "'-'";

    case TOKEN_MUL:
        return "'*'";

    case TOKEN_DIV:
        return "'/'";

    case TOKEN_SET:
        return "'='";

    case TOKEN_IF:
        return "'if'";

    case TOKEN_ELSE:
        return "'else'";

    case TOKEN_RETURN:
        return "'return'";

    case TOKEN_FN:
        return "'fn'";

    case TOKEN_VAR:
        return "'var'";

    case TOKEN_PRINT:
        return "'print'";

    default:
        unreachable();
    }
}
