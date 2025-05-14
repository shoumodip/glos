#include "token.h"

static_assert(COUNT_TOKENS == 9, "");
const char *tokenKindName(TokenKind kind) {
    switch (kind) {
    case TOKEN_EOF:
        return "end of file";

    case TOKEN_INT:
        return "integer";

    case TOKEN_BOOL:
        return "boolean";

    case TOKEN_IDENT:
        return "identifier";

    case TOKEN_ADD:
        return "'+'";

    case TOKEN_SUB:
        return "'-'";

    case TOKEN_MUL:
        return "'*'";

    case TOKEN_DIV:
        return "'/'";

    case TOKEN_PRINT:
        return "'print'";

    default:
        unreachable();
    }
}
