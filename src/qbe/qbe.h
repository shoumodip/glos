// Copyright (C) 2025 Shoumodip Kar <shoumodipkar@gmail.com>

// Permission is hereby granted, free of charge, to any person obtaining a copy of
// this software and associated documentation files (the "Software"), to deal in
// the Software without restriction, including without limitation the rights to
// use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
// of the Software, and to permit persons to whom the Software is furnished to do
// so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef QBE_H
#define QBE_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *data;
    size_t      count;
} QbeSV;

#define QbeSVFmt    "%.*s"
#define QbeSVArg(s) (int) ((s).count), ((s).data)

typedef struct Qbe     Qbe;
typedef struct QbeNode QbeNode;

typedef struct QbeFn     QbeFn;
typedef struct QbeCall   QbeCall;
typedef struct QbeBlock  QbeBlock;
typedef struct QbeField  QbeField;
typedef struct QbeStruct QbeStruct;

typedef enum {
    QBE_TYPE_I0,
    QBE_TYPE_I8,
    QBE_TYPE_I16,
    QBE_TYPE_I32,
    QBE_TYPE_I64,
    QBE_TYPE_F32,
    QBE_TYPE_F64,
    QBE_TYPE_PTR,
    QBE_TYPE_STRUCT,
    QBE_COUNT_TYPES
} QbeTypeKind;

typedef struct {
    QbeTypeKind kind;
    QbeStruct  *spec;
} QbeType;

typedef enum {
    QBE_UNARY_NEG,
    QBE_UNARY_BNOT,
    QBE_UNARY_LNOT,
    QBE_COUNT_UNARYS
} QbeUnaryOp;

typedef enum {
    QBE_BINARY_ADD,
    QBE_BINARY_SUB,
    QBE_BINARY_MUL,
    QBE_BINARY_SDIV,
    QBE_BINARY_UDIV,
    QBE_BINARY_SMOD,
    QBE_BINARY_UMOD,

    QBE_BINARY_OR,
    QBE_BINARY_AND,
    QBE_BINARY_XOR,
    QBE_BINARY_SHL,
    QBE_BINARY_SSHR,
    QBE_BINARY_USHR,

    QBE_BINARY_SGT,
    QBE_BINARY_UGT,
    QBE_BINARY_SGE,
    QBE_BINARY_UGE,
    QBE_BINARY_SLT,
    QBE_BINARY_ULT,
    QBE_BINARY_SLE,
    QBE_BINARY_ULE,
    QBE_BINARY_EQ,
    QBE_BINARY_NE,

    QBE_COUNT_BINARYS
} QbeBinaryOp;

typedef struct {
    QbeNode  *value;
    QbeBlock *block;
} QbePhiBranch;

typedef enum {
    QBE_TARGET_DEFAULT,
    QBE_TARGET_X86_64_LINUX,
    QBE_TARGET_X86_64_MACOS,
    QBE_TARGET_ARM64_LINUX,
    QBE_TARGET_ARM64_MACOS,
    QBE_TARGET_RV64_LINUX
} QbeTarget;

// String View
QbeSV qbe_sv_from_cstr(const char *cstr);

// Types
QbeType qbe_type_basic(QbeTypeKind kind);
QbeType qbe_type_struct(QbeStruct *spec);

QbeType qbe_typeof(QbeNode *node);
size_t  qbe_sizeof(QbeType type);
size_t  qbe_offsetof(QbeField *field);

// Atoms
QbeNode *qbe_atom_int(Qbe *q, QbeTypeKind kind, size_t n);
QbeNode *qbe_atom_float(Qbe *q, QbeTypeKind kind, double n);
QbeNode *qbe_atom_symbol(Qbe *q, QbeSV name, QbeType type);

// Creators
QbeFn     *qbe_fn_new(Qbe *q, QbeSV name, QbeType return_type);
QbeNode   *qbe_var_new(Qbe *q, QbeSV name, QbeType type);
QbeNode   *qbe_str_new(Qbe *q, QbeSV sv);
QbeBlock  *qbe_block_new(Qbe *q);
QbeStruct *qbe_struct_new(Qbe *q, bool packed);

// Adders
void      qbe_call_add_arg(Qbe *q, QbeCall *call, QbeNode *arg);
QbeNode  *qbe_fn_add_arg(Qbe *q, QbeFn *fn, QbeType arg_type);
QbeNode  *qbe_fn_add_var(Qbe *q, QbeFn *fn, QbeType var_type);
QbeField *qbe_struct_add_field(Qbe *q, QbeStruct *st, QbeType field_type);

// Builder
QbeNode *qbe_build_phi(Qbe *q, QbeFn *fn, QbePhiBranch a, QbePhiBranch b);
QbeCall *qbe_build_call(Qbe *q, QbeFn *fn, QbeNode *value, QbeType return_type);
QbeNode *qbe_build_unary(Qbe *q, QbeFn *fn, QbeUnaryOp op, QbeType type, QbeNode *operand);
QbeNode *qbe_build_binary(Qbe *q, QbeFn *fn, QbeBinaryOp op, QbeType type, QbeNode *lhs, QbeNode *rhs);
QbeNode *qbe_build_load(Qbe *q, QbeFn *fn, QbeNode *ptr, QbeType type);

// Rules for 'is_signed':
//
// Int   -> Int   -- Signedness of the final type
// Float -> Int   -- Signedness of the final type
// Int   -> Float -- Signedness of the original type
// Float -> Float -- Doesn't matter
QbeNode *qbe_build_cast(Qbe *q, QbeFn *fn, QbeNode *value, QbeTypeKind type_kind, bool is_signed);

void qbe_build_store(Qbe *q, QbeFn *fn, QbeNode *ptr, QbeNode *value);
void qbe_build_block(Qbe *q, QbeFn *fn, QbeBlock *block);
void qbe_build_jump(Qbe *q, QbeFn *fn, QbeBlock *block);
void qbe_build_branch(Qbe *q, QbeFn *fn, QbeNode *cond, QbeBlock *then_block, QbeBlock *else_block);
void qbe_build_return(Qbe *q, QbeFn *fn, QbeNode *value);

// Debug
void qbe_build_debug_line(Qbe *q, QbeFn *fn, size_t line);
void qbe_fn_set_debug_file(Qbe *q, QbeFn *fn, QbeSV path);

// Primitives
Qbe *qbe_new(void);
void qbe_free(Qbe *q);
void qbe_compile(Qbe *q);
int  qbe_generate(Qbe *q, QbeTarget target, const char *output, const char **flags, size_t flags_count);

bool  qbe_has_been_compiled(Qbe *q);
QbeSV qbe_get_compiled_program(Qbe *q);

#endif // QBE_H
