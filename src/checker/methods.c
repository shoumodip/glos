#include "../error.h"
#include "checker.h"

static_assert(COUNT_TOKENS == 79, "");
const char *operator_method_name_from_token_kind(Token_Kind kind) {
    switch (kind) {
    case TOKEN_ADD:
    case TOKEN_ADD_SET:
        return "add";

    case TOKEN_SUB:
    case TOKEN_SUB_SET:
        return "sub";

    case TOKEN_MUL:
    case TOKEN_MUL_SET:
        return "mul";

    case TOKEN_DIV:
    case TOKEN_DIV_SET:
        return "div";

    case TOKEN_MOD:
    case TOKEN_MOD_SET:
        return "mod";

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_EQ:
    case TOKEN_NE:
        return "compare";

    default:
        unreachable();
    }
}

void check_that_methods_can_be_accessed(Compiler *c, Node *receiver, Module *definition) {
    if (definition->orderless_check_status != CHECKED) {
        error_node(EK_ERROR, receiver, "Cannot access methods at this stage of compilation yet");

        assert(c->current_comptime_conditional_stmt);
        error_node(EK_NOTE, c->current_comptime_conditional_stmt, "Evaluating this conditional statement");

        afprintf(
            stderr,
            ANSI_COLOR_YELLOW | ANSI_BOLD,
            "    The '#if' statements are evaluated immediately instead of waiting for all the definitions\n"
            "    to be registered. Therefore at this point of time, the methods might not be defined yet.\n"
            "    Thus, to prevent inconsistent behaviour, any operations involving them are disallowed.\n\n");

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

        check_that_methods_can_be_accessed(c, receiver_node, definition->module);
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

        check_that_methods_can_be_accessed(c, receiver_node, definition->node.module);
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

        check_that_methods_can_be_accessed(c, receiver_node, definition->node.module);
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

        check_that_methods_can_be_accessed(c, receiver_node, receiver_type.distinct->node.module);
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

            check_that_methods_can_be_accessed(c, receiver_node, c->builtin_module);
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
        check_definition_if_needed(c, method->defined_as, REF_NONE);
    }

    return method;
}

Node_Fn *get_operator_overload(Compiler *c, const char *operator, Node *receiver, Node *op, Module *module) {
    Method_Spec spec = {0};
    if (get_method_spec(c, receiver, receiver->type, sv_from_cstr(operator), &spec, NULL, NULL)) {
        Node_Fn *method = get_method(c, spec, module);
        if (method) {
            const Type_Fn *method_spec = method->node.type.spec.fn;

            const Type receiver_type = method_spec->args[0].type;
            if ((receiver_type.ref > receiver->type.ref + 1) ||
                (receiver_type.ref > receiver->type.ref && !receiver->is_memory)) //
            {
                error_node(EK_ERROR, op, "Too many levels of pointer indirection in method call");
                error_node(
                    EK_NOTE,
                    receiver,
                    "This is of type %s, but the receiver is expected to be %s",
                    type_to_cstr(receiver->type),
                    type_to_cstr(receiver_type));
                error_node(EK_NOTE, (Node *) method->defined_as, "This is the overload used");
                exit(c, 1);
            }

            if (method->polymorphs.count) {
                Call_Checker cc = {0};
                cc.expr = op;
                cc.fn_source = op;
                cc.fn = (Node *) method;
                cc.end = op->token;

                cc.is_method = true;
                cc.receiver = receiver;
                cc.is_polymorph = true;

                check_call_arguments(c, &cc, false);
                assert(cc.fn->kind == NODE_FN);

                method = (Node_Fn *) cc.fn;
            }
            return method;
        }
    }

    check_that_type_is_known(c, receiver);
    error_node(
        EK_ERROR, op, "Method '" SV_Fmt "' is not defined for %s", SV_Arg(spec.name), type_to_cstr(receiver->type));
    exit(c, 1);
}

void error_special_method_wrong_signature(Token name, const char *signature, const char *note) {
    error_token(
        EK_ERROR,
        name,
        "The method '" SV_Fmt "' is special because it implements an operator overload",
        SV_Arg(name.sv));

    ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
    fprintf(
        stderr,
        "    It should have this signature:\n"
        "\n"
        "        " SV_Fmt " :: %s\n"
        "\n",
        SV_Arg(name.sv),
        signature);

    if (note) {
        SV sv = sv_from_cstr(note);
        while (sv.count) {
            const SV line = sv_split_mut(&sv, '\n');
            fprintf(stderr, "    " SV_Fmt "\n", SV_Arg(line));
        }
        fprintf(stderr, "\n");
    }

    fprintf(
        stderr,
        "    It may have other optional arguments at the end, but this is the bare minimum that must be implemented.\n"
        "\n");

    ansi_reset(stderr);
}

void check_special_method_signature_args_count(
    Compiler *c, Node_Fn *fn, const size_t args_count, const char *signature, const char *note) {
    assert(fn->node.type.kind == TYPE_FN);
    const Type_Fn *fn_spec = fn->node.type.spec.fn;

    if (fn_spec->args_count < args_count) {
        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
        error_token(
            EK_NOTE, fn->args_end_token, "Expected at least %zu arguments, got %zu", args_count, fn_spec->args_count);
        exit(c, 1);
    }

    for (size_t i = 0; i < fn_spec->args_count; i++) {
        const Type_Fn_Arg *it = &fn_spec->args[i];
        if (!it->has_default_value && i >= args_count) {
            error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
            error_parts(
                EK_NOTE,
                fn_spec->args[i].name,
                fn_spec->args[i].pos,
                "All arguments after the %zu%s argument must be optional",
                (size_t) args_count,
                order_postfix(args_count));
            exit(c, 1);
        }
    }

    // The previous loop guarantees this
    assert(fn_spec->args_count_min <= args_count);
}
