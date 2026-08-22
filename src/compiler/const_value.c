#include "compiler.h"

LLVMValueRef compile_const_value_into_memory(Compiler *c, LLVMValueRef value) {
    LLVMValueRef memory = LLVMAddGlobal(c->llvm_module, LLVMTypeOf(value), "");
    LLVMSetInitializer(memory, value);
    LLVMSetLinkage(memory, LLVMPrivateLinkage);
    return memory;
}

LLVMValueRef compile_string_into_const_value(Compiler *c, SV sv) {
    LLVMValueRef memory = LLVMConstStringInContext(c->llvm_context, sv.data, sv.count, false);
    LLVMValueRef fields[] = {
        compile_const_value_into_memory(c, memory),
        LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), sv.count, true),
    };
    return LLVMConstStructInContext(c->llvm_context, fields, len(fields), false);
}

LLVMValueRef create_const_slice_from_memory(Compiler *c, LLVMTypeRef element, LLVMValueRef *memory, size_t count) {
    LLVMValueRef fields[2] = {0};
    if (count) {
        fields[0] = compile_const_value_into_memory(c, LLVMConstArray(element, memory, count));
    } else {
        fields[0] = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
    }

    fields[1] = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), count, true);
    return LLVMConstStructInContext(c->llvm_context, fields, len(fields), false);
}

LLVMValueRef create_const_struct_from_single_value_if_not_already(Compiler *c, LLVMValueRef value) {
    if (LLVMGetValueKind(value) == LLVMConstantStructValueKind) {
        return value;
    }
    return LLVMConstStructInContext(c->llvm_context, &value, 1, false);
}

static_assert(COUNT_CONST_VALUES == 12, "");
LLVMValueRef compile_const_value(Compiler *c, Const_Value value, Type type) {
    switch (value.kind) {
    case CONST_VALUE_INT:
        if (type.ref) {
            assert(int128_is_zero(value.as.integer));
            return LLVMConstNull(type.llvm);
        }

        if (type.kind == TYPE_RAWPTR) {
            return LLVMConstIntToPtr(
                LLVMConstInt(
                    LLVMInt64TypeInContext(c->llvm_context), i64_from_int128(value.as.integer), type_is_signed(type)),
                type.llvm);
        }
        return LLVMConstInt(type.llvm, i64_from_int128(value.as.integer), type_is_signed(type));

    case CONST_VALUE_FN:
        return compile_fn(c, value.as.fn);

    case CONST_VALUE_VAR: {
        Node_Atom *var = value.as.var;
        if (!var->definition_spec->llvm) {
            compile_var_def(c, var);
        }

        assert(var->definition_spec->llvm);
        return var->definition_spec->llvm;
    } break;

    case CONST_VALUE_TYPE:
        return compile_type_info(c, &value.as.type);

    case CONST_VALUE_TRAIT: {
        Type_Trait_Impl *impl = value.as.trait.impl;
        if (impl) {
            compile_trait_impl(c, impl);
        }

        LLVMValueRef fields[3] = {0};
        size_t       fields_iota = 0;

        // Type
        if (value.as.trait.type) {
            fields[fields_iota++] = compile_type_info(c, value.as.trait.type);
        } else {
            fields[fields_iota++] = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
        }

        // Data
        if (value.as.trait.data) {
            fields[fields_iota++] =
                compile_const_value_into_memory(c, compile_const_value(c, *value.as.trait.data, *value.as.trait.type));
        } else {
            fields[fields_iota++] = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
        }

        // Impl
        if (impl) {
            fields[fields_iota++] = impl->llvm;
        } else {
            fields[fields_iota++] = LLVMConstNull(LLVMPointerTypeInContext(c->llvm_context, 0));
        }
        return LLVMConstStructInContext(c->llvm_context, fields, fields_iota, false);
    }

    case CONST_VALUE_UNION: {
        const Type_Union *spec = value.as.unionn.spec;

        LLVMValueRef fields[3] = {0};
        size_t       fields_iota = 0;
        fields[fields_iota++] = LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), value.as.unionn.index, true);

        size_t real_size = 0;
        if (value.as.unionn.index) {
            Type type = spec->variants[value.as.unionn.index - 1].type;
            real_size = compile_sizeof(c, &type);

            assert(value.as.unionn.real); // The index is not zero, that means a real value exists
            fields[fields_iota++] = compile_const_value(c, *value.as.unionn.real, type);
        }

        const size_t padding = LLVMABISizeOfType(c->llvm_target_data, spec->llvm) - 8 - real_size;
        if (padding) {
            fields[fields_iota++] = LLVMConstNull(LLVMArrayType(LLVMInt8TypeInContext(c->llvm_context), padding));
        }

        return LLVMConstStructInContext(c->llvm_context, fields, fields_iota, false);
    }

    case CONST_VALUE_STRUCT: {
        const Type_Struct *spec = value.as.structt.spec;

        LLVMValueRef *fields = arena_alloc(&temp_arena, spec->fields_count * sizeof(*fields));
        for (size_t i = 0; i < spec->fields_count; i++) {
            fields[i] = compile_const_value(c, value.as.structt.fields[i], spec->fields[i].type);
        }

        LLVMValueRef result = LLVMConstStructInContext(c->llvm_context, fields, spec->fields_count, false);
        arena_reset(&temp_arena, fields);
        return result;
    }

    case CONST_VALUE_ARRAY: {
        const Const_Value_Array array = value.as.array;
        compile_type(c, array.element_type);

        LLVMValueRef memory = NULL;
        {
            const size_t group_values_count_save = c->group_values.count;
            for (size_t i = 0; i < array.count; i++) {
                da_push(&c->group_values, compile_const_value(c, array.data[i], *array.element_type));
            }
            LLVMValueRef *elements = &c->group_values.data[group_values_count_save];

            memory = LLVMConstArray(array.element_type->llvm, elements, array.count);
            c->group_values.count = group_values_count_save;
        }

        if (array.is_slice) {
            LLVMValueRef fields[] = {
                compile_const_value_into_memory(c, memory),
                LLVMConstInt(LLVMInt64TypeInContext(c->llvm_context), array.count, true),
            };
            memory = LLVMConstStructInContext(c->llvm_context, fields, len(fields), false);
        }
        return memory;
    }

    case CONST_VALUE_DYNAMIC_ARRAY:
        return LLVMConstNull(c->llvm_dynamic_array_type);

    case CONST_VALUE_STRING:
        return compile_string_into_const_value(c, value.as.string);

    case CONST_VALUE_MODULE:
        unreachable();

    case CONST_VALUE_POLYMORPH:
        unreachable();

    default:
        unreachable();
    }
}
