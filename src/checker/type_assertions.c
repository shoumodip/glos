#include "../error.h"
#include "checker.h"

bool check_that_type_is_known_noexit(const Node *n) {
    if (type_is_unknown(n->type)) {
        error_node(EK_ERROR, n, "Cannot infer type of this expression");
        return false;
    }

    return true;
}

void check_that_type_is_known(Compiler *c, const Node *n) {
    if (!check_that_type_is_known_noexit(n)) {
        exit(c, 1);
    }
}

bool type_assert_noexit(Compiler *c, Node *n, Type expected) {
    if (type_eq(n->type, expected)) {
        return true;
    }

    if (try_auto_cast(c, n, expected, -1)) {
        return true;
    }

    if (!check_that_type_is_known_noexit(n)) {
        return false;
    }

    error_node(EK_ERROR, n, "Expected %s, got %s", type_to_cstr(expected), type_to_cstr(n->type));
    maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(n, expected);
    return false;
}

Type type_assert(Compiler *c, Node *n, Type expected) {
    if (type_assert_noexit(c, n, expected)) {
        return expected;
    }
    exit(c, 1);
}

bool type_assert_grouped_noexit(Compiler *c, Node *n, Type expected, i64 group_index, Token *requirement) {
    Type actual = n->type;

    const bool is_group = group_index != -1 && type_kind_eq(actual, TYPE_GROUP);
    if (is_group) {
        actual = n->type.spec.group.data[group_index];
    }

    if (type_eq(actual, expected)) {
        return true;
    }

    if (!is_group) {
        if (try_auto_cast(c, n, expected, -1)) {
            return true;
        }

        if (!check_that_type_is_known_noexit(n)) {
            return false;
        }

        error_node(EK_ERROR, n, "Expected %s, got %s", type_to_cstr(expected), type_to_cstr(actual));
        maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(n, expected);
    } else {
        if (!check_that_type_is_known_noexit(n)) {
            return false;
        }

        if (try_auto_cast(c, n, expected, group_index)) {
            return true;
        }

        const char *postfix = order_postfix(group_index + 1);
        error_node(
            EK_ERROR,
            n,
            "Expected %zd%s value of this to be %s, got %s. The type of this entire expression is %s",
            group_index + 1,
            postfix,
            type_to_cstr(expected),
            type_to_cstr(actual),
            type_to_cstr(n->type));

        if (requirement) {
            error_token(EK_NOTE, *requirement, "Required here");
        }
    }

    return false;
}

Type type_assert_grouped(Compiler *c, Node *n, Type expected, i64 group_index, Token *requirement) {
    if (type_assert_grouped_noexit(c, n, expected, group_index, requirement)) {
        return expected;
    }
    exit(c, 1);
}

Type type_assert_node(Compiler *c, Node *a, Node *b) {
    if (type_eq(a->type, b->type)) {
        return a->type;
    }

    if (try_auto_cast(c, b, a->type, -1)) {
        return a->type;
    }

    if (try_auto_cast(c, a, b->type, -1)) {
        return b->type;
    }

    check_that_type_is_known(c, a);
    check_that_type_is_known(c, b);
    error_node(EK_ERROR, a, "Expected %s, got %s", type_to_cstr(b->type), type_to_cstr(a->type));
    maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(a, b->type);
    exit(c, 1);
}

Type type_assert_numeric(Compiler *c, const Node *n, bool pointers_allowed, bool floats_allowed) {
    if (type_is_pointer(n->type)) {
        if (pointers_allowed) {
            return n->type;
        }

        goto fail;
    }

    if (type_is_float(n->type)) {
        if (floats_allowed) {
            return n->type;
        }

        goto fail;
    }

    if (type_is_numeric(n->type)) {
        return n->type;
    }

fail:
    check_that_type_is_known(c, n);

    const char *label = NULL;
    if (floats_allowed) {
        label = pointers_allowed ? "numeric or pointer" : "numeric";
    } else {
        label = pointers_allowed ? "integer or pointer" : "integer";
    }

    error_node(EK_ERROR, n, "Expected %s value, got %s", label, type_to_cstr(n->type));
    exit(c, 1);
}

Type type_assert_scalar(Compiler *c, const Node *n) {
    if (type_is_scalar(n->type)) {
        return n->type;
    }

    check_that_type_is_known(c, n);
    error_node(EK_ERROR, n, "Expected scalar value, got %s", type_to_cstr(n->type));
    exit(c, 1);
}

bool type_assert_type_noexit(const Node *n) {
    if (!check_that_type_is_known_noexit(n)) {
        return false;
    }

    if (n->type.is_meta) {
        return true;
    }

    error_node(EK_ERROR, n, "Expected a type, got %s", type_to_cstr(n->type));
    return false;
}

Type type_assert_type(Compiler *c, const Node *n) {
    if (type_assert_type_noexit(n)) {
        return n->type;
    }
    exit(c, 1);
}

void type_assert_type_or_Type(Compiler *c, const Node *n) {
    check_that_type_is_known(c, n);
    if (n->type.is_meta || type_eq(n->type, c->type_info_pointer_type)) {
        return;
    }

    error_node(EK_ERROR, n, "Expected a type, got %s", type_to_cstr(n->type));
    exit(c, 1);
}

Type_Trait_Impl *check_type_satisfies_trait(Compiler *c, Type receiver, Type_Trait *trait, Node *n, i64 group_index) {
    if (receiver.is_meta) {
        error_node(EK_ERROR, n, "A type cannot implement traits");
        exit(c, 1);
    }

    if (trait->methods_count) {
        check_that_methods_can_be_accessed(c, n);
    }

    const Type receiver_without_ref = type_without_ref(receiver);
    ll_foreach(it, &trait->impls) {
        if (!trait->methods_count) {
            return it;
        }

        if (type_eq(it->type, receiver_without_ref)) {
            return it;
        }
    }

    const Type expected = {.kind = TYPE_TRAIT, .spec.trait = trait};

    Type_Trait_Impl impl = {0};
    impl.type = receiver_without_ref;

    if (trait->methods_count) {
        impl.methods = arena_alloc(&default_arena, trait->methods_count * sizeof(*impl.methods));
        impl.methods_count = trait->methods_count;

        typedef enum {
            OK,
            UNDEFINED,
            WRONG_RECEIVER,
            WRONG_SIGNATURE,
        } Error_Kind;

        typedef struct {
            Error_Kind kind;
            Node_Fn   *fn;
        } Error;

        Error *errors = arena_alloc(&temp_arena, trait->methods_count * sizeof(*errors));
        {
            for (size_t i = 0; i < trait->methods_count; i++) {
                const Type_Trait_Method *it = &trait->methods[i];

                Method_Spec spec = {0};
                if (!get_method_spec(c, n, receiver, it->name, &spec, NULL, NULL)) {
                    errors[i] = (Error) {.kind = UNDEFINED};
                    goto next;
                }

                Node_Fn *fn = get_method(c, spec, n->module);
                if (!fn) {
                    errors[i] = (Error) {.kind = UNDEFINED};
                    goto next;
                }

                if (fn->polymorphs.count) {
                    Call_Checker cc = {0};
                    cc.expr = n;
                    cc.fn_source = n;
                    cc.fn = (Node *) fn;
                    cc.end = n->token;

                    cc.is_method = true;
                    cc.receiver = n;
                    cc.is_polymorph = true;

                    check_call_arguments(c, &cc, false);
                    assert(cc.fn->kind == NODE_FN);

                    fn = (Node_Fn *) cc.fn;
                }

                assert(it->type.kind == TYPE_FN);
                const Type_Fn *expected_spec = it->type.spec.fn;

                assert(fn->node.type.kind == TYPE_FN);
                const Type_Fn *actual_spec = fn->node.type.spec.fn;

                if (!type_eq(actual_spec->args[0].type, receiver)) {
                    errors[i] = (Error) {.kind = WRONG_RECEIVER, .fn = fn};
                    goto next;
                }

                if (expected_spec->args_count != actual_spec->args_count) {
                    errors[i] = (Error) {.kind = WRONG_SIGNATURE, .fn = fn};
                    goto next;
                }

                for (size_t j = 0; j < actual_spec->args_count; j++) {
                    if (j == 0) {
                        continue;
                    }

                    if (!type_eq(actual_spec->args[j].type, expected_spec->args[j].type)) {
                        errors[i] = (Error) {.kind = WRONG_SIGNATURE, .fn = fn};
                        goto next;
                    }
                }

                if (!type_eq(*actual_spec->return_type, *expected_spec->return_type)) {
                    errors[i] = (Error) {.kind = WRONG_SIGNATURE, .fn = fn};
                    goto next;
                }

                impl.methods[i].fn = fn;

            next:;
            }

            bool ok = true;
            bool impl_for_other_type = false;
            if (trait->methods_count && errors[0].kind == WRONG_RECEIVER) {
                impl_for_other_type = true;
                const Type receiver = errors[0].fn->node.type.spec.fn->args[0].type;
                for (size_t i = 1; i < trait->methods_count; i++) {
                    if (errors[i].kind != WRONG_RECEIVER ||
                        !type_eq(errors[i].fn->node.type.spec.fn->args[0].type, receiver)) //
                    {
                        impl_for_other_type = false;
                        break;
                    }
                }
            }

            for (size_t i = 0; i < trait->methods_count; i++) {
                const Error it = errors[i];
                if (it.kind == OK) {
                    continue;
                }

                if (ok) {
                    ok = false;
                    if (group_index == -1) {
                        error_node(
                            EK_ERROR,
                            n,
                            "Type %s does not implement %s",
                            type_to_cstr(receiver),
                            type_to_cstr(expected));
                    } else {
                        const char *postfix = order_postfix(group_index + 1);
                        error_node(
                            EK_ERROR,
                            n,
                            "The %zd%s value of this expression has type %s, which does not implement %s. The type of this entire expression is %s",
                            group_index + 1,
                            postfix,
                            type_to_cstr(receiver),
                            type_to_cstr(expected),
                            type_to_cstr(n->type));
                    }
                }

                if (impl_for_other_type) {
                    const Type impl = it.fn->node.type.spec.fn->args[0].type;
                    error_node_begin(EK_NOTE, n);
                    fprintf(
                        stderr, "The trait is implemented for %s, not %s", type_to_cstr(impl), type_to_cstr(receiver));

                    if (type_eq(type_without_ref(impl), type_without_ref(receiver))) {
                        fprintf(stderr, ". Perhaps try %s?", impl.ref > receiver.ref ? "referencing" : "dereferencing");
                    }

                    error_finalize();
                    exit(c, 1);
                }

                switch (it.kind) {
                case UNDEFINED:
                    error_parts(
                        EK_NOTE,
                        trait->methods[i].name,
                        trait->methods[i].pos,
                        "The method '" SV_Fmt "' is not defined for type %s",
                        SV_Arg(trait->methods[i].name),
                        type_to_cstr(receiver));
                    break;

                case WRONG_RECEIVER:
                    it.fn->body = NULL;
                    error_node(
                        EK_NOTE,
                        it.fn->args.head,
                        "The method '" SV_Fmt "' has receiver %s, not %s",
                        SV_Arg(it.fn->defined_as->node.token.sv),
                        type_to_cstr(it.fn->node.type.spec.fn->args[0].type),
                        type_to_cstr(receiver));
                    break;

                case WRONG_SIGNATURE: {
                    Type expected = trait->methods[i].type;
                    assert(expected.kind == TYPE_FN);

                    Type_Fn *spec = expected.spec.fn;
                    assert(spec->args_count);

                    const Type arg0_save = spec->args[0].type;
                    spec->args[0].type = receiver;

                    it.fn->body = NULL;
                    error_node(
                        EK_NOTE,
                        (Node *) it.fn,
                        "The method '" SV_Fmt "' has wrong signature",
                        SV_Arg(it.fn->defined_as->node.token.sv));

                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "    Expected: %s\n"
                        "    Actual:   %s\n\n",
                        type_to_cstr_raw(trait->methods[i].type),
                        type_to_cstr_raw(it.fn->node.type));

                    spec->args[0].type = arg0_save;
                } break;

                case OK:
                    break;
                }
            }

            if (!ok) {
                exit(c, 1);
            }
        }
        arena_reset(&temp_arena, errors);
    }

    impl.trait = trait;
    impl.next = trait->impls.head;
    trait->impls.head = arena_clone(&default_arena, &impl, sizeof(impl));
    return trait->impls.head;
}
