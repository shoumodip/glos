#include "../dwarf.h"
#include "compiler.h"

static_assert(COUNT_TYPES == 30, "");
LLVMTypeRef compile_type(Compiler *c, Type *type) {
    if (!type) {
        return NULL;
    }

    if (type->llvm) {
        return type->llvm;
    }

    assert(type->kind != TYPE_MODULE);
    assert(type->kind != TYPE_UNKNOWN_ENUM);
    assert(type->kind != TYPE_UNKNOWN_COMPOUND);

    // NOTE: Do not use `type*` functions because this function should not care whether a type is a metatype or not.
    if (type->ref) {
        type->llvm = LLVMPointerTypeInContext(c->llvm_context, 0);
        return type->llvm;
    }

    switch (type->kind) {
    case TYPE_VOID:
        type->llvm = LLVMVoidTypeInContext(c->llvm_context);
        break;

    case TYPE_BOOL:
        type->llvm = LLVMInt1TypeInContext(c->llvm_context);
        break;

    case TYPE_S8:
    case TYPE_U8:
    case TYPE_CHAR:
        type->llvm = LLVMInt8TypeInContext(c->llvm_context);
        break;

    case TYPE_S16:
    case TYPE_U16:
        type->llvm = LLVMInt16TypeInContext(c->llvm_context);
        break;

    case TYPE_S32:
    case TYPE_U32:
        type->llvm = LLVMInt32TypeInContext(c->llvm_context);
        break;

    case TYPE_S64:
    case TYPE_U64:
    case TYPE_INT:
        type->llvm = LLVMInt64TypeInContext(c->llvm_context);
        break;

    case TYPE_F32:
        type->llvm = LLVMFloatTypeInContext(c->llvm_context);
        break;

    case TYPE_F64:
    case TYPE_FLOAT:
        type->llvm = LLVMDoubleTypeInContext(c->llvm_context);
        break;

    case TYPE_RAWPTR:
    case TYPE_FN:
        type->llvm = LLVMPointerTypeInContext(c->llvm_context, 0);
        break;

    case TYPE_ENUM: {
        Node_Enum *definition = type->spec.enumm.definition;
        if (!definition->llvm) {
            Type stub = {.kind = type->spec.enumm.underlying};
            compile_type(c, &stub);
            definition->llvm = stub.llvm;
        }
        type->llvm = definition->llvm;
    } break;

    case TYPE_TRAIT:
        type->llvm = c->llvm_trait_type;
        break;

    case TYPE_UNION: {
        Type_Union *spec = type->spec.unionn;
        if (!spec->llvm) {
            for (size_t i = 0; i < spec->variants_count; i++) {
                Type_Union_Variant *it = &spec->variants[i];
                compile_type(c, &it->type);

                it->size = LLVMABISizeOfType(c->llvm_target_data, it->type.llvm);
                spec->variants_size_max = max(spec->variants_size_max, it->size);

                it->align = LLVMABIAlignmentOfType(c->llvm_target_data, it->type.llvm);
                spec->variants_align_max = max(spec->variants_align_max, it->align);
            }

            LLVMTypeRef fields[] = {
                LLVMInt64TypeInContext(c->llvm_context),
                LLVMArrayType(LLVMInt8TypeInContext(c->llvm_context), spec->variants_size_max),
            };

            spec->llvm = LLVMStructTypeInContext(c->llvm_context, fields, len(fields), false);
        }
        type->llvm = spec->llvm;
    } break;

    case TYPE_STRUCT: {
        assert(type->spec.structt);

        Type_Struct *spec = type->spec.structt;
        if (!spec->llvm) {
            LLVMTypeRef *fields = arena_alloc(&temp_arena, spec->fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];
                it->size = compile_sizeof(c, &it->type);
                fields[i] = it->type.llvm;
            }

            spec->llvm = LLVMStructTypeInContext(c->llvm_context, fields, spec->fields_count, false);
            arena_reset(&temp_arena, fields);

            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];
                it->offset = LLVMOffsetOfElement(c->llvm_target_data, spec->llvm, i);
            }
        }
        type->llvm = spec->llvm;
    } break;

    case TYPE_ARRAY:
        compile_type(c, type->spec.array.element);
        type->llvm = LLVMArrayType(type->spec.array.element->llvm, type->spec.array.count);
        break;

    case TYPE_DYNAMIC_ARRAY:
        type->llvm = c->llvm_dynamic_array_type;
        break;

    case TYPE_SLICE:
    case TYPE_STRING:
        type->llvm = c->llvm_slice_type;
        break;

    case TYPE_POLYMORPH:
        unreachable();

    case TYPE_GROUP: {
        Type_Group *spec = &type->spec.group;
        if (!spec->llvm) {
            LLVMTypeRef *fields = arena_alloc(&temp_arena, spec->count * sizeof(*fields));
            for (size_t i = 0; i < spec->count; i++) {
                Type *it = &spec->data[i];
                compile_type(c, it);
                fields[i] = it->llvm;
            }

            spec->llvm = LLVMStructTypeInContext(c->llvm_context, fields, spec->count, false);
            arena_reset(&temp_arena, fields);

            spec->offsets = arena_alloc(&default_arena, spec->count * sizeof(*spec->offsets));
            for (size_t i = 0; i < spec->count; i++) {
                spec->offsets[i] = LLVMOffsetOfElement(c->llvm_target_data, spec->llvm, i);
            }
        }
        type->llvm = spec->llvm;
    } break;

    default:
        unreachable();
        break;
    }

    return type->llvm;
}

LLVMTypeRef compile_fn_type(Compiler *c, Type type, ABI *abi) {
    assert(!type.ref && type.kind == TYPE_FN);
    Type_Fn *spec = type.spec.fn;

    const void *checkpoint = arena_alloc(&temp_arena, 0);

    abi_set_return_type(c, abi, spec->return_type);
    for (size_t i = 0; i < spec->args_count; i++) {
        abi_set_argument_type(c, abi, i, &spec->args[i].type);
    }

    if (spec->variadics_kind == VARIADICS_UNTYPED) {
        abi_set_variadic_at(abi, abi->actual_args_count);
    }

    abi->actual_args = arena_alloc(&temp_arena, abi->actual_args_count * sizeof(*abi->actual_args));
    spec->llvm = abi_finalize(c, abi);

    arena_reset(&temp_arena, checkpoint);
    return spec->llvm;
}

typedef struct {
    SV   name;
    Type type; // Constraint: 'sizeof(type) == 8'
} Builtin_Compound_Type_Field;

static LLVMMetadataRef
get_debug_for_builtin_compound_type(Compiler *c, SV name, Builtin_Compound_Type_Field *fields, size_t fields_count) //
{
    const void *checkpoint = arena_alloc(&temp_arena, 0);

    LLVMMetadataRef  empty_file_path_metadata = get_debug_file(c, "");
    LLVMMetadataRef *members = arena_alloc(&temp_arena, fields_count * sizeof(*members));

    size_t size_bits = 0;
    for (size_t i = 0; i < fields_count; i++) {
        Builtin_Compound_Type_Field it = fields[i];
        assert(compile_sizeof(c, &it.type) == 8);

        members[i] = LLVMDIBuilderCreateMemberType(
            c->llvm_debug_builder,
            c->llvm_debug_compile_unit,
            it.name.data,
            it.name.count,
            empty_file_path_metadata,
            0,
            64,
            64,
            size_bits,
            0,
            get_debug_for_type(c, &it.type));

        size_bits += 64;
    }

    LLVMMetadataRef real_metadata = LLVMDIBuilderCreateStructType(
        c->llvm_debug_builder,
        c->llvm_debug_compile_unit,
        "",
        0,
        empty_file_path_metadata,
        0,
        size_bits,
        64,
        0,
        NULL,
        members,
        fields_count,
        0,
        NULL,
        "",
        0);

    LLVMMetadataRef typedef_metadata = LLVMDIBuilderCreateTypedef(
        c->llvm_debug_builder,
        real_metadata,
        name.data,
        name.count,
        empty_file_path_metadata,
        0,
        c->llvm_debug_compile_unit,
        64);

    arena_reset(&temp_arena, checkpoint);
    return typedef_metadata;
}

static_assert(COUNT_TYPES == 30, "");
LLVMMetadataRef get_debug_for_type(Compiler *c, Type *type) {
    assert(!type->is_meta);
    if (type->ref) {
        Type inner = *type;
        inner.ref--;
        inner.llvm = NULL;
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, get_debug_for_type(c, &inner), sizeof(void *), sizeof(void *), 0, "", 0);
    }

    switch (type->kind) {
    case TYPE_VOID:
        return NULL;

    case TYPE_BOOL:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "bool", strlen("bool"), 8, DW_ATE_boolean, 0);

    case TYPE_CHAR:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "char", strlen("char"), 8, DW_ATE_unsigned_char, 0);

    case TYPE_S8:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "s8", strlen("s8"), 8, DW_ATE_signed, 0);

    case TYPE_S16:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "s16", strlen("s16"), 16, DW_ATE_signed, 0);

    case TYPE_S32:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "s32", strlen("s32"), 32, DW_ATE_signed, 0);

    case TYPE_S64:
    case TYPE_INT:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "s64", strlen("s64"), 64, DW_ATE_signed, 0);

    case TYPE_U8:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u8", strlen("u8"), 8, DW_ATE_unsigned, 0);

    case TYPE_U16:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u16", strlen("u16"), 16, DW_ATE_unsigned, 0);

    case TYPE_U32:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u32", strlen("u32"), 32, DW_ATE_unsigned, 0);

    case TYPE_U64:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "u64", strlen("u64"), 64, DW_ATE_unsigned, 0);

    case TYPE_F32:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "f32", strlen("f32"), 32, DW_ATE_float, 0);

    case TYPE_F64:
    case TYPE_FLOAT:
        return LLVMDIBuilderCreateBasicType(c->llvm_debug_builder, "f64", strlen("f64"), 64, DW_ATE_float, 0);

    case TYPE_RAWPTR:
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, NULL, sizeof(void *), sizeof(void *), 0, "rawptr", strlen("rawptr"));

    case TYPE_FN: {
        const Type_Fn *spec = type->spec.fn;

        LLVMMetadataRef *args = arena_alloc(&temp_arena, (spec->args_count + 1) * sizeof(*args));
        args[0] = get_debug_for_type(c, spec->return_type);
        for (size_t i = 0; i < spec->args_count; i++) {
            args[i + 1] = get_debug_for_type(c, &spec->args[i].type);
        }

        LLVMMetadataRef fn_debug_type =
            LLVMDIBuilderCreateSubroutineType(c->llvm_debug_builder, NULL, args, spec->args_count + 1, 0);

        arena_reset(&temp_arena, args);
        return LLVMDIBuilderCreatePointerType(
            c->llvm_debug_builder, fn_debug_type, sizeof(void *), sizeof(void *), 0, "", 0);
    }

    case TYPE_ENUM: {
        Node_Enum *definition = type->spec.enumm.definition;
        if (!definition->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            const size_t size = compile_sizeof(c, type);
            const SV     name = sv_from_cstr(type_to_cstr(*type));
            definition->debug = LLVMDIBuilderCreateBasicType(
                c->llvm_debug_builder,
                name.data,
                name.count,
                size * 8,
                type_is_signed(*type) ? DW_ATE_signed : DW_ATE_unsigned,
                0);

            arena_reset(&temp_arena, checkpoint);
        }
        return definition->debug;
    }

    case TYPE_TRAIT: {
        compile_type(c, type);

        Type_Trait *spec = type->spec.trait;
        if (!spec->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            SV name = {0};
            {
                Node_Atom *defined_as = spec->definition->defined_as;
                if (defined_as) {
                    const size_t start = default_sb.count;
                    sb_push_fn_name(&default_sb, spec->definition->defined_in, spec->definition->node.module);
                    sb_sprintf(&default_sb, "." SV_Fmt, SV_Arg(defined_as->node.token.sv));
                    name = sv_from_cstr(arena_sb_to_cstr(&temp_arena, &default_sb, start));
                }
            }

            Builtin_Compound_Type_Field fields[3] = {0};
            fields[0].name = sv_from_cstr("type");
            fields[0].type = c->type_info_pointer_type;

            fields[1].name = sv_from_cstr("data");
            fields[1].type = (Type) {.kind = TYPE_RAWPTR};

            fields[2].name = sv_from_cstr("impl");
            fields[2].type = (Type) {.kind = TYPE_RAWPTR};

            spec->debug = get_debug_for_builtin_compound_type(c, name, fields, len(fields));
            arena_reset(&temp_arena, checkpoint);
        }

        return spec->debug;
    }

    case TYPE_UNION: {
        compile_type(c, type);

        Type_Union *spec = type->spec.unionn;
        if (!spec->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            SV name = {0};
            {
                Node_Atom *defined_as = spec->definition->defined_as;
                if (defined_as) {
                    const size_t start = default_sb.count;
                    sb_push_fn_name(&default_sb, spec->definition->defined_in, spec->definition->node.module);
                    sb_sprintf(&default_sb, "." SV_Fmt, SV_Arg(defined_as->node.token.sv));
                    name = sv_from_cstr(arena_sb_to_cstr(&temp_arena, &default_sb, start));
                }
            }

            LLVMMetadataRef scope_metadata = c->llvm_debug_compile_unit;
            LLVMMetadataRef file_metadata = get_debug_file(c, spec->definition->node.token.pos.path);
            LLVMMetadataRef fields[2];

            spec->debug = LLVMDIBuilderCreateReplaceableCompositeType(
                c->llvm_debug_builder,
                DW_TAG_structure_type,
                name.data,
                name.count,
                scope_metadata,
                file_metadata,
                spec->definition->node.token.pos.row + 1,
                0,
                0,
                0,
                0,
                NULL,
                0);

            // Case
            {
                Type            case_type = {.kind = TYPE_S64};
                LLVMMetadataRef case_type_metadata = get_debug_for_type(c, &case_type);

                fields[0] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    spec->debug,
                    "case",
                    strlen("case"),
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    64,
                    64,
                    0,
                    0,
                    case_type_metadata);
            }

            // Payload
            {
                LLVMMetadataRef forward = LLVMDIBuilderCreateReplaceableCompositeType(
                    c->llvm_debug_builder,
                    DW_TAG_union_type,
                    name.data,
                    name.count,
                    spec->debug,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    0,
                    0,
                    0,
                    0,
                    NULL,
                    0);

                LLVMMetadataRef *variants = arena_alloc(&temp_arena, spec->variants_count * sizeof(*variants));
                for (size_t i = 0; i < spec->variants_count; i++) {
                    Type_Union_Variant *it = &spec->variants[i];

                    const SV     name = sv_from_cstr(type_to_cstr_raw(it->type));
                    const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->type.llvm) * 8;
                    const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->type.llvm) * 8;

                    variants[i] = LLVMDIBuilderCreateMemberType(
                        c->llvm_debug_builder,
                        forward,
                        name.data,
                        name.count,
                        file_metadata,
                        it->pos.row + 1,
                        size_bits,
                        align_bits,
                        0,
                        0,
                        get_debug_for_type(c, &it->type));
                }

                LLVMMetadataRef real = LLVMDIBuilderCreateUnionType(
                    c->llvm_debug_builder,
                    spec->debug,
                    "",
                    0,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    spec->variants_size_max * 8,
                    spec->variants_align_max * 8,
                    0,
                    variants,
                    spec->variants_count,
                    0,
                    "",
                    0);

                LLVMMetadataReplaceAllUsesWith(forward, real);
                fields[1] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    spec->debug,
                    "",
                    0,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    spec->variants_size_max * 8,
                    spec->variants_align_max * 8,
                    64,
                    0,
                    real);
            }

            LLVMMetadataRef real = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                scope_metadata,
                name.data,
                name.count,
                file_metadata,
                spec->definition->node.token.pos.row + 1,
                LLVMABISizeOfType(c->llvm_target_data, spec->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8,
                0,
                NULL,
                fields,
                len(fields),
                0,
                NULL,
                "",
                0);

            LLVMMetadataReplaceAllUsesWith(spec->debug, real);
            if (spec->definition->defined_as) {
                real = LLVMDIBuilderCreateTypedef(
                    c->llvm_debug_builder,
                    real,
                    name.data,
                    name.count,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    scope_metadata,
                    LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8);
            }

            spec->debug = real;
            arena_reset(&temp_arena, checkpoint);
        }
        return spec->debug;
    }

    case TYPE_STRUCT: {
        compile_type(c, type);

        Type_Struct *spec = type->spec.structt;
        if (!spec->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);

            SV name = {0};
            {
                Node_Atom *defined_as = spec->definition->defined_as;
                if (defined_as) {
                    const size_t start = default_sb.count;
                    sb_push_fn_name(&default_sb, spec->definition->defined_in, spec->definition->node.module);
                    sb_push(&default_sb, '.');
                    sb_push_type(&default_sb, type_without_meta(*type));
                    name = sv_from_cstr(arena_sb_to_cstr(&temp_arena, &default_sb, start));
                }
            }

            LLVMMetadataRef scope_metadata = c->llvm_debug_compile_unit;
            spec->debug = LLVMDIBuilderCreateReplaceableCompositeType(
                c->llvm_debug_builder,
                DW_TAG_structure_type,
                name.data,
                name.count,
                scope_metadata,
                get_debug_file(c, spec->definition->node.token.pos.path),
                spec->definition->node.token.pos.row + 1,
                0,
                0,
                0,
                0,
                NULL,
                0);

            LLVMMetadataRef *fields = arena_alloc(&temp_arena, spec->fields_count * sizeof(*fields));
            for (size_t i = 0; i < spec->fields_count; i++) {
                Type_Struct_Field *it = &spec->fields[i];

                const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->type.llvm) * 8;
                const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->type.llvm) * 8;
                const size_t offset_bits = it->offset * 8;

                fields[i] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    spec->debug,
                    it->name.data,
                    it->name.count,
                    get_debug_file(c, it->pos.path),
                    it->pos.row + 1,
                    size_bits,
                    align_bits,
                    offset_bits,
                    0,
                    get_debug_for_type(c, &it->type));
            }

            LLVMMetadataRef file_metadata = get_debug_file(c, spec->definition->node.token.pos.path);
            LLVMMetadataRef real = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                scope_metadata,
                name.data,
                name.count,
                file_metadata,
                spec->definition->node.token.pos.row + 1,
                LLVMABISizeOfType(c->llvm_target_data, spec->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8,
                0,
                NULL,
                fields,
                spec->fields_count,
                0,
                NULL,
                "",
                0);

            LLVMMetadataReplaceAllUsesWith(spec->debug, real);
            if (spec->definition->defined_as) {
                real = LLVMDIBuilderCreateTypedef(
                    c->llvm_debug_builder,
                    real,
                    name.data,
                    name.count,
                    file_metadata,
                    spec->definition->node.token.pos.row + 1,
                    scope_metadata,
                    LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8);
            }

            spec->debug = real;
            arena_reset(&temp_arena, checkpoint);
        }
        return spec->debug;
    }

    case TYPE_ARRAY: {
        compile_type(c, type);

        size_t subscripts_count = 0;
        for (Type *t = type; t->kind == TYPE_ARRAY; t = t->spec.array.element) {
            subscripts_count++;
        }
        LLVMMetadataRef *subscripts = arena_alloc(&temp_arena, subscripts_count * sizeof(*subscripts));

        Type  *innermost = NULL;
        size_t subscripts_iota = 0;
        for (innermost = type; innermost->kind == TYPE_ARRAY; innermost = innermost->spec.array.element) {
            subscripts[subscripts_iota++] =
                LLVMDIBuilderGetOrCreateSubrange(c->llvm_debug_builder, 0, innermost->spec.array.count);
        }

        LLVMMetadataRef metadata = LLVMDIBuilderCreateArrayType(
            c->llvm_debug_builder,
            compile_sizeof(c, type) * 8,
            0,
            get_debug_for_type(c, innermost),
            subscripts,
            subscripts_count);

        arena_reset(&temp_arena, subscripts);
        return metadata;
    } break;

    case TYPE_DYNAMIC_ARRAY: {
        const void *checkpoint = arena_alloc(&temp_arena, 0);

        SV name = sv_from_cstr(type_to_cstr_raw(*type));

        Builtin_Compound_Type_Field fields[3] = {0};
        fields[0].name = sv_from_cstr("data");
        fields[0].type = *type->spec.dynamic_array.element;
        fields[0].type.ref++;
        fields[0].type.llvm = NULL;

        fields[1].name = sv_from_cstr("count");
        fields[1].type = (Type) {.kind = TYPE_S64};

        fields[2].name = sv_from_cstr("capacity");
        fields[2].type = (Type) {.kind = TYPE_S64};

        LLVMMetadataRef metadata = get_debug_for_builtin_compound_type(c, name, fields, len(fields));
        arena_reset(&temp_arena, checkpoint);
        return metadata;
    }

    case TYPE_SLICE: {
        const void *checkpoint = arena_alloc(&temp_arena, 0);

        SV name = sv_from_cstr(type_to_cstr_raw(*type));

        Builtin_Compound_Type_Field fields[2] = {0};
        fields[0].name = sv_from_cstr("data");
        fields[0].type = *type->spec.slice.element;
        fields[0].type.ref++;
        fields[0].type.llvm = NULL;

        fields[1].name = sv_from_cstr("count");
        fields[1].type = (Type) {.kind = TYPE_S64};

        LLVMMetadataRef metadata = get_debug_for_builtin_compound_type(c, name, fields, len(fields));
        arena_reset(&temp_arena, checkpoint);
        return metadata;
    }

    case TYPE_STRING: {
        Builtin_Compound_Type_Field fields[2] = {0};
        fields[0].name = sv_from_cstr("data");
        fields[0].type = (Type) {.kind = TYPE_CHAR, .ref = 1};

        fields[1].name = sv_from_cstr("count");
        fields[1].type = (Type) {.kind = TYPE_S64};
        return get_debug_for_builtin_compound_type(c, sv_from_cstr("string"), fields, len(fields));
    }

    case TYPE_POLYMORPH:
        unreachable();

    case TYPE_GROUP: {
        compile_type(c, type);

        Type_Group *spec = &type->spec.group;
        if (!spec->debug) {
            const void *checkpoint = arena_alloc(&temp_arena, 0);
            const SV    name = sv_from_cstr(type_to_cstr_raw(*type));

            LLVMMetadataRef empty_file_path_metadata = get_debug_file(c, "");

            LLVMMetadataRef *fields = arena_alloc(&temp_arena, spec->count * sizeof(*fields));
            for (size_t i = 0; i < spec->count; i++) {
                Type *it = &spec->data[i];

                const size_t size_bits = LLVMABISizeOfType(c->llvm_target_data, it->llvm) * 8;
                const size_t align_bits = LLVMABIAlignmentOfType(c->llvm_target_data, it->llvm) * 8;
                const size_t offset_bits = LLVMOffsetOfElement(c->llvm_target_data, spec->llvm, i) * 8;
                const SV     name = sv_from_cstr(arena_sprintf(&temp_arena, "%zu", i));

                fields[i] = LLVMDIBuilderCreateMemberType(
                    c->llvm_debug_builder,
                    c->llvm_debug_compile_unit,
                    name.data,
                    name.count,
                    empty_file_path_metadata,
                    0,
                    size_bits,
                    align_bits,
                    offset_bits,
                    0,
                    get_debug_for_type(c, it));
            }

            spec->debug = LLVMDIBuilderCreateStructType(
                c->llvm_debug_builder,
                c->llvm_debug_compile_unit,
                "",
                0,
                empty_file_path_metadata,
                0,
                LLVMABISizeOfType(c->llvm_target_data, spec->llvm) * 8,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8,
                0,
                NULL,
                fields,
                spec->count,
                0,
                NULL,
                "",
                0);

            spec->debug = LLVMDIBuilderCreateTypedef(
                c->llvm_debug_builder,
                spec->debug,
                name.data,
                name.count,
                empty_file_path_metadata,
                0,
                c->llvm_debug_compile_unit,
                LLVMABIAlignmentOfType(c->llvm_target_data, spec->llvm) * 8);

            arena_reset(&temp_arena, checkpoint);
        }

        return spec->debug;
    }

    case TYPE_MODULE:
        unreachable();

    case TYPE_UNKNOWN_ENUM:
        unreachable();

    case TYPE_UNKNOWN_COMPOUND:
        unreachable();

    default:
        unreachable();
        break;
    }
}
