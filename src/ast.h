#ifndef AST_H
#define AST_H

#include "token.h"

typedef struct Node             Node;
typedef struct LLVMOpaqueType  *LLVMTypeRef;
typedef struct LLVMOpaqueValue *LLVMValueRef;

typedef struct {
    Node *head;
    Node *tail;
} Nodes;

typedef struct {
    Node **data;
    size_t length;
    size_t capacity;
} Scope;

Node *scopeFind(Scope s, Str name);

typedef enum {
    TYPE_UNIT,
    TYPE_BOOL,

    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,

    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,

    TYPE_INT, // Untyped literal, defaults to i64

    TYPE_FN,
    TYPE_RAWPTR,
    COUNT_TYPES
} TypeKind;

typedef struct {
    TypeKind kind;
    size_t   ref;
    Node    *spec;

    LLVMTypeRef llvm;
} Type;

const char *typeToString(Type type);

bool typeEq(Type a, Type b);
bool typeIsSigned(Type type);
bool typeIsPointer(Type type);
bool typeIsInteger(Type type);
bool typeKindIsInteger(TypeKind kind);

typedef enum {
    NODE_ATOM,
    NODE_CALL,
    NODE_CAST,
    NODE_UNARY,
    NODE_BINARY,
    NODE_SIZEOF,

    NODE_BLOCK,
    NODE_IF,
    NODE_FOR,

    NODE_FLOW,

    NODE_FN,
    NODE_ARG,
    NODE_VAR,

    NODE_PRINT,
    COUNT_NODES
} NodeKind;

typedef struct {
    Node *definition;
} NodeAtom;

typedef struct {
    Node  *fn;
    Nodes  args;
    size_t arity;
} NodeCall;

typedef struct {
    Node *from;
    Node *to;
} NodeCast;

typedef struct {
    Node *operand;
} NodeUnary;

typedef struct {
    Node *lhs;
    Node *rhs;
} NodeBinary;

typedef struct {
    Node *operand;
    bool  isExpr;
} NodeSizeof;

typedef struct {
    Node *condition;
    Node *consequence;
    Node *antecedence;
} NodeIf;

typedef struct {
    Node *init;
    Node *condition;
    Node *update;
    Node *body;
} NodeFor;

typedef struct {
    Node *operand;
} NodeFlow;

typedef struct {
    Node  *ret;
    Node  *body;
    Nodes  args;
    size_t arity;

    Scope locals;

    LLVMValueRef llvm;
} NodeFn;

Type nodeFnReturnType(const NodeFn *fn);

typedef struct {
    Node  *type;
    size_t index;
    bool   memory;

    LLVMValueRef llvm;
} NodeArg;

typedef struct {
    Node *expr;
    Node *type;
    bool  local;

    LLVMValueRef llvm;
} NodeVar;

typedef struct {
    Node *operand;
} NodePrint;

struct Node {
    NodeKind kind;
    Type     type;
    Token    token;

    union {
        NodeAtom   atom;
        NodeCall   call;
        NodeCast   cast;
        NodeUnary  unary;
        NodeBinary binary;
        NodeSizeof sizeoff;

        Nodes   block;
        NodeIf  iff;
        NodeFor forr;

        NodeFlow flow;

        NodeFn  fn;
        NodeArg arg;
        NodeVar var;

        NodePrint print;
    } as;

    Node *next;
};

#endif // AST_H
