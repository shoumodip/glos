#include "token.h"

static_assert(COUNT_TOKENS == 77, "");
const char *token_kind_to_cstr(Token_Kind kind) {
    switch (kind) {
    case TOKEN_EOF:
        return "end of file";

    case TOKEN_EOL:
        return "';'";

    case TOKEN_DOT:
        return "'.'";

    case TOKEN_ARROW:
        return "'->'";

    case TOKEN_COLON:
        return "':'";

    case TOKEN_COMMA:
        return "','";

    case TOKEN_RANGE:
        return "'..'";

    case TOKEN_SPREAD:
        return "'...'";

    case TOKEN_INT:
        return "integer";

    case TOKEN_BOOL:
        return "boolean";

    case TOKEN_CHAR:
        return "character";

    case TOKEN_NULL:
        return "'null'";

    case TOKEN_IDENT:
        return "identifier";

    case TOKEN_STRING:
    case TOKEN_ISTRING:
        return "string";

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

    case TOKEN_MOD:
        return "'%'";

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

    case TOKEN_ADD_SET:
        return "'+='";

    case TOKEN_SUB_SET:
        return "'-='";

    case TOKEN_MUL_SET:
        return "'*='";

    case TOKEN_DIV_SET:
        return "'/='";

    case TOKEN_MOD_SET:
        return "'%='";

    case TOKEN_SHL_SET:
        return "'<<='";

    case TOKEN_SHR_SET:
        return "'>>='";

    case TOKEN_BOR_SET:
        return "'|='";

    case TOKEN_BAND_SET:
        return "'&='";

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

    case TOKEN_ENUM:
        return "'enum'";

    case TOKEN_TRAIT:
        return "'trait'";

    case TOKEN_UNION:
        return "'union'";

    case TOKEN_STRUCT:
        return "'struct'";

    case TOKEN_SIZEOF:
        return "'sizeof'";

    case TOKEN_TYPEOF:
        return "'typeof'";

    case TOKEN_INLINE:
        return "'inline'";

    case TOKEN_DISTINCT:
        return "'distinct'";

    case TOKEN_DIRECTIVE_IF:
        return "'#if'";

    case TOKEN_DIRECTIVE_ASSERT:
        return "'#assert'";

    case TOKEN_DIRECTIVE_LINK:
        return "'#link'";

    case TOKEN_DIRECTIVE_IMPORT:
        return "'#import'";

    case TOKEN_DIRECTIVE_STATIC:
        return "'#static'";

    case TOKEN_DIRECTIVE_PRIVATE:
        return "'#private'";

    case TOKEN_DIRECTIVE_LIBRARY:
        return "'#library'";

    case TOKEN_DIRECTIVE_MAIN:
        return "'#main'";

    case TOKEN_DIRECTIVE_PLATFORM:
        return "'#platform'";

    case TOKEN_DIRECTIVE_CALLER_LOCATION:
        return "'#caller_location'";

    case TOKEN_IF:
        return "'if'";

    case TOKEN_ELSE:
        return "'else'";

    case TOKEN_FOR:
        return "'for'";

    case TOKEN_CASE:
        return "'case'";

    case TOKEN_DEFER:
        return "'defer'";

    case TOKEN_BREAK:
        return "'break'";

    case TOKEN_CONTINUE:
        return "'continue'";

    case TOKEN_RETURN:
        return "'return'";

    case TOKEN_EXTERN:
        return "'extern'";

    default:
        unreachable();
    }
}
