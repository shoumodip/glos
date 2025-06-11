#include <ctype.h>

#include "compiler.h"

typedef struct {
    bool   local;
    size_t locals;
    size_t globals;

    SB     sb;
    size_t indent;

    Scope delayed_fns;
} Compiler;

static inline CompileData compile_data_new(Compiler *c) {
    CompileData data = {.local = c->local};
    if (c->local) {
        data.iota = ++c->locals;
    } else {
        data.iota = ++c->globals;
    }
    return data;
}

#define compile_sb_sprintf(c, ...) sb_sprintf(&(c)->sb, __VA_ARGS__)

static inline void compile_sb_indent(Compiler *c) {
    for (size_t i = 0; i < c->indent; i++) {
        da_push(&c->sb, '\t');
    }
}

static inline void compile_sb_quoted(Compiler *c, SV sv) {
    compile_sb_sprintf(c, "\"");
    for (size_t i = 0; i < sv.count; i++) {
        const char it = sv.data[i];
        if (it == '"') {
            compile_sb_sprintf(c, "\\\"");
        } else if (isprint(it)) {
            compile_sb_sprintf(c, "%c", it);
        } else {
            compile_sb_sprintf(c, "\\x%x", it);
        }
    }
    compile_sb_sprintf(c, "\"");
}

static inline void sb_compile_data(Compiler *c, CompileData data) {
    if (data.local) {
        compile_sb_sprintf(c, "__glos_l%zu", data.iota);
    } else {
        compile_sb_sprintf(c, "__glos_g%zu", data.iota);
    }
}

static_assert(COUNT_NODES == 9, "");
static void compile_type(Compiler *c, Type *type) {
    if (!type) {
        return;
    }

    switch (type->kind) {
    case TYPE_BOOL:
        compile_sb_sprintf(c, "_Bool");
        break;

    case TYPE_I64:
        compile_sb_sprintf(c, "long");
        break;

    case TYPE_FN:
        assert(type->compile.iota);
        sb_compile_data(c, type->compile);
        break;

    default:
        unreachable();
    }
}

static void compile_fn(Compiler *c, Node *n);

static_assert(COUNT_NODES == 9, "");
static void compile_expr(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        NodeAtom *atom = (NodeAtom *) n;

        static_assert(COUNT_TOKENS == 20, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            compile_sb_sprintf(c, "%zuL", n->token.as.integer);
            break;

        case TOKEN_BOOL:
            compile_sb_sprintf(c, "%dL", n->token.as.boolean);
            break;

        case TOKEN_IDENT:
            sb_compile_data(c, atom->definition->compile);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        compile_expr(c, call->fn);
        compile_sb_sprintf(c, "(");
        for (Node *it = call->args.head; it; it = it->next) {
            // TODO: This inherits the undefined order of call arguments from C
            //       Use "SSA" to define the evaluation from left to right
            compile_expr(c, it);
            if (it->next) {
                compile_sb_sprintf(c, ", ");
            }
        }
        compile_sb_sprintf(c, ")");
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;

        static_assert(COUNT_TOKENS == 20, "");
        switch (n->token.kind) {
        case TOKEN_SUB: {
            compile_sb_sprintf(c, "-(");
            compile_expr(c, unary->operand);
            compile_sb_sprintf(c, ")");
        } break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;

        static_assert(COUNT_TOKENS == 20, "");
        switch (n->token.kind) {
        case TOKEN_ADD:
            compile_sb_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sb_sprintf(c, " + ");
            compile_expr(c, binary->rhs);
            compile_sb_sprintf(c, ")");
            break;

        case TOKEN_SUB:
            compile_sb_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sb_sprintf(c, " - ");
            compile_expr(c, binary->rhs);
            compile_sb_sprintf(c, ")");
            break;

        case TOKEN_MUL:
            compile_sb_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sb_sprintf(c, " * ");
            compile_expr(c, binary->rhs);
            compile_sb_sprintf(c, ")");
            break;

        case TOKEN_DIV:
            compile_sb_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sb_sprintf(c, " / ");
            compile_expr(c, binary->rhs);
            compile_sb_sprintf(c, ")");
            break;

        case TOKEN_SET:
            compile_sb_sprintf(c, "(");
            compile_expr(c, binary->lhs);
            compile_sb_sprintf(c, " = ");
            compile_expr(c, binary->rhs);
            compile_sb_sprintf(c, ")");
            break;

        default:
            unreachable();
        }
    } break;

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 9, "");
static void compile_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        compile_sb_sprintf(c, "if (");
        compile_expr(c, iff->condition);
        compile_sb_sprintf(c, ") ");
        compile_stmt(c, iff->consequence);

        if (iff->antecedence) {
            compile_sb_sprintf(c, " else ");
            compile_stmt(c, iff->antecedence);
        }
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        compile_sb_sprintf(c, "{\n");
        c->indent++;
        for (Node *it = block->body.head; it; it = it->next) {
            compile_sb_sprintf(c, "#line %zu\n", it->token.pos.row + 1);
            compile_sb_indent(c);
            compile_stmt(c, it);
            compile_sb_sprintf(c, "\n");
        }
        c->indent--;
        compile_sb_indent(c);
        compile_sb_sprintf(c, "}");
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        if (fn->local) {
            da_push(&c->delayed_fns, n);
        } else {
            compile_fn(c, n);
            for (size_t i = 0; i < c->delayed_fns.count; i++) {
                compile_fn(c, c->delayed_fns.data[i]);
            }
            c->delayed_fns.count = 0;
        }
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        if (var->local) {
            compile_type(c, &n->type);
            compile_sb_sprintf(c, " ");

            n->compile = compile_data_new(c);
            sb_compile_data(c, n->compile);

            if (var->expr) {
                compile_sb_sprintf(c, " = ");
                compile_expr(c, var->expr);
            }

            compile_sb_sprintf(c, ";");
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        compile_sb_sprintf(c, "printf(\"%%ld\\n\", (long) (");
        compile_expr(c, print->operand);
        compile_sb_sprintf(c, "));");
    } break;

    default:
        compile_expr(c, n);
        compile_sb_sprintf(c, ";");
        break;
    }
}

static void compile_fn(Compiler *c, Node *n) {
    assert(n->compile.iota);

    NodeFn *fn = (NodeFn *) n;
    compile_sb_sprintf(c, "\n#line %zu ", n->token.pos.row + 1);
    compile_sb_quoted(c, sv_from_cstr(n->token.pos.path));

    compile_sb_sprintf(c, "\nstatic void ");
    sb_compile_data(c, n->compile);

    const bool local_save = c->local;
    c->local = true;
    c->locals = 0;

    compile_sb_sprintf(c, "(");
    for (Node *it = fn->args.head; it; it = it->next) {
        compile_type(c, &it->type);
        compile_sb_sprintf(c, " ");

        it->compile = compile_data_new(c);
        sb_compile_data(c, it->compile);

        if (it->next) {
            compile_sb_sprintf(c, ", ");
        }
    }
    compile_sb_sprintf(c, ") ");

    compile_stmt(c, fn->body);
    c->local = local_save;

    compile_sb_sprintf(c, "\n");
}

static_assert(COUNT_NODES == 9, "");
static void pre_compile_type(Compiler *c, Type *type) {
    if (!type || type->compile.iota) {
        return;
    }

    switch (type->kind) {
    case TYPE_BOOL:
    case TYPE_I64:
        break;

    case TYPE_FN: {
        NodeFn *spec = (NodeFn *) type->spec;
        for (Node *it = spec->args.head; it; it = it->next) {
            pre_compile_type(c, &it->type);
        }

        type->compile = compile_data_new(c);

        compile_sb_sprintf(c, "typedef void (*");
        sb_compile_data(c, type->compile);

        compile_sb_sprintf(c, ")(");
        for (Node *it = spec->args.head; it; it = it->next) {
            compile_type(c, &it->type);
            if (it->next) {
                compile_sb_sprintf(c, ", ");
            }
        }
        compile_sb_sprintf(c, ");\n");
    } break;

    default:
        unreachable();
    }
}

// TODO: Perform hashing of type definitions
static_assert(COUNT_NODES == 9, "");
static void pre_compile_node(Compiler *c, Node *n) {
    if (!n || n->compile.iota) {
        return;
    }

    switch (n->kind) {
    case NODE_ATOM:
        break;

    case NODE_CALL: {
        NodeCall *call = (NodeCall *) n;
        pre_compile_node(c, call->fn);

        for (Node *it = call->args.head; it; it = it->next) {
            pre_compile_node(c, it);
        }
    } break;

    case NODE_UNARY: {
        NodeUnary *unary = (NodeUnary *) n;
        pre_compile_node(c, unary->operand);
    } break;

    case NODE_BINARY: {
        NodeBinary *binary = (NodeBinary *) n;
        pre_compile_node(c, binary->lhs);
        pre_compile_node(c, binary->rhs);
    } break;

    case NODE_IF: {
        NodeIf *iff = (NodeIf *) n;
        pre_compile_node(c, iff->condition);
        pre_compile_node(c, iff->consequence);
        pre_compile_node(c, iff->antecedence);
    } break;

    case NODE_BLOCK: {
        NodeBlock *block = (NodeBlock *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            pre_compile_node(c, it);
        }
    } break;

    case NODE_FN: {
        NodeFn *fn = (NodeFn *) n;
        for (Node *it = fn->args.head; it; it = it->next) {
            pre_compile_node(c, it);
        }

        n->compile = compile_data_new(c);
        compile_sb_sprintf(c, "static void ");
        sb_compile_data(c, n->compile);

        compile_sb_sprintf(c, "(");
        for (Node *it = fn->args.head; it; it = it->next) {
            compile_type(c, &it->type);
            if (it->next) {
                compile_sb_sprintf(c, ", ");
            }
        }
        compile_sb_sprintf(c, ");\n");

        pre_compile_node(c, fn->body);
    } break;

    case NODE_VAR: {
        NodeVar *var = (NodeVar *) n;
        pre_compile_node(c, var->expr);
        pre_compile_type(c, &n->type);

        if (!var->local) {
            compile_type(c, &n->type);
            compile_sb_sprintf(c, " ");

            n->compile = compile_data_new(c);
            sb_compile_data(c, n->compile);

            compile_sb_sprintf(c, ";\n");
        }
    } break;

    case NODE_PRINT: {
        NodePrint *print = (NodePrint *) n;
        pre_compile_node(c, print->operand);
    } break;

    default:
        unreachable();
        break;
    }
}

void compile_nodes(Context *context, Cmd *cmd, const char *output) {
    Node *main = scope_find(context->globals, sv_from_cstr("main"));
    if (!main) {
        fprintf(stderr, "ERROR: Function 'main' is not defined\n");
        exit(1);
    }

    if (main->kind != NODE_FN) {
        fprintf(stderr, PosFmt "ERROR: Function 'main' must be a function literal\n", PosArg(main->token.pos));
        exit(1);
    }

    Compiler c = {0};
    compile_sb_sprintf(&c, "extern int printf(const char *fmt, ...);\n");

    for (size_t i = 0; i < context->globals.count; i++) {
        pre_compile_node(&c, context->globals.data[i]);
    }

    for (size_t i = 0; i < context->globals.count; i++) {
        compile_stmt(&c, context->globals.data[i]);
    }

    compile_sb_sprintf(&c, "\n#line 1 \"%s.c\"", output);
    compile_sb_sprintf(&c, "\nint main(void) {\n");
    c.indent++;

    for (size_t i = 0; i < context->globals.count; i++) {
        Node *it = context->globals.data[i];
        if (it->kind == NODE_VAR) {
            NodeVar *var = (NodeVar *) it;
            if (var->expr) {
                compile_sb_indent(&c);
                sb_compile_data(&c, it->compile);
                compile_sb_sprintf(&c, " = ");
                compile_expr(&c, var->expr);
                compile_sb_sprintf(&c, ";\n");
            }
        }
    }

    compile_sb_indent(&c);
    sb_compile_data(&c, main->compile);
    compile_sb_sprintf(&c, "();\n");
    compile_sb_indent(&c);
    compile_sb_sprintf(&c, "return 0;\n");
    c.indent--;
    compile_sb_sprintf(&c, "}\n");

#if 0
    fwrite(c.sb.data, c.sb.count, 1, stdout);
    exit(0);
#endif

    da_push(cmd, "cc");
    da_push(cmd, "-g");
    da_push(cmd, "-o");
    da_push(cmd, output);
    da_push(cmd, "-x");
    da_push(cmd, "c");
    da_push(cmd, "-");

    FILE *in = NULL;
    Proc  proc = cmd_run_async(cmd, (CmdStdio) {.in = &in});

    if (in) {
        fwrite(c.sb.data, c.sb.count, 1, in);
        fclose(in);
    }

    const int code = cmd_wait(proc);
    if (code) {
        exit(code);
    }
}
