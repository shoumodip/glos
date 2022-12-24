#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

// Misc
bool isident(char ch)
{
    return isalpha(ch) || ch == '_';
}

// Arena
#define ARENA_CAP 1024
typedef struct {
    char data[ARENA_CAP];
    size_t size;
} Arena;

void arena_push(Arena *arena, char *data, size_t size)
{
    assert(arena->size + size <= ARENA_CAP);
    memcpy(arena->data + arena->size, data, size);
    arena->size += size;
}

// Str
typedef struct {
    char *data;
    size_t size;
} Str;

void print_str(FILE *file, Str str)
{
    fwrite(str.data, str.size, 1, file);
}

bool str_eq(Str a, Str b)
{
    return a.size == b.size && memcmp(a.data, b.data, b.size) == 0;
}

Str str_from_cstr(char *data)
{
    Str str;
    str.data = data;
    str.size = strlen(data);
    return str;
}

// OS
bool read_file(Str *out, char *path)
{
    int fd = open(path, O_RDONLY, 420);
    if (fd < 0) {
        return false;
    }

    struct stat statbuf;
    if (fstat(fd, &statbuf) < 0) {
        close(fd);
        return false;
    }

    out->size = statbuf.st_size;
    out->data = mmap(NULL, out->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    return out->data != MAP_FAILED;
}

int execute_command(char **args, bool silent)
{
    pid_t pid = fork();
    if (pid == -1) {
        fprintf(stderr, "error: could not fork child\n");
        exit(1);
    }

    if (pid == 0) {
        if (silent) {
            int fd = open("/dev/null", O_WRONLY, 0);
            if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0) {
                fprintf(stderr, "error: could not silence child\n");
                exit(1);
            }
        }

        if (execvp(*args, args) == -1) {
            fprintf(stderr, "error: could not execute child\n");
            exit(1);
        }
    }

    int wstatus;
    if (wait(&wstatus) == -1) {
        fprintf(stderr, "error: could not wait for child\n");
        exit(1);
    }
    return WEXITSTATUS(wstatus);
}

// Pos
typedef struct {
    char *path;
    size_t row;
    size_t col;
} Pos;

void print_pos(FILE *file, Pos pos)
{
    fprintf(file, "%s:%zu:%zu: ", pos.path, pos.row, pos.col);
}

// Token
enum {
    TOKEN_EOF,
    TOKEN_INT,
    TOKEN_BOOL,
    TOKEN_IDENT,

    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_MUL,
    TOKEN_DIV,

    TOKEN_LNOT,

    TOKEN_PRINT,
    COUNT_TOKENS
};

static_assert(COUNT_TOKENS == 10);
char *cstr_from_token_type(int type)
{
    switch (type) {
    case TOKEN_EOF:
        return "end of file";

    case TOKEN_INT:
        return "integer";

    case TOKEN_BOOL:
        return "boolean";

    case TOKEN_IDENT:
        return "identifier";

    case TOKEN_ADD:
        return "'+'";

    case TOKEN_SUB:
        return "'-'";

    case TOKEN_MUL:
        return "'*'";

    case TOKEN_DIV:
        return "'/'";

    case TOKEN_LNOT:
        return "'!'";

    case TOKEN_PRINT:
        return "keyword 'print'";

    default: assert(0 && "unreachable");
    }
}

typedef struct {
    int type;
    size_t data;

    Pos pos;
    Str str;
} Token;

// Lexer
typedef struct {
    Pos pos;
    Str str;
    bool peeked;
    Token buffer;
    size_t prev_row;
} Lexer;

Lexer lexer;

void lexer_open(char *path)
{
    if (!read_file(&lexer.str, path)) {
        fprintf(stderr, "error: could not read file '%s'\n", path);
        exit(1);
    }

    lexer.pos.path = path;
    lexer.pos.row = 1;
    lexer.pos.col = 1;
}

void lexer_buffer(Token token)
{
    lexer.peeked = true;
    lexer.buffer = token;
}

void lexer_advance(void)
{
    if (*lexer.str.data == '\n') {
        lexer.pos.row += 1;
        lexer.pos.col = 1;
    } else {
        lexer.pos.col += 1;
    }

    lexer.str.data += 1;
    lexer.str.size -= 1;
}

char lexer_consume(void)
{
    lexer_advance();
    return lexer.str.data[-1];
}

static_assert(COUNT_TOKENS == 10);
Token lexer_next(void)
{
    if (lexer.peeked) {
        lexer.peeked = false;
        lexer.prev_row = lexer.buffer.pos.row;
        return lexer.buffer;
    }

    while (lexer.str.size > 0) {
        if (isspace(*lexer.str.data)) {
            lexer_advance();
        } else if (*lexer.str.data == '#') {
            while (lexer.str.size > 0 && *lexer.str.data != '\n') {
                lexer_advance();
            }
        } else {
            break;
        }
    }

    Token token;
    token.pos = lexer.pos;
    token.str = lexer.str;

    if (lexer.str.size == 0) {
        token.type = TOKEN_EOF;
    } else if (isdigit(*lexer.str.data)) {
        token.type = TOKEN_INT;
        token.data = 0;

        while (lexer.str.size > 0 && isdigit(*lexer.str.data)) {
            token.data = token.data * 10 + lexer_consume() - '0';
        }

        token.str.size -= lexer.str.size;
    } else if (isident(*lexer.str.data)) {
        while (lexer.str.size > 0 && isident(*lexer.str.data)) {
            lexer_advance();
        }

        token.str.size -= lexer.str.size;

        if (str_eq(token.str, str_from_cstr("true"))) {
            token.type = TOKEN_BOOL;
            token.data = 1;
        } else if (str_eq(token.str, str_from_cstr("false"))) {
            token.type = TOKEN_BOOL;
            token.data = 0;
        } else if (str_eq(token.str, str_from_cstr("print"))) {
            token.type = TOKEN_PRINT;
        } else {
            token.type = TOKEN_IDENT;
        }
    } else {
        switch (lexer_consume()) {
        case '+':
            token.type = TOKEN_ADD;
            break;

        case '-':
            token.type = TOKEN_SUB;
            break;

        case '*':
            token.type = TOKEN_MUL;
            break;

        case '/':
            token.type = TOKEN_DIV;
            break;

        case '!':
            token.type = TOKEN_LNOT;
            break;

        default:
            print_pos(stderr, token.pos);
            fprintf(stderr, "error: invalid character '%c'\n", *token.str.data);
            exit(1);
        }

        token.str.size -= lexer.str.size;
    }

    lexer.prev_row = token.pos.row;
    return token;
}

Token lexer_peek(void)
{
    int prev_row = lexer.prev_row;
    if (!lexer.peeked) {
        lexer_buffer(lexer_next());
        lexer.prev_row = prev_row;
    }
    return lexer.buffer;
}

bool lexer_read(int type)
{
    lexer_peek();
    lexer.peeked = lexer.buffer.type != type;
    return !lexer.peeked;
}

bool lexer_peek_row(Token *token)
{
	*token = lexer_peek();
	return token->pos.row == lexer.prev_row;
}

// AST
enum {
    NODE_ATOM,
    NODE_UNARY,
    NODE_BINARY,

    NODE_PRINT,
    COUNT_NODES
};

#define NODE_UNARY_VALUE 0

#define NODE_BINARY_LHS 0
#define NODE_BINARY_RHS 1

#define NODE_PRINT_VALUE 0

typedef struct {
    int type;
    Token token;
    size_t kind;
    size_t nodes[2];
} Node;

#define NODES_CAP 1024
Node nodes[NODES_CAP];
size_t nodes_count;

size_t node_new(int type, Token token)
{
    assert(nodes_count < NODES_CAP);
    nodes[nodes_count].type = type;
    nodes[nodes_count].token = token;
    nodes_count += 1;
    return nodes_count - 1;
}

// Parser
enum {
    POWER_NIL,
    POWER_ADD,
    POWER_MUL,
    POWER_PRE
};

static_assert(COUNT_TOKENS == 10);
int power_from_token_type(int type)
{
    switch (type) {
    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
        return POWER_MUL;

    default: return POWER_NIL;
    }
}

void error_unexpected(Token token)
{
    print_pos(stderr, token.pos);
    fprintf(stderr, "error: unexpected %s\n", cstr_from_token_type(token.type));
    exit(1);
}

static_assert(COUNT_TOKENS == 10);
size_t parse_expr(int mbp)
{
    size_t node;
    Token token = lexer_next();

    switch (token.type) {
    case TOKEN_INT:
    case TOKEN_BOOL:
        node = node_new(NODE_ATOM, token);
        break;

    case TOKEN_SUB:
    case TOKEN_LNOT:
        node = node_new(NODE_UNARY, token);
        nodes[node].nodes[NODE_UNARY_VALUE] = parse_expr(POWER_PRE);
        break;

    default: error_unexpected(token);
    }

    while (true) {
        if (!lexer_peek_row(&token)) {
            break;
        }

        int lbp = power_from_token_type(token.type);
        if (lbp <= mbp) {
            break;
        }
        lexer.peeked = false;

        size_t binary = node_new(NODE_BINARY, token);
        nodes[binary].nodes[NODE_BINARY_LHS] = node;
        nodes[binary].nodes[NODE_BINARY_RHS] = parse_expr(lbp);
        node = binary;
    }

    return node;
}

static_assert(COUNT_TOKENS == 10);
size_t parse_stmt(void)
{
    size_t node;
    Token token = lexer_next();

    switch (token.type) {
    case TOKEN_PRINT:
        node = node_new(NODE_PRINT, token);
        nodes[node].nodes[NODE_PRINT_VALUE] = parse_expr(POWER_NIL);
        break;

    default:
        lexer_buffer(token);
        node = parse_expr(POWER_NIL);
    }

    return node;
}

// Op
enum {
    OP_PUSH,
    OP_DROP,

    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_NEG,

    OP_LNOT,

    OP_PRINT,
    COUNT_OPS
};

typedef struct {
    int type;
    size_t data;
} Op;

static_assert(COUNT_OPS == 9);
void print_op(FILE *file, Op op)
{
    switch (op.type) {
    case OP_PUSH:
        fprintf(file, "push %zu\n", op.data);
        break;

    case OP_DROP:
        fprintf(file, "drop %zu\n", op.data);
        break;

    case OP_ADD:
        fprintf(file, "add\n");
        break;

    case OP_SUB:
        fprintf(file, "sub\n");
        break;

    case OP_MUL:
        fprintf(file, "mul\n");
        break;

    case OP_DIV:
        fprintf(file, "div\n");
        break;

    case OP_NEG:
        fprintf(file, "neg\n");
        break;

    case OP_LNOT:
        fprintf(file, "lnot\n");
        break;

    case OP_PRINT:
        fprintf(file, "print\n");
        break;

    default: assert(0 && "unreachable");
    }
}

#define OPS_CAP 1024
Op ops[OPS_CAP];
size_t ops_count;

void ops_push(int type, size_t data)
{
    assert(ops_count < OPS_CAP);
    ops[ops_count].type = type;
    ops[ops_count].data = data;
    ops_count += 1;
}

void print_ops(FILE *file)
{
    for (size_t i = 0; i < ops_count; i += 1) {
        print_op(file, ops[i]);
    }
}

// Type
enum {
    TYPE_INT,
    TYPE_BOOL,
    COUNT_TYPES
};

bool type_eq(size_t a, size_t b)
{
    return a == b;
}

static_assert(COUNT_TYPES == 2);
void print_type(FILE *file, size_t type)
{
    switch (type) {
    case TYPE_INT:
        fprintf(file, "int");
        break;

    case TYPE_BOOL:
        fprintf(file, "bool");
        break;

    default: assert(0 && "unreachable");
    }
}

// Checker
size_t type_assert(size_t node, size_t expected)
{
    size_t actual = nodes[node].kind;
    if (!type_eq(actual, expected)) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: expected type '");
        print_type(stderr, expected);
        fprintf(stderr, "', got '");
        print_type(stderr, actual);
        fprintf(stderr, "'\n");
        exit(1);
    }
    return actual;
}

size_t type_assert_arith(size_t node)
{
    size_t actual = nodes[node].kind;
    if (!type_eq(actual, TYPE_INT)) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: expected arithmetic type, got '");
        print_type(stderr, actual);
        fprintf(stderr, "'\n");
        exit(1);
    }
    return actual;
}

static_assert(COUNT_NODES == 4);
static_assert(COUNT_TOKENS == 10);
void check_expr(size_t node)
{
    switch (nodes[node].type) {
    case NODE_ATOM:
        switch (nodes[node].token.type) {
        case TOKEN_INT:
            nodes[node].kind = TYPE_INT;
            break;

        case TOKEN_BOOL:
            nodes[node].kind = TYPE_BOOL;
            break;

        default: assert(0 && "unreachable");
        }
        break;

    case NODE_UNARY: {
        size_t value = nodes[node].nodes[NODE_UNARY_VALUE];

        switch (nodes[node].token.type) {
        case TOKEN_SUB:
            check_expr(value);
            nodes[node].kind = type_assert_arith(value);
            break;

        case TOKEN_LNOT:
            check_expr(value);
            nodes[node].kind = type_assert(value, TYPE_BOOL);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    case NODE_BINARY: {
        size_t lhs = nodes[node].nodes[NODE_BINARY_LHS];
        size_t rhs = nodes[node].nodes[NODE_BINARY_RHS];

        switch (nodes[node].token.type) {
        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
            check_expr(lhs);
            check_expr(rhs);
            nodes[node].kind = type_assert(rhs, type_assert_arith(lhs));
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    default: assert(0 && "unreachable");
    }
}

static_assert(COUNT_NODES == 4);
static_assert(COUNT_TOKENS == 10);
void check_stmt(size_t node)
{
    switch (nodes[node].type) {
    case NODE_PRINT:
        check_expr(nodes[node].nodes[NODE_PRINT_VALUE]);
        break;

    default: check_expr(node);
    }
}

// Compiler
size_t align(size_t n)
{
    return (n + 7) & -8;
}

static_assert(COUNT_TYPES == 2);
size_t type_size(size_t type)
{
    switch (type) {
    case TYPE_INT:
        return 8;

    case TYPE_BOOL:
        return 1;

    default: assert(0 && "unreachable");
    }
}

static_assert(COUNT_NODES == 4);
static_assert(COUNT_TOKENS == 10);
void compile_expr(size_t node)
{
    switch (nodes[node].type) {
    case NODE_ATOM:
        switch (nodes[node].token.type) {
        case TOKEN_INT:
        case TOKEN_BOOL:
            ops_push(OP_PUSH, nodes[node].token.data);
            break;

        default: assert(0 && "unreachable");
        }
        break;

    case NODE_UNARY: {
        size_t value = nodes[node].nodes[NODE_UNARY_VALUE];

        switch (nodes[node].token.type) {
        case TOKEN_SUB:
            compile_expr(value);
            ops_push(OP_NEG, 0);
            break;

        case TOKEN_LNOT:
            compile_expr(value);
            ops_push(OP_LNOT, 0);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    case NODE_BINARY: {
        size_t lhs = nodes[node].nodes[NODE_BINARY_LHS];
        size_t rhs = nodes[node].nodes[NODE_BINARY_RHS];

        switch (nodes[node].token.type) {
        case TOKEN_ADD:
            compile_expr(lhs);
            compile_expr(rhs);
            ops_push(OP_ADD, 0);
            break;

        case TOKEN_SUB:
            compile_expr(lhs);
            compile_expr(rhs);
            ops_push(OP_SUB, 0);
            break;

        case TOKEN_MUL:
            compile_expr(lhs);
            compile_expr(rhs);
            ops_push(OP_MUL, 0);
            break;

        case TOKEN_DIV:
            compile_expr(lhs);
            compile_expr(rhs);
            ops_push(OP_DIV, 0);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    default:
        assert(0 && "unreachable");
    }
}

static_assert(COUNT_NODES == 4);
static_assert(COUNT_TOKENS == 10);
void compile_stmt(size_t node)
{
    switch (nodes[node].type) {
    case NODE_PRINT:
        compile_expr(nodes[node].nodes[NODE_PRINT_VALUE]);
        ops_push(OP_PRINT, 0);
        break;

    default:
        compile_expr(node);
        ops_push(OP_DROP, align(type_size(nodes[node].kind)));
    }
}

// Generator
static_assert(COUNT_OPS == 9);
void generate(char *path)
{
    FILE *file = fopen(path, "w");
    if (!file) {
        fprintf(stderr, "error: could not write file '%s'\n", path);
        exit(1);
    }

    fprintf(file, "format elf64 executable\n");
    fprintf(file, "segment readable executable\n");

    for (size_t i = 0; i < ops_count; ++i) {
        Op op = ops[i];

        fprintf(file, "I%zu: ; ", i);
        print_op(file, op);

        switch (op.type) {
        case OP_PUSH:
            fprintf(file, "mov rax, %zu\n", op.data);
            fprintf(file, "push rax\n");
            break;

        case OP_DROP:
            fprintf(file, "add rsp, %zu\n", op.data);
            break;

        case OP_ADD:
            fprintf(file, "pop rax\n");
            fprintf(file, "add [rsp], rax\n");
            break;

        case OP_SUB:
            fprintf(file, "pop rax\n");
            fprintf(file, "sub [rsp], rax\n");
            break;

        case OP_MUL:
            fprintf(file, "pop rbx\n");
            fprintf(file, "pop rax\n");
            fprintf(file, "imul rbx\n");
            fprintf(file, "push rax\n");
            break;

        case OP_DIV:
            fprintf(file, "pop rbx\n");
            fprintf(file, "pop rax\n");
            fprintf(file, "cqo\n");
            fprintf(file, "idiv rbx\n");
            fprintf(file, "push rax\n");
            break;

        case OP_NEG:
            fprintf(file, "neg qword [rsp]\n");
            break;

        case OP_LNOT:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "test rax, rax\n");
            fprintf(file, "sete bl\n");
            fprintf(file, "push rbx\n");
            break;

        case OP_PRINT:
            fprintf(file, "pop rdi\n");
            fprintf(file, "call PRINT\n");
            break;

        default: assert(0 && "unreachable");
        }
    }

    fprintf(file, "mov rax, 60\n");
    fprintf(file, "xor rdi, rdi\n");
    fprintf(file, "syscall\n");

    fprintf(file, "PRINT:\n");
    fprintf(file, "mov r9, -3689348814741910323\n");
    fprintf(file, "sub rsp, 40\n");
    fprintf(file, "mov BYTE [rsp+31], 10\n");
    fprintf(file, "lea rcx, [rsp+30]\n");
    fprintf(file, ".L2:\n");
    fprintf(file, "mov rax, rdi\n");
    fprintf(file, "lea r8, [rsp+32]\n");
    fprintf(file, "mul r9\n");
    fprintf(file, "mov rax, rdi\n");
    fprintf(file, "sub r8, rcx\n");
    fprintf(file, "shr rdx, 3\n");
    fprintf(file, "lea rsi, [rdx+rdx*4]\n");
    fprintf(file, "add rsi, rsi\n");
    fprintf(file, "sub rax, rsi\n");
    fprintf(file, "add eax, 48\n");
    fprintf(file, "mov BYTE [rcx], al\n");
    fprintf(file, "mov rax, rdi\n");
    fprintf(file, "mov rdi, rdx\n");
    fprintf(file, "mov rdx, rcx\n");
    fprintf(file, "sub rcx, 1\n");
    fprintf(file, "cmp rax, 9\n");
    fprintf(file, "ja .L2\n");
    fprintf(file, "lea rax, [rsp+32]\n");
    fprintf(file, "mov edi, 1\n");
    fprintf(file, "sub rdx, rax\n");
    fprintf(file, "xor eax, eax\n");
    fprintf(file, "lea rsi, [rsp+32+rdx]\n");
    fprintf(file, "mov rdx, r8\n");
    fprintf(file, "mov rax, 1\n");
    fprintf(file, "syscall\n");
    fprintf(file, "add rsp, 40\n");
    fprintf(file, "ret\n");
    fclose(file);

    char *args[] = {"fasm", path, NULL};
    if (execute_command(args, true) == 0) {
        unlink(path);
    } else {
        fprintf(stderr, "error: could not generate executable\n");
        exit(1);
    }
}

// Main
Arena path_arena;

void usage(FILE *file)
{
    fprintf(file, "usage:\n");
    fprintf(file, "  glos [FLAG] FILE\n\n");
    fprintf(file, "flags:\n");
    fprintf(file, "  -h    Print this help and exit\n");
    fprintf(file, "  -r    Run the program after compilation\n");
}

int main(int argc, char **argv)
{
    (void) argc;

    bool run = false;
    char *path = argv[1];
    if (path != NULL) {
        if (strcmp(path, "-h") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(path, "-r") == 0) {
            run = true;
            path = argv[2];
        }
    }

    if (path == NULL) {
        usage(stderr);
        fprintf(stderr, "\nerror: file path not provided\n");
        exit(1);
    }

    lexer_open(path);
    while (!lexer_read(TOKEN_EOF)) {
        size_t node = parse_stmt();
        check_stmt(node);
        compile_stmt(node);
    }

    path_arena.size = 0;
    arena_push(&path_arena, "./", 2);
    arena_push(&path_arena, path, strlen(path));

    path_arena.size -= 5;
    path_arena.data[path_arena.size] = '\0';
    unlink(path_arena.data);

    arena_push(&path_arena, ".fasm", 6);
    generate(path_arena.data);

    if (run) {
        path_arena.size -= 6;
        path_arena.data[path_arena.size] = '\0';

        argv[2] = path_arena.data;
        exit(execute_command(argv + 2, false));
    }
}
