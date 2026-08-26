#include "../error.h"
#include "checker.h"

void check_switch_expr_and_alloc_preds(Compiler *c, Node_Switch *sw) {
    check_expr(c, sw->expr, REF_NONE);
    finalize_untyped_type(c, sw->expr);
    check_that_type_is_known(c, sw->expr);

    if (!sw->expr->type.ref && type_kind_eq(sw->expr->type, TYPE_ENUM)) {
        sw->enumeration = sw->expr->type.spec.enumm.definition;
    } else if (type_is_trait(sw->expr->type)) {
        sw->trait = sw->expr->type.spec.trait->definition;
    } else if (type_is_union(sw->expr->type)) {
        sw->unionn = sw->expr->type.spec.unionn->definition;
    } else if (sw->expr->type.is_meta) {
        sw->expr->emit_type_info = arena_clone(&default_arena, &sw->expr->type, sizeof(sw->expr->type));
        sw->expr->emit_type_info->is_meta = false;
        sw->expr->type = c->type_info_pointer_type;
        sw->is_expr_type_info = true;
    } else if (type_eq(sw->expr->type, c->type_info_pointer_type)) {
        sw->is_expr_type_info = true;
    } else if (!type_is_scalar(sw->expr->type)) {
        // TODO: Use equal
        sw->compare_overload = get_operator_overload(c, c->ordered_trait, sw->expr, sw->expr, -1);
    }

    if (!sw->preds) {
        sw->preds = arena_alloc(&default_arena, sw->preds_count * sizeof(*sw->preds));
    }
}

Const_Value check_switch_pred(Compiler *c, Node_Switch *sw, Node *pred, size_t *iota) {
    Const_Value value = {0};
    check_expr(c, pred, REF_NONE);

    if (sw->trait) {
        if (node_is_null(pred)) {
            value = const_value_u64(0);
        } else {
            type_assert_type(c, pred);
            const Type type = type_without_meta(pred->type);
            check_type_satisfies_trait(c, type, sw->trait->node.type.spec.trait, pred, -1);
            value = const_value_type(type);
        }
    } else if (sw->unionn) {
        if (node_is_null(pred)) {
            value = const_value_u64(0);
        } else {
            type_assert_type(c, pred);
            value = const_value_u64(get_union_type_index(c, pred, sw->expr->type));
        }
    } else {
        type_assert(c, pred, sw->expr->type);
        value = eval_const_expr(c, pred, false);
    }

    for (size_t i = 0; i < *iota; i++) {
        if (const_value_eq(sw->preds[i].value, value)) {
            error_node_begin(EK_ERROR, pred);
            fprintf(stderr, "Duplicate case ");
            const_value_debug(stderr, pred->type, value);
            error_finalize();
            error_node(EK_NOTE, sw->preds[i].pred, "Already here");
            exit(c, 1);
        }
    }

    sw->preds[*iota].pred = pred;
    sw->preds[*iota].value = value;
    (*iota)++;
    return value;
}

void check_stmt_assert(Compiler *c, Node_Assert *assertt) {
    Node *n = (Node *) assertt;
    check_expr(c, assertt->expr, REF_NONE);
    type_assert(c, assertt->expr, (Type) {.kind = TYPE_BOOL});

    if (assertt->message) {
        check_expr(c, assertt->message, REF_NONE);
        type_assert(c, assertt->message, (Type) {.kind = TYPE_STRING});
    }

    if (int128_is_zero(eval_const_expr(c, assertt->expr, false).as.integer)) {
        error_node_begin(EK_BLANK, n);
        afprintf(stderr, ANSI_COLOR_RED | ANSI_BOLD, "Assertion Failed");
        if (assertt->message) {
            const SV message = eval_const_expr(c, assertt->message, false).as.string;
            afprintf(stderr, ANSI_COLOR_RED | ANSI_BOLD, ": ");
            fprintf(stderr, SV_Fmt, SV_Arg(message));
        }
        error_finalize();
        exit(c, 1);
    }
}

void check_stmt_define(Compiler *c, Node_Define *define) {
    if (define->expr && define->is_value_known_at_compile_time) {
        Node_Atom *lhs = NULL;
        Node      *rhs = NULL;
        while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
            rhs = node_iter(rhs, define->expr);
            assert(rhs);
            check_definition(c, lhs, rhs, define->type, false);
        }
    } else {
        Node_Atom *lhs = NULL;
        while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
            check_definition(c, lhs, define->expr, define->type, false);
        }
    }
}

void check_stmt_block(Compiler *c, Node_Block *block) {
    const size_t context_defines_end_save = c->context.fn->defines_end;
    const size_t context_imports_end_save = c->context.fn->imports_end;
    for (Node *it = block->body.head; it; it = it->next) {
        define_orderless_node(c, it, context_defines_end_save);
    }

    for (Node *it = block->body.head; it; it = it->next) {
        check_stmt(c, it);
    }
    context_set_end(&c->context, context_defines_end_save, context_imports_end_save);
}

void check_stmt_if(Compiler *c, Node_If *iff) {
    if (iff->is_compile_time) {
        if (iff->compile_time_real) {
            Context_Replace *context_replace_save = c->context.replace;
            c->context.replace = &iff->context_replace;

            if (iff->compile_time_real->kind == NODE_BLOCK) {
                Node_Block *block = (Node_Block *) iff->compile_time_real;
                for (Node *it = block->body.head; it; it = it->next) {
                    check_stmt(c, it);
                }
            } else {
                check_stmt(c, iff->compile_time_real);
            }

            c->context.replace = context_replace_save;
        }
    } else {
        check_expr(c, iff->condition, REF_NONE);
        type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});

        iff->context_replace.outer = c->context.replace;
        if (iff->condition->kind == NODE_BINARY && iff->condition->token.kind == TOKEN_EQ) {
            Node_Binary *condition = (Node_Binary *) iff->condition;
            if ((type_is_trait(condition->lhs->type) || type_is_union(condition->lhs->type)) &&
                condition->lhs->kind == NODE_ATOM) //
            {
                if (!node_is_null(condition->rhs)) {
                    push_context_replace(
                        c, &iff->context_replace, ((Node_Atom *) condition->lhs)->definition, condition->rhs->type);
                }
            } else if (
                (type_is_trait(condition->rhs->type) || type_is_union(condition->rhs->type)) &&
                condition->rhs->kind == NODE_ATOM) //
            {
                if (!node_is_null(condition->lhs)) {
                    push_context_replace(
                        c, &iff->context_replace, ((Node_Atom *) condition->rhs)->definition, condition->lhs->type);
                }
            }
        }

        check_stmt(c, iff->consequence);
        c->context.replace = iff->context_replace.outer;

        check_stmt(c, iff->antecedence);
    }
}

void check_stmt_for(Compiler *c, Node_For *forr) {
    const size_t context_defines_end_save = c->context.fn->defines_end;
    const size_t context_imports_end_save = c->context.fn->imports_end;
    {
        check_stmt(c, forr->init);
        if (forr->condition) {
            check_expr(c, forr->condition, REF_NONE);
            type_assert(c, forr->condition, (Type) {.kind = TYPE_BOOL});
        }
        check_stmt(c, forr->update);
        check_stmt(c, forr->body);
    }
    context_set_end(&c->context, context_defines_end_save, context_imports_end_save);
}

void check_stmt_switch(Compiler *c, Node_Switch *sw) {
    check_switch_expr_and_alloc_preds(c, sw);
    if (sw->is_compile_time) {
        if (sw->compile_time_real) {
            Context_Replace *context_replace_save = c->context.replace;

            Node_Case *branch = sw->compile_time_real;
            c->context.replace = &branch->context_replace;

            assert(branch->body->kind == NODE_BLOCK);
            Node_Block *block = (Node_Block *) branch->body;
            for (Node *it = block->body.head; it; it = it->next) {
                check_stmt(c, it);
            }

            c->context.replace = context_replace_save;
        }
    } else {
        size_t iota = 0;
        for (Node *it = sw->cases.head; it; it = it->next) {
            Node_Case *branch = (Node_Case *) it;
            for (Node *pred = branch->preds.head; pred; pred = pred->next) {
                check_switch_pred(c, sw, pred, &iota);
            }

            branch->context_replace.outer = c->context.replace;
            if ((sw->trait || sw->unionn) && sw->expr->kind == NODE_ATOM && branch->preds_count == 1) {
                if (!node_is_null(branch->preds.head)) {
                    push_context_replace(
                        c, &branch->context_replace, ((Node_Atom *) sw->expr)->definition, branch->preds.head->type);
                }
            }

            check_stmt(c, branch->body);
            c->context.replace = branch->context_replace.outer;
        }
        assert(iota == sw->preds_count);

        check_switch_exhaustive(c, sw);
    }
}

void check_stmt_return(Compiler *c, Node_Return *returnn) {
    Node          *n = (Node *) returnn;
    const Type_Fn *fn_type = c->context.fn->fn->node.type.spec.fn;
    if (returnn->value) {
        check_expr(c, returnn->value, REF_NONE);

        const bool   is_group = type_kind_eq(returnn->value->type, TYPE_GROUP);
        const size_t actual_count = is_group ? returnn->value->type.spec.group.count : 1;

        if (actual_count != fn_type->returns_count) {
            error_number_of_return_values_mismatch(c, n->token, fn_type->returns_count, actual_count);
        }

        assert(actual_count == fn_type->returns_count);
        for (size_t i = 0; i < fn_type->returns_count; i++) {
            i64   group_index = -1;
            Node *n = get_node_from_group(returnn->value, i, &group_index);
            type_assert_grouped(c, n, fn_type->returns[i], group_index, NULL);
        }

        // The inference of the individual group items might not have reflected here
        returnn->value->type = *fn_type->return_type;
    } else {
        if (fn_type->returns_count) {
            error_number_of_return_values_mismatch(c, n->token, fn_type->returns_count, 0);
        }
    }

    n->type = *fn_type->return_type;
}

static_assert(COUNT_NODES == 29, "");
void check_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ASSERT:
        check_stmt_assert(c, (Node_Assert *) n);
        break;

    case NODE_DEFINE:
        check_stmt_define(c, (Node_Define *) n);
        break;

    case NODE_BLOCK:
        check_stmt_block(c, (Node_Block *) n);
        break;

    case NODE_IF:
        check_stmt_if(c, (Node_If *) n);
        break;

    case NODE_FOR:
        check_stmt_for(c, (Node_For *) n);
        break;

    case NODE_CASE:
        unreachable();

    case NODE_SWITCH:
        check_stmt_switch(c, (Node_Switch *) n);
        break;

    case NODE_JUMP:
        // Pass
        break;

    case NODE_DEFER: {
        Node_Defer *defer = (Node_Defer *) n;
        check_stmt(c, defer->stmt);
    } break;

    case NODE_RETURN:
        check_stmt_return(c, (Node_Return *) n);
        break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            check_stmt(c, it);
        }
    } break;

    default:
        check_expr(c, n, REF_NONE);
        check_that_type_is_known(c, n);
        break;
    }
}
