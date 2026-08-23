#include "../error.h"
#include "checker.h"

Node_Atom *module_globals_find_ex(Compiler *c, Module *m, SV name, Module *skip) {
    if (m->orderless_check_status == UNCHECKED) {
        define_orderless_nodes_of_module(c, m, NULL);
    }

    Node_Atom *g = global_scope_find(&m->globals, name);
    if (g) {
        return g;
    }

    for (size_t i = 0; i < m->imports.count; i++) {
        Module *module = m->imports.data[i]->module;
        if (skip && module == skip) {
            continue;
        }

        g = global_scope_find(&module->globals, name);
        if (g) {
            return g;
        }
    }

    return NULL;
}

Node_Atom *module_globals_find(Compiler *c, Module *m, SV name) {
    return module_globals_find_ex(c, m, name, NULL);
}

Node_Fn *get_main(Compiler *c) {
    if (c->main_fn) {
        return c->main_fn;
    }

    Node_Atom *main = module_globals_find(c, c->main_module, sv_from_cstr("main"));
    if (!main) {
        error_standalone(EK_ERROR, "Function 'main' is not defined");
        afprintf(
            stderr,
            ANSI_COLOR_YELLOW | ANSI_BOLD,
            "\n"
            "    main :: () {\n"
            "    }\n"
            "\n");
        exit(c, 1);
    }

    if (!main->definition_spec->is_const || main->definition_spec->assignment_node->kind != NODE_FN) {
        error_node(EK_ERROR, (Node *) main, "Identifier 'main' must be a function literal");
        afprintf(
            stderr,
            ANSI_COLOR_YELLOW | ANSI_BOLD,
            "    Expected this:\n"
            "\n"
            "        main :: () {\n"
            "        }\n"
            "\n"
            "    This enforcement improves the debugger experience, since the entry function is guaranteed\n"
            "    to have the link name of 'main.main'\n"
            "\n");

        // NOTE: If in the future, we change how namespaces are denoted in the link names, this needs to be updated.
        exit(c, 1);
    }

    c->main_fn = (Node_Fn *) main->definition_spec->assignment_node;
    check_stmt(c, (Node *) main->definition_spec->definition_node);

    const Type_Fn *signature = main->node.type.spec.fn;
    if (signature->args_count) {
        c->main_fn->body = NULL;
        error_node(EK_ERROR, (Node *) c->main_fn, "Function 'main' cannot take any arguments");
        exit(c, 1);
    }

    if (signature->returns_count) {
        c->main_fn->body = NULL;
        error_node(EK_ERROR, (Node *) c->main_fn, "Function 'main' cannot return anything");
        exit(c, 1);
    }

    return c->main_fn;
}

static_assert(COUNT_NODES == 29, "");
void define_orderless_node(Compiler *c, Node *n, const size_t block_start) {
    switch (n->kind) {
    case NODE_IMPORT: {
        Node_Import *import = (Node_Import *) n;
        if (import->is_stmt) {
            make_sure_import_is_ready(c, import);

            bool imported = false;
            for (size_t i = 0; i < n->module->imports.count; i++) {
                if (n->module->imports.data[i]->module == import->module) {
                    imported = true;
                    break;
                }
            }

            if (!imported) {
                if (import->is_local) {
                    context_push_import(&c->context, import);
                } else {
                    da_push(&n->module->imports, import);
                }

                define_orderless_nodes_of_module(c, import->module, &n->token);
                ht_foreach(it, &import->module->globals) {
                    if ((*it.value)->definition_spec->is_private) {
                        continue;
                    }

                    Node_Atom *previous = context_find_define_skipping(&c->context, *it.key, import->module);
                    if (!previous) {
                        previous = module_globals_find_ex(c, n->module, *it.key, import->module);
                        if (previous && previous->definition_spec->is_private && previous->module != n->module) {
                            continue;
                        }
                    }

                    if (previous) {
                        error_redefinition_global(c, (Node *) *it.value, (Node *) previous, n->module, &c->context);
                    }
                }
            }
        }
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;

        Node_Atom *it = NULL;
        while ((it = (Node_Atom *) node_iter((Node *) it, define->name))) {
            if (!sv_match(it->node.token.sv, "_")) {
                if (it->definition_spec->is_local) {
                    it->definition_spec->fn_context = c->context.fn;
                    if (it->definition_spec->is_const) {
                        const Context_Fn *fn = c->context.fn;

                        assert(fn->defines_end <= c->context.defines.count);
                        assert(block_start <= c->context.defines.count);
                        assert(block_start <= fn->defines_end);
                        for (size_t i = fn->defines_end; i > block_start; i--) {
                            Node_Atom *previous = c->context.defines.data[i - 1];
                            if (!previous->definition_spec->is_const) {
                                continue;
                            }

                            if (sv_eq(it->node.token.sv, previous->node.token.sv)) {
                                error_redefinition(c, (Node *) it, &previous->node.token.pos);
                                break;
                            }
                        }

                        context_push_define(&c->context, it);
                    }
                } else {
                    if (get_builtin_type_kind(it->node.token.sv, NULL)) {
                        error_redefinition(c, (Node *) it, NULL);
                    }

                    bool is_method = false;
                    if (it->definition_spec->assignment_node && it->definition_spec->assignment_node->kind == NODE_FN) {
                        Node_Fn *fn = (Node_Fn *) it->definition_spec->assignment_node;
                        is_method = fn->is_method;

                        if (is_method) {
                            da_push(&c->methods_list, fn);
                        }
                    }

                    if (!is_method) {
                        Node_Atom *previous = module_globals_find(c, it->module, it->node.token.sv);
                        if (previous) {
                            error_redefinition_global(c, (Node *) it, (Node *) previous, it->module, &c->context);
                        }
                        global_scope_push(&it->module->globals, it);
                    }
                }

                it->definition_spec->replace_context = c->context.replace;
            }
        }
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            define_orderless_node(c, it, block_start);
        }
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        if (iff->is_compile_time) {
            Node *current_comptime_conditional_stmt_save = c->current_comptime_conditional_stmt;
            c->current_comptime_conditional_stmt = n;

            check_expr(c, iff->condition, REF_NONE);
            type_assert(c, iff->condition, (Type) {.kind = TYPE_BOOL});

            const Const_Value value = eval_const_expr(c, iff->condition, false);
            iff->compile_time_real = !int128_is_zero(value.as.integer) ? iff->consequence : iff->antecedence;

            if (iff->compile_time_real) {
                iff->context_replace.outer = c->context.replace;

                if (iff->compile_time_real == iff->consequence) {
                    if (iff->condition->kind == NODE_BINARY && iff->condition->token.kind == TOKEN_EQ) {
                        Node_Binary *condition = (Node_Binary *) iff->condition;
                        if ((type_is_trait(condition->lhs->type) || type_is_union(condition->lhs->type)) &&
                            condition->lhs->kind == NODE_ATOM) //
                        {
                            if (!node_is_null(condition->rhs)) {
                                push_context_replace(
                                    c,
                                    &iff->context_replace,
                                    ((Node_Atom *) condition->lhs)->definition,
                                    condition->rhs->type);
                            }
                        } else if (
                            (type_is_trait(condition->rhs->type) || type_is_union(condition->rhs->type)) &&
                            condition->rhs->kind == NODE_ATOM) //
                        {
                            if (!node_is_null(condition->lhs)) {
                                push_context_replace(
                                    c,
                                    &iff->context_replace,
                                    ((Node_Atom *) condition->rhs)->definition,
                                    condition->lhs->type);
                            }
                        }
                    }
                }

                if (iff->compile_time_real->kind == NODE_BLOCK) {
                    Node_Block *block = (Node_Block *) iff->compile_time_real;
                    for (Node *it = block->body.head; it; it = it->next) {
                        define_orderless_node(c, it, block_start);
                    }
                } else {
                    define_orderless_node(c, iff->compile_time_real, block_start);
                }

                c->context.replace = iff->context_replace.outer;
            }

            c->current_comptime_conditional_stmt = current_comptime_conditional_stmt_save;
        }
    } break;

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            Node *current_comptime_conditional_stmt_save = c->current_comptime_conditional_stmt;
            c->current_comptime_conditional_stmt = n;

            check_switch_expr_and_alloc_preds(c, sw);
            const Const_Value value = eval_const_expr(c, sw->expr, false);

            size_t iota = 0;
            for (Node *it = sw->cases.head; it; it = it->next) {
                Node_Case *branch = (Node_Case *) it;
                for (Node *pred = branch->preds.head; pred; pred = pred->next) {
                    const Const_Value pred_value = check_switch_pred(c, sw, pred, &iota);
                    if (sw->trait) {
                        assert(value.kind == CONST_VALUE_TRAIT);

                        const Type *pred_type = NULL;
                        if (pred_value.kind == CONST_VALUE_TYPE) {
                            pred_type = &pred_value.as.type;
                        }

                        if (value.as.trait.type) {
                            if (pred_type && type_eq(*value.as.trait.type, *pred_type)) {
                                sw->compile_time_real = branch;
                            }
                        } else {
                            if (!pred_type) {
                                sw->compile_time_real = branch;
                            }
                        }
                    } else if (sw->unionn) {
                        assert(value.kind == CONST_VALUE_UNION);
                        assert(pred_value.kind == CONST_VALUE_INT);
                        if (int128_eq(int128_from_u64(value.as.unionn.index), pred_value.as.integer)) {
                            sw->compile_time_real = branch;
                        }
                    } else if (const_value_eq(pred_value, value)) {
                        sw->compile_time_real = branch;
                    }
                }
            }
            assert(iota == sw->preds_count);

            if (!sw->compile_time_real && sw->fallback) {
                assert(sw->fallback->kind == NODE_CASE);
                sw->compile_time_real = (Node_Case *) sw->fallback;
            }

            Node_Case *branch = sw->compile_time_real;
            if (branch) {
                branch->context_replace.outer = c->context.replace;

                if ((sw->trait || sw->unionn) && sw->expr->kind == NODE_ATOM && branch->preds_count == 1) {
                    if (!node_is_null(branch->preds.head)) {
                        push_context_replace(
                            c,
                            &branch->context_replace,
                            ((Node_Atom *) sw->expr)->definition,
                            branch->preds.head->type);
                    }
                }

                assert(branch->body->kind == NODE_BLOCK);
                Node_Block *block = (Node_Block *) branch->body;
                for (Node *it = block->body.head; it; it = it->next) {
                    define_orderless_node(c, it, block_start);
                }

                c->context.replace = branch->context_replace.outer;
            }

            check_switch_exhaustive(c, sw);
            c->current_comptime_conditional_stmt = current_comptime_conditional_stmt_save;
        }
    } break;

    default:
        // Pass
        break;
    }
}

void define_orderless_nodes_of_module(Compiler *c, Module *module, const Token *unqualified_import_token) {
    switch (module->orderless_check_status) {
    case UNCHECKED:
        module->orderless_check_status = CHECKING;
        for (Node *it = module->nodes.head; it; it = it->next) {
            define_orderless_node(c, it, 0);
        }
        module->orderless_check_status = CHECKED;
        break;

    case CHECKING:
        assert(unqualified_import_token);
        error_token(EK_ERROR, *unqualified_import_token, "Cyclic unqualified import");
        exit(c, 1);
        break;

    case CHECKED:
        // Pass
        break;

    default:
        unreachable();
    }
}

void push_context_replace(Compiler *c, Context_Replace *replace, Node_Atom *from, Type to) {
    replace->from = from;

    // The only definitions which have definitions are ghosts
    while (replace->from->definition) {
        replace->from = replace->from->definition;
    }

    replace->to = arena_clone(&default_arena, from, sizeof(*from));
    replace->to->is_ghost = true;
    replace->to->definition = from;
    replace->to->definition_spec =
        arena_clone(&default_arena, replace->to->definition_spec, sizeof(*replace->to->definition_spec));

    replace->to->node.type = type_without_meta(to);

    // Technically the code generated in the mismatch condition will access invalid memory.
    // However it will be unreachable, so does it even matter?
    if (replace->to->definition_spec->is_const) {
        Const_Value *value = &replace->to->definition_spec->const_value;

        static_assert(COUNT_CONST_VALUES == 12, "");
        switch (value->kind) {
        case CONST_VALUE_TRAIT: {
            const Const_Value_Trait trait = value->as.trait;
            if (trait.type && type_eq(*trait.type, replace->to->node.type)) {
                assert(trait.data);
                *value = *trait.data;
            }
        } break;

        case CONST_VALUE_UNION: {
            const Const_Value_Union unionn = value->as.unionn;
            if (unionn.index && type_eq(unionn.spec->variants[unionn.index - 1].type, replace->to->node.type)) {
                assert(unionn.real);
                *value = *unionn.real;
            }
        } break;

        default:
            unreachable();
        }
    }

    c->context.replace = replace;
}

void check_definition(Compiler *c, Node_Atom *it, Node *it_expr, Node *type) {
    const bool dont_allow_polymorphs_save = c->dont_allow_polymorphs;
    c->dont_allow_polymorphs = false;

    assert(it->definition_spec->check_status != CHECKING); // It is already checked
    if (it->definition_spec->check_status == CHECKED) {
        goto end;
    }
    it->definition_spec->check_status = CHECKING;

    if (type) {
        if (type_kind_eq(type->type, TYPE_UNIT)) {
            check_expr(c, type, REF_NONE);
            type_assert_type(c, type);
        }

        type->type.is_meta = false;
        if (it->polymorph && type_eq(type->type, c->type_info_pointer_type)) {
            it->polymorph->is_type = true;
        }
        it->node.type = type->type;
    }

    if (it_expr) {
        Node_Define *definition = it->definition_spec->definition_node;

        if (type_kind_eq(it_expr->type, TYPE_UNIT)) {
            if (it->definition_spec->arg_index && is_node_caller_location(it_expr)) {
                it_expr->type = c->source_code_location_type;
            } else {
                if (it->definition_spec->is_const) {
                    assert(it_expr);
                    if (it_expr->kind == NODE_DISTINCT) {
                        Node_Distinct *distinct = (Node_Distinct *) it_expr;
                        distinct->defined_as = it;
                    }
                }

                check_expr(c, it_expr, REF_NONE);
                if (!type) {
                    if (it_expr->kind == NODE_GROUP) {
                        Node_Group *group = (Node_Group *) it_expr;
                        ll_foreach(it, &group->nodes) {
                            check_that_type_is_known(c, it);
                        }
                    } else {
                        check_that_type_is_known(c, it_expr);
                    }
                }

                if (it_expr->type.is_meta && !it->definition_spec->is_const) {
                    it_expr->emit_type_info = arena_clone(&default_arena, &it_expr->type, sizeof(it_expr->type));
                    it_expr->emit_type_info->is_meta = false;
                    it_expr->type = c->type_info_pointer_type;
                }

                if (type_kind_eq(it_expr->type, TYPE_MODULE) && !it->definition_spec->is_const) {
                    error_node(
                        EK_ERROR,
                        it_expr,
                        "Cannot store %s in a %s",
                        type_to_cstr(it_expr->type),
                        it->definition_spec->is_const ? "constant" : "variable");
                    exit(c, 1);
                }
            }
        }

        bool type_determined = false;
        if (!definition->is_value_known_at_compile_time) {
            size_t lhs_count = definition->count;
            size_t rhs_count = 1;
            if (type_kind_eq(it_expr->type, TYPE_GROUP)) {
                rhs_count = it_expr->type.spec.group.count;
            }

            if (lhs_count != rhs_count) {
                error_number_of_values_mismatch(
                    (Node *) definition,
                    lhs_count,
                    rhs_count,
                    add_trailing_s_if_plural("definition", lhs_count),
                    add_trailing_s_if_plural("assignment", rhs_count));
            }

            if (type_kind_eq(it_expr->type, TYPE_GROUP)) {
                assert(it->definition_spec->group_index < it_expr->type.spec.group.count);

                if (type) {
                    i64   group_index = -1;
                    Node *n = get_node_from_group(it_expr, it->definition_spec->group_index, &group_index);
                    type_assert_grouped(c, n, type->type, group_index, &it->node.token);
                } else {
                    it->node.type = it_expr->type.spec.group.data[it->definition_spec->group_index];
                    if (type_is_untyped(it->node.type)) {
                        i64   group_index = -1;
                        Node *n = get_node_from_group(it_expr, it->definition_spec->group_index, &group_index);
                        finalize_untyped_type(c, n);
                        it->node.type = n->type;
                    }
                }

                type_determined = true;
            }
        }

        if (!type_determined) {
            if (type) {
                type_assert(c, it_expr, type->type);
            } else {
                if (!it->definition_spec->is_const) {
                    finalize_untyped_type(c, it_expr);
                }
                it->node.type = it_expr->type;
            }
        }
    }

    if (it_expr && it->definition_spec->definition_node->is_value_known_at_compile_time) {
        if (!it->definition_spec->is_const_value_evaluated) {
            it->definition_spec->const_value = eval_const_expr(c, it_expr, false);
            it->definition_spec->is_const_value_evaluated = true;
        }
    }

    if (it->definition_spec->is_local) {
        if (!it->definition_spec->is_const && !sv_match(it->node.token.sv, "_") && !it->definition_spec->polymorph) {
            context_push_define(&c->context, it);
        }
    }

    it->definition_spec->check_status = CHECKED;

end:
    c->dont_allow_polymorphs = dont_allow_polymorphs_save;
}

void check_definition_if_needed(Compiler *c, Node_Atom *definition, Ref_Kind ref) {
    switch (definition->definition_spec->check_status) {
    case UNCHECKED: {
        Context_Fn *context_fn_save = c->context.fn;
        c->context.fn = definition->definition_spec->fn_context;

        Context_Replace *context_replace_save = c->context.replace;
        c->context.replace = definition->definition_spec->replace_context;

        // Only orderless definitions can be uninffered, and the assignment of such definitions must be constant
        assert(definition->definition_spec->definition_node->is_value_known_at_compile_time);

        check_definition(
            c,
            definition,
            definition->definition_spec->assignment_node,
            definition->definition_spec->definition_node->type);

        context_restore_fn(&c->context, context_fn_save);
        c->context.replace = context_replace_save;
    } break;

    case CHECKING:
        if ((ref == REF_ADDR || ref == REF_SLICE) && definition->node.type.is_meta) {
            // Reference to incomplete type definition is allowed
        } else {
            error_node(EK_ERROR, (Node *) definition, "Cyclic definition");
            exit(c, 1);
        }
        break;

    case CHECKED:
        // Pass
        break;
    }
}

void check_ident(Compiler *c, Node *n, Ref_Kind ref) {
    Node_Atom   *atom = NULL;
    Node_Member *member = NULL;

    Module *module = NULL;
    if (n->kind == NODE_ATOM) {
        atom = (Node_Atom *) n;
        module = atom->module;
    } else if (n->kind == NODE_MEMBER) {
        member = (Node_Member *) n;
        assert(member->lhs->type.kind == TYPE_MODULE);
        module = member->lhs->type.spec.module;
    } else {
        unreachable();
    }

    if (sv_match(n->token.sv, "_")) {
        error_token(EK_ERROR, n->token, "Identifier '_' cannot be used as a value");
        exit(c, 1);
    }

    Node_Atom *definition = NULL;
    if (atom) {
        if (atom->definition) {
            definition = atom->definition;
        } else {
            definition = context_find_define(&c->context, n->token.sv);
            if (definition) {
                if (definition->polymorph) {
                    if (!definition->polymorph->is_monomorphized) {
                        if (definition->definition_spec &&                              //
                            definition->definition_spec->fn_context && c->context.fn && //
                            definition->definition_spec->fn_context != c->context.fn)   //
                        {
                            // The polymorph is being accessed before it was monomorphized. That means the
                            // access occurs in the signature itself
                            error_node(
                                EK_ERROR,
                                n,
                                "Cannot use polymorphic parameters of outer function before they are monomorphized");
                            error_node(EK_NOTE, (Node *) definition, "Here is the polymorphic parameter being used");
                            exit(c, 1);
                        }
                    }
                } else if (definition->definition_spec->fn_context && c->context.fn) {
                    if (definition->definition_spec->fn_context != c->context.fn &&
                        !definition->definition_spec->is_const && !definition->definition_spec->static_var_fn) //
                    {
                        error_node(EK_ERROR, n, "Cannot use variable from stack frame of outer function");
                        error_node(EK_NOTE, (Node *) definition, "Here is the variable being used");
                        exit(c, 1);
                    }
                }
            }
        }
    }

    if (!definition && c->monomorphization_stack.count) {
        const Monomorphization m = c->monomorphization_stack.data[c->monomorphization_stack.count - 1];
        if (m.into->kind == NODE_STRUCT) {
            Node_Struct *structt = (Node_Struct *) m.into;
            ll_foreach(it, &structt->monomorphs) {
                if (sv_eq(it->name->node.token.sv, n->token.sv)) {
                    definition = it->name;
                    break;
                }
            }
        }
    }

    if (!definition) {
        if (atom) {
            Type_Kind builtin_type_kind;
            if (get_builtin_type_kind(n->token.sv, &builtin_type_kind)) {
                n->type = (Type) {.kind = builtin_type_kind, .is_meta = true};
                if (ref == REF_ASSIGN) {
                    error_node(EK_ERROR, n, "Cannot assign to compile time constant value");
                    exit(c, 1);
                }
                return;
            }
        }

        definition = module_globals_find(c, module, n->token.sv);
        if (!definition && atom) {
            if (module != c->builtin_module) {
                module = c->builtin_module;
                definition = module_globals_find(c, module, n->token.sv);
            }
        }

        if (definition && definition->definition_spec->is_private && definition->module != n->module) {
            definition = NULL;
        }
    }

    if (definition) {
        for (Context_Replace *it = c->context.replace; it; it = it->outer) {
            if (it->from == definition) {
                definition = it->to;
                break;
            }
        }
    }

    if (atom) {
        atom->definition = definition;
    } else if (member) {
        member->module_access_definition = definition;
    }

    if (definition) {
        if (definition->polymorph) {
            if (definition->polymorph->is_monomorphized) {
                n->type = definition->polymorph->node.type;
            } else {
                n->type = definition->node.type;
                n->type.spec.polymorph.is_definition = false;
            }

            if (definition->polymorph->is_type) {
                n->type.is_meta = true;
            }
        } else {
            check_definition_if_needed(c, definition, ref);
            n->type = definition->node.type;
            n->is_memory = !definition->definition_spec->is_const;
        }

        if (!n->is_memory) {
            switch (ref) {
            case REF_NONE:
            case REF_SLICE:
                // OK
                break;

            case REF_ADDR:
                if (!n->type.is_meta) {
                    error_node(EK_ERROR, n, "Cannot take reference to compile time constant value");
                    error_node(EK_NOTE, (Node *) definition, "Here is the constant being used");
                    exit(c, 1);
                }
                break;

            case REF_ADDR_MEMBER:
                if (!n->type.is_meta && !type_kind_eq(definition->node.type, TYPE_MODULE)) {
                    error_node(EK_ERROR, n, "Cannot take reference to compile time constant value");
                    error_node(EK_NOTE, (Node *) definition, "Here is the constant being used");
                    exit(c, 1);
                }
                break;

            case REF_ASSIGN:
                error_node(EK_ERROR, n, "Cannot assign to compile time constant value");
                error_node(EK_NOTE, (Node *) definition, "Here is the constant being used");
                exit(c, 1);
                break;

            case REF_ASSIGN_MEMBER:
                if (!type_kind_eq(definition->node.type, TYPE_MODULE)) {
                    error_node(EK_ERROR, n, "Cannot assign to compile time constant value");
                    error_node(EK_NOTE, (Node *) definition, "Here is the constant being used");
                    exit(c, 1);
                }
                break;
            }
        }
    } else {
        error_undefined(c, &n->token, "identifier", false);
    }
}
