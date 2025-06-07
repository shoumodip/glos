#ifndef PARSER_H
#define PARSER_H

#include "lexer/lexer.h"
#include "node/node.h"

typedef struct {
    Arena *arena;
    Lexer  lexer;

    Nodes nodes;
} Parser;

void parse_file(Parser *p, Lexer lexer);

#endif // PARSER_H
