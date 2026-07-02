#include "node.h"
#include "basic.h"

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
    for (Module *m = ms->head; m; m = m->next) {
        ht_free(&m->globals);
    }
    ht_free(&ms->table);
}

static_assert(COUNT_TYPES == 25, "");
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
    case TYPE_UNIT:
        sb_push_cstr(sb, "void");
        break;

    case TYPE_BOOL:
        sb_push_cstr(sb, "bool");
        break;

    case TYPE_CHAR:
        sb_push_cstr(sb, "char");
        break;

    case TYPE_I8:
        sb_push_cstr(sb, "i8");
        break;

    case TYPE_I16:
        sb_push_cstr(sb, "i16");
        break;

    case TYPE_I32:
        sb_push_cstr(sb, "i32");
        break;

    case TYPE_I64:
        sb_push_cstr(sb, "i64");
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

    case TYPE_INT:
        sb_push_cstr(sb, "i64");
        break;

    case TYPE_RAWPTR:
        sb_push_cstr(sb, "rawptr");
        break;

    case TYPE_FN:
        sb_push_cstr(sb, "(");

        for (size_t i = 0; i < type.spec.fn->args_count; i++) {
            Type_Fn_Arg it = type.spec.fn->args[i];
            if (i) {
                sb_push_cstr(sb, ", ");
            }
            sb_sprintf(sb, SV_Fmt ": ", SV_Arg(it.name));

            Type it_type = it.type;
            if (type.spec.fn->variadics_kind == VARIADICS_TYPED && i == type.spec.fn->variadics_index) {
                sb_push_cstr(sb, "...");
                assert(it_type.kind == TYPE_SLICE);
                it_type = *it_type.spec.slice.element;
            }
            sb_push_type(sb, it_type);
        }

        if (type.spec.fn->variadics_kind == VARIADICS_UNTYPED) {
            sb_push_cstr(sb, ", ...");
        }
        sb_push_cstr(sb, ")");

        if (type.spec.fn->returns_count) {
            sb_push_cstr(sb, " -> ");
            sb_push_type(sb, *type.spec.fn->return_type);
        }
        break;

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
            for (size_t i = 0; i < spec->methods_count; i++) {
                const Type_Trait_Method it = spec->methods[i];
                if (i) {
                    sb_push_cstr(sb, "; ");
                }
                sb_sprintf(sb, SV_Fmt ": ", SV_Arg(it.name));
                sb_push_type(sb, it.type);
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
            for (size_t i = 0; i < spec->variants_count; i++) {
                Type_Union_Variant it = spec->variants[i];
                if (i) {
                    sb_push_cstr(sb, "; ");
                }
                sb_push_type(sb, it.type);
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
            sb_push_cstr(sb, "struct {");
            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field it = spec->fields[i];
                if (i) {
                    sb_push_cstr(sb, "; ");
                }

                sb_sprintf(sb, SV_Fmt ": ", SV_Arg(it.name));
                sb_push_type(sb, it.type);
            }
            sb_push_cstr(sb, "}");
        }
    } break;

    case TYPE_ARRAY:
        sb_sprintf(sb, "[%zu]", type.spec.array.count);
        sb_push_type(sb, *type.spec.array.element);
        break;

    case TYPE_SLICE:
        sb_push_cstr(sb, "[]");
        sb_push_type(sb, *type.spec.slice.element);
        break;

    case TYPE_STRING:
        sb_push_cstr(sb, "string");
        break;

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
    if (a->definition->defined_as || b->definition->defined_as) {
        return a->definition->defined_as == b->definition->defined_as;
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

static_assert(COUNT_TYPES == 25, "");
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

static_assert(COUNT_TYPES == 25, "");
bool type_kind_eq(Type type, Type_Kind kind) {
    if (type.is_meta) {
        return false;
    }

    return type.kind == kind;
}

bool type_is_numeric(Type type) {
    return type_is_integer(type) || type_kind_eq(type, TYPE_ENUM) || type_kind_eq(type, TYPE_UNKNOWN_ENUM) ||
           type_kind_eq(type, TYPE_UNKNOWN_COMPOUND);
}

static_assert(COUNT_TYPES == 25, "");
bool type_is_integer(Type type) {
    if (type.ref || type.is_meta) {
        return false;
    }

    switch (type.kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:

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

static_assert(COUNT_TYPES == 25, "");
bool type_is_signed(Type type) {
    if (type.ref || type.is_meta) {
        return false;
    }

    Type_Kind kind = type.kind;
    if (kind == TYPE_ENUM) {
        kind = type.spec.enumm.underlying;
    }

    switch (kind) {
    case TYPE_I8:
    case TYPE_I16:
    case TYPE_I32:
    case TYPE_I64:
    case TYPE_INT:

    case TYPE_UNKNOWN_ENUM:
    case TYPE_UNKNOWN_COMPOUND:
        return true;

    default:
        return false;
    }
}

static_assert(COUNT_TYPES == 25, "");
bool type_is_untyped(Type type) {
    if (type.is_meta || type.ref) {
        return false;
    }

    return type.kind == TYPE_INT;
}

static_assert(COUNT_TYPES == 25, "");
bool type_is_unknown(Type type) {
    if (type.is_meta || type.ref) {
        return false;
    }

    return type.kind == TYPE_UNKNOWN_ENUM || type.kind == TYPE_UNKNOWN_COMPOUND;
}

static_assert(COUNT_CONST_VALUES == 10, "");
bool const_value_eq(Const_Value a, Const_Value b) {
    if (a.kind != b.kind) {
        return false;
    }

    switch (a.kind) {
    case CONST_VALUE_INT:
        return int128_eq(a.as.integer, b.as.integer);

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

    case CONST_VALUE_STRING:
        return sv_eq(a.as.string, b.as.string);

    case CONST_VALUE_MODULE:
        unreachable();

    default:
        unreachable();
    }
}

static_assert(COUNT_NODES == 28, "");
Node *node_alloc(Module *module, Node_Kind kind, Token token) {
    static const size_t sizes[COUNT_NODES] = {
        [NODE_ATOM] = sizeof(Node_Atom), // This comment is here to prevent clang-format from messing this up
        [NODE_GROUP] = sizeof(Node_Group),
        [NODE_UNARY] = sizeof(Node_Unary),
        [NODE_BINARY] = sizeof(Node_Binary),
        [NODE_MEMBER] = sizeof(Node_Member),
        [NODE_ASSERT] = sizeof(Node_Assert),
        [NODE_IMPORT] = sizeof(Node_Import),
        [NODE_DISTINCT] = sizeof(Node_Distinct),
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
    Node *node = arena_alloc(&default_arena, sizes[kind]);
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

#define Indent_Fmt    "%*s"
#define Indent_Arg(d) (d) * 4, ""

static void node_debug_impl(FILE *f, Node *n, int depth, const char *label);

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
    for (Node *it = ns.head; it; it = it->next) {
        node_debug_impl(f, it, child_depth, NULL);
    }

    if (label) {
        fprintf(f, Indent_Fmt "}\n", Indent_Arg(depth));
    }
}

static_assert(COUNT_NODES == 28, "");
static void node_debug_impl(FILE *f, Node *n, int depth, const char *label) {
    if (!n) {
        return;
    }

    fprintf(f, Indent_Fmt, Indent_Arg(depth));
    if (label) {
        fprintf(f, "%s = ", label);
    }

    switch (n->kind) {
    case NODE_ATOM:
        fprintf(f, "Atom " SV_Fmt "\n", SV_Arg(n->token.sv));
        break;

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
        node_debug_impl(f, call->fn, depth + 1, "Fn");
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

void node_debug(FILE *f, Node *n) {
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
    return wrapper_node;
}

// TODO: Print space in anonymous types
