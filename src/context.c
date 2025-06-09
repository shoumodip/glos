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
