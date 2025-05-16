#include "checker.h"

// Scope
static void scopePush(Scope *s, Node *n) {
    if (s->length >= s->capacity) {
        s->capacity = s->capacity ? s->capacity * 2 : 128;
        s->data = realloc(s->data, s->capacity * sizeof(*s->data));
        assert(s->data);
    }

    s->data[s->length++] = n;
}

static FnContext fnContextBegin(Context *c, NodeFn *fn) {
    const FnContext save = c->fnContext;
    c->fnContext.fn = fn;
    c->fnContext.base = c->locals.length;
    return save;
}

static void fnContextEnd(Context *c, FnContext save) {
    c->fnContext = save;
    c->locals.length = save.base;
}

static Node *fnContextFind(FnContext f, Scope s, Str name) {
    assert(f.base <= s.length);
    s.data += f.base;
    s.length -= f.base;
    return scopeFind(s, name);
}

static Node *identFind(Context *c, Str name) {
    if (c->fnContext.fn) {
        Node *local = fnContextFind(c->fnContext, c->locals, name);
        if (local) {
            return local;
        }
    }

    return scopeFind(c->globals, name);
}

static size_t blockBegin(Context *c) {
    return c->locals.length;
}

static void blockRestore(Context *c, size_t save) {
    c->locals.length = save;
}

// Checker
static Type typeAssert(const Node *n, Type expected) {
    if (!typeEq(n->type, expected)) {
        fprintf(
            stderr,
            PosFmt "ERROR: Expected type '%s', got '%s'\n",
            PosArg(n->token.pos),
            typeToString(expected),
            typeToString(n->type));

        exit(1);
    }
    return n->type;
}

static Type typeAssertArith(const Node *n) {
    if (n->type.kind != TYPE_I64 && n->type.ref == 0) {
        fprintf(
            stderr,
            PosFmt "ERROR: Expected arithmetic type, got '%s'\n",
            PosArg(n->token.pos),
            typeToString(n->type));

        exit(1);
    }
    return n->type;
}

static Type typeAssertScalar(const Node *n) {
    if (n->type.kind != TYPE_I64 && n->type.kind != TYPE_BOOL && n->type.ref == 0) {
        fprintf(
            stderr,
            PosFmt "ERROR: Expected scalar type, got '%s'\n",
            PosArg(n->token.pos),
            typeToString(n->type));

        exit(1);
    }
    return n->type;
}

static void errorUndefined(const Node *n, const char *label) {
    fprintf(
        stderr,
        PosFmt "ERROR: Undefined %s '" StrFmt "'\n",
        PosArg(n->token.pos),
        label,
        StrArg(n->token.str));

    exit(1);
}

static void errorRedefinition(const Node *n, const Node *previous, const char *label) {
    fprintf(
        stderr,
        PosFmt "ERROR: Redefinition of %s '" StrFmt "'\n",
        PosArg(n->token.pos),
        label,
        StrArg(n->token.str));

    fprintf(stderr, PosFmt "NOTE: Defined here\n", PosArg(previous->token.pos));
    exit(1);
}

static_assert(COUNT_TYPES == 4, "");
static void checkType(Context *c, Node *n) {
    unused(c);

    switch (n->kind) {
    case NODE_ATOM:
        if (strMatch(n->token.str, "bool")) {
            n->type = (Type) {.kind = TYPE_BOOL};
        } else if (strMatch(n->token.str, "i64")) {
            n->type = (Type) {.kind = TYPE_I64};
        } else {
            errorUndefined(n, "type");
        }
        break;

    case NODE_UNARY:
        checkType(c, n->as.unary.operand);
        n->type = n->as.unary.operand->type;
        n->type.ref++;
        break;

    case NODE_FN:
        for (Node *it = n->as.fn.args.head; it; it = it->next) {
            checkType(c, it->as.arg.type);
            it->type = it->as.arg.type->type;
        }

        assert(!n->as.fn.ret);
        n->type = (Type) {.kind = TYPE_FN, .spec = n};
        break;

    default:
        unreachable();
    }
}

static void refPrevent(Node *n, bool ref) {
    if (ref) {
        fprintf(
            stderr,
            PosFmt "ERROR: Cannot take reference to value not in memory\n",
            PosArg(n->token.pos));

        exit(1);
    }
}

static_assert(COUNT_NODES == 11, "");
static void checkExpr(Context *c, Node *n, bool ref) {
    bool allowRef = false;

    switch (n->kind) {
    case NODE_ATOM:
        static_assert(COUNT_TOKENS == 28, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_I64};
            break;

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_IDENT: {
            Node *definition = identFind(c, n->token.str);
            if (definition) {
                n->as.atom.definition = definition;
                n->type = definition->type;
                allowRef = true;

                if (definition->kind == NODE_ARG && ref) {
                    definition->as.arg.memory = true;
                }
            } else {
                errorUndefined(n, "identifier");
            }
        } break;

        default:
            unreachable();
        }
        break;

    case NODE_CALL: {
        Node *fn = n->as.call.fn;
        checkExpr(c, fn, false);

        if (fn->type.kind != TYPE_FN) {
            fprintf(
                stderr,
                PosFmt "ERROR: Cannot call type '%s'\n",
                PosArg(fn->token.pos),
                typeToString(fn->type));

            exit(1);
        }

        if (fn->type.ref != 0) {
            fprintf(
                stderr,
                PosFmt "ERROR: Cannot call type '%s' without dereferencing it first\n",
                PosArg(fn->token.pos),
                typeToString(fn->type));

            exit(1);
        }

        const NodeCall actual = n->as.call;
        const NodeFn   expected = fn->type.spec->as.fn;
        if (actual.arity != expected.arity) {
            fprintf(
                stderr,
                PosFmt "ERROR: Expected %zu argument%s, got %zu\n",
                PosArg(n->token.pos),
                expected.arity,
                expected.arity == 1 ? "" : "s",
                actual.arity);

            exit(1);
        }

        for (Node *a = actual.args.head, *e = expected.args.head; a; a = a->next, e = e->next) {
            checkExpr(c, a, false);
            typeAssert(a, e->type);
        }

        n->type = nodeFnReturnType(fn->type.spec->as.fn);
    } break;

    case NODE_UNARY: {
        Node *operand = n->as.unary.operand;

        static_assert(COUNT_TOKENS == 28, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            checkExpr(c, operand, false);
            n->type = typeAssertArith(operand);
            break;

        case TOKEN_MUL:
            checkExpr(c, operand, false);
            if (operand->type.ref == 0) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Expected pointer type, got '%s'\n",
                    PosArg(operand->token.pos),
                    typeToString(operand->type));

                exit(1);
            }
            n->type = operand->type;
            n->type.ref--;

            allowRef = true;
            break;

        case TOKEN_BAND:
            checkExpr(c, operand, true);
            n->type = operand->type;
            n->type.ref++;
            break;

        case TOKEN_LNOT:
            checkExpr(c, operand, false);
            n->type = typeAssert(operand, (Type) {.kind = TYPE_BOOL});
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        Node *lhs = n->as.binary.lhs;
        Node *rhs = n->as.binary.rhs;

        static_assert(COUNT_TOKENS == 28, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
            checkExpr(c, lhs, false);
            checkExpr(c, rhs, false);
            n->type = typeAssert(rhs, typeAssertArith(lhs));
            break;

        case TOKEN_SET:
            checkExpr(c, lhs, true);
            checkExpr(c, rhs, false);
            typeAssert(rhs, lhs->type);
            n->type = (Type) {.kind = TYPE_NIL};
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            checkExpr(c, lhs, false);
            checkExpr(c, rhs, false);
            typeAssert(rhs, typeAssertArith(lhs));
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }

    if (!allowRef) {
        refPrevent(n, ref);
    }
}

static_assert(COUNT_NODES == 11, "");
static void checkStmt(Context *c, Node *n) {
    switch (n->kind) {
    case NODE_BLOCK: {
        const size_t blockSave = blockBegin(c);
        for (Node *it = n->as.block.head; it; it = it->next) {
            checkStmt(c, it);
        }
        blockRestore(c, blockSave);
    } break;

    case NODE_IF:
        checkExpr(c, n->as.iff.condition, false);
        typeAssert(n->as.iff.condition, (Type) {.kind = TYPE_BOOL});

        checkStmt(c, n->as.iff.consequence);
        if (n->as.iff.antecedence) {
            checkStmt(c, n->as.iff.antecedence);
        }
        break;

    case NODE_FOR:
        assert(!n->as.forr.init);
        assert(!n->as.forr.update);

        checkExpr(c, n->as.forr.condition, false);
        typeAssert(n->as.forr.condition, (Type) {.kind = TYPE_BOOL});

        checkStmt(c, n->as.forr.body);
        break;

    case NODE_FN:
        assert(!n->as.fn.ret);

        {
            const Node *previous = scopeFind(c->globals, n->token.str);
            if (previous) {
                errorRedefinition(n, previous, "identifier");
            }
        }

        n->type = (Type) {.kind = TYPE_FN, .spec = n};
        scopePush(&c->globals, n);

        {
            const FnContext fnContextSave = fnContextBegin(c, &n->as.fn);
            for (Node *it = n->as.fn.args.head; it; it = it->next) {
                checkStmt(c, it);
            }

            checkStmt(c, n->as.fn.body);
            fnContextEnd(c, fnContextSave);
        }
        break;

    case NODE_ARG:
        checkType(c, n->as.arg.type);
        n->type = n->as.arg.type->type;

        scopePush(&c->locals, n);
        scopePush(&c->fnContext.fn->locals, n);
        break;

    case NODE_VAR:
        if (!n->as.var.local) {
            const Node *previous = scopeFind(c->globals, n->token.str);
            if (previous) {
                errorRedefinition(n, previous, "identifier");
            }
        }

        if (n->as.var.type) {
            checkType(c, n->as.var.type);
            n->type = n->as.var.type->type;
        }

        if (n->as.var.expr) {
            checkExpr(c, n->as.var.expr, false);
            n->type = n->as.var.expr->type;

            if (n->as.var.type) {
                typeAssert(n->as.var.expr, n->as.var.type->type);
            }
        }

        if (n->as.var.local) {
            scopePush(&c->locals, n);
            scopePush(&c->fnContext.fn->locals, n);
        } else {
            scopePush(&c->globals, n);
        }
        break;

    case NODE_PRINT:
        checkExpr(c, n->as.print.operand, false);
        typeAssertScalar(n->as.print.operand);
        break;

    default:
        checkExpr(c, n, false);
        break;
    }
}

void checkNodes(Context *c, Nodes nodes) {
    for (Node *it = nodes.head; it; it = it->next) {
        checkStmt(c, it);
    }
}
