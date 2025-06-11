#include "context.h"

Node *scope_find(Scope s, SV name) {
    for (size_t i = s.count; i > 0; i--) {
        Node *it = s.data[i - 1];
        if (sv_eq(it->token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

ContextFn context_fn_begin(Context *c, NodeFn *fn) {
    const ContextFn save = c->fn;
    c->fn.base = c->locals.count;
    c->fn.fn = fn;
    return save;
}

void context_fn_end(Context *c, ContextFn save) {
    c->locals.count = c->fn.base;
    c->fn = save;
}

Node *context_fn_find(ContextFn f, Scope s, SV name) {
    assert(f.base <= s.count);
    s.data += f.base;
    s.count -= f.base;
    return scope_find(s, name);
}
