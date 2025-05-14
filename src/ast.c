#include "ast.h"

static_assert(COUNT_TYPES == 2, "");
const char *typeToString(Type type) {
    switch (type.kind) {
    case TYPE_BOOL:
        return "bool";

    case TYPE_I64:
        return "i64";

    default:
        unreachable();
    }
}

bool typeEq(Type a, Type b) {
    return a.kind == b.kind;
}
