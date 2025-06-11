#include "node.h"

static_assert(COUNT_TYPES == 4, "");
const char *type_to_cstr(Type type) {
    const char *s = temp_alloc(0);

    switch (type.kind) {
    case TYPE_UNIT:
        temp_sprintf("()");
        break;

    case TYPE_BOOL:
        temp_sprintf("bool");
        break;

    case TYPE_I64:
        temp_sprintf("i64");
        break;

    case TYPE_FN:
        temp_sprintf("fn (");
        {
            NodeFn *spec = (NodeFn *) type.spec;
            for (Node *it = spec->args.head; it; it = it->next) {
                temp_remove_null();
                type_to_cstr(it->type);

                if (it->next) {
                    temp_remove_null();
                    temp_sprintf(", ");
                }
            }
        }
        temp_remove_null();
        temp_sprintf(")");
        break;

    default:
        unreachable();
    }

    return s;
}

static_assert(COUNT_TYPES == 4, "");
bool type_eq(Type a, Type b) {
    if (a.kind != b.kind) {
        return false;
    }

    switch (a.kind) {
    case TYPE_FN: {
        NodeFn *a_spec = (NodeFn *) a.spec;
        NodeFn *b_spec = (NodeFn *) b.spec;

        if (a_spec->arity != b_spec->arity) {
            return false;
        }

        for (Node *a = a_spec->args.head, *b = b_spec->args.head; a; a = a->next, b = b->next) {
            if (!type_eq(a->type, b->type)) {
                return false;
            }
        }

        return true;
    } break;

    default:
        return true;
    }
}

static_assert(COUNT_TYPES == 4, "");
bool type_is_integer(Type type) {
    return type.kind == TYPE_I64;
}
