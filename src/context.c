#include "context.h"
#include "basic.h"
#include "node.h"
#include "token.h"

void local_scope_push(Local_Scope *scope, Node_Atom *atom) {
    da_push(scope, atom);
}

Node_Atom *local_scope_find(Local_Scope scope, SV name) {
    for (size_t i = scope.count; i > 0; i--) {
        Node_Atom *it = scope.data[i - 1];
        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

void global_scope_push(Global_Scope *scope, Node_Atom *atom) {
    ht_set(scope, atom->node.token.sv, atom);
}

Node_Atom *global_scope_find(Global_Scope *scope, SV name) {
    if (!scope->hasheq) {
        scope->hasheq = ht_hasheq_sv;
    }

    Node_Atom **p = ht_get(scope, name);
    return p ? *p : NULL;
}

void context_push_fn(Context *c, Context_Fn *fn) {
    assert(fn);
    fn->defines_begin = c->defines.count;
    fn->defines_end = c->defines.count;

    fn->imports_begin = c->imports.count;
    fn->imports_end = c->imports.count;
    c->fn = fn;
}

void context_pop_fn(Context *c) {
    assert(c->fn);
    c->defines.count = c->fn->defines_begin;
    c->imports.count = c->fn->imports_begin;
    c->fn = c->fn->outer;
}

void context_restore_fn(Context *c, Context_Fn *save) {
    c->fn = save;
    if (c->fn) {
        c->defines.count = c->fn->defines_end;
        c->imports.count = c->fn->imports_end;
    }
}

void context_push_define(Context *c, Node_Atom *define) {
    assert(c->fn);
    da_push(&c->defines, define);
    c->fn->defines_end++;
}

void context_push_import(Context *c, Node_Import *import) {
    assert(c->fn);
    da_push(&c->imports, import);
    c->fn->imports_end++;
}

Node_Atom *context_find_define_in_fn(const Context *c, const Context_Fn *fn, SV name) {
    for (size_t i = fn->defines_end; i > fn->defines_begin; i--) {
        Node_Atom *it = c->defines.data[i - 1];
        if (sv_eq(it->node.token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

Node_Atom *context_find_define_skipping(const Context *c, SV name, Module *skip) {
    for (Context_Fn *fn = c->fn; fn; fn = fn->outer) {
        Node_Atom *it = context_find_define_in_fn(c, fn, name);
        if (it) {
            return it;
        }
    }

    for (Context_Fn *fn = c->fn; fn; fn = fn->outer) {
        for (size_t i = fn->imports_end; i > fn->imports_begin; i--) {
            Module *module = c->imports.data[i - 1]->module;
            if (skip && module == skip) {
                continue;
            }

            Node_Atom *define = global_scope_find(&module->globals, name);
            if (define) {
                return define;
            }
        }
    }

    return NULL;
}

Node_Atom *context_find_define(const Context *c, SV name) {
    return context_find_define_skipping(c, name, NULL);
}

void context_set_end(Context *c, size_t defines_end, size_t imports_end) {
    if (c->fn) {
        assert(c->fn->defines_end == c->defines.count);
        c->fn->defines_end = defines_end;
        c->defines.count = defines_end;

        assert(c->fn->imports_end == c->imports.count);
        c->fn->imports_end = imports_end;
        c->imports.count = imports_end;
    }
}
