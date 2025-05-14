#ifndef PARSER_H
#define PARSER_H

#include "ast.h"

typedef struct {
    Node  *data;
    size_t length;
} NodePool;

typedef struct {
    Lexer lexer;
    Nodes nodes;

    NodePool pool;
} Parser;

void parseFile(Parser *p, Lexer lexer);

#endif // PARSER_H
