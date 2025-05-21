#ifndef CHECKER_H
#define CHECKER_H

#include "ast.h"

typedef struct {
    NodeFn *fn;
    size_t  base;
} FnContext;

typedef struct {
    bool      inExtern;
    FnContext fnContext;

    Scope locals;
    Scope globals;
    Scope globalTemps;

    Type strType;

    NodeAlloc *nodeAlloc;
} Context;

void checkNodes(Context *c, Nodes nodes);

#endif // CHECKER_H
