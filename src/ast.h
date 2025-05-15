#ifndef AST_H
#define AST_H

#include "token.h"

typedef struct Node Node;

typedef struct LLVMOpaqueType  *LLVMTypeRef;
typedef struct LLVMOpaqueValue *LLVMValueRef;

typedef enum {
    TYPE_BOOL,
    TYPE_I64,
    TYPE_FN,
    COUNT_TYPES
} TypeKind;

typedef struct {
    TypeKind kind;

    LLVMTypeRef llvm;
} Type;

bool        typeEq(Type a, Type b);
const char *typeToString(Type type);

typedef enum {
    NODE_ATOM,
    NODE_UNARY,
    NODE_BINARY,

    NODE_BLOCK,

    NODE_FN,

    NODE_PRINT,
    COUNT_NODES
} NodeKind;

typedef struct {
    Node *head;
    Node *tail;
} Nodes;

typedef struct {
    Node *operand;
} NodeUnary;

typedef struct {
    Node *lhs;
    Node *rhs;
} NodeBinary;

typedef struct {
    Node *args;
    Node *ret;

    Node *body;
} NodeFn;

typedef struct {
    Node *operand;
} NodePrint;

struct Node {
    NodeKind kind;
    Type     type;
    Token    token;

    union {
        NodeUnary  unary;
        NodeBinary binary;

        Nodes block;

        NodeFn fn;

        NodePrint print;
    } as;

    Node *next;

    LLVMValueRef llvm;
};

#endif // AST_H
