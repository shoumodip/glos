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

    TOKEN_INT,
    TOKEN_BOOL,
    TOKEN_IDENT,

    TOKEN_LPAREN,
    TOKEN_RPAREN,

    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_MUL,
    TOKEN_DIV,

    TOKEN_PRINT,
    COUNT_TOKENS
} TokenKind;

const char *token_kind_to_cstr(TokenKind kind);

typedef struct {
    TokenKind kind;

    SV   sv;
    Pos  pos;
    bool newline;

    union {
        bool   boolean;
        size_t integer;
    } as;
} Token;

#endif // TOKEN_H
