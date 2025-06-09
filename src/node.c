#include "node.h"

static_assert(COUNT_TYPES == 2, "");
const char *type_to_cstr(Type type) {
    switch (type.kind) {
    case TYPE_BOOL:
        return "bool";

    case TYPE_I64:
        return "i64";

    default:
        unreachable();
    }
}

bool type_eq(Type a, Type b) {
    return a.kind == b.kind;
}

bool type_is_integer(Type type) {
    return type.kind == TYPE_I64;
}
