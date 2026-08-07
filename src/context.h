#ifndef CONTEXT_H
#define CONTEXT_H

#include "node.h"

void       local_scope_push(Local_Scope *scope, Node_Atom *node);
Node_Atom *local_scope_find(Local_Scope scope, SV name);

void       global_scope_push(Global_Scope *scope, Node_Atom *node);
Node_Atom *global_scope_find(Global_Scope *scope, SV name);

struct Context_Fn {
    Node_Fn *fn;

    size_t defines_begin;
    size_t defines_end;

    size_t imports_begin;
    size_t imports_end;

    Context_Fn *outer;
};

typedef struct {
    Local_Scope  defines;
    Node_Imports imports;

    Context_Fn      *fn;
    Context_Replace *replace;
} Context;

// The function `context_push_fn()` will modify the following fields of `fn`:
//   - begin
//   - end
void context_push_fn(Context *c, Context_Fn *fn);
void context_pop_fn(Context *c);
void context_restore_fn(Context *c, Context_Fn *save);

void context_push_define(Context *c, Node_Atom *define);
void context_push_import(Context *c, Node_Import *import);

Node_Atom *context_find_define_in_fn(const Context *c, const Context_Fn *fn, SV name);
Node_Atom *context_find_define_skipping(const Context *c, SV name, Module *skip);
Node_Atom *context_find_define(const Context *c, SV name);

void context_set_end(Context *c, size_t defines_end, size_t imports_end);

#endif // CONTEXT_H
