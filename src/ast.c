#include "ast.h"
#include "basic.h"

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
        return tempSprintf("nil");

    case TYPE_BOOL:
        return tempSprintf("bool");

    case TYPE_I64:
        return tempSprintf("i64");

    case TYPE_FN: {
        const char *str = tempSprintf("fn (");
        {
            assert(type.spec);
            const NodeFn fn = type.spec->as.fn;
            for (Node *arg = fn.args.head; arg; arg = arg->next) {
                if (arg != fn.args.head) {
                    tempContinue();
                    tempSprintf(", ");
                }

                tempContinue();
                typeToString(arg->type);
            }
        }

        tempContinue();
        tempSprintf(")");

        assert(!type.spec->as.fn.ret);
        return str;
    }

    default:
        unreachable();
    }
}

static_assert(sizeof(Type) == 24, "");
bool typeEq(Type a, Type b) {
    if (a.kind != b.kind) {
        return false;
    }

    switch (a.kind) {
    case TYPE_FN: {
        assert(a.spec);
        const NodeFn aFn = a.spec->as.fn;

        assert(b.spec);
        const NodeFn bFn = b.spec->as.fn;

        if (aFn.arity != bFn.arity) {
            return false;
        }

        for (const Node *a = aFn.args.head, *b = bFn.args.head; a; a = a->next, b = b->next) {
            if (!typeEq(a->type, b->type)) {
                return false;
            }
        }

        assert(!aFn.ret && !bFn.ret);
        return true;
    };

    default:
        return true;
    }
}

Type nodeFnReturnType(NodeFn fn) {
    assert(!fn.ret);
    return (Type) {.kind = TYPE_NIL};
}
