#ifndef LEXER_H
#define LEXER_H

#include "token.h"

typedef struct {
    Pos  pos;
    SV   sv;
    bool newline;
    bool after_operator_keyword;
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

#ifdef PROFILING
extern size_t total_lines_processed;
#endif // PROFILING

#endif // LEXER_H
