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

static u64 ht_hasheq_method_spec(const void *va, const void *vb, size_t n) {
    unused(n);

    const Method_Spec a = *(const Method_Spec *) va;
    if (vb) {
        const Method_Spec b = *(const Method_Spec *) vb;
        return a.uid == b.uid && sv_eq(a.name, b.name);
    }

    const u64 receiver_hash = ht_hasheq_bytes(&a.uid, NULL, sizeof(a.uid));
    const u64 name_hash = ht_hasheq_bytes(a.name.data, NULL, a.name.count);
    return ht_hash_combine(receiver_hash, name_hash);
}

static void get_type_from_builtin_module(Compiler *c, const char *name, Const_Value *v, Type *t) {
    *v = get_const_definition_value(c, c->builtin_module, sv_from_cstr(name), NULL);
    assert(v->kind == CONST_VALUE_TYPE);
    *t = type_without_meta(v->as.type);
}

static Type_Trait *get_trait_from_builtin_module(Compiler *c, const char *name, Const_Value *v) {
    *v = get_const_definition_value(c, c->builtin_module, sv_from_cstr(name), NULL);
    assert(v->kind == CONST_VALUE_TYPE);
    assert(type_meta_kind_eq(v->as.type, TYPE_TRAIT));
    return v->as.type.spec.trait;
}

void check_nodes(Compiler *c) {
    assert(c->parser);
    assert(c->modules);
    assert(c->main_module);
    assert(c->builtin_module);

    c->methods_table.hasheq = ht_hasheq_method_spec;

    {
        Type_Fn *fn_spec = arena_alloc(&default_arena, sizeof(*fn_spec));

        const Type unit = {.kind = TYPE_VOID};
        fn_spec->return_type = arena_clone(&default_arena, &unit, sizeof(unit));

        c->main_fn_type = (Type) {
            .kind = TYPE_FN,
            .spec.fn = fn_spec,
        };
    }

    for (Module *m = c->modules->head; m; m = m->next) {
        define_orderless_nodes_of_module(c, m, NULL);
    }

    Const_Value value;

    // Type info
    {
        get_type_from_builtin_module(c, "Type_Info", &value, &c->type_info_type);
        get_type_from_builtin_module(c, "Type", &value, &c->type_info_pointer_type);

        assert(c->type_info_pointer_type.kind == TYPE_STRUCT);
        const Type_Struct *type_info_structure = c->type_info_pointer_type.spec.structt;

        assert(type_info_structure->fields_count == 4);
        const Type *type_info_variant = &type_info_structure->fields[3].type;

        assert(type_info_variant->kind == TYPE_UNION);
        c->type_info_variants_union = type_info_variant->spec.unionn;
        assert(c->type_info_variants_union->variants_count == 14);

        static_assert(COUNT_TYPES == 30, "");
        c->type_info_variants[TYPE_BOOL] = CONTRACT_TYPE_INFO_BOOLEAN;
        c->type_info_variants[TYPE_CHAR] = CONTRACT_TYPE_INFO_CHARACTER;

        c->type_info_variants[TYPE_S8] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_S16] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_S32] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_S64] = CONTRACT_TYPE_INFO_INTEGER;

        c->type_info_variants[TYPE_U8] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_U16] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_U32] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_U64] = CONTRACT_TYPE_INFO_INTEGER;

        c->type_info_variants[TYPE_F32] = CONTRACT_TYPE_INFO_FLOAT;
        c->type_info_variants[TYPE_F64] = CONTRACT_TYPE_INFO_FLOAT;

        c->type_info_variants[TYPE_INT] = CONTRACT_TYPE_INFO_INTEGER;
        c->type_info_variants[TYPE_FLOAT] = CONTRACT_TYPE_INFO_FLOAT;
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

    // Source code location
    get_type_from_builtin_module(c, "Source_Code_Location", &value, &c->source_code_location_type);

    // Interpolation
    get_type_from_builtin_module(c, "Interpolation", &value, &c->interpolation_type);

    // Panic
    {
        Type panic = {0};
        get_type_from_builtin_module(c, "Panic", &value, &panic);
        assert(panic.kind == TYPE_ENUM);
        assert(panic.spec.enumm.definition->values_count == 8);
    }

    // Comparisons
    get_type_from_builtin_module(c, "Ordering", &value, &c->ordering_type);

    // Builtin Traits
    {
        get_type_from_builtin_module(c, "Any", &value, &c->any_type);
        assert(type_kind_eq(c->any_type, TYPE_TRAIT));

        c->add_trait = get_trait_from_builtin_module(c, "Add", &value);
        c->sub_trait = get_trait_from_builtin_module(c, "Sub", &value);
        c->mul_trait = get_trait_from_builtin_module(c, "Mul", &value);
        c->div_trait = get_trait_from_builtin_module(c, "Div", &value);
        c->mod_trait = get_trait_from_builtin_module(c, "Mod", &value);
        c->neg_trait = get_trait_from_builtin_module(c, "Neg", &value);
        c->equal_trait = get_trait_from_builtin_module(c, "Equal", &value);
        c->ordered_trait = get_trait_from_builtin_module(c, "Ordered", &value);
    }

    // Impls
    {
        for (size_t i = 0; i < c->impls_list.count; i++) {
            Node_Impl *impl = c->impls_list.data[i];
            ll_foreach(it, &impl->polymorphs) {
                ll_foreach(prev, &impl->polymorphs) {
                    if (prev == it) {
                        break;
                    }

                    const Token *previous = &prev->name->node.token;
                    if (sv_eq(previous->sv, it->name->node.token.sv)) {
                        error_redefinition(c, (Node *) it->name, &previous->pos);
                    }
                }

                it->name->node.type = (Type) {
                    .kind = TYPE_POLYMORPH,
                    .spec.polymorph.definition = it,
                    .spec.polymorph.is_definition = true,
                    .is_meta = true,
                };

                it->node.type = it->name->node.type;
            }

            check_expr(c, impl->receiver, REF_NONE);
            type_assert_type(c, impl->receiver);

            Type *receiver = &impl->receiver->type;
            receiver->is_meta = false;

            if (type_kind_eq(*receiver, TYPE_TRAIT)) {
                error_node_begin(EK_ERROR, impl->receiver);
                fprintf(stderr, "Cannot define methods on %s", type_to_cstr(*receiver));
                if (receiver->spec.trait->definition->defined_as) {
                    fprintf(stderr, ". (It is a trait)");
                }
                error_finalize();
                exit(c, 1);
            }

            bool        is_named = false;
            Method_Spec spec = {0};
            if (!get_method_spec(c, impl->receiver, *receiver, (SV) {0}, &spec, impl->node.module, &is_named)) {
                error_node(EK_ERROR, impl->receiver, "Can only define methods on types defined in the same module");
                exit(c, 1);
            }

            // TODO: Is this even relevant anymore?
            if (!is_named) {
                error_node(EK_ERROR, impl->receiver, "The receiver of a method cannot have an anonymous type");
                exit(c, 1);
            }

            if (impl->trait) {
                check_expr(c, impl->trait, REF_NONE);
                type_assert_type(c, impl->trait);

                Type *trait = &impl->trait->type;
                trait->is_meta = false;

                if (!type_kind_eq(*trait, TYPE_TRAIT)) {
                    error_node(EK_ERROR, impl->trait, "Expected trait type, got %s", type_to_cstr(*trait));
                    exit(c, 1);
                }

                add_trait_implementation(c, *receiver, trait->spec.trait, impl);
                continue;
            }

            ll_foreach(it, &impl->methods) {
                assert(it->kind == NODE_DEFINE);
                Node_Define *define = (Node_Define *) it;

                assert(define->expr->kind == NODE_FN);
                Node_Fn *fn = (Node_Fn *) define->expr;

                assert(fn->defined_as && fn->args.head && fn->args.head->kind == NODE_DEFINE);
                spec.name = fn->defined_as->node.token.sv;
                if (type_kind_eq(*receiver, TYPE_ENUM)) {
                    ll_foreach(it, &receiver->spec.enumm.definition->values) {
                        if (sv_eq(it->token.sv, spec.name)) {
                            error_redefinition(c, (Node *) fn->defined_as, &it->token.pos);
                        }
                    }
                } else if (type_kind_eq(*receiver, TYPE_STRUCT)) {
                    for (size_t i = 0; i < receiver->spec.structt->fields_count; i++) {
                        const Type_Struct_Field it = receiver->spec.structt->fields[i];
                        if (sv_eq(it.name, spec.name)) {
                            error_redefinition(c, (Node *) fn->defined_as, &it.pos);
                        }
                    }
                }

                Node_Fn **previous = ht_get(&c->methods_table, spec);
                if (previous) {
                    error_redefinition(c, (Node *) fn->defined_as, &(*previous)->defined_as->node.token.pos);
                }
                ht_set(&c->methods_table, spec, fn);
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

// TODO: Apply the type restriction of special methods into traits
//       -> Or rather should we move from "special" methods into particular traits?
//       -> Perhaps after compile time polymorphism is implemented?
