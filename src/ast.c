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

static_assert(COUNT_TYPES == 5, "");
const char *typeToString(Type type) {
    const char *start = tempSprintf("");
    tempContinue();

    for (size_t i = 0; i < type.ref; i++) {
        tempSprintf("&");
        tempContinue();
    }

    switch (type.kind) {
    case TYPE_UNIT:
        tempSprintf("()");
        break;

    case TYPE_BOOL:
        tempSprintf("bool");
        break;

    case TYPE_I64:
        tempSprintf("i64");
        break;

    case TYPE_FN: {
        tempSprintf("fn (");
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

        const Node *ret = type.spec->as.fn.ret;
        if (ret) {
            tempContinue();
            tempSprintf(" ");

            tempContinue();
            typeToString(ret->type);
        }
    } break;

    case TYPE_RAWPTR:
        tempSprintf("rawptr");
        break;

    default:
        unreachable();
    }

    return start;
}

bool typeEq(Type a, Type b) {
    if (a.kind != b.kind || a.ref != b.ref) {
        return false;
    }

    switch (a.kind) {
    case TYPE_FN: {
        assert(a.spec);
        const NodeFn *aFn = &a.spec->as.fn;

        assert(b.spec);
        const NodeFn *bFn = &b.spec->as.fn;

        if (aFn->arity != bFn->arity) {
            return false;
        }

        for (const Node *a = aFn->args.head, *b = bFn->args.head; a; a = a->next, b = b->next) {
            if (!typeEq(a->type, b->type)) {
                return false;
            }
        }

        return typeEq(nodeFnReturnType(aFn), nodeFnReturnType(bFn));
    };

    default:
        return true;
    }
}

bool typeIsPointer(Type type) {
    return type.kind == TYPE_RAWPTR || type.ref != 0;
}

Type nodeFnReturnType(const NodeFn *fn) {
    if (fn->ret) {
        return fn->ret->type;
    }

    return (Type) {.kind = TYPE_UNIT};
}
