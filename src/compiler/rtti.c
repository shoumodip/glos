#include "compiler.h"

static u64 ht_hasheq_type(const void *va, const void *vb, size_t n) {
    unused(n);
    if (vb) {
        return type_eq(*(const Type *) va, *(const Type *) vb);
    }

    // Technically this is correct, however this will decay to O(n) very often.
    // TODO: Implement a more specific hashing algorithm for types
    u64 hash = 14695981039346656037UL;
    for (size_t i = 0; i < sizeof(Type); i++) {
        hash ^= *(const uint8_t *) va;
        hash *= 1099511628211UL;
    }
    return hash;
}

static void compile_type_info_init(Compiler *c, Type_Info_Compiler *tic, Type *type) {
    compile_type(c, type);
    tic->type = type;

    if (!c->type_info_cache.hasheq) {
        c->type_info_cache.hasheq = ht_hasheq_type;
    }

    tic->variant_index = c->type_info_variants[type->ref ? TYPE_RAWPTR : type->kind];
    assert(tic->variant_index);

    // Emit unique RTTI
    tic->type_info = NULL;
    {
        tic->type_info = ht_get(&c->type_info_cache, *type);
        if (tic->type_info) {
            tic->done = *tic->type_info;
            return;
        }

        tic->type_info = ht_set(&c->type_info_cache, *type, LLVMAddGlobal(c->llvm_module, c->type_info_type.llvm, ""));
    }

    tic->ti_fields[tic->ti_fields_iota++] = LLVMConstInt(
        LLVMInt64TypeInContext(c->llvm_context), LLVMABISizeOfType(c->llvm_target_data, type->llvm), false);

    tic->ti_fields[tic->ti_fields_iota++] = LLVMConstInt(
        LLVMInt64TypeInContext(c->llvm_context), LLVMABIAlignmentOfType(c->llvm_target_data, type->llvm), false);

    const void *checkpoint = arena_alloc(&temp_arena, 0);

    SV name = {0};
    {
        static_assert(COUNT_TYPES == 30, "");
        Node_Atom *defined_as = NULL;
        if (type->distinct) {
            defined_as = type->distinct;
        } else if (type->kind == TYPE_ENUM) {
            defined_as = type->spec.enumm.definition->defined_as;
        } else if (type->kind == TYPE_TRAIT) {
            defined_as = type->spec.trait->definition->defined_as;
        } else if (type->kind == TYPE_UNION) {
            defined_as = type->spec.unionn->definition->defined_as;
        } else if (type->kind == TYPE_STRUCT) {
            Node_Struct *structt = (Node_Struct *) type->spec.structt->definition;
            defined_as = structt->defined_as;

            if (defined_as && defined_as->node.type.ref == type->ref && structt->monomorphs.count) {
                const size_t start = default_sb.count;
                sb_push_type(&default_sb, type_without_meta(structt->node.type));
                name = sv_from_cstr(arena_sb_to_cstr(&temp_arena, &default_sb, start));
                defined_as = NULL;
            }
        }

        if (defined_as && defined_as->node.type.ref == type->ref) {
            name = defined_as->node.token.sv;
        }
    }
    tic->ti_fields[tic->ti_fields_iota++] = compile_string_into_const_value(c, name);

    tic->tiv_fields[tic->tiv_fields_iota++] =
        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), tic->variant_index, false);

    arena_reset(&temp_arena, checkpoint);
}

static void compile_type_info_fn(Compiler *c, Type_Info_Compiler *tic, bool skip_first_arg) {
    const void *checkpoint = arena_alloc(&temp_arena, 0);

    LLVMValueRef fn_fields[3] = {0};
    size_t       fn_fields_iota = 0;

    assert(tic->type->kind == TYPE_FN);
    const Type_Fn *spec = tic->type->spec.fn;

    LLVMValueRef *args = arena_alloc(&temp_arena, spec->args_count * sizeof(*args));
    for (size_t i = skip_first_arg; i < spec->args_count; i++) {
        args[i] = compile_type_info(c, &spec->args[i].type);
    }
    fn_fields[fn_fields_iota++] = create_const_slice_from_memory(
        c, LLVMPointerTypeInContext(c->llvm_context, 0), args + skip_first_arg, spec->args_count - skip_first_arg);

    LLVMValueRef *returns = arena_alloc(&temp_arena, spec->returns_count * sizeof(*returns));
    for (size_t i = 0; i < spec->returns_count; i++) {
        returns[i] = compile_type_info(c, &spec->returns[i]);
    }
    fn_fields[fn_fields_iota++] =
        create_const_slice_from_memory(c, LLVMPointerTypeInContext(c->llvm_context, 0), returns, spec->returns_count);

    fn_fields[fn_fields_iota++] = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), spec->variadics_kind, true);

    tic->tiv_fields[tic->tiv_fields_iota++] =
        LLVMConstStructInContext(c->llvm_context, fn_fields, fn_fields_iota, false);
    arena_reset(&temp_arena, checkpoint);
}

static LLVMValueRef compile_type_info_finalize(Compiler *c, Type_Info_Compiler *tic);

static_assert(COUNT_TYPES == 30, "");
static void compile_type_info_variant(Compiler *c, Type_Info_Compiler *tic) {
    if (tic->done) {
        return;
    }

    if (tic->type->ref) {
        Type underlying = *tic->type;
        underlying.ref--;
        underlying.llvm = NULL;
        tic->tiv_fields[tic->tiv_fields_iota++] =
            create_const_struct_from_single_value_if_not_already(c, compile_type_info(c, &underlying));
    } else {
        switch (tic->type->kind) {
        case TYPE_BOOL:
        case TYPE_CHAR:
        case TYPE_RAWPTR:
        case TYPE_STRING:
            // Pass
            break;

        case TYPE_S8:
        case TYPE_S16:
        case TYPE_S32:
        case TYPE_S64:
        case TYPE_INT:

        case TYPE_U8:
        case TYPE_U16:
        case TYPE_U32:
        case TYPE_U64:
            tic->tiv_fields[tic->tiv_fields_iota++] = create_const_struct_from_single_value_if_not_already(
                c, LLVMConstInt(LLVMInt1TypeInContext(c->llvm_context), type_is_signed(*tic->type), true));
            break;

        case TYPE_F32:
        case TYPE_F64:
        case TYPE_FLOAT:
            // Pass
            break;

        case TYPE_FN:
            compile_type_info_fn(c, tic, false);
            break;

        case TYPE_ENUM: {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            LLVMValueRef enum_fields[3] = {0};
            size_t       enum_fields_iota = 0;

            const Type_Enum *spec = &tic->type->spec.enumm;

            LLVMValueRef *names = arena_alloc(&temp_arena, spec->definition->values_count * sizeof(*names));
            LLVMValueRef *values = arena_alloc(&temp_arena, spec->definition->values_count * sizeof(*values));

            {
                size_t iota = 0;
                ll_foreach(it, &spec->definition->values) {
                    names[iota] = compile_string_into_const_value(c, it->token.sv);
                    values[iota] = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), it->token.as.integer, true);
                    iota++;
                }
                assert(iota == spec->definition->values_count);
            }

            enum_fields[enum_fields_iota++] =
                create_const_slice_from_memory(c, c->llvm_slice_type, names, spec->definition->values_count);

            enum_fields[enum_fields_iota++] = create_const_slice_from_memory(
                c, LLVMInt64TypeInContext(c->llvm_context), values, spec->definition->values_count);

            Type underlying = {.kind = spec->underlying};
            enum_fields[enum_fields_iota++] = compile_type_info(c, &underlying);

            tic->tiv_fields[tic->tiv_fields_iota++] =
                LLVMConstStructInContext(c->llvm_context, enum_fields, enum_fields_iota, false);

            arena_reset(&temp_arena, checkpoint);
        } break;

        case TYPE_TRAIT: {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            LLVMValueRef struct_fields[2] = {0};
            size_t       struct_fields_iota = 0;

            const Type_Trait *spec = tic->type->spec.trait;

            LLVMValueRef *names = arena_alloc(&temp_arena, spec->methods_count * sizeof(*names));
            LLVMValueRef *types = arena_alloc(&temp_arena, spec->methods_count * sizeof(*types));

            for (size_t i = 0; i < spec->methods_count; i++) {
                Type_Trait_Method *it = &spec->methods[i];
                names[i] = compile_string_into_const_value(c, it->name);

                Type_Info_Compiler tic = {0};
                compile_type_info_init(c, &tic, &it->type);
                compile_type_info_fn(c, &tic, true);
                types[i] = compile_type_info_finalize(c, &tic);
            }

            struct_fields[struct_fields_iota++] =
                create_const_slice_from_memory(c, c->llvm_slice_type, names, spec->methods_count);

            struct_fields[struct_fields_iota++] = create_const_slice_from_memory(
                c, LLVMPointerTypeInContext(c->llvm_context, 0), types, spec->methods_count);

            tic->tiv_fields[tic->tiv_fields_iota++] =
                LLVMConstStructInContext(c->llvm_context, struct_fields, struct_fields_iota, false);
            arena_reset(&temp_arena, checkpoint);
        } break;

        case TYPE_UNION: {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            const Type_Union *spec = tic->type->spec.unionn;

            LLVMValueRef *variants = arena_alloc(&temp_arena, spec->variants_count * sizeof(*variants));
            for (size_t i = 0; i < spec->variants_count; i++) {
                variants[i] = compile_type_info(c, &spec->variants[i].type);
            }
            tic->tiv_fields[tic->tiv_fields_iota++] = create_const_slice_from_memory(
                c, LLVMPointerTypeInContext(c->llvm_context, 0), variants, spec->variants_count);

            arena_reset(&temp_arena, checkpoint);
        } break;

        case TYPE_STRUCT: {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            LLVMValueRef struct_fields[4] = {0};
            size_t       struct_fields_iota = 0;

            const Type_Struct *spec = tic->type->spec.structt;

            LLVMValueRef *names = arena_alloc(&temp_arena, spec->fields_count * sizeof(*names));
            LLVMValueRef *types = arena_alloc(&temp_arena, spec->fields_count * sizeof(*types));
            LLVMValueRef *offsets = arena_alloc(&temp_arena, spec->fields_count * sizeof(*offsets));

            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];
                names[i] = compile_string_into_const_value(c, it->name);
                types[i] = compile_type_info(c, &it->type);
                offsets[i] = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), it->offset, true);
            }

            struct_fields[struct_fields_iota++] =
                create_const_slice_from_memory(c, c->llvm_slice_type, names, spec->fields_count);

            struct_fields[struct_fields_iota++] = create_const_slice_from_memory(
                c, LLVMPointerTypeInContext(c->llvm_context, 0), types, spec->fields_count);

            struct_fields[struct_fields_iota++] =
                create_const_slice_from_memory(c, LLVMInt64TypeInContext(c->llvm_context), offsets, spec->fields_count);

            struct_fields[struct_fields_iota++] = LLVMConstInt(
                LLVMInt8TypeInContext(c->llvm_context), spec->original_definition != spec->definition, true);

            tic->tiv_fields[tic->tiv_fields_iota++] =
                LLVMConstStructInContext(c->llvm_context, struct_fields, struct_fields_iota, false);
            arena_reset(&temp_arena, checkpoint);
        } break;

        case TYPE_ARRAY: {
            LLVMValueRef array_fields[2] = {0};
            size_t       array_fields_iota = 0;

            array_fields[array_fields_iota++] = compile_type_info(c, tic->type->spec.array.element);
            array_fields[array_fields_iota++] =
                LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), tic->type->spec.array.count, true);

            tic->tiv_fields[tic->tiv_fields_iota++] =
                LLVMConstStructInContext(c->llvm_context, array_fields, array_fields_iota, false);
        } break;

        case TYPE_DYNAMIC_ARRAY:
            tic->tiv_fields[tic->tiv_fields_iota++] = create_const_struct_from_single_value_if_not_already(
                c, compile_type_info(c, tic->type->spec.dynamic_array.element));
            break;

        case TYPE_SLICE:
            tic->tiv_fields[tic->tiv_fields_iota++] = create_const_struct_from_single_value_if_not_already(
                c, compile_type_info(c, tic->type->spec.slice.element));
            break;

        default:
            unreachable();
            break;
        }
    }
}

static LLVMValueRef compile_type_info_finalize(Compiler *c, Type_Info_Compiler *tic) {
    if (tic->done) {
        return tic->done;
    }

    const size_t variant_size = c->type_info_variants_union->variants[tic->variant_index - 1].size;
    const size_t variant_padding = c->type_info_variants_union->variants_size_max - variant_size;
    if (variant_padding) {
        tic->tiv_fields[tic->tiv_fields_iota++] =
            LLVMConstNull(LLVMArrayType(LLVMInt8TypeInContext(c->llvm_context), variant_padding));
    }

    tic->ti_fields[tic->ti_fields_iota++] =
        LLVMConstStructInContext(c->llvm_context, tic->tiv_fields, tic->tiv_fields_iota, false);

    LLVMValueRef real = compile_const_value_into_memory(
        c, LLVMConstStructInContext(c->llvm_context, tic->ti_fields, tic->ti_fields_iota, false));

    LLVMReplaceAllUsesWith(*tic->type_info, real);
    LLVMDeleteGlobal(*tic->type_info);
    *tic->type_info = real;
    tic->done = real;
    return real;
}

LLVMValueRef compile_type_info(Compiler *c, Type *type) {
    Type_Info_Compiler tic = {0};
    compile_type_info_init(c, &tic, type);
    compile_type_info_variant(c, &tic);
    return compile_type_info_finalize(c, &tic);
}
