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

    case TYPE_FN: {
        const NodeFn *spec = (const NodeFn *) type.spec;

        temp_sprintf("fn (");
        for (const Node *it = spec->args.head; it; it = it->next) {
            temp_remove_null();
            type_to_cstr(it->type);

            if (it->next) {
                temp_remove_null();
                temp_sprintf(", ");
            }
        }
        temp_remove_null();
        temp_sprintf(")");

        if (spec->ret) {
            temp_remove_null();
            temp_sprintf(" ");
            temp_remove_null();
            type_to_cstr(spec->ret->type);
        }
    } break;

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
        const NodeFn *a_spec = (const NodeFn *) a.spec;
        const NodeFn *b_spec = (const NodeFn *) b.spec;

        if (a_spec->arity != b_spec->arity) {
            return false;
        }

        for (const Node *a = a_spec->args.head, *b = b_spec->args.head; a; a = a->next, b = b->next) {
            if (!type_eq(a->type, b->type)) {
                return false;
            }
        }

        return type_eq(node_fn_return_type(a_spec), node_fn_return_type(b_spec));
    } break;

    default:
        return true;
    }
}

static_assert(COUNT_TYPES == 4, "");
bool type_is_integer(Type type) {
    return type.kind == TYPE_I64;
}

Type node_fn_return_type(const NodeFn *fn) {
    if (fn->ret) {
        return fn->ret->type;
    }

    return (Type) {.kind = TYPE_UNIT};
}
