#ifndef TOKEN_H
#define TOKEN_H

#include "basic.h"

typedef struct {
    const char *path;
    size_t      row;
    size_t      col;
} Pos;

#define PosFmt    "%s:%zu:%zu: "
#define PosArg(p) ((p).path), ((p).row + 1), ((p).col + 1)

typedef enum {
    TOKEN_EOF,
    TOKEN_EOL,
    TOKEN_DOT,
    TOKEN_COMMA,

    TOKEN_INT,
    TOKEN_BOOL,
    TOKEN_IDENT,

    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LPAREN,
    TOKEN_RPAREN,

    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_MUL,
    TOKEN_DIV,

    TOKEN_SHL,
    TOKEN_SHR,
    TOKEN_BOR,
    TOKEN_BAND,
    TOKEN_BNOT,

    TOKEN_SET,

    TOKEN_LOR,
    TOKEN_LAND,
    TOKEN_LNOT,

    TOKEN_GT,
    TOKEN_GE,
    TOKEN_LT,
    TOKEN_LE,
    TOKEN_EQ,
    TOKEN_NE,

    TOKEN_SIZEOF,

    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,

    TOKEN_RETURN,

    TOKEN_FN,
    TOKEN_VAR,
    TOKEN_TYPE,
    TOKEN_STRUCT,
    TOKEN_EXTERN,

    TOKEN_PRINT,
    COUNT_TOKENS
} TokenKind;

const char *tokenKindName(TokenKind kind);

typedef struct {
    TokenKind kind;

    Str  str;
    Pos  pos;
    bool onNewline;

    union {
        bool   boolean;
        size_t integer;
    } as;
} Token;

#endif // TOKEN_H
