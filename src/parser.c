#include "parser.h"
#include "basic.h"
#include "node.h"
#include "token.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#ifndef PLATFORM_X86_64_WINDOWS
#include <dirent.h>
#include <errno.h>
#endif //  PLATFORM_X86_64_WINDOWS

void error_number_of_values_mismatch(
    Pos pos, size_t lhs_count, size_t rhs_count, const char *lhs_label, const char *rhs_label) {
    if (!lhs_label) {
        lhs_label = "on the left hand side";
    }

    if (!rhs_label) {
        rhs_label = "on the right hand side";
    }
    fprintf(
        stderr,
        Pos_Fmt "ERROR: Unequal number of values. There %s %zu %s, and %zu %s\n",
        Pos_Arg(pos),
        lhs_count == 1 ? "is" : "are",
        lhs_count,
        lhs_label,
        rhs_count,
        rhs_label);
    exit(1);
}

typedef enum {
    POWER_NIL,
    POWER_SET,
    POWER_TUP,
    POWER_LOR,
    POWER_CMP,
    POWER_SHL,
    POWER_ADD,
    POWER_BOR,
    POWER_MUL,
    POWER_PRE,
    POWER_CALL,
    POWER_REF,
    POWER_DOT,
} Power;

static_assert(COUNT_TOKENS == 77, "");
static Power token_kind_to_power(Token_Kind kind) {
    switch (kind) {
    case TOKEN_DOT:
    case TOKEN_LBRACE:
    case TOKEN_LBRACKET:
        return POWER_DOT;

    case TOKEN_COLON:
        return POWER_SET;

    case TOKEN_COMMA:
        return POWER_TUP;

    case TOKEN_LPAREN:
        return POWER_CALL;

    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
    case TOKEN_MOD:
        return POWER_MUL;

    case TOKEN_SHL:
    case TOKEN_SHR:
        return POWER_SHL;

    case TOKEN_BOR:
    case TOKEN_BAND:
        return POWER_BOR;

    case TOKEN_LOR:
    case TOKEN_LAND:
        return POWER_LOR;

    case TOKEN_SET:
    case TOKEN_ADD_SET:
    case TOKEN_SUB_SET:
    case TOKEN_MUL_SET:
    case TOKEN_DIV_SET:
    case TOKEN_MOD_SET:
    case TOKEN_SHL_SET:
    case TOKEN_SHR_SET:
    case TOKEN_BOR_SET:
    case TOKEN_BAND_SET:
        return POWER_SET;

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_EQ:
    case TOKEN_NE:
        return POWER_CMP;

    default:
        return POWER_NIL;
    }
}

Module *module_get(Parser *p, const char *path) {
    assert(p->modules);

    if (!p->modules->table.hasheq) {
        p->modules->table.hasheq = ht_hasheq_cstr;
    }

    Module **modulep = ht_get(&p->modules->table, path);
    if (modulep) {
        return *modulep;
    }

    Module *module = arena_alloc(&default_arena, sizeof(*module));
    module->absolute_path = path;
    module->relative_path = get_relative_path(p->cwd, sv_from_cstr(path), &default_arena);

    ht_set(&p->modules->table, path, module);
    if (p->modules->tail) {
        p->modules->tail->next = module;
    } else {
        p->modules->head = module;
    }
    p->modules->tail = module;
    return module;
}

static void error_unexpected(Token token) {
    fprintf(stderr, Pos_Fmt "ERROR: Unexpected %s\n", Pos_Arg(token.pos), token_kind_to_cstr(token.kind));
    exit(1);
}

static void buffer_token(Parser *p, Token token) {
    p->state.peeked = true;
    p->state.ahead = token;
}

static Token next_token(Parser *p) {
    if (p->state.peeked) {
        p->state.peeked = false;
        return p->state.ahead;
    }

    return lexer_iter(&p->state.lexer);
}

static Token peek_token(Parser *p) {
    if (p->state.peeked) {
        return p->state.ahead;
    }

    buffer_token(p, lexer_iter(&p->state.lexer));
    return p->state.ahead;
}

static void split_logical_and(Parser *p, Token *token) {
    assert(token->kind == TOKEN_LAND);
    token->kind = TOKEN_BAND;
    token->sv.count--;

    assert(!p->state.peeked);
    Token next = *token;
    next.sv.data++;
    next.pos.col++;
    buffer_token(p, next);
}

static bool read_token(Parser *p, Token_Kind kind) {
    peek_token(p);
    p->state.peeked = p->state.ahead.kind != kind;
    return !p->state.peeked;
}

static Token expect_token(Parser *p, const Token_Kind *kinds) {
    const Token token = next_token(p);
    for (const Token_Kind *it = kinds; *it != TOKEN_EOF; it++) {
        if (token.kind == *it) {
            return token;
        }
    }

    fprintf(stderr, Pos_Fmt "ERROR: Expected ", Pos_Arg(token.pos));
    for (const Token_Kind *it = kinds; *it != TOKEN_EOF; it++) {
        if (it != kinds) {
            fprintf(stderr, " or ");
        }

        fprintf(stderr, "%s", token_kind_to_cstr(*it));
    }

    fprintf(stderr, ", got %s\n", token_kind_to_cstr(token.kind));
    exit(1);
}
#define expect_token(p, ...) expect_token((p), (const Token_Kind[]) {__VA_ARGS__, TOKEN_EOF})

static void consume_tokens(Parser *p, Token_Kind kind) {
    while (read_token(p, kind));
}

static bool read_eol_or_rbrace(Parser *p) {
    const Token ahead = peek_token(p);
    if (ahead.newline || ahead.kind == TOKEN_EOL || ahead.kind == TOKEN_EOF || ahead.kind == TOKEN_RBRACE) {
        consume_tokens(p, TOKEN_EOL);
        return true;
    }
    return false;
}

static void expect_stmt_terminator(Parser *p) {
    if (read_eol_or_rbrace(p)) {
        return;
    }

    const Token ahead = peek_token(p);
    fprintf(
        stderr, Pos_Fmt "ERROR: Expected newline or ';', got %s\n", Pos_Arg(ahead.pos), token_kind_to_cstr(ahead.kind));
    exit(1);
}

static Node *parse_expr(Parser *p, Power mbp, bool groups_allowed, bool compounds_allowed, bool *should_be_switch);
static Node *parse_stmt(Parser *p);

static Node *parse_block(Parser *p, Token token) {
    Node_Block *block = (Node_Block *) node_alloc(p->module_current, NODE_BLOCK, token);
    while (!read_token(p, TOKEN_RBRACE)) {
        nodes_push(&block->body, parse_stmt(p));
        expect_stmt_terminator(p);
    }

    assert(p->state.ahead.kind == TOKEN_RBRACE);
    block->end = p->state.ahead.pos;
    return (Node *) block;
}

static Node *parse_if(Parser *p, Token token, bool is_compile_time) {
    Node *node = NULL;

    const bool in_compile_time_condition_save = p->state.in_compile_time_condition;
    if (is_compile_time) {
        p->state.in_compile_time_condition = true;
    }

    bool  should_be_switch = false;
    Node *expr = parse_expr(p, POWER_SET, false, false, &should_be_switch);
    if (should_be_switch) {
        Node_Switch *sw = (Node_Switch *) node_alloc(p->module_current, NODE_SWITCH, token);
        sw->is_compile_time = is_compile_time;
        sw->expr = expr;

        expect_token(p, TOKEN_LBRACE);
        while (!read_token(p, TOKEN_RBRACE)) {
            token = expect_token(p, TOKEN_CASE);

            bool  fallback = false;
            Token ahead = peek_token(p);
            if (ahead.kind == TOKEN_EOL || ahead.kind == TOKEN_RBRACE || ahead.newline) {
                if (sw->fallback) {
                    fprintf(stderr, Pos_Fmt "ERROR: Multiple fallback cases is not allowed\n", Pos_Arg(token.pos));
                    fprintf(stderr, Pos_Fmt "NOTE: Already here\n", Pos_Arg(sw->fallback->token.pos));
                    exit(1);
                }

                fallback = true;
            }

            Node_Case *case_ = (Node_Case *) node_alloc(p->module_current, NODE_CASE, token);
            if (!fallback) {
                do {
                    nodes_push(&case_->preds, parse_expr(p, POWER_SET, false, false, NULL));
                    case_->preds_count++;
                    sw->preds_count++;
                } while (read_token(p, TOKEN_COMMA));
            }
            expect_stmt_terminator(p);

            case_->body = node_alloc(p->module_current, NODE_BLOCK, token);
            Node_Block *block = (Node_Block *) case_->body;
            while (true) {
                ahead = peek_token(p);
                if (ahead.kind == TOKEN_CASE || ahead.kind == TOKEN_RBRACE) {
                    break;
                }
                nodes_push(&block->body, parse_stmt(p));
            }

            nodes_push(&sw->cases, (Node *) case_);
            if (fallback) {
                sw->fallback = (Node *) case_;
            }
        }

        node = (Node *) sw;
    } else {
        Node_If *iff = (Node_If *) node_alloc(p->module_current, NODE_IF, token);
        iff->condition = expr;
        iff->is_compile_time = is_compile_time;

        token = expect_token(p, TOKEN_LBRACE);
        iff->consequence = parse_block(p, token);

        if (read_token(p, TOKEN_ELSE)) {
            if (is_compile_time) {
                token = expect_token(p, TOKEN_LBRACE, TOKEN_IF, TOKEN_DIRECTIVE_IF);
            } else {
                token = expect_token(p, TOKEN_LBRACE, TOKEN_IF);
            }

            if (token.kind == TOKEN_LBRACE) {
                iff->antecedence = parse_block(p, token);
            } else {
                iff->antecedence = parse_if(p, token, is_compile_time);
            }
        }

        node = (Node *) iff;
    }

    p->state.in_compile_time_condition = in_compile_time_condition_save;
    return node;
}

static Node *parse_for(Parser *p, Token token) {
    Node_For *forr = (Node_For *) node_alloc(p->module_current, NODE_FOR, token);

    if (peek_token(p).kind != TOKEN_LBRACE) {
        forr->condition = parse_expr(p, POWER_NIL, false, false, NULL);

        bool was_init = false;
        if (forr->condition->kind == NODE_DEFINE ||
            (forr->condition->kind == NODE_BINARY && token_kind_to_power(forr->condition->token.kind) == POWER_SET)) {
            expect_stmt_terminator(p);
            was_init = true;
        }

        if (was_init || read_eol_or_rbrace(p)) {
            forr->init = forr->condition;
            forr->condition = parse_expr(p, POWER_SET, false, false, NULL);

            if (read_eol_or_rbrace(p)) {
                forr->update = parse_expr(p, POWER_NIL, false, false, NULL);
            }
        }
    }

    const bool inside_loop_save = p->state.in_loop;
    p->state.in_loop = true;
    {
        token = expect_token(p, TOKEN_LBRACE);
        forr->body = parse_block(p, token);
    }
    p->state.in_loop = inside_loop_save;
    return (Node *) forr;
}

static void not_in_extern_assert(Parser *p, Token token) {
    if (p->state.in_extern) {
        fprintf(
            stderr,
            Pos_Fmt "ERROR: Extern blocks can only have variable and function definitions\n",
            Pos_Arg(token.pos));
        exit(1);
    }
}

static void definition_lhs_atom_setup(
    Parser      *p,
    Node_Define *define,
    Node_Atom   *it,
    Node        *it_expr,
    bool         is_static,
    bool         is_assigned,
    size_t       group_index) //
{
    if (!it->definition_spec) {
        it->definition_spec = arena_alloc(&default_arena, sizeof(*it->definition_spec));
    }

    it->definition_spec->group_index = group_index;
    it->definition_spec->is_const = define->is_const;
    it->definition_spec->is_local = p->state.fn_current != NULL;
    it->definition_spec->is_extern = p->state.in_extern;
    it->definition_spec->is_private = p->state.after_private;
    it->definition_spec->is_assigned = is_assigned;
    it->definition_spec->definition_node = define;
    it->definition_spec->assignment_node = it_expr;

    if (is_static) {
        it->definition_spec->static_var_fn = p->state.fn_current;
    }

    if (it->definition_spec->is_const) {
        assert(it_expr);
        if (it_expr->kind == NODE_FN) {
            ((Node_Fn *) it_expr)->defined_as = it;
        } else if (it_expr->kind == NODE_ENUM) {
            ((Node_Enum *) it_expr)->defined_as = it;
        } else if (it_expr->kind == NODE_TRAIT) {
            ((Node_Trait *) it_expr)->defined_as = it;
        } else if (it_expr->kind == NODE_UNION) {
            ((Node_Union *) it_expr)->defined_as = it;
        } else if (it_expr->kind == NODE_STRUCT) {
            ((Node_Struct *) it_expr)->defined_as = it;
        }
    }
}

static void definition_lhs_setup(Parser *p, Node_Define *define, bool is_static) {
    const bool is_assigned = define->expr != NULL;
    define->is_value_known_at_compile_time = define->is_const || !p->state.fn_current || is_static;

    size_t lhs_count = 1;
    size_t rhs_count = 1;

    if (define->name->kind == NODE_ATOM) {
        if (define->expr && define->expr->kind == NODE_GROUP) {
            rhs_count = ((Node_Group *) define->expr)->count;
            error_number_of_values_mismatch(
                define->node.token.pos,
                lhs_count,
                rhs_count,
                add_trailing_s_if_plural("definition", lhs_count),
                add_trailing_s_if_plural("assignment", rhs_count));
            exit(1);
        }

        definition_lhs_atom_setup(p, define, (Node_Atom *) define->name, define->expr, is_static, is_assigned, 0);
    } else {
        Node_Group *lhs = (Node_Group *) define->name;
        lhs_count = lhs->count;

        if (is_assigned && define->is_value_known_at_compile_time) {
            if (define->expr->kind != NODE_GROUP) {
                error_number_of_values_mismatch(
                    define->node.token.pos,
                    lhs_count,
                    rhs_count,

                    add_trailing_s_if_plural("definition", lhs_count),
                    add_trailing_s_if_plural("assignment", rhs_count));
                exit(1);
            }

            Node_Group *rhs = (Node_Group *) define->expr;
            rhs_count = rhs->count;
            if (lhs_count != rhs_count) {
                error_number_of_values_mismatch(
                    define->node.token.pos,
                    lhs_count,
                    rhs_count,

                    add_trailing_s_if_plural("definition", lhs_count),
                    add_trailing_s_if_plural("assignment", rhs_count));
                exit(1);
            }

            size_t iota = 0;
            ll_foreach2(lhs_iota, rhs_iota, &lhs->nodes, &rhs->nodes) {
                assert(lhs_iota->kind == NODE_ATOM);
                definition_lhs_atom_setup(p, define, (Node_Atom *) lhs_iota, rhs_iota, is_static, is_assigned, iota++);
            }
        } else {
            size_t iota = 0;
            ll_foreach(it, &lhs->nodes) {
                definition_lhs_atom_setup(p, define, (Node_Atom *) it, NULL, is_static, is_assigned, iota++);
            }
        }
    }
}

bool parser_import(Parser *p, Node_Import *import) {
    if (import->module) {
        return false;
    }

    SV          root = {0};
    const char *absolute_path = NULL;
    if (!absolute_path) {
        // Directory inside the current module
        root = sv_from_cstr(p->module_current->absolute_path);
        absolute_path = get_absolute_path(root, import->path.sv, &default_arena);
        if (!directory_exists(absolute_path)) {
            arena_reset(&default_arena, absolute_path);
            root = (SV) {0};
            absolute_path = NULL;
        }
    }

    if (!absolute_path) {
        // Directory inside root
        root = p->root;
        absolute_path = get_absolute_path(root, import->path.sv, &default_arena);
        if (!directory_exists(absolute_path)) {
            arena_reset(&default_arena, absolute_path);
            root = (SV) {0};
            absolute_path = NULL;
        }
    }

    if (!absolute_path) {
        // Directory inside std
        root = p->std;
        absolute_path = get_absolute_path(root, import->path.sv, &default_arena);
        if (!directory_exists(absolute_path)) {
            arena_reset(&default_arena, absolute_path);
            root = (SV) {0};
            absolute_path = NULL;
        }
    }

    if (!absolute_path) {
        fprintf(
            stderr,
            Pos_Fmt "ERROR: Could not find module '" SV_Fmt "'\n",
            Pos_Arg(import->path.pos),
            SV_Arg(import->path.sv));
        exit(1);
    }

    bool    newly_imported = true;
    Module *module_current_save = p->module_current;
    {
        Module *module = module_get(p, absolute_path);

        if (module->name.count) {
            newly_imported = false;
        } else {
            module->name = sv_from_cstr(get_relative_path(root, sv_from_cstr(module->absolute_path), &default_arena));
            if (sv_match(module->name, "main") || sv_match(module->name, "builtin")) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: The module path '" SV_Fmt "' is reserved\n",
                    Pos_Arg(import->path.pos),
                    SV_Arg(module->name));
                exit(1);
            }

            p->module_current = module;

            switch (parse_directory(p, module->relative_path)) {
            case PARSE_OK:
                // Pass
                break;

            case PARSE_FAILURE:
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Could not read directory '%s'\n",
                    Pos_Arg(import->path.pos),
                    module->relative_path);
                exit(1);
                break;

            case PARSE_EMPTY_DIRECTORY:
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Directory '%s' does not contain any glos files\n",
                    Pos_Arg(import->path.pos),
                    module->relative_path);
                exit(1);
                break;

            default:
                unreachable();
            }
        }

        import->module = module;
    }
    p->module_current = module_current_save;
    return newly_imported;
}

static Node *
parse_define(Parser *p, Node *name, Token token, bool groups_allowed, bool spread_allowed, bool is_static) {
    Node_Define *define = (Node_Define *) node_alloc(p->module_current, NODE_DEFINE, token);
    if (spread_allowed && peek_token(p).kind == TOKEN_SPREAD) {
        define->has_spread = true;
        define->spread_pos = next_token(p).pos;
    }

    {
        Node *illegal = NULL;
        if (name->kind == NODE_ATOM && name->token.kind == TOKEN_IDENT) {
            define->count = 1;
        } else if (name->kind == NODE_GROUP) {
            Node_Group *group = (Node_Group *) name;
            ll_foreach(it, &group->nodes) {
                if (it->kind != NODE_ATOM || it->token.kind != TOKEN_IDENT) {
                    illegal = it;
                    break;
                }
            }
            define->count = group->count;
        } else {
            illegal = name;
        }

        if (illegal) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Expected definition name to be an identifier, got expression\n",
                Pos_Arg(illegal->token.pos));
            exit(1);
        }
    }
    define->name = name;

    token = peek_token(p);
    if (token.kind != TOKEN_SET && token.kind != TOKEN_COLON) {
        define->type = parse_expr(p, POWER_PRE, false, true, NULL);
    }

    token = peek_token(p);
    if (token.kind == TOKEN_SET) {
        if (define->has_spread) {
            fprintf(stderr, Pos_Fmt "ERROR: Cannot have default value here\n", Pos_Arg(token.pos));
            exit(1);
        }

        p->state.peeked = false;
        if (p->state.in_extern) {
            assert(p->state.ahead.kind == TOKEN_SET);
            fprintf(stderr, Pos_Fmt "ERROR: External variable cannot have assignment\n", Pos_Arg(p->state.ahead.pos));
            exit(1);
        }

        define->expr = parse_expr(p, POWER_SET, groups_allowed, true, NULL);
    } else if (token.kind == TOKEN_COLON) {
        if (define->has_spread) {
            fprintf(stderr, Pos_Fmt "ERROR: Cannot have default value here\n", Pos_Arg(token.pos));
            exit(1);
        }

        p->state.peeked = false;
        define->expr = parse_expr(p, POWER_SET, groups_allowed, true, NULL);
        define->is_const = true;

        if (p->state.in_extern) {
            if (define->expr->kind != NODE_FN) {
                not_in_extern_assert(p, define->expr->token);
            }

            Node_Fn *fn = (Node_Fn *) define->expr;
            if (fn->body) {
                fprintf(stderr, Pos_Fmt "ERROR: External function cannot have body\n", Pos_Arg(fn->body->token.pos));
                exit(1);
            }

            fn->is_type = false;
            fn->is_extern = true;
        }
    }

    definition_lhs_setup(p, define, is_static);
    return (Node *) define;
}

static Node *parse_compound(Parser *p, Node *lhs, Token token) {
    Node_Compound *compound = (Node_Compound *) node_alloc(p->module_current, NODE_COMPOUND, token);
    compound->lhs = lhs;
    compound->is_designated = false;
    while (!read_token(p, TOKEN_RBRACE)) {
        Node *child = parse_expr(p, POWER_SET, false, true, NULL);

        bool child_is_designated = false;
        if (read_token(p, TOKEN_SET)) {
            assert(p->state.ahead.kind == TOKEN_SET);
            Node_Binary *binary = (Node_Binary *) node_alloc(p->module_current, NODE_BINARY, p->state.ahead);
            binary->lhs = child;
            binary->rhs = parse_expr(p, POWER_SET, false, true, NULL);
            binary->module = p->module_current;
            child = (Node *) binary;
            child_is_designated = true;
        }

        if (compound->children.head) {
            if (compound->is_designated != child_is_designated) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot mix ordered and designated initializers\n",
                    Pos_Arg(child->token.pos));
                exit(1);
            }
        } else {
            compound->is_designated = child_is_designated;
        }

        nodes_push(&compound->children, child);
        compound->children_count++;
        if (expect_token(p, TOKEN_COMMA, TOKEN_RBRACE).kind != TOKEN_COMMA) {
            break;
        }
    }

    return (Node *) compound;
}

static_assert(COUNT_TOKENS == 77, "");
static Node *parse_expr(Parser *p, Power mbp, bool groups_allowed, bool compounds_allowed, bool *should_be_switch) {
    Node *node = NULL;
    Token token = next_token(p);
    if (token.kind == TOKEN_LAND) {
        split_logical_and(p, &token);
    }

    switch (token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_CHAR:
    case TOKEN_NULL:
    case TOKEN_IDENT:
    case TOKEN_STRING:
    case TOKEN_DIRECTIVE_MAIN:
    case TOKEN_DIRECTIVE_PLATFORM:
    case TOKEN_DIRECTIVE_CALLER_LOCATION:
        node = node_alloc(p->module_current, NODE_ATOM, token);
        ((Node_Atom *) node)->module = p->module_current;
        break;

    case TOKEN_ISTRING: {
        node = node_alloc(p->module_current, NODE_INTERPOLATION, token);
        Node_Interpolation *interp = (Node_Interpolation *) node;

        nodes_push(&interp->children, node_alloc(p->module_current, NODE_ATOM, token));
        interp->children_count++;

        while (token.kind == TOKEN_ISTRING) {
            nodes_push(&interp->children, parse_expr(p, POWER_SET, false, true, NULL));
            interp->children_count++;
            expect_token(p, TOKEN_RBRACE); // This also ensures that there is nothing left in the buffer

            token = lexer_get_string(&p->state.lexer, p->state.lexer.pos);
            nodes_push(&interp->children, node_alloc(p->module_current, NODE_ATOM, token));
            interp->children_count++;
        }
    } break;

    case TOKEN_DOT: {
        node = node_alloc(p->module_current, NODE_MEMBER, expect_token(p, TOKEN_IDENT));
        Node_Member *member = (Node_Member *) node;
        member->dot = token;
        member->module = p->module_current;
    } break;

    case TOKEN_SUB:
    case TOKEN_MUL:
    case TOKEN_BNOT:
    case TOKEN_LNOT: {
        node = node_alloc(p->module_current, NODE_UNARY, token);
        Node_Unary *unary = (Node_Unary *) node;
        unary->value = parse_expr(p, POWER_PRE, false, compounds_allowed, NULL);
        unary->module = p->module_current;
    } break;

    case TOKEN_BAND: {
        node = node_alloc(p->module_current, NODE_UNARY, token);
        Node_Unary *unary = (Node_Unary *) node;
        unary->value = parse_expr(p, POWER_REF, false, compounds_allowed, NULL);
        unary->module = p->module_current;
    } break;

    case TOKEN_DISTINCT: {
        node = node_alloc(p->module_current, NODE_DISTINCT, token);
        Node_Distinct *distinct = (Node_Distinct *) node;
        distinct->value = parse_expr(p, POWER_PRE, false, compounds_allowed, NULL);

        static_assert(COUNT_NODES == 28, "");
        switch (distinct->value->kind) {
        case NODE_ENUM:
        case NODE_TRAIT:
        case NODE_UNION:
        case NODE_STRUCT:
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Redundant application of %s to %s\n",
                Pos_Arg(token.pos),
                token_kind_to_cstr(token.kind),
                token_kind_to_cstr(distinct->value->token.kind));
            exit(1);
            break;

        default:
            break;
        }
    } break;

    case TOKEN_LPAREN: {
        bool is_fn = false;
        Pos  fn_args_end_pos = {0};
        if (read_token(p, TOKEN_RPAREN)) {
            is_fn = true;
        } else {
            node = parse_expr(p, POWER_SET, false, true, NULL);
            if (peek_token(p).kind == TOKEN_COLON) {
                is_fn = true;
                node = parse_define(p, node, next_token(p), false, true, false);
            } else {
                fn_args_end_pos = expect_token(p, TOKEN_RPAREN).pos;
            }
        }

        if (is_fn) {
            Node *arg = node;
            node = node_alloc(p->module_current, NODE_FN, token);

            Node_Fn *fn = (Node_Fn *) node;
            fn->outer_fn = p->state.fn_current;
            fn->module = p->module_current;

            Node_Fn *fn_current_save = p->state.fn_current;
            p->state.fn_current = fn;

            if (arg) {
                Node_Define *define = (Node_Define *) arg;
                assert(define->name->kind == NODE_ATOM && define->name->token.kind == TOKEN_IDENT);

                if (sv_match(define->name->token.sv, "this")) {
                    if (fn->outer_fn) {
                        fprintf(stderr, Pos_Fmt "ERROR: Local function cannot be a method\n", Pos_Arg(token.pos));
                        fprintf(
                            stderr,
                            Pos_Fmt "NOTE: This argument is taken to be the receiver\n",
                            Pos_Arg(define->name->token.pos));
                        exit(1);
                    }

                    if (define->has_spread) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: The receiver of a method cannot be variadic\n",
                            Pos_Arg(define->spread_pos));
                        exit(1);
                    }

                    if (define->expr) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: The receiver of a method cannot have a default value\n",
                            Pos_Arg(define->expr->token.pos));
                        exit(1);
                    }

                    fn->is_method = true;
                }

                definition_lhs_setup(p, define, false);
            }

            Node *typed_variadics = NULL;

            size_t args_iota = 1;
            while (arg) {
                Node_Define *define = (Node_Define *) arg;
                if (define->is_const) {
                    fprintf(
                        stderr, Pos_Fmt "ERROR: Expected argument definition, got constant\n", Pos_Arg(arg->token.pos));
                    exit(1);
                }

                if (!define->expr && fn->variadics_kind == VARIADICS_TYPED) {
                    fprintf(
                        stderr,
                        Pos_Fmt "ERROR: Cannot have argument without default value after typed variadics\n",
                        Pos_Arg(arg->token.pos));
                    exit(1);
                }

                if (define->has_spread) {
                    if (fn->variadics_kind == VARIADICS_TYPED) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Cannot have multiple variadic arguments\n",
                            Pos_Arg(arg->token.pos));
                        fprintf(stderr, Pos_Fmt "NOTE: Previously here\n", Pos_Arg(typed_variadics->token.pos));
                        exit(1);
                    }

                    fn->variadics_kind = VARIADICS_TYPED;
                    typed_variadics = arg;
                }

                if (define->name->kind == NODE_ATOM && define->name->token.kind == TOKEN_IDENT) {
                    Node_Atom *it = (Node_Atom *) define->name;
                    it->definition_spec->arg_index = args_iota++;
                } else {
                    unreachable();
                }

                nodes_push(&fn->args, arg);
                fn->args_count += define->count;
                if (!define->expr && fn->variadics_kind == VARIADICS_NONE) {
                    fn->args_count_min += define->count;
                }

                token = expect_token(p, TOKEN_COMMA, TOKEN_RPAREN);
                if (token.kind != TOKEN_COMMA) {
                    fn_args_end_pos = token.pos;
                    break;
                }

                if (read_token(p, TOKEN_SPREAD)) {
                    if (fn->is_method) {
                        assert(p->state.ahead.kind == TOKEN_SPREAD);
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Methods cannot have untyped variadics\n",
                            Pos_Arg(p->state.ahead.pos));
                        exit(1);
                    }

                    fn->variadics_kind = VARIADICS_UNTYPED;
                    fn_args_end_pos = expect_token(p, TOKEN_RPAREN).pos;
                    break;
                }

                Node *name = node_alloc(p->module_current, NODE_ATOM, expect_token(p, TOKEN_IDENT));
                if (sv_match(name->token.sv, "this")) {
                    fprintf(
                        stderr,
                        Pos_Fmt
                        "ERROR: The argument 'this' is used to describe the receiver of a method, and therefore must be the first argument\n",
                        Pos_Arg(name->token.pos));
                    exit(1);
                }

                arg = parse_define(p, name, expect_token(p, TOKEN_COLON), false, true, false);
            }

            fn->args_end_pos = fn_args_end_pos;

            if (read_token(p, TOKEN_ARROW)) {
                if (read_token(p, TOKEN_LPAREN)) {
                    while (!read_token(p, TOKEN_RPAREN)) {
                        nodes_push(&fn->returns, parse_expr(p, POWER_PRE, false, false, NULL));
                        fn->returns_count++;
                        if (expect_token(p, TOKEN_COMMA, TOKEN_RPAREN).kind != TOKEN_COMMA) {
                            break;
                        }
                    }
                } else {
                    nodes_push(&fn->returns, parse_expr(p, POWER_PRE, false, false, NULL));
                    fn->returns_count++;
                }
            }

            if (peek_token(p).kind == TOKEN_LBRACE) {
                fn->body = parse_block(p, next_token(p));
            } else {
                if (fn->is_method && !p->state.in_extern) {
                    Node_Define *define = (Node_Define *) fn->args.head;
                    assert(define && define->name->kind == NODE_ATOM && define->name->token.kind == TOKEN_IDENT);
                    fprintf(stderr, Pos_Fmt "ERROR: Function type cannot be a method\n", Pos_Arg(fn->node.token.pos));
                    fprintf(
                        stderr,
                        Pos_Fmt "NOTE: This argument is taken to be the receiver\n",
                        Pos_Arg(define->name->token.pos));
                    exit(1);
                }

                fn->is_type = true;
            }

            p->state.fn_current = fn_current_save;
        }
    } break;

    case TOKEN_LBRACE:
        if (!compounds_allowed) {
            error_unexpected(token);
        }
        node = parse_compound(p, NULL, token);
        break;

    case TOKEN_LBRACKET: {
        node = node_alloc(p->module_current, NODE_INDEXABLE, token);
        Node_Indexable *indexable = (Node_Indexable *) node;

        token = peek_token(p);
        if (token.kind != TOKEN_RBRACKET) {
            indexable->count = parse_expr(p, POWER_SET, false, true, NULL);
        }
        expect_token(p, TOKEN_RBRACKET);
        indexable->element = parse_expr(p, POWER_REF, false, false, NULL);
    } break;

    case TOKEN_ENUM: {
        node = node_alloc(p->module_current, NODE_ENUM, token);
        Node_Enum *enumm = (Node_Enum *) node;
        enumm->defined_in = p->state.fn_current;
        enumm->module = p->module_current;

        token = peek_token(p);
        if (token.kind != TOKEN_LBRACE) {
            enumm->underlying = parse_expr(p, POWER_REF, false, false, NULL);
        }

        expect_token(p, TOKEN_LBRACE);
        while (!read_token(p, TOKEN_RBRACE)) {
            Node *it = node_alloc(p->module_current, NODE_UNARY, expect_token(p, TOKEN_IDENT));
            if (read_token(p, TOKEN_SET)) {
                Node_Unary *unary = (Node_Unary *) it;
                unary->value = parse_expr(p, POWER_SET, false, true, NULL);
                unary->module = p->module_current;
            }

            nodes_push(&enumm->values, it);
            enumm->values_count++;
            expect_stmt_terminator(p);
        }
    } break;

    case TOKEN_TRAIT: {
        node = node_alloc(p->module_current, NODE_TRAIT, token);
        Node_Trait *trait = (Node_Trait *) node;
        trait->module = p->module_current;
        trait->defined_in = p->state.fn_current;

        expect_token(p, TOKEN_LBRACE);
        while (!read_token(p, TOKEN_RBRACE)) {
            Node_Atom *name = (Node_Atom *) node_alloc(p->module_current, NODE_ATOM, expect_token(p, TOKEN_IDENT));
            name->module = p->module_current;
            Node *method = parse_define(p, (Node *) name, expect_token(p, TOKEN_COLON), false, false, false);

            Node_Define *define = (Node_Define *) method;
            if (define->is_const) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Expected trait method, got constant definition\n",
                    Pos_Arg(method->token.pos));
                exit(1);
            }

            if (define->expr) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Trait method definition cannot have assignment\n",
                    Pos_Arg(method->token.pos));
                exit(1);
            }

            if (define->type->kind != NODE_FN) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Trait method must be a literal function type\n",
                    Pos_Arg(define->type->token.pos));
                exit(1);
            }

            Node_Fn *fn = (Node_Fn *) define->type;
            fn->trait_method_type = &node->type;

            nodes_push(&trait->methods, method);
            trait->methods_count++;
            expect_stmt_terminator(p);
        }
    } break;

    case TOKEN_UNION: {
        node = node_alloc(p->module_current, NODE_UNION, token);
        Node_Union *unionn = (Node_Union *) node;
        unionn->module = p->module_current;
        unionn->defined_in = p->state.fn_current;

        expect_token(p, TOKEN_LBRACE);
        while (!read_token(p, TOKEN_RBRACE)) {
            nodes_push(&unionn->variants, parse_expr(p, POWER_SET, false, true, NULL));
            unionn->variants_count++;
            expect_stmt_terminator(p);
        }
    } break;

    case TOKEN_STRUCT: {
        node = node_alloc(p->module_current, NODE_STRUCT, token);
        Node_Struct *structt = (Node_Struct *) node;
        structt->module = p->module_current;
        structt->defined_in = p->state.fn_current;

        expect_token(p, TOKEN_LBRACE);
        while (!read_token(p, TOKEN_RBRACE)) {
            token = peek_token(p);
            if (token.kind == TOKEN_SPREAD) {
                Node_Unary *spread = (Node_Unary *) node_alloc(p->module_current, NODE_UNARY, next_token(p));
                spread->value = parse_expr(p, POWER_PRE, false, false, NULL);
                spread->module = p->module_current;
                nodes_push(&structt->fields, (Node *) spread);
            } else {
                Node *field = parse_expr(p, POWER_NIL, true, true, NULL);
                if (field->kind != NODE_DEFINE) {
                    fprintf(stderr, Pos_Fmt "ERROR: Expected field, got expression\n", Pos_Arg(field->token.pos));
                    exit(1);
                }

                Node_Define *define = (Node_Define *) field;
                if (define->is_const) {
                    fprintf(
                        stderr, Pos_Fmt "ERROR: Expected field, got constant definition\n", Pos_Arg(field->token.pos));
                    exit(1);
                }

                if (define->expr) {
                    fprintf(
                        stderr, Pos_Fmt "ERROR: Field definition cannot have assignment\n", Pos_Arg(field->token.pos));
                    exit(1);
                }
                nodes_push(&structt->fields, field);
            }
            expect_stmt_terminator(p);
        }
    } break;

    case TOKEN_SIZEOF:
    case TOKEN_TYPEOF: {
        node = node_alloc(p->module_current, NODE_UNARY, token);
        Node_Unary *unary = (Node_Unary *) node;
        expect_token(p, TOKEN_LPAREN);
        unary->value = parse_expr(p, POWER_SET, false, true, NULL);
        unary->module = p->module_current;
        expect_token(p, TOKEN_RPAREN);
    } break;

    case TOKEN_DIRECTIVE_IMPORT: {
        node = node_alloc(p->module_current, NODE_IMPORT, token);
        Node_Import *import = (Node_Import *) node;
        import->path = expect_token(p, TOKEN_STRING);

        if (!p->state.in_compile_time_condition) {
            parser_import(p, import);
        }
    } break;

    case TOKEN_INLINE: {
        node = parse_expr(p, POWER_DOT, false, false, NULL);
        if (node->kind != NODE_FN) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Expected function literal after %s\n",
                Pos_Arg(token.pos),
                token_kind_to_cstr(token.kind));
            exit(1);
        }

        Node_Fn *fn = (Node_Fn *) node;
        if (p->state.in_extern) {
            fprintf(stderr, Pos_Fmt "ERROR: External function cannot be inlined\n", Pos_Arg(token.pos));
            exit(1);
        }

        if (fn->is_type) {
            fprintf(stderr, Pos_Fmt "ERROR: Function type cannot be inlined\n", Pos_Arg(token.pos));
            exit(1);
        }

        fn->is_inline = true;
    } break;

    default:
        error_unexpected(token);
        break;
    }

    while (true) {
        token = peek_token(p);
        if (token.newline) {
            break;
        }

        const Power lbp = token_kind_to_power(token.kind);
        if (lbp <= mbp) {
            break;
        }
        p->state.peeked = false;

        switch (token.kind) {
        case TOKEN_DOT: {
            Node_Member *member =
                (Node_Member *) node_alloc(p->module_current, NODE_MEMBER, expect_token(p, TOKEN_IDENT, TOKEN_LPAREN));

            member->lhs = node;
            member->dot = token;
            member->module = p->module_current;
            if (member->node.token.kind == TOKEN_LPAREN) {
                member->rhs = parse_expr(p, POWER_SET, false, true, NULL);
                expect_token(p, TOKEN_RPAREN);
            }
            node = (Node *) member;
        } break;

        case TOKEN_COLON:
            return parse_define(p, node, token, groups_allowed, false, false);

        case TOKEN_COMMA: {
            if (!groups_allowed) {
                buffer_token(p, token);
                return node;
            }

            Node_Group *group = (Node_Group *) node_alloc(p->module_current, NODE_GROUP, token);
            nodes_push(&group->nodes, node);
            group->count++;

            do {
                nodes_push(&group->nodes, parse_expr(p, lbp, false, compounds_allowed, NULL));
                group->count++;
            } while (read_token(p, TOKEN_COMMA));

            node = (Node *) group;
        } break;

        case TOKEN_LPAREN: {
            Node_Call *call = (Node_Call *) node_alloc(p->module_current, NODE_CALL, token);
            call->fn = node;

            bool has_named_args = false;
            while (!read_token(p, TOKEN_RPAREN)) {
                token = peek_token(p);

                bool need_to_spread = token.kind == TOKEN_SPREAD;
                if (need_to_spread) {
                    next_token(p);
                }

                Node *arg = parse_expr(p, POWER_SET, false, true, NULL);
                if (arg->kind == NODE_INTERPOLATION) {
                    if (need_to_spread) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Redundant %s before interpolated string\n",
                            Pos_Arg(token.pos),
                            token_kind_to_cstr(token.kind));
                        exit(1);
                    }

                    need_to_spread = true;
                }

                if (need_to_spread) {
                    if (call->spread) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Multiple typed variadic sources found\n",
                            Pos_Arg(call->node.token.pos));

                        fprintf(stderr, Pos_Fmt "... This provide one source\n", Pos_Arg(call->spread_pos));
                        fprintf(stderr, Pos_Fmt "... This provide another\n", Pos_Arg(token.pos));
                        exit(1);
                    }

                    call->spread_pos = token.pos;
                }

                token = peek_token(p);
                if (token.kind == TOKEN_SET) {
                    if (need_to_spread) {
                        fprintf(stderr, Pos_Fmt "ERROR: Cannot spread a named argument\n", Pos_Arg(token.pos));
                        exit(1);
                    }

                    if (arg->kind != NODE_ATOM || arg->token.kind != TOKEN_IDENT) {
                        error_unexpected(token);
                    }

                    if (sv_match(arg->token.sv, "_")) {
                        fprintf(stderr, Pos_Fmt "ERROR: Cannot use '_' as a named argument\n", Pos_Arg(arg->token.pos));
                        exit(1);
                    }

                    Node_Binary *binary = (Node_Binary *) node_alloc(p->module_current, NODE_BINARY, next_token(p));
                    binary->lhs = arg;
                    binary->rhs = parse_expr(p, POWER_SET, false, true, NULL);
                    binary->module = p->module_current;
                    arg = (Node *) binary;
                    has_named_args = true;
                } else {
                    if (call->spread) {
                        const char *label = "variadics spread";
                        if (call->spread->kind == NODE_INTERPOLATION) {
                            label = "interpolated string";
                        }

                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Cannot have positional arguments after %s\n",
                            Pos_Arg(arg->token.pos),
                            label);
                        exit(1);
                    }

                    if (has_named_args) {
                        fprintf(
                            stderr,
                            Pos_Fmt "ERROR: Cannot have positional arguments after named arguments\n",
                            Pos_Arg(arg->token.pos));
                        exit(1);
                    }
                }

                if (need_to_spread) {
                    call->spread = arg;
                    call->do_not_allocate_typed_variadic_array = true;
                }

                nodes_push(&call->args, arg);
                if (expect_token(p, TOKEN_COMMA, TOKEN_RPAREN).kind != TOKEN_COMMA) {
                    break;
                }
            }

            assert(p->state.ahead.kind == TOKEN_RPAREN);
            call->end = p->state.ahead.pos;
            node = (Node *) call;
        } break;

        case TOKEN_LBRACE:
            if (!compounds_allowed) {
                buffer_token(p, token);
                return node;
            }
            node = parse_compound(p, node, token);
            break;

        case TOKEN_LBRACKET: {
            Node_Index *index = (Node_Index *) node_alloc(p->module_current, NODE_INDEX, token);
            index->lhs = node;
            index->module = p->module_current;

            if (peek_token(p).kind != TOKEN_RANGE) {
                index->a = parse_expr(p, POWER_SET, false, true, NULL);
            }

            token = expect_token(p, TOKEN_RANGE, TOKEN_RBRACKET);
            if (token.kind == TOKEN_RANGE) {
                index->is_ranged = true;
                if (peek_token(p).kind != TOKEN_RBRACKET) {
                    index->b = parse_expr(p, POWER_SET, false, true, NULL);
                }
                expect_token(p, TOKEN_RBRACKET);
            }

            node = (Node *) index;
        } break;

        default:
            if (should_be_switch && token.kind == TOKEN_EQ && peek_token(p).kind == TOKEN_LBRACE) {
                *should_be_switch = true;
                return node;
            } else {
                Node_Binary *binary = (Node_Binary *) node_alloc(p->module_current, NODE_BINARY, token);
                binary->lhs = node;
                binary->rhs = parse_expr(p, lbp, groups_allowed, compounds_allowed, NULL);
                binary->module = p->module_current;
                node = (Node *) binary;
                if (lbp == POWER_SET) {
                    return node;
                }
            }
            break;
        }
    }

    return node;
}

static void local_assert(Parser *p, bool expected_is_local, Token token, const char *label) {
    if ((p->state.fn_current != NULL) != expected_is_local) {
        if (!label) {
            label = token_kind_to_cstr(token.kind);
        }

        fprintf(
            stderr,
            Pos_Fmt "ERROR: Unexpected %s in %s scope\n",
            Pos_Arg(token.pos),
            label,
            p->state.fn_current ? "local" : "global");

        exit(1);
    }
}

static_assert(COUNT_NODES == 28, "");
static Node *parse_stmt(Parser *p) {
    Node *node = NULL;

    Token token = next_token(p);
    switch (token.kind) {
    case TOKEN_DIRECTIVE_ASSERT: {
        node = node_alloc(p->module_current, NODE_ASSERT, token);
        Node_Assert *assertt = (Node_Assert *) node;

        expect_token(p, TOKEN_LPAREN);
        assertt->expr = parse_expr(p, POWER_SET, false, true, NULL);

        if (expect_token(p, TOKEN_COMMA, TOKEN_RPAREN).kind == TOKEN_COMMA) {
            assertt->message = parse_expr(p, POWER_SET, false, true, NULL);
            expect_token(p, TOKEN_RPAREN);
        }
    } break;

    case TOKEN_DIRECTIVE_LINK: {
        if (!p->state.in_extern) {
            local_assert(p, false, token, NULL);
        }

        const Token name = expect_token(p, TOKEN_STRING);
        if (!name.sv.count) {
            fprintf(stderr, Pos_Fmt "ERROR: Link name cannot be empty\n", Pos_Arg(name.pos));
            exit(1);
        }

        node = parse_stmt(p);
        if (node->kind != NODE_DEFINE) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Expected definition after %s\n",
                Pos_Arg(node->token.pos),
                token_kind_to_cstr(token.kind));
            exit(1);
        }

        Node_Define *define = (Node_Define *) node;
        if (define->count != 1) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot apply %s to multiple definitions\n",
                Pos_Arg(node->token.pos),
                token_kind_to_cstr(token.kind));
            exit(1);
        }

        if (define->is_const) {
            bool ok = false;
            if (define->expr && define->expr->kind == NODE_FN) {
                Node_Fn *fn = (Node_Fn *) define->expr;
                if (fn->body || p->state.in_extern) {
                    ok = true;
                }
            }

            if (!ok) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Cannot apply %s to constant definitions\n",
                    Pos_Arg(node->token.pos),
                    token_kind_to_cstr(token.kind));
                exit(1);
            }
        }

        assert(define->name->kind == NODE_ATOM);
        Node_Atom *it = (Node_Atom *) define->name;

        assert(it->definition_spec);
        it->definition_spec->link_as = name.sv;
    } break;

    case TOKEN_DIRECTIVE_STATIC: {
        if (p->state.in_extern) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Variables defined inside extern block cannot have static storage\n",
                Pos_Arg(token.pos));
            exit(1);
        }

        if (!p->state.fn_current) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Variables defined in global scope already have static storage\n",
                Pos_Arg(token.pos));
            exit(1);
        }

        node = parse_expr(p, POWER_NIL, true, true, NULL);
        if (node->kind != NODE_DEFINE) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Expected variable definition after %s\n",
                Pos_Arg(node->token.pos),
                token_kind_to_cstr(token.kind));
            exit(1);
        }

        Node_Define *define = (Node_Define *) node;
        if (define->is_const) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Expected variable definition after %s, got constant\n",
                Pos_Arg(node->token.pos),
                token_kind_to_cstr(token.kind));
            exit(1);
        }

        definition_lhs_setup(p, define, true);
    } break;

    case TOKEN_DIRECTIVE_PRIVATE: {
        local_assert(p, false, token, NULL);

        const bool after_private_save = p->state.after_private;
        p->state.after_private = true;

        if (!read_token(p, TOKEN_SPREAD)) {
            node = parse_stmt(p);
            if (node->kind != NODE_DEFINE && node->kind != NODE_EXTERN) {
                fprintf(
                    stderr,
                    Pos_Fmt "ERROR: Expected definition or extern block after %s\n",
                    Pos_Arg(node->token.pos),
                    token_kind_to_cstr(token.kind));
                exit(1);
            }

            p->state.after_private = after_private_save;
        }
    } break;

    case TOKEN_DIRECTIVE_LIBRARY: {
        node = node_alloc(p->module_current, NODE_IMPORT, token);
        Node_Import *import = (Node_Import *) node;
        if (read_token(p, TOKEN_LBRACE)) {
            while (!read_token(p, TOKEN_RBRACE)) {
                Node *library = node_alloc(p->module_current, NODE_ATOM, expect_token(p, TOKEN_STRING));
                nodes_push(&import->libraries, library);
                expect_stmt_terminator(p);
            }
        } else {
            Node *library = node_alloc(p->module_current, NODE_ATOM, expect_token(p, TOKEN_STRING));
            nodes_push(&import->libraries, library);
        }
    } break;

    case TOKEN_LBRACE:
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        node = parse_block(p, token);
        break;

    case TOKEN_IF:
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        node = parse_if(p, token, false);
        break;

    case TOKEN_DIRECTIVE_IF: {
        const bool after_private_save = p->state.after_private;
        node = parse_if(p, token, true);
        p->state.after_private = after_private_save;
    } break;

    case TOKEN_FOR:
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        node = parse_for(p, token);
        break;

    case TOKEN_DEFER: {
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        if (p->state.in_defer) {
            fprintf(stderr, Pos_Fmt "ERROR: Nested 'defer' is not allowed\n", Pos_Arg(token.pos));
            exit(1);
        }

        const bool in_defer_save = p->state.in_defer;
        p->state.in_defer = true;
        node = node_alloc(p->module_current, NODE_DEFER, token);
        Node_Defer *defer = (Node_Defer *) node;
        defer->stmt = parse_stmt(p);
        p->state.in_defer = in_defer_save;
    } break;

    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
        not_in_extern_assert(p, token);
        if (!p->state.in_loop) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot use %s outside of a loop\n",
                Pos_Arg(token.pos),
                token_kind_to_cstr(token.kind));
            exit(1);
        }

        if (p->state.in_defer) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot use %s inside 'defer' statement\n",
                Pos_Arg(token.pos),
                token_kind_to_cstr(token.kind));
            exit(1);
        }

        node = node_alloc(p->module_current, NODE_JUMP, token);
        break;

    case TOKEN_RETURN: {
        not_in_extern_assert(p, token);
        local_assert(p, true, token, NULL);
        if (p->state.in_defer) {
            fprintf(
                stderr,
                Pos_Fmt "ERROR: Cannot use %s inside 'defer' statement\n",
                Pos_Arg(token.pos),
                token_kind_to_cstr(token.kind));
            exit(1);
        }

        node = node_alloc(p->module_current, NODE_RETURN, token);

        Node_Return *returnn = (Node_Return *) node;
        if (!peek_token(p).newline) {
            returnn->value = parse_expr(p, POWER_SET, true, true, NULL);
        }
    } break;

    case TOKEN_EXTERN: {
        if (p->state.in_extern) {
            fprintf(stderr, Pos_Fmt "ERROR: Cannot have nested externs\n", Pos_Arg(token.pos));
            exit(1);
        }

        const bool after_private_save = p->state.after_private;

        node = node_alloc(p->module_current, NODE_EXTERN, token);
        Node_Extern *externn = (Node_Extern *) node;

        expect_token(p, TOKEN_LBRACE);
        p->state.in_extern = true;
        while (!read_token(p, TOKEN_RBRACE)) {
            nodes_push(&externn->nodes, parse_stmt(p));
        }
        p->state.in_extern = false;

        p->state.after_private = after_private_save;
    } break;

    default:
        buffer_token(p, token);
        node = parse_expr(p, POWER_NIL, true, true, NULL);
        if (node->kind != NODE_DEFINE) {
            not_in_extern_assert(p, token);
            if (node->kind != NODE_IMPORT) {
                local_assert(p, true, node->token, "expression");
            }
        }

        if (node->kind == NODE_CALL) {
            ((Node_Call *) node)->is_stmt = true;
        }
        break;
    }

    return node;
}

void parser_free(Parser *p) {
    da_free(&p->paths);
}

Parse_Result parse_file(Parser *p, const char *path) {
    assert(p->modules);
    assert(p->module_current);
    if (!lexer_open(&p->state.lexer, path)) {
        return PARSE_FAILURE;
    }

    while (true) {
        consume_tokens(p, TOKEN_EOL);
        if (read_token(p, TOKEN_EOF)) {
            break;
        }

        nodes_push(&p->module_current->nodes, parse_stmt(p));
    }

    p->state.after_private = false;
    return PARSE_OK;
}

static int compare_cstrs(const void *a, const void *b) {
    const char *str1 = *(const char **) a;
    const char *str2 = *(const char **) b;
    return strcmp(str1, str2);
}

// `path` should be relative. No processing will be done
static bool get_source_files(const char *path, Paths *items, Arena *a) {
    bool         result = true;
    const size_t start = items->count;

    const SV   path_sv = sv_from_cstr(path);
    const bool path_is_dot = sv_match(path_sv, ".");
    const bool path_ends_with_slash = sv_has_suffix(path_sv, sv_from_cstr("/"));

#ifdef PLATFORM_X86_64_WINDOWS
    char *search_path = arena_sprintf(&temp_arena, "%s\\*", path);

    WIN32_FIND_DATA find_file_data;
    HANDLE          handle = FindFirstFile(search_path, &find_file_data);
    do {
        if (handle == INVALID_HANDLE_VALUE) {
            return_defer(false);
        }

        if (!strcmp(find_file_data.cFileName, ".") || !strcmp(find_file_data.cFileName, "..")) {
            continue;
        }

        if (find_file_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }

        if (!sv_has_suffix(sv_from_cstr(find_file_data.cFileName), sv_from_cstr(".glos"))) {
            continue;
        }

        const char *item = NULL;
        if (path_is_dot) {
            item = arena_sprintf(a, "%s", find_file_data.cFileName);
        } else if (path_ends_with_slash) {
            item = arena_sprintf(a, "%s%s", path, find_file_data.cFileName);
        } else {
            item = arena_sprintf(a, "%s/%s", path, find_file_data.cFileName);
        }
        da_push(items, item);
    } while (FindNextFile(handle, &find_file_data) != 0);

    FindClose(handle);
#else
    DIR *dir = opendir(path);
    if (!dir) {
        return_defer(false);
    }

    errno = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
            continue;
        }

        if (entry->d_type == DT_DIR) {
            continue;
        }

        if (!sv_has_suffix(sv_from_cstr(entry->d_name), sv_from_cstr(".glos"))) {
            continue;
        }

        const char *item = NULL;
        if (path_is_dot) {
            item = arena_sprintf(a, "%s", entry->d_name);
        } else if (path_ends_with_slash) {
            item = arena_sprintf(a, "%s%s", path, entry->d_name);
        } else {
            item = arena_sprintf(a, "%s/%s", path, entry->d_name);
        }
        da_push(items, item);
    }

    if (errno) {
        return_defer(false);
    }
#endif // PLATFORM_X86_64_WINDOWS

    qsort(items->data + start, items->count - start, sizeof(*items->data), compare_cstrs);
    return_defer(true);

defer:
    if (!result && items->count > start) {
        arena_reset(a, items->data[start]);
        items->count = start;
    }

#ifdef PLATFORM_X86_64_WINDOWS
    arena_reset(&temp_arena, search_path);
#else
    if (dir) {
        closedir(dir);
    }
#endif // PLATFORM_X86_64_WINDOWS

    return result;
}

Parse_Result parse_directory(Parser *p, const char *path) {
    assert(p->modules);
    assert(p->module_current);

    const size_t start = p->paths.count;
    if (!get_source_files(path, &p->paths, &default_arena)) {
        return PARSE_FAILURE;
    }

    if (sv_match(p->module_current->name, "builtin")) {
        bool found = false;
        for (size_t i = start; i < p->paths.count; i++) {
            const char *it = p->paths.data[i];
            if (sv_has_suffix(sv_from_cstr(it), sv_from_cstr("builtin/contract.glos"))) {
                found = true;
                for (size_t j = i; j > start; j--) {
                    p->paths.data[j] = p->paths.data[j - i];
                }
                p->paths.data[start] = it;
                break;
            }
        }
        assert(found);
    }

    bool empty = true;

    const Parser_State parser_state_save = p->state;
    memset(&p->state, 0, sizeof(p->state));

    for (size_t i = start; i < p->paths.count; i++) {
        const char *it = p->paths.data[i];

        empty = false;
        if (parse_file(p, it) != PARSE_OK) {
            fprintf(stderr, "ERROR: Could not read file '%s'", it);
            exit(1);
        }
    }

    p->paths.count = start;
    p->state = parser_state_save;

    if (empty) {
        return PARSE_EMPTY_DIRECTORY;
    }
    return PARSE_OK;
}
