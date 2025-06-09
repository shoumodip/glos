#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    Pos  pos;
    SV   sv;
    bool newline;

    bool  peeked;
    Token buffer;
} Lexer;

bool lexer_open(Lexer *l, const char *path);

void lexer_buffer(Lexer *l, Token token);
void lexer_unbuffer(Lexer *l);

Token lexer_next(Lexer *l);
Token lexer_peek(Lexer *l);
bool  lexer_read(Lexer *l, TokenKind kind);

Token lexer_expect_impl(Lexer *l, const TokenKind *kinds);
#define lexer_expect(l, ...) lexer_expect_impl((l), (const TokenKind[]) {__VA_ARGS__, TOKEN_EOF})

#endif // LEXER_H
