#ifndef TOKEN_H
#define TOKEN_H

#include "basic.h"

typedef struct {
    const char *path;
    size_t      row;
    size_t      col;
} Pos;

#define Pos_Fmt    "%s:%zu:%zu: "
#define Pos_Arg(p) ((p).path), ((p).row + 1), ((p).col + 1)

typedef enum {
    TOKEN_EOF,
    TOKEN_EOL,
    TOKEN_DOT,
    TOKEN_ARROW,
    TOKEN_COLON,
    TOKEN_COMMA,
    TOKEN_RANGE,
    TOKEN_SPREAD,

    TOKEN_INT,
    TOKEN_BOOL,
    TOKEN_CHAR,
    TOKEN_NULL,
    TOKEN_IDENT,
    TOKEN_STRING,
    TOKEN_ISTRING,

    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,

    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_MUL,
    TOKEN_DIV,
    TOKEN_MOD,

    TOKEN_SHL,
    TOKEN_SHR,
    TOKEN_BOR,
    TOKEN_BAND,
    TOKEN_BNOT,

    TOKEN_SET,
    TOKEN_ADD_SET,
    TOKEN_SUB_SET,
    TOKEN_MUL_SET,
    TOKEN_DIV_SET,
    TOKEN_MOD_SET,
    TOKEN_SHL_SET,
    TOKEN_SHR_SET,
    TOKEN_BOR_SET,
    TOKEN_BAND_SET,

    TOKEN_LOR,
    TOKEN_LAND,
    TOKEN_LNOT,

    TOKEN_GT,
    TOKEN_GE,
    TOKEN_LT,
    TOKEN_LE,
    TOKEN_EQ,
    TOKEN_NE,

    TOKEN_ENUM,
    TOKEN_TRAIT,
    TOKEN_UNION,
    TOKEN_STRUCT,
    TOKEN_SIZEOF,
    TOKEN_TYPEOF,

    TOKEN_INLINE,
    TOKEN_DISTINCT,

    TOKEN_DIRECTIVE_IF,
    TOKEN_DIRECTIVE_ASSERT,

    TOKEN_DIRECTIVE_LINK,
    TOKEN_DIRECTIVE_IMPORT,
    TOKEN_DIRECTIVE_STATIC,
    TOKEN_DIRECTIVE_PRIVATE,
    TOKEN_DIRECTIVE_LIBRARY,

    TOKEN_DIRECTIVE_MAIN,
    TOKEN_DIRECTIVE_PLATFORM,
    TOKEN_DIRECTIVE_CALLER_LOCATION,

    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,
    TOKEN_CASE,

    TOKEN_DEFER,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_RETURN,

    TOKEN_EXTERN,
    COUNT_TOKENS
} Token_Kind;

const char *token_kind_to_cstr(Token_Kind kind);

typedef struct {
    Token_Kind kind;

    SV   sv;
    Pos  pos;
    bool newline;

    union {
        u64 integer;
    } as;
} Token;

#endif // TOKEN_H
