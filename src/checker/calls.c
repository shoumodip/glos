#include "../error.h"
#include "checker.h"

static const char *
fn_type_to_cstr_but_excluding_receiver_if_required(const Type_Fn *fn_spec_raw, bool exclude_receiver) {
    Type_Fn spec = *fn_spec_raw;
    if (exclude_receiver) {
        assert(spec.args_count);
        spec.args++;
        spec.args_count--;
        if (spec.args_count_min) spec.args_count_min--;
        if (spec.variadics_index) spec.variadics_index--;
    }

    return type_to_cstr((Type) {.kind = TYPE_FN, .spec.fn = &spec});
}

static void show_note_about_the_function_being_called(Node *fn, bool is_method, const Type_Fn *fn_spec) {
    if (!fn_spec) {
        return;
    }

    const char *label = is_method ? "method" : "function";

    Node_Fn *literal = get_function_literal(fn);
    Node    *literal_body = NULL;
    if (literal) {
        literal_body = literal->body;
        literal->body = NULL;
    }

    error_node(
        EK_NOTE,
        literal ? (Node *) literal : fn,
        "The %s being called has signature %s",
        label,
        fn_type_to_cstr_but_excluding_receiver_if_required(fn_spec, is_method));

    if (literal) {
        literal->body = literal_body;
    }
}

static void show_error_for_uninferred_polymorphic_parameter_in_call(
    Compiler *c, Nodes args, const Type_Fn *fn_spec, Node_Polymorph *polymorph) //
{
    size_t it_index = 0;
    ll_foreach(arg, &args) {
        Node *it = arg;

        const bool is_named = it->kind == NODE_BINARY && it->token.kind == TOKEN_SET;
        if (is_named) {
            it_index = it->token.as.integer;
            it = ((Node_Binary *) it)->rhs;
        }

        const bool is_spread = it->kind == NODE_UNARY && it->token.kind == TOKEN_SPREAD;
        if (is_spread) {
            it = ((Node_Unary *) it)->value;
        }

        const size_t parts = type_kind_eq(it->type, TYPE_GROUP) ? it->type.spec.group.count : 1;
        for (size_t i = 0; i < parts; i++) {
            if (it_index == polymorph->arg_index) {
                Type *expected = &fn_spec->args[it_index].type;
                if (!is_named && !is_spread &&                                                          //
                    fn_spec->variadics_kind == VARIADICS_TYPED && it_index >= fn_spec->variadics_index) //
                {
                    // It is not named, therefore it must be a variadic parameter
                    expected = &fn_spec->args[fn_spec->variadics_index].type;
                    assert(type_kind_eq(*expected, TYPE_SLICE));
                    expected = expected->spec.slice.element;
                }

                if (check_that_type_is_known_noexit(it)) {
                    if (parts == 1) {
                        type_assert_noexit(c, it, *expected);
                    } else {
                        type_assert_grouped_noexit(c, it, *expected, i, NULL);
                    }
                }
                return;
            }
            it_index++;
        }
    }

    if (fn_spec->variadics_kind == VARIADICS_TYPED && polymorph->arg_index == fn_spec->variadics_index) {
        error_node(
            EK_ERROR,
            (Node *) polymorph,
            "Cannot infer the type of polymorphic parameter '" SV_Fmt "' because no variadic arguments were provided",
            SV_Arg(polymorph->name->node.token.sv));
    }
}

void check_call_arity(
    Compiler *c,
    Node     *fn,
    size_t    args_count,
    Token     end,
    bool      is_method,
    size_t    args_count_min,
    size_t    args_count_max,
    Node     *excess_argument) //
{
    if (args_count < args_count_min) {
        error_token(
            EK_ERROR,
            end,
            "Too few arguments: Expected%s %zu, got %zu",
            args_count_min == args_count_max ? "" : " at least",
            args_count_min - is_method,
            args_count - is_method);

        if (!fn->type.is_meta) {
            show_note_about_the_function_being_called(fn, is_method, fn->type.spec.fn);
        }
        exit(c, 1);
    }

    if (args_count > args_count_max) {
        error_node(
            EK_ERROR,
            excess_argument,
            "Too many arguments: Expected %zu, got %zu",
            args_count_max - is_method,
            args_count - is_method);

        if (!fn->type.is_meta) {
            show_note_about_the_function_being_called(fn, is_method, fn->type.spec.fn);
        }
        exit(c, 1);
    }
}

void check_call_arguments(Compiler *c, Call_Checker *cc, bool check_arguments_provided) {
    assert(type_kind_eq(cc->fn->type, TYPE_FN));
    const Type_Fn *fn_spec = cc->fn->type.spec.fn;

    typedef struct {
        Node *node;
    } Argument;

    Argument *args = NULL;
    size_t    args_count_min = 1;
    size_t    args_count_max = 1;

    args = arena_alloc(&temp_arena, fn_spec->args_count * sizeof(*args));
    args_count_min = fn_spec->args_count_min;
    args_count_max = fn_spec->variadics_kind != VARIADICS_NONE ? UINT64_MAX : fn_spec->args_count;

    if (cc->is_method || cc->is_trait) {
        assert(cc->receiver);
        if (!cc->receiver->type.is_meta) {
            args[cc->args_count++].node = cc->receiver;
        }
    }

    if (!cc->is_polymorph) {
        cc->is_polymorph = node_is_runtime_polymorphic_expression(cc->fn);
    }

    const size_t monomorph_parameters_begin_save = c->monomorph_parameters.begin;
    if (cc->is_polymorph) {
        ht_clear(&c->monomorph_replacements);
        c->monomorph_parameters.begin = c->monomorph_parameters.count;
    }

    typedef enum {
        VS_NONE,
        VS_ARGS,
        VS_DIRECT, // For named and spread
    } VS_Kind;

    // This is for typed variadics
    Node   *variadic_source = NULL;
    VS_Kind variadic_source_kind = VS_NONE;

    Node *excess_argument = NULL;
    ll_foreach(arg, &cc->args) {
        Node  *it = arg;
        Node  *it_name = NULL;
        size_t it_index = cc->args_count;
        if (it->kind == NODE_BINARY && it->token.kind == TOKEN_SET) {
            Node_Binary *it_binary = (Node_Binary *) it;
            it = it_binary->rhs;
            it_name = it_binary->lhs;

            bool ok = false;
            for (size_t i = 0; i < fn_spec->args_count; i++) {
                const Type_Fn_Arg *arg = &fn_spec->args[i];
                if (sv_eq(arg->name, it_name->token.sv)) {
                    it_index = i;
                    ok = true;
                    break;
                }
            }

            if (!ok) {
                error_undefined(c, &it_name->token, "argument", true);
                show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                exit(c, 1);
            }

            arg->token.as.integer = it_index;
        }

        const bool is_spread = it->kind == NODE_UNARY && it->token.kind == TOKEN_SPREAD;
        if (is_spread) {
            if (fn_spec->variadics_kind != VARIADICS_TYPED) {
                error_node(
                    EK_ERROR, it, "Cannot spread arguments in a call to a function that does not have typed variadics");
                show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                exit(c, 1);
            }

            Node_Unary *unary = (Node_Unary *) it;
            it = unary->value;
            check_expr(c, it, REF_NONE);
        } else {
            check_expr(c, it, REF_NONE);
        }

        const size_t parts = type_kind_eq(it->type, TYPE_GROUP) ? it->type.spec.group.count : 1;
        for (size_t i = 0; i < parts; i++, it_index++) {
            if (it_index >= args_count_max) {
                if (!excess_argument) {
                    excess_argument = arg;
                }
                continue;
            }

            if (fn_spec->variadics_kind == VARIADICS_UNTYPED && it_index >= fn_spec->args_count) {
                continue;
            }

            if (fn_spec->variadics_kind == VARIADICS_TYPED && it_index >= fn_spec->variadics_index) {
                if (!it_name) {
                    // This is a positional argument, therefore it must be variadic
                    it_index = fn_spec->variadics_index;
                }
            }

            if (fn_spec->variadics_kind == VARIADICS_TYPED && it_index == fn_spec->variadics_index) {
                if (variadic_source) {
                    if (it_name && variadic_source->kind == NODE_BINARY && variadic_source->token.kind == TOKEN_SET) {
                        error_node(
                            EK_ERROR,
                            arg,
                            "Duplication of argument '" SV_Fmt "'",
                            SV_Arg(fn_spec->args[it_index].name));
                        error_node(EK_NOTE, variadic_source, "Passed here already");
                        show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                        exit(c, 1);
                    }

                    if (is_spread || it_name || variadic_source_kind == VS_DIRECT) {
                        error_node(EK_ERROR, cc->expr, "Multiple typed variadic sources found");

                        bool is_variadic_source_direct = false;
                        if (variadic_source->kind == NODE_UNARY && variadic_source->token.kind == TOKEN_SPREAD) {
                            is_variadic_source_direct = true;
                        } else if (variadic_source->kind == NODE_BINARY && variadic_source->token.kind == TOKEN_SET) {
                            is_variadic_source_direct = true;
                        }

                        error_node(
                            EK_NOTE,
                            variadic_source,
                            "This %s one source",
                            is_variadic_source_direct ? "provides" : "starts");
                        error_node(EK_NOTE, arg, "This provides another");
                        show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                        exit(c, 1);
                    }

                    if (it->kind == NODE_INTERPOLATION) {
                        Node_Interpolation *interpolation = (Node_Interpolation *) it;
                        interpolation->do_not_allocate = true;
                        if (interpolation->children_count > 1) {
                            interpolation->children_count++; // The marker
                        }
                        cc->typed_variadics_count += interpolation->children_count;
                    } else {
                        cc->typed_variadics_count++;
                    }
                } else {
                    variadic_source = arg;

                    assert(variadic_source_kind == VS_NONE);
                    if (is_spread || it_name) {
                        variadic_source_kind = VS_DIRECT;
                        cc->is_typed_variadics_direct = true;
                    } else {
                        variadic_source_kind = VS_ARGS;
                        if (it->kind == NODE_INTERPOLATION) {
                            Node_Interpolation *interpolation = (Node_Interpolation *) it;
                            interpolation->do_not_allocate = true;
                            if (interpolation->children_count > 1) {
                                interpolation->children_count++; // The marker
                            }
                            cc->typed_variadics_count += interpolation->children_count;
                        } else {
                            cc->typed_variadics_count++;
                        }
                    }
                }
                continue;
            }

            if (is_spread) {
                error_node(EK_ERROR, arg, "Cannot spread arguments at this position");
                show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                exit(c, 1);
            }

            Argument *argument = &args[it_index];
            if (argument->node) {
                error_node(EK_ERROR, arg, "Duplication of argument '" SV_Fmt "'", SV_Arg(fn_spec->args[it_index].name));
                error_node(EK_NOTE, argument->node, "Passed here already");
                show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                exit(c, 1);
            }

            argument->node = arg;
        }

        cc->args_count += parts;
    }

    // Check that proper number of arguments is provided
    if (check_arguments_provided) {
        check_call_arity(
            c, cc->fn, cc->args_count, cc->end, cc->is_method, args_count_min, args_count_max, excess_argument);

        size_t not_provided_count = 0;
        SV     not_provided_name = {0};
        for (size_t i = 0; i < fn_spec->args_count; i++) {
            if (fn_spec->variadics_kind == VARIADICS_TYPED && i == fn_spec->variadics_index) {
                continue;
            }

            const Type_Fn_Arg *it = &fn_spec->args[i];
            if (!args[i].node && !it->has_default_value) {
                not_provided_count++;
                if (not_provided_count == 1) {
                    not_provided_name = it->name;
                } else if (not_provided_count == 2) {
                    error_token_begin(EK_ERROR, cc->end);
                    fprintf(
                        stderr,
                        "The following arguments are not provided: " SV_Fmt ", " SV_Fmt,
                        SV_Arg(not_provided_name),
                        SV_Arg(it->name));
                } else {
                    fprintf(stderr, ", " SV_Fmt, SV_Arg(it->name));
                }
            }
        }

        if (not_provided_count) {
            if (not_provided_count == 1) {
                error_token(EK_ERROR, cc->end, "Argument '" SV_Fmt "' is not provided", SV_Arg(not_provided_name));
            } else {
                error_finalize();
            }

            show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
            exit(c, 1);
        }
    }

    if (cc->is_polymorph) {
        if (cc->is_method) {
            const Type expected = type_with_ref(fn_spec->args[0].type, cc->receiver->type.ref);
            if (!infer_monomorph_parameters(c, cc->receiver, &cc->receiver->type, &expected)) {
                error_node(EK_NOTE, cc->expr, "While attempting to monomorphize this");
                show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                exit(c, 1);
            }
        }

        Nodes  final_args = {0};
        size_t final_args_count = 0;

        size_t it_index = cc->is_method || cc->is_trait;
        ll_foreach(arg, &cc->args) {
            Node *it = arg;

            const bool is_spread = it->kind == NODE_UNARY && it->token.kind == TOKEN_SPREAD;
            if (is_spread) {
                it = ((Node_Unary *) it)->value;
            }

            const bool is_named = it->kind == NODE_BINARY && it->token.kind == TOKEN_SET;
            if (is_named) {
                it_index = it->token.as.integer;
                it = ((Node_Binary *) it)->rhs;
            }

            Type  *types = &it->type;
            size_t count = 1;
            if (type_kind_eq(it->type, TYPE_GROUP)) {
                Type_Group *group = &it->type.spec.group;
                types = group->data;
                count = group->count;
            }

            for (size_t i = 0; i < count; i++, it_index++) {
                Type_Fn_Arg *argument = it_index < fn_spec->args_count ? &fn_spec->args[it_index] : NULL;
                bool         add_to_final_args = true;
                if (argument && argument->polymorph) {
                    if (argument->polymorph->is_type) {
                        if (!type_assert_type_or_Type_noexit(c, it)) {
                            error_node(EK_NOTE, cc->expr, "While attempting to monomorphize this");
                            show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                            exit(c, 1);
                        }

                        Const_Value value = eval_const_expr(c, it, false);
                        if (value.kind == CONST_VALUE_TYPE) {
                            value.as.type.is_meta = false;
                        } else {
                            assert(value.kind == CONST_VALUE_INT && int128_is_zero(value.as.integer));
                            error_node(
                                EK_ERROR, it, "This expression is not a constant type (It is a null RTTI pointer)");
                            error_node(EK_NOTE, cc->expr, "While attempting to monomorphize this");
                            exit(c, 1);
                        }
                        add_monomorph_parameter(c, argument->polymorph, it->type, value, NULL);
                    } else {
                        if (!type_assert_noexit(c, it, argument->type)) {
                            error_node(EK_NOTE, cc->expr, "While attempting to monomorphize this");
                            show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                            exit(c, 1);
                        }

                        add_monomorph_parameter(c, argument->polymorph, it->type, eval_const_expr(c, it, false), NULL);
                    }

                    assert(count == 1); // This is guaranteed to be a singular value.
                    add_to_final_args = false;
                } else {
                    Type *expected = argument ? &argument->type : NULL;
                    if (!is_named && !is_spread &&                                                          //
                        fn_spec->variadics_kind == VARIADICS_TYPED && it_index >= fn_spec->variadics_index) //
                    {
                        // It is not named, therefore it must be a variadic parameter
                        expected = &fn_spec->args[fn_spec->variadics_index].type;
                        assert(type_kind_eq(*expected, TYPE_SLICE));
                        expected = expected->spec.slice.element;
                    }

                    if (!infer_monomorph_parameters(c, it, &types[i], expected)) {
                        error_node(EK_NOTE, cc->expr, "While attempting to monomorphize this");
                        show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                        exit(c, 1);
                    }
                }

                if (add_to_final_args) {
                    nodes_push(&final_args, arg);
                    final_args_count++;
                }
            }
        }

        if (final_args_count != cc->args_count) {
            // Terminate at the last argument
            if (final_args.tail) {
                final_args.tail->next = NULL;
            }

            cc->args = final_args;
            cc->args_count = final_args_count;
        }

        for (size_t i = 0; i < fn_spec->args_count; i++) {
            Type_Fn_Arg *arg = &fn_spec->args[i];
            if (arg->polymorph && arg->has_default_value) {
                add_monomorph_parameter_default_value(
                    c,
                    arg->polymorph,
                    arg->type,
                    arg->default_value,
                    arg->default_value_is_caller_location ? cc->expr : NULL);
            }
        }

        if (c->monomorph_parameters.count - c->monomorph_parameters.begin != fn_spec->polymorphs_count) {
            for (size_t i = c->monomorph_parameters.begin; i < c->monomorph_parameters.count; i++) {
                Monomorph_Parameter it = c->monomorph_parameters.data[i];
                it.from->is_monomorphized = true;

                // It does not matter that we mutate these, since we are about to die anyway
                it.from->monomorphization_value = it.value;
            }

            bool *shown = arena_alloc(&temp_arena, fn_spec->args_count * sizeof(*shown));
            for (size_t i = 0; i < fn_spec->polymorphs_count; i++) {
                Node_Polymorph *it = fn_spec->polymorphs[i];
                if (shown[it->arg_index]) {
                    continue;
                }

                if (!it->is_monomorphized) {
                    show_error_for_uninferred_polymorphic_parameter_in_call(c, cc->args, fn_spec, it);
                    shown[it->arg_index] = true;
                }
            }

            error_node(EK_NOTE, cc->expr, "While attempting to monomorphize this");
            show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
            exit(c, 1);
        }

        cc->fn = monomorphize(c, cc->fn, cc->expr);
        fn_spec = cc->fn->type.spec.fn;

        if (cc->is_method) {
            // 'fn_source' and 'receiver' are same in case of traits
            if (cc->fn_source != cc->receiver) {
                cc->fn_source->type = cc->fn->type;
            }
        }
    }

    // Check the argument types
    {
        if (cc->is_method) {
            type_assert(c, cc->receiver, type_with_ref(fn_spec->args[0].type, cc->receiver->type.ref));
        }

        size_t it_index = cc->is_method || cc->is_trait;
        ll_foreach(arg, &cc->args) {
            Node      *it = arg;
            const bool is_spread = it->kind == NODE_UNARY && it->token.kind == TOKEN_SPREAD;
            if (is_spread) {
                it = ((Node_Unary *) it)->value;
            }

            const bool is_named = it->kind == NODE_BINARY && it->token.kind == TOKEN_SET;
            if (is_named) {
                it_index = it->token.as.integer;
                it = ((Node_Binary *) it)->rhs;

                if (!type_assert_noexit(c, it, fn_spec->args[it_index].type)) {
                    if (cc->is_polymorph) {
                        error_node(EK_NOTE, cc->expr, "While attempting to monomorphize this");
                    }
                    show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                    exit(c, 1);
                }
            } else {
                const size_t parts = type_kind_eq(it->type, TYPE_GROUP) ? it->type.spec.group.count : 1;
                for (size_t i = 0; i < parts; i++) {
                    Type *expected = it_index < fn_spec->args_count ? &fn_spec->args[it_index].type : NULL;
                    if (fn_spec->variadics_kind == VARIADICS_TYPED && it_index >= fn_spec->variadics_index) {
                        if (!is_spread && !is_named) {
                            // This is a positional argument, therefore it must be variadic
                            it_index = fn_spec->variadics_index;

                            expected = &fn_spec->args[it_index].type;
                            assert(type_kind_eq(*expected, TYPE_SLICE));
                            expected = expected->spec.slice.element;
                        }
                    }

                    if (expected) {
                        bool ok = true;
                        if (parts == 1) {
                            bool pass = false;
                            if (it->kind == NODE_INTERPOLATION &&             //
                                fn_spec->variadics_kind == VARIADICS_TYPED && //
                                it_index == fn_spec->variadics_index)         //
                            {
                                Type *type = &fn_spec->args[it_index].type;
                                assert(!type->ref && type_kind_eq(*type, TYPE_SLICE));
                                pass = type_eq(*type->spec.slice.element, c->any_type);
                            }

                            if (!pass) {
                                ok = type_assert_noexit(c, it, *expected);
                            }
                        } else {
                            ok = type_assert_grouped_noexit(c, it, *expected, i, NULL);
                        }

                        if (!ok) {
                            if (cc->is_polymorph) {
                                error_node(EK_NOTE, cc->expr, "While attempting to monomorphize this");
                            }
                            show_note_about_the_function_being_called(cc->fn, cc->is_method, fn_spec);
                            exit(c, 1);
                        }
                    }

                    it_index++;
                }
            }
        }
    }

    c->monomorph_parameters.count = c->monomorph_parameters.begin;
    c->monomorph_parameters.begin = monomorph_parameters_begin_save;
    arena_reset(&temp_arena, args);
}
