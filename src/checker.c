#include "checker.h"
#include "basic.h"
#include "compiler.h"
#include "context.h"
#include "contract.h"
#include "error.h"
#include "int128.h"
#include "node.h"
#include "parser.h"
#include "token.h"
#include <assert.h>
#include <stdint.h>

static bool type_is_trait(Type type) {
    return !type.ref && type_kind_eq(type, TYPE_TRAIT);
}

static bool type_is_union(Type type) {
    return !type.ref && type_kind_eq(type, TYPE_UNION);
}

static bool node_is_null(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_NULL;
}

static bool node_is_runtime_polymorphic_expression(Node *n) {
    if (type_kind_eq(n->type, TYPE_FN) && !n->type.ref && n->type.spec.fn->polymorphs_count) {
        return n->kind != NODE_FN || ((Node_Fn *) n)->defined_as == NULL;
    }
    return false;
}

static inline Type type_with_ref(Type t, size_t ref) {
    t.ref = ref;
    return t;
}

static inline Type type_without_ref(Type t) {
    t.ref = 0;
    return t;
}

static void error_undefined(Compiler *c, const Token *t, const char *label, bool no_exit) {
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
        exit(1);
    }
}

static void error_redefinition(const Node *n, const Pos *previous_pos) {
    error_token(EK_ERROR, n->token, "Redefinition of '" SV_Fmt "'", SV_Arg(n->token.sv));
    if (previous_pos) {
        SV previous_sv = {0};
        previous_sv.data = previous_pos->line.data + previous_pos->col;
        previous_sv.count = n->token.sv.count;
        error_parts(EK_NOTE, previous_sv, *previous_pos, "Here is the first definition");
    }
    exit(1);
}

static void error_redefinition_add_helper_message_for_import(
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

static void
error_redefinition_global(const Node *this, const Node *previous, const Module *module, const Context *context) //
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
    exit(1);
}

static void error_number_of_return_values_mismatch(Token token, size_t expected, size_t actual) {
    error_token(
        EK_ERROR,
        token,
        "Too %s return values: Expected %zu, got %zu",
        actual < expected ? "few" : "many",
        expected,
        actual);
    exit(1);
}

static void print_quoted_char(FILE *f, char ch, char quote) {
    switch (ch) {
    case 033:
        fputs("\\e", f);
        break;

    case '\n':
        fputs("\\n", f);
        break;

    case '\r':
        fputs("\\r", f);
        break;

    case '\t':
        fputs("\\t", f);
        break;

    case '\0':
        fputs("\\0", f);
        break;

    case '\\':
        fputs("\\\\", f);
        break;

    case '\'':
        if (quote == '\'') {
            fputs("\\'", f);
        } else {
            fputc(ch, f);
        }
        break;

    case '"':
        if (quote == '"') {
            fputs("\\\"", f);
        } else {
            fputc(ch, f);
        }
        break;

    default:
        fputc(ch, f);
        break;
    }
}

static void check_that_type_is_known(const Node *n) {
    if (type_is_unknown(n->type)) {
        error_node(EK_ERROR, n, "Cannot infer type of this expression");
        exit(1);
    }
}

static_assert(COUNT_TYPES == 26, "");
static void check_int_limit_ex(Node *n, Int128 value, bool min_zero, const char *label) {
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
        exit(1);
    }
}

static inline void check_int_limit(Node *n, Int128 value) {
    check_int_limit_ex(n, value, false, NULL);
}

static i64 get_enum_value(Compiler *c, Node_Enum *enumm, SV name, const Token *t) {
    ll_foreach(it, &enumm->values) {
        if (sv_eq(it->token.sv, name)) {
            return it->token.as.integer;
        }
    }

    error_undefined(c, t, "enumeration value", true);
    error_node(EK_NOTE, (Node *) enumm, "Enumeration defined here");
    exit(1);
}

static size_t get_union_type_index(Node *n, Type unionn) {
    assert(unionn.kind == TYPE_UNION);
    const Type_Union *spec = unionn.spec.unionn;

    Type type = n->type;
    type.is_meta = false;

    for (size_t i = 0; i < spec->variants_count; i++) {
        if (type_eq(spec->variants[i].type, type)) {
            return i + 1;
        }
    }

    error_node(EK_ERROR, n, "Type %s is not a variant of %s", type_to_cstr(type), type_to_cstr(unionn));
    error_node(EK_NOTE, (Node *) spec->definition, "Union defined here");
    exit(1);
}

static void     check_compound_expr(Compiler *c, Node_Compound *compound);
static void     check_binary_expr(Compiler *c, Node_Binary *binary, bool check_children);
static Node_Fn *get_operator_overload(Compiler *c, const char *operator, Node *receiver, Node *op, Module *module);

static void set_auto_cast(Node *n, i64 index, Auto_Cast_Kind kind, Type from, Type to) {
    if (!n->auto_casts) {
        n->auto_casts_count = type_kind_eq(n->type, TYPE_GROUP) ? n->type.spec.group.count : 1;
        n->auto_casts = arena_alloc(&default_arena, n->auto_casts_count * sizeof(*n->auto_casts));
    }

    Auto_Cast *it = &n->auto_casts[index == -1 ? 0 : index];
    it->kind = kind;
    it->from = from;
    it->to = to;
    if (kind == AUTO_CAST_TO_UNION) {
        it->union_index = get_union_type_index(n, to);
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
static void cast_untyped(Compiler *c, Node *n, Type expected) {
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
        check_binary_expr(c, binary, false);
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
        check_compound_expr(c, (Node_Compound *) n);
        if (type_kind_eq(n->type, TYPE_ARRAY) && type_kind_eq(expected, TYPE_SLICE)) {
            set_auto_cast(n, -1, AUTO_CAST_ARRAY_TO_SLICE, n->type, expected);
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

static Const_Value eval_const_expr(Compiler *c, Node *n, bool ref);

static bool try_auto_cast_untyped(Compiler *c, Node *n, Type expected) {
    if (type_kind_eq(n->type, TYPE_INT) &&
        (type_is_integer(expected) || (type_kind_eq(expected, TYPE_ENUM) && !expected.ref))) //
    {
        if (!type_kind_eq(expected, TYPE_INT)) {
            cast_untyped(c, n, expected);

            // Only constant expressions can be untyped integers
            const Const_Value value = eval_const_expr(c, n, false);
            assert(value.kind == CONST_VALUE_INT);

            check_int_limit(n, value.as.integer);
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

static bool try_auto_cast_type_to_rtti(Compiler *c, Node *n, Type expected) {
    if (n->type.is_meta && type_eq(expected, c->type_info_pointer_type)) {
        n->emit_type_info = arena_clone(&default_arena, &n->type, sizeof(n->type));
        n->emit_type_info->is_meta = false;
        n->type = c->type_info_pointer_type;
        return true;
    }

    return false;
}

static bool type_eq_without_distinct(Type a, Type b) {
    a.distinct = NULL;
    b.distinct = NULL;
    return type_eq(a, b);
}

static void finalize_untyped_type(Compiler *c, Node *n) {
    if (type_kind_eq(n->type, TYPE_INT)) {
        const Const_Value value = eval_const_expr(c, n, false);
        n->type.kind = TYPE_I64;

        assert(value.kind == CONST_VALUE_INT);
        check_int_limit(n, value.as.integer);
    }
}

// Nice name
static void maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(Node *n, Type expected) {
    if (type_eq_without_distinct(n->type, expected)) {
        error_node(EK_NOTE, n, "The underlying types seem to be equal, but distinct. Try an explicit cast.");
    }
}

static bool try_auto_cast_literal(Node *n, Type expected) {
    // untyped 'null' -> typed 'null'
    if (node_is_null(n) && (expected.ref || type_kind_eq(expected, TYPE_RAWPTR) || type_kind_eq(expected, TYPE_FN))) {
        // NOTE: We are also checking for rawptr because distinct types exist
        n->type = expected;
        return true;
    }

    // untyped string -> &char
    if (n->kind == NODE_ATOM && n->token.kind == TOKEN_STRING &&
        type_eq(expected, (Type) {.kind = TYPE_CHAR, .ref = 1})) //
    {
        n->type = expected;
        return true;
    }

    return false;
}

static Type_Trait_Impl *check_type_satisfies_trait(Compiler *c, Type type, Type_Trait *trait, Node *n, i64 group_index);

// Set 'group_index' to -1 for no group
static bool try_auto_cast(Compiler *c, Node *n, Type expected, i64 group_index) {
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

    if (type_is_union(expected) && !type_is_unknown(actual)) {
        set_auto_cast(n, group_index, AUTO_CAST_TO_UNION, actual, expected);
        return true;
    }

    if (type_kind_eq(actual, TYPE_ARRAY) &&                                //
        type_kind_eq(expected, TYPE_SLICE) &&                              //
        !actual.ref && !expected.ref &&                                    //
        type_eq(*actual.spec.array.element, *expected.spec.slice.element)) //
    {
        set_auto_cast(n, group_index, AUTO_CAST_ARRAY_TO_SLICE, actual, expected);
        return true;
    }

    if (type_kind_eq(expected, TYPE_TRAIT) && !expected.ref && !type_is_unknown(actual)) {
        if (actual.is_meta) {
            assert(group_index == -1); // Literals cannot be part of a group
            try_auto_cast_type_to_rtti(c, n, c->type_info_pointer_type);
            actual = n->type;
        }
        finalize_untyped_type(c, n);

        Type_Trait_Impl *impl = check_type_satisfies_trait(c, actual, expected.spec.trait, n, group_index);
        set_auto_cast(n, group_index, AUTO_CAST_TO_TRAIT, actual, expected);
        n->auto_casts[group_index == -1 ? 0 : group_index].trait_impl = impl;
        return true;
    }

    return false;
}

static bool type_assert_noexit(Compiler *c, Node *n, Type expected) {
    if (type_eq(n->type, expected)) {
        return true;
    }

    if (try_auto_cast(c, n, expected, -1)) {
        return true;
    }

    check_that_type_is_known(n);
    error_node(EK_ERROR, n, "Expected %s, got %s", type_to_cstr(expected), type_to_cstr(n->type));
    maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(n, expected);
    return false;
}

static Type type_assert(Compiler *c, Node *n, Type expected) {
    if (type_assert_noexit(c, n, expected)) {
        return expected;
    }
    exit(1);
}

static const char *order_postfix(size_t n) {
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

static bool type_assert_grouped_noexit(Compiler *c, Node *n, Type expected, i64 group_index, Token *requirement) {
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

        check_that_type_is_known(n);
        error_node(EK_ERROR, n, "Expected %s, got %s", type_to_cstr(expected), type_to_cstr(actual));
        maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(n, expected);
    } else {
        check_that_type_is_known(n);
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

static Type type_assert_grouped(Compiler *c, Node *n, Type expected, i64 group_index, Token *requirement) {
    if (type_assert_grouped_noexit(c, n, expected, group_index, requirement)) {
        return expected;
    }
    exit(1);
}

static Type type_assert_node(Compiler *c, Node *a, Node *b) {
    if (type_eq(a->type, b->type)) {
        return a->type;
    }

    if (try_auto_cast(c, b, a->type, -1)) {
        return a->type;
    }

    if (try_auto_cast(c, a, b->type, -1)) {
        return b->type;
    }

    check_that_type_is_known(a);
    check_that_type_is_known(b);
    error_node(EK_ERROR, a, "Expected %s, got %s", type_to_cstr(b->type), type_to_cstr(a->type));
    maybe_show_note_about_underlying_types_being_equal_and_suggest_an_explicit_cast(a, b->type);
    exit(1);
}

static Type type_assert_numeric(const Node *n, bool pointers_allowed) {
    if (type_is_numeric(n->type)) {
        return n->type;
    }

    if (pointers_allowed && type_is_pointer(n->type)) {
        return n->type;
    }

    check_that_type_is_known(n);

    const char *label = "arithmetic";
    if (!pointers_allowed) {
        label = "numeric";
    }

    error_node(EK_ERROR, n, "Expected %s value, got %s", label, type_to_cstr(n->type));
    exit(1);
}

static Type type_assert_scalar(const Node *n) {
    if (type_is_scalar(n->type)) {
        return n->type;
    }

    check_that_type_is_known(n);
    error_node(EK_ERROR, n, "Expected scalar value, got %s", type_to_cstr(n->type));
    exit(1);
}

static Type type_assert_type(const Node *n) {
    check_that_type_is_known(n);
    if (n->type.is_meta) {
        return n->type;
    }

    error_node(EK_ERROR, n, "Expected a type, got %s", type_to_cstr(n->type));
    exit(1);
}

static bool get_builtin_type_kind(SV name, Type_Kind *kind) {
    static_assert(COUNT_TYPES == 26, "");
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

static_assert(COUNT_NODES == 29, "");
static bool loop_breaks(Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            if (loop_breaks(it)) {
                return true;
            }
        }
        return false;
    }

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        if (iff->is_compile_time) {
            return loop_breaks(iff->compile_time_real);
        }
        return loop_breaks(iff->consequence) || loop_breaks(iff->antecedence);
    }

    case NODE_CASE: {
        Node_Case *case_ = (Node_Case *) n;
        return loop_breaks(case_->body);
    }

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            if (!sw->compile_time_real) {
                return false;
            }
            return loop_breaks(sw->compile_time_real->body);
        }

        for (Node *it = sw->cases.head; it; it = it->next) {
            if (!loop_breaks(it)) {
                return false;
            }
        }
        return sw->fallback != NULL;
    }

    case NODE_JUMP:
        return n->token.kind == TOKEN_BREAK;

    default:
        return false;
    }
}

static bool is_atom_true(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_BOOL && n->token.as.integer;
}

static bool is_atom_false(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_BOOL && !n->token.as.integer;
}

static_assert(COUNT_NODES == 29, "");
static bool always_returns(Node *n) {
    if (!n) {
        return false;
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        return is_atom_false(assertt->expr);
    }

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        for (Node *it = block->body.head; it; it = it->next) {
            if (always_returns(it)) {
                return true;
            }
        }
        return false;
    }

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        if (iff->is_compile_time) {
            return always_returns(iff->compile_time_real);
        }

        if (is_atom_true(iff->condition)) {
            return always_returns(iff->consequence);
        }

        if (is_atom_false(iff->condition)) {
            return always_returns(iff->antecedence);
        }

        if (!iff->antecedence) {
            return false;
        }
        return always_returns(iff->consequence) && always_returns(iff->antecedence);
    }

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;
        if (forr->init && always_returns(forr->init)) {
            return true;
        }

        bool infinite = false;
        if (!forr->condition) {
            infinite = true;
        } else if (is_atom_true(forr->condition)) {
            infinite = true;
        }

        if (infinite) {
            return !loop_breaks(forr->body);
        }

        return false;
    }

    case NODE_CASE: {
        Node_Case *case_ = (Node_Case *) n;
        return always_returns(case_->body);
    }

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        if (sw->is_compile_time) {
            if (!sw->compile_time_real) {
                return false;
            }
            return always_returns(sw->compile_time_real->body);
        }

        for (Node *it = sw->cases.head; it; it = it->next) {
            if (!always_returns(it)) {
                return false;
            }
        }
        return sw->fallback != NULL;
    }

    case NODE_RETURN:
        return true;

    default:
        return false;
    }
}

typedef enum {
    REF_NONE,
    REF_ADDR,
    REF_ASSIGN,

    REF_ADDR_MEMBER,
    REF_ASSIGN_MEMBER,
} Ref_Kind;

static void check_expr(Compiler *c, Node *n, Ref_Kind ref);
static void check_stmt(Compiler *c, Node *n);

static void define_orderless_nodes_of_module(Compiler *c, Module *module, const Token *unqualified_import_token);

static Node_Atom *module_globals_find_ex(Compiler *c, Module *m, SV name, Module *skip) {
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

static Node_Atom *module_globals_find(Compiler *c, Module *m, SV name) {
    return module_globals_find_ex(c, m, name, NULL);
}

static Node_Fn *get_main(Compiler *c) {
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
        exit(1);
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
        exit(1);
    }

    c->main_fn = (Node_Fn *) main->definition_spec->assignment_node;
    check_stmt(c, (Node *) main->definition_spec->definition_node);

    const Type_Fn *signature = main->node.type.spec.fn;
    if (signature->args_count) {
        c->main_fn->body = NULL;
        error_node(EK_ERROR, (Node *) c->main_fn, "Function 'main' cannot take any arguments");
        exit(1);
    }

    if (signature->returns_count) {
        c->main_fn->body = NULL;
        error_node(EK_ERROR, (Node *) c->main_fn, "Function 'main' cannot return anything");
        exit(1);
    }

    return c->main_fn;
}

static_assert(COUNT_TYPES == 26, "");
static Const_Value default_const_value(Compiler *c, Type type) {
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
static Const_Value const_value_to_trait(Node *n, Type *type, Type_Trait_Impl *impl, Const_Value value) {
    Const_Value_Trait trait = {0};
    assert(n->type.kind == TYPE_TRAIT);
    trait.impl = impl;
    trait.data = arena_clone(&default_arena, &value, sizeof(value));
    trait.type = type;
    return const_value_trait(trait);
}

static Const_Value const_value_to_union(Type union_type, size_t union_index, Const_Value value) {
    Const_Value_Union unionn = {0};
    assert(union_type.kind == TYPE_UNION);
    unionn.spec = union_type.spec.unionn;
    unionn.index = union_index;
    unionn.real = arena_clone(&default_arena, &value, sizeof(value));
    return const_value_union(unionn);
}

static bool eval_const_binary_equality(Compiler *c, Node_Binary *binary) {
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

        Type expected = type.as.type;
        expected.is_meta = false;
        return type_eq(*trait.as.trait.type, expected);
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

        Type expected = variant.as.type;
        expected.is_meta = false;
        return type_eq(unionn.as.unionn.spec->variants[unionn.as.unionn.index - 1].type, expected);
    }

    return const_value_eq(lhs, rhs);
}

static Const_Value const_value_of_var(Compiler *c, Node_Atom *var) {
    if (!var->definition_spec->is_const_value_evaluated) {
        var->definition_spec->const_value = default_const_value(c, var->node.type);
        var->definition_spec->is_const_value_evaluated = true;
    }

    return var->definition_spec->const_value;
}

static inline i64 i64_from_int128(Node *n, Int128 x, bool min_zero, const char *label) {
    check_int_limit_ex(n, x, min_zero, label);
    return x.low;
}

// Is this valid for signedness?
static_assert(COUNT_NODES == 29, "");
static Const_Value eval_const_expr_impl(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return (Const_Value) {0};
    }

    if (n->emit_type_info) {
        return const_value_type(*n->emit_type_info);
    }

    if (ref) {
        if (n->kind != NODE_ATOM || n->token.kind != TOKEN_IDENT) {
            error_node(EK_ERROR, n, "Can only take reference to variables in a constant expression");
            exit(1);
        }
    }

    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;

        static_assert(COUNT_TOKENS == 78, "");
        switch (n->token.kind) {
        case TOKEN_INT:
        case TOKEN_BOOL:
        case TOKEN_CHAR:
            return const_value_u64(n->token.as.integer);

        case TOKEN_NULL:
            return const_value_u64(0);

        case TOKEN_IDENT:
            if (n->type.is_meta) {
                return const_value_type(n->type);
            }

            assert(atom->definition);
            if (!atom->definition->definition_spec->is_const) {
                if (atom->definition->definition_spec->is_local) {
                    error_node(EK_ERROR, n, "Cannot use local variables in a constant expression");
                    error_node(EK_NOTE, (Node *) atom->definition, "Here is the variable being used");
                    exit(1);
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
                exit(1);
            }
            return const_value_string(n->token.as.string);

        case TOKEN_ISTRING:
            unreachable();

        case TOKEN_DIRECTIVE_MAIN:
            return const_value_fn(get_main(c));

        case TOKEN_DIRECTIVE_PLATFORM:
            return get_platform(c, NULL);

        default:
            unreachable();
        }
    }

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        if (unary->overload) {
            error_node(EK_ERROR, n, "Cannot call operator overload in compile time expressions");
            error_node(EK_NOTE, (Node *) unary->overload->defined_as, "This is the overload used");
            exit(1);
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
            exit(1);
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
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        if (binary->overload) {
            error_node(EK_ERROR, n, "Cannot call operator overload in compile time expressions");
            error_node(EK_NOTE, (Node *) binary->overload->defined_as, "This is the overload used");
            exit(1);
        }

        Const_Value lhs = {0};
        Const_Value rhs = {0};

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
                    exit(1);
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
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
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

        static_assert(COUNT_CONST_VALUES == 10, "");
        switch (lhs.kind) {
        case CONST_VALUE_TRAIT: {
            if (member->rhs) {
                if (!lhs.as.trait.type || !type_eq(n->type, *lhs.as.trait.type)) {
                    error_node(
                        EK_ERROR,
                        n,
                        "Type mismatch: Accessing %s, but real type is %s",
                        type_to_cstr(n->type),
                        lhs.as.trait.type ? type_to_cstr(*lhs.as.trait.type) : "null");
                    exit(1);
                }

                assert(lhs.as.trait.data); // The type is checked, that means a real value exists
                return *lhs.as.trait.data;
            } else if (member->is_trait) {
                if (!lhs.as.trait.impl) {
                    error_node(EK_ERROR, n, "Cannot access method of null trait");
                    exit(1);
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
                exit(1);
            } else {
                unreachable();
            }
        }

        case CONST_VALUE_UNION:
            if (member->rhs) {
                if (member->union_index != lhs.as.unionn.index) {
                    const Type_Union *spec = lhs.as.unionn.spec;
                    error_node(
                        EK_ERROR,
                        n,
                        "Type mismatch: Accessing %s, but real type is %s",
                        member->union_index ? type_to_cstr(spec->variants[member->union_index - 1].type) : "null",
                        lhs.as.unionn.index ? type_to_cstr(spec->variants[lhs.as.unionn.index - 1].type) : "null");
                    exit(1);
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
                exit(1);
            } else if (member->field_index == 1) {
                return const_value_u64(lhs.as.array.count);
            } else {
                unreachable();
            }

        case CONST_VALUE_STRING:
            if (member->field_index == 0) {
                error_node(EK_ERROR, n, "Cannot access pointers in constant expressions");
                exit(1);
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
                exit(1);
            }

            return definition->definition_spec->const_value;
        }

        default:
            unreachable();
        }
    }

    case NODE_IMPORT:
        assert(n->type.kind == TYPE_MODULE);
        return const_value_module(n->type.spec.module);

    case NODE_POLYMORPH:
        todo(); //@polymorph

    case NODE_DISTINCT:
        assert(n->type.is_meta);
        return const_value_type(n->type);

    case NODE_INTERPOLATION:
        unreachable();

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

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        Const_Value    value = default_const_value(c, n->type);

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

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        if (!call->is_type_cast) {
            error_node(EK_ERROR, call->fn, "Cannot call functions in a constant expression");
            exit(1);
        }

        const Const_Value value = eval_const_expr(c, call->args.head, false);
        if (value.kind == CONST_VALUE_VAR || type_is_pointer(n->type)) {
            error_node(EK_ERROR, n, "This expression is not constant at compile time");
            exit(1);
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
    } break;

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;
        if (index->overload) {
            error_node(EK_ERROR, n, "Cannot call operator overload in compile time expressions");
            error_node(EK_NOTE, (Node *) index->overload->defined_as, "This is the overload used");
            exit(1);
        }

        const Const_Value lhs = eval_const_expr(c, index->lhs, false);
        if (index->is_ranged) {
            if (type_is_pointer(index->lhs->type)) {
                error_node(EK_ERROR, n, "Cannot construct slices from pointers in constant expressions");
                exit(1);
            }

            static_assert(COUNT_CONST_VALUES == 10, "");
            switch (lhs.kind) {
            case CONST_VALUE_ARRAY: {
                Const_Value_Array array = lhs.as.array;

                i64 begin = 0;
                if (index->a) {
                    begin = i64_from_int128(
                        index->a, eval_const_expr(c, index->a, false).as.integer, true, "beginning of range");
                }

                i64 end = array.count;
                if (index->b) {
                    end =
                        i64_from_int128(index->a, eval_const_expr(c, index->b, false).as.integer, true, "end of range");
                }

                if (begin > end) {
                    index->lhs = NULL;
                    error_node(
                        EK_ERROR, n, "Range (%zd..%zd) is invalid: Beginning of range is more than end", begin, end);
                    exit(1);
                }

                if (begin < 0 || end < 0 || (size_t) begin > array.count || (size_t) end > array.count) {
                    index->lhs = NULL;
                    error_node(
                        EK_ERROR,
                        n,
                        "Range (%zd..%zd) is out of bounds in array of length %zu",
                        begin,
                        end,
                        array.count);
                    exit(1);
                }

                array.data += begin;
                array.count = end - begin;
                array.is_slice = true;
                return const_value_array(array);
            }

            case CONST_VALUE_STRING: {
                SV sv = lhs.as.string;

                i64 begin = 0;
                if (index->a) {
                    begin = i64_from_int128(
                        index->a, eval_const_expr(c, index->a, false).as.integer, true, "beginning of range");
                }

                i64 end = sv.count;
                if (index->b) {
                    end =
                        i64_from_int128(index->a, eval_const_expr(c, index->b, false).as.integer, true, "end of range");
                }

                if (begin > end) {
                    index->lhs = NULL;
                    error_node(
                        EK_ERROR, n, "Range (%zd..%zd) is invalid: Beginning of range is more than end", begin, end);
                    exit(1);
                }

                if (begin < 0 || end < 0 || (size_t) begin > sv.count || (size_t) end > sv.count) {
                    index->lhs = NULL;
                    error_node(
                        EK_ERROR, n, "Range (%zd..%zd) is out of bounds in string of length %zu", begin, end, sv.count);
                    exit(1);
                }

                sv.data += begin;
                sv.count = end - begin;
                return const_value_string(sv);
            }

            default:
                unreachable();
            }
        } else {
            const i64 at = i64_from_int128(index->a, eval_const_expr(c, index->a, false).as.integer, true, "index");

            static_assert(COUNT_CONST_VALUES == 10, "");
            switch (lhs.kind) {
            case CONST_VALUE_ARRAY: {
                if (at < 0 || (size_t) at >= lhs.as.array.count) {
                    error_node(
                        EK_ERROR,
                        index->a,
                        "Index %zd is out of bounds in array of length %zu",
                        at,
                        lhs.as.array.count);
                    exit(1);
                };

                return lhs.as.array.data[at];
            }

            case CONST_VALUE_STRING: {
                if (at < 0 || (size_t) at >= lhs.as.string.count) {
                    error_node(
                        EK_ERROR,
                        index->a,
                        "Index %zd is out of bounds in string of length %zu",
                        at,
                        lhs.as.string.count);
                    exit(1);
                };

                return const_value_u64(lhs.as.string.data[at]);
            }

            default:
                unreachable();
            }
        }
    }

    case NODE_INDEXABLE:
        return const_value_type(n->type);

    default:
        unreachable();
        break;
    }
}

static Const_Value eval_const_expr(Compiler *c, Node *n, bool ref) {
    if (!n) {
        return (Const_Value) {0};
    }

    Type n_type_save;
    if (n->auto_casts) {
        assert(n->auto_casts_count == 1); // Functions cannot be called in constant expressions
        n_type_save = n->type;
        n->type = n->auto_casts->from;
    }

    Const_Value result = eval_const_expr_impl(c, n, ref);
    if (n->auto_casts) {
        n->type = n_type_save;

        static_assert(COUNT_AUTO_CASTS == 4, "");
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

        default:
            unreachable();
        }
    }

    return result;
}

static_assert(COUNT_TOKENS == 78, "");
static const char *operator_method_name_from_token_kind(Token_Kind kind) {
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

static void check_switch_expr_and_alloc_preds(Compiler *c, Node_Switch *sw) {
    check_expr(c, sw->expr, REF_NONE);
    finalize_untyped_type(c, sw->expr);
    check_that_type_is_known(sw->expr);

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
        sw->compare_overload = get_operator_overload(c, "compare", sw->expr, sw->expr, sw->node.module);
    }

    if (!sw->preds) {
        sw->preds = arena_alloc(&default_arena, sw->preds_count * sizeof(*sw->preds));
    }
}

static Const_Value check_switch_pred(Compiler *c, Node_Switch *sw, Node *pred, size_t *iota) {
    Const_Value value = {0};
    check_expr(c, pred, REF_NONE);

    if (sw->trait) {
        if (node_is_null(pred)) {
            value = const_value_u64(0);
        } else {
            type_assert_type(pred);
            Type type = pred->type;
            type.is_meta = false;
            check_type_satisfies_trait(c, type, sw->trait->node.type.spec.trait, pred, -1);
            value = const_value_type(type);
        }
    } else if (sw->unionn) {
        if (node_is_null(pred)) {
            value = const_value_u64(0);
        } else {
            type_assert_type(pred);
            value = const_value_u64(get_union_type_index(pred, sw->expr->type));
        }
    } else {
        type_assert(c, pred, sw->expr->type);
        value = eval_const_expr(c, pred, false);
    }

    for (size_t i = 0; i < *iota; i++) {
        if (const_value_eq(sw->preds[i].value, value)) {
            error_node_begin(EK_ERROR, pred);
            fprintf(stderr, "Duplicate case ");

            if (sw->unionn) {
                pred->type.is_meta = false;
                fprintf(stderr, "%s", type_to_cstr(pred->type));
                pred->type.is_meta = true;
            } else {
                static_assert(COUNT_CONST_VALUES == 10, "");
                switch (value.kind) {
                case CONST_VALUE_INT:
                    if (type_kind_eq(pred->type, TYPE_CHAR)) {
                        fprintf(stderr, "'");
                        print_quoted_char(stderr, value.as.integer.low, '\'');
                        fprintf(stderr, "'");
                    } else {
                        fprintf(stderr, "%s", int128_to_cstr(value.as.integer));
                    }
                    break;

                default:
                    unreachable(); // TODO: Fix this. Now switch supports any type that implements comparisons
                }
            }

            error_finalize();
            error_node(EK_NOTE, sw->preds[i].pred, "Already here");
            exit(1);
        }
    }

    sw->preds[*iota].pred = pred;
    sw->preds[*iota].value = value;
    (*iota)++;
    return value;
}

static void check_switch_exhaustive(Node_Switch *sw) {
    if (sw->enumeration) {
        if (sw->preds_count < sw->enumeration->values_count && !sw->fallback) {
            error_node(EK_ERROR, (Node *) sw, "This switch statement is not complete");
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "    The following enumeration values are not handled:\n");
            ll_foreach(it, &sw->enumeration->values) {
                bool handled = false;
                for (size_t i = 0; i < sw->preds_count; i++) {
                    const Const_Value *pred_value = &sw->preds[i].value;
                    assert(pred_value->kind == CONST_VALUE_INT);
                    if (int128_eq(pred_value->as.integer, int128_from_u64(it->token.as.integer))) {
                        handled = true;
                        break;
                    }
                }

                if (!handled) {
                    afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "        - " SV_Fmt "\n", SV_Arg(it->token.sv));
                }
            }
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "\n");
            error_node(EK_NOTE, (Node *) sw->enumeration, "Enumeration defined here");
            exit(1);
        }
    } else if (sw->unionn) {
        if (sw->preds_count < sw->unionn->variants_count + 1 && !sw->fallback) {
            error_node(EK_ERROR, (Node *) sw, "This switch statement is not complete");
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "    The following union variants are not handled:\n");

            const size_t variants_count = sw->unionn->variants_count + 1;

            bool *handled = arena_alloc(&temp_arena, variants_count * sizeof(*handled));
            for (size_t i = 0; i < sw->preds_count; i++) {
                const Const_Value *pred_value = &sw->preds[i].value;
                assert(pred_value->kind == CONST_VALUE_INT);

                const size_t pred_index = pred_value->as.integer.low;
                assert(pred_index < variants_count);
                handled[pred_index] = true;
            }

            for (size_t i = 0; i < variants_count; i++) {
                if (!handled[i]) {
                    afprintf(
                        stderr,
                        ANSI_COLOR_YELLOW | ANSI_BOLD,
                        "        - %s\n",
                        i ? type_to_cstr_raw(sw->unionn->node.type.spec.unionn->variants[i - 1].type) : "null");
                }
            }
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "\n");

            error_node(EK_NOTE, (Node *) sw->unionn, "Union defined here");
            exit(1);
        }
    }
}

static void push_context_replace(Compiler *c, Context_Replace *replace, Node_Atom *from, Type to) {
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

    replace->to->node.type = to;
    replace->to->node.type.is_meta = false;

    // Technically the code generated in the mismatch condition will access invalid memory.
    // However it will be unreachable, so does it even matter?
    if (replace->to->definition_spec->is_const) {
        Const_Value *value = &replace->to->definition_spec->const_value;

        static_assert(COUNT_CONST_VALUES == 10, "");
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

static void define_orderless_node(Compiler *c, Node *n, const size_t block_start);

static void define_orderless_nodes_of_module(Compiler *c, Module *module, const Token *unqualified_import_token) {
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
        exit(1);
        break;

    case CHECKED:
        // Pass
        break;

    default:
        unreachable();
    }
}

static void make_sure_import_is_ready(Compiler *c, Node_Import *import) {
    if (!import->module && parser_import(c->parser, import)) {
        const Context context_save = c->context;
        memset(&c->context, 0, sizeof(c->context));
        define_orderless_nodes_of_module(c, import->module, &import->node.token);
        c->context = context_save;
    }
}

// TODO: This performs more than just definitions...
static_assert(COUNT_NODES == 29, "");
static void define_orderless_node(Compiler *c, Node *n, const size_t block_start) {
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
                    Node_Atom *previous = context_find_define_skipping(&c->context, *it.key, import->module);
                    if (!previous) {
                        previous = module_globals_find_ex(c, n->module, *it.key, import->module);
                    }

                    if (previous) {
                        error_redefinition_global((Node *) *it.value, (Node *) previous, n->module, &c->context);
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
                                error_redefinition((Node *) it, &previous->node.token.pos);
                                break;
                            }
                        }

                        context_push_define(&c->context, it);
                    }

                    it->definition_spec->fn_context = c->context.fn;
                } else {
                    if (get_builtin_type_kind(it->node.token.sv, NULL)) {
                        error_redefinition((Node *) it, NULL);
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
                            error_redefinition_global((Node *) it, (Node *) previous, it->module, &c->context);
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

            check_switch_exhaustive(sw);
            c->current_comptime_conditional_stmt = current_comptime_conditional_stmt_save;
        }
    } break;

    default:
        // Pass
        break;
    }
}

static bool is_node_caller_location(Node *n) {
    return n->kind == NODE_ATOM && n->token.kind == TOKEN_DIRECTIVE_CALLER_LOCATION;
}

static Node *get_node_from_group(Node *n, size_t index, i64 *group_index) {
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

static void check_definition(Compiler *c, Node_Atom *it, Node *it_expr, Node *type) {
    assert(it->definition_spec->check_status != CHECKING); // It is already checked
    if (it->definition_spec->check_status == CHECKED) {
        return;
    }
    it->definition_spec->check_status = CHECKING;

    if (type) {
        if (type_kind_eq(type->type, TYPE_UNIT)) {
            check_expr(c, type, REF_NONE);
            type_assert_type(type);
        }
        type->type.is_meta = false;
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
                            check_that_type_is_known(it);
                        }
                    } else {
                        check_that_type_is_known(it_expr);
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
                    exit(1);
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
        if (!it->definition_spec->is_const && !sv_match(it->node.token.sv, "_")) {
            context_push_define(&c->context, it);
        }
    }

    it->definition_spec->check_status = CHECKED;
}

static void check_definition_if_needed(Compiler *c, Node_Atom *definition, Ref_Kind ref) {
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
        if (ref == REF_ADDR && definition->node.type.is_meta) {
            // Reference to incomplete type definition is allowed
        } else {
            error_node(EK_ERROR, (Node *) definition, "Cyclic definition");
            exit(1);
        }
        break;

    case CHECKED:
        // Pass
        break;
    }
}

static void check_ident(Compiler *c, Node *n, Ref_Kind ref) {
    Node_Atom   *atom = NULL;
    Node_Member *member = NULL;

    Module *module = NULL;
    bool    importing = false;
    if (n->kind == NODE_ATOM) {
        atom = (Node_Atom *) n;
        module = atom->module;
    } else if (n->kind == NODE_MEMBER) {
        member = (Node_Member *) n;
        assert(member->lhs->type.kind == TYPE_MODULE);
        module = member->lhs->type.spec.module;
        importing = true;
    } else {
        unreachable();
    }

    if (sv_match(n->token.sv, "_")) {
        error_token(EK_ERROR, n->token, "Identifier '_' cannot be used as a value");
        exit(1);
    }

    Node_Atom *definition = NULL;
    if (atom) {
        if (atom->definition) {
            definition = atom->definition;
        } else {
            definition = context_find_define(&c->context, n->token.sv);
            if (definition) {
                if (definition->polymorph) {
                    // Pass
                } else if (definition->definition_spec->fn_context && c->context.fn) {
                    if (definition->definition_spec->fn_context != c->context.fn &&
                        !definition->definition_spec->is_const) //
                    {
                        error_node(EK_ERROR, n, "Cannot use variable from stack frame of outer function");
                        error_node(EK_NOTE, (Node *) definition, "Here is the variable being used");
                        exit(1);
                    }
                }
            }
        }
    }

    if (!definition) {
        definition = module_globals_find(c, module, n->token.sv);
        if (!definition && atom) {
            Type_Kind builtin_type_kind;
            if (get_builtin_type_kind(n->token.sv, &builtin_type_kind)) {
                n->type = (Type) {.kind = builtin_type_kind, .is_meta = true};
                if (ref == REF_ASSIGN) {
                    error_node(EK_ERROR, n, "Cannot assign to compile time constant value");
                    exit(1);
                }
                return;
            }

            if (module != c->builtin_module) {
                module = c->builtin_module;
                importing = true;
                definition = module_globals_find(c, module, n->token.sv);
            }
        }

        if (definition && definition->definition_spec->is_private && importing) {
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
            n->type = definition->node.type;
            if (definition->polymorph->is_monomorphized) {
                if (n->type.kind == TYPE_UNIT) {
                    Node *polymorph = (Node *) definition->polymorph;
                    check_expr(c, polymorph, REF_NONE);
                    n->type = polymorph->type;
                }
            } else {
                n->type.spec.polymorph.is_definition = false;
            }
        } else {
            check_definition_if_needed(c, definition, ref);
            n->type = definition->node.type;
            n->is_memory = !definition->definition_spec->is_const;
        }

        if (!n->is_memory) {
            switch (ref) {
            case REF_NONE:
                // OK
                break;

            case REF_ADDR:
                if (!n->type.is_meta) {
                    error_node(EK_ERROR, n, "Cannot take reference to compile time constant value");
                    error_node(EK_NOTE, (Node *) definition, "Here is the constant being used");
                    exit(1);
                }
                break;

            case REF_ADDR_MEMBER:
                if (!n->type.is_meta && !type_kind_eq(definition->node.type, TYPE_MODULE)) {
                    error_node(EK_ERROR, n, "Cannot take reference to compile time constant value");
                    error_node(EK_NOTE, (Node *) definition, "Here is the constant being used");
                    exit(1);
                }
                break;

            case REF_ASSIGN:
                error_node(EK_ERROR, n, "Cannot assign to compile time constant value");
                error_node(EK_NOTE, (Node *) definition, "Here is the constant being used");
                exit(1);
                break;

            case REF_ASSIGN_MEMBER:
                if (!type_kind_eq(definition->node.type, TYPE_MODULE)) {
                    error_node(EK_ERROR, n, "Cannot assign to compile time constant value");
                    error_node(EK_NOTE, (Node *) definition, "Here is the constant being used");
                    exit(1);
                }
                break;
            }
        }
    } else {
        error_undefined(c, &n->token, "identifier", false);
    }
}

static void check_that_methods_can_be_accessed(Compiler *c, Node *receiver, Module *definition) {
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

        exit(1);
    }
}

static bool get_method_spec(
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

            return defining_in_module == definition->module;
        }

        check_that_methods_can_be_accessed(c, receiver_node, definition->module);
        return true;
    } else if (type_kind_eq(receiver_type, TYPE_STRUCT)) {
        Node_Struct *definition = receiver_type.spec.structt->definition;
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
    } else if (receiver_type.distinct) {
        if (spec) {
            spec->uid = (uintptr_t) receiver_type.distinct;
        }

        if (defining_in_module) {
            if (is_named) {
                *is_named = true;
            }

            return defining_in_module == receiver_type.distinct->module;
        }

        check_that_methods_can_be_accessed(c, receiver_node, receiver_type.distinct->module);
        return true;
    }

    static const Type string_type = {.kind = TYPE_STRING};
    if (type_eq(receiver_type, string_type)) {
        if (spec) {
            spec->uid = (uintptr_t) &string_type;
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

    return false;
}

static Node_Fn *get_method(Compiler *c, Method_Spec spec, Module *module) {
    Node_Fn **fn = ht_get(&c->methods_table, spec);
    if (!fn) {
        return NULL;
    }

    Node_Fn *method = *fn;
    assert(method->defined_as);

    if (method->module != module && method->defined_as->definition_spec->is_private) {
        return NULL;
    }

    if (method->node.type.kind != TYPE_FN) {
        check_definition_if_needed(c, method->defined_as, REF_NONE);
    }

    return method;
}

static Type_Trait_Impl *
check_type_satisfies_trait(Compiler *c, Type receiver, Type_Trait *trait, Node *n, i64 group_index) //
{
    const Type receiver_without_ref = type_without_ref(receiver);
    ll_foreach(it, &trait->impls) {
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

                {
                    //
                }

                Node_Fn *fn = get_method(c, spec, n->module);
                if (!fn) {
                    errors[i] = (Error) {.kind = UNDEFINED};
                    goto next;
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
                    exit(1);
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
                        (Node *) it.fn->defined_as->definition_spec->definition_node,
                        "The method '" SV_Fmt "' has receiver %s, not %s",
                        SV_Arg(it.fn->defined_as->node.token.sv),
                        type_to_cstr(it.fn->node.type.spec.fn->args[0].type),
                        type_to_cstr(receiver));
                    break;

                case WRONG_SIGNATURE:
                    it.fn->body = NULL;
                    error_node(
                        EK_NOTE,
                        (Node *) it.fn->defined_as->definition_spec->definition_node,
                        "The method '" SV_Fmt "' has wrong signature. Expected %s, got %s",
                        SV_Arg(it.fn->defined_as->node.token.sv),
                        type_to_cstr(trait->methods[i].type),
                        type_to_cstr(it.fn->node.type));
                    break;

                case OK:
                    break;
                }
            }

            if (!ok) {
                exit(1);
            }
        }
        arena_reset(&temp_arena, errors);
    }

    impl.trait = trait;
    impl.next = trait->impls.head;
    trait->impls.head = arena_clone(&default_arena, &impl, sizeof(impl));
    return trait->impls.head;
}

static bool is_indexable(Compiler *c, Node *n, Type type, Module *module) {
    if (type_kind_eq(type, TYPE_ARRAY) || type_kind_eq(type, TYPE_SLICE) || type_kind_eq(type, TYPE_STRING)) {
        return true;
    }

    Method_Spec spec = {0};
    if (get_method_spec(c, n, type, sv_from_cstr("index"), &spec, NULL, NULL)) {
        return get_method(c, spec, module) != NULL;
    }

    return false;
}

static Node_Fn *get_operator_overload(Compiler *c, const char *operator, Node *receiver, Node *op, Module *module) {
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
                exit(1);
            }

            return method;
        }
    }

    check_that_type_is_known(receiver);
    error_node(
        EK_ERROR, op, "Method '" SV_Fmt "' is not defined for %s", SV_Arg(spec.name), type_to_cstr(receiver->type));
    exit(1);
}

static Node_Fn *check_assignment_lhs_for_arithmetics(Compiler *c, Node_Binary *binary, Node *n) {
    const Token_Kind op = binary->node.token.kind;
    switch (op) {
    case TOKEN_ADD_SET:
    case TOKEN_SUB_SET:
        if (!type_is_numeric(n->type) && !type_is_pointer(n->type)) {
            return get_operator_overload(
                c, operator_method_name_from_token_kind(op), n, (Node *) binary, binary->module);
        }
        break;

    case TOKEN_MUL_SET:
    case TOKEN_DIV_SET:
    case TOKEN_MOD_SET:
        if (type_is_pointer(n->type)) {
            error_node(EK_ERROR, (Node *) binary, "This operation is not valid for pointers");
            error_node(EK_NOTE, n, "The operands are of type %s", type_to_cstr(n->type));
            exit(1);
        }

        if (!type_is_numeric(n->type)) {
            return get_operator_overload(
                c, operator_method_name_from_token_kind(op), n, (Node *) binary, binary->module);
        }
        break;

    case TOKEN_SHL_SET:
    case TOKEN_SHR_SET:
    case TOKEN_BOR_SET:
    case TOKEN_BAND_SET:
        if (type_is_pointer(n->type)) {
            error_node(EK_ERROR, (Node *) binary, "This operation is not valid for pointers");
            error_node(EK_NOTE, n, "The operands are of type %s", type_to_cstr(n->type));
            exit(1);
        }

        type_assert_numeric(n, false);
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

    binary->node.type = (Type) {.kind = TYPE_UNIT};
}

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

static Node_Fn *get_function_literal(Node *fn) {
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

static void add_monomorph_parameter(Compiler *c, Node_Polymorph *polymorph, Type type) {
    for (size_t i = 0; i < c->monomorph_parameters.count; i++) {
        if (c->monomorph_parameters.data[i].from == polymorph) {
            return;
        }
    }

    const Monomorph_Parameter mp = {
        .from = polymorph,
        .to = type,
    };
    da_push(&c->monomorph_parameters, mp);
}

static void infer_monomorph_parameters(Compiler *c, Node *n, i64 group_index, Type *expected) {
    if (!expected) {
        return;
    }

    Type *actual = &n->type;
    if (group_index != -1) {
        assert(actual->kind == TYPE_GROUP);
        actual = &actual->spec.group.data[group_index];
    }

    if (actual->is_meta) {
        todo(); // TODO(@polymorphism): Type -> RTTI
    }

    // TODO(@polymorphism): This is incomplete, just to get the tests working again
    if (expected->kind != TYPE_POLYMORPH) {
        return;
    }

    if (!expected->spec.polymorph.is_definition) {
        return;
    }

    if (group_index == -1) {
        check_that_type_is_known(n);
        finalize_untyped_type(c, n);
    }

    Node_Polymorph *polymorph = expected->spec.polymorph.definition;
    assert(actual->ref >= expected->ref);
    add_monomorph_parameter(c, polymorph, type_with_ref(*actual, actual->ref - expected->ref));
}

static void monomorphize_node(Compiler *c, Node **np, bool first);

static void monomorphize_nodes(Compiler *c, Nodes *ns, bool first) {
    Nodes from = *ns;
    memset(ns, 0, sizeof(*ns));

    ll_foreach(it, &from) {
        monomorphize_node(c, &it, first);
        nodes_push(ns, it);
    }
}

static void monomorphize_replace(Compiler *c, Node **from) {
    if (!*from) {
        return;
    }

    Node **to = (Node **) ht_get(&c->monomorph_replacements, *from);
    if (to) {
        *from = *to;
    }
}

static void monomorphize_node(Compiler *c, Node **np, bool first) {
    if (!*np) {
        return;
    }

    if (first && ht_get(&c->monomorph_replacements, *np)) {
        // TODO(@polymorph): Consider whether it loops or not. Leaving it for now
        //
        // This will not be a problem here, but in the second pass, it will be a problem
        error_node(EK_ERROR, *np, "Loop");
        exit(1);
    }

    Node *n = *np;
    if ((*np)->kind == NODE_ATOM && (*np)->token.kind != TOKEN_IDENT) {
        return;
    }

    if (first) {
        Node *copy = arena_clone(&default_arena, n, node_size(n->kind));
        memset(&copy->type, 0, sizeof(copy->type));
        ht_set(&c->monomorph_replacements, n, copy);
        n = copy;
    } else {
        monomorphize_replace(c, np);
        n = *np;
    }

    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;
        if (first) {
            if (atom->definition_spec) {
                atom->definition_spec =
                    arena_clone(&default_arena, atom->definition_spec, sizeof(*atom->definition_spec));
                atom->definition_spec->check_status = UNCHECKED;
            }
        } else {
            if (atom->definition_spec) {
                monomorphize_replace(c, (Node **) &atom->definition_spec->static_var_fn);
                monomorphize_replace(c, (Node **) &atom->definition_spec->definition_node);
                monomorphize_replace(c, (Node **) &atom->definition_spec->assignment_node);
            }

            monomorphize_replace(c, (Node **) &atom->definition);
            monomorphize_replace(c, (Node **) &atom->polymorph);
        }
    } break;

    case NODE_GROUP: {
        todo();
    } break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        monomorphize_node(c, &unary->value, first);
    } break;

    case NODE_BINARY: {
        todo();
    } break;

    case NODE_MEMBER: {
        todo();
    } break;

    case NODE_ASSERT: {
        todo();
    } break;

    case NODE_IMPORT: {
        todo();
    } break;

    case NODE_DISTINCT: {
        todo();
    } break;

    case NODE_POLYMORPH: {
        Node_Polymorph *polymorph = (Node_Polymorph *) n;
        monomorphize_node(c, (Node **) &polymorph->name, first);
    } break;

    case NODE_INTERPOLATION: {
        todo();
    } break;

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;
        monomorphize_nodes(c, &fn->args, first);
        monomorphize_nodes(c, &fn->returns, first);
        monomorphize_node(c, &fn->body, first);

        if (!first) {
            monomorphize_replace(c, (Node **) &fn->outer_fn);
            monomorphize_replace(c, (Node **) &fn->defined_as);
        }
    } break;

    case NODE_ENUM: {
        todo();
    } break;

    case NODE_TRAIT: {
        todo();
    } break;

    case NODE_UNION: {
        todo();
    } break;

    case NODE_STRUCT: {
        todo();
    } break;

    case NODE_COMPOUND: {
        todo();
    } break;

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        monomorphize_node(c, &call->fn, first);
        monomorphize_nodes(c, &call->args, first);
    } break;

    case NODE_INDEX: {
        todo();
    } break;

    case NODE_INDEXABLE: {
        todo();
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        monomorphize_node(c, &define->name, first);
        monomorphize_node(c, &define->expr, first);
        monomorphize_node(c, &define->type, first);
    } break;

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        monomorphize_nodes(c, &block->body, first);
    } break;

    case NODE_IF: {
        todo();
    } break;

    case NODE_FOR: {
        todo();
    } break;

    case NODE_CASE: {
        todo();
    } break;

    case NODE_SWITCH: {
        todo();
    } break;

    case NODE_JUMP: {
        todo();
    } break;

    case NODE_DEFER: {
        todo();
    } break;

    case NODE_RETURN: {
        Node_Return *returnn = (Node_Return *) n;
        monomorphize_node(c, &returnn->value, first);
    } break;

    case NODE_EXTERN: {
        todo();
    } break;

    default:
        break;
    }
}

static void monomorphize(Compiler *c, Node **np) {
    const bool is_fn = type_kind_eq((*np)->type, TYPE_FN);
    if (is_fn) {
        Node_Fn *fn = get_function_literal(*np);
        assert(fn);
        *np = (Node *) fn;
    } else {
        // TODO(@polymorph): Right now, only functions can be polymorphic
        unreachable();
    }

    monomorphize_node(c, np, true);
    monomorphize_node(c, np, false);
    for (size_t i = 0; i < c->monomorph_parameters.count; i++) {
        Monomorph_Parameter it = c->monomorph_parameters.data[i];
        Node_Polymorph     *polymorph = *(Node_Polymorph **) ht_get(&c->monomorph_replacements, it.from);

        polymorph->is_monomorphized = true;
        polymorph->monomorphized_to = it.to;
    }

    if (is_fn) {
        Node_Fn *fn = (Node_Fn *) *np;
        memset(&fn->polymorphs, 0, sizeof(fn->polymorphs));
    }

    check_expr(c, *np, REF_NONE);
}

// TODO: Remove this
static void ensure_interpolation_is_valid(Node_Interpolation *interpolation, bool noexit) {
    if (!interpolation->is_valid) {
        error_node(EK_ERROR, (Node *) interpolation, "Cannot use interpolated strings here");
        afprintf(
            stderr,
            ANSI_COLOR_YELLOW | ANSI_BOLD,
            "    Interpolated strings are compiled to a temporary '[]Any' value, and can only be used like this:\n"
            "\n"
            "        bar :: (vs: []Any) {}\n"
            "        foo :: (vs: ...Any) {}\n"
            "\n"
            "        foo(\"Nice: \\{34 + 35}\")\n"
            "        bar(\"Nice: \\{200 + 220}\")\n"
            "\n");

        if (!noexit) {
            exit(1);
        }
    }
}

static void check_call_arguments(Compiler *c, Node_Call *call) {
    typedef struct {
        Node *node;
    } Argument;

    Argument *args = NULL;
    size_t    args_count_min = 1;
    size_t    args_count_max = 1;

    const Type_Fn *fn_spec = NULL;

    bool is_trait = false;
    bool is_method = false;
    bool is_polymorph = false;
    if (call->fn->type.is_meta) {
        args = arena_alloc(&temp_arena, 1 * sizeof(*args));
    } else {
        assert(call->fn->type.kind == TYPE_FN);
        fn_spec = call->fn->type.spec.fn;

        args = arena_alloc(&temp_arena, fn_spec->args_count * sizeof(*args));
        args_count_min = fn_spec->args_count_min;
        args_count_max = fn_spec->variadics_kind != VARIADICS_NONE ? UINT64_MAX : fn_spec->args_count;

        if (call->fn->kind == NODE_MEMBER) {
            Node_Member *member = (Node_Member *) call->fn;

            is_trait = member->is_trait;
            is_method = member->method != NULL;
            if ((is_method || is_trait) && !member->lhs->type.is_meta) {
                assert(member->lhs);
                args[call->args_count++].node = member->lhs;
            }
        }

        is_polymorph = node_is_runtime_polymorphic_expression(call->fn);
        if (is_polymorph) {
            ht_clear(&c->monomorph_replacements);
            c->monomorph_parameters.count = 0;
        }
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
    ll_foreach(arg, &call->args) {
        Node  *it = arg;
        Node  *it_name = NULL;
        size_t it_index = call->args_count;
        if (it->kind == NODE_BINARY && it->token.kind == TOKEN_SET) {
            if (!fn_spec) {
                error_node(EK_ERROR, arg, "Cannot use named arguments in a cast expression");
                exit(1);
            }

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
                show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
                exit(1);
            }

            arg->token.as.integer = it_index;
        }

        const bool is_spread = it->kind == NODE_UNARY && it->token.kind == TOKEN_SPREAD;
        if (is_spread) {
            if (!fn_spec) {
                error_node(EK_ERROR, it, "Cannot spread arguments in a cast expression");
                exit(1);
            }

            if (fn_spec->variadics_kind != VARIADICS_TYPED) {
                error_node(
                    EK_ERROR, it, "Cannot spread arguments in a call to a function that does not have typed variadics");
                show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
                exit(1);
            }

            Node_Unary *unary = (Node_Unary *) it;
            it = unary->value;
            check_expr(c, it, REF_NONE);
        } else if (it->kind == NODE_INTERPOLATION) {
            Node_Interpolation *interpolation = (Node_Interpolation *) it;
            interpolation->is_valid = (fn_spec != NULL);
            check_expr(c, it, REF_NONE);
            interpolation->is_valid = false;
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

            if (!fn_spec) {
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
                        show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
                        exit(1);
                    }

                    if (is_spread || it_name || variadic_source_kind == VS_DIRECT) {
                        error_node(EK_ERROR, (Node *) call, "Multiple typed variadic sources found");

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
                        show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
                        exit(1);
                    }

                    if (it->kind == NODE_INTERPOLATION) {
                        Node_Interpolation *interpolation = (Node_Interpolation *) it;
                        interpolation->do_not_allocate = true;
                        call->typed_variadics_count += interpolation->children_count;
                    } else {
                        call->typed_variadics_count++;
                    }
                } else {
                    variadic_source = arg;

                    assert(variadic_source_kind == VS_NONE);
                    if (is_spread || it_name) {
                        variadic_source_kind = VS_DIRECT;
                        call->is_typed_variadics_direct = true;
                    } else {
                        variadic_source_kind = VS_ARGS;
                        if (it->kind == NODE_INTERPOLATION) {
                            Node_Interpolation *interpolation = (Node_Interpolation *) it;
                            interpolation->do_not_allocate = true;
                            call->typed_variadics_count += interpolation->children_count;
                        } else {
                            call->typed_variadics_count++;
                        }
                    }
                }
                continue;
            }

            if (is_spread) {
                error_node(EK_ERROR, arg, "Cannot spread arguments at this position");
                show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
                exit(1);
            }

            Argument *argument = &args[it_index];
            if (argument->node) {
                error_node(EK_ERROR, arg, "Duplication of argument '" SV_Fmt "'", SV_Arg(fn_spec->args[it_index].name));
                error_node(EK_NOTE, argument->node, "Passed here already");
                show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
                exit(1);
            }

            argument->node = arg;
        }

        call->args_count += parts;
    }

    if (call->args_count < args_count_min) {
        error_token(
            EK_ERROR,
            call->end,
            "Too few arguments: Expected%s %zu, got %zu",
            args_count_min == args_count_max ? "" : " at least",
            args_count_min - is_method,
            call->args_count - is_method);
        show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
        exit(1);
    }

    if (call->args_count > args_count_max) {
        error_node(
            EK_ERROR,
            excess_argument,
            "Too many arguments: Expected %zu, got %zu",
            args_count_max - is_method,
            call->args_count - is_method);
        show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
        exit(1);
    }

    if (fn_spec) {
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
                    error_token_begin(EK_ERROR, call->end);
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
                error_token(EK_ERROR, call->end, "Argument '" SV_Fmt "' is not provided", SV_Arg(not_provided_name));
            } else {
                error_finalize();
            }

            show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
            exit(1);
        }
    }

    if (is_polymorph) {
        assert(fn_spec);
        assert(!is_trait); // TODO: Think about this

        if (is_method) {
            assert(call->fn->kind == NODE_MEMBER);
            Node *receiver = ((Node_Member *) call->fn)->lhs;
            Type  expected = type_with_ref(fn_spec->args[0].type, receiver->type.ref);
            infer_monomorph_parameters(c, receiver, -1, &expected);
        }

        size_t it_index = is_method || is_trait;
        ll_foreach(arg, &call->args) {
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

            if (type_kind_eq(it->type, TYPE_GROUP)) {
                Type_Group *group = &it->type.spec.group;
                for (size_t i = 0; i < group->count; i++, it_index++) {
                    Type *expected = it_index < fn_spec->args_count ? &fn_spec->args[it_index].type : NULL;
                    infer_monomorph_parameters(c, it, i, expected);
                }
            } else {
                Type *expected = it_index < fn_spec->args_count ? &fn_spec->args[it_index].type : NULL;
                infer_monomorph_parameters(c, it, -1, expected);
                it_index++;
            }
        }

        monomorphize(c, &call->fn);
        fn_spec = call->fn->type.spec.fn;
    }

    if (fn_spec) {
        if (is_method) {
            assert(call->fn->kind == NODE_MEMBER);
            Node *receiver = ((Node_Member *) call->fn)->lhs;
            type_assert(c, receiver, type_with_ref(fn_spec->args[0].type, receiver->type.ref));
        }

        size_t it_index = is_method || is_trait;
        ll_foreach(arg, &call->args) {
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
                    show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
                    exit(1);
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
                            if (it->kind == NODE_INTERPOLATION &&               //
                                fn_spec->variadics_kind == VARIADICS_TYPED &&   //
                                it_index == fn_spec->variadics_index &&         //
                                type_eq(fn_spec->args[it_index].type, it->type) //
                            ) {
                                // Pass
                            } else {
                                ok = type_assert_noexit(c, it, *expected);
                            }
                        } else {
                            ok = type_assert_grouped_noexit(c, it, *expected, i, NULL);
                        }

                        if (!ok) {
                            show_note_about_the_function_being_called(call->fn, is_method, fn_spec);
                            exit(1);
                        }
                    }

                    it_index++;
                }
            }
        }
    }
}

static void check_whether_member_access_is_valid(Node_Member *m) {
    if (m->rhs) {
        assert(m->lhs); // A bare '.(Type)' will error out at parse time
        if (!type_is_trait(m->lhs->type) && !type_is_union(m->lhs->type)) {
            error_node(EK_ERROR, (Node *) m, "Cannot access variant of %s", type_to_cstr(m->lhs->type));
            exit(1);
        }
    } else {
        if (sv_match(m->node.token.sv, "_")) {
            error_token(EK_ERROR, m->node.token, "Field '_' cannot be accessed");
            exit(1);
        }
    }
}

static void error_special_method_wrong_signature(Token name, const char *signature, const char *note) {
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

static void check_special_method_signature_args_count(
    Node_Fn *fn, const size_t args_count, const char *signature, const char *note) {
    assert(fn->node.type.kind == TYPE_FN);
    const Type_Fn *fn_spec = fn->node.type.spec.fn;

    if (fn_spec->args_count < args_count) {
        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
        error_token(
            EK_NOTE, fn->args_end_token, "Expected at least %zu arguments, got %zu", args_count, fn_spec->args_count);
        exit(1);
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
            exit(1);
        }
    }

    // The previous loop guarantees this
    assert(fn_spec->args_count_min <= args_count);
}

static void check_compound_expr(Compiler *c, Node_Compound *compound) {
    Node *n = (Node *) compound;

    // For structure literal
    Type_Struct *struct_spec = NULL;
    if (n->type.kind == TYPE_STRUCT) {
        struct_spec = n->type.spec.structt;
    }

    size_t array_count = 0;
    size_t ordered_iota = 0;
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
                    exit(1);
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
                    error_undefined(c, &it_field_name->node.token, "field", true);
                    error_node(EK_NOTE, (Node *) struct_spec->definition, "Structure defined here");
                    exit(1);
                }
            } else if (n->type.kind == TYPE_ARRAY || n->type.kind == TYPE_SLICE) {
                check_expr(c, it_binary->lhs, REF_NONE);
                type_assert_numeric(it_binary->lhs, false);

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
                    exit(1);
                }

                it->token.as.integer = i64_from_int128(it_binary->lhs, value.as.integer, true, "index");
            } else if (n->type.kind == TYPE_UNKNOWN_COMPOUND) {
                // Nothing
            } else {
                unreachable();
            }

            it_iota = it->token.as.integer;
            it = it_binary->rhs;
        } else {
            if (n->type.kind == TYPE_STRUCT) {
                if (it_iota >= struct_spec->fields_count) {
                    error_node(EK_ERROR, it, "Too many ordered initializers");
                    error_node(EK_NOTE, (Node *) struct_spec->definition, "Structure defined here");
                    exit(1);
                }
            } else if (n->type.kind == TYPE_ARRAY) {
                if (it_iota >= n->type.spec.array.count) {
                    error_node(
                        EK_ERROR,
                        it,
                        "Index %zu is out of bounds in array of length %zu",
                        it_iota,
                        n->type.spec.array.count);
                    exit(1);
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
    compound->are_children_checked = true;

    if (n->type.kind == TYPE_SLICE) {
        Type *element = n->type.spec.slice.element;
        n->type.spec.array.element = element;
        n->type.spec.array.count = array_count;
        n->type.kind = TYPE_ARRAY;
    }
}

static void check_binary_expr(Compiler *c, Node_Binary *binary, bool check_children) {
    Node *n = (Node *) binary;
    static_assert(COUNT_TOKENS == 78, "");
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
                c, operator_method_name_from_token_kind(n->token.kind), binary->lhs, n, binary->module);
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
            exit(1);
        }

        if (!type_is_numeric(binary->lhs->type)) {
            binary->overload = get_operator_overload(
                c, operator_method_name_from_token_kind(n->token.kind), binary->lhs, n, binary->module);
        }
        n->type = binary->lhs->type;
        break;

    case TOKEN_SHL:
    case TOKEN_SHR:
    case TOKEN_BOR:
    case TOKEN_BAND:
        if (check_children) {
            check_expr(c, binary->lhs, REF_NONE);
            check_expr(c, binary->rhs, REF_NONE);
            type_assert_node(c, binary->rhs, binary->lhs);
        }

        if (type_is_pointer(binary->lhs->type)) {
            error_node(EK_ERROR, n, "This operation is not valid for pointers");
            error_node(EK_NOTE, binary->lhs, "The operands are of type %s", type_to_cstr(binary->lhs->type));
            exit(1);
        }

        n->type = type_assert_numeric(binary->lhs, false);
        break;

        // The following can never be ran as a result of autocast, therefore not considering 'check_children'

    case TOKEN_LOR:
    case TOKEN_LAND:
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
            binary->overload = get_operator_overload(
                c, operator_method_name_from_token_kind(n->token.kind), binary->lhs, n, binary->module);

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
                exit(1);
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
                type_assert_type(binary->rhs);
                binary->rhs->type.is_meta = false;
                check_type_satisfies_trait(c, binary->rhs->type, binary->lhs->type.spec.trait, binary->rhs, -1);
                binary->trait_check_type = arena_clone(&default_arena, &binary->rhs->type, sizeof(binary->rhs->type));
                binary->rhs->type.is_meta = true;
            }
        } else if (type_is_trait(binary->rhs->type)) {
            binary->trait_check = binary->rhs;
            if (!node_is_null(binary->lhs)) {
                type_assert_type(binary->lhs);
                binary->lhs->type.is_meta = false;
                check_type_satisfies_trait(c, binary->lhs->type, binary->rhs->type.spec.trait, binary->lhs, -1);
                binary->trait_check_type = arena_clone(&default_arena, &binary->lhs->type, sizeof(binary->lhs->type));
                binary->lhs->type.is_meta = true;
            }
        } else if (type_is_union(binary->lhs->type)) {
            binary->union_check = binary->lhs;
            if (!node_is_null(binary->rhs)) {
                type_assert_type(binary->rhs);
                binary->union_check_index = get_union_type_index(binary->rhs, binary->lhs->type);
            }
        } else if (type_is_union(binary->rhs->type)) {
            binary->union_check = binary->rhs;
            if (!node_is_null(binary->lhs)) {
                type_assert_type(binary->lhs);
                binary->union_check_index = get_union_type_index(binary->lhs, binary->rhs->type);
            }
        } else {
            type_assert_node(c, binary->rhs, binary->lhs);
            check_that_type_is_known(binary->lhs);

            if (try_auto_cast_type_to_rtti(c, binary->lhs, c->type_info_pointer_type)) {
                assert(try_auto_cast_type_to_rtti(c, binary->rhs, c->type_info_pointer_type));
            } else if (!type_is_scalar(binary->lhs->type)) {
                binary->overload = get_operator_overload(
                    c, operator_method_name_from_token_kind(n->token.kind), binary->lhs, n, binary->module);
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
        check_assignment(c, binary);
        break;

    default:
        unreachable();
    }
}

// The argument 'expected_type' is a hint in order to infer the types of implicit expressions. Checking against it is
// NOT the responsibility of this function.
static_assert(COUNT_NODES == 29, "");
static void check_expr_impl(Compiler *c, Node *n, Ref_Kind ref) {
    if (!n) {
        return;
    }

    bool is_ref_valid = false;
    switch (n->kind) {
    case NODE_ATOM: {
        static_assert(COUNT_TOKENS == 78, "");
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

        case TOKEN_IDENT:
            check_ident(c, n, ref);
            is_ref_valid = true; // check_ident() has already checked whether the reference is valid
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

        case TOKEN_DIRECTIVE_CALLER_LOCATION:
            error_node(
                EK_ERROR,
                n,
                "Cannot use %s here. It can only be used as the default value for a function argument",
                token_kind_to_cstr(n->token.kind));
            exit(1);
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_GROUP: {
        Node_Group *group = (Node_Group *) n;

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
        is_ref_valid = true;
    } break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        static_assert(COUNT_TOKENS == 78, "");
        switch (n->token.kind) {
        case TOKEN_SUB:
            check_expr(c, unary->value, REF_NONE);
            if (!type_is_numeric(unary->value->type) && !type_is_pointer(unary->value->type)) {
                unary->overload = get_operator_overload(c, "neg", unary->value, n, unary->module);
            }
            n->type = unary->value->type;
            break;

        case TOKEN_MUL: {
            check_expr(c, unary->value, REF_NONE);
            check_that_type_is_known(unary->value);

            if (!unary->value->type.ref) {
                if (type_kind_eq(unary->value->type, TYPE_RAWPTR)) {
                    error_node(EK_ERROR, unary->value, "Cannot dereference raw pointer");
                    exit(1);
                }

                error_node(EK_ERROR, unary->value, "Expected typed pointer, got %s", type_to_cstr(unary->value->type));
                exit(1);
            }

            n->type = unary->value->type;
            n->type.ref--;
            if (n->type.distinct && n->type.distinct->node.type.ref > n->type.ref) {
                n->type.distinct = NULL;
            }

            is_ref_valid = true;
            n->is_memory = true;
        } break;

        case TOKEN_BAND: {
            check_expr(c, unary->value, REF_ADDR);
            check_that_type_is_known(unary->value);
            n->type = unary->value->type;
            n->type.ref++;
        } break;

        case TOKEN_BNOT:
            check_expr(c, unary->value, REF_NONE);
            n->type = type_assert_numeric(unary->value, false);
            break;

        case TOKEN_LNOT:
            check_expr(c, unary->value, REF_NONE);
            n->type = type_assert(c, unary->value, (Type) {.kind = TYPE_BOOL});
            break;

        case TOKEN_SIZEOF:
            check_expr(c, unary->value, REF_NONE);
            check_that_type_is_known(unary->value);
            n->type = (Type) {.kind = TYPE_INT};
            break;

        case TOKEN_TYPEOF:
            check_expr(c, unary->value, REF_NONE);
            check_that_type_is_known(unary->value);
            n->type = unary->value->type;

            finalize_untyped_type(c, n);
            n->type.is_meta = true;
            break;

        default:
            unreachable();
        }
    } break;

    case NODE_BINARY:
        check_binary_expr(c, (Node_Binary *) n, true);
        break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
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
                case REF_ADDR_MEMBER:
                case REF_ASSIGN_MEMBER:
                    // Pass
                    break;
                }

                check_expr(c, member->lhs, ref_member);
            }

            check_that_type_is_known(member->lhs);

            is_ref_valid = true; // check_node() has already determined that the reference is valid

            // Method
            bool can_have_methods = false;
            {
                Method_Spec spec = {0};
                if (get_method_spec(c, member->lhs, member->lhs->type, n->token.sv, &spec, NULL, NULL)) {
                    can_have_methods = true;
                    member->method = get_method(c, spec, member->module);
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
                            exit(1);
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
                            exit(1);
                        }
                        is_ref_valid = ref == REF_NONE;
                    }
                }
            }

            if (!member->method) {
                n->is_memory = member->lhs->is_memory;
                if (member->lhs->type.is_meta && member->lhs->type.kind == TYPE_ENUM) {
                    check_whether_member_access_is_valid(member);
                    Node_Enum *enumm = member->lhs->type.spec.enumm.definition;
                    member->enum_value = get_enum_value(c, enumm, n->token.sv, &n->token);
                    member->is_enum = true;
                    n->type = member->lhs->type;
                    n->type.is_meta = false;
                } else if (type_kind_eq(member->lhs->type, TYPE_TRAIT)) {
                    check_whether_member_access_is_valid(member);

                    Type_Trait *spec = member->lhs->type.spec.trait;
                    if (member->rhs) {
                        check_expr(c, member->rhs, REF_NONE);
                        type_assert_type(member->rhs);
                        n->type = member->rhs->type;
                        n->type.is_meta = false;
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
                            error_undefined(c, &n->token, "field or method", false);
                        }
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_UNION)) {
                    check_whether_member_access_is_valid(member);
                    if (member->rhs) {
                        check_expr(c, member->rhs, REF_NONE);
                        type_assert_type(member->rhs);
                        member->union_index = get_union_type_index(member->rhs, member->lhs->type);
                        n->type = member->rhs->type;
                        n->type.is_meta = false;
                    } else {
                        if (sv_match(n->token.sv, "type")) {
                            n->type = (Type) {.kind = TYPE_I64};
                            member->field_index = 0;
                        } else {
                            error_undefined(c, &n->token, "field or method", false);
                        }
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_STRUCT)) {
                    check_whether_member_access_is_valid(member);
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
                        error_undefined(c, &n->token, "field or method", true);
                        error_node(EK_NOTE, (Node *) spec->definition, "Structure defined here");
                        exit(1);
                    }

                    n->type = definition->type;
                } else if (type_kind_eq(member->lhs->type, TYPE_ARRAY)) {
                    check_whether_member_access_is_valid(member);
                    if (sv_match(n->token.sv, "data")) {
                        n->type = *member->lhs->type.spec.array.element;
                        n->type.ref++;
                        member->field_index = 0;
                    } else if (sv_match(n->token.sv, "count")) {
                        n->type = (Type) {.kind = TYPE_I64};
                        member->field_index = 1;
                    } else {
                        error_undefined(c, &n->token, "field", false);
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_SLICE)) {
                    check_whether_member_access_is_valid(member);
                    if (sv_match(n->token.sv, "data")) {
                        n->type = *member->lhs->type.spec.slice.element;
                        n->type.ref++;
                        member->field_index = 0;
                    } else if (sv_match(n->token.sv, "count")) {
                        n->type = (Type) {.kind = TYPE_I64};
                        member->field_index = 1;
                    } else {
                        error_undefined(c, &n->token, "field", false);
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_STRING)) {
                    check_whether_member_access_is_valid(member);
                    if (sv_match(n->token.sv, "data")) {
                        n->type = (Type) {.kind = TYPE_CHAR, .ref = 1};
                        member->field_index = 0;
                    } else if (sv_match(n->token.sv, "count")) {
                        n->type = (Type) {.kind = TYPE_I64};
                        member->field_index = 1;
                    } else {
                        error_undefined(c, &n->token, "field", false);
                    }
                } else if (type_kind_eq(member->lhs->type, TYPE_MODULE)) {
                    check_whether_member_access_is_valid(member);
                    check_ident(c, n, ref);
                } else {
                    bool ok = false;
                    if (member->lhs->type.is_meta) {
                        Type receiver = member->lhs->type;
                        receiver.is_meta = false;

                        Method_Spec spec = {0};
                        if (get_method_spec(c, member->lhs, receiver, n->token.sv, &spec, NULL, NULL)) {
                            member->method = get_method(c, spec, member->module);
                            if (member->method) {
                                ok = true;
                                n->type = member->method->node.type;
                            } else {
                                error_undefined(c, &n->token, "method", false);
                            }
                        } else {
                            error_node(EK_ERROR, n, "There are no methods defined on %s", type_to_cstr(receiver));
                            exit(1);
                        }
                    }

                    if (!ok) {
                        if (can_have_methods) {
                            error_node(EK_ERROR, n, "Undefined method '" SV_Fmt "'", SV_Arg(n->token.sv));
                        } else {
                            error_node(EK_ERROR, n, "Cannot access field of %s", type_to_cstr(member->lhs->type));
                        }
                        exit(1);
                    }
                }
            }
        } else {
            check_whether_member_access_is_valid(member);
            n->type = (Type) {.kind = TYPE_UNKNOWN_ENUM};
            member->is_enum = true;
        }
    } break;

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
            exit(1);
        }

        check_expr(c, distinct->value, REF_NONE);
        type_assert_type(distinct->value);
        n->type = distinct->value->type;
        n->type.distinct = distinct->defined_as;
    } break;

    case NODE_POLYMORPH: {
        Node_Polymorph *polymorph = (Node_Polymorph *) n;
        if (polymorph->is_monomorphized) {
            n->type = polymorph->monomorphized_to;
            n->type.is_meta = true; // TODO(@polymorph): For now, only considering types to be polymorphic
        } else {
            n->type = polymorph->name->node.type;
        }
    } break;

    case NODE_INTERPOLATION: {
        Node_Interpolation *interpolation = (Node_Interpolation *) n;
        ensure_interpolation_is_valid(interpolation, false);

        ll_foreach(it, &interpolation->children) {
            check_expr(c, it, REF_NONE);
            type_assert(c, it, c->any_type);
            interpolation->children_count += type_kind_eq(it->type, TYPE_GROUP) ? it->type.spec.group.count : 1;
        }

        n->type = c->interpolated_string_type;
    } break;

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;

        Context_Fn context_fn = {.fn = fn, .outer = c->context.fn};
        context_push_fn(&c->context, &context_fn);

        {
            Type_Fn *fn_spec = arena_alloc(&default_arena, sizeof(*fn_spec));
            fn_spec->polymorphs =
                arena_alloc(&default_arena, fn->polymorphs.params_count * sizeof(*fn_spec->polymorphs));

            ll_foreach(it, &fn->polymorphs.params) {
                Node_Polymorph *polymorph = (Node_Polymorph *) it;
                Node_Atom      *previous =
                    context_find_define_in_fn(&c->context, c->context.fn, polymorph->name->node.token.sv);

                if (previous) {
                    error_redefinition((Node *) polymorph->name, &previous->node.token.pos);
                }
                context_push_define(&c->context, polymorph->name);

                polymorph->name->node.type = (Type) {
                    .kind = TYPE_POLYMORPH,
                    .spec.polymorph.definition = polymorph,
                    .spec.polymorph.is_definition = true,
                    .is_meta = true,
                };
                fn_spec->polymorphs[fn_spec->polymorphs_count++] = polymorph;
            }

            fn_spec->args_count = fn->args_count;
            fn_spec->args_count_min = fn->args_count_min;
            if (fn->trait_method_type) {
                assert(fn->is_type);
                fn_spec->args_count++;
                fn_spec->args_count_min++;
            }

            fn_spec->args = arena_alloc(&default_arena, fn_spec->args_count * sizeof(*fn_spec->args));
            fn_spec->variadics_kind = fn->variadics_kind;

            size_t iota = 0;
            if (fn->trait_method_type) {
                assert(fn->trait_method_type->kind == TYPE_TRAIT);

                Type_Fn_Arg *it_arg = &fn_spec->args[iota++];
                it_arg->name = sv_from_cstr("this");
                it_arg->pos = fn->trait_method_type->spec.trait->definition->node.token.pos;
                it_arg->type.kind = TYPE_RAWPTR;
            }

            for (Node *arg = fn->args.head; arg; arg = arg->next) {
                assert(arg->kind == NODE_DEFINE);
                Node_Define *define = (Node_Define *) arg;

                assert(define->name->kind == NODE_ATOM);
                Node_Atom *it = (Node_Atom *) define->name;
                if (!sv_match(it->node.token.sv, "_")) {
                    Node_Atom *previous = context_find_define_in_fn(&c->context, c->context.fn, it->node.token.sv);
                    if (previous) {
                        error_redefinition((Node *) it, &previous->node.token.pos);
                    }
                }

                Type_Fn_Arg *it_arg = &fn_spec->args[iota];
                it_arg->name = it->node.token.sv;
                it_arg->pos = it->node.token.pos;

                check_stmt(c, arg);
                if (define->has_spread) {
                    fn_spec->variadics_index = iota;
                    it->node.type.kind = TYPE_SLICE;
                    it->node.type.spec.slice.element = &define->type->type;
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
                it_arg->type = it->node.type;

                iota += define->count;
            }

            if (fn->returns.head) {
                fn_spec->returns = arena_alloc(&default_arena, fn->returns_count * sizeof(*fn_spec->returns));

                size_t iota = 0;
                ll_foreach(it, &fn->returns) {
                    check_expr(c, it, REF_NONE);
                    type_assert_type(it);

                    fn_spec->returns[iota] = it->type;
                    fn_spec->returns[iota].is_meta = false;
                    iota++;
                }
            }
            fn_spec->returns_count = fn->returns_count;

            Type return_type = {0};
            if (fn_spec->returns_count == 0) {
                return_type.kind = TYPE_UNIT;
            } else if (fn_spec->returns_count == 1) {
                return_type = *fn_spec->returns;
            } else {
                return_type.kind = TYPE_GROUP;
                return_type.spec.group.data = fn_spec->returns;
                return_type.spec.group.count = fn_spec->returns_count;
            }
            fn_spec->return_type = arena_clone(&default_arena, &return_type, sizeof(return_type));

            n->type = (Type) {.kind = TYPE_FN, .spec.fn = fn_spec};

            if (fn->defined_as && type_kind_eq(fn->defined_as->node.type, TYPE_UNIT)) {
                // The body of a function is irrelevant for outer expressions
                fn->defined_as->node.type = n->type;
                fn->defined_as->definition_spec->check_status = CHECKED;
            }

            if (fn->is_method) {
                if (!fn->defined_as) {
                    Node_Define *define = (Node_Define *) fn->args.head;
                    assert(define);

                    error_node(EK_ERROR, (Node *) fn, "Anonymous function cannot be a method");
                    error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                    exit(1);
                }

                assert(fn->defined_as);
                const SV name = fn->defined_as->node.token.sv;
                if (sv_match(name, "add") || sv_match(name, "sub") || sv_match(name, "mul") || sv_match(name, "div") ||
                    sv_match(name, "mod")) //
                {
                    const char *signature = "(this: T, that: T) -> T";
                    const char *note = NULL;
                    check_special_method_signature_args_count(fn, 2, signature, note);

                    const Type lhs_type = fn_spec->args[0].type;
                    if (lhs_type.ref) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_parts(
                            EK_NOTE,
                            fn_spec->args[0].name,
                            fn_spec->args[0].pos,
                            "Operand cannot be a pointer. (Provided type is %s)",
                            type_to_cstr(lhs_type));
                        exit(1);
                    }

                    const Type rhs_type = fn_spec->args[1].type;
                    if (!type_eq(rhs_type, lhs_type)) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_parts(
                            EK_NOTE,
                            fn_spec->args[1].name,
                            fn_spec->args[1].pos,
                            "Operand types must be same: Expected %s, got %s",
                            type_to_cstr(lhs_type),
                            type_to_cstr(rhs_type));
                        exit(1);
                    }

                    if (!type_eq(*fn_spec->return_type, lhs_type)) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_token(
                            EK_NOTE,
                            fn->returns.head ? fn->returns.head->token : fn->body->token,
                            "Operand types and return type must be same: Expected to return %s, got %s",
                            type_to_cstr(lhs_type),
                            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
                        exit(1);
                    }
                } else if (sv_match(name, "neg")) {
                    const char *signature = "(this: T) -> T";
                    const char *note = NULL;
                    check_special_method_signature_args_count(fn, 1, signature, note);

                    const Type operand_type = fn_spec->args[0].type;
                    if (operand_type.ref) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_parts(
                            EK_NOTE,
                            fn_spec->args[0].name,
                            fn_spec->args[0].pos,
                            "Operand cannot be a pointer. (Provided type is %s)",
                            type_to_cstr(operand_type));
                        exit(1);
                    }

                    if (!type_eq(*fn_spec->return_type, operand_type)) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_token(
                            EK_NOTE,
                            fn->returns.head ? fn->returns.head->token : fn->body->token,
                            "Operand type and return type must be same: Expected to return %s, got %s",
                            type_to_cstr(operand_type),
                            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
                        exit(1);
                    }
                } else if (sv_match(name, "compare")) {
                    const char *signature = "(this: T, that: T) -> Ordering | Equivalence";
                    const char *note =
                        "Return 'Ordering' if you want this method to implement both equality checking as well as ordered comparisons.\n"
                        "Otherwise return 'Equivalence' to implement just equality checking. Do NOT return 'Ordering | Equivalence' literally.\n";
                    check_special_method_signature_args_count(fn, 2, signature, note);

                    const Type lhs_type = fn_spec->args[0].type;
                    if (lhs_type.ref) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_parts(
                            EK_NOTE,
                            fn_spec->args[0].name,
                            fn_spec->args[0].pos,
                            "Operand cannot be a pointer. (Provided type is %s)",
                            type_to_cstr(lhs_type));
                        exit(1);
                    }

                    const Type rhs_type = fn_spec->args[1].type;
                    if (!type_eq(rhs_type, lhs_type)) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_parts(
                            EK_NOTE,
                            fn_spec->args[1].name,
                            fn_spec->args[1].pos,
                            "Operand types must be same: Expected %s, got %s",
                            type_to_cstr(lhs_type),
                            type_to_cstr(rhs_type));
                        exit(1);
                    }

                    if (!type_eq(*fn_spec->return_type, c->equivalence_type) &&
                        !type_eq(*fn_spec->return_type, c->ordering_type)) //
                    {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_token(
                            EK_NOTE,
                            fn->returns.head ? fn->returns.head->token : fn->body->token,
                            "Expected to return %s or %s, got %s",
                            type_to_cstr(c->equivalence_type),
                            type_to_cstr(c->ordering_type),
                            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
                        exit(1);
                    }

                    fn->is_compare_operator_complete = type_eq(*fn_spec->return_type, c->ordering_type);
                } else if (sv_match(name, "index")) {
                    const char *signature = "(this: T, key: K, assign: bool) -> &V";
                    const char *note = NULL;
                    check_special_method_signature_args_count(fn, 3, signature, note);

                    const Type assign_type = fn_spec->args[2].type;
                    if (!type_eq(assign_type, (Type) {.kind = TYPE_BOOL})) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_parts(
                            EK_NOTE,
                            fn_spec->args[2].name,
                            fn_spec->args[2].pos,
                            "Expected the third argument to be %s, got %s",
                            type_to_cstr((Type) {.kind = TYPE_BOOL}),
                            type_to_cstr(assign_type));
                        exit(1);
                    }

                    if (!type_is_pointer(*fn_spec->return_type)) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_token(
                            EK_NOTE,
                            fn->returns.head ? fn->returns.head->token : fn->body->token,
                            "Expected to return a pointer, got %s",
                            fn_spec->returns_count ? type_to_cstr(*fn_spec->return_type) : "nothing");
                        exit(1);
                    }
                } else if (sv_match(name, "range")) {
                    const char *signature = "(this: T, begin: A, end: A) -> V";
                    const char *note = NULL;
                    check_special_method_signature_args_count(fn, 3, signature, note);

                    const Type begin_type = fn_spec->args[1].type;
                    const Type end_type = fn_spec->args[2].type;
                    if (!type_eq(end_type, begin_type)) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_parts(
                            EK_NOTE,
                            fn_spec->args[2].name,
                            fn_spec->args[2].pos,
                            "Types of range beginning and end must be same: Expected %s, got %s",
                            type_to_cstr(begin_type),
                            type_to_cstr(end_type));
                        exit(1);
                    }

                    if (fn_spec->returns_count != 1) {
                        error_special_method_wrong_signature(fn->defined_as->node.token, signature, note);
                        error_token(
                            EK_NOTE,
                            fn->returns.head ? fn->returns.head->token : fn->body->token,
                            "The range operator cannot return %zu values",
                            fn_spec->returns_count);
                        exit(1);
                    }
                }
            }

            if (fn->is_type) {
                n->type.is_meta = true;
                is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
            } else if (fn->body && !fn->polymorphs.params_count) {
                check_stmt(c, fn->body);
                if (fn_spec->returns_count && !always_returns(fn->body)) {
                    assert(fn->body->kind == NODE_BLOCK);
                    error_token(
                        EK_ERROR,
                        ((Node_Block *) fn->body)->end,
                        "Expected to return %s",
                        type_to_cstr(*fn_spec->return_type));
                    exit(1);
                }
            }
        }

        context_pop_fn(&c->context);
    } break;

    case NODE_ENUM: {
        Node_Enum *enumm = (Node_Enum *) n;

        Type_Enum spec = {.underlying = TYPE_INT, .definition = enumm};
        Type      underlying = {.kind = spec.underlying};
        if (enumm->underlying) {
            check_expr(c, enumm->underlying, REF_NONE);
            type_assert_type(enumm->underlying);

            underlying = enumm->underlying->type;
            underlying.is_meta = false;
            if (!type_is_integer(underlying)) {
                error_node(
                    EK_ERROR,
                    enumm->underlying,
                    "Expected underlying type of the enumeration to be an integer, got %s",
                    type_to_cstr(underlying));
                exit(1);
            }

            spec.underlying = underlying.kind;
        }

        i64 iota = 0;
        i64 iota_max = 0;
        ll_foreach(it, &enumm->values) {
            ll_foreach(prev, &enumm->values) {
                if (prev == it) {
                    break;
                }

                if (sv_eq(it->token.sv, prev->token.sv)) {
                    error_redefinition(it, &prev->token.pos);
                }
            }

            assert(it->kind == NODE_UNARY);
            Node_Unary *unary = (Node_Unary *) it;
            if (unary->value) {
                check_expr(c, unary->value, REF_NONE);
                type_assert(c, unary->value, underlying);

                const Const_Value value = eval_const_expr(c, unary->value, false);
                assert(value.kind == CONST_VALUE_INT);
                iota = i64_from_int128(unary->value, value.as.integer, false, NULL);
            }
            iota_max = max(iota_max, iota);

            it->type.kind = underlying.kind;
            check_int_limit(it, int128_from_i64(iota));
            it->type.kind = TYPE_UNIT;
            it->token.as.integer = iota++;
        }

        n->type = (Type) {.kind = TYPE_ENUM, .is_meta = true, .spec.enumm = spec};
    } break;

    case NODE_TRAIT: {
        Node_Trait *trait = (Node_Trait *) n;

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
                    error_redefinition((const Node *) it, &previous->pos);
                }
            }

            it->definition_spec->is_local = false;
            check_definition(c, it, define->expr, define->type);
            assert(type_kind_eq(define->type->type, TYPE_FN) && !define->type->type.ref);

            Type_Trait_Method *tm = &spec->methods[iota++];
            tm->pos = it->node.token.pos;
            tm->name = it->node.token.sv;
            tm->type = define->type->type;
        }

        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
    } break;

    case NODE_UNION: {
        Node_Union *unionn = (Node_Union *) n;

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
            type_assert_type(it);

            Type_Union_Variant *variant = &spec->variants[iota];
            variant->pos = it->token.pos;
            variant->type = it->type;
            variant->type.is_meta = false;

            for (size_t i = 0; i < iota; i++) {
                const Type_Union_Variant *prev = &spec->variants[i];
                if (type_eq(prev->type, variant->type)) {
                    error_redefinition(it, &prev->pos);
                }
            }

            iota++;
        }

        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
    } break;

    case NODE_STRUCT: {
        Node_Struct *structt = (Node_Struct *) n;

        Type_Struct *spec = arena_alloc(&default_arena, sizeof(*spec));
        spec->definition = structt;

        n->type = (Type) {
            .kind = TYPE_STRUCT,
            .is_meta = true,
            .spec.structt = spec,
        };

        if (structt->defined_as) {
            structt->defined_as->node.type = n->type;
        }

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

                                    exit(1);
                                }
                                error_redefinition((Node *) it, &previous.pos);
                            }
                        }
                    }

                    it->definition_spec->is_local = false;
                    check_definition(c, it, define->expr, define->type);

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
                type_assert_type(unary->value);

                Type from = unary->value->type;
                from.is_meta = false;
                if (!type_kind_eq(from, TYPE_STRUCT)) {
                    error_node(EK_ERROR, unary->value, "Expected structure type, got %s", type_to_cstr(from));
                    exit(1);
                }

                if (from.ref) {
                    error_node(
                        EK_ERROR, unary->value, "Cannot spread %s without dereferencing it first", type_to_cstr(from));
                    exit(1);
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

                                exit(1);
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
        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
    } break;

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        if (compound->lhs) {
            check_expr(c, compound->lhs, REF_NONE);
            type_assert_type(compound->lhs);

            n->type = compound->lhs->type;
            n->type.is_meta = false;
            if (n->type.ref ||
                (n->type.kind != TYPE_STRUCT && n->type.kind != TYPE_ARRAY && n->type.kind != TYPE_SLICE)) {
                error_node(EK_ERROR, compound->lhs, "Expected structure or array type, got %s", type_to_cstr(n->type));
                exit(1);
            }
        } else {
            n->type = (Type) {.kind = TYPE_UNKNOWN_COMPOUND};
        }

        check_compound_expr(c, compound);
        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
        n->is_memory = true;
    } break;

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        check_expr(c, call->fn, REF_NONE);
        check_that_type_is_known(call->fn);

        const Type *fn_type = &call->fn->type;
        if (fn_type->is_meta) {
            call->is_type_cast = true;
            n->type = *fn_type;
            n->type.is_meta = false;

            check_call_arguments(c, call);
            Type *from_type = &call->args.head->type;
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
                    error_node(EK_ERROR, call->fn, "Cannot cast to %s", type_to_cstr(*to_type));
                    exit(1);
                }
            }

            if (!same) {
                if (to_any) {
                    // Pass
                } else if (to_trait) {
                    finalize_untyped_type(c, call->args.head);
                    call->type_cast_trait_impl =
                        check_type_satisfies_trait(c, *from_type, to_type->spec.trait, call->args.head, -1);
                } else if (to_union) {
                    finalize_untyped_type(c, call->args.head);
                    call->type_cast_union_index = get_union_type_index(call->args.head, *to_type);
                } else if (type_is_scalar(*to_type)) {
                    type_assert_scalar(call->args.head);

                    bool ok = true;
                    if (type_kind_eq(*from_type, TYPE_FN) && !from_type->ref) {
                        // fn -> rawptr
                        ok = type_eq(*to_type, (Type) {.kind = TYPE_RAWPTR});
                    } else if (type_kind_eq(*to_type, TYPE_FN) && !to_type->ref) {
                        // rawptr -> fn
                        ok = type_eq(*from_type, (Type) {.kind = TYPE_RAWPTR});
                    } else if (!type_is_pointer(*from_type) && type_is_pointer(*to_type)) {
                        // i64/u64 -> ptr
                        if (!type_kind_eq(*from_type, TYPE_I64) && !type_kind_eq(*from_type, TYPE_U64) &&
                            !type_kind_eq(*from_type, TYPE_INT)) {
                            ok = false;
                        }
                    } else if (type_is_pointer(*from_type) && !type_is_pointer(*to_type)) {
                        // ptr -> i64/u64
                        if (!type_kind_eq(*to_type, TYPE_I64) && !type_kind_eq(*to_type, TYPE_U64) &&
                            !type_kind_eq(*to_type, TYPE_INT)) {
                            ok = false;
                        }
                    } else if (
                        type_kind_eq(*from_type, TYPE_INT) &&
                        (type_is_integer(*to_type) || type_kind_eq(*to_type, TYPE_ENUM))) //
                    {
                        ok = try_auto_cast_untyped(c, call->args.head, n->type);
                        same = true;
                    }

                    if (!ok) {
                        error_node(
                            EK_ERROR,
                            (Node *) call,
                            "Cannot cast %s to %s",
                            type_to_cstr(*from_type),
                            type_to_cstr(*to_type));
                        exit(1);
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
        } else {
            if (!type_kind_eq(*fn_type, TYPE_FN)) {
                error_node(EK_ERROR, call->fn, "Cannot call %s", type_to_cstr(*fn_type));
                exit(1);
            }

            if (fn_type->ref) {
                error_node(EK_ERROR, call->fn, "Cannot call %s without deferencing it first", type_to_cstr(*fn_type));
                exit(1);
            }

            check_call_arguments(c, call);
            fn_type = &call->fn->type;

            n->type = *fn_type->spec.fn->return_type;
            if (!call->is_stmt && type_kind_eq(n->type, TYPE_UNIT)) {
                error_node(EK_ERROR, n, "This call cannot be used as a value as it does not return anything");
                exit(1);
            }
        }
    } break;

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;
        check_expr(c, index->lhs, ref);
        check_that_type_is_known(index->lhs);

        is_ref_valid = true; // check_node() has already determined that the reference is valid
        index->is_assign = ref == REF_ASSIGN || ref == REF_ASSIGN_MEMBER;
        if (index->is_ranged) {
            if (index->lhs->type.is_meta) {
                error_node(EK_ERROR, index->lhs, "Cannot take slice into %s", type_to_cstr(index->lhs->type));
                exit(1);
            }

            if (index->lhs->type.ref) {
                // The beginning can be inferred to be 0
                if (index->a) {
                    check_expr(c, index->a, REF_NONE);
                    type_assert_numeric(index->a, false);
                }

                // The ending CANNOT be inferred
                if (!index->b) {
                    error_node(EK_ERROR, n, "Cannot infer end of range from %s", type_to_cstr(index->lhs->type));
                    exit(1);
                }

                check_expr(c, index->b, REF_NONE);
                type_assert_numeric(index->b, false);

                Type element_type = index->lhs->type;
                element_type.ref--;
                n->type = (Type) {
                    .kind = TYPE_SLICE,
                    .spec.slice.element = arena_clone(&default_arena, &element_type, sizeof(element_type)),
                };
            } else if (
                type_kind_eq(index->lhs->type, TYPE_ARRAY) || type_kind_eq(index->lhs->type, TYPE_SLICE) ||
                type_kind_eq(index->lhs->type, TYPE_STRING)) //
            {
                // The beginning can be inferred to be the beginning of the slice
                if (index->a) {
                    check_expr(c, index->a, REF_NONE);
                    type_assert_numeric(index->a, false);
                }

                // The ending can be inferred to be the ending of the slice
                if (index->b) {
                    check_expr(c, index->b, REF_NONE);
                    type_assert_numeric(index->b, false);
                }

                n->type = index->lhs->type;
                if (type_kind_eq(n->type, TYPE_ARRAY)) {
                    n->type.kind = TYPE_SLICE;
                }
            } else {
                index->overload = get_operator_overload(c, "range", index->lhs, n, index->module);
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
                        "The method 'range' does not have a default value for its beginning argument");
                    exit(1);
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
                        "The method 'range' does not have a default value for its end argument");
                    exit(1);
                }

                n->type = *fn_spec->return_type;
            }

            is_ref_valid = ref == REF_NONE;
        } else {
            n->is_memory = index->lhs->is_memory;
            if (type_kind_eq(index->lhs->type, TYPE_ARRAY) && !index->lhs->type.ref) {
                check_expr(c, index->a, REF_NONE);
                type_assert_numeric(index->a, false);
                n->type = *index->lhs->type.spec.array.element;
            } else if (type_kind_eq(index->lhs->type, TYPE_SLICE) && !index->lhs->type.ref) {
                check_expr(c, index->a, REF_NONE);
                type_assert_numeric(index->a, false);
                n->type = *index->lhs->type.spec.slice.element;
            } else if (type_kind_eq(index->lhs->type, TYPE_STRING) && !index->lhs->type.ref) {
                check_expr(c, index->a, REF_NONE);
                type_assert_numeric(index->a, false);
                n->type = (Type) {.kind = TYPE_CHAR};
            } else {
                if (index->lhs->type.ref) {
                    error_node(
                        EK_ERROR, index->lhs, "Pointers must be converted into slices before they can be indexed");
                    if (is_indexable(c, index->lhs, index->lhs->type, index->module)) {
                        afprintf(
                            stderr,
                            ANSI_COLOR_YELLOW | ANSI_BOLD,
                            "    Here the value is %s. Perhaps it was meant to be dereferenced before indexing?\n",
                            type_to_cstr(index->lhs->type));
                    }
                    exit(1);
                }

                index->overload = get_operator_overload(c, "index", index->lhs, n, index->module);

                assert(index->overload->node.type.kind == TYPE_FN);
                const Type_Fn *fn_spec = index->overload->node.type.spec.fn;

                check_expr(c, index->a, REF_NONE);
                type_assert(c, index->a, fn_spec->args[1].type);

                n->type = *fn_spec->return_type;
                assert(n->type.ref);
                n->type.ref--;
            }
        }
    } break;

    case NODE_INDEXABLE: {
        Node_Indexable *indexable = (Node_Indexable *) n;

        size_t array_count = 0;
        if (indexable->count) {
            check_expr(c, indexable->count, REF_NONE);
            type_assert_numeric(indexable->count, false);

            const Const_Value value = eval_const_expr(c, indexable->count, false);
            assert(value.kind == CONST_VALUE_INT);

            check_int_limit_ex(indexable->count, value.as.integer, true, "array capacity");
            array_count = value.as.integer.low;
            check_expr(c, indexable->element, REF_NONE);
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
            check_expr(c, indexable->element, REF_ADDR);
        }

        Type *element_type = arena_alloc(&default_arena, sizeof(*element_type));
        *element_type = type_assert_type(indexable->element);
        element_type->is_meta = false;

        if (indexable->count) {
            n->type = (Type) {
                .kind = TYPE_ARRAY,
                .is_meta = true,
                .spec.array.element = element_type,
                .spec.array.count = array_count,
            };
        } else {
            n->type = (Type) {
                .kind = TYPE_SLICE,
                .is_meta = true,
                .spec.slice.element = element_type,
            };
        }

        is_ref_valid = ref == REF_ADDR || ref == REF_ADDR_MEMBER;
    } break;

    default:
        unreachable();
    }

    if (!is_ref_valid) {
        switch (ref) {
        case REF_NONE:
            // OK
            break;

        case REF_ADDR:
        case REF_ADDR_MEMBER:
            if (!n->type.is_meta) {
                error_node(EK_ERROR, n, "Cannot take reference to value not in memory");
                exit(1);
            }
            break;

        case REF_ASSIGN:
        case REF_ASSIGN_MEMBER:
            error_node(EK_ERROR, n, "Cannot assign to value not in memory");
            exit(1);
            break;
        }
    }
}

static void check_expr(Compiler *c, Node *n, Ref_Kind ref) {
    if (!n) {
        return;
    }

    check_expr_impl(c, n, ref);
    if (node_is_runtime_polymorphic_expression(n) && !n->is_called) {
        error_node(EK_ERROR, n, "Polymorphic functions cannot be used as runtime expressions. (They must be called)");
        exit(1);
    }
}

static_assert(COUNT_NODES == 29, "");
static void check_stmt(Compiler *c, Node *n) {
    if (!n) {
        return;
    }

    switch (n->kind) {
    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        check_expr(c, assertt->expr, REF_NONE);
        type_assert(c, assertt->expr, (Type) {.kind = TYPE_BOOL});

        if (assertt->message) {
            check_expr(c, assertt->message, REF_NONE);
            type_assert(c, assertt->message, (Type) {.kind = TYPE_STRING});
        }

        if (int128_is_zero(eval_const_expr(c, assertt->expr, false).as.integer)) {
            fprintf(stderr, Pos_Fmt " Assertion failed", Pos_Arg(n->token.pos));
            if (assertt->message) {
                const SV message = eval_const_expr(c, assertt->message, false).as.string;
                fprintf(stderr, ": " SV_Fmt, SV_Arg(message));
            }
            fprintf(stderr, "\n");
            exit(1);
        }
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        if (define->expr && define->is_value_known_at_compile_time) {
            Node_Atom *lhs = NULL;
            Node      *rhs = NULL;
            while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
                rhs = node_iter(rhs, define->expr);
                assert(rhs);
                check_definition(c, lhs, rhs, define->type);
            }
        } else {
            Node_Atom *lhs = NULL;
            while ((lhs = (Node_Atom *) node_iter((Node *) lhs, define->name))) {
                check_definition(c, lhs, define->expr, define->type);
            }
        }
    } break;

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;

        const size_t context_defines_end_save = c->context.fn->defines_end;
        const size_t context_imports_end_save = c->context.fn->imports_end;
        for (Node *it = block->body.head; it; it = it->next) {
            define_orderless_node(c, it, context_defines_end_save);
        }

        for (Node *it = block->body.head; it; it = it->next) {
            check_stmt(c, it);
        }
        context_set_end(&c->context, context_defines_end_save, context_imports_end_save);
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
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
    } break;

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;

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
    } break;

    case NODE_CASE:
        unreachable();

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
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
                            c,
                            &branch->context_replace,
                            ((Node_Atom *) sw->expr)->definition,
                            branch->preds.head->type);
                    }
                }

                check_stmt(c, branch->body);
                c->context.replace = branch->context_replace.outer;
            }
            assert(iota == sw->preds_count);

            check_switch_exhaustive(sw);
        }
    } break;

    case NODE_JUMP:
        // Pass
        break;

    case NODE_DEFER: {
        Node_Defer *defer = (Node_Defer *) n;
        check_stmt(c, defer->stmt);
    } break;

    case NODE_RETURN: {
        Node_Return   *returnn = (Node_Return *) n;
        const Type_Fn *fn_type = c->context.fn->fn->node.type.spec.fn;
        if (returnn->value) {
            check_expr(c, returnn->value, REF_NONE);

            const bool   is_group = type_kind_eq(returnn->value->type, TYPE_GROUP);
            const size_t actual_count = is_group ? returnn->value->type.spec.group.count : 1;

            if (actual_count != fn_type->returns_count) {
                error_number_of_return_values_mismatch(n->token, fn_type->returns_count, actual_count);
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
                error_number_of_return_values_mismatch(n->token, fn_type->returns_count, 0);
            }
        }

        n->type = *fn_type->return_type;
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        for (Node *it = externn->nodes.head; it; it = it->next) {
            check_stmt(c, it);
        }
    } break;

    default:
        check_expr(c, n, REF_NONE);
        check_that_type_is_known(n);
        break;
    }
}

Const_Value get_platform(Compiler *c, Type *type) {
    Const_Value platform = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Platform"), NULL);
    assert(platform.kind == CONST_VALUE_TYPE);

    Type platform_type = platform.as.type;
    assert(platform_type.is_meta);
    platform_type.is_meta = false;
    assert(platform_type.kind == TYPE_ENUM);

    if (type) {
        *type = platform_type;
    }

#ifdef PLATFORM_X86_64_LINUX
    return const_value_u64(CONTRACT_PLATFORM_LINUX);
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    return const_value_u64(CONTRACT_PLATFORM_MACOS);
#endif // PLATFORM_ARM64_MACOS

#ifdef PLATFORM_X86_64_WINDOWS
    return const_value_u64(CONTRACT_PLATFORM_WINDOWS);
#endif // PLATFORM_X86_64_WINDOWS

    unreachable();
}

Const_Value get_const_definition_value(Compiler *c, Module *m, SV name, Type *type) {
    Node_Atom *atom = module_globals_find(c, m, name);
    assert(atom);
    assert(atom->definition_spec->is_const);
    check_stmt(c, (Node *) atom->definition_spec->definition_node);

    if (type) {
        *type = atom->node.type;
    }
    return atom->definition_spec->const_value;
}

static uint64_t ht_hasheq_method_spec(const void *va, const void *vb, size_t n) {
    unused(n);

    const Method_Spec a = *(const Method_Spec *) va;
    if (vb) {
        const Method_Spec b = *(const Method_Spec *) vb;
        return a.uid == b.uid && sv_eq(a.name, b.name);
    }

    const uint64_t receiver_hash = ht_hasheq_bytes(&a.uid, NULL, sizeof(a.uid));
    const uint64_t name_hash = ht_hasheq_bytes(a.name.data, NULL, a.name.count);
    return ht_hash_combine(receiver_hash, name_hash);
}

void check_nodes(Compiler *c) {
    assert(c->parser);
    assert(c->modules);
    assert(c->main_module);
    assert(c->builtin_module);

    c->methods_table.hasheq = ht_hasheq_method_spec;

    {
        Type_Fn *fn_spec = arena_alloc(&default_arena, sizeof(*fn_spec));

        const Type unit = {.kind = TYPE_UNIT};
        fn_spec->return_type = arena_clone(&default_arena, &unit, sizeof(unit));

        c->main_fn_type = (Type) {
            .kind = TYPE_FN,
            .spec.fn = fn_spec,
        };
    }

    for (Module *m = c->modules->head; m; m = m->next) {
        define_orderless_nodes_of_module(c, m, NULL);
    }

    {
        const Const_Value value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Any"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->any_type = value.as.type;
        c->any_type.is_meta = false;

        c->interpolated_string_type = (Type) {
            .kind = TYPE_SLICE,
            .spec.slice.element = arena_clone(&default_arena, &c->any_type, sizeof(c->any_type)),
        };
    }

    // Type info
    {
        const Const_Value value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Type_Info"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->type_info_type = value.as.type;
        c->type_info_type.is_meta = false;

        c->type_info_pointer_type = c->type_info_type;
        c->type_info_pointer_type.ref++;

        assert(c->type_info_pointer_type.kind == TYPE_STRUCT);
        const Type_Struct *type_info_structure = c->type_info_pointer_type.spec.structt;

        assert(type_info_structure->fields_count == 4);
        const Type *type_info_variant = &type_info_structure->fields[3].type;

        assert(type_info_variant->kind == TYPE_UNION);
        c->type_info_variants_union = type_info_variant->spec.unionn;
        assert(c->type_info_variants_union->variants_count == 12);

        static_assert(COUNT_TYPES == 26, "");
        c->type_info_variants[TYPE_BOOL] = CONTRACT_TYPE_INFO_BOOLEAN;
        c->type_info_variants[TYPE_CHAR] = CONTRACT_TYPE_INFO_CHARACTER;

        c->type_info_variants[TYPE_I8] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_I16] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_I32] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_I64] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_INT] = CONTRACT_TYPE_INFO_INTEGER;

        c->type_info_variants[TYPE_U8] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_U16] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_U32] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_U64] = CONTRACT_TYPE_INFO_INTEGER;

        c->type_info_variants[TYPE_RAWPTR] = CONTRACT_TYPE_INFO_POINTER;

        c->type_info_variants[TYPE_FN] = CONTRACT_TYPE_INFO_FUNCTION;
        c->type_info_variants[TYPE_ENUM] = CONTRACT_TYPE_INFO_ENUMERATION;
        c->type_info_variants[TYPE_TRAIT] = CONTRACT_TYPE_INFO_TRAIT;
        c->type_info_variants[TYPE_UNION] = CONTRACT_TYPE_INFO_UNION;
        c->type_info_variants[TYPE_STRUCT] = CONTRACT_TYPE_INFO_STRUCTURE;

        c->type_info_variants[TYPE_ARRAY] = CONTRACT_TYPE_INFO_ARRAY;
        c->type_info_variants[TYPE_SLICE] = CONTRACT_TYPE_INFO_SLICE;
        c->type_info_variants[TYPE_STRING] = CONTRACT_TYPE_INFO_STRING;
    }

    // Comparisons
    {
        Const_Value value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Ordering"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->ordering_type = value.as.type;
        c->ordering_type.is_meta = false;

        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Equivalence"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->equivalence_type = value.as.type;
        c->equivalence_type.is_meta = false;
    }

    // Source code location
    {
        const Const_Value value =
            get_const_definition_value(c, c->builtin_module, sv_from_cstr("Source_Code_Location"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);

        c->source_code_location_type = value.as.type;
        c->source_code_location_type.is_meta = false;
    }

    // Define the methods
    {
        for (size_t i = 0; i < c->methods_list.count; i++) {
            Node_Fn *fn = c->methods_list.data[i];
            assert(fn->args.head && fn->args.head->kind == NODE_DEFINE); // Guaranteed by the parser

            Node_Define *define = (Node_Define *) fn->args.head;
            assert(define->name->kind == NODE_ATOM && define->type); // Guaranteed by the parser

            if (!fn->defined_as) {
                error_node(EK_ERROR, (Node *) fn, "Anonymous function cannot be a method");
                error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                exit(1);
            }
            const SV name = fn->defined_as->node.token.sv;

            check_expr(c, define->type, REF_NONE);
            type_assert_type(define->type);
            define->type->type.is_meta = false;
            const Type receiver_type = define->type->type;

            bool        is_named = false;
            Method_Spec spec = {0};
            if (type_kind_eq(receiver_type, TYPE_TRAIT)) {
                error_node(
                    EK_ERROR,
                    define->type,
                    "Cannot define methods on %s. (It is a trait)",
                    type_to_cstr(receiver_type));
                error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                exit(1);
            } else if (get_method_spec(c, define->type, receiver_type, name, &spec, fn->module, &is_named)) {
                if (!is_named) {
                    error_node(EK_ERROR, define->type, "The receiver of a method cannot have an anonymous type");
                    error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                    exit(1);
                }

                if (type_kind_eq(receiver_type, TYPE_ENUM)) {
                    ll_foreach(it, &receiver_type.spec.enumm.definition->values) {
                        if (sv_eq(it->token.sv, name)) {
                            error_redefinition((Node *) fn->defined_as, &it->token.pos);
                        }
                    }
                } else if (type_kind_eq(receiver_type, TYPE_STRUCT)) {
                    for (size_t i = 0; i < receiver_type.spec.structt->fields_count; i++) {
                        const Type_Struct_Field it = receiver_type.spec.structt->fields[i];
                        if (sv_eq(it.name, name)) {
                            error_redefinition((Node *) fn->defined_as, &it.pos);
                        }
                    }
                }

                Node_Fn **previous = ht_get(&c->methods_table, spec);
                if (previous) {
                    error_redefinition((Node *) fn->defined_as, &(*previous)->defined_as->node.token.pos);
                }
                ht_set(&c->methods_table, spec, fn);
            } else {
                error_node(EK_ERROR, define->type, "Can only define methods on types defined in the same module");
                error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                exit(1);
            }
        }
    }

    for (Module *m = c->modules->head; m; m = m->next) {
        for (Node *it = m->nodes.head; it; it = it->next) {
            check_stmt(c, it);
        }
    }

    get_main(c);
}

// TODO: Sometimes non-cyclic definitions are falsely flagged as cyclic
// TODO: Enum values is a bit broken (signedness)
//
// TODO: Apply the type restriction of special methods into traits
//       -> Or rather should we move from "special" methods into particular traits?
//       -> Perhaps after compile time polymorphism is implemented?
