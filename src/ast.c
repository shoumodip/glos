#include "ast.h"

Node *scopeFind(Scope s, Str name, bool isType) {
    for (size_t i = s.length; i > 0; i--) {
        Node *it = s.data[i - 1];
        if (strEq(it->token.str, name) && (it->kind == NODE_TYPE) == isType) {
            return it;
        }
    }

    return NULL;
}

static_assert(COUNT_TYPES == 15, "");
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

    case TYPE_RAWPTR:
        tempSprintf("rawptr");
        break;

    case TYPE_I8:
        tempSprintf("i8");
        break;

    case TYPE_I16:
        tempSprintf("i16");
        break;

    case TYPE_I32:
        tempSprintf("i32");
        break;

    case TYPE_I64:
        tempSprintf("i64");
        break;

    case TYPE_U8:
        tempSprintf("u8");
        break;

    case TYPE_U16:
        tempSprintf("u16");
        break;

    case TYPE_U32:
        tempSprintf("u32");
        break;

    case TYPE_U64:
        tempSprintf("u64");
        break;

    case TYPE_INT:
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

    case TYPE_ALIAS:
        assert(type.spec);
        tempStrToCstr(type.spec->token.str);
        break;

    case TYPE_STRUCT:
        assert(type.spec);
        tempStrToCstr(type.spec->token.str);
        break;

    default:
        unreachable();
    }

    return start;
}

bool typeEq(Type a, Type b) {
    a = typeResolve(a);
    b = typeResolve(b);

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
    }

    case TYPE_STRUCT: {
        assert(a.spec);
        const NodeStruct *aS = &a.spec->as.structt;

        assert(b.spec);
        const NodeStruct *bS = &b.spec->as.structt;

        if (aS->fieldsCount != bS->fieldsCount) {
            return false;
        }

        for (const Node *a = aS->fields.head, *b = bS->fields.head; a; a = a->next, b = b->next) {
            if (!strEq(a->token.str, b->token.str) || !typeEq(a->type, b->type)) {
                return false;
            }
        }

        return true;
    }

    default:
        return true;
    }
}

static_assert(COUNT_TYPES == 15, "");
bool typeIsSigned(Type type) {
    if (type.ref != 0) {
        return false;
    }

    switch (type.kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_INT:
        return true;

    case TYPE_ALIAS:
        assert(type.spec);
        return typeIsSigned(type.spec->as.type.real);

    default:
        return false;
    }
}

static_assert(COUNT_TYPES == 15, "");
bool typeIsInteger(Type type) {
    if (type.ref != 0) {
        return false;
    }

    switch (type.kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:
    case TYPE_INT:
        return true;

    case TYPE_ALIAS:
        assert(type.spec);
        return typeIsInteger(type.spec->as.type.real);

    default:
        return false;
    }
}

bool typeIsPointer(Type type) {
    if (type.kind == TYPE_RAWPTR || type.ref != 0) {
        return true;
    }

    if (type.kind == TYPE_ALIAS) {
        assert(type.spec);
        return typeIsPointer(type.spec->as.type.real);
    }

    return false;
}

Type typeResolve(Type type) {
    if (type.kind == TYPE_ALIAS) {
        assert(type.spec);
        Type resolved = type.spec->as.type.real;

        resolved.ref += type.ref;
        return resolved;
    }

    return type;
}

Type typeRemoveRef(Type type) {
    type.ref = 0;
    return type;
}

Type nodeFnReturnType(const NodeFn *fn) {
    if (fn->ret) {
        return fn->ret->type;
    }

    return (Type) {.kind = TYPE_UNIT};
}

Node *nodeAlloc(NodeAlloc *a, NodeKind kind, Token token) {
#define NODE_POOL_CAP 16000

    if (!a->data) {
        a->data = malloc(NODE_POOL_CAP * sizeof(*a->data));
        assert(a->data);
    }
    assert(a->length < NODE_POOL_CAP);

#undef NODE_POOL_CAP

    Node *node = &a->data[a->length++];
    *node = (Node) {
        .kind = kind,
        .token = token,
    };
    return node;
}
