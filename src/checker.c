#include "checker/checker.h"
#include "contract.h"
#include "error.h"

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

    // Any
    Const_Value value;
    {
        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Any"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->any_type = type_without_meta(value.as.type);
    }

    // Interpolation
    {
        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Interpolation"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->interpolation_type = type_without_meta(value.as.type);
    }

    // Panic
    {
        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Panic"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);

        const Type panic = type_without_meta(value.as.type);
        assert(panic.kind == TYPE_ENUM);
        assert(panic.spec.enumm.definition->values_count == 8);
    }

    // Type info
    {
        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Type_Info"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->type_info_type = type_without_meta(value.as.type);

        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Type"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->type_info_pointer_type = type_without_meta(value.as.type);

        assert(c->type_info_pointer_type.kind == TYPE_STRUCT);
        const Type_Struct *type_info_structure = c->type_info_pointer_type.spec.structt;

        assert(type_info_structure->fields_count == 4);
        const Type *type_info_variant = &type_info_structure->fields[3].type;

        assert(type_info_variant->kind == TYPE_UNION);
        c->type_info_variants_union = type_info_variant->spec.unionn;
        assert(c->type_info_variants_union->variants_count == 13);

        static_assert(COUNT_TYPES == 27, "");
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
        c->type_info_variants[TYPE_DYNAMIC_ARRAY] = CONTRACT_TYPE_INFO_DYNAMIC_ARRAY;

        c->type_info_variants[TYPE_SLICE] = CONTRACT_TYPE_INFO_SLICE;
        c->type_info_variants[TYPE_STRING] = CONTRACT_TYPE_INFO_STRING;
    }

    // Comparisons
    {
        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Ordering"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->ordering_type = type_without_meta(value.as.type);

        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Equivalence"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->equivalence_type = type_without_meta(value.as.type);
    }

    // Source code location
    {
        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Source_Code_Location"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);

        c->source_code_location_type = type_without_meta(value.as.type);
    }

    // Define the methods
    {
        for (size_t i = 0; i < c->methods_list.count; i++) {
            Node_Fn *fn = c->methods_list.data[i];
            assert(fn->args.head && fn->args.head->kind == NODE_DEFINE); // Guaranteed by the parser

            // Define the polymorphic parameters
            {
                bool is_ref_valid = false;
                check_fn(c, fn, REF_NONE, &is_ref_valid, true);
            }

            Node_Define *define = (Node_Define *) fn->args.head;
            assert(define->name->kind == NODE_ATOM && define->type); // Guaranteed by the parser

            if (!fn->defined_as) {
                error_node(EK_ERROR, (Node *) fn, "Anonymous function cannot be a method");
                error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                exit(c, 1);
            }
            const SV name = fn->defined_as->node.token.sv;

            check_expr(c, define->type, REF_NONE);
            type_assert_type(c, define->type);
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
                exit(c, 1);
            } else if (get_method_spec(c, define->type, receiver_type, name, &spec, fn->module, &is_named)) {
                if (!is_named) {
                    error_node(EK_ERROR, define->type, "The receiver of a method cannot have an anonymous type");
                    error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                    exit(c, 1);
                }

                if (type_kind_eq(receiver_type, TYPE_ENUM)) {
                    ll_foreach(it, &receiver_type.spec.enumm.definition->values) {
                        if (sv_eq(it->token.sv, name)) {
                            error_redefinition(c, (Node *) fn->defined_as, &it->token.pos);
                        }
                    }
                } else if (type_kind_eq(receiver_type, TYPE_STRUCT)) {
                    for (size_t i = 0; i < receiver_type.spec.structt->fields_count; i++) {
                        const Type_Struct_Field it = receiver_type.spec.structt->fields[i];
                        if (sv_eq(it.name, name)) {
                            error_redefinition(c, (Node *) fn->defined_as, &it.pos);
                        }
                    }
                }

                Node_Fn **previous = ht_get(&c->methods_table, spec);
                if (previous) {
                    error_redefinition(c, (Node *) fn->defined_as, &(*previous)->defined_as->node.token.pos);
                }
                ht_set(&c->methods_table, spec, fn);
            } else {
                error_node(EK_ERROR, define->type, "Can only define methods on types defined in the same module");
                error_node(EK_NOTE, define->name, "This argument is taken to be the receiver");
                exit(c, 1);
            }
        }
    }

    for (Module *m = c->modules->head; m; m = m->next) {
        for (Node *it = m->nodes.head; it; it = it->next) {
            check_stmt(c, it);
        }
    }

    // Interpolation Marker
    {
        value = get_const_definition_value(c, c->builtin_module, sv_from_cstr("Interpolation_Marker"), NULL);
        assert(value.kind == CONST_VALUE_TYPE);
        c->interpolation_marker_type = type_without_meta(value.as.type);
    }

    get_main(c);
}

// TODO: Sometimes non-cyclic definitions are falsely flagged as cyclic
// TODO: Enum values is a bit broken (signedness)
//
// TODO: Apply the type restriction of special methods into traits
//       -> Or rather should we move from "special" methods into particular traits?
//       -> Perhaps after compile time polymorphism is implemented?
//
// TODO: Replace all uint64_t with u64
//
// TODO: The eval_const_expr() for polymorph monomorphization does not inform the user that it is monomorphizing in the
// diagnostics which might cause confusion.
