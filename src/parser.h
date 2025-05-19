#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Lexer lexer;
    bool  local;
    bool  inExtern;
    bool  dontConsumeEols;

    Nodes      nodes;
    NodeAlloc *nodeAlloc;
} Parser;

void parseFile(Parser *p, Lexer lexer);

#endif // PARSER_H
