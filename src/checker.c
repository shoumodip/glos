#include "checker.h"

static Type type_assert(Node *n, Type expected) {
    if (type_eq(n->type, expected)) {
        return n->type;
    }

    fprintf(
        stderr,
        PosFmt "ERROR: Expected type '%s', got '%s'\n",
        PosArg(n->token.pos),
        type_to_cstr(expected),
        type_to_cstr(n->type));

    exit(1);
}

static Type type_assert_node(Node *a, Node *b) {
    if (type_eq(a->type, b->type)) {
        return a->type;
    }

    fprintf(
        stderr,
        PosFmt "ERROR: Expected type '%s', got '%s'\n",
        PosArg(a->token.pos),
        type_to_cstr(b->type),
        type_to_cstr(a->type));

    exit(1);
}

static Type type_assert_arith(const Node *n) {
    if (type_is_integer(n->type)) {
        return n->type;
    }

    fprintf(stderr, PosFmt "ERROR: Expected arithmetic type, got '%s'\n", PosArg(n->token.pos), type_to_cstr(n->type));
    exit(1);
}

static Type type_assert_scalar(const Node *n) {
    if (type_is_integer(n->type)) {
        return n->type;
    }

    if (type_eq(n->type, (Type) {.kind = TYPE_BOOL})) {
        return n->type;
    }

    fprintf(stderr, PosFmt "ERROR: Expected scalar type, got '%s'\n", PosArg(n->token.pos), type_to_cstr(n->type));
    exit(1);
}

static void error_undefined(const Node *n, const char *label) {
    fprintf(stderr, PosFmt "ERROR: Undefined %s '" SVFmt "'\n", PosArg(n->token.pos), label, SVArg(n->token.sv));
    exit(1);
}

static Node *ident_find(Context *c, SV name) {
    if (c->fn.fn) {
        Node *n = context_fn_find(c->fn, c->locals, name);
        if (n) {
            return n;
        }
    }

    return scope_find(c->globals, name);
}

static Node *nodes_find(Nodes ns, SV name, Node *until) {
    for (Node *it = ns.head; it && it != until; it = it->next) {
        if (sv_eq(it->token.sv, name)) {
            return it;
        }
    }

    return NULL;
}

static_assert(COUNT_NODES == 10, "");
static void check_type(Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM:
        if (sv_match(n->token.sv, "bool")) {
            n->type = (Type) {.kind = TYPE_BOOL};
        } else if (sv_match(n->token.sv, "i64")) {
            n->type = (Type) {.kind = TYPE_I64};
        } else {
            error_undefined(n, "type");
        }
        break;

    case NODE_FN: {
        NodeFn *spec = (NodeFn *) n;
        for (Node *it = spec->args.head; it; it = it->next) {
            NodeVar *arg = (NodeVar *) it;
            check_type(arg->type);
            it->type = arg->type->type;
        }

        check_type(spec->ret);
        n->type = (Type) {.kind = TYPE_FN, .spec = n};
    } break;

    default:
        unreachable();
    }
}

static void check_fn(Context *c, Node *n);

static_assert(COUNT_NODES == 10, "");
static void check_expr(Context *c, Node *n, bool ref) {
    if (!n) {
        return;
    }

    bool allow_ref = false;
    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 21, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = (Type) {.kind = TYPE_I64};
            break;

        case TOKEN_BOOL:
            n->type = (Type) {.kind = TYPE_BOOL};
            break;

        case TOKEN_IDENT:
            atom->definition = ident_find(c, n->token.sv);
            if (!atom->definition) {
                error_undefined(n, "identifier");
            }

            allow_ref = atom->definition->kind == NODE_VAR;
            n->type = atom->definition->type;
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        check_expr(c, call->fn, false);

        const Type fn_type = call->fn->type;
        if (fn_type.kind != TYPE_FN) {
            fprintf(
                stderr, PosFmt "ERROR: Cannot call type '%s'\n", PosArg(call->fn->token.pos), type_to_cstr(fn_type));
            exit(1);
        }

        const NodeFn *expected = (const NodeFn *) fn_type.spec;
        if (call->arity != expected->arity) {
            fprintf(
                stderr,
                PosFmt "ERROR: Expected %zu argument%s, got %zu\n",
                PosArg(n->token.pos),
                expected->arity,
                expected->arity == 1 ? "" : "s",
                call->arity);

            exit(1);
        }

        for (Node *a = call->args.head, *e = expected->args.head; a; a = a->next, e = e->next) {
            check_expr(c, a, false);
            type_assert_node(a, e);
        }

        n->type = node_fn_return_type(expected);
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 21, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            check_expr(c, unary->operand, false);
            n->type = type_assert_arith(unary->operand);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 21, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
            check_expr(c, binary->lhs, false);
            check_expr(c, binary->rhs, false);
            type_assert_arith(binary->lhs);
            n->type = type_assert_node(binary->rhs, binary->lhs);
            break;

        case TOKEN_SET:
            check_expr(c, binary->lhs, true);
            check_expr(c, binary->rhs, false);
            type_assert_node(binary->rhs, binary->lhs);
            n->type = (Type) {.kind = TYPE_UNIT};
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_FN:
        check_fn(c, n);
        break;

    default:
        unreachable();
    }

    if (!allow_ref && ref) {
        fprintf(stderr, PosFmt "ERROR: Cannot take reference to value not in memory\n", PosArg(n->token.pos));
        exit(1);
    }
}

static void error_redefinition(const Node *n, const Node *previous, const char *label) {
    fprintf(stderr, PosFmt "ERROR: Redefinition of %s '" SVFmt "'\n", PosArg(n->token.pos), label, SVArg(n->token.sv));
    fprintf(stderr, PosFmt "NOTE: Defined here\n", PosArg(previous->token.pos));
    exit(1);
}

static_assert(COUNT_NODES == 10, "");
static void check_stmt(Context *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        check_expr(c, iff->condition, false);
        type_assert(iff->condition, (Type) {.kind = TYPE_BOOL});

        check_stmt(c, iff->consequence);
        check_stmt(c, iff->antecedence);
    } break;

    case NODE_BLOCK: {
        const size_t locals_count_save = c->locals.count;

        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            check_stmt(c, it);
        }

        c->locals.count = locals_count_save;
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;

        n->type = (Type) {.kind = TYPE_UNIT};
        if (ret->value) {
            check_expr(c, ret->value, false);
            n->type = ret->value->type;
        }

        type_assert(n, node_fn_return_type(c->fn.fn));
    } break;

    case NODE_FN:
        check_fn(c, n);
        break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (!var->local) {
            const Node *previous = scope_find(c->globals, n->token.sv);
            if (previous) {
                error_redefinition(n, previous, "identifier");
            }
        }

        if (var->type) {
            check_type(var->type);
            n->type = var->type->type;
        }

        if (var->expr) {
            check_expr(c, var->expr, false);
            n->type = var->expr->type;

            if (n->type.kind == TYPE_UNIT) {
                fprintf(
                    stderr,
                    PosFmt "ERROR: Cannot define variable with type '%s'\n",
                    PosArg(n->token.pos),
                    type_to_cstr(n->type));

                exit(1);
            }

            if (var->type) {
                type_assert(var->expr, var->type->type);
                n->type = var->expr->type;
            }
        }

        if (var->local) {
            da_push(&c->locals, n);
        } else {
            da_push(&c->globals, n);
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        check_expr(c, print->operand, false);
        type_assert_scalar(print->operand);
    } break;

    default:
        check_expr(c, n, false);
        break;
    }
}

static void check_fn(Context *c, Node *n) {
    NodeFn *fn = (NodeFn *) n;
    if (fn->local) {
        da_push(&c->locals, n);
    } else {
        const Node *previous = scope_find(c->globals, n->token.sv);
        if (previous) {
            error_redefinition(n, previous, "identifier");
        }
        da_push(&c->globals, n);
    }
    n->type = (Type) {.kind = TYPE_FN, .spec = n};

    const ContextFn context_fn_save = context_fn_begin(c, fn);
    for (Node *it = fn->args.head; it; it = it->next) {
        if (it->token.kind == TOKEN_IDENT) {
            const Node *previous = nodes_find(fn->args, it->token.sv, it);
            if (previous) {
                error_redefinition(it, previous, "argument");
            }
        }

        check_stmt(c, it);
    }

    check_type(fn->ret);
    check_stmt(c, fn->body);
    context_fn_end(c, context_fn_save);
}

void check_nodes(Context *c, Nodes ns) {
    for (Node *it = ns.head; it; it = it->next) {
        check_stmt(c, it);
    }
}
