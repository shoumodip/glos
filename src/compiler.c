#include "compiler.h"
#include "qbe.h"

typedef struct {
    Qbe   *qbe;
    QbeFn *fn;
} Compiler;

static_assert(COUNT_NODES == 10, "");
static void compile_type(Type *type) {
    if (!type) {
        return;
    }

    switch (type->kind) {
    case TYPE_UNIT:
        type->qbe = qbe_type_basic(QBE_TYPE_I0);
        break;

    case TYPE_BOOL:
        type->qbe = qbe_type_basic(QBE_TYPE_I8);
        break;

    case TYPE_I64:
        type->qbe = qbe_type_basic(QBE_TYPE_I64);
        break;

    case TYPE_FN:
        type->qbe = qbe_type_basic(QBE_TYPE_I64);
        break;

    default:
        unreachable();
    }
}

static void compile_stmt(Compiler *c, Node *n);

static_assert(COUNT_NODES == 10, "");
static QbeNode *compile_expr(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return NULL;
    }

    compile_type(&n->type);
    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 21, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            return qbe_atom_int(c->qbe, QBE_TYPE_I64, n->token.as.integer);

        case TOKEN_BOOL:
            return qbe_atom_int(c->qbe, QBE_TYPE_I64, n->token.as.boolean);

        case TOKEN_IDENT:
            switch (atom->definition->kind) {
            case NODE_FN: {
                NodeFn *fn = (NodeFn *) atom->definition;
                if (!fn->qbe) {
                    compile_stmt(c, atom->definition);
                }
                return fn->qbe;
            };

            case NODE_VAR: {
                NodeVar *var = (NodeVar *) atom->definition;
                if (!var->qbe) {
                    compile_stmt(c, atom->definition);
                }

                if (ref || var->kind == NODE_VAR_ARG) {
                    return var->qbe;
                }

                return qbe_build_load(c->qbe, c->fn, var->qbe, n->type.qbe);
            } break;

            default:
                unreachable();
            }
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        QbeNode  *fn = compile_expr(c, call->fn, false);

        QbeCall *fn_call = qbe_build_call(c->qbe, c->fn, fn, n->type.qbe);
        for (Node *it = call->args.head; it; it = it->next) {
            qbe_call_add_arg(c->qbe, fn_call, compile_expr(c, it, false));
        }

        return (QbeNode *) fn_call;
    };

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 21, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            QbeNode *operand = compile_expr(c, unary->operand, false);
            return qbe_build_unary(c->qbe, c->fn, QBE_UNARY_NEG, n->type.qbe, operand);
        }

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 21, "");
        switch (n->token.kind) {
        case TOKEN_ADD: {
            QbeNode *lhs = compile_expr(c, binary->lhs, false);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            return qbe_build_binary(c->qbe, c->fn, QBE_BINARY_ADD, n->type.qbe, lhs, rhs);
        }

        case TOKEN_SUB: {
            QbeNode *lhs = compile_expr(c, binary->lhs, false);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            return qbe_build_binary(c->qbe, c->fn, QBE_BINARY_SUB, n->type.qbe, lhs, rhs);
        }

        case TOKEN_MUL: {
            QbeNode *lhs = compile_expr(c, binary->lhs, false);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            return qbe_build_binary(c->qbe, c->fn, QBE_BINARY_MUL, n->type.qbe, lhs, rhs);
        }

        case TOKEN_DIV: {
            QbeNode *lhs = compile_expr(c, binary->lhs, false);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            return qbe_build_binary(c->qbe, c->fn, QBE_BINARY_SDIV, n->type.qbe, lhs, rhs);
        }

        case TOKEN_SET: {
            QbeNode *lhs = compile_expr(c, binary->lhs, true);
            QbeNode *rhs = compile_expr(c, binary->rhs, false);
            qbe_build_store(c->qbe, c->fn, lhs, rhs);
            return NULL;
        }

        default:
            unreachable();
        }
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        compile_stmt(c, n);
        return fn->qbe;
    }

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 10, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;

        QbeBlock *consequence = qbe_block_new(c->qbe);
        QbeBlock *antecedence = qbe_block_new(c->qbe);

        QbeBlock *end = antecedence;
        if (iff->antecedence) {
            end = qbe_block_new(c->qbe);
        }

        // Condition
        QbeNode *condition = compile_expr(c, iff->condition, false);
        qbe_build_branch(c->qbe, c->fn, condition, consequence, antecedence);

        // Consequence
        qbe_build_block(c->qbe, c->fn, consequence);
        compile_stmt(c, iff->consequence);
        qbe_build_jump(c->qbe, c->fn, end);

        // Antecedence
        if (iff->antecedence) {
            qbe_build_block(c->qbe, c->fn, antecedence);
            compile_stmt(c, iff->antecedence);
            qbe_build_jump(c->qbe, c->fn, end);
        }

        // End
        qbe_build_block(c->qbe, c->fn, end);
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            qbe_build_debug_line(c->qbe, c->fn, it->token.pos.row + 1);
            compile_stmt(c, it);
        }
        qbe_build_debug_line(c->qbe, c->fn, n->token.pos.row + 1);
    } break;

    case NODE_RETURN: {
        NodeReturn *ret = (NodeReturn *) n;
        qbe_build_return(c->qbe, c->fn, compile_expr(c, ret->value, false));
        qbe_build_block(c->qbe, c->fn, qbe_block_new(c->qbe));
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;

        Type return_type = node_fn_return_type(fn);
        compile_type(&return_type);

        QbeFn *fn_save = c->fn;
        c->fn = qbe_fn_new(c->qbe, (QbeSV) {0}, return_type.qbe);
        fn->qbe = (QbeNode *) c->fn;

        for (Node *it = fn->args.head; it; it = it->next) {
            NodeVar *arg = (NodeVar *) it;
            compile_type(&it->type);
            arg->qbe = qbe_fn_add_arg(c->qbe, c->fn, it->type.qbe);
            if (arg->kind == NODE_VAR_LOCAL) {
                QbeNode *var = qbe_fn_add_var(c->qbe, c->fn, it->type.qbe);
                qbe_build_store(c->qbe, c->fn, var, arg->qbe);
                arg->qbe = var;
            }
        }

        assert(fn->body->kind == NODE_BLOCK);
        NodeBlock *fn_block = (NodeBlock *) fn->body;

        size_t fn_row = 0;
        if (fn_block->body.head) {
            fn_row = fn_block->body.head->token.pos.row;

            compile_stmt(c, fn_block->body.head);
            for (Node *it = fn_block->body.head->next; it; it = it->next) {
                qbe_build_debug_line(c->qbe, c->fn, it->token.pos.row + 1);
                compile_stmt(c, it);
            }
        } else {
            fn_row = fn_block->node.token.pos.row;
        }

        qbe_build_debug_line(c->qbe, c->fn, fn_block->node.token.pos.row + 1);
        qbe_fn_set_debug(c->qbe, c->fn, qbe_sv_from_cstr(n->token.pos.path), fn_row + 1);
        qbe_build_return(c->qbe, c->fn, NULL);

        c->fn = fn_save;
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;

        compile_type(&n->type);
        if (var->kind == NODE_VAR_GLOBAL) {
            var->qbe = qbe_var_new(c->qbe, (QbeSV) {0}, n->type.qbe);
        } else {
            var->qbe = qbe_fn_add_var(c->qbe, c->fn, n->type.qbe);
            if (var->expr) {
                qbe_build_store(c->qbe, c->fn, var->qbe, compile_expr(c, var->expr, false));
            } else {
                // TODO: Move "zero"-ing logic into LibQBE
                QbeNode *memset = qbe_atom_symbol(c->qbe, qbe_sv_from_cstr("memset"), qbe_type_basic(QBE_TYPE_I64));
                QbeCall *call = qbe_build_call(c->qbe, c->fn, memset, qbe_type_basic(QBE_TYPE_I64));
                qbe_call_add_arg(c->qbe, call, var->qbe);
                qbe_call_add_arg(c->qbe, call, qbe_atom_int(c->qbe, QBE_TYPE_I32, 0));
                qbe_call_add_arg(c->qbe, call, qbe_atom_int(c->qbe, QBE_TYPE_I64, qbe_sizeof(n->type.qbe)));
            }
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;

        static QbeNode *fn;
        if (!fn) {
            fn = qbe_atom_symbol(c->qbe, qbe_sv_from_cstr("printf"), qbe_type_basic(QBE_TYPE_I64));
        }

        static QbeNode *fmt;
        if (!fmt) {
            fmt = qbe_str_new(c->qbe, qbe_sv_from_cstr("%ld\n"));
        }

        QbeCall *call = qbe_build_call(c->qbe, c->fn, fn, qbe_type_basic(QBE_TYPE_I32));
        qbe_call_add_arg(c->qbe, call, fmt);
        qbe_call_start_variadic(c->qbe, call);

        QbeNode *operand = compile_expr(c, print->operand, false);
        qbe_call_add_arg(c->qbe, call, qbe_build_cast(c->qbe, c->fn, operand, QBE_TYPE_I64, true));
    } break;

    default:
        compile_expr(c, n, false);
        break;
    }
}

static NodeFn *get_main(Context *c) {
    Node *main = scope_find(c->globals, sv_from_cstr("main"));
    if (!main) {
        fprintf(stderr, "ERROR: Function 'main' is not defined\n");
        exit(1);
    }

    if (main->kind != NODE_FN) {
        fprintf(stderr, PosFmt "ERROR: Function 'main' must be a function literal\n", PosArg(main->token.pos));
        exit(1);
    }

    NodeFn *main_fn = (NodeFn *) main;
    if (main_fn->arity) {
        fprintf(stderr, PosFmt "ERROR: Function 'main' cannot take any arguments\n", PosArg(main->token.pos));
        exit(1);
    }

    if (main_fn->ret) {
        fprintf(stderr, PosFmt "ERROR: Function 'main' cannot return anything\n", PosArg(main->token.pos));
        exit(1);
    }
    return main_fn;
}

void compile_nodes(Context *context, const char *output) {
    NodeFn *main = get_main(context);

    Compiler c = {0};
    c.qbe = qbe_new();

    for (size_t i = 0; i < context->globals.count; i++) {
        compile_stmt(&c, context->globals.data[i]);
    }

    // Entry
    c.fn = qbe_fn_new(c.qbe, qbe_sv_from_cstr("main"), qbe_type_basic(QBE_TYPE_I32));
    qbe_fn_set_debug(c.qbe, c.fn, qbe_sv_from_cstr("glos_start_call_main.h"), 1);

    for (size_t i = 0; i < context->globals.count; i++) {
        Node *it = context->globals.data[i];
        if (it->kind == NODE_VAR) {
            NodeVar *var = (NodeVar *) it;
            if (var->expr) {
                qbe_build_store(c.qbe, c.fn, var->qbe, compile_expr(&c, var->expr, false));
            }
        }
    }

    qbe_build_call(c.qbe, c.fn, main->qbe, qbe_type_basic(QBE_TYPE_I0));
    qbe_build_return(c.qbe, c.fn, qbe_atom_int(c.qbe, QBE_TYPE_I32, 0));

#if 0
    qbe_compile(c.qbe);
    QbeSV program = qbe_get_compiled_program(c.qbe);
    fwrite(program.data, program.count, 1, stdout);
    exit(0);
#endif

    const int code = qbe_generate(c.qbe, QBE_TARGET_DEFAULT, output, NULL, 0);
    if (code) {
        exit(code);
    }
}
