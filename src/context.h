#ifndef CONTEXT_H
#define CONTEXT_H

#include "node.h"

typedef struct {
    Node **data;
    size_t count;
    size_t capacity;
} Scope;

Node *scope_find(Scope s, SV name);

typedef struct {
    Scope locals;
    Scope globals;
} Context;

#endif // CONTEXT_H
