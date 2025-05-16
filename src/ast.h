#ifndef AST_H
#define AST_H

#include "token.h"

typedef struct Node             Node;
typedef struct LLVMOpaqueType  *LLVMTypeRef;
typedef struct LLVMOpaqueValue *LLVMValueRef;

typedef enum {
    TYPE_NIL,
    TYPE_BOOL,
    TYPE_I64,
    TYPE_FN,
    COUNT_TYPES
} TypeKind;

typedef struct {
    TypeKind    kind;
    LLVMTypeRef llvm;
} Type;

bool        typeEq(Type a, Type b);
const char *typeToString(Type type);

typedef enum {
    NODE_ATOM,
    NODE_UNARY,
    NODE_BINARY,

    NODE_BLOCK,
    NODE_IF,
    NODE_FOR,

    NODE_FN,
    NODE_VAR,

    NODE_PRINT,
    COUNT_NODES
} NodeKind;

typedef struct {
    Node *definition;
} NodeAtom;

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
    Node *args;
    Node *ret;
    Node *body;

    LLVMValueRef llvm;
} NodeFn;

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
        NodeUnary  unary;
        NodeBinary binary;

        Nodes   block;
        NodeIf  iff;
        NodeFor forr;

        NodeFn  fn;
        NodeVar var;

        NodePrint print;
    } as;

    Node *next;
};

#endif // AST_H
