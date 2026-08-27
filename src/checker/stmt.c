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
            check_type_satisfies_trait_old(c, type, sw->trait->node.type.spec.trait, pred, -1);
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

static void prepare_impl_method_for_printing_type(Node_Impl *impl, Node_Fn *actual, Type_Trait_Method *expected) {
    assert(expected->type.kind == TYPE_FN);
    Type_Fn *spec = expected->type.spec.fn;
    assert(spec->args_count);

    Type type = impl->receiver->type;
    type.distinct = (Node_Atom *) node_alloc(impl->node.module, NODE_ATOM, (Token) {.sv = sv_from_cstr("Self")});
    // This allocation per method might be a bit wasteful, but we are gonna die anyways, so who cares...

    spec->args[0].type = type;
    if (actual) {
        actual->body = NULL;

        assert(actual->node.type.kind == TYPE_FN);
        Type_Fn *spec = actual->node.type.spec.fn;
        assert(spec->args_count);

        spec->args[0].type = type_with_ref(type, spec->args[0].type.ref);
    }
}

void check_stmt_impl(Compiler *c, Node_Impl *impl) {
    if (impl->methods_checked) {
        return;
    }

    ll_foreach(it, &impl->methods) {
        assert(it->kind == NODE_DEFINE);
        Node_Define *define = (Node_Define *) it;

        assert(define->expr->kind == NODE_FN);
        check_fn(c, (Node_Fn *) define->expr, REF_NONE, NULL, false, false);
    }

    if (impl->trait) {
        assert(type_kind_eq(impl->trait->type, TYPE_TRAIT));
        Type_Trait *trait = impl->trait->type.spec.trait;
        Type_Trait *trait_save = trait;
        if (trait->polymorph) {
            ht_clear(&c->monomorph_replacements);

            const size_t monomorph_parameters_begin_save = c->monomorph_parameters.begin;
            c->monomorph_parameters.begin = c->monomorph_parameters.count;

            const Monomorphizing_Site monomorphizing_site_save = c->monomorphizing_site;
            c->monomorphizing_site.expr = impl->receiver;
            c->monomorphizing_site.node = impl->receiver;

            add_monomorph_parameter(
                c, trait->polymorph, impl->receiver->type, const_value_type(impl->receiver->type), NULL);
            Node *node = monomorphize(c, (Node *) trait->original_definition, impl->receiver);

            c->monomorph_parameters.count = c->monomorph_parameters.begin;
            c->monomorph_parameters.begin = monomorph_parameters_begin_save;
            c->monomorphizing_site = monomorphizing_site_save;

            assert(type_meta_kind_eq(node->type, TYPE_TRAIT));
            trait = node->type.spec.trait;
        }

        Node_Fn **methods = arena_alloc(&temp_arena, trait->methods_count * sizeof(*methods));

        // Collect the methods from the impl block into an array by the index of the trait method
        {
            bool ok = true;
            ll_foreach(it, &impl->methods) {
                assert(it->kind == NODE_DEFINE);
                Node_Define *define = (Node_Define *) it;

                assert(define->expr->kind == NODE_FN);
                Node_Fn *fn = (Node_Fn *) define->expr;

                bool found = false;
                for (size_t i = 0; i < trait->methods_count; i++) {
                    Type_Trait_Method *it = &trait->methods[i];
                    if (sv_eq(it->name, define->name->token.sv)) {
                        if (methods[i]) {
                            error_redefinition(c, define->name, &methods[i]->defined_as->node.token.pos);
                        }

                        found = true;
                        methods[i] = fn;
                        break;
                    }
                }

                if (!found) {
                    error_token(
                        EK_ERROR,
                        define->name->token,
                        "Method '" SV_Fmt "' is not a requirement of %s",
                        SV_Arg(define->name->token.sv),
                        type_to_cstr(impl->trait->type));
                    ok = false;
                }
            }

            if (!ok) {
                error_node(EK_NOTE, (Node *) trait->definition, "Trait defined here");
                exit(c, 1);
            }
        }

        typedef enum {
            OK,
            UNDEFINED,
            WRONG_RECEIVER,
            WRONG_SIGNATURE,
        } Error;

        Error *errors = arena_alloc(&temp_arena, trait->methods_count * sizeof(*errors));

        {
            for (size_t i = 0; i < trait->methods_count; i++) {
                const Type_Trait_Method *expected = &trait->methods[i];

                Node_Fn *actual = methods[i];
                if (!actual) {
                    errors[i] = UNDEFINED;
                    continue;
                }

                if (actual->polymorphs.count) {
                    todo();
                }

                assert(expected->type.kind == TYPE_FN);
                const Type_Fn *expected_spec = expected->type.spec.fn;

                assert(actual->node.type.kind == TYPE_FN);
                const Type_Fn *actual_spec = actual->node.type.spec.fn;

                if (expected_spec->args_count != actual_spec->args_count) {
                    errors[i] = WRONG_SIGNATURE;
                    continue;
                }

                for (size_t j = 1; j < actual_spec->args_count; j++) {
                    if (!type_eq(actual_spec->args[j].type, expected_spec->args[j].type)) {
                        errors[i] = WRONG_SIGNATURE;
                        continue;
                    }
                }

                if (!type_eq(*actual_spec->return_type, *expected_spec->return_type)) {
                    errors[i] = WRONG_SIGNATURE;
                    continue;
                }

                if (!type_eq(actual_spec->args[0].type, impl->receiver->type)) {
                    errors[i] = WRONG_RECEIVER;
                    continue;
                }
            }

            bool ok = true;
            for (size_t i = 0; i < trait->methods_count; i++) {
                const Error it = errors[i];
                if (it == OK) {
                    continue;
                }

                if (ok) {
                    error_token_range(
                        EK_ERROR,
                        impl->node.token,
                        get_rightmost_token_of_node(impl->trait),
                        "This implementation of %s does not satisfy %s",
                        type_to_cstr(impl->receiver->type),
                        type_to_cstr(impl->trait->type));
                    ok = false;
                }

                Node_Fn           *actual = methods[i];
                Type_Trait_Method *expected = &trait->methods[i];
                prepare_impl_method_for_printing_type(impl, actual, expected);

                switch (it) {
                case UNDEFINED:
                    error_parts(
                        EK_NOTE,
                        expected->name,
                        expected->pos,
                        "The trait method '" SV_Fmt "' is not implemented",
                        SV_Arg(expected->name));

                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "    Expected: %s\n\n",
                        type_to_cstr_raw(expected->type));
                    break;

                case WRONG_RECEIVER:
                    error_node(
                        EK_NOTE,
                        actual->args.head,
                        "The trait method '" SV_Fmt "' has receiver %s, not 'Self'",
                        SV_Arg(actual->defined_as->node.token.sv),
                        type_to_cstr(actual->node.type.spec.fn->args[0].type));
                    break;

                case WRONG_SIGNATURE:
                    error_node(
                        EK_NOTE,
                        (Node *) actual,
                        "The trait method '" SV_Fmt "' has wrong signature",
                        SV_Arg(actual->defined_as->node.token.sv));

                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "    Expected: %s\n"
                        "    Actual:   %s\n\n",
                        type_to_cstr_raw(expected->type),
                        type_to_cstr_raw(actual->node.type));
                    break;

                case OK:
                    break;
                }
            }

            if (!ok) {
                error_node(EK_NOTE, (Node *) trait->definition, "Trait defined here");
                exit(c, 1);
            }
        }

        Type_Trait_Impl trait_impl = {0};
        trait_impl.type = type_without_ref(impl->receiver->type);
        trait_impl.trait = trait;

        trait_impl.methods = arena_alloc(&default_arena, trait->methods_count * sizeof(*trait_impl.methods));
        trait_impl.methods_count = trait->methods_count;
        for (size_t i = 0; i < trait_impl.methods_count; i++) {
            trait_impl.methods[i].fn = methods[i];
        }

        trait = trait_save;
        trait_impl.next = trait->impls.head;
        trait->impls.head = arena_clone(&default_arena, &trait_impl, sizeof(trait_impl));

        arena_reset(&temp_arena, methods);
    }

    impl->methods_checked = true;
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

static_assert(COUNT_NODES == 31, "");
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

    case NODE_IMPL:
        check_stmt_impl(c, (Node_Impl *) n);
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
