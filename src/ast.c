#include "ast.h"

Node *scopeFind(Scope s, Str name) {
    for (size_t i = s.length; i > 0; i--) {
        Node *it = s.data[i - 1];
        if (strEq(it->token.str, name)) {
            return it;
        }
    }

    return NULL;
}

static_assert(COUNT_TYPES == 4, "");
const char *typeToString(Type type) {
    switch (type.kind) {
    case TYPE_NIL:
        return "nil";

    case TYPE_BOOL:
        return "bool";

    case TYPE_I64:
        return "i64";

    case TYPE_FN:
        return "fn ()";

    default:
        unreachable();
    }
}

bool typeEq(Type a, Type b) {
    return a.kind == b.kind;
}
