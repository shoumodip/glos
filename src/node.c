#include "node.h"
#include "basic.h"
#include <stdio.h>

void nodes_push(Nodes *ns, Node *n) {
    if (!n) {
        return;
    }

    if (ns->tail) {
        ns->tail->next = n;
    } else {
        ns->head = n;
    }

    ns->tail = n;
}

void modules_free(Modules *ms) {
    ll_foreach(m, ms) {
        ht_free(&m->globals);
        da_free(&m->imports);
    }
    ht_free(&ms->table);
}

Type type_with_ref(Type t, size_t ref) {
    t.ref = ref;
    if (t.distinct && t.ref < t.distinct->node.type.ref) {
        t.distinct = NULL;
    }
    return t;
}

Type type_without_ref(Type t) {
    t.ref = 0;
    if (t.distinct && t.ref < t.distinct->node.type.ref) {
        t.distinct = NULL;
    }
    return t;
}

Type type_with_meta(Type t) {
    t.is_meta = true;
    return t;
}

Type type_without_meta(Type t) {
    t.is_meta = false;
    return t;
}

static void sb_push_polymorph(SB *sb, Node_Polymorph *p) {
    sb_push_sv(sb, p->name->node.token.sv);
    if (p->constraints.head) {
        if (p->constraints.head->next) {
            sb_push_cstr(sb, "/{");
        } else {
            sb_push(sb, '/');
        }

        ll_foreach(it, &p->constraints) {
            sb_push_type(sb, it->type);
            if (it->next) {
                sb_push_cstr(sb, ", ");
            }
        }

        if (p->constraints.head->next) {
            sb_push(sb, '}');
        }
    }
}

static void sb_push_polymorphs(SB *sb, Polymorphs ps) {
    sb_push(sb, '(');
    ll_foreach(it, &ps) {
        Node_Polymorph *monomorph = (Node_Polymorph *) it;
        if (monomorph->is_monomorphized) {
            sb_push_const_value(sb, monomorph->monomorphization_type, monomorph->monomorphization_value);
        } else {
            sb_push(sb, '$');
            sb_push_polymorph(sb, monomorph);
        }

        if (it->next) {
            sb_push_cstr(sb, ", ");
        }
    }
    sb_push(sb, ')');
}

static_assert(COUNT_TYPES == 30, "");
void sb_push_type(SB *sb, Type type) {
    assert(!type.is_meta);
    if (type.distinct) {
        const Type distinct_type = type.distinct->node.type;
        if (distinct_type.ref <= type.ref) {
            for (size_t i = distinct_type.ref; i < type.ref; i++) {
                sb_push(sb, '&');
            }
            sb_push_sv(sb, type.distinct->node.token.sv);
            return;
        }
    }

    for (size_t i = 0; i < type.ref; i++) {
        sb_push(sb, '&');
    }

    switch (type.kind) {
    case TYPE_VOID:
        sb_push_cstr(sb, "void");
        break;

    case TYPE_BOOL:
        sb_push_cstr(sb, "bool");
        break;

    case TYPE_CHAR:
        sb_push_cstr(sb, "char");
        break;

    case TYPE_S8:
        sb_push_cstr(sb, "s8");
        break;

    case TYPE_S16:
        sb_push_cstr(sb, "s16");
        break;

    case TYPE_S32:
        sb_push_cstr(sb, "s32");
        break;

    case TYPE_S64:
        sb_push_cstr(sb, "s64");
        break;

    case TYPE_U8:
        sb_push_cstr(sb, "u8");
        break;

    case TYPE_U16:
        sb_push_cstr(sb, "u16");
        break;

    case TYPE_U32:
        sb_push_cstr(sb, "u32");
        break;

    case TYPE_U64:
        sb_push_cstr(sb, "u64");
        break;

    case TYPE_F32:
        sb_push_cstr(sb, "f32");
        break;

    case TYPE_F64:
        sb_push_cstr(sb, "f64");
        break;

    case TYPE_INT:
        sb_push_cstr(sb, "s64");
        break;

    case TYPE_FLOAT:
        sb_push_cstr(sb, "f64");
        break;

    case TYPE_RAWPTR:
        sb_push_cstr(sb, "rawptr");
        break;

    case TYPE_FN: {
        const Type_Fn *spec = type.spec.fn;
        if (spec->is_noreturn) {
            sb_push_cstr(sb, "noreturn ");
        }

        sb_push(sb, '(');
        for (size_t i = 0; i < spec->args_count; i++) {
            Type_Fn_Arg it = spec->args[i];
            if (i) {
                sb_push_cstr(sb, ", ");
            }

            if (it.polymorph) {
                sb_push(sb, '$');
            }
            sb_sprintf(sb, SV_Fmt ": ", SV_Arg(it.name));

            Type it_type = it.type;
            if (spec->variadics_kind == VARIADICS_TYPED && i == spec->variadics_index) {
                sb_push_cstr(sb, "...");
                assert(it_type.kind == TYPE_SLICE);
                it_type = *it_type.spec.slice.element;
            }
            sb_push_type(sb, it_type);
        }

        if (spec->variadics_kind == VARIADICS_UNTYPED) {
            sb_push_cstr(sb, ", ...");
        }
        sb_push_cstr(sb, ")");

        if (spec->returns_count) {
            sb_push_cstr(sb, " -> ");
            sb_push_type(sb, *spec->return_type);
        }
    } break;

    case TYPE_ENUM: {
        assert(type.spec.enumm.definition);
        const Node_Atom *defined_as = type.spec.enumm.definition->defined_as;
        if (defined_as) {
            sb_push_sv(sb, defined_as->node.token.sv);
        } else {
            sb_push_cstr(sb, "enum ");
            sb_push_type(sb, (Type) {.kind = type.spec.enumm.underlying});
        }
    } break;

    case TYPE_TRAIT: {
        const Type_Trait *spec = type.spec.trait;
        const Node_Atom  *defined_as = spec->definition->defined_as;
        if (defined_as) {
            sb_push_sv(sb, defined_as->node.token.sv);
        } else {
            sb_push_cstr(sb, "trait {");
            if (spec->methods_count) {
                sb_push(sb, ' ');
            }

            for (size_t i = 0; i < spec->methods_count; i++) {
                const Type_Trait_Method it = spec->methods[i];
                if (i) {
                    sb_push_cstr(sb, "; ");
                }
                sb_sprintf(sb, SV_Fmt ": ", SV_Arg(it.name));
                sb_push_type(sb, it.type);
            }

            if (spec->methods_count) {
                sb_push(sb, ' ');
            }
            sb_push_cstr(sb, "}");
        }
    } break;

    case TYPE_UNION: {
        const Type_Union *spec = type.spec.unionn;
        const Node_Atom  *defined_as = spec->definition->defined_as;
        if (defined_as) {
            sb_push_sv(sb, defined_as->node.token.sv);
        } else {
            sb_push_cstr(sb, "union {");
            if (spec->variants_count) {
                sb_push(sb, ' ');
            }

            for (size_t i = 0; i < spec->variants_count; i++) {
                Type_Union_Variant it = spec->variants[i];
                if (i) {
                    sb_push_cstr(sb, "; ");
                }
                sb_push_type(sb, it.type);
            }

            if (spec->variants_count) {
                sb_push(sb, ' ');
            }
            sb_push_cstr(sb, "}");
        }
    } break;

    case TYPE_STRUCT: {
        const Type_Struct *spec = type.spec.structt;
        const Node_Atom   *defined_as = spec->definition->defined_as;
        if (defined_as) {
            sb_push_sv(sb, defined_as->node.token.sv);
        } else {
            sb_push_cstr(sb, "struct");
        }

        if (spec->definition->polymorphs.count) {
            assert(!spec->definition->monomorphs.count);
            sb_push_polymorphs(sb, spec->definition->polymorphs);
        }

        if (spec->definition->monomorphs.count) {
            assert(!spec->definition->polymorphs.count);
            sb_push_polymorphs(sb, spec->definition->monomorphs);
        }

        if (!defined_as) {
            sb_push_cstr(sb, " {");
            if (spec->fields_count) {
                sb_push(sb, ' ');
            }

            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field it = spec->fields[i];
                if (i) {
                    sb_push_cstr(sb, "; ");
                }

                sb_sprintf(sb, SV_Fmt ": ", SV_Arg(it.name));
                sb_push_type(sb, it.type);
            }

            if (spec->fields_count) {
                sb_push(sb, ' ');
            }
            sb_push_cstr(sb, "}");
        }
    } break;

    case TYPE_ARRAY: {
        const Type_Array array = type.spec.array;
        sb_sprintf(sb, "[");
        if (array.count_polymorph) {
            sb_push_type(sb, type_without_meta(array.count_polymorph->node.type));
        } else {
            sb_sprintf(sb, "%zu", array.count);
        }
        sb_sprintf(sb, "]");
        sb_push_type(sb, *array.element);
    } break;

    case TYPE_DYNAMIC_ARRAY:
        sb_push_cstr(sb, "[..]");
        sb_push_type(sb, *type.spec.dynamic_array.element);
        break;

    case TYPE_SLICE:
        sb_push_cstr(sb, "[]");
        sb_push_type(sb, *type.spec.slice.element);
        break;

    case TYPE_STRING:
        sb_push_cstr(sb, "string");
        break;

    case TYPE_POLYMORPH: {
        const Type_Polymorph spec = type.spec.polymorph;
        if (spec.is_definition) {
            sb_push(sb, '$');
        }
        sb_push_polymorph(sb, spec.definition);
    } break;

    case TYPE_GROUP:
        sb_push_cstr(sb, "(");
        for (size_t i = 0; i < type.spec.group.count; i++) {
            if (i) {
                sb_push_cstr(sb, ", ");
            }
            sb_push_type(sb, type.spec.group.data[i]);
        }
        sb_push_cstr(sb, ")");
        break;

    case TYPE_MODULE:
        unreachable();

    case TYPE_UNKNOWN_ENUM:
    case TYPE_UNKNOWN_COMPOUND:
        unreachable();

    default:
        unreachable();
    }
}

const char *type_to_cstr_raw(Type type) {
    const size_t start = default_sb.count;
    sb_push_type(&default_sb, type);
    return arena_sb_to_cstr(&temp_arena, &default_sb, start);
}

const char *type_to_cstr(Type type) {
    if (type.is_meta) {
        return arena_sprintf(&temp_arena, "a type");
    }

    if (type.kind == TYPE_MODULE) {
        return arena_sprintf(&temp_arena, "a module");
    }

    if (type.kind == TYPE_UNKNOWN_ENUM) {
        return arena_sprintf(&temp_arena, "unknown enum");
    }

    const size_t start = default_sb.count;
    sb_push(&default_sb, '\'');
    sb_push_type(&default_sb, type);
    sb_push(&default_sb, '\'');
    return arena_sb_to_cstr(&temp_arena, &default_sb, start);
}

static bool type_trait_eq(Type_Trait *a, Type_Trait *b) {
    if (a->definition->defined_as || b->definition->defined_as) {
        return a->definition->defined_as == b->definition->defined_as;
    }

    if (a->definition->methods_count != b->definition->methods_count) {
        return false;
    }

    if (a->methods == b->methods) {
        return true;
    }

    for (size_t i = 0; i < a->methods_count; i++) {
        if (!type_eq(a->methods[i].type, b->methods[i].type)) {
            return false;
        }
    }

    return true;
}

static bool type_union_eq(Type_Union *a, Type_Union *b) {
    if (a->definition->defined_as || b->definition->defined_as) {
        return a->definition->defined_as == b->definition->defined_as;
    }

    if (a->variants_count != b->variants_count) {
        return false;
    }

    if (a->variants == b->variants) {
        return true;
    }

    for (size_t i = 0; i < a->variants_count; i++) {
        if (!type_eq(a->variants[i].type, b->variants[i].type)) {
            return false;
        }
    }

    return true;
}

static bool type_struct_eq(Type_Struct *a, Type_Struct *b) {
    if (a->definition == b->definition) {
        return true;
    }

    if (a->definition->defined_as != b->definition->defined_as) {
        return false;
    }

    if (a->fields_count != b->fields_count) {
        return false;
    }

    if (a->fields == b->fields) {
        return true;
    }

    for (size_t i = 0; i < a->fields_count; i++) {
        if (!type_eq(a->fields[i].type, b->fields[i].type)) {
            return false;
        }
    }

    return true;
}

static_assert(COUNT_TYPES == 30, "");
bool type_eq(Type a, Type b) {
    if (a.is_meta) {
        return b.is_meta;
    }

    if (b.is_meta) {
        return a.is_meta;
    }

    if (a.kind != b.kind || a.ref != b.ref) {
        return false;
    }

    if (a.distinct || b.distinct) {
        return a.distinct == b.distinct;
    }

    switch (a.kind) {
    case TYPE_FN: {
        const Type_Fn *as = a.spec.fn;
        const Type_Fn *bs = b.spec.fn;
        if (as->args == bs->args && as->return_type == bs->return_type) {
            return true;
        }

        if (as->is_noreturn != bs->is_noreturn) {
            return false;
        }

        if (as->args_count != bs->args_count || as->returns_count != bs->returns_count) {
            return false;
        }

        for (size_t i = 0; i < as->args_count; i++) {
            if (!type_eq(as->args[i].type, bs->args[i].type)) {
                return false;
            }
        }

        return type_eq(*as->return_type, *bs->return_type);
    }

    case TYPE_ENUM:
        return a.spec.enumm.definition == b.spec.enumm.definition;

    case TYPE_TRAIT:
        return type_trait_eq(a.spec.trait, b.spec.trait);

    case TYPE_UNION:
        return type_union_eq(a.spec.unionn, b.spec.unionn);

    case TYPE_STRUCT:
        return type_struct_eq(a.spec.structt, b.spec.structt);

    case TYPE_ARRAY:
        return a.spec.array.count == b.spec.array.count && type_eq(*a.spec.array.element, *b.spec.array.element);

    case TYPE_DYNAMIC_ARRAY:
        return type_eq(*a.spec.dynamic_array.element, *b.spec.dynamic_array.element);

    case TYPE_SLICE:
        return type_eq(*a.spec.slice.element, *b.spec.slice.element);

    case TYPE_GROUP:
        if (a.spec.group.count != b.spec.group.count) {
            return false;
        }

        for (size_t i = 0; i < a.spec.group.count; i++) {
            if (!type_eq(a.spec.group.data[i], b.spec.group.data[i])) {
                return false;
            }
        }

        return true;

    case TYPE_UNKNOWN_ENUM:
        return true;

    case TYPE_UNKNOWN_COMPOUND:
        return true;

    default:
        return true;
    }
}

bool type_kind_eq(Type type, Type_Kind kind) {
    return !type.is_meta && type.kind == kind;
}

bool type_meta_kind_eq(Type type, Type_Kind kind) {
    return type.is_meta && type.kind == kind;
}

bool type_is_numeric(Type type) {
    return type_is_integer(type) || type_is_float(type) || type_kind_eq(type, TYPE_ENUM) ||
           type_kind_eq(type, TYPE_UNKNOWN_ENUM) || type_kind_eq(type, TYPE_UNKNOWN_COMPOUND);
}

static_assert(COUNT_TYPES == 30, "");
bool type_is_integer(Type type) {
    if (type.ref || type.is_meta) {
        return false;
    }

    switch (type.kind) {
    case TYPE_S8:
    case TYPE_S16:
    case TYPE_S32:
    case TYPE_S64:

    case TYPE_U8:
    case TYPE_U16:
    case TYPE_U32:
    case TYPE_U64:

    case TYPE_INT:
        return true;

    default:
        return false;
    }
}

static_assert(COUNT_TYPES == 30, "");
bool type_is_float(Type type) {
    if (type.ref || type.is_meta) {
        return false;
    }
    return type.kind == TYPE_F32 || type.kind == TYPE_F64 || type.kind == TYPE_FLOAT;
}

bool type_is_pointer(Type type) {
    if (type.is_meta) {
        return false;
    }
    return type.ref != 0 || type.kind == TYPE_RAWPTR;
}

bool type_is_scalar(Type type) {
    if (type.is_meta) {
        return false;
    }

    if (type_is_numeric(type) || type_is_pointer(type)) {
        return true;
    }

    if (type.kind == TYPE_BOOL || type.kind == TYPE_CHAR || type.kind == TYPE_FN) {
        return true;
    }

    return false;
}

static_assert(COUNT_TYPES == 30, "");
bool type_is_signed(Type type) {
    if (type.ref || type.is_meta) {
        return false;
    }

    Type_Kind kind = type.kind;
    if (kind == TYPE_ENUM) {
        kind = type.spec.enumm.underlying;
    }

    switch (kind) {
    case TYPE_S8:
    case TYPE_S16:
    case TYPE_S32:
    case TYPE_S64:
    case TYPE_INT:

    case TYPE_F32:
    case TYPE_F64:
    case TYPE_FLOAT:

    case TYPE_UNKNOWN_ENUM:
    case TYPE_UNKNOWN_COMPOUND:
        return true;

    default:
        return false;
    }
}

static_assert(COUNT_TYPES == 30, "");
bool type_is_untyped(Type type) {
    if (type.is_meta || type.ref) {
        return false;
    }

    return type.kind == TYPE_INT || type.kind == TYPE_FLOAT;
}

static_assert(COUNT_TYPES == 30, "");
bool type_is_unknown(Type type) {
    if (type.is_meta || type.ref) {
        return false;
    }

    return type.kind == TYPE_UNKNOWN_ENUM || type.kind == TYPE_UNKNOWN_COMPOUND;
}

static_assert(COUNT_CONST_VALUES == 13, "");
bool const_value_eq(Const_Value a, Const_Value b) {
    if (a.kind != b.kind) {
        return false;
    }

    switch (a.kind) {
    case CONST_VALUE_INT:
        return int128_eq(a.as.integer, b.as.integer);

    case CONST_VALUE_FLOAT:
        return a.as.real == b.as.real;

    case CONST_VALUE_FN:
        return a.as.fn == b.as.fn;

    case CONST_VALUE_VAR:
        return a.as.var == b.as.var;

    case CONST_VALUE_TYPE:
        return type_eq(a.as.type, b.as.type);

    case CONST_VALUE_TRAIT:
        if (a.as.trait.impl != b.as.trait.impl) {
            return false;
        }

        if (!a.as.trait.type || !b.as.trait.type) {
            return a.as.trait.type == b.as.trait.type;
        }

        if (!type_eq(*a.as.trait.type, *b.as.trait.type)) {
            return false;
        }

        if (!a.as.trait.data || !b.as.trait.data) {
            return a.as.trait.data == b.as.trait.data;
        }

        return const_value_eq(*a.as.trait.data, *b.as.trait.data);

    case CONST_VALUE_UNION:
        if (!type_union_eq(a.as.unionn.spec, b.as.unionn.spec)) {
            return false;
        }

        if (a.as.unionn.index != b.as.unionn.index) {
            return false;
        }

        if (a.as.unionn.index == 0) {
            return true;
        }

        assert(a.as.unionn.real);
        assert(b.as.unionn.real);
        return const_value_eq(*a.as.unionn.real, *a.as.unionn.real);

    case CONST_VALUE_STRUCT:
        if (!type_struct_eq(a.as.structt.spec, b.as.structt.spec)) {
            return false;
        }

        for (size_t i = 0; i < a.as.structt.spec->fields_count; i++) {
            if (!const_value_eq(a.as.structt.fields[i], b.as.structt.fields[i])) {
                return false;
            }
        }

        return true;

    case CONST_VALUE_ARRAY:
        if (!type_eq(*a.as.array.element_type, *b.as.array.element_type)) {
            return false;
        }

        if (a.as.array.count != b.as.array.count) {
            return false;
        }

        for (size_t i = 0; i < a.as.array.count; i++) {
            if (!const_value_eq(a.as.array.data[i], b.as.array.data[i])) {
                return false;
            }
        }

        return true;

    case CONST_VALUE_DYNAMIC_ARRAY:
        return type_eq(*a.as.dynamic_array, *b.as.dynamic_array);

    case CONST_VALUE_STRING:
        return sv_eq(a.as.string, b.as.string);

    case CONST_VALUE_MODULE:
        unreachable();

    case CONST_VALUE_POLYMORPH:
        // I don't know why but this branch never triggers. Perhaps it is indeed unreachable, although that would need
        // more usage to conclude.
        unreachable();

    default:
        unreachable();
    }
}

static_assert(COUNT_CONST_VALUES == 13, "");
static void sb_push_const_value_impl(SB *sb, Type type, Const_Value v, bool raw) {
    switch (v.kind) {
    case CONST_VALUE_INT:
        if (type_kind_eq(type, TYPE_CHAR)) {
            if (raw) {
                sb_push(sb, (char) v.as.integer.low);
            } else {
                sb_push(sb, '\'');
                sb_push_quoted_char(sb, (char) v.as.integer.low, '\'');
                sb_push(sb, '\'');
            }
        } else {
            sb_push_cstr(sb, int128_to_cstr(v.as.integer));
        }
        break;

    case CONST_VALUE_FLOAT:
        sb_sprintf(sb, "%.14g", v.as.real);
        break;

    case CONST_VALUE_FN:
        sb_push_fn_name(sb, v.as.fn, v.as.fn->node.module);
        break;

    case CONST_VALUE_VAR: {
        Node_Atom *definition = v.as.var;
        assert(definition->definition_spec); // This is a variable
        sb_push(sb, '&');
        sb_push_fn_name(sb, definition->definition_spec->static_var_fn, definition->node.module);
        sb_sprintf(sb, "." SV_Fmt, SV_Arg(definition->node.token.sv));
    } break;

    case CONST_VALUE_TYPE:
        sb_push_type(sb, type_without_meta(v.as.type));
        break;

    case CONST_VALUE_TRAIT:
        if (v.as.trait.data) {
            sb_push_const_value_impl(sb, *v.as.trait.type, *v.as.trait.data, raw);
        } else {
            sb_push_cstr(sb, "null");
        }
        break;

    case CONST_VALUE_UNION:
        if (v.as.unionn.real) {
            sb_push_const_value_impl(sb, v.as.unionn.spec->variants[v.as.unionn.index].type, *v.as.unionn.real, raw);
        } else {
            sb_push_cstr(sb, "null");
        }
        break;

    case CONST_VALUE_STRUCT: {
        Const_Value_Struct structure = v.as.structt;
        sb_push(sb, '{');
        for (size_t i = 0; i < structure.spec->fields_count; i++) {
            if (i) {
                sb_push_cstr(sb, ", ");
            }
            sb_push_const_value_impl(sb, structure.spec->fields[i].type, structure.fields[i], false);
        }
        sb_push(sb, '}');
    } break;

    case CONST_VALUE_ARRAY: {
        Const_Value_Array array = v.as.array;
        sb_push(sb, '{');
        for (size_t i = 0; i < array.count; i++) {
            if (i) {
                sb_push_cstr(sb, ", ");
            }
            sb_push_const_value_impl(sb, *array.element_type, array.data[i], false);
        }
        sb_push(sb, '}');
    } break;

    case CONST_VALUE_DYNAMIC_ARRAY:
        sb_push_cstr(sb, "{}");
        break;

    case CONST_VALUE_STRING:
        if (raw) {
            sb_push_sv(sb, v.as.string);
        } else {
            sb_push(sb, '"');
            for (size_t i = 0; i < v.as.string.count; i++) {
                sb_push_quoted_char(sb, v.as.string.data[i], '"');
            }
            sb_push(sb, '"');
        }
        break;

    case CONST_VALUE_MODULE:
        unreachable();

    case CONST_VALUE_POLYMORPH: {
        Node_Polymorph *polymorph = v.as.polymorph.polymorph;
        if (polymorph->is_monomorphized) {
            sb_push_const_value_impl(sb, polymorph->monomorphization_type, polymorph->monomorphization_value, false);
        } else {
            if (v.as.polymorph.is_definition) {
                sb_push(sb, '$');
            }
            sb_push_polymorph(sb, v.as.polymorph.polymorph);
        }
    } break;

    default:
        unreachable();
    }
}

void sb_push_const_value(SB *sb, Type type, Const_Value v) {
    sb_push_const_value_impl(sb, type, v, false);
}

void sb_push_const_value_raw(SB *sb, Type type, Const_Value v) {
    sb_push_const_value_impl(sb, type, v, true);
}

void const_value_debug(FILE *f, Type type, Const_Value v) {
    const size_t start = default_sb.count;
    sb_push_const_value(&default_sb, type, v);
    fwrite(default_sb.data + start, default_sb.count - start, 1, f);
    default_sb.count = start;
}

static_assert(COUNT_NODES == 29, "");
size_t node_size(Node_Kind kind) {
    static const size_t sizes[COUNT_NODES] = {
        [NODE_ATOM] = sizeof(Node_Atom), // This comment is here to prevent clang-format from messing this up
        [NODE_GROUP] = sizeof(Node_Group),
        [NODE_UNARY] = sizeof(Node_Unary),
        [NODE_BINARY] = sizeof(Node_Binary),
        [NODE_MEMBER] = sizeof(Node_Member),
        [NODE_ASSERT] = sizeof(Node_Assert),
        [NODE_IMPORT] = sizeof(Node_Import),
        [NODE_DISTINCT] = sizeof(Node_Distinct),
        [NODE_POLYMORPH] = sizeof(Node_Polymorph),
        [NODE_INTERPOLATION] = sizeof(Node_Interpolation),

        // This comment is here to prevent clang-format from messing this up
        [NODE_FN] = sizeof(Node_Fn),
        [NODE_ENUM] = sizeof(Node_Enum),
        [NODE_TRAIT] = sizeof(Node_Trait),
        [NODE_UNION] = sizeof(Node_Union),
        [NODE_STRUCT] = sizeof(Node_Struct),
        [NODE_COMPOUND] = sizeof(Node_Compound),

        [NODE_CALL] = sizeof(Node_Call),
        [NODE_INDEX] = sizeof(Node_Index),
        [NODE_INDEXABLE] = sizeof(Node_Indexable),

        [NODE_DEFINE] = sizeof(Node_Define),
        [NODE_BLOCK] = sizeof(Node_Block),
        [NODE_IF] = sizeof(Node_If),
        [NODE_FOR] = sizeof(Node_For),

        [NODE_CASE] = sizeof(Node_Case),
        [NODE_SWITCH] = sizeof(Node_Switch),

        [NODE_JUMP] = sizeof(Node_Jump),
        [NODE_DEFER] = sizeof(Node_Defer),
        [NODE_RETURN] = sizeof(Node_Return),

        [NODE_EXTERN] = sizeof(Node_Extern),
    };

    assert(kind >= NODE_ATOM && kind < COUNT_NODES);
    return sizes[kind];
}

Node *node_alloc(Module *module, Node_Kind kind, Token token) {
    Node *node = arena_alloc(&default_arena, node_size(kind));
    node->kind = kind;
    node->token = token;
    node->module = module;
    return node;
}

Node *node_iter(Node *it, Node *ll) {
    if (it) {
        if (ll->kind == NODE_GROUP) {
            return it->next;
        } else {
            return NULL;
        }
    } else {
        if (ll->kind == NODE_GROUP) {
            return ((Node_Group *) ll)->nodes.head;
        } else {
            return ll;
        }
    }
}

void polymorphs_push(Polymorphs *ps, Node_Polymorph *p) {
    if (!p) {
        return;
    }

    if (ps->tail) {
        ps->tail->next = p;
    } else {
        ps->head = p;
    }

    ps->tail = p;
    ps->count++;
}

void sb_push_fn_name(SB *sb, Node_Fn *fn, Module *module) {
    if (!fn) {
        sb_push_sv(sb, module->name);
        return;
    }

    if (fn->wrapper) {
        assert(fn->defined_as && !fn->outer_fn && fn->wrapper_for_trait);

        Node_Trait *definition = fn->wrapper_for_trait->definition;
        sb_push_fn_name(sb, definition->defined_in, definition->node.module);
        sb_push(sb, '.');
        sb_push_type(sb, (Type) {.kind = TYPE_TRAIT, .spec.trait = fn->wrapper_for_trait});
        sb_push(sb, '(');
        sb_push_fn_name(sb, fn->wrapper, fn->node.module);
        sb_push(sb, ')');
        return;
    }

    sb_push_fn_name(sb, fn->outer_fn, module);
    if (fn->is_method) {
        assert(fn->defined_as);
        assert(!fn->outer_fn);

        assert(fn->node.type.kind == TYPE_FN);
        const Type_Fn *fn_spec = fn->node.type.spec.fn;

        assert(fn_spec->args_count);
        sb_sprintf(sb, ".");

        Type receiver = fn_spec->args[0].type;
        receiver.ref = 0;
        sb_push_type(sb, receiver);
    }

    if (fn->defined_as) {
        sb_sprintf(sb, "." SV_Fmt, SV_Arg(fn->defined_as->node.token.sv));
    } else {
        if (!fn->defined_as_anon_iota) {
            static size_t iota = 0;
            fn->defined_as_anon_iota = ++iota;
        }
        sb_sprintf(sb, ".anon.%zu", fn->defined_as_anon_iota);
    }

    if (fn->monomorphs.count) {
        sb_push(sb, '(');
        ll_foreach(it, &fn->monomorphs) {
            assert(it->is_monomorphized);
            sb_sprintf(sb, "$" SV_Fmt " = ", SV_Arg(it->name->node.token.sv));
            sb_push_const_value(sb, it->node.type, it->monomorphization_value);

            if (it->next) {
                sb_push_cstr(sb, ", ");
            }
        }
        sb_push(sb, ')');
    }
}

#define Indent_Fmt    "%*s"
#define Indent_Arg(d) (d) * 4, ""

static void node_debug_impl(FILE *f, const Node *n, int depth, const char *label);

static void nodes_debug_impl(FILE *f, Nodes ns, int depth, const char *label) {
    fprintf(f, Indent_Fmt, Indent_Arg(depth));
    if (label) {
        fprintf(f, "%s = {", label);
        if (ns.head) {
            fprintf(f, "\n");
        } else {
            fprintf(f, "}\n");
            return;
        }
    }

    const size_t child_depth = depth + (label != NULL);
    ll_foreach(it, &ns) {
        node_debug_impl(f, it, child_depth, NULL);
    }

    if (label) {
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    }
}

static void polymorphs_debug_impl(FILE *f, Polymorphs ns, int depth, const char *label) {
    fprintf(f, Indent_Fmt, Indent_Arg(depth));
    if (label) {
        fprintf(f, "%s = {", label);
        if (ns.head) {
            fprintf(f, "\n");
        } else {
            fprintf(f, "}\n");
            return;
        }
    }

    const size_t child_depth = depth + (label != NULL);
    ll_foreach(it, &ns) {
        node_debug_impl(f, (Node *) it, child_depth, NULL);
    }

    if (label) {
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    }
}

static_assert(COUNT_NODES == 29, "");
static void node_debug_impl(FILE *f, const Node *n, int depth, const char *label) {
    if (!n) {
        return;
    }

    fprintf(f, Indent_Fmt, Indent_Arg(depth));
    fprintf(f, "[%p] (%s) ", (void *) n, type_to_cstr(n->type)); // @remove
    if (label) {
        fprintf(f, "%s = ", label);
    }

    switch (n->kind) {
    case NODE_ATOM: {
        Node_Atom *atom = (Node_Atom *) n;
        if (atom->polymorph) {
            fprintf(f, "Atom $" SV_Fmt "\n", SV_Arg(n->token.sv));
        } else {
            fprintf(f, "Atom " SV_Fmt "\n", SV_Arg(n->token.sv));
        }
    } break;

    case NODE_GROUP: {
        Node_Group *group = (Node_Group *) n;
        fprintf(f, "Group {\n");
        nodes_debug_impl(f, group->nodes, depth + 1, "Nodes");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_UNARY: {
        Node_Unary *unary = (Node_Unary *) n;
        fprintf(f, "Unary '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        node_debug_impl(f, unary->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_BINARY: {
        Node_Binary *binary = (Node_Binary *) n;
        fprintf(f, "Binary '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        node_debug_impl(f, binary->lhs, depth + 1, "Lhs");
        node_debug_impl(f, binary->rhs, depth + 1, "Rhs");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_MEMBER: {
        Node_Member *member = (Node_Member *) n;
        fprintf(f, "Member '" SV_Fmt "' {\n", SV_Arg(n->token.sv));
        node_debug_impl(f, member->lhs, depth + 1, "Lhs");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_ASSERT: {
        Node_Assert *assertt = (Node_Assert *) n;
        fprintf(f, "Assert {\n");
        node_debug_impl(f, assertt->expr, depth + 1, "Expr");
        node_debug_impl(f, assertt->message, depth + 1, "Message");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_IMPORT: {
        Node_Import *import = (Node_Import *) n;
        fprintf(f, "Import '%s'\n ", import->module->relative_path);
    } break;

    case NODE_DISTINCT: {
        Node_Distinct *distinct = (Node_Distinct *) n;
        fprintf(f, "Distinct {\n");
        node_debug_impl(f, distinct->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_POLYMORPH: {
        Node_Polymorph *polymorph = (Node_Polymorph *) n;
        fprintf(
            f,
            "Polymorph (%s) '" SV_Fmt "'\n",
            polymorph->is_type ? "Type" : "Not Type",
            SV_Arg(polymorph->name->node.token.sv));
    } break;

    case NODE_INTERPOLATION: {
        Node_Interpolation *interp = (Node_Interpolation *) n;
        fprintf(f, "Interpolation {\n");
        nodes_debug_impl(f, interp->children, depth + 1, "Children");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_FN: {
        Node_Fn *fn = (Node_Fn *) n;
        if (fn->is_inline) {
            fprintf(f, "Inline ");
        }

        if (fn->is_method) {
            fprintf(f, "Method {\n");
        } else {
            fprintf(f, "Function {\n");
        }
        polymorphs_debug_impl(f, fn->polymorphs, depth + 1, "Polymorphs");
        nodes_debug_impl(f, fn->args, depth + 1, "Args");
        nodes_debug_impl(f, fn->returns, depth + 1, "Returns");
        node_debug_impl(f, fn->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_ENUM: {
        Node_Enum *enumm = (Node_Enum *) n;
        fprintf(f, "Enumeration {\n");
        nodes_debug_impl(f, enumm->values, depth + 1, "Values");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_TRAIT: {
        Node_Trait *trait = (Node_Trait *) n;
        fprintf(f, "Trait {\n");
        nodes_debug_impl(f, trait->methods, depth + 1, "Methods");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_UNION: {
        Node_Union *unionn = (Node_Union *) n;
        fprintf(f, "Union {\n");
        nodes_debug_impl(f, unionn->variants, depth + 1, "Variants");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_STRUCT: {
        Node_Struct *structt = (Node_Struct *) n;
        fprintf(f, "Structure {\n");
        polymorphs_debug_impl(f, structt->polymorphs, depth + 1, "Polymorphs");
        nodes_debug_impl(f, structt->fields, depth + 1, "Fields");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_COMPOUND: {
        Node_Compound *compound = (Node_Compound *) n;
        fprintf(f, "Compound {\n");
        node_debug_impl(f, compound->lhs, depth + 1, "Lhs");
        nodes_debug_impl(f, compound->children, depth + 1, "Children");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_CALL: {
        Node_Call *call = (Node_Call *) n;
        fprintf(f, "Call {\n");
        node_debug_impl(f, call->fn_source, depth + 1, "Fn");
        nodes_debug_impl(f, call->args, depth + 1, "Args");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_INDEX: {
        Node_Index *index = (Node_Index *) n;
        fprintf(f, "Index {\n");
        node_debug_impl(f, index->lhs, depth + 1, "Lhs");
        if (index->is_ranged) {
            node_debug_impl(f, index->a, depth + 1, "Index");
        } else {
            node_debug_impl(f, index->a, depth + 1, "From");
            node_debug_impl(f, index->b, depth + 1, "To");
        }
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_INDEXABLE: {
        Node_Indexable *indexable = (Node_Indexable *) n;
        fprintf(f, "Indexable {\n");
        node_debug_impl(f, indexable->count, depth + 1, "Count");
        node_debug_impl(f, indexable->element, depth + 1, "Element");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_DEFINE: {
        Node_Define *define = (Node_Define *) n;
        fprintf(f, "Define %s {\n", define->is_const ? "constant" : "variable");
        node_debug_impl(f, define->name, depth + 1, "Name");
        node_debug_impl(f, define->type, depth + 1, "Type");
        node_debug_impl(f, define->expr, depth + 1, "Expr");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_BLOCK: {
        Node_Block *block = (Node_Block *) n;
        fprintf(f, "Block {\n");
        for (Node *it = block->body.head; it; it = it->next) {
            node_debug_impl(f, it, depth + 1, NULL);
        }
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_IF: {
        Node_If *iff = (Node_If *) n;
        fprintf(f, "If {\n");
        node_debug_impl(f, iff->condition, depth + 1, "Condition");
        node_debug_impl(f, iff->consequence, depth + 1, "Consequence");
        node_debug_impl(f, iff->antecedence, depth + 1, "Antecedence");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_FOR: {
        Node_For *forr = (Node_For *) n;
        fprintf(f, "For {\n");
        node_debug_impl(f, forr->init, depth + 1, "Init");
        node_debug_impl(f, forr->condition, depth + 1, "Condition");
        node_debug_impl(f, forr->update, depth + 1, "Update");
        node_debug_impl(f, forr->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_CASE: {
        Node_Case *case_ = (Node_Case *) n;
        fprintf(f, "Case");
        if (!case_->preds.head) {
            fprintf(f, " (fallback)");
        }
        fprintf(f, " {\n");
        if (case_->preds.head) {
            nodes_debug_impl(f, case_->preds, depth + 1, "Predicates");
        }
        node_debug_impl(f, case_->body, depth + 1, "Body");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_SWITCH: {
        Node_Switch *sw = (Node_Switch *) n;
        fprintf(f, "Switch {\n");
        node_debug_impl(f, sw->expr, depth + 1, "Expr");
        nodes_debug_impl(f, sw->cases, depth + 1, "Cases");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_JUMP:
        if (n->token.kind == TOKEN_BREAK) {
            fprintf(f, "Break\n");
        } else if (n->token.kind == TOKEN_CONTINUE) {
            fprintf(f, "Continue\n");
        } else {
            unreachable();
        }
        break;

    case NODE_DEFER: {
        Node_Defer *defer = (Node_Defer *) n;
        fprintf(f, "Defer {\n");
        node_debug_impl(f, defer->stmt, depth + 1, "Stmt");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_RETURN: {
        Node_Return *returnn = (Node_Return *) n;
        fprintf(f, "Return {\n");
        node_debug_impl(f, returnn->value, depth + 1, "Value");
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    } break;

    case NODE_EXTERN: {
        Node_Extern *externn = (Node_Extern *) n;
        if (externn->nodes.head) {
            fprintf(f, "Extern {\n");
            for (Node *it = externn->nodes.head; it; it = it->next) {
                node_debug_impl(f, it, depth + 1, NULL);
            }
            fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
        } else {
            fprintf(f, "Extern {}\n");
        }
    } break;

    default:
        unreachable();
    }
}

void node_debug(FILE *f, const Node *n) {
    node_debug_impl(f, n, 0, NULL);
}

void nodes_debug(FILE *f, Nodes ns) {
    nodes_debug_impl(f, ns, 0, NULL);
}

Node_Fn *create_trait_method_wrapper(Arena *a, Node_Fn *fn, Type_Trait *trait, size_t method_index) {
    assert(fn->node.type.kind == TYPE_FN);
    Type_Fn *wrapper_spec = arena_clone(a, fn->node.type.spec.fn, sizeof(*fn->node.type.spec.fn));
    wrapper_spec->args = arena_clone(a, wrapper_spec->args, wrapper_spec->args_count * sizeof(*wrapper_spec->args));

    wrapper_spec->args[0].type.ref++;
    wrapper_spec->args[0].type.llvm = NULL;
    wrapper_spec->llvm = NULL;
    assert(wrapper_spec->variadics_kind != VARIADICS_UNTYPED);

    Node_Fn *wrapper_node = arena_clone(a, fn, sizeof(*fn));
    wrapper_node->node.token.pos = trait->methods[method_index].pos;
    wrapper_node->node.type.spec.fn = wrapper_spec;
    wrapper_node->llvm = NULL;
    wrapper_node->llvm_debug_scope = NULL;
    wrapper_node->wrapper = fn;
    wrapper_node->wrapper_for_trait = trait;
    wrapper_node->wrapper_signature = trait->methods[method_index].signature;
    return wrapper_node;
}
