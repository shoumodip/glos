#include "token.h"

static_assert(COUNT_TOKENS == 49, "");
const char *tokenKindName(TokenKind kind) {
    switch (kind) {
    case TOKEN_EOF:
        return "end of file";

    case TOKEN_EOL:
        return "';'";

    case TOKEN_DOT:
        return "'.'";

    case TOKEN_COMMA:
        return "','";

    case TOKEN_COLON:
        return "':'";

    case TOKEN_RANGE:
        return "'..'";

    case TOKEN_WALRUS:
        return "':='";

    case TOKEN_INT:
        return "integer";

    case TOKEN_BOOL:
        return "boolean";

    case TOKEN_STR:
    case TOKEN_CSTR:
        return "string";

    case TOKEN_CHAR:
        return "character";

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

    case TOKEN_LBRACKET:
        return "'['";

    case TOKEN_RBRACKET:
        return "']'";

    case TOKEN_ADD:
        return "'+'";

    case TOKEN_SUB:
        return "'-'";

    case TOKEN_MUL:
        return "'*'";

    case TOKEN_DIV:
        return "'/'";

    case TOKEN_SHL:
        return "'<<'";

    case TOKEN_SHR:
        return "'>>'";

    case TOKEN_BOR:
        return "'|'";

    case TOKEN_BAND:
        return "'&'";

    case TOKEN_BNOT:
        return "'~'";

    case TOKEN_SET:
        return "'='";

    case TOKEN_LOR:
        return "'||'";

    case TOKEN_LAND:
        return "'&&'";

    case TOKEN_LNOT:
        return "'!'";

    case TOKEN_GT:
        return "'>'";

    case TOKEN_GE:
        return "'>='";

    case TOKEN_LT:
        return "'<'";

    case TOKEN_LE:
        return "'<='";

    case TOKEN_EQ:
        return "'=='";

    case TOKEN_NE:
        return "'!='";

    case TOKEN_SIZEOF:
        return "'sizeof'";

    case TOKEN_IF:
        return "'if'";

    case TOKEN_ELSE:
        return "'else'";

    case TOKEN_FOR:
        return "'for'";

    case TOKEN_RETURN:
        return "'return'";

    case TOKEN_FN:
        return "'fn'";

    case TOKEN_VAR:
        return "'var'";

    case TOKEN_TYPE:
        return "'type'";

    case TOKEN_STRUCT:
        return "'struct'";

    case TOKEN_EXTERN:
        return "'extern'";

    case TOKEN_PRINT:
        return "'print'";

    default:
        unreachable();
    }
}
