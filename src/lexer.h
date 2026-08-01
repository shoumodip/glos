#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    Pos  pos;
    SV   sv;
    bool newline;
} Lexer;

bool lexer_open(Lexer *l, const char *path);

// For normal strings:
//
//   "Foo"
//   ^
//   pos,start
//
// For interpolated continuation strings:
//
//   "Foo \{69} Yes"
//   ^        ^
//   start    pos
Token lexer_get_string(Lexer *l, Pos pos, Pos start);

Token lexer_iter(Lexer *l);

#endif // LEXER_H
