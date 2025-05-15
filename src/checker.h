#ifndef CHECKER_H
#define CHECKER_H

#include "ast.h"

typedef struct {
    Node **data;
    size_t length;
    size_t capacity;
} Scope;

Node *scopeFind(Scope s, Str name);

typedef struct {
    Scope globals;
} Context;

void checkNodes(Context *c, Nodes nodes);

#endif // CHECKER_H
