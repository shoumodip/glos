#include "../error.h"
#include "checker.h"

static_assert(COUNT_TYPES == 27, "");
Const_Value default_const_value(Compiler *c, Type type) {
    if (type.ref) {
        return const_value_u64(0);
    }

    switch (type.kind) {
    case TYPE_BOOL:
    case TYPE_CHAR:

    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_INT:

    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:

    case TYPE_RAWPTR:
    case TYPE_FN:
    case TYPE_ENUM:
        return const_value_u64(0);

    case TYPE_TRAIT:
        return const_value_trait((Const_Value_Trait) {0});

    case TYPE_UNION:
        return const_value_union((Const_Value_Union) {.spec = type.spec.unionn});

    case TYPE_STRUCT: {
        Const_Value_Struct structure = {0};
        structure.spec = type.spec.structt;
        structure.fields = arena_alloc(&default_arena, structure.spec->fields_count * sizeof(*structure.fields));
        for (size_t i = 0; i < structure.spec->fields_count; i++) {
            structure.fields[i] = default_const_value(c, structure.spec->fields[i].type);
        }
        return const_value_struct(structure);
    }

    case TYPE_ARRAY: {
        Const_Value_Array array = {0};
        array.count = type.spec.array.count;
        array.data = arena_alloc(&default_arena, array.count * sizeof(*array.data));
        array.element_type = type.spec.array.element;
        for (size_t i = 0; i < array.count; i++) {
            array.data[i] = default_const_value(c, *array.element_type);
        }
        return const_value_array(array);
    }

    case TYPE_DYNAMIC_ARRAY:
        return const_value_dynamic_array(type.spec.dynamic_array.element);

    case TYPE_SLICE: {
        Const_Value_Array array = {0};
        array.is_slice = true;
        array.element_type = type.spec.slice.element;
        return const_value_array(array);
    }

    case TYPE_STRING:
        return const_value_string((SV) {0});

    default:
        unreachable();
    }
}

// n    -> Final type (trait)
// type -> Original type
Const_Value const_value_to_trait(Node *n, Type *type, Type_Trait_Impl *impl, Const_Value value) {
    Const_Value_Trait trait = {0};
    assert(n->type.kind == TYPE_TRAIT);
    trait.impl = impl;
    trait.data = arena_clone(&default_arena, &value, sizeof(value));
    trait.type = type;
    return const_value_trait(trait);
}

Const_Value const_value_to_union(Type union_type, size_t union_index, Const_Value value) {
    Const_Value_Union unionn = {0};
    assert(union_type.kind == TYPE_UNION);
    unionn.spec = union_type.spec.unionn;
    unionn.index = union_index;
    unionn.real = arena_clone(&default_arena, &value, sizeof(value));
    return const_value_union(unionn);
}

bool eval_const_binary_equality(Compiler *c, Node_Binary *binary) {
    Const_Value lhs = eval_const_expr(c, binary->lhs, false);
    Const_Value rhs = eval_const_expr(c, binary->rhs, false);

    if (binary->trait_check) {
        Const_Value trait;
        Const_Value type;
        if (binary->trait_check == binary->lhs) {
            trait = lhs;
            type = rhs;
        } else {
            trait = rhs;
            type = lhs;
        }
        assert(trait.kind == CONST_VALUE_TRAIT);

        if (type.kind == CONST_VALUE_INT && int128_is_zero(type.as.integer)) {
            return !trait.as.trait.type;
        }

        assert(type.kind == CONST_VALUE_TYPE);
        if (!trait.as.trait.type) {
            return false;
        }

        return type_eq(*trait.as.trait.type, type_without_meta(type.as.type));
    }

    if (binary->union_check) {
        Const_Value unionn;
        Const_Value variant;
        if (binary->union_check == binary->lhs) {
            unionn = lhs;
            variant = rhs;
        } else {
            unionn = rhs;
            variant = lhs;
        }
        assert(unionn.kind == CONST_VALUE_UNION);

        if (variant.kind == CONST_VALUE_INT && int128_is_zero(variant.as.integer)) {
            return unionn.as.unionn.index == 0;
        }

        assert(variant.kind == CONST_VALUE_TYPE);
        if (unionn.as.unionn.index == 0) {
            return false;
        }

        return type_eq(
            unionn.as.unionn.spec->variants[unionn.as.unionn.index - 1].type, type_without_meta(variant.as.type));
    }

    return const_value_eq(lhs, rhs);
}

Const_Value const_value_of_var(Compiler *c, Node_Atom *var) {
    if (!var->definition_spec->is_const_value_evaluated) {
        var->definition_spec->const_value = default_const_value(c, var->node.type);
        var->definition_spec->is_const_value_evaluated = true;
    }

    return var->definition_spec->const_value;
}

Const_Value eval_const_expr_atom(Compiler *c, Node_Atom *atom, bool ref) {
    Node *n = (Node *) atom;

    static_assert(COUNT_TOKENS == 78, "");
    switch (n->token.kind) {
    case TOKEN_INT:
    case TOKEN_BOOL:
    case TOKEN_CHAR:
        return const_value_u64(n->token.as.integer);

    case TOKEN_NULL:
        return const_value_u64(0);

    case TOKEN_IDENT:
        if (atom->definition && atom->definition->polymorph) {
            Node_Polymorph *polymorph = atom->definition->polymorph;
            if (polymorph->is_monomorphized) {
                return polymorph->monomorphization_value;
            }

            if (c->dont_allow_polymorphs) {
                error_node(
                    EK_ERROR,
                    n,
                    "Cannot use polymorphic parameters in a constant expression before they are monomorphized");
                error_node(EK_NOTE, (Node *) polymorph, "Here is the polymorphic parameter being used");
                exit(c, 1);
            }

            return eval_const_expr(c, (Node *) polymorph, false);
        }

        if (n->type.is_meta) {
            return const_value_type(n->type);
        }

        if (!atom->definition) {
            // The only reason why it reached this point
            assert(c->dont_allow_polymorphs);
            return const_value_u64(0);
        }

        if (!atom->definition->definition_spec->is_const) {
            if (atom->definition->definition_spec->is_local && !atom->definition->definition_spec->static_var_fn) {
                error_node(EK_ERROR, n, "Cannot use local variables in a constant expression");
                error_node(EK_NOTE, (Node *) atom->definition, "Here is the variable being used");
                exit(c, 1);
            }

            if (ref) {
                return const_value_var(atom->definition);
            }

            return const_value_of_var(c, atom->definition);
        }

        assert(!ref);
        return atom->definition->definition_spec->const_value;

    case TOKEN_STRING:
        if (type_eq(n->type, (Type) {.kind = TYPE_CHAR, .ref = 1})) {
            error_node(
                EK_ERROR,
                n,
                "Cannot access pointers in constant expressions. (This string literal is auto casted to %s)",
                type_to_cstr(n->type));
            exit(c, 1);
        }
        return const_value_string(n->token.as.string);

    case TOKEN_ISTRING:
        return const_value_string(n->token.as.string);

    case TOKEN_DIRECTIVE_MAIN:
        return const_value_fn(get_main(c));

    case TOKEN_DIRECTIVE_PLATFORM:
        return get_platform(c, NULL);

    default:
        unreachable();
    }
}

Const_Value eval_const_expr_unary(Compiler *c, Node_Unary *unary) {
    Node *n = (Node *) unary;
    if (unary->overload) {
        error_node(EK_ERROR, n, "Cannot call operator overload in compile time expressions");
        error_node(EK_NOTE, (Node *) unary->overload->defined_as, "This is the overload used");
        exit(c, 1);
    }

    Const_Value value = {0};

    static_assert(COUNT_TOKENS == 78, "");
    switch (n->token.kind) {
    case TOKEN_SUB:
        value = eval_const_expr(c, unary->value, false);
        return const_value_int(int128_neg(value.as.integer));

    case TOKEN_MUL:
        value = eval_const_expr(c, unary->value, false);
        if (value.kind == CONST_VALUE_VAR) {
            return const_value_of_var(c, value.as.var);
        }

        error_node(EK_ERROR, n, "This expression is not constant at compile time");
        exit(c, 1);
        break;

    case TOKEN_BAND:
        value = eval_const_expr(c, unary->value, true);
        if (value.kind == CONST_VALUE_TYPE) {
            value.as.type.ref++;
        }
        return value;

    case TOKEN_BNOT:
        value = eval_const_expr(c, unary->value, false);
        return const_value_int(int128_not(value.as.integer));

    case TOKEN_LNOT:
        value = eval_const_expr(c, unary->value, false);
        return const_value_u64(int128_is_zero(value.as.integer));

    case TOKEN_SIZEOF:
        if (c->dont_allow_polymorphs) {
            eval_const_expr(c, unary->value, false);
        }

        if (unary->value->type.kind == TYPE_POLYMORPH &&
            !unary->value->type.spec.polymorph.definition->is_monomorphized) //
        {
            return const_value_u64(0);
        }
        return const_value_u64(compile_sizeof(c, &unary->value->type));

    case TOKEN_TYPEOF: {
        finalize_untyped_type(c, unary->value);
        Type type = unary->value->type;
        type.is_meta = true;
        return const_value_type(type);
    }

    default:
        unreachable();
    }
}

Const_Value eval_const_expr_binary(Compiler *c, Node_Binary *binary) {
    Node *n = (Node *) binary;

    Const_Value lhs = {0};
    Const_Value rhs = {0};
    if (type_eq(binary->lhs->type, (Type) {.kind = TYPE_STRING}) && token_kind_to_power(n->token.kind) == POWER_CMP) {
        lhs = eval_const_expr(c, binary->lhs, false);
        assert(lhs.kind == CONST_VALUE_STRING);

        rhs = eval_const_expr(c, binary->rhs, false);
        assert(rhs.kind == CONST_VALUE_STRING);

        const int ordering = sv_cmp(lhs.as.string, rhs.as.string);
        switch (n->token.kind) {
        case TOKEN_GT:
            return const_value_u64(ordering > 0);

        case TOKEN_GE:
            return const_value_u64(ordering >= 0);

        case TOKEN_LT:
            return const_value_u64(ordering < 0);

        case TOKEN_LE:
            return const_value_u64(ordering <= 0);

        case TOKEN_EQ:
            return const_value_u64(ordering == 0);

        case TOKEN_NE:
            return const_value_u64(ordering != 0);

        default:
            unreachable();
        }
    }

    if (binary->overload) {
        error_node(EK_ERROR, n, "Cannot call operator overload in compile time expressions");
        error_node(EK_NOTE, (Node *) binary->overload->defined_as, "This is the overload used");
        exit(c, 1);
    }

    // Arithmetic operations
    {
        typedef Int128 (*Int_Op)(Int128 lhs, Int128 rhs, bool is_signed);

        static_assert(COUNT_TOKENS == 78, "");
        static const Int_Op ops[COUNT_TOKENS] = {
            [TOKEN_ADD] = int128_add,
            [TOKEN_SUB] = int128_sub,
            [TOKEN_MUL] = int128_mul,
            [TOKEN_DIV] = int128_div,
            [TOKEN_MOD] = int128_mod,

            [TOKEN_SHL] = int128_shl,
            [TOKEN_SHR] = int128_shr,
            [TOKEN_BOR] = int128_or,
            [TOKEN_BAND] = int128_and,
        };

        const Int_Op op = ops[n->token.kind];
        if (op) {
            lhs = eval_const_expr(c, binary->lhs, false);
            rhs = eval_const_expr(c, binary->rhs, false);

            if ((n->token.kind == TOKEN_DIV || n->token.kind == TOKEN_MOD) && int128_is_zero(rhs.as.integer)) {
                error_node(EK_ERROR, binary->rhs, "Cannot divide by zero");
                exit(c, 1);
            }

            return const_value_int(op(lhs.as.integer, rhs.as.integer, type_is_signed(n->type)));
        }
    }

    // Arithmetic comparisons
    {
        typedef bool (*Int_Op)(Int128 lhs, Int128 rhs, bool is_signed);

        static_assert(COUNT_TOKENS == 78, "");
        static const Int_Op ops[COUNT_TOKENS] = {
            [TOKEN_GT] = int128_gt,
            [TOKEN_GE] = int128_ge,
            [TOKEN_LT] = int128_lt,
            [TOKEN_LE] = int128_le,
        };

        const Int_Op op = ops[n->token.kind];
        if (op) {
            lhs = eval_const_expr(c, binary->lhs, false);
            rhs = eval_const_expr(c, binary->rhs, false);
            return const_value_u64(op(lhs.as.integer, rhs.as.integer, type_is_signed(n->type)));
        }
    }

    static_assert(COUNT_TOKENS == 78, "");
    switch (n->token.kind) {
    case TOKEN_LOR:
        lhs = eval_const_expr(c, binary->lhs, false);
        assert(lhs.kind == CONST_VALUE_INT);
        if (!int128_is_zero(lhs.as.integer)) {
            return lhs;
        }

        rhs = eval_const_expr(c, binary->rhs, false);
        return rhs;

    case TOKEN_LAND:
        lhs = eval_const_expr(c, binary->lhs, false);
        assert(lhs.kind == CONST_VALUE_INT);
        if (!int128_is_zero(lhs.as.integer)) {
            return lhs;
        }

        rhs = eval_const_expr(c, binary->rhs, false);
        return rhs;

    case TOKEN_EQ:
        return const_value_u64(eval_const_binary_equality(c, binary));

    case TOKEN_NE:
        return const_value_u64(!eval_const_binary_equality(c, binary));

    default:
        unreachable();
        break;
    }
}

Const_Value eval_const_expr_member(Compiler *c, Node_Member *member) {
    Node *n = (Node *) member;
    if (member->is_enum) {
        return const_value_i64(member->enum_value);
    }

    if (member->method) {
        return const_value_fn(member->method);
    }

    Const_Value lhs = eval_const_expr(c, member->lhs, false);
    while (lhs.kind == CONST_VALUE_VAR) {
        lhs = const_value_of_var(c, lhs.as.var);
    }

    static_assert(COUNT_CONST_VALUES == 12, "");
    switch (lhs.kind) {
    case CONST_VALUE_TRAIT: {
        if (member->rhs) {
            if (!lhs.as.trait.type || !type_eq(n->type, *lhs.as.trait.type)) {
                error_token_range(
                    EK_ERROR,
                    member->dot,
                    member->rhs_end,
                    "Type Mismatch: Accessing %s, but real type is %s",
                    type_to_cstr(n->type),
                    lhs.as.trait.type ? type_to_cstr(*lhs.as.trait.type) : "null");
                exit(c, 1);
            }

            assert(lhs.as.trait.data); // The type is checked, that means a real value exists
            return *lhs.as.trait.data;
        } else if (member->is_trait) {
            if (!lhs.as.trait.impl) {
                error_node(EK_ERROR, n, "Cannot access method of null trait");
                exit(c, 1);
            }

            Node_Fn *fn = lhs.as.trait.impl->methods[member->trait_method].fn;
            return const_value_fn(
                create_trait_method_wrapper(&default_arena, fn, lhs.as.trait.impl->trait, member->trait_method));
        } else if (member->field_index == 0) {
            if (lhs.as.trait.type) {
                return const_value_type(*lhs.as.trait.type);
            }

            return const_value_u64(0);
        } else if (member->field_index == 1 || member->field_index == 2) {
            error_node(EK_ERROR, n, "Cannot access pointers in constant expressions");
            exit(c, 1);
        } else {
            unreachable();
        }
    }

    case CONST_VALUE_UNION:
        if (member->rhs) {
            if (member->union_index != lhs.as.unionn.index) {
                const Type_Union *spec = lhs.as.unionn.spec;
                error_token_range(
                    EK_ERROR,
                    member->dot,
                    member->rhs_end,
                    "Type Mismatch: Accessing %s, but real type is %s",
                    member->union_index ? type_to_cstr(spec->variants[member->union_index - 1].type) : "null",
                    lhs.as.unionn.index ? type_to_cstr(spec->variants[lhs.as.unionn.index - 1].type) : "null");
                exit(c, 1);
            }

            assert(lhs.as.unionn.real); // The type is checked, that means a real value exists
            return *lhs.as.unionn.real;
        } else if (member->field_index == 0) {
            return const_value_u64(lhs.as.unionn.index);
        } else {
            unreachable();
        }

    case CONST_VALUE_STRUCT:
        return lhs.as.structt.fields[member->field_index];

    case CONST_VALUE_ARRAY:
        if (member->field_index == 0) {
            error_node(EK_ERROR, n, "Cannot access pointers in constant expressions");
            exit(c, 1);
        } else if (member->field_index == 1) {
            return const_value_u64(lhs.as.array.count);
        } else {
            unreachable();
        }

    case CONST_VALUE_DYNAMIC_ARRAY:
        if (member->field_index == 0) {
            error_node(EK_ERROR, n, "Cannot access pointers in constant expressions");
            exit(c, 1);
        } else if (member->field_index == 1 || member->field_index == 2) {
            return const_value_u64(0); // Dynamic arrays in constant expressions can only be empty ones
        } else {
            unreachable();
        }

    case CONST_VALUE_STRING:
        if (member->field_index == 0) {
            error_node(EK_ERROR, n, "Cannot access pointers in constant expressions");
            exit(c, 1);
        } else if (member->field_index == 1) {
            return const_value_u64(lhs.as.string.count);
        } else {
            unreachable();
        }

    case CONST_VALUE_MODULE: {
        Node_Atom *definition = member->module_access_definition;
        assert(definition);

        if (n->type.is_meta) {
            return const_value_type(n->type);
        }

        if (!definition->definition_spec->is_const) {
            error_node(EK_ERROR, n, "Cannot use variables in a constant expression");
            exit(c, 1);
        }

        return definition->definition_spec->const_value;
    }

    default:
        unreachable();
    }
}

Const_Value eval_const_expr_interpolation(Compiler *c, Node_Interpolation *interpolation) {
    Node *n = (Node *) interpolation;
    interpolation->is_constant = true;

    const size_t start = default_sb.count;
    ll_foreach(it, &interpolation->children) {
        assert(type_eq(it->type, c->any_type));

        Type it_type_save;
        if (it->auto_casts) {
            assert(it->auto_casts_count == 1);
            assert(it->auto_casts[0].kind == AUTO_CAST_TO_TRAIT);
            it_type_save = it->type;
            it->type = it->auto_casts->from;
        }

        const Const_Value result = eval_const_expr_impl(c, it, false);
        sb_push_const_value_raw(&default_sb, it->type, result);

        if (it->auto_casts) {
            it->type = it_type_save;
        }
    }

    const Const_Value string = const_value_string(arena_sb_to_sv(&default_arena, &default_sb, start));
    assert(type_kind_eq(n->type, TYPE_UNION));
    return const_value_to_union(n->type, 1 + interpolation->is_constant, string);
}

Const_Value eval_const_expr_compound(Compiler *c, Node_Compound *compound) {
    Node       *n = (Node *) compound;
    Const_Value value = default_const_value(c, n->type);

    size_t ordered_iota = 0;
    ll_foreach(iter, &compound->children) {
        size_t it_iota = 0;
        if (!compound->is_designated) {
            it_iota = ordered_iota++;
        }

        Node *it = iter;
        if (compound->is_designated) {
            assert(it->kind == NODE_BINARY && it->token.kind == TOKEN_SET);
            Node_Binary *it_binary = (Node_Binary *) it;
            it_iota = it->token.as.integer;
            it = it_binary->rhs;
        }

        if (n->type.kind == TYPE_STRUCT) {
            value.as.structt.fields[it_iota] = eval_const_expr(c, it, false);
        } else if (n->type.kind == TYPE_ARRAY) {
            value.as.array.data[it_iota] = eval_const_expr(c, it, false);
        } else {
            unreachable();
        }
    }

    return value;
}

Const_Value eval_const_expr_call(Compiler *c, Node_Call *call) {
    Node *n = (Node *) call;
    if (!call->is_type_cast) {
        error_node(EK_ERROR, call->fn_source, "Cannot call functions in a constant expression");
        exit(c, 1);
    }

    const Const_Value value = eval_const_expr(c, call->args.head, false);
    if (value.kind == CONST_VALUE_VAR || (!n->type.is_meta && n->type.ref)) {
        error_node(EK_ERROR, n, "This expression is not constant at compile time");
        exit(c, 1);
    }

    static_assert(COUNT_TYPE_CASTS == 5, "");
    switch (call->type_cast) {
    case TYPE_CAST_NOP:
        return value;

    case TYPE_CAST_NORMAL:
        return value;

    case TYPE_CAST_TO_BOOL:
        return const_value_u64(!int128_is_zero(value.as.integer));

    case TYPE_CAST_TO_TRAIT:
        return const_value_to_trait(n, &call->args.head->type, call->type_cast_trait_impl, value);

    case TYPE_CAST_TO_UNION:
        return const_value_to_union(n->type, call->type_cast_union_index, value);

    default:
        unreachable();
    }
}

Const_Value eval_const_expr_index(Compiler *c, Node_Index *index) {
    Node *n = (Node *) index;
    if (index->overload) {
        error_node(EK_ERROR, n, "Cannot call operator overload in compile time expressions");
        error_node(EK_NOTE, (Node *) index->overload->defined_as, "This is the overload used");
        exit(c, 1);
    }

    const Const_Value lhs = eval_const_expr(c, index->lhs, false);
    if (index->is_ranged) {
        if (type_is_pointer(index->lhs->type)) {
            error_node(EK_ERROR, n, "Cannot construct slices from pointers in constant expressions");
            exit(c, 1);
        }

        static_assert(COUNT_CONST_VALUES == 12, "");
        switch (lhs.kind) {
        case CONST_VALUE_ARRAY: {
            Const_Value_Array array = lhs.as.array;

            i64 begin = 0;
            if (index->a) {
                begin = i64_from_int128(
                    c, index->a, eval_const_expr(c, index->a, false).as.integer, true, "beginning of range");
            }

            i64 end = array.count;
            if (index->b) {
                end =
                    i64_from_int128(c, index->a, eval_const_expr(c, index->b, false).as.integer, true, "end of range");
            }

            if (begin > end) {
                index->lhs = NULL;
                error_node(EK_ERROR, n, "Range (%zd..%zd) is invalid: Beginning of range is more than end", begin, end);
                exit(c, 1);
            }

            if (begin < 0 || end < 0 || (size_t) begin > array.count || (size_t) end > array.count) {
                index->lhs = NULL;
                error_node(
                    EK_ERROR, n, "Range (%zd..%zd) is out of bounds in array of length %zu", begin, end, array.count);
                exit(c, 1);
            }

            array.data += begin;
            array.count = end - begin;
            array.is_slice = true;
            return const_value_array(array);
        }

        case CONST_VALUE_DYNAMIC_ARRAY: {
            i64 begin = 0;
            if (index->a) {
                begin = i64_from_int128(
                    c, index->a, eval_const_expr(c, index->a, false).as.integer, true, "beginning of range");
            }

            i64 end = 0;
            if (index->b) {
                end =
                    i64_from_int128(c, index->a, eval_const_expr(c, index->b, false).as.integer, true, "end of range");
            }

            if (begin > end) {
                index->lhs = NULL;
                error_node(EK_ERROR, n, "Range (%zd..%zd) is invalid: Beginning of range is more than end", begin, end);
                exit(c, 1);
            }

            // Constant dynamic arrays can only be empty
            if (begin < 0 || end < 0 || (size_t) begin > 0 || (size_t) end > 0) {
                index->lhs = NULL;
                error_node(EK_ERROR, n, "Range (%zd..%zd) is out of bounds in dynamic array of length 0", begin, end);
                exit(c, 1);
            }

            return lhs;
        }

        case CONST_VALUE_STRING: {
            SV sv = lhs.as.string;

            i64 begin = 0;
            if (index->a) {
                begin = i64_from_int128(
                    c, index->a, eval_const_expr(c, index->a, false).as.integer, true, "beginning of range");
            }

            i64 end = sv.count;
            if (index->b) {
                end =
                    i64_from_int128(c, index->a, eval_const_expr(c, index->b, false).as.integer, true, "end of range");
            }

            if (begin > end) {
                index->lhs = NULL;
                error_node(EK_ERROR, n, "Range (%zd..%zd) is invalid: Beginning of range is more than end", begin, end);
                exit(c, 1);
            }

            if (begin < 0 || end < 0 || (size_t) begin > sv.count || (size_t) end > sv.count) {
                index->lhs = NULL;
                error_node(
                    EK_ERROR, n, "Range (%zd..%zd) is out of bounds in string of length %zu", begin, end, sv.count);
                exit(c, 1);
            }

            sv.data += begin;
            sv.count = end - begin;
            return const_value_string(sv);
        }

        default:
            unreachable();
        }
    } else {
        const i64 at = i64_from_int128(c, index->a, eval_const_expr(c, index->a, false).as.integer, true, "index");

        static_assert(COUNT_CONST_VALUES == 12, "");
        switch (lhs.kind) {
        case CONST_VALUE_ARRAY: {
            if (at < 0 || (size_t) at >= lhs.as.array.count) {
                error_node(
                    EK_ERROR, index->a, "Index %zd is out of bounds in array of length %zu", at, lhs.as.array.count);
                exit(c, 1);
            };

            return lhs.as.array.data[at];
        }

        case CONST_VALUE_DYNAMIC_ARRAY:
            error_node(EK_ERROR, index->a, "Index %zd is out of bounds in dynamic array of length 0", at);
            exit(c, 1);

        case CONST_VALUE_STRING: {
            if (at < 0 || (size_t) at >= lhs.as.string.count) {
                error_node(
                    EK_ERROR, index->a, "Index %zd is out of bounds in string of length %zu", at, lhs.as.string.count);
                exit(c, 1);
            };

            return const_value_u64(lhs.as.string.data[at]);
        }

        default:
            unreachable();
        }
    }
}

static_assert(COUNT_NODES == 29, "");
Const_Value eval_const_expr_impl(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return (Const_Value) {0};
    }

    if (n->emit_type_info) {
        if (c->dont_allow_polymorphs) {
            Type *emit_type_info_save = n->emit_type_info;
            n->emit_type_info = NULL;
            eval_const_expr(c, n, ref);
            n->emit_type_info = emit_type_info_save;
        }

        return const_value_type(*n->emit_type_info);
    }

    if (ref && !n->type.is_meta) {
        if (n->kind != NODE_ATOM || n->token.kind != TOKEN_IDENT) {
            if (n->kind == NODE_MEMBER) {
                Node_Member *member = (Node_Member *) n;
                if (member->module_access_definition) {
                    Node_Atom *var = member->module_access_definition;
                    assert(!var->definition_spec->is_const); // The analyzer should have already checked this
                    return const_value_var(var);
                }
            }
            error_node(EK_ERROR, n, "Can only take reference to variables in a constant expression");
            exit(c, 1);
        }
    }

    switch (n->kind) {
    case NODE_ATOM:
        return eval_const_expr_atom(c, (Node_Atom *) n, ref);

    case NODE_UNARY:
        return eval_const_expr_unary(c, (Node_Unary *) n);

    case NODE_BINARY:
        return eval_const_expr_binary(c, (Node_Binary *) n);

    case NODE_MEMBER:
        return eval_const_expr_member(c, (Node_Member *) n);

    case NODE_IMPORT:
        assert(n->type.kind == TYPE_MODULE);
        return const_value_module(n->type.spec.module);

    case NODE_POLYMORPH: {
        Const_Value_Polymorph spec = {0};
        spec.polymorph = (Node_Polymorph *) n;
        spec.is_definition = true;
        return const_value_polymorph(spec);
    }

    case NODE_DISTINCT:
        assert(n->type.is_meta);
        return const_value_type(n->type);

    case NODE_INTERPOLATION:
        return eval_const_expr_interpolation(c, (Node_Interpolation *) n);

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;
        if (fn->is_type) {
            return const_value_type(fn->node.type);
        } else {
            return const_value_fn(fn);
        }
    }

    case NODE_ENUM:
    case NODE_TRAIT:
    case NODE_UNION:
    case NODE_STRUCT:
        return const_value_type(n->type);

    case NODE_COMPOUND:
        return eval_const_expr_compound(c, (Node_Compound *) n);

    case NODE_CALL:
        return eval_const_expr_call(c, (Node_Call *) n);

    case NODE_INDEX:
        return eval_const_expr_index(c, (Node_Index *) n);

    case NODE_INDEXABLE: {
        Node_Indexable *indexable = (Node_Indexable *) n;
        if (c->dont_allow_polymorphs) {
            eval_const_expr(c, indexable->count, false);
            eval_const_expr(c, indexable->element, false);
        }
        return const_value_type(n->type);
    }

    default:
        unreachable();
        break;
    }
}

Const_Value eval_const_expr(Compiler *c, Node *n, bool ref) {
    Const_Value result = {0};
    if (!n) {
        return result;
    }

    Type n_type_save;
    if (n->auto_casts) {
        assert(n->auto_casts_count == 1); // Functions cannot be called in constant expressions
        n_type_save = n->type;
        n->type = n->auto_casts->from;
    }

    result = eval_const_expr_impl(c, n, ref);
    if (n->auto_casts) {
        n->type = n_type_save;

        static_assert(COUNT_AUTO_CASTS == 5, "");
        switch (n->auto_casts[0].kind) {
        case AUTO_CAST_TO_TRAIT:
            result = const_value_to_trait(n, &n->auto_casts[0].from, n->auto_casts[0].trait_impl, result);
            break;

        case AUTO_CAST_TO_UNION:
            result = const_value_to_union(n->type, n->auto_casts[0].union_index, result);
            break;

        case AUTO_CAST_ARRAY_TO_SLICE:
            assert(result.kind == CONST_VALUE_ARRAY);
            result.as.array.is_slice = true;
            break;

        case AUTO_CAST_DYNAMIC_ARRAY_TO_SLICE: {
            assert(result.kind == CONST_VALUE_DYNAMIC_ARRAY);
            Const_Value_Array array = {0};
            array.element_type = result.as.dynamic_array;
            array.is_slice = true;
            result = const_value_array(array);
        } break;

        default:
            unreachable();
        }
    }

    return result;
}
