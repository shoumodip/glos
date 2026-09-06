#include "../error.h"
#include "checker.h"

static bool is_indexable(Compiler *c, Node *n, Type type, Module *module) {
    if (type_kind_eq(type, TYPE_ARRAY) || type_kind_eq(type, TYPE_DYNAMIC_ARRAY) || //
        type_kind_eq(type, TYPE_SLICE) || type_kind_eq(type, TYPE_STRING))          //
    {
        return true;
    }

    Method_Spec spec = {0};
    if (get_method_spec(c, n, type, OPERATOR_INDEX, &spec, NULL, NULL)) {
        return get_method(c, spec, module) != NULL;
    }

    return false;
}

static void check_whether_member_access_is_valid(Compiler *c, Node_Member *m) {
    if (m->rhs) {
        assert(m->lhs); // A bare '.(Type)' will error out at parse time
        if (!type_is_trait(m->lhs->type) && !type_is_union(m->lhs->type)) {
            error_node(EK_ERROR, (Node *) m, "Cannot access variant of %s", type_to_cstr(m->lhs->type));
            exit(c, 1);
        }
    } else {
        if (sv_match(m->node.token.sv, "_")) {
            error_token(EK_ERROR, m->node.token, "Field '_' cannot be accessed");
            exit(c, 1);
        }
    }
}

static_assert(COUNT_TOKENS == 90, "");
static Node_Fn *check_assignment_lhs_for_arithmetics(Compiler *c, Node_Binary *binary, Node *n) {
    const Token_Kind op = binary->node.token.kind;
    switch (op) {
    case TOKEN_ADD_SET:
    case TOKEN_SUB_SET:
        if (!type_is_numeric(n->type) && !type_is_pointer(n->type)) {
            return get_operator_overload(c, token_kind_to_operator_method_name(op), n, (Node *) binary, binary->module);
        }
        break;

    case TOKEN_MUL_SET:
    case TOKEN_DIV_SET:
    case TOKEN_MOD_SET:
        if (type_is_pointer(n->type)) {
            error_node(EK_ERROR, (Node *) binary, "This operation is not valid for pointers");
            error_node(EK_NOTE, n, "The operands are of type %s", type_to_cstr(n->type));
            exit(c, 1);
        }

        if (!type_is_numeric(n->type)) {
            return get_operator_overload(c, token_kind_to_operator_method_name(op), n, (Node *) binary, binary->module);
        }
        break;

    case TOKEN_SHL_SET:
    case TOKEN_SHR_SET:
    case TOKEN_BOR_SET:
    case TOKEN_BAND_SET:
    case TOKEN_BXOR_SET:
        if (type_is_pointer(n->type)) {
            error_node(EK_ERROR, (Node *) binary, "This operation is not valid for pointers");
            error_node(EK_NOTE, n, "The operands are of type %s", type_to_cstr(n->type));
            exit(c, 1);
        }

        type_assert_numeric(c, n, false, false);
        break;

    default:
        // Pass
        break;
    }

    return NULL;
}

static void check_assignment(Compiler *c, Node_Binary *binary) {
    check_expr(c, binary->lhs, REF_ASSIGN);
    check_expr(c, binary->rhs, REF_NONE);

    const bool is_lhs_group = type_kind_eq(binary->lhs->type, TYPE_GROUP);
    const bool is_rhs_group = type_kind_eq(binary->rhs->type, TYPE_GROUP);

    const size_t lhs_count = is_lhs_group ? binary->lhs->type.spec.group.count : 1;
    const size_t rhs_count = is_rhs_group ? binary->rhs->type.spec.group.count : 1;
    if (lhs_count != rhs_count) {
        error_number_of_values_mismatch((Node *) binary, lhs_count, rhs_count, NULL, NULL);
    }

    if (is_lhs_group) {
        if (binary->node.token.kind != TOKEN_SET) {
            binary->overloads = arena_alloc(&default_arena, lhs_count * sizeof(*binary->overloads));
        }

        assert(is_rhs_group);
        for (size_t i = 0; i < lhs_count; i++) {
            i64   lhs_group_index = -1;
            Node *lhs = get_node_from_group(binary->lhs, i, &lhs_group_index);
            i64   rhs_group_index = -1;
            Node *rhs = get_node_from_group(binary->rhs, i, &rhs_group_index);
            type_assert_grouped(c, rhs, lhs->type, rhs_group_index, &lhs->token);

            if (binary->overloads) {
                binary->overloads[i] = check_assignment_lhs_for_arithmetics(c, binary, lhs);
            }
        }
    } else {
        type_assert(c, binary->rhs, binary->lhs->type);
        binary->overload = check_assignment_lhs_for_arithmetics(c, binary, binary->lhs);
    }

    binary->node.type = (Type) {.kind = TYPE_VOID};
}

void check_expr_atom(Compiler *c, Node_Atom *atom, Ref_Kind ref, bool *is_ref_valid) {
    Node *n = (Node *) atom;
    static_assert(COUNT_TOKENS == 90, "");
    switch (n->token.kind) {
    case TOKEN_INT:
        n->type = (Type) {.kind = TYPE_INT};
        break;

    case TOKEN_BOOL:
        n->type = (Type) {.kind = TYPE_BOOL};
        break;

    case TOKEN_CHAR:
        n->type = (Type) {.kind = TYPE_CHAR};
        break;

    case TOKEN_NULL:
        n->type = (Type) {.kind = TYPE_RAWPTR};
        break;

    case TOKEN_FLOAT:
        n->type = (Type) {.kind = TYPE_FLOAT};
        break;

    case TOKEN_IDENT:
        check_ident(c, n, ref);
        *is_ref_valid = true; // check_ident() has already checked whether the reference is valid
        break;

    case TOKEN_STRING:
        n->type = (Type) {.kind = TYPE_STRING};
        break;

    case TOKEN_ISTRING:
        n->type = (Type) {.kind = TYPE_STRING};
        break;

    case TOKEN_DIRECTIVE_MAIN:
        n->type = c->main_fn_type;
        break;

    case TOKEN_DIRECTIVE_PLATFORM:
        get_platform(c, &n->type);
        break;

    case TOKEN_DIRECTIVE_LOCATION:
        n->type = c->source_code_location_type;
        break;

    case TOKEN_DIRECTIVE_CALLER_LOCATION:
        error_node(
            EK_ERROR,
            n,
            "Cannot use %s here. It can only be used as the default value for a function argument",
            token_kind_to_cstr(n->token.kind));
        exit(c, 1);
        break;

    default:
        unreachable();
    }
}

void check_expr_group(Compiler *c, Node_Group *group, Ref_Kind ref, bool *is_ref_valid) {
    Node      *n = (Node *) group;
    Type_Group spec = {0};
    ll_foreach(it, &group->nodes) {
        check_expr(c, it, ref);
        if (type_kind_eq(it->type, TYPE_GROUP)) {
            spec.count += it->type.spec.group.count;
        } else {
            spec.count++;
        }
    }

    spec.data = arena_alloc(&default_arena, spec.count * sizeof(*spec.data));

    size_t iota = 0;
    ll_foreach(it, &group->nodes) {
        if (type_kind_eq(it->type, TYPE_GROUP)) {
            for (size_t i = 0; i < it->type.spec.group.count; i++) {
                spec.data[iota++] = it->type.spec.group.data[i];
            }
        } else {
            spec.data[iota++] = it->type;
        }
    }

    n->type = (Type) {.kind = TYPE_GROUP, .spec.group = spec};
    *is_ref_valid = true;
}

void check_expr_unary(Compiler *c, Node_Unary *unary, bool *is_ref_valid) {
    Node *n = (Node *) unary;
    static_assert(COUNT_TOKENS == 90, "");
    switch (n->token.kind) {
    case TOKEN_SUB:
        check_expr(c, unary->value, REF_NONE);
        if (!type_is_numeric(unary->value->type) && !type_is_pointer(unary->value->type)) {
            unary->overload = get_operator_overload(c, OPERATOR_SUB, unary->value, n, unary->module);
        }
        n->type = unary->value->type;
        break;

    case TOKEN_MUL: {
        check_expr(c, unary->value, REF_NONE);
        check_that_type_is_known(c, unary->value);

        if (!unary->value->type.ref || unary->value->type.is_meta) {
            if (type_kind_eq(unary->value->type, TYPE_RAWPTR)) {
                error_node(EK_ERROR, unary->value, "Cannot dereference raw pointer");
                exit(c, 1);
            }

            error_node(
                EK_ERROR, unary->value, "Can only dereference typed pointer, got %s", type_to_cstr(unary->value->type));
            exit(c, 1);
        }

        n->type = unary->value->type;
        n->type.ref--;
        if (n->type.distinct && n->type.distinct->node.type.ref > n->type.ref) {
            n->type.distinct = NULL;
        }

        *is_ref_valid = true;
        n->is_memory = true;
    } break;

    case TOKEN_BAND: {
        check_expr(c, unary->value, REF_ADDR);
        check_that_type_is_known(c, unary->value);
        n->type = unary->value->type;
        n->type.ref++;
    } break;

    case TOKEN_BNOT:
        check_expr(c, unary->value, REF_NONE);
        n->type = type_assert_numeric(c, unary->value, false, false);
        break;

    case TOKEN_LNOT:
        check_expr(c, unary->value, REF_NONE);
        n->type = type_assert(c, unary->value, (Type) {.kind = TYPE_BOOL});
        break;

    case TOKEN_SIZEOF:
        check_expr(c, unary->value, REF_NONE);
        check_that_type_is_known(c, unary->value);
        n->type = (Type) {.kind = TYPE_INT};
        break;

    case TOKEN_TYPEOF:
        check_expr(c, unary->value, REF_NONE);
        check_that_type_is_known(c, unary->value);
        finalize_untyped_type(c, unary->value);
        n->type = type_with_meta(unary->value->type);
        break;

    default:
        unreachable();
    }
}

void check_expr_binary(Compiler *c, Node_Binary *binary, bool check_children) {
    Node *n = (Node *) binary;
    static_assert(COUNT_TOKENS == 90, "");
    switch (n->token.kind) {
    case TOKEN_ADD:
    case TOKEN_SUB:
        if (check_children) {
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert_node(c, binary->rhs, binary->lhs);
        }

        if (!type_is_numeric(binary->lhs->type) && !type_is_pointer(binary->lhs->type)) {
            binary->overload = get_operator_overload(
                c, token_kind_to_operator_method_name(n->token.kind), binary->lhs, n, binary->module);
        }
        n->type = binary->lhs->type;
        break;

    case TOKEN_MUL:
    case TOKEN_DIV:
    case TOKEN_MOD:
        if (check_children) {
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert_node(c, binary->rhs, binary->lhs);
        }

        if (type_is_pointer(binary->lhs->type)) {
            error_node(EK_ERROR, n, "This operation is not valid for pointers");
            error_node(EK_NOTE, binary->lhs, "The operands are of type %s", type_to_cstr(binary->lhs->type));
            exit(c, 1);
        }

        if (!type_is_numeric(binary->lhs->type)) {
            binary->overload = get_operator_overload(
                c, token_kind_to_operator_method_name(n->token.kind), binary->lhs, n, binary->module);
        }
        n->type = binary->lhs->type;
        break;

    case TOKEN_SHL:
    case TOKEN_SHR:
    case TOKEN_BOR:
    case TOKEN_BAND:
    case TOKEN_BXOR:
        if (check_children) {
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert_node(c, binary->rhs, binary->lhs);
        }

        if (type_is_pointer(binary->lhs->type)) {
            error_node(EK_ERROR, n, "This operation is not valid for pointers");
            error_node(EK_NOTE, binary->lhs, "The operands are of type %s", type_to_cstr(binary->lhs->type));
            exit(c, 1);
        }

        n->type = type_assert_numeric(c, binary->lhs, false, false);
        break;

        // The following can never be ran as a result of autocast, therefore not considering 'check_children'

    case TOKEN_LOR:
    case TOKEN_LAND:
    case TOKEN_LXOR:
        if (check_children) {
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert(c, binary->lhs, (Type) {.kind = TYPE_BOOL});
            type_assert_node(c, binary->rhs, binary->lhs);
        }
        n->type = binary->lhs->type;
        break;

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
        check_expr(c, binary->lhs, REF_NONE);
        check_expr(c, binary->rhs, REF_NONE);
        type_assert_node(c, binary->rhs, binary->lhs);
        if (!type_is_numeric(binary->lhs->type) && !type_is_pointer(binary->lhs->type)) {
            binary->overload = get_operator_overload_ex(
                c,
                token_kind_to_operator_method_name(n->token.kind),
                binary->lhs->type,
                n,
                binary->module,
                true,
                false,
                binary->lhs,
                -1);

            if (!binary->overload->is_compare_operator_complete) {
                assert(binary->overload->returns.head);
                error_node(
                    EK_ERROR, n, "Type %s does not implement ordered comparisons", type_to_cstr(binary->lhs->type));
                error_node(
                    EK_NOTE,
                    binary->overload->returns.head,
                    "The method '" SV_Fmt "' only implements equality checking since its return type is %s, not %s",
                    SV_Arg(binary->overload->defined_as->node.token.sv),
                    type_to_cstr(*binary->overload->node.type.spec.fn->return_type),
                    type_to_cstr(c->ordering_type));
                exit(c, 1);
            }
        }
        n->type = (Type) {.kind = TYPE_BOOL};
        break;

    case TOKEN_EQ:
    case TOKEN_NE:
        check_expr(c, binary->lhs, REF_NONE);
        check_expr(c, binary->rhs, REF_NONE);
        if (type_is_trait(binary->lhs->type)) {
            binary->trait_check = binary->lhs;
            if (!node_is_null(binary->rhs)) {
                type_assert_type(c, binary->rhs);
                binary->rhs->type.is_meta = false;
                check_type_satisfies_trait(c, binary->rhs->type, binary->lhs->type.spec.trait, binary->rhs, -1);
                binary->trait_check_type = arena_clone(&default_arena, &binary->rhs->type, sizeof(binary->rhs->type));
                binary->rhs->type.is_meta = true;
            }
        } else if (type_is_trait(binary->rhs->type)) {
            binary->trait_check = binary->rhs;
            if (!node_is_null(binary->lhs)) {
                type_assert_type(c, binary->lhs);
                binary->lhs->type.is_meta = false;
                check_type_satisfies_trait(c, binary->lhs->type, binary->rhs->type.spec.trait, binary->lhs, -1);
                binary->trait_check_type = arena_clone(&default_arena, &binary->lhs->type, sizeof(binary->lhs->type));
                binary->lhs->type.is_meta = true;
            }
        } else if (type_is_union(binary->lhs->type)) {
            binary->union_check = binary->lhs;
            if (!node_is_null(binary->rhs)) {
                type_assert_type(c, binary->rhs);
                binary->union_check_index = get_union_type_index(c, binary->rhs, binary->lhs->type);
            }
        } else if (type_is_union(binary->rhs->type)) {
            binary->union_check = binary->rhs;
            if (!node_is_null(binary->lhs)) {
                type_assert_type(c, binary->lhs);
                binary->union_check_index = get_union_type_index(c, binary->lhs, binary->rhs->type);
            }
        } else {
            type_assert_node(c, binary->rhs, binary->lhs);
            check_that_type_is_known(c, binary->lhs);

            if (try_auto_cast_type_to_rtti(c, binary->lhs, c->type_info_pointer_type)) {
                assert(try_auto_cast_type_to_rtti(c, binary->rhs, c->type_info_pointer_type));
            } else if (!type_is_scalar(binary->lhs->type)) {
                binary->overload = get_operator_overload(
                    c, token_kind_to_operator_method_name(n->token.kind), binary->lhs, n, binary->module);
            }
        }
        n->type = (Type) {.kind = TYPE_BOOL};
        break;

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
    case TOKEN_BXOR_SET:
        check_assignment(c, binary);
        break;

    default:
        unreachable();
    }
}

static void error_undefined_in(Compiler *c, const Token *token, const Type *type, const char *label) {
    error_token(
        EK_ERROR,
        *token,
        "Undefined %s '" SV_Fmt "' in type %s",
        label,
        SV_Arg(token->sv),
        type_to_cstr(type_without_meta(*type)));

    if (type->kind == TYPE_TRAIT) {
        error_node(EK_NOTE, (Node *) type->spec.trait->definition, "Trait defined here");
    } else if (type->kind == TYPE_STRUCT) {
        error_node(EK_NOTE, (Node *) type->spec.structt->definition, "Structure defined here");
    }
    exit(c, 1);
}

void check_expr_member(Compiler *c, Node_Member *member, Ref_Kind ref, bool *is_ref_valid) {
    Node *n = (Node *) member;
    if (member->lhs) {
        {
            Ref_Kind ref_member = ref;
            switch (ref_member) {
            case REF_ADDR:
                ref_member = REF_ADDR_MEMBER;
                break;

            case REF_ASSIGN:
                ref_member = REF_ASSIGN_MEMBER;
                break;

            case REF_NONE:
            case REF_SLICE:
            case REF_ADDR_MEMBER:
            case REF_ASSIGN_MEMBER:
                // Pass
                break;
            }

            check_expr(c, member->lhs, ref_member);
        }

        check_that_type_is_known(c, member->lhs);

        *is_ref_valid = true; // check_node() has already determined that the reference is valid

        // Method
        bool can_have_methods = false;
        {
            Method_Spec spec = {0};
            if (get_method_spec(c, member->lhs, member->lhs->type, n->token.sv, &spec, NULL, NULL)) {
                can_have_methods = true;
                member->method = get_method(c, spec, member->node.module);
                if (member->method) {
                    n->type = member->method->node.type;
                    assert(n->type.kind == TYPE_FN);

                    const Type_Fn *method_spec = n->type.spec.fn;
                    assert(method_spec->args_count);

                    const Type receiver_type = method_spec->args[0].type;
                    if (receiver_type.ref > member->lhs->type.ref + 1) {
                        error_node(EK_ERROR, n, "Too many levels of pointer indirection in method call");
                        error_node(
                            EK_NOTE,
                            member->lhs,
                            "This is of type %s, but the receiver is expected to be %s",
                            type_to_cstr(member->lhs->type),
                            type_to_cstr(receiver_type));
                        exit(c, 1);
                    }

                    if (receiver_type.ref > member->lhs->type.ref && !member->lhs->is_memory) {
                        error_node(EK_ERROR, n, "Too many levels of pointer indirection in method call");
                        error_node(
                            EK_NOTE,
                            member->lhs,
                            "This is of type %s, but the receiver is expected to be %s",
                            type_to_cstr(member->lhs->type),
                            type_to_cstr(receiver_type));
                        error_node(
                            EK_NOTE,
                            member->lhs,
                            "This value does not exist in memory, therefore cannot take reference to it");
                        exit(c, 1);
                    }
                    *is_ref_valid = ref == REF_NONE;
                }
            }
        }

        if (!member->method) {
            n->is_memory = member->lhs->is_memory;
            if (member->lhs->type.is_meta && member->lhs->type.kind == TYPE_ENUM) {
                check_whether_member_access_is_valid(c, member);
                Node_Enum *enumm = member->lhs->type.spec.enumm.definition;
                member->enum_value = get_enum_value(c, enumm, n->token.sv, &n->token);
                member->is_enum = true;
                n->type = type_without_meta(member->lhs->type);
            } else if (type_kind_eq(member->lhs->type, TYPE_TRAIT)) {
                check_whether_member_access_is_valid(c, member);

                Type_Trait *spec = member->lhs->type.spec.trait;
                if (member->rhs) {
                    check_expr(c, member->rhs, REF_NONE);
                    type_assert_type(c, member->rhs);
                    n->type = type_without_meta(member->rhs->type);
                    check_type_satisfies_trait(c, n->type, spec, member->rhs, -1);
                } else if (sv_match(n->token.sv, "type")) {
                    n->type = c->type_info_pointer_type;
                    member->field_index = 0;
                } else if (sv_match(n->token.sv, "data")) {
                    n->type = (Type) {.kind = TYPE_RAWPTR};
                    member->field_index = 1;
                } else if (sv_match(n->token.sv, "impl")) {
                    n->type = (Type) {.kind = TYPE_RAWPTR};
                    member->field_index = 2;
                } else {
                    bool ok = false;
                    for (size_t i = 0; i < spec->methods_count; i++) {
                        const Type_Trait_Method *it = &spec->methods[i];
                        if (sv_eq(n->token.sv, it->name)) {
                            member->trait_method = i;
                            member->is_trait = true;
                            ok = true;
                            n->type = it->type;
                        }
                    }

                    if (!ok) {
                        error_undefined_in(c, &n->token, &member->lhs->type, "field or method");
                    }
                }
            } else if (type_kind_eq(member->lhs->type, TYPE_UNION)) {
                check_whether_member_access_is_valid(c, member);
                if (member->rhs) {
                    check_expr(c, member->rhs, REF_NONE);
                    type_assert_type(c, member->rhs);
                    member->union_index = get_union_type_index(c, member->rhs, member->lhs->type);
                    n->type = type_without_meta(member->rhs->type);
                } else {
                    if (sv_match(n->token.sv, "type")) {
                        n->type = (Type) {.kind = TYPE_S64};
                        member->field_index = 0;
                    } else {
                        error_undefined_in(c, &n->token, &member->lhs->type, "field or method");
                    }
                }
            } else if (type_kind_eq(member->lhs->type, TYPE_STRUCT)) {
                check_whether_member_access_is_valid(c, member);
                Type_Struct_Field *definition = NULL;

                Type_Struct *spec = member->lhs->type.spec.structt;
                for (size_t i = 0; i < spec->fields_count; i++) {
                    Type_Struct_Field *it = &spec->fields[i];
                    if (sv_eq(it->name, n->token.sv)) {
                        definition = it;
                        member->field_index = i;
                        break;
                    }
                }

                if (!definition) {
                    error_undefined_in(c, &n->token, &member->lhs->type, "field or method");
                }

                n->type = definition->type;
            } else if (type_kind_eq(member->lhs->type, TYPE_ARRAY)) {
                check_whether_member_access_is_valid(c, member);
                if (sv_match(n->token.sv, "data")) {
                    n->type = *member->lhs->type.spec.array.element;
                    n->type.ref++;
                    member->field_index = 0;
                } else if (sv_match(n->token.sv, "count")) {
                    n->type = (Type) {.kind = TYPE_S64};
                    member->field_index = 1;

                    n->is_memory = false;
                    *is_ref_valid = ref == REF_NONE;
                } else {
                    error_undefined_in(c, &n->token, &member->lhs->type, "field");
                }
            } else if (type_kind_eq(member->lhs->type, TYPE_DYNAMIC_ARRAY)) {
                check_whether_member_access_is_valid(c, member);
                if (sv_match(n->token.sv, "data")) {
                    n->type = *member->lhs->type.spec.slice.element;
                    n->type.ref++;
                    member->field_index = 0;
                } else if (sv_match(n->token.sv, "count")) {
                    n->type = (Type) {.kind = TYPE_S64};
                    member->field_index = 1;
                } else if (sv_match(n->token.sv, "capacity")) {
                    n->type = (Type) {.kind = TYPE_S64};
                    member->field_index = 2;
                } else {
                    error_undefined_in(c, &n->token, &member->lhs->type, "field");
                }
            } else if (type_kind_eq(member->lhs->type, TYPE_SLICE)) {
                check_whether_member_access_is_valid(c, member);
                if (sv_match(n->token.sv, "data")) {
                    n->type = *member->lhs->type.spec.slice.element;
                    n->type.ref++;
                    member->field_index = 0;
                } else if (sv_match(n->token.sv, "count")) {
                    n->type = (Type) {.kind = TYPE_S64};
                    member->field_index = 1;
                } else {
                    error_undefined_in(c, &n->token, &member->lhs->type, "field");
                }
            } else if (type_kind_eq(member->lhs->type, TYPE_STRING)) {
                check_whether_member_access_is_valid(c, member);
                if (sv_match(n->token.sv, "data")) {
                    n->type = (Type) {.kind = TYPE_CHAR, .ref = 1};
                    member->field_index = 0;
                } else if (sv_match(n->token.sv, "count")) {
                    n->type = (Type) {.kind = TYPE_S64};
                    member->field_index = 1;
                } else {
                    error_undefined_in(c, &n->token, &member->lhs->type, "field");
                }
            } else if (type_kind_eq(member->lhs->type, TYPE_MODULE)) {
                check_whether_member_access_is_valid(c, member);
                check_ident(c, n, ref);
            } else {
                bool ok = false;
                if (member->lhs->type.is_meta) {
                    const Type receiver = type_without_meta(member->lhs->type);

                    Method_Spec spec = {0};
                    if (get_method_spec(c, member->lhs, receiver, n->token.sv, &spec, NULL, NULL)) {
                        member->method = get_method(c, spec, member->node.module);
                        if (member->method) {
                            ok = true;
                            n->type = member->method->node.type;
                        } else {
                            error_undefined_in(c, &n->token, &member->lhs->type, "method");
                        }
                    } else {
                        error_node(EK_ERROR, n, "There are no methods defined on %s", type_to_cstr(receiver));
                        exit(c, 1);
                    }
                }

                if (!ok) {
                    if (can_have_methods) {
                        error_node(EK_ERROR, n, "Undefined method '" SV_Fmt "'", SV_Arg(n->token.sv));
                    } else {
                        error_node(EK_ERROR, n, "Cannot access field of %s", type_to_cstr(member->lhs->type));
                    }
                    exit(c, 1);
                }
            }
        }
    } else {
        check_whether_member_access_is_valid(c, member);
        n->type = (Type) {.kind = TYPE_UNKNOWN_ENUM};
        member->is_enum = true;
    }
}

void check_expr_enum(Compiler *c, Node_Enum *enumm) {
    Node *n = (Node *) enumm;

    Type_Enum spec = {.underlying = TYPE_INT, .definition = enumm};
    Type      underlying = {.kind = spec.underlying};
    if (enumm->underlying) {
        check_expr(c, enumm->underlying, REF_NONE);
        type_assert_type(c, enumm->underlying);

        underlying = type_without_meta(enumm->underlying->type);
        if (!type_is_integer(underlying)) {
            error_node(
                EK_ERROR,
                enumm->underlying,
                "Expected underlying type of the enumeration to be an integer, got %s",
                type_to_cstr(underlying));
            exit(c, 1);
        }

        spec.underlying = underlying.kind;
    }

    Int128 iota = {0};
    Int128 iota_max = {0};
    ll_foreach(it, &enumm->values) {
        ll_foreach(prev, &enumm->values) {
            if (prev == it) {
                break;
            }

            if (sv_eq(it->token.sv, prev->token.sv)) {
                error_redefinition(c, it, &prev->token.pos);
            }
        }

        assert(it->kind == NODE_UNARY);
        Node_Unary *unary = (Node_Unary *) it;
        if (unary->value) {
            check_expr(c, unary->value, REF_NONE);
            type_assert(c, unary->value, underlying);

            const Const_Value value = eval_const_expr(c, unary->value, false);
            assert(value.kind == CONST_VALUE_INT);
            iota = value.as.integer;
        }

        if (int128_lt(iota_max, iota, true)) {
            iota_max = iota;
        }

        it->type.kind = underlying.kind;
        check_int_limit(c, it, iota);
        it->type.kind = TYPE_VOID;
        it->token.as.integer = iota.low;
        iota = int128_add(iota, INT128_FROM_U64(1), true);
    }

    n->type = (Type) {.kind = TYPE_ENUM, .is_meta = true, .spec.enumm = spec};
}

void check_expr_trait(Compiler *c, Node_Trait *trait) {
    Node *n = (Node *) trait;

    Type_Trait *spec = arena_alloc(&default_arena, sizeof(*spec));
    spec->definition = trait;

    n->type = (Type) {
        .kind = TYPE_TRAIT,
        .is_meta = true,
        .spec.trait = spec,
    };

    if (trait->defined_as) {
        trait->defined_as->node.type = n->type;
        trait->defined_as->definition_spec->check_status = CHECKED;
    }

    spec->methods = arena_alloc(&default_arena, trait->methods_count * sizeof(*spec->methods));
    spec->methods_count = trait->methods_count;

    size_t iota = 0;
    ll_foreach(method, &trait->methods) {
        assert(method->kind == NODE_DEFINE);
        Node_Define *define = (Node_Define *) method;

        assert(define->name->kind == NODE_ATOM && define->name->token.kind == TOKEN_IDENT);
        Node_Atom *it = (Node_Atom *) define->name;
        for (size_t i = 0; i < iota; i++) {
            const Type_Trait_Method *previous = &spec->methods[i];
            if (sv_eq(previous->name, it->node.token.sv)) {
                error_redefinition(c, (const Node *) it, &previous->pos);
            }
        }

        it->definition_spec->is_local = false;
        check_definition(c, it, define->expr, define->type, false);
        assert(define->type);
        assert(type_kind_eq(define->type->type, TYPE_FN) && !define->type->type.ref);

        Type_Trait_Method *tm = &spec->methods[iota++];
        tm->pos = it->node.token.pos;
        tm->name = it->node.token.sv;
        tm->type = define->type->type;

        assert(define->type->kind == NODE_FN);
        tm->signature = (Node_Fn *) define->type;
    }
}

void check_expr_union(Compiler *c, Node_Union *unionn) {
    Node *n = (Node *) unionn;

    Type_Union *spec = arena_alloc(&default_arena, sizeof(*spec));
    spec->definition = unionn;

    n->type = (Type) {
        .kind = TYPE_UNION,
        .is_meta = true,
        .spec.unionn = spec,
    };

    if (unionn->defined_as) {
        unionn->defined_as->node.type = n->type;
    }

    spec->variants = arena_alloc(&default_arena, unionn->variants_count * sizeof(*spec->variants));
    spec->variants_count = unionn->variants_count;

    size_t iota = 0;
    ll_foreach(it, &unionn->variants) {
        check_expr(c, it, REF_NONE);
        type_assert_type(c, it);

        Type_Union_Variant *variant = &spec->variants[iota];
        variant->pos = it->token.pos;
        variant->type = type_without_meta(it->type);

        for (size_t i = 0; i < iota; i++) {
            const Type_Union_Variant *prev = &spec->variants[i];
            if (type_eq(prev->type, variant->type)) {
                error_redefinition(c, it, &prev->pos);
            }
        }

        iota++;
    }
}

static void check_polymorph(Compiler *c, Node_Polymorph *p) {
    ll_foreach(it, &p->constraints) {
        check_expr(c, it, REF_NONE);
        if (!type_meta_kind_eq(it->type, TYPE_TRAIT)) {
            error_node(
                EK_ERROR, it, "Expected polymorph constraint to be a trait type, got %s", type_to_cstr(it->type));
            exit(c, 1);
        }
        it->type.is_meta = false;
    }
    p->node.type = p->name->node.type;
}

void check_expr_struct(Compiler *c, Node_Struct *structt) {
    Node *n = (Node *) structt;

    Type_Struct *spec = arena_alloc(&default_arena, sizeof(*spec));
    spec->definition = structt;
    spec->original_definition = structt;
    spec->polymorphs = arena_alloc(&default_arena, structt->polymorphs.count * sizeof(*spec->polymorphs));

    if (structt->polymorphs.count && !structt->defined_as) {
        error_node(EK_NOTE, n, "A polymorphic type must be defined as a constant before it can be used");
        exit(c, 1);
    }

    ll_foreach(it, &structt->polymorphs) {
        if (!it->is_monomorphized) {
            for (size_t i = 0; i < spec->polymorphs_count; i++) {
                const Token *previous = &spec->polymorphs[i]->name->node.token;
                if (sv_eq(previous->sv, it->name->node.token.sv)) {
                    error_redefinition(c, (Node *) it->name, &previous->pos);
                }
            }

            assert(it->name->definition_spec);
            Node_Define *define = it->name->definition_spec->definition_node;

            if (define->type) {
                check_expr(c, define->type, REF_NONE);
                it->name->node.type = type_without_meta(type_assert_type(c, define->type));
            }

            if (define->expr) {
                check_expr(c, define->expr, REF_NONE);
                if (define->type) {
                    type_assert(c, define->expr, it->name->node.type);
                } else {
                    if (define->expr->type.is_meta) {
                        it->name->node.type = c->type_info_pointer_type;
                    } else {
                        it->name->node.type = define->expr->type;
                    }
                }

                it->name->definition_spec->const_value = eval_const_expr(c, define->expr, false);
                it->name->definition_spec->is_const_value_evaluated = true;
            } else {
                spec->polymorphs_count_min++;
            }

            check_polymorph(c, it);
            if (type_eq(it->node.type, c->type_info_pointer_type)) {
                it->is_type = true;
            }
        }

        spec->polymorphs[spec->polymorphs_count++] = it;
    }

    ll_foreach(it, &structt->monomorphs) {
        assert(it->is_monomorphized);
        if (it->is_type) {
            assert(it->monomorphization_value.kind == CONST_VALUE_TYPE);
            it->node.type = it->monomorphization_value.as.type;
            it->node.type.is_meta = true;
        } else {
            it->node.type = it->monomorphization_type;
        }
    }

    n->type = (Type) {
        .kind = TYPE_STRUCT,
        .is_meta = true,
        .spec.structt = spec,
    };

    if (structt->defined_as && type_kind_eq(structt->defined_as->node.type, TYPE_VOID)) {
        structt->defined_as->node.type = n->type;
    }

    if (!structt->polymorphs.count) {
        const size_t fields_start = c->struct_fields.count;
        ll_foreach(field, &structt->fields) {
            if (field->kind == NODE_DEFINE) {
                Node_Define *define = (Node_Define *) field;

                Node_Atom *it = NULL;
                while ((it = (Node_Atom *) node_iter((Node *) it, define->name))) {
                    if (!sv_match(it->node.token.sv, "_")) {
                        for (size_t i = fields_start; i < c->struct_fields.count; i++) {
                            Type_Struct_Field previous = c->struct_fields.data[i];
                            if (sv_eq(previous.name, it->node.token.sv)) {
                                if (previous.spread) {
                                    error_node(
                                        EK_ERROR,
                                        (Node *) it,
                                        "Redefinition of '" SV_Fmt "'",
                                        SV_Arg(it->node.token.sv));
                                    error_node(
                                        EK_NOTE,
                                        previous.spread,
                                        "It was first defined in this structure here by spreading this");

                                    Node_Struct *definition = previous.spread->type.spec.structt->definition;
                                    if (definition->defined_as) {
                                        error_node(
                                            EK_NOTE,
                                            (Node *) definition,
                                            "Here is the structure we are spreading from");
                                    }

                                    exit(c, 1);
                                }
                                error_redefinition(c, (Node *) it, &previous.pos);
                            }
                        }
                    }

                    it->definition_spec->is_local = false;
                    check_definition(c, it, define->expr, define->type, false);

                    const Type_Struct_Field it_field = {
                        .name = it->node.token.sv,
                        .pos = it->node.token.pos,
                        .type = it->node.type,
                    };
                    da_push(&c->struct_fields, it_field);
                }
            } else if (field->kind == NODE_UNARY && field->token.kind == TOKEN_SPREAD) {
                Node_Unary *unary = (Node_Unary *) field;
                check_expr(c, unary->value, REF_NONE);
                type_assert_type(c, unary->value);

                const Type from = type_without_meta(unary->value->type);
                if (!type_kind_eq(from, TYPE_STRUCT)) {
                    error_node(EK_ERROR, unary->value, "Expected structure type, got %s", type_to_cstr(from));
                    exit(c, 1);
                }

                if (from.ref) {
                    error_node(
                        EK_ERROR, unary->value, "Cannot spread %s without dereferencing it first", type_to_cstr(from));
                    exit(c, 1);
                }

                for (size_t i = 0; i < from.spec.structt->fields_count; i++) {
                    Type_Struct_Field it = from.spec.structt->fields[i];
                    if (!sv_match(it.name, "_")) {
                        for (size_t i = fields_start; i < c->struct_fields.count; i++) {
                            Type_Struct_Field previous = c->struct_fields.data[i];
                            if (sv_eq(previous.name, it.name)) {
                                error_node(
                                    EK_ERROR,
                                    unary->value,
                                    "While spreading this structure, we encountered a field '" SV_Fmt
                                    "' that is already defined",
                                    SV_Arg(it.name));
                                error_parts(
                                    EK_NOTE,
                                    previous.name,
                                    previous.pos,
                                    "It was first defined in this structure here");

                                Node_Struct *definition = from.spec.structt->definition;
                                if (definition->defined_as) {
                                    error_node(EK_NOTE, (Node *) definition, "Here is the structure we are spreading");
                                }

                                exit(c, 1);
                            }
                        }
                    }

                    it.pos = unary->value->token.pos;
                    it.spread = unary->value;
                    da_push(&c->struct_fields, it);
                }
            } else {
                unreachable();
            }
        }

        const size_t fields_count = c->struct_fields.count - fields_start;
        if (fields_count) {
            spec->fields = arena_clone(
                &default_arena, &c->struct_fields.data[fields_start], fields_count * sizeof(*c->struct_fields.data));
            spec->fields_count = fields_count;
        }

        c->struct_fields.count = fields_start;
    }
}

void check_expr_compound(Compiler *c, Node_Compound *compound) {
    Node *n = (Node *) compound;

    // For structure literal
    Type_Struct *struct_spec = NULL;
    if (n->type.kind == TYPE_STRUCT) {
        struct_spec = n->type.spec.structt;
    }

    size_t array_count = 0;
    size_t ordered_iota = 0;

    const size_t designated_initializers_count_save = c->designated_initializers.count;
    for (Node *iter = compound->children.head; iter; iter = iter->next) {
        size_t it_iota = 0;
        if (!compound->is_designated) {
            it_iota = ordered_iota++;
        }

        Node *it = iter;
        if (compound->is_designated) {
            assert(it->kind == NODE_BINARY && it->token.kind == TOKEN_SET);
            Node_Binary *it_binary = (Node_Binary *) it;

            if (n->type.kind == TYPE_STRUCT) {
                if (it_binary->lhs->kind != NODE_ATOM || it_binary->lhs->token.kind != TOKEN_IDENT) {
                    error_node(EK_ERROR, it_binary->lhs, "Expected designated initializer to be field name");
                    exit(c, 1);
                }
                Node_Atom *it_field_name = (Node_Atom *) it_binary->lhs;

                bool ok = false;
                for (size_t i = 0; i < struct_spec->fields_count; i++) {
                    Type_Struct_Field field = struct_spec->fields[i];
                    if (sv_eq(field.name, it_field_name->node.token.sv)) {
                        it->token.as.integer = i;
                        ok = true;
                        break;
                    }
                }

                if (!ok) {
                    error_undefined_in(c, &it_field_name->node.token, &n->type, "field");
                }
            } else if (n->type.kind == TYPE_ARRAY || n->type.kind == TYPE_SLICE) {
                check_expr(c, it_binary->lhs, REF_NONE);
                type_assert_numeric(c, it_binary->lhs, false, false);

                const Const_Value value = eval_const_expr(c, it_binary->lhs, false);
                assert(value.kind == CONST_VALUE_INT);

                if (n->type.kind == TYPE_ARRAY &&
                    int128_ge(value.as.integer, int128_from_u64(n->type.spec.array.count), true)) //
                {
                    error_node(
                        EK_ERROR,
                        it_binary->lhs,
                        "Index %s is out of bounds in array of length %zu",
                        int128_to_cstr(value.as.integer),
                        n->type.spec.array.count);
                    exit(c, 1);
                }

                it->token.as.integer = i64_from_int128(c, it_binary->lhs, value.as.integer, true, "index");
            } else if (n->type.kind == TYPE_UNKNOWN_COMPOUND) {
                // Nothing
            } else {
                unreachable();
            }

            if (n->type.kind == TYPE_STRUCT || n->type.kind == TYPE_ARRAY || n->type.kind == TYPE_SLICE) {
                for (size_t i = designated_initializers_count_save; i < c->designated_initializers.count; i++) {
                    const size_t it_index = it->token.as.integer;

                    Node *previous = c->designated_initializers.data[i];
                    if (previous->token.as.integer == it_index) {
                        if (n->type.kind == TYPE_STRUCT) {
                            error_node(
                                EK_ERROR,
                                it,
                                "Multiple initializers passed for field '" SV_Fmt "'",
                                SV_Arg(struct_spec->fields[it_index].name));
                        } else if (n->type.kind == TYPE_ARRAY || n->type.kind == TYPE_SLICE) {
                            error_node(EK_ERROR, it, "Multiple initializers passed for index %zu", it_index);
                        } else {
                            unreachable();
                        }

                        error_node(EK_NOTE, previous, "Passed here already");
                        exit(c, 1);
                    }
                }
                da_push(&c->designated_initializers, it);
            }

            it_iota = it->token.as.integer;
            it = it_binary->rhs;
        } else {
            if (n->type.kind == TYPE_STRUCT) {
                if (it_iota >= struct_spec->fields_count) {
                    error_node(EK_ERROR, it, "Too many ordered initializers");
                    error_node(EK_NOTE, (Node *) struct_spec->definition, "Structure defined here");
                    exit(c, 1);
                }
            } else if (n->type.kind == TYPE_ARRAY) {
                if (it_iota >= n->type.spec.array.count) {
                    error_node(
                        EK_ERROR,
                        it,
                        "Index %zu is out of bounds in array of length %zu",
                        it_iota,
                        n->type.spec.array.count);
                    exit(c, 1);
                }
            } else if (n->type.kind == TYPE_SLICE) {
                // Pass
            } else if (n->type.kind == TYPE_UNKNOWN_COMPOUND) {
                // Pass
            } else {
                unreachable();
            }
        }

        const Type *it_type = NULL;
        if (n->type.kind == TYPE_STRUCT) {
            it_type = &struct_spec->fields[it_iota].type;
        } else if (n->type.kind == TYPE_ARRAY) {
            it_type = n->type.spec.array.element;
        } else if (n->type.kind == TYPE_SLICE) {
            it_type = n->type.spec.slice.element;
            array_count = max(array_count, it_iota + 1);
        } else if (n->type.kind == TYPE_UNKNOWN_COMPOUND) {
            // Pass
        } else {
            unreachable();
        }

        if (!compound->are_children_checked) {
            check_expr(c, it, REF_NONE);
        }

        if (it_type) {
            type_assert(c, it, *it_type);
        }
    }
    c->designated_initializers.count = designated_initializers_count_save;

    compound->are_children_checked = true;
    if (n->type.kind == TYPE_SLICE) {
        Type *element = n->type.spec.slice.element;
        n->type.spec.array.element = element;
        n->type.spec.array.count = array_count;
        n->type.kind = TYPE_ARRAY;
    }
}

void check_expr_call(Compiler *c, Node_Call *call) {
    Node *n = (Node *) call;
    if (!call->fn) {
        call->fn = call->fn_source;
        check_expr(c, call->fn, REF_NONE);
        check_that_type_is_known(c, call->fn);
    }
    call->args_count = 0;

    const Type *fn_type = &call->fn->type;
    if (fn_type->is_meta) {
        if (node_is_runtime_polymorphic_expression(call->fn)) {
            call->is_monomorphization_of_polymorphic_type = true;
        }

        if (call->is_monomorphization_of_polymorphic_type) {
            const size_t monomorph_parameters_begin_save = c->monomorph_parameters.begin;
            c->monomorph_parameters.begin = c->monomorph_parameters.count;

            const Monomorphizing_Site monomorphizing_site_save = c->monomorphizing_site;
            c->monomorphizing_site.expr = (Node *) call;
            c->monomorphizing_site.node = call->fn;

            if (type_meta_kind_eq(*fn_type, TYPE_STRUCT)) {
                Type_Struct *spec = fn_type->spec.structt;

                Node **parameters = arena_alloc(&temp_arena, spec->polymorphs_count * sizeof(*parameters));
                Node  *excess_argument = NULL;
                ll_foreach(arg, &call->args) {
                    Node *it = arg;
                    if (it->kind == NODE_UNARY && it->token.kind == TOKEN_SPREAD) {
                        error_node(EK_ERROR, it, "Cannot spread arguments in a cast expression");
                        exit(c, 1);
                    }

                    size_t it_index = call->args_count;
                    if (it->kind == NODE_BINARY && it->token.kind == TOKEN_SET) {
                        Node_Binary *it_binary = (Node_Binary *) it;
                        it = it_binary->rhs;

                        Node *it_name = it_binary->lhs;
                        bool  ok = false;
                        for (size_t i = 0; i < spec->polymorphs_count; i++) {
                            Node_Polymorph *arg = spec->polymorphs[i];
                            if (sv_eq(arg->name->node.token.sv, it_name->token.sv)) {
                                it_index = i;
                                ok = true;
                                break;
                            }
                        }

                        if (!ok) {
                            error_undefined_in(c, &it_name->token, fn_type, "polymorphic parameter");
                        }

                        arg->token.as.integer = it_index;
                    }
                    check_expr(c, it, REF_NONE);

                    const size_t parts = type_kind_eq(it->type, TYPE_GROUP) ? it->type.spec.group.count : 1;
                    for (size_t i = 0; i < parts; i++) {
                        const size_t n = it_index + i;
                        if (n >= spec->polymorphs_count) {
                            continue;
                        }

                        if (parameters[n]) {
                            error_node(
                                EK_ERROR,
                                arg,
                                "Duplication of polymorphic parameter '" SV_Fmt "'",
                                SV_Arg(spec->polymorphs[n]->name->node.token.sv));

                            error_node(EK_NOTE, parameters[n], "Passed here already");
                            error_node(EK_NOTE, (Node *) spec->definition, "Structure defined here");
                            exit(c, 1);
                        }
                        parameters[n] = arg;
                    }

                    call->args_count += parts;
                    if (!excess_argument && call->args_count > spec->polymorphs_count) {
                        excess_argument = it;
                    }
                }

                // Ensure that all arguments are provided
                {
                    check_call_arity(
                        c,
                        call->fn,
                        call->args_count,
                        call->end,
                        false,
                        spec->polymorphs_count_min,
                        spec->polymorphs_count,
                        excess_argument);

                    size_t not_provided_count = 0;
                    SV     not_provided_name = {0};
                    for (size_t i = 0; i < spec->polymorphs_count; i++) {
                        Node_Polymorph *it = spec->polymorphs[i];
                        if (!parameters[i] && !it->name->definition_spec->is_const_value_evaluated) {
                            not_provided_count++;
                            if (not_provided_count == 1) {
                                not_provided_name = it->name->node.token.sv;
                            } else if (not_provided_count == 2) {
                                error_token_begin(EK_ERROR, call->end);
                                fprintf(
                                    stderr,
                                    "The following polymorphic parameters are not provided: " SV_Fmt ", " SV_Fmt,
                                    SV_Arg(not_provided_name),
                                    SV_Arg(it->name->node.token.sv));
                            } else {
                                fprintf(stderr, ", " SV_Fmt, SV_Arg(it->name->node.token.sv));
                            }
                        }
                    }

                    if (not_provided_count) {
                        if (not_provided_count == 1) {
                            error_token(
                                EK_ERROR,
                                call->end,
                                "Polymorphic parameter '" SV_Fmt "' is not provided",
                                SV_Arg(not_provided_name));
                        } else {
                            error_finalize();
                        }

                        error_node(EK_NOTE, (Node *) spec->definition, "Structure defined here");
                        exit(c, 1);
                    }
                }

                size_t it_index = 0;
                ll_foreach(arg, &call->args) {
                    Node *it = arg;
                    if (it->kind == NODE_BINARY && it->token.kind == TOKEN_SET) {
                        it = ((Node_Binary *) it)->rhs;
                        it_index = arg->token.as.integer;
                    }

                    Node_Polymorph *from_polymorph = spec->polymorphs[it_index++];
                    Node_Polymorph *to_polymorph = NULL;
                    if (it->kind == NODE_POLYMORPH) {
                        to_polymorph = (Node_Polymorph *) it;
                        to_polymorph->is_type = from_polymorph->is_type;
                        to_polymorph->is_arg = from_polymorph->is_arg;
                    }

                    bool done = false;
                    if (from_polymorph->is_type) {
                        if (to_polymorph) {
                            if (to_polymorph->is_monomorphized) {
                                add_monomorph_parameter(
                                    c,
                                    from_polymorph,
                                    to_polymorph->monomorphization_type,
                                    to_polymorph->monomorphization_value,
                                    to_polymorph);
                                done = true;
                            }
                        } else {
                            type_assert_type_or_Type(c, it);
                        }
                    } else {
                        if (to_polymorph) {
                            if (to_polymorph->is_monomorphized) {
                                add_monomorph_parameter(
                                    c,
                                    from_polymorph,
                                    to_polymorph->monomorphization_type,
                                    to_polymorph->monomorphization_value,
                                    to_polymorph);
                                done = true;
                            }
                        } else {
                            type_assert(c, it, from_polymorph->node.type);
                        }
                    }

                    if (!done) {
                        const Const_Value value = eval_const_expr(c, it, false);
                        if (from_polymorph->is_type && value.kind != CONST_VALUE_TYPE &&
                            value.kind != CONST_VALUE_POLYMORPH) //
                        {
                            assert(value.kind == CONST_VALUE_INT && int128_is_zero(value.as.integer));
                            error_node(
                                EK_ERROR, it, "This expression is not a constant type (It is a null RTTI pointer)");
                            exit(c, 1);
                        }
                        add_monomorph_parameter(c, from_polymorph, it->type, value, to_polymorph);
                    }
                }

                for (size_t i = 0; i < spec->polymorphs_count; i++) {
                    Node_Polymorph *polymorph = spec->polymorphs[i];
                    if (polymorph->name->definition_spec->is_const_value_evaluated) {
                        add_monomorph_parameter_default_value(
                            c, polymorph, polymorph->node.type, &polymorph->name->definition_spec->const_value, NULL);
                    }
                }

                arena_reset(&temp_arena, parameters);

                call->fn = monomorphize(c, call->fn, (Node *) call);
                n->type = call->fn->type;

                c->monomorph_parameters.count = c->monomorph_parameters.begin;
                c->monomorph_parameters.begin = monomorph_parameters_begin_save;
                c->monomorphizing_site = monomorphizing_site_save;
            } else {
                unreachable();
            }
        } else {
            call->is_type_cast = true;
            n->type = type_without_meta(*fn_type);

            // Check the arguments and the arity
            {
                Node *excess_argument = NULL;
                ll_foreach(it, &call->args) {
                    if (it->kind == NODE_BINARY && it->token.kind == TOKEN_SET) {
                        error_node(EK_ERROR, it, "Cannot use named arguments in a cast expression");
                        exit(c, 1);
                    }

                    if (it->kind == NODE_UNARY && it->token.kind == TOKEN_SPREAD) {
                        error_node(EK_ERROR, it, "Cannot spread arguments in a cast expression");
                        exit(c, 1);
                    }

                    check_expr(c, it, REF_NONE);
                    call->args_count += type_kind_eq(it->type, TYPE_GROUP) ? it->type.spec.group.count : 1;
                    if (call->args_count > 1) {
                        excess_argument = it;
                    }
                }
                check_call_arity(c, call->fn, call->args_count, call->end, false, 1, 1, excess_argument);
            }

            Node *from = call->args.head;
            Type *from_type = &from->type;
            Type *to_type = &n->type;

            bool same = false;
            bool to_any = false;
            bool to_trait = false;
            bool to_union = false;
            if (type_eq_without_distinct(*to_type, *from_type)) {
                same = true;
            } else if (type_is_trait(*to_type)) {
                to_trait = true;
            } else if (type_is_union(*to_type)) {
                to_union = true;
            } else if (type_is_scalar(*to_type)) {
                // Pass
            } else {
                Type char_type = {.kind = TYPE_CHAR};
                Type char_slice_type = {
                    .kind = TYPE_SLICE,
                    .spec.slice.element = &char_type,
                };
                Type string_type = {.kind = TYPE_STRING};

                if (type_eq(*to_type, string_type) && type_eq(*from_type, char_slice_type)) {
                    same = true;
                } else if (type_eq(*from_type, string_type) && type_eq(*to_type, char_slice_type)) {
                    same = true;
                } else {
                    error_node(EK_ERROR, call->fn_source, "Cannot cast to %s", type_to_cstr(*to_type));
                    exit(c, 1);
                }
            }

            if (!same) {
                if (to_any) {
                    // Pass
                } else if (to_trait) {
                    finalize_untyped_type(c, from);
                    call->type_cast_trait_impl =
                        check_type_satisfies_trait(c, *from_type, to_type->spec.trait, from, -1);
                } else if (to_union) {
                    finalize_untyped_type(c, from);
                    call->type_cast_union_index = get_union_type_index(c, from, *to_type);
                } else if (type_eq_without_distinct(*to_type, c->type_info_pointer_type) && from_type->is_meta) {
                    from->emit_type_info = arena_clone(&default_arena, &from->type, sizeof(from->type));
                    from->emit_type_info->is_meta = false;
                    from->type = c->type_info_pointer_type;
                    same = true;
                } else if (
                    type_eq(*to_type, (Type) {.kind = TYPE_CHAR, .ref = 1}) &&   //
                    from->kind == NODE_ATOM && from->token.kind == TOKEN_STRING) //
                {
                    same = true;
                    from->type = *to_type;
                } else if (type_is_scalar(*to_type)) {
                    type_assert_scalar(c, from);

                    bool ok = true;
                    if (type_kind_eq(*from_type, TYPE_FN) && !from_type->ref) {
                        // fn -> rawptr
                        ok = type_eq(*to_type, (Type) {.kind = TYPE_RAWPTR});
                    } else if (type_kind_eq(*to_type, TYPE_FN) && !to_type->ref) {
                        // rawptr -> fn
                        ok = type_eq(*from_type, (Type) {.kind = TYPE_RAWPTR});
                    } else if (!type_is_pointer(*from_type) && type_is_pointer(*to_type)) {
                        // s64/u64 -> ptr
                        ok = type_kind_eq(*from_type, TYPE_S64) || type_kind_eq(*from_type, TYPE_U64) ||
                             type_kind_eq(*from_type, TYPE_INT);
                    } else if (type_is_pointer(*from_type) && !type_is_pointer(*to_type)) {
                        // ptr -> s64/u64
                        ok = type_kind_eq(*to_type, TYPE_S64) || type_kind_eq(*to_type, TYPE_U64) ||
                             type_kind_eq(*to_type, TYPE_INT);
                    } else if (!type_is_float(*from_type) && type_is_float(*to_type)) {
                        // integer -> float
                        ok = type_is_integer(*from_type);
                    } else if (type_is_float(*from_type) && !type_is_float(*to_type)) {
                        // float -> integer
                        ok = type_is_integer(*to_type);
                        if (ok && type_kind_eq(*from_type, TYPE_FLOAT)) {
                            call->type_cast = TYPE_CAST_NORMAL;

                            // This is guaranted to be a constant expression, since we are casting from 'float'
                            eval_const_expr(c, n, false);
                        }
                    } else if (
                        type_kind_eq(*from_type, TYPE_INT) &&
                        (type_is_integer(*to_type) || type_kind_eq(*to_type, TYPE_ENUM))) //
                    {
                        ok = try_auto_cast_untyped(c, from, n->type);
                        same = true;
                    }

                    if (!ok) {
                        error_node(
                            EK_ERROR,
                            (Node *) call,
                            "Cannot cast %s to %s",
                            type_to_cstr(*from_type),
                            type_to_cstr(*to_type));
                        exit(c, 1);
                    }
                } else {
                    unreachable();
                }
            }

            if (same) {
                call->type_cast = TYPE_CAST_NOP;
            } else if (type_eq(*to_type, (Type) {.kind = TYPE_BOOL})) {
                call->type_cast = TYPE_CAST_TO_BOOL;
            } else if (to_trait) {
                call->type_cast = TYPE_CAST_TO_TRAIT;
            } else if (to_union) {
                call->type_cast = TYPE_CAST_TO_UNION;
            } else {
                call->type_cast = TYPE_CAST_NORMAL;
            }
        }
    } else {
        if (!type_kind_eq(*fn_type, TYPE_FN)) {
            error_node(EK_ERROR, call->fn_source, "Cannot call %s", type_to_cstr(*fn_type));
            exit(c, 1);
        }

        if (fn_type->ref) {
            error_node(
                EK_ERROR, call->fn_source, "Cannot call %s without deferencing it first", type_to_cstr(*fn_type));
            exit(c, 1);
        }

        Call_Checker cc = {0};
        cc.expr = (Node *) call;
        cc.fn = call->fn;
        cc.fn_source = call->fn_source;
        cc.args = call->args;
        cc.end = call->end;

        if (call->fn->kind == NODE_MEMBER) {
            Node_Member *member = (Node_Member *) call->fn;
            cc.is_trait = member->is_trait;
            cc.is_method = member->method != NULL && !member->lhs->type.is_meta;
            if (cc.is_method || cc.is_trait) {
                cc.receiver = member->lhs;
            }
        }

        check_call_arguments(c, &cc, true);
        call->args = cc.args;
        call->args_count = cc.args_count;
        call->is_typed_variadics_direct = cc.is_typed_variadics_direct;
        call->typed_variadics_count = cc.typed_variadics_count;

        if (cc.is_method) {
            if (cc.is_polymorph) {
                Node_Member *member = (Node_Member *) call->fn;
                assert(cc.fn->kind == NODE_FN);
                member->method = (Node_Fn *) cc.fn;
            }
        } else {
            call->fn = cc.fn;
        }

        fn_type = &call->fn->type;

        n->type = *fn_type->spec.fn->return_type;
        if (!call->is_stmt && type_kind_eq(n->type, TYPE_VOID)) {
            error_node(EK_ERROR, n, "This call cannot be used as a value as it does not return anything");
            exit(c, 1);
        }
    }
}

void check_expr_index(Compiler *c, Node_Index *index, Ref_Kind ref, bool *is_ref_valid) {
    Node *n = (Node *) index;
    check_expr(c, index->lhs, ref);
    check_that_type_is_known(c, index->lhs);

    *is_ref_valid = true; // check_node() has already determined that the reference is valid
    index->is_assign = ref == REF_ASSIGN || ref == REF_ASSIGN_MEMBER;
    if (index->is_ranged) {
        if (index->lhs->type.is_meta) {
            error_node(EK_ERROR, index->lhs, "Cannot take slice into %s", type_to_cstr(index->lhs->type));
            exit(c, 1);
        }

        if (index->lhs->type.ref) {
            // The beginning can be inferred to be 0
            if (index->a) {
                check_expr(c, index->a, REF_NONE);
                type_assert_numeric(c, index->a, false, false);
            }

            // The ending CANNOT be inferred
            if (!index->b) {
                error_node(EK_ERROR, n, "Cannot infer end of range from %s", type_to_cstr(index->lhs->type));
                exit(c, 1);
            }

            check_expr(c, index->b, REF_NONE);
            type_assert_numeric(c, index->b, false, false);

            Type element_type = index->lhs->type;
            element_type.ref--;
            n->type = (Type) {
                .kind = TYPE_SLICE,
                .spec.slice.element = arena_clone(&default_arena, &element_type, sizeof(element_type)),
            };
        } else if (
            type_kind_eq(index->lhs->type, TYPE_ARRAY) ||         //
            type_kind_eq(index->lhs->type, TYPE_DYNAMIC_ARRAY) || //
            type_kind_eq(index->lhs->type, TYPE_SLICE) ||         //
            type_kind_eq(index->lhs->type, TYPE_STRING))          //
        {
            // The beginning can be inferred to be the beginning of the slice
            if (index->a) {
                check_expr(c, index->a, REF_NONE);
                type_assert_numeric(c, index->a, false, false);
            }

            // The ending can be inferred to be the ending of the slice
            if (index->b) {
                check_expr(c, index->b, REF_NONE);
                type_assert_numeric(c, index->b, false, false);
            }

            n->type = index->lhs->type;
            if (type_kind_eq(n->type, TYPE_ARRAY) || type_kind_eq(n->type, TYPE_DYNAMIC_ARRAY)) {
                n->type.kind = TYPE_SLICE;
            }
        } else {
            index->overload = get_operator_overload(c, OPERATOR_SLICE, index->lhs, n, index->module);
            assert(index->overload->node.type.kind == TYPE_FN);
            const Type_Fn *fn_spec = index->overload->node.type.spec.fn;

            if (index->a) {
                check_expr(c, index->a, REF_NONE);
                type_assert(c, index->a, fn_spec->args[1].type);
            } else if (!fn_spec->args[1].has_default_value) {
                error_node(EK_ERROR, n, "Cannot infer beginning of range from %s", type_to_cstr(index->lhs->type));
                error_parts(
                    EK_ERROR,
                    fn_spec->args[1].name,
                    fn_spec->args[1].pos,
                    "The method '" SV_Fmt "' does not have a default value for its beginning argument",
                    SV_Arg(OPERATOR_SLICE));
                exit(c, 1);
            }

            if (index->b) {
                check_expr(c, index->b, REF_NONE);
                type_assert(c, index->b, fn_spec->args[2].type);
            } else if (!fn_spec->args[2].has_default_value) {
                error_node(EK_ERROR, n, "Cannot infer end of range from %s", type_to_cstr(index->lhs->type));
                error_parts(
                    EK_ERROR,
                    fn_spec->args[2].name,
                    fn_spec->args[2].pos,
                    "The method '" SV_Fmt "' does not have a default value for its end argument",
                    SV_Arg(OPERATOR_SLICE));
                exit(c, 1);
            }

            n->type = *fn_spec->return_type;
        }

        *is_ref_valid = ref == REF_NONE;
    } else {
        n->is_memory = index->lhs->is_memory;
        if (type_kind_eq(index->lhs->type, TYPE_ARRAY) && !index->lhs->type.ref) {
            check_expr(c, index->a, REF_NONE);
            type_assert_numeric(c, index->a, false, false);
            n->type = *index->lhs->type.spec.array.element;
        } else if (type_kind_eq(index->lhs->type, TYPE_DYNAMIC_ARRAY) && !index->lhs->type.ref) {
            check_expr(c, index->a, REF_NONE);
            type_assert_numeric(c, index->a, false, false);
            n->type = *index->lhs->type.spec.dynamic_array.element;
        } else if (type_kind_eq(index->lhs->type, TYPE_SLICE) && !index->lhs->type.ref) {
            check_expr(c, index->a, REF_NONE);
            type_assert_numeric(c, index->a, false, false);
            n->type = *index->lhs->type.spec.slice.element;
        } else if (type_kind_eq(index->lhs->type, TYPE_STRING) && !index->lhs->type.ref) {
            check_expr(c, index->a, REF_NONE);
            type_assert_numeric(c, index->a, false, false);
            n->type = (Type) {.kind = TYPE_CHAR};
        } else {
            if (index->lhs->type.ref) {
                error_node(EK_ERROR, index->lhs, "Pointers must be converted into slices before they can be indexed");
                if (is_indexable(c, index->lhs, index->lhs->type, index->module)) {
                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "    Here the value is %s. Perhaps it was meant to be dereferenced before indexing?\n",
                        type_to_cstr(index->lhs->type));
                } else {
                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "    A slice can be constructed from a pointer like this:\n"
                        "\n"
                        "        slice := pointer[begin..end]\n"
                        "        slice[index]\n"
                        "\n"
                        "    If you omit the beginning of the slice, it will default to 0. But the end must be provided.\n\n");
                }
                exit(c, 1);
            }

            index->overload = get_operator_overload(c, OPERATOR_INDEX, index->lhs, n, index->module);

            assert(index->overload->node.type.kind == TYPE_FN);
            const Type_Fn *fn_spec = index->overload->node.type.spec.fn;

            check_expr(c, index->a, REF_NONE);
            type_assert(c, index->a, fn_spec->args[1].type);

            n->type = *fn_spec->return_type;
            assert(n->type.ref);
            n->type.ref--;
        }
    }
}

void check_expr_indexable(Compiler *c, Node_Indexable *indexable, Ref_Kind ref, bool *is_ref_valid) {
    Node  *n = (Node *) indexable;
    bool   has_count = true;
    size_t array_count = 0;
    if (indexable->count) {
        check_expr(c, indexable->count, REF_NONE);

        Const_Value count_value = {0};
        if (indexable->count->kind == NODE_POLYMORPH) {
            Node_Polymorph *polymorph = (Node_Polymorph *) indexable->count;
            if (polymorph->is_monomorphized) {
                count_value = polymorph->monomorphization_value;
            } else {
                has_count = false;
            }
        } else {
            type_assert_numeric(c, indexable->count, false, false);
            count_value = eval_const_expr(c, indexable->count, false);
        }

        if (has_count) {
            if (count_value.kind == CONST_VALUE_INT) {
                check_int_limit_ex(c, indexable->count, count_value.as.integer, true, "array capacity");
                array_count = count_value.as.integer.low;
            } else {
                assert(count_value.kind == CONST_VALUE_POLYMORPH);
            }
        }
        check_expr(c, indexable->element, REF_NONE);
    } else if (indexable->is_dynamic) {
        check_expr(c, indexable->element, REF_SLICE);
    } else {
        // The type `[]T` gets compiled to:
        //
        // ```
        // struct {
        //     T  *data;
        //     i64 count;
        // }
        // ```
        //
        // It is not immediately necessary to calculate the properties of T, which allows for recursive definitions.
        check_expr(c, indexable->element, REF_SLICE);
    }
    type_assert_type(c, indexable->element);

    Type *element_type = &indexable->element->type;
    element_type->is_meta = false;
    if (indexable->count) {
        Type_Array spec = {0};
        spec.element = element_type;
        spec.count = array_count;
        if (!has_count) {
            assert(indexable->count->kind == NODE_POLYMORPH);
            spec.count_polymorph = (Node_Polymorph *) indexable->count;
            spec.count_polymorph->is_type = false;
        }

        n->type = (Type) {.kind = TYPE_ARRAY, .is_meta = true, .spec.array = spec};
    } else if (indexable->is_dynamic) {
        n->type = (Type) {
            .kind = TYPE_DYNAMIC_ARRAY,
            .is_meta = true,
            .spec.dynamic_array.element = element_type,
        };
    } else {
        n->type = (Type) {
            .kind = TYPE_SLICE,
            .is_meta = true,
            .spec.slice.element = element_type,
        };
    }

    *is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
}

static_assert(COUNT_NODES == 30, "");
void check_expr(Compiler *c, Node *n, Ref_Kind ref) {
    if (!n) {
        return;
    }
    bool is_ref_valid = false;

    da_push(&c->partial_stack, n);
    switch (n->kind) {
    case NODE_ATOM:
        check_expr_atom(c, (Node_Atom *) n, ref, &is_ref_valid);
        break;

    case NODE_EMBED: {
        Node_Embed *embed = (Node_Embed *) n;
        if (!embed->read && !parser_embed(c->parser, embed)) {
            error_node(EK_ERROR, n, "Could not read file '" SV_Fmt "'", SV_Arg(embed->path.as.string));
            exit(c, 1);
        }
        n->type = c->char_slice_type;
    } break;

    case NODE_GROUP:
        check_expr_group(c, (Node_Group *) n, ref, &is_ref_valid);
        break;

    case NODE_UNARY:
        check_expr_unary(c, (Node_Unary *) n, &is_ref_valid);
        break;

    case NODE_BINARY:
        check_expr_binary(c, (Node_Binary *) n, true);
        break;

    case NODE_MEMBER:
        check_expr_member(c, (Node_Member *) n, ref, &is_ref_valid);
        break;

    case NODE_IMPORT: {
        Node_Import *import = (Node_Import *) n;
        if (import->libraries.head) {
            ll_foreach(it, &import->libraries) {
                link_flags_add_libname(c->link_flags, it->token.as.string);
            }
        } else {
            make_sure_import_is_ready(c, import);
        }
        n->type = (Type) {.kind = TYPE_MODULE, .spec.module = import->module};
    } break;

    case NODE_DISTINCT: {
        Node_Distinct *distinct = (Node_Distinct *) n;
        if (!distinct->defined_as) {
            error_node(EK_ERROR, n, "A distinct type must be defined as a constant before it can be used");
            exit(c, 1);
        }

        check_expr(c, distinct->value, REF_NONE);
        type_assert_type(c, distinct->value);
        n->type = distinct->value->type;
        n->type.distinct = distinct->defined_as;
    } break;

    case NODE_POLYMORPH:
        // Pass
        break;

    case NODE_INTERPOLATION: {
        Node_Interpolation *interpolation = (Node_Interpolation *) n;
        ll_foreach(it, &interpolation->children) {
            check_expr(c, it, REF_NONE);
            if (type_kind_eq(it->type, TYPE_GROUP)) {
                error_node(
                    EK_ERROR,
                    it,
                    "Cannot have grouped expressions inside an interpolated string. The type of this is %s",
                    type_to_cstr(it->type));
                exit(c, 1);
            }
            type_assert(c, it, c->any_type);
            interpolation->children_count++;
        }
        n->type = c->interpolation_type;
    } break;

    case NODE_FN:
        check_fn(c, (Node_Fn *) n, ref, &is_ref_valid, false, false);
        break;

    case NODE_ENUM:
        check_expr_enum(c, (Node_Enum *) n);
        break;

    case NODE_TRAIT: {
        check_expr_trait(c, (Node_Trait *) n);
    } break;

    case NODE_UNION:
        check_expr_union(c, (Node_Union *) n);
        break;

    case NODE_STRUCT:
        check_expr_struct(c, (Node_Struct *) n);
        break;

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        if (compound->lhs) {
            check_expr(c, compound->lhs, REF_NONE);
            type_assert_type(c, compound->lhs);

            n->type = type_without_meta(compound->lhs->type);
            if (n->type.ref ||
                (n->type.kind != TYPE_STRUCT && n->type.kind != TYPE_ARRAY && n->type.kind != TYPE_SLICE)) {
                error_node(EK_ERROR, compound->lhs, "Expected structure or array type, got %s", type_to_cstr(n->type));
                exit(c, 1);
            }
        } else {
            n->type = (Type) {.kind = TYPE_UNKNOWN_COMPOUND};
        }

        check_expr_compound(c, compound);
        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
        n->is_memory = true;
    } break;

    case NODE_CALL:
        check_expr_call(c, (Node_Call *) n);
        break;

    case NODE_INDEX:
        check_expr_index(c, (Node_Index *) n, ref, &is_ref_valid);
        break;

    case NODE_INDEXABLE:
        check_expr_indexable(c, (Node_Indexable *) n, ref, &is_ref_valid);
        break;

    default:
        unreachable();
    }
    c->partial_stack.count--;

    if (!is_ref_valid) {
        switch (ref) {
        case REF_NONE:
        case REF_SLICE:
            // OK
            break;

        case REF_ADDR:
        case REF_ADDR_MEMBER:
            if (!n->type.is_meta) {
                error_node(EK_ERROR, n, "Cannot take reference to value not in memory");
                exit(c, 1);
            }
            break;

        case REF_ASSIGN:
        case REF_ASSIGN_MEMBER:
            error_node(EK_ERROR, n, "Cannot assign to value not in memory");
            exit(c, 1);
            break;
        }
    }

    if (node_is_runtime_polymorphic_expression(n) && !n->is_called) {
        if (ref == REF_ADDR || ref == REF_ADDR_MEMBER) {
            return;
        }

        if (type_kind_eq(n->type, TYPE_FN)) {
            Node_Fn *literal = get_function_literal(n);
            assert(literal);

            Node *literal_body_save = literal->body;
            literal->body = NULL;
            error_node(
                EK_ERROR,
                n,
                "Polymorphic %s cannot be used as runtime expressions. They must be called",
                literal->is_method ? "methods" : "functions");

            error_node(EK_NOTE, (Node *) literal, "%s defined here", literal->is_method ? "Method" : "Function");
            literal->body = literal_body_save;
        } else if (type_meta_kind_eq(n->type, TYPE_STRUCT)) {
            Node_Struct *structure = n->type.spec.structt->definition;

            const Token fields_end_save = structure->fields_end;
            structure->fields_end = structure->polymorphs_end;

            error_node(EK_ERROR, n, "Polymorphic structures cannot be used directly. They must be monomorphized first");
            afprintf(
                stderr,
                ANSI_COLOR_YELLOW | ANSI_BOLD,
                "    Call the structure like a function, providing the polymorphic parameters as arguments\n\n");
            error_node(EK_NOTE, (Node *) structure, "Structure defined here");

            structure->fields_end = fields_end_save;
        } else {
            unreachable();
        }
        exit(c, 1);
    }
}

void check_fn(
    Compiler *c,
    Node_Fn  *fn,
    Ref_Kind  ref,
    bool     *is_ref_valid,
    bool      only_check_polymorphic_parameters,
    bool      only_check_signature) //
{
    if (fn->checked_signature && (only_check_signature || only_check_polymorphic_parameters)) {
        return;
    }

    Node *n = (Node *) fn;

    Context_Fn      *context_fn_save = c->context.fn;
    Context_Replace *context_replace_save = c->context.replace;

    Context_Fn context = {0};
    context.fn = fn;
    if (fn->checked_signature) {
        for (Context_Fn *f = c->context.fn; f; f = f->outer) {
            if (f->fn == fn->outer_fn) {
                context.outer = f;
                break;
            }
        }

        c->context.replace = fn->context_replace;
    } else {
        context.outer = c->context.fn;
    }
    context_push_fn(&c->context, &context);

    Type_Fn *fn_spec = n->type.spec.fn;
    if (!fn_spec) {
        fn_spec = arena_alloc(&default_arena, sizeof(*fn_spec));
        fn_spec->polymorphs = arena_alloc(&default_arena, fn->polymorphs.count * sizeof(*fn_spec->polymorphs));
        fn_spec->is_noreturn = fn->is_noreturn;

        ll_foreach(it, &fn->polymorphs) {
            assert(!it->is_monomorphized);
            Node_Atom *previous = context_find_define_in_fn(&c->context, c->context.fn, it->name->node.token.sv);
            if (previous) {
                error_redefinition(c, (Node *) it->name, &previous->node.token.pos);
            }
            context_push_define(&c->context, it->name);

            it->name->node.type = (Type) {
                .kind = TYPE_POLYMORPH,
                .spec.polymorph.definition = it,
                .spec.polymorph.is_definition = true,
                .is_meta = true,
            };

            check_polymorph(c, it);
            fn_spec->polymorphs[fn_spec->polymorphs_count++] = it;
        }
    } else {
        ll_foreach(it, &fn->polymorphs) {
            context_push_define(&c->context, it->name);
        }
    }

    ll_foreach(it, &fn->monomorphs) {
        assert(it->is_monomorphized);

        if (it->is_type) {
            assert(it->monomorphization_value.kind == CONST_VALUE_TYPE);
            it->node.type = it->monomorphization_value.as.type;
            it->node.type.is_meta = true;
        } else {
            it->node.type = it->monomorphization_type;
        }

        context_push_define(&c->context, it->name);
    }

    if (only_check_polymorphic_parameters) {
        goto end;
    }

    if (fn->checked_signature) {
        ll_foreach(arg, &fn->args) {
            assert(arg->kind == NODE_DEFINE);
            Node_Define *define = (Node_Define *) arg;

            assert(define->name->kind == NODE_ATOM);
            Node_Atom *it = (Node_Atom *) define->name;
            it->definition_spec->fn_context = c->context.fn;

            context_push_define(&c->context, it);
        }

        if (fn->defined_as) {
            fn->defined_as->definition_spec->check_status = CHECKED;
        }
    } else {
        fn_spec->args_count = fn->args_count;
        fn_spec->args_count_min = fn->args_count_min;
        if (fn->trait_method) {
            assert(fn->is_type);
            fn_spec->args_count++;
            fn_spec->args_count_min++;
        }

        fn_spec->args = arena_alloc(&default_arena, fn_spec->args_count * sizeof(*fn_spec->args));
        fn_spec->variadics_kind = fn->variadics_kind;

        size_t iota = 0;
        if (fn->trait_method) {
            assert(fn->trait_method->node.type.kind == TYPE_TRAIT);

            Type_Fn_Arg *it_arg = &fn_spec->args[iota++];
            it_arg->name = sv_from_cstr("this");
            it_arg->pos = fn->trait_method->node.type.spec.trait->definition->node.token.pos;
            it_arg->type.kind = TYPE_RAWPTR;
        }

        for (Node *arg = fn->args.head; arg; arg = arg->next) {
            assert(arg->kind == NODE_DEFINE);
            Node_Define *define = (Node_Define *) arg;

            assert(define->name->kind == NODE_ATOM);
            Node_Atom *it = (Node_Atom *) define->name;
            it->definition_spec->fn_context = c->context.fn;

            if (!sv_match(it->node.token.sv, "_")) {
                Node_Atom *previous = context_find_define_in_fn(&c->context, c->context.fn, it->node.token.sv);
                if (previous && previous != it) {
                    error_redefinition(c, (Node *) it, &previous->node.token.pos);
                }
            }

            Type_Fn_Arg *it_arg = &fn_spec->args[iota];
            it_arg->name = it->node.token.sv;
            it_arg->pos = it->node.token.pos;
            it_arg->polymorph = define->name_polymorph;

            check_stmt(c, arg);
            if (define->has_spread) {
                fn_spec->variadics_index = iota;
                it->node.type.kind = TYPE_SLICE;
                it->node.type.spec.slice.element = &define->type->type;
            }

            const bool dont_allow_polymorphs_save = c->dont_allow_polymorphs;
            if (define->name_polymorph) {
                c->dont_allow_polymorphs = true;
                if (define->type) {
                    define->type->type.is_meta = true;
                    eval_const_expr(c, define->type, false);
                    define->type->type.is_meta = false;
                }
            }

            if (define->expr) {
                if (is_node_caller_location(define->expr)) {
                    it_arg->default_value_is_caller_location = true;
                } else {
                    it->definition_spec->const_value = eval_const_expr(c, define->expr, false);
                    it_arg->default_value = &it->definition_spec->const_value;
                }
                it_arg->has_default_value = true;
            }
            c->dont_allow_polymorphs = dont_allow_polymorphs_save;

            it_arg->type = it->node.type;
            iota += define->count;
        }

        if (fn->returns.head) {
            fn_spec->returns = arena_alloc(&default_arena, fn->returns_count * sizeof(*fn_spec->returns));

            size_t iota = 0;
            ll_foreach(it, &fn->returns) {
                check_expr(c, it, REF_NONE);
                type_assert_type(c, it);
                fn_spec->returns[iota++] = type_without_meta(it->type);
            }
        }
        fn_spec->returns_count = fn->returns_count;

        Type return_type = {0};
        if (fn_spec->returns_count == 0) {
            return_type.kind = TYPE_VOID;
        } else if (fn_spec->returns_count == 1) {
            return_type = *fn_spec->returns;
        } else {
            return_type.kind = TYPE_GROUP;
            return_type.spec.group.data = fn_spec->returns;
            return_type.spec.group.count = fn_spec->returns_count;
        }
        fn_spec->return_type = arena_clone(&default_arena, &return_type, sizeof(return_type));

        n->type = (Type) {.kind = TYPE_FN, .spec.fn = fn_spec};

        if (fn->defined_as && type_kind_eq(fn->defined_as->node.type, TYPE_VOID) && !only_check_signature) {
            // The body of a function is irrelevant for outer expressions
            fn->defined_as->node.type = n->type;
            fn->defined_as->definition_spec->check_status = CHECKED;

            fn->defined_as->definition_spec->const_value = const_value_fn(fn);
            fn->defined_as->definition_spec->is_const_value_evaluated = true;
        }
    }

    if (fn->is_method) {
        if (!fn->defined_as) {
            Node_Define *define = (Node_Define *) fn->args.head;
            assert(define);

            error_node(EK_ERROR, (Node *) fn, "Anonymous function cannot be a method");
            error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
            exit(c, 1);
        }

        assert(fn->defined_as);
        const SV name = fn->defined_as->node.token.sv;
        if (sv_match(name, "+") || sv_match(name, "-") || sv_match(name, "*") || sv_match(name, "/") ||
            sv_match(name, "%")) //
        {
            check_signature_of_arithmetic_operator(c, fn, fn_spec);
        } else if (sv_match(name, "<=>")) {
            check_signature_of_binary_comparison_operator(c, fn, fn_spec);
            fn->is_compare_operator_complete = type_eq(*fn_spec->return_type, c->ordering_type);
        } else if (sv_match(name, "[]")) {
            check_signature_of_index_operator(c, fn, fn_spec);
        } else if (sv_match(name, "[..]")) {
            check_signature_of_slice_operator(c, fn, fn_spec);
        }
    }
    fn->checked_signature = true;

    if (fn->is_type) {
        n->type.is_meta = true;
        if (is_ref_valid) {
            *is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
        }
    } else if (fn->body && !fn->polymorphs.count && !only_check_signature) {
        check_stmt(c, fn->body);

        if ((fn_spec->is_noreturn || fn_spec->returns_count) && !always_returns(fn->body)) {
            assert(fn->body->kind == NODE_BLOCK);
            Node_Block *block = (Node_Block *) fn->body;
            if (fn_spec->is_noreturn) {
                error_token(
                    EK_ERROR, block->end, "This function is marked as 'noreturn', but control flow reaches here");
            } else {
                error_token(EK_ERROR, block->end, "Expected to return %s", type_to_cstr(*fn_spec->return_type));
            }
            exit(c, 1);
        }
    }

    if (!only_check_signature || !fn->body) {
        fn->checked_fully = true;
    }

end:
    context_restore_fn(&c->context, context_fn_save);
    c->context.replace = context_replace_save;
}
