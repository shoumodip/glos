#ifndef PARSER_H
#define PARSER_H

#include "ast.h"
#include "lexer.h"

typedef struct {
    Node  *data;
    size_t length;
} NodePool;

typedef struct {
    Lexer lexer;
    bool  local;
    bool  inExtern;
    bool  dontConsumeEols;

    Nodes    nodes;
    NodePool pool;
} Parser;

void parseFile(Parser *p, Lexer lexer);

#endif // PARSER_H
