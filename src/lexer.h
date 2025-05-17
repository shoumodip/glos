#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    Pos  pos;
    Str  str;
    bool onNewline;

    bool  peeked;
    Token buffer;
} Lexer;

bool lexerNew(Lexer *l, const char *path);

void lexerBuffer(Lexer *l, Token token);
void lexerUnbuffer(Lexer *l);

Token lexerNext(Lexer *l);
Token lexerPeek(Lexer *l);
bool  lexerRead(Lexer *l, TokenKind kind);

Token lexerExpect(Lexer *l, const TokenKind *kinds);
#define lexerExpect(l, ...) lexerExpect((l), (const TokenKind[]) {__VA_ARGS__, TOKEN_EOF})

Token lexerSplitToken(Lexer *l, Token token);

#endif // LEXER_H
