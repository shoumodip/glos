#include "../error.h"
#include "checker.h"

static_assert(COUNT_TOKENS == 86, "");
SV token_kind_to_operator_method_name(Token_Kind kind) {
    switch (kind) {
    case TOKEN_ADD:
    case TOKEN_ADD_SET:
        return OPERATOR_ADD;

    case TOKEN_SUB:
    case TOKEN_SUB_SET:
        return OPERATOR_SUB;

    case TOKEN_MUL:
    case TOKEN_MUL_SET:
        return OPERATOR_MUL;

    case TOKEN_DIV:
    case TOKEN_DIV_SET:
        return OPERATOR_DIV;

    case TOKEN_MOD:
    case TOKEN_MOD_SET:
        return OPERATOR_MOD;

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_EQ:
    case TOKEN_NE:
        return OPERATOR_CMP;

    case TOKEN_OPERATOR_CMP:
        return OPERATOR_CMP;

    case TOKEN_OPERATOR_INDEX:
        return OPERATOR_INDEX;

    case TOKEN_OPERATOR_SLICE:
        return OPERATOR_SLICE;

    default:
        unreachable();
    }
}

void check_that_methods_can_be_accessed(Compiler *c, Node *receiver) {
    if (c->methods_list.count && !type_kind_eq(receiver->type, TYPE_MODULE)) {
        error_node(EK_ERROR, receiver, "Cannot access methods at this stage of compilation yet");
        if (c->current_comptime_conditional_stmt) {
            error_node(EK_NOTE, c->current_comptime_conditional_stmt, "Evaluating this conditional statement");
            afprintf(
                stderr,
                ANSI_COLOR_YELLOW | ANSI_BOLD,
                "    The '#if' statements are evaluated immediately instead of waiting for all the definitions\n"
                "    to be registered. Therefore at this point of time, the methods might not be defined yet.\n"
                "    Thus, to prevent inconsistent behaviour, any operations involving them are disallowed.\n\n");
        }
        exit(c, 1);
    }
}

bool get_method_spec(
    Compiler    *c,
    Node        *receiver_node,
    Type         receiver_type,
    SV           name,
    Method_Spec *spec,
    Module      *defining_in_module,
    bool        *is_named) //
{
    if (spec) {
        spec->name = name;
    }

    if (type_kind_eq(receiver_type, TYPE_ENUM)) {
        Node_Enum *definition = receiver_type.spec.enumm.definition;
        if (spec) {
            spec->uid = (uintptr_t) definition;
        }

        if (defining_in_module) {
            if (is_named) {
                *is_named = definition->defined_as != NULL;
            }

            return defining_in_module == definition->module;
        }

        check_that_methods_can_be_accessed(c, receiver_node);
        return true;
    } else if (type_kind_eq(receiver_type, TYPE_UNION)) {
        Node_Union *definition = receiver_type.spec.unionn->definition;
        if (spec) {
            spec->uid = (uintptr_t) definition;
        }

        if (defining_in_module) {
            if (is_named) {
                *is_named = definition->defined_as != NULL;
            }

            return defining_in_module == definition->node.module;
        }

        check_that_methods_can_be_accessed(c, receiver_node);
        return true;
    } else if (type_kind_eq(receiver_type, TYPE_STRUCT)) {
        Node_Struct *definition = receiver_type.spec.structt->original_definition;
        if (spec) {
            spec->uid = (uintptr_t) definition;
        }

        if (defining_in_module) {
            if (is_named) {
                *is_named = definition->defined_as != NULL;
            }

            return defining_in_module == definition->node.module;
        }

        check_that_methods_can_be_accessed(c, receiver_node);
        return true;
    } else if (receiver_type.distinct) {
        if (spec) {
            spec->uid = (uintptr_t) receiver_type.distinct;
        }

        if (defining_in_module) {
            if (is_named) {
                *is_named = true;
            }

            return defining_in_module == receiver_type.distinct->node.module;
        }

        check_that_methods_can_be_accessed(c, receiver_node);
        return true;
    }

    static const uint8_t builtin_type_kinds[COUNT_TYPES];
    for (Type_Kind kind = 0; kind < COUNT_TYPES; kind++) {
        if (type_kind_eq(receiver_type, kind)) {
            if (spec) {
                spec->uid = (uintptr_t) &builtin_type_kinds[kind];
            }

            if (defining_in_module) {
                if (is_named) {
                    *is_named = true;
                }

                return defining_in_module == c->builtin_module;
            }

            check_that_methods_can_be_accessed(c, receiver_node);
            return true;
        }
    }

    return false;
}

Node_Fn *get_method(Compiler *c, Method_Spec spec, Module *module) {
    Node_Fn **fn = ht_get(&c->methods_table, spec);
    if (!fn) {
        return NULL;
    }

    Node_Fn *method = *fn;
    assert(method->defined_as);

    if (method->node.module != module && method->defined_as->definition_spec->is_private) {
        return NULL;
    }

    if (method->node.type.kind != TYPE_FN) {
        check_definition_if_needed(c, method->defined_as, NULL, REF_NONE);
    }

    return method;
}

typedef enum {
    OMS_ARITH = 1,
    OMS_CMP,
    OMS_INDEX,
    OMS_SLICE,
} OMS;

static void pretty_print_oms(SV name, OMS oms, const Type *receiver, bool partial_comparison_acceptable) {
    fprintf(stderr, "        operator " SV_Fmt " :: ", SV_Arg(name));

    const char *T = NULL;
    switch (oms) {
    case OMS_ARITH:
        T = type_to_cstr_raw(type_without_ref(*receiver));
        fprintf(stderr, "(this: %s, that: %s) -> %s", T, T, T);
        break;

    case OMS_CMP:
        T = type_to_cstr_raw(type_without_ref(*receiver));
        if (partial_comparison_acceptable) {
            fprintf(stderr, "(this: %s, that: %s) -> Ordering | Equivalence", T, T);
        } else {
            fprintf(stderr, "(this: %s, that: %s) -> Ordering", T, T);
        }
        break;

    case OMS_INDEX:
        T = type_to_cstr_raw(*receiver);
        fprintf(stderr, "(this: %s, key: K, assign: bool) -> &V", T);
        break;

    case OMS_SLICE:
        T = type_to_cstr_raw(*receiver);
        fprintf(stderr, "(this: %s, begin: A, end: A) -> V", T);
        break;
    }
    fprintf(stderr, " {}\n\n");

    if (oms == OMS_CMP && partial_comparison_acceptable) {
        fprintf(
            stderr,
            "    Return 'Ordering' if you want this method to implement both equality checking as well as ordered comparisons.\n"
            "    Otherwise return 'Equivalence' to implement just equality checking. Do NOT return 'Ordering | Equivalence' literally.\n\n");
    }

    fprintf(
        stderr,
        "    It may have other optional arguments at the end, but this is the bare minimum that must be implemented.\n"
        "\n");

    ansi_reset(stderr);
}

Node_Fn *get_operator_overload(Compiler *c, SV operator, Node *receiver, Node *op, Module *module) {
    return get_operator_overload_ex(c, operator, receiver->type, op, module, true, true, receiver, -1);
}

Node_Fn *get_operator_overload_ex(
    Compiler *c,
    SV        operator,
    Type      receiver,
    Node     *op,
    Module   *module,
    bool      monomorphize_if_needed,
    bool      partial_comparison_acceptable,
    Node     *n,
    i64       group_index) //
{
    unused(partial_comparison_acceptable);
    Method_Spec spec = {0};
    if (get_method_spec(c, n, receiver, operator, &spec, NULL, NULL)) {
        if (sv_eq(operator, OPERATOR_MUL) || sv_eq(operator, OPERATOR_DIV) || sv_eq(operator, OPERATOR_MOD)) {
            if (type_is_pointer(receiver)) {
                error_node(EK_ERROR, n, "The operator '" SV_Fmt "' is not valid for pointers", SV_Arg(operator));
                if (group_index == -1) {
                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "    The value is of type %s\n\n",
                        type_to_cstr(receiver));
                } else {
                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "    The %zu%s value of this expression has type %s. The type of this entire expression is %s\n\n",
                        group_index + 1,
                        order_postfix(group_index + 1),
                        type_to_cstr(receiver),
                        type_to_cstr(n->type));
                }
                exit(c, 1);
            }
        }

        Node_Fn *method = get_method(c, spec, module);
        if (method) {
            if (method->polymorphs.count && monomorphize_if_needed) {
                Call_Checker cc = {0};
                cc.expr = op;
                cc.fn_source = op;
                cc.fn = (Node *) method;
                cc.end = op->token;

                cc.is_method = true;
                cc.receiver = n;
                cc.is_polymorph = true;

                check_call_arguments(c, &cc, false);
                assert(cc.fn->kind == NODE_FN);

                method = (Node_Fn *) cc.fn;
            }

            if (sv_eq(spec.name, OPERATOR_CMP)) {
                if (!partial_comparison_acceptable && !method->is_compare_operator_complete) {
                    assert(method->returns.head);
                    error_node(EK_ERROR, n, "Type %s does not implement ordered comparisons", type_to_cstr(receiver));
                    error_node(
                        EK_NOTE,
                        method->returns.head,
                        "The method '" SV_Fmt "' only implements equality checking since its return type is %s, not %s",
                        SV_Arg(method->defined_as->node.token.sv),
                        type_to_cstr(c->equivalence_type),
                        type_to_cstr(c->ordering_type));
                    exit(c, 1);
                }
            }
            return method;
        }
    }

    check_that_type_is_known(c, n);
    error_node_begin(EK_ERROR, op);

    OMS oms = 0;
    if (sv_eq(spec.name, OPERATOR_ADD)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_SUB)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_MUL)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_DIV)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_MOD)) {
        oms = OMS_ARITH;
    } else if (sv_eq(spec.name, OPERATOR_CMP)) {
        oms = OMS_CMP;
    } else if (sv_eq(spec.name, OPERATOR_INDEX)) {
        oms = OMS_INDEX;
    } else if (sv_eq(spec.name, OPERATOR_SLICE)) {
        oms = OMS_SLICE;
    } else {
        unreachable();
    }
    fprintf(stderr, "Operator '" SV_Fmt "' is not defined for %s", SV_Arg(spec.name), type_to_cstr(receiver));

    if (group_index != -1) {
        const char *postfix = order_postfix(group_index + 1);
        fprintf(
            stderr,
            ", which is the %zd%s value of this expression. The type of the entire expression is %s.",
            group_index + 1,
            postfix,
            type_to_cstr(n->type));
    }

    error_finalize();
    ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
    fprintf(stderr, "    Implement this:\n\n");
    pretty_print_oms(spec.name, oms, &receiver, partial_comparison_acceptable);
    exit(c, 1);
}

static void error_operator_method_wrong_signature(Token name, OMS oms, const Type *receiver) {
    error_token(
        EK_ERROR,
        name,
        "The method '" SV_Fmt "' is special because it implements an operator overload",
        SV_Arg(name.sv));

    ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
    fprintf(stderr, "    It should have this signature:\n\n");
    pretty_print_oms(name.sv, oms, receiver, true);
}

static void check_operator_method_signature_args_count(
    Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec, const size_t args_count, OMS oms) {
    const Type *receiver = &fn_spec->args[0].type;
    if (fn_spec->args_count < args_count) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(
            EK_NOTE, fn->args_end_token, "Expected at least %zu arguments, got %zu", args_count, fn_spec->args_count);
        exit(c, 1);
    }

    for (size_t i = 0; i < fn_spec->args_count; i++) {
        const Type_Fn_Arg *it = &fn_spec->args[i];
        if (!it->has_default_value && i >= args_count) {
            error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
            error_parts(
                EK_NOTE,
                fn_spec->args[i].name,
                fn_spec->args[i].pos,
                "All arguments after the %zu%s argument must have a default value",
                (size_t) args_count,
                order_postfix(args_count));
            exit(c, 1);
        }
    }
}

void check_signature_of_arithmetic_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    const Type *receiver = &fn_spec->args[0].type;

    const OMS oms = OMS_ARITH;
    check_operator_method_signature_args_count(c, fn, fn_spec, 2, oms);

    const Type lhs_type = fn_spec->args[0].type;
    if (lhs_type.ref) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[0].name,
            fn_spec->args[0].pos,
            "Operand cannot be a pointer. (Provided type is %s)",
            type_to_cstr(lhs_type));
        exit(c, 1);
    }

    const Type rhs_type = fn_spec->args[1].type;
    if (!type_eq(rhs_type, lhs_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[1].name,
            fn_spec->args[1].pos,
            "Operand types must be same: Expected %s, got %s",
            type_to_cstr(lhs_type),
            type_to_cstr(rhs_type));
        exit(c, 1);
    }

    if (!type_eq(*fn_spec->return_type, lhs_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(
            EK_NOTE,
            fn->returns.head ? fn->returns.head->token : fn->body->token,
            "Operand types and return type must be same: Expected to return %s, got %s",
            type_to_cstr(lhs_type),
            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
        exit(c, 1);
    }
}

void check_signature_of_binary_comparison_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    const Type *receiver = &fn_spec->args[0].type;

    const OMS oms = OMS_CMP;
    check_operator_method_signature_args_count(c, fn, fn_spec, 2, oms);

    const Type lhs_type = fn_spec->args[0].type;
    if (lhs_type.ref) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[0].name,
            fn_spec->args[0].pos,
            "Operand cannot be a pointer. (Provided type is %s)",
            type_to_cstr(lhs_type));
        exit(c, 1);
    }

    const Type rhs_type = fn_spec->args[1].type;
    if (!type_eq(rhs_type, lhs_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[1].name,
            fn_spec->args[1].pos,
            "Operand types must be same: Expected %s, got %s",
            type_to_cstr(lhs_type),
            type_to_cstr(rhs_type));
        exit(c, 1);
    }

    if (!type_eq(*fn_spec->return_type, c->equivalence_type) && !type_eq(*fn_spec->return_type, c->ordering_type)) //
    {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(
            EK_NOTE,
            fn->returns.head ? fn->returns.head->token : fn->body->token,
            "Expected to return %s or %s, got %s",
            type_to_cstr(c->equivalence_type),
            type_to_cstr(c->ordering_type),
            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
        exit(c, 1);
    }
}

void check_signature_of_index_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    const Type *receiver = &fn_spec->args[0].type;

    const OMS oms = OMS_INDEX;
    check_operator_method_signature_args_count(c, fn, fn_spec, 3, oms);

    const Type assign_type = fn_spec->args[2].type;
    if (!type_eq(assign_type, (Type) {.kind = TYPE_BOOL})) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[2].name,
            fn_spec->args[2].pos,
            "Expected the third argument to be %s, got %s",
            type_to_cstr((Type) {.kind = TYPE_BOOL}),
            type_to_cstr(assign_type));
        exit(c, 1);
    }

    if (!type_is_pointer(*fn_spec->return_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(
            EK_NOTE,
            fn->returns.head ? fn->returns.head->token : fn->body->token,
            "Expected to return a pointer, got %s",
            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
        exit(c, 1);
    }
}

void check_signature_of_slice_operator(Compiler *c, Node_Fn *fn, const Type_Fn *fn_spec) {
    const Type *receiver = &fn_spec->args[0].type;

    const OMS oms = OMS_SLICE;
    check_operator_method_signature_args_count(c, fn, fn_spec, 3, oms);

    const Type begin_type = fn_spec->args[1].type;
    const Type end_type = fn_spec->args[2].type;
    if (!type_eq(end_type, begin_type)) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_parts(
            EK_NOTE,
            fn_spec->args[2].name,
            fn_spec->args[2].pos,
            "Types of range beginning and end must be same: Expected %s, got %s",
            type_to_cstr(begin_type),
            type_to_cstr(end_type));
        exit(c, 1);
    }

    if (fn_spec->returns_count != 1) {
        error_operator_method_wrong_signature(fn->defined_as->node.token, oms, receiver);
        error_token(
            EK_NOTE,
            fn->returns.head ? fn->returns.head->token : fn->body->token,
            "The range operator cannot return %zu values",
            fn_spec->returns_count);
        exit(c, 1);
    }
}
