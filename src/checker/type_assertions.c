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

static void error_does_not_implement(Node *n, i64 group_index, Type receiver, Type_Trait *trait) {
    const Type expected = {.kind = TYPE_TRAIT, .spec.trait = trait};
    if (group_index == -1) {
        if (receiver.is_meta) {
            error_node(EK_ERROR, n, "This expression is a type, and thus cannot implement %s", type_to_cstr(expected));
            afprintf(
                stderr,
                ANSI_COLOR_YELLOW | ANSI_BOLD,
                "    Traits are applicable to values. A value of type %s may implement %s, but the type itself\n"
                "    cannot, as it is a compile time concept.\n\n",
                type_to_cstr(type_without_meta(receiver)),
                type_to_cstr(expected));
        } else {
            error_node(EK_ERROR, n, "Type %s does not implement %s", type_to_cstr(receiver), type_to_cstr(expected));
        }
    } else {
        error_node_begin(EK_ERROR, n);
        fprintf(
            stderr,
            "The %zd%s value of this expression has type %s, which does not implement %s",
            group_index + 1,
            order_postfix(group_index + 1),
            type_to_cstr(receiver),
            type_to_cstr(expected));

        if (!type_kind_eq(n->type, TYPE_VOID)) {
            fprintf(stderr, ". The type of this entire expression is %s", type_to_cstr(n->type));
        }
        error_finalize();
    }
}

Type_Trait_Impl *check_type_satisfies_trait(Compiler *c, Type receiver, Type_Trait *trait, Node *n, i64 group_index) {
    const Type receiver_without_ref = type_without_ref(receiver);
    ll_foreach(it, &trait->impls) {
        if (type_eq(it->type, receiver_without_ref)) {
            return it;
        }
    }
    Type_Trait *trait_save = trait;

    Type_Trait_Impl trait_impl = {.type = receiver_without_ref};
    if (trait->polymorph) {
        ht_clear(&c->monomorph_replacements);

        const size_t monomorph_parameters_begin_save = c->monomorph_parameters.begin;
        c->monomorph_parameters.begin = c->monomorph_parameters.count;

        const Monomorphizing_Site monomorphizing_site_save = c->monomorphizing_site;
        c->monomorphizing_site.expr = n;
        c->monomorphizing_site.node = n; // TODO: The entire expression (in case of an explicit cast) would be helpful

        add_monomorph_parameter(c, trait->polymorph, receiver, const_value_type(receiver), NULL);
        Node *node = monomorphize(c, (Node *) trait->definition, n);

        c->monomorph_parameters.count = c->monomorph_parameters.begin;
        c->monomorph_parameters.begin = monomorph_parameters_begin_save;
        c->monomorphizing_site = monomorphizing_site_save;

        assert(type_meta_kind_eq(node->type, TYPE_TRAIT));
        trait = node->type.spec.trait;
    }

    Node_Impl *impl = get_trait_implementation(c, receiver, trait, n);
    if (impl) {
        check_stmt_impl(c, impl);

        Type impl_receiver = impl->receiver->type;
        if (impl->polymorphs.count) {
            trait_impl.methods = arena_alloc(&default_arena, trait->methods_count * sizeof(*trait_impl.methods));
            trait_impl.methods_count = trait->methods_count;

            ll_foreach(it, &impl->methods) {
                assert(it->kind == NODE_DEFINE);
                Node *method = ((Node_Define *) it)->expr;
                assert(method->kind == NODE_FN);

                Call_Checker cc = {0};
                cc.expr = n;
                cc.fn_source = n;
                cc.fn = method;
                cc.end = n->token;

                cc.is_method = true;
                cc.receiver = n;
                cc.is_polymorph = true;

                check_call_arguments(c, &cc, false);
                assert(cc.fn->kind == NODE_FN);

                trait_impl.methods[it->token.as.integer].fn = (Node_Fn *) cc.fn;
            }

            // TODO: I don't know how correct this. Try checking this
            impl_receiver = type_with_ref(receiver, impl_receiver.ref);
        }

        if (!type_eq(impl_receiver, receiver)) {
            error_does_not_implement(n, group_index, receiver, trait);
            error_token_range_begin(EK_NOTE, impl->node.token, get_rightmost_token_of_node(impl->trait));
            fprintf(
                stderr,
                "The trait is implemented for %s, not %s",
                type_to_cstr(impl->receiver->type),
                type_to_cstr(receiver));

            if (type_eq(type_without_ref(impl_receiver), type_without_ref(receiver))) {
                fprintf(
                    stderr, ". Perhaps try %s?", impl_receiver.ref > receiver.ref ? "referencing" : "dereferencing");
            }
            error_finalize();
            exit(c, 1);
        }
    } else if (trait->methods_count) {
        error_does_not_implement(n, group_index, receiver, trait);

        // TODO: This will need to be reworked when explicit trait monomorphization is introduced
        const Type expected = {.kind = TYPE_TRAIT, .spec.trait = trait};
        ansi_set(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD);
        fprintf(stderr, "    impl %s for %s {\n", type_to_cstr_raw(receiver), type_to_cstr_raw(expected));
        for (size_t i = 0; i < trait->methods_count; i++) {
            Type_Trait_Method *it = &trait->methods[i];
            prepare_impl_method_for_printing_type(NULL, it);
            fprintf(stderr, "        " SV_Fmt " :: %s { todo() }\n", SV_Arg(it->name), type_to_cstr_raw(it->type));
        }
        fprintf(stderr, "    }\n\n");
        ansi_reset(stderr);

        error_node(EK_NOTE, (Node *) trait->definition, "Trait defined here");
        exit(c, 1);
    }

    trait_impl.trait = trait;
    trait = trait_save;
    if (!impl || impl->polymorphs.count) {
        trait_impl.next = trait->impls.head;
        trait->impls.head = arena_clone(&default_arena, &trait_impl, sizeof(trait_impl));
    }
    return trait->impls.head;
}
