#include "../error.h"
#include "checker.h"

const char *order_postfix(size_t n) {
    switch (n % 10) {
    case 1:
        return "st";

    case 2:
        return "nd";

    case 3:
        return "rd";

    default:
        return "th";
    }
}

i64 i64_from_int128(Compiler *c, Node *n, Int128 x, bool min_zero, const char *label) {
    check_int_limit_ex(c, n, x, min_zero, label);
    return x.low;
}

bool type_is_trait(Type type) {
    return !type.ref && type_kind_eq(type, TYPE_TRAIT);
}

bool type_is_union(Type type) {
    return !type.ref && type_kind_eq(type, TYPE_UNION);
}

bool type_eq_without_distinct(Type a, Type b) {
    a.distinct = NULL;
    b.distinct = NULL;
    return type_eq(a, b);
}

bool is_atom_true(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_BOOL && n->token.as.integer;
}

bool is_atom_false(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_BOOL && !n->token.as.integer;
}

bool node_is_null(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_NULL;
}

bool is_node_caller_location(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_DIRECTIVE_CALLER_LOCATION;
}

bool node_is_runtime_polymorphic_expression(Node *n) {
    if (n->is_defined_as_constant) {
        return false;
    }

    if (type_kind_eq(n->type, TYPE_FN) && n->type.spec.fn->polymorphs_count) {
        return n->kind != NODE_FN || ((Node_Fn *) n)->defined_as == NULL;
    }

    if (type_meta_kind_eq(n->type, TYPE_STRUCT) && n->type.spec.structt->polymorphs_count) {
        while (n->kind == NODE_UNARY && n->token.kind == TOKEN_BAND) {
            n = ((Node_Unary *) n)->value;
        }

        return n->kind != NODE_CALL || !((Node_Call *) n)->is_monomorphization_of_polymorphic_type;
    }

    return false;
}

void error_undefined(Compiler *c, const Token *t, const char *label, bool no_exit) {
    error_token(EK_ERROR, *t, "Undefined %s '" SV_Fmt "'", label, SV_Arg(t->sv));
    if (c->current_comptime_conditional_stmt) {
        error_node(EK_NOTE, c->current_comptime_conditional_stmt, "Evaluating this conditional statement");
        afprintf(
            stderr,
            ANSI_COLOR_YELLOW | ANSI_BOLD,
            "    The '#if' statements are evaluated immediately instead of waiting for all the definitions to\n"
            "    be registered. Thus, even though this identifier might be defined later, right now it isn't.\n\n");
    }

    if (!no_exit) {
        exit(c, 1);
    }
}

void error_redefinition(Compiler *c, const Node *n, const Pos *previous_pos) {
    error_token(EK_ERROR, n->token, "Redefinition of '" SV_Fmt "'", SV_Arg(n->token.sv));
    if (previous_pos) {
        SV previous_sv = {0};
        previous_sv.data = previous_pos->line.data + previous_pos->col;
        previous_sv.count = n->token.sv.count;
        error_parts(EK_NOTE, previous_sv, *previous_pos, "Here is the first definition");
    }
    exit(c, 1);
}

void error_redefinition_add_helper_message_for_import(
    const Node *this, const Module *module, const Context *context, const char *label) //
{
    for (size_t i = 0; i < module->imports.count; i++) {
        Node_Import *it = module->imports.data[i];
        if (it->module == this->module) {
            error_node(EK_NOTE, (Node *) it, "The %s was imported here", label);
            return;
        }
    }

    for (Context_Fn *fn = context->fn; fn; fn = fn->outer) {
        for (size_t i = fn->imports_end; i > fn->imports_begin; i--) {
            Node_Import *it = context->imports.data[i - 1];
            if (it->module == this->module) {
                error_node(EK_NOTE, (Node *) it, "The %s was imported here", label);
                return;
            }
        }
    }
}

void error_redefinition_global(
    Compiler *c, const Node *this, const Node *previous, const Module *module, const Context *context) //
{
    error_token(EK_ERROR, this->token, "Redefinition of '" SV_Fmt "'", SV_Arg(this->token.sv));
    if (this->module != module) {
        error_redefinition_add_helper_message_for_import(this, module, context, "redefinition");
    }

    if (previous) {
        error_token(EK_NOTE, previous->token, "Here is the first definition");
        if (previous->module != module) {
            error_redefinition_add_helper_message_for_import(previous, module, context, "first definition");
        }
    }
    exit(c, 1);
}

void error_number_of_return_values_mismatch(Compiler *c, Token token, size_t expected, size_t actual) {
    error_token(
        EK_ERROR,
        token,
        "Too %s return values: Expected %zu, got %zu",
        actual < expected ? "few" : "many",
        expected,
        actual);
    exit(c, 1);
}

void maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(Node *n, Type expected) {
    if (type_eq_without_distinct(n->type, expected)) {
        afprintf(
            stderr,
            ANSI_COLOR_YELLOW | ANSI_BOLD,
            "    The underlying types seem to be equal, but distinct. Try an explicit cast.\n\n");
    }
}

static_assert(COUNT_TYPES == 27, "");
void check_int_limit_ex(Compiler *c, Node *n, Int128 value, bool min_zero, const char *label) {
    const Type_Kind type_kind = type_kind_eq(n->type, TYPE_ENUM) ? n->type.spec.enumm.underlying : n->type.kind;

    typedef struct {
        Int128 min;
        Int128 max;
    } Limit;

    Limit limit = {0};
    if (type_is_signed(n->type)) {
        const Limit limits[COUNT_TYPES] = {
            [TYPE_I8] = {.min = INT128_FROM_I64(INT8_MIN), .max = INT128_FROM_I64(INT8_MAX)},
            [TYPE_I16] = {.min = INT128_FROM_I64(INT16_MIN), .max = INT128_FROM_I64(INT16_MAX)},
            [TYPE_I32] = {.min = INT128_FROM_I64(INT32_MIN), .max = INT128_FROM_I64(INT32_MAX)},
            [TYPE_I64] = {.min = INT128_FROM_I64(INT64_MIN), .max = INT128_FROM_I64(INT64_MAX)},
            [TYPE_INT] = {.min = INT128_FROM_I64(INT64_MIN), .max = INT128_FROM_I64(INT64_MAX)},
        };
        limit = limits[type_kind];
    } else {
        const Limit limits[COUNT_TYPES] = {
            [TYPE_U8] = {.min = INT128_FROM_U64(0), .max = INT128_FROM_U64(UINT8_MAX)},
            [TYPE_U16] = {.min = INT128_FROM_U64(0), .max = INT128_FROM_U64(UINT16_MAX)},
            [TYPE_U32] = {.min = INT128_FROM_U64(0), .max = INT128_FROM_U64(UINT32_MAX)},
            [TYPE_U64] = {.min = INT128_FROM_U64(0), .max = INT128_FROM_U64(UINT64_MAX)},
        };
        limit = limits[type_kind];
    }

    if (min_zero) {
        limit.min = INT128_FROM_U64(0);
    }

    if (int128_lt(value, limit.min, true) || int128_gt(value, limit.max, true)) {
        error_node(
            EK_ERROR,
            n,
            "Number '%s' is invalid for %s, which must be in range [%s, %s]",
            int128_to_cstr(value),
            label ? label : type_to_cstr(n->type),
            int128_to_cstr(limit.min),
            int128_to_cstr(limit.max));
        exit(c, 1);
    }
}

void check_int_limit(Compiler *c, Node *n, Int128 value) {
    check_int_limit_ex(c, n, value, false, NULL);
}

bool get_builtin_type_kind(SV name, Type_Kind *kind) {
    static_assert(COUNT_TYPES == 27, "");
    static const char *names[COUNT_TYPES] = {
        [TYPE_BOOL] = "bool",
        [TYPE_CHAR] = "char",

        [TYPE_I8] = "i8",
        [TYPE_I16] = "i16",
        [TYPE_I32] = "i32",
        [TYPE_I64] = "i64",

        [TYPE_U8] = "u8",
        [TYPE_U16] = "u16",
        [TYPE_U32] = "u32",
        [TYPE_U64] = "u64",

        [TYPE_RAWPTR] = "rawptr",
        [TYPE_STRING] = "string",
    };

    for (Type_Kind k = 0; k < len(names); k++) {
        const char *it = names[k];
        if (it && sv_match(name, it)) {
            if (kind) {
                *kind = k;
            }
            return true;
        }
    }

    return false;
}

i64 get_enum_value(Compiler *c, Node_Enum *enumm, SV name, const Token *t) {
    ll_foreach(it, &enumm->values) {
        if (sv_eq(it->token.sv, name)) {
            return it->token.as.integer;
        }
    }

    error_undefined(c, t, "enumeration value", true);
    error_node(EK_NOTE, (Node *) enumm, "Enumeration defined here");
    exit(c, 1);
}

size_t get_union_type_index(Compiler *c, Node *n, Type unionn) {
    assert(unionn.kind == TYPE_UNION);
    const Type_Union *spec = unionn.spec.unionn;

    const Type type = type_without_meta(n->type);
    for (size_t i = 0; i < spec->variants_count; i++) {
        if (type_eq(spec->variants[i].type, type)) {
            return i + 1;
        }
    }

    error_node(EK_ERROR, n, "Type %s is not a variant of %s", type_to_cstr(type), type_to_cstr(unionn));
    error_node(EK_NOTE, (Node *) spec->definition, "Union defined here");
    exit(c, 1);
}

Node *get_node_from_group(Node *n, size_t index, i64 *group_index) {
    if (!type_kind_eq(n->type, TYPE_GROUP)) {
        if (group_index) {
            *group_index = 0;
        }
        return n;
    }

    if (n->kind == NODE_GROUP) {
        Node_Group *group = (Node_Group *) n;
        size_t      iota = 0;
        ll_foreach(it, &group->nodes) {
            size_t count = 1;
            if (type_kind_eq(it->type, TYPE_GROUP)) {
                count = it->type.spec.group.count;
            }

            const size_t start = iota;
            iota += count;
            if (iota > index) {
                if (group_index) {
                    *group_index = (i64) (index - start);
                }
                return it;
            }
        }
        unreachable();
    }

    if (n->kind == NODE_CALL) {
        assert(index < n->type.spec.group.count);
        *group_index = index;
    }

    return n;
}

Node_Fn *get_function_literal(Node *fn) {
    if (fn->kind == NODE_FN) {
        return (Node_Fn *) fn;
    }

    if (fn->kind == NODE_ATOM && fn->token.kind == TOKEN_IDENT) {
        Node_Atom *atom = (Node_Atom *) fn;
        if (atom->definition->definition_spec->is_const_value_evaluated) {
            const Const_Value value = atom->definition->definition_spec->const_value;
            if (value.kind == CONST_VALUE_FN) {
                return value.as.fn;
            }
        }
    }

    if (fn->kind == NODE_MEMBER) {
        Node_Member *member = (Node_Member *) fn;
        if (member->module_access_definition) {
            Node_Atom        *atom = member->module_access_definition;
            const Const_Value value = atom->definition_spec->const_value;
            if (value.kind == CONST_VALUE_FN) {
                return value.as.fn;
            }
        }

        if (member->method) {
            return member->method;
        }
    }
    return NULL;
}

void set_auto_cast(Compiler *c, Node *n, i64 index, Auto_Cast_Kind kind, Type from, Type to) {
    if (!n->auto_casts) {
        n->auto_casts_count = type_kind_eq(n->type, TYPE_GROUP) ? n->type.spec.group.count : 1;
        n->auto_casts = arena_alloc(&default_arena, n->auto_casts_count * sizeof(*n->auto_casts));
    }

    Auto_Cast *it = &n->auto_casts[index == -1 ? 0 : index];
    it->kind = kind;
    it->from = from;
    it->to = to;
    if (kind == AUTO_CAST_TO_UNION) {
        it->union_index = get_union_type_index(c, n, to);
    }

    if (index == -1) {
        n->type = to;
    } else {
        assert(type_kind_eq(n->type, TYPE_GROUP));
        if (!n->auto_casts_group) {
            n->auto_casts_group = arena_clone(
                &default_arena, n->type.spec.group.data, sizeof(*n->type.spec.group.data) * n->type.spec.group.count);
            n->type.spec.group.data = n->auto_casts_group;
        }
        n->type.spec.group.data[index] = to;
    }
}

static_assert(COUNT_NODES == 29, "");
void cast_untyped(Compiler *c, Node *n, Type expected) {
    switch (n->kind) {
    case NODE_ATOM: {
        static_assert(COUNT_TOKENS == 78, "");
        switch (n->token.kind) {
        case TOKEN_INT:
            n->type = expected;
            break;

        case TOKEN_IDENT: {
            Node_Atom *atom = (Node_Atom *) n;
            assert(atom->definition->definition_spec->is_const); // Only constants can be defined as untyped int
            n->type = expected;
        } break;

        default:
            unreachable();
        }
    } break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        n->type = expected;
        if (n->token.kind != TOKEN_SIZEOF) {
            cast_untyped(c, unary->value, expected);
            if (n->token.kind == TOKEN_SUB) {
                if (!type_is_numeric(n->type) && !type_is_pointer(n->type)) {
                    unary->overload = get_operator_overload(c, "neg", unary->value, n, unary->module);
                }
            }
        }
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        cast_untyped(c, binary->lhs, expected);
        cast_untyped(c, binary->rhs, expected);
        check_expr_binary(c, binary, false);
        n->type = expected;
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        if (member->is_enum) {
            assert(type_kind_eq(member->node.type, TYPE_UNKNOWN_ENUM));
            assert(type_kind_eq(expected, TYPE_ENUM));
            member->enum_value = get_enum_value(c, expected.spec.enumm.definition, n->token.sv, &n->token);
            n->type = expected;
        } else {
            assert(member->module_access_definition); // Must be a module access
            const Definition_Spec *definition_spec = member->module_access_definition->definition_spec;
            assert(definition_spec->is_const); // Only constants can be defined as untyped int
            n->type = expected;
        }
    } break;

    case NODE_COMPOUND:
        n->type = expected;
        check_expr_compound(c, (Node_Compound *) n);
        if (type_kind_eq(n->type, TYPE_ARRAY) && type_kind_eq(expected, TYPE_SLICE)) {
            set_auto_cast(c, n, -1, AUTO_CAST_ARRAY_TO_SLICE, n->type, expected);
        }
        break;

    case NODE_RETURN: {
        Node_Return *ret = (Node_Return *) n;
        cast_untyped(c, ret->value, expected);
        n->type = ret->value->type;
    } break;

    default:
        unreachable();
    }
}

void finalize_untyped_type(Compiler *c, Node *n) {
    if (type_kind_eq(n->type, TYPE_INT)) {
        const Const_Value value = eval_const_expr(c, n, false);
        n->type.kind = TYPE_I64;

        assert(value.kind == CONST_VALUE_INT);
        check_int_limit(c, n, value.as.integer);
    }
}

bool try_auto_cast_untyped(Compiler *c, Node *n, Type expected) {
    if (type_kind_eq(n->type, TYPE_INT) &&
        (type_is_integer(expected) || (type_kind_eq(expected, TYPE_ENUM) && !expected.ref))) //
    {
        if (!type_kind_eq(expected, TYPE_INT)) {
            cast_untyped(c, n, expected);

            // Only constant expressions can be untyped integers
            const Const_Value value = eval_const_expr(c, n, false);
            assert(value.kind == CONST_VALUE_INT);

            check_int_limit(c, n, value.as.integer);
        }
        return true;
    }

    if (type_kind_eq(n->type, TYPE_UNKNOWN_ENUM)) {
        if (type_kind_eq(expected, TYPE_ENUM) && !expected.ref) {
            cast_untyped(c, n, expected);
            return true;
        }
    }

    if (type_kind_eq(n->type, TYPE_UNKNOWN_COMPOUND)) {
        if (expected.ref != n->type.ref) {
            return false;
        }

        if (type_kind_eq(expected, TYPE_STRUCT) || type_kind_eq(expected, TYPE_ARRAY) ||
            type_kind_eq(expected, TYPE_SLICE)) //
        {
            cast_untyped(c, n, expected);
            return true;
        }
    }

    return false;
}

bool try_auto_cast_type_to_rtti(Compiler *c, Node *n, Type expected) {
    if (n->type.is_meta && type_eq(expected, c->type_info_pointer_type)) {
        n->emit_type_info = arena_clone(&default_arena, &n->type, sizeof(n->type));
        n->emit_type_info->is_meta = false;
        n->type = c->type_info_pointer_type;
        return true;
    }

    return false;
}

bool try_auto_cast_literal(Node *n, Type expected) {
    // untyped 'null' -> typed 'null'
    if (node_is_null(n) && (expected.ref || type_kind_eq(expected, TYPE_RAWPTR) || type_kind_eq(expected, TYPE_FN))) {
        // NOTE: We are also checking for rawptr because distinct types exist
        n->type = expected;
        return true;
    }

    // untyped string -> &char
    if (n->kind == NODE_ATOM && n->token.kind == TOKEN_STRING &&
        type_eq_without_distinct(expected, (Type) {.kind = TYPE_CHAR, .ref = 1})) //
    {
        n->type = expected;
        return true;
    }

    return false;
}

// Set 'group_index' to -1 for no group
bool try_auto_cast(Compiler *c, Node *n, Type expected, i64 group_index) {
    // Literals cannot be part of a group
    if (group_index == -1) {
        if (try_auto_cast_untyped(c, n, expected)) {
            return true;
        }

        if (try_auto_cast_literal(n, expected)) {
            return true;
        }

        if (try_auto_cast_type_to_rtti(c, n, expected)) {
            return true;
        }

        finalize_untyped_type(c, n);
    }

    Type actual = n->type;
    if (group_index != -1) {
        assert(actual.kind == TYPE_GROUP);
        actual = actual.spec.group.data[group_index];
    }

    if (type_is_union(expected) && !type_is_unknown(actual) && !type_kind_eq(actual, TYPE_MODULE)) {
        set_auto_cast(c, n, group_index, AUTO_CAST_TO_UNION, actual, expected);
        return true;
    }

    if (type_kind_eq(actual, TYPE_ARRAY) &&                                //
        type_kind_eq(expected, TYPE_SLICE) &&                              //
        !actual.ref && !expected.ref &&                                    //
        type_eq(*actual.spec.array.element, *expected.spec.slice.element)) //
    {
        set_auto_cast(c, n, group_index, AUTO_CAST_ARRAY_TO_SLICE, actual, expected);
        return true;
    }

    if (type_kind_eq(actual, TYPE_DYNAMIC_ARRAY) &&                                //
        type_kind_eq(expected, TYPE_SLICE) &&                                      //
        !actual.ref && !expected.ref &&                                            //
        type_eq(*actual.spec.dynamic_array.element, *expected.spec.slice.element)) //
    {
        set_auto_cast(c, n, group_index, AUTO_CAST_DYNAMIC_ARRAY_TO_SLICE, actual, expected);
        return true;
    }

    if (type_kind_eq(expected, TYPE_TRAIT) && !expected.ref &&          //
        !type_is_unknown(actual) && !type_kind_eq(actual, TYPE_MODULE)) //
    {
        if (actual.is_meta) {
            assert(group_index == -1); // Literals cannot be part of a group
            try_auto_cast_type_to_rtti(c, n, c->type_info_pointer_type);
            actual = n->type;
        }
        finalize_untyped_type(c, n);

        Type_Trait_Impl *impl = check_type_satisfies_trait(c, actual, expected.spec.trait, n, group_index);
        set_auto_cast(c, n, group_index, AUTO_CAST_TO_TRAIT, actual, expected);
        n->auto_casts[group_index == -1 ? 0 : group_index].trait_impl = impl;
        return true;
    }

    return false;
}

void make_sure_import_is_ready(Compiler *c, Node_Import *import) {
    if (!import->module && parser_import(c->parser, import)) {
        const Context context_save = c->context;
        memset(&c->context, 0, sizeof(c->context));
        define_orderless_nodes_of_module(c, import->module, &import->node.token);
        c->context = context_save;
    }
}
