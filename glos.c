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
#include <sys/ioctl.h>

// Misc
size_t min(size_t a, size_t b)
{
    return a < b ? a : b;
}

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

bool str_ends_with(Str a, Str b)
{
    return a.size >= b.size && memcmp(a.data + a.size - b.size, b.data, b.size) == 0;
}

bool str_starts_with(Str a, Str b)
{
    return a.size >= b.size && memcmp(a.data, b.data, b.size) == 0;
}

Str str_drop_left(Str str, int n)
{
    n = min(n, str.size);
	str.data += n;
	str.size -= n;
	return str;
}

Str str_trim_left(Str str, char ch)
{
    size_t n = 0;
    while (n < str.size && str.data[n] == ch) {
        n += 1;
    }
	return str_drop_left(str, n);
}

Str str_split_at(Str *str, size_t n)
{
    n = min(n, str->size);
    Str head = *str;
    head.size = n;
    *str = str_drop_left(*str, n + 1);
    return head;
}

Str str_split_by(Str *str, char ch)
{
    size_t i = 0;
    while (i < str->size && str->data[i] != ch) {
        i += 1;
    }
    return str_split_at(str, i);
}

Str str_from_cstr(char *data)
{
    Str str;
    str.data = data;
    str.size = strlen(data);
    return str;
}

bool int_from_str(Str str, size_t *out)
{
    size_t n = 0;
    for (size_t i = 0; i < str.size; ++i) {
        if (isdigit(str.data[i])) {
            n = n * 10 + str.data[i] - '0';
        } else {
            return false;
        }
    }

    *out = n;
    return true;
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
    if (pid < 0) {
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

        if (execvp(*args, args) < 0) {
            fprintf(stderr, "error: could not execute child\n");
            exit(1);
        }
    }

    int wstatus;
    if (wait(&wstatus) < 0) {
        fprintf(stderr, "error: could not wait for child\n");
        exit(1);
    }
    return WEXITSTATUS(wstatus);
}

#define MAP_ANONYMOUS 32

int capture_command(char **args, Str *out, Str *err)
{
    int pipe_stdout[2];
    int pipe_stderr[2];

    if (pipe(pipe_stdout) < 0 || pipe(pipe_stderr) < 0) {
        fprintf(stderr, "error: could not create pipe\n");
        exit(1);
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "error: could not fork child\n");
        exit(1);
    }

    if (pid == 0) {
        if (dup2(pipe_stdout[1], STDOUT_FILENO) < 0 || dup2(pipe_stderr[1], STDERR_FILENO) < 0) {
            fprintf(stderr, "error: could not create pipe\n");
            exit(1);
        }

        close(pipe_stdout[1]);
        close(pipe_stderr[1]);

        close(pipe_stdout[0]);
        close(pipe_stderr[0]);

        if (execvp(*args, args) < 0) {
            fprintf(stderr, "error: could not execute child\n");
            exit(1);
        }
    }

    close(pipe_stdout[1]);
    close(pipe_stderr[1]);

    int wstatus;
    if (wait(&wstatus) < 0) {
        fprintf(stderr, "error: could not wait for child\n");
        exit(1);
    }
    int code = WEXITSTATUS(wstatus);

    if (ioctl(pipe_stdout[0], FIONREAD, &out->size) < 0 || ioctl(pipe_stderr[0], FIONREAD, &err->size) < 0) {
        fprintf(stderr, "error: could not capture query pipe size\n");
        exit(1);
    }

    if (out->size != 0) {
        out->data = mmap(NULL, out->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        if (out->data == MAP_FAILED || read(pipe_stdout[0], out->data, out->size) < 0) {
            fprintf(stderr, "error: could not capture child stdout\n");
            exit(1);
        }
    }

    if (err->size != 0) {
        err->data = mmap(NULL, err->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        if (err->data == MAP_FAILED || read(pipe_stderr[0], err->data, err->size) < 0) {
            fprintf(stderr, "error: could not capture child stderr\n");
            exit(1);
        }
    }

    close(pipe_stdout[0]);
    close(pipe_stderr[0]);
    return code;
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

    TOKEN_LPAREN,
    TOKEN_RPAREN,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    TOKEN_LBRACKET,
    TOKEN_RBRACKET,

    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_MUL,
    TOKEN_DIV,

    TOKEN_BOR,
    TOKEN_BAND,
    TOKEN_BNOT,

    TOKEN_SET,

    TOKEN_LNOT,

    TOKEN_GT,
    TOKEN_GE,
    TOKEN_LT,
    TOKEN_LE,
    TOKEN_EQ,
    TOKEN_NE,

    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_FOR,

    TOKEN_LET,

    TOKEN_PRINT,
    COUNT_TOKENS
};

static_assert(COUNT_TOKENS == 30);
char *cstr_from_token_kind(int kind)
{
    switch (kind) {
    case TOKEN_EOF:
        return "end of file";

    case TOKEN_INT:
        return "integer";

    case TOKEN_BOOL:
        return "boolean";

    case TOKEN_IDENT:
        return "identifier";

    case TOKEN_LPAREN:
        return "'('";

    case TOKEN_RPAREN:
        return "')'";

    case TOKEN_LBRACE:
        return "'{'";

    case TOKEN_RBRACE:
        return "'}'";

    case TOKEN_LBRACKET:
        return "'['";

    case TOKEN_RBRACKET:
        return "']'";

    case TOKEN_ADD:
        return "'+'";

    case TOKEN_SUB:
        return "'-'";

    case TOKEN_MUL:
        return "'*'";

    case TOKEN_DIV:
        return "'/'";

    case TOKEN_BOR:
        return "'|'";

    case TOKEN_BAND:
        return "'&'";

    case TOKEN_BNOT:
        return "'~'";

    case TOKEN_SET:
        return "'='";

    case TOKEN_LNOT:
        return "'!'";

    case TOKEN_GT:
        return "'>'";

    case TOKEN_GE:
        return "'>='";

    case TOKEN_LT:
        return "'<'";

    case TOKEN_LE:
        return "'<='";

    case TOKEN_EQ:
        return "'=='";

    case TOKEN_NE:
        return "'!='";

    case TOKEN_IF:
        return "keyword 'if'";

    case TOKEN_ELSE:
        return "keyword 'else'";

    case TOKEN_FOR:
        return "keyword 'for'";

    case TOKEN_LET:
        return "keyword 'let'";

    case TOKEN_PRINT:
        return "keyword 'print'";

    default: assert(0 && "unreachable");
    }
}

typedef struct {
    int kind;
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

bool lexer_match(char ch)
{
    if (lexer.str.size > 0 && *lexer.str.data == ch) {
        lexer_advance();
        return true;
    }
    return false;
}

static_assert(COUNT_TOKENS == 30);
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
        } else if (lexer_match('#')) {
            if (lexer_match('#')) {
                while (lexer.str.size > 0) {
                    if (lexer_match('#') && lexer_match('#')) {
                        break;
                    }

                    lexer_advance();
                }
            } else {
                while (lexer.str.size > 0 && *lexer.str.data != '\n') {
                    lexer_advance();
                }
            }
        } else {
            break;
        }
    }

    Token token;
    token.pos = lexer.pos;
    token.str = lexer.str;

    if (lexer.str.size == 0) {
        token.kind = TOKEN_EOF;
    } else if (isdigit(*lexer.str.data)) {
        token.kind = TOKEN_INT;
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
            token.kind = TOKEN_BOOL;
            token.data = 1;
        } else if (str_eq(token.str, str_from_cstr("false"))) {
            token.kind = TOKEN_BOOL;
            token.data = 0;
        } else if (str_eq(token.str, str_from_cstr("if"))) {
            token.kind = TOKEN_IF;
        } else if (str_eq(token.str, str_from_cstr("else"))) {
            token.kind = TOKEN_ELSE;
        } else if (str_eq(token.str, str_from_cstr("for"))) {
            token.kind = TOKEN_FOR;
        } else if (str_eq(token.str, str_from_cstr("let"))) {
            token.kind = TOKEN_LET;
        } else if (str_eq(token.str, str_from_cstr("print"))) {
            token.kind = TOKEN_PRINT;
        } else {
            token.kind = TOKEN_IDENT;
        }
    } else {
        switch (lexer_consume()) {
        case '(':
            token.kind = TOKEN_LPAREN;
            break;

        case ')':
            token.kind = TOKEN_RPAREN;
            break;

        case '{':
            token.kind = TOKEN_LBRACE;
            break;

        case '}':
            token.kind = TOKEN_RBRACE;
            break;

        case '[':
            token.kind = TOKEN_LBRACKET;
            break;

        case ']':
            token.kind = TOKEN_RBRACKET;
            break;

        case '+':
            token.kind = TOKEN_ADD;
            break;

        case '-':
            token.kind = TOKEN_SUB;
            break;

        case '*':
            token.kind = TOKEN_MUL;
            break;

        case '/':
            token.kind = TOKEN_DIV;
            break;

        case '|':
            token.kind = TOKEN_BOR;
            break;

        case '&':
            token.kind = TOKEN_BAND;
            break;

        case '~':
            token.kind = TOKEN_BNOT;
            break;

        case '>':
            if (lexer_match('=')) {
                token.kind = TOKEN_GE;
            } else {
                token.kind = TOKEN_GT;
            }
            break;

        case '<':
            if (lexer_match('=')) {
                token.kind = TOKEN_LE;
            } else {
                token.kind = TOKEN_LT;
            }
            break;

        case '=':
            if (lexer_match('=')) {
                token.kind = TOKEN_EQ;
            } else {
                token.kind = TOKEN_SET;
            }
            break;

        case '!':
            if (lexer_match('=')) {
                token.kind = TOKEN_NE;
            } else {
                token.kind = TOKEN_LNOT;
            }
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

bool lexer_read(int kind)
{
    lexer_peek();
    lexer.peeked = lexer.buffer.kind != kind;
    return !lexer.peeked;
}

Token lexer_expect(int kind)
{
    Token token = lexer_next();
    if (token.kind != kind) {
        print_pos(stderr, token.pos);
        fprintf(stderr, "error: expected %s, got %s\n",
                cstr_from_token_kind(kind),
                cstr_from_token_kind(token.kind));
        exit(1);
    }
    return token;
}

bool lexer_peek_row(Token *token)
{
	*token = lexer_peek();
	return token->pos.row == lexer.prev_row;
}

// AST
enum {
    TYPE_NIL,
    TYPE_INT,
    TYPE_BOOL,
    COUNT_TYPES
};

typedef struct {
    int kind;
    size_t ref;
} Type;

enum {
    NODE_ATOM,
    NODE_UNARY,
    NODE_BINARY,

    NODE_BLOCK,
    NODE_IF,
    NODE_FOR,

    NODE_LET,

    NODE_PRINT,
    COUNT_NODES
};

#define NODE_UNARY_VALUE 0

#define NODE_BINARY_LHS 0
#define NODE_BINARY_RHS 1

#define NODE_BLOCK_START 0

#define NODE_IF_COND 0
#define NODE_IF_THEN 1
#define NODE_IF_ELSE 2

#define NODE_FOR_COND 0
#define NODE_FOR_BODY 1

#define NODE_LET_EXPR 0
#define NODE_LET_TYPE 1

#define NODE_PRINT_VALUE 0

typedef struct {
    int kind;
    Type type;
    Token token;

    size_t nodes[3];
    size_t next;
} Node;

#define NODES_CAP 1024
Node nodes[NODES_CAP];
size_t nodes_count;

size_t node_new(int kind, Token token)
{
    assert(nodes_count < NODES_CAP);
    nodes[nodes_count].kind = kind;
    nodes[nodes_count].token = token;
    nodes_count += 1;
    return nodes_count - 1;
}

size_t *node_list_push(size_t *list, size_t node)
{
    if (*list != 0) {
        list = &nodes[*list].next;
    }

    *list = node;
    return list;
}

// Parser
enum {
    POWER_NIL,
    POWER_SET,
    POWER_CMP,
    POWER_ADD,
    POWER_BOR,
    POWER_MUL,
    POWER_PRE,
    POWER_IDX,
};

static_assert(COUNT_TOKENS == 30);
int power_from_token_kind(int kind)
{
    switch (kind) {
    case TOKEN_LBRACKET:
        return POWER_IDX;

    case TOKEN_ADD:
    case TOKEN_SUB:
        return POWER_ADD;

    case TOKEN_MUL:
    case TOKEN_DIV:
        return POWER_MUL;

    case TOKEN_BOR:
    case TOKEN_BAND:
        return POWER_BOR;

    case TOKEN_SET:
        return POWER_SET;

    case TOKEN_GT:
    case TOKEN_GE:
    case TOKEN_LT:
    case TOKEN_LE:
    case TOKEN_EQ:
    case TOKEN_NE:
        return POWER_CMP;

    default: return POWER_NIL;
    }
}

void error_unexpected(Token token)
{
    print_pos(stderr, token.pos);
    fprintf(stderr, "error: unexpected %s\n", cstr_from_token_kind(token.kind));
    exit(1);
}

size_t parse_type(void)
{
    size_t node;
    Token token = lexer_next();
    switch (token.kind) {
    case TOKEN_IDENT:
        node = node_new(NODE_ATOM, token);
        break;

    case TOKEN_MUL:
        node = node_new(NODE_UNARY, token);
        nodes[node].nodes[NODE_UNARY_VALUE] = parse_type();
        break;

    default: error_unexpected(token);
    }

    return node;
}

static_assert(COUNT_TOKENS == 30);
size_t parse_expr(int mbp)
{
    size_t node;
    Token token = lexer_next();

    switch (token.kind) {
    case TOKEN_LPAREN:
        node = parse_expr(POWER_SET);
        lexer_expect(TOKEN_RPAREN);
        break;

    case TOKEN_INT:
    case TOKEN_BOOL:
        node = node_new(NODE_ATOM, token);
        break;

    case TOKEN_IDENT:
        node = node_new(NODE_ATOM, token);
        break;

    case TOKEN_SUB:
    case TOKEN_MUL:
    case TOKEN_BNOT:
    case TOKEN_BAND:
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

        int lbp = power_from_token_kind(token.kind);
        if (lbp <= mbp) {
            break;
        }
        lexer.peeked = false;

        size_t binary = node_new(NODE_BINARY, token);
        nodes[binary].nodes[NODE_BINARY_LHS] = node;
        switch (token.kind) {
        case TOKEN_LBRACKET:
            nodes[binary].nodes[NODE_BINARY_RHS] = parse_expr(POWER_SET);
            lexer_expect(TOKEN_RBRACKET);
            break;

        default:
            nodes[binary].nodes[NODE_BINARY_RHS] = parse_expr(lbp);
        }
        node = binary;
    }

    return node;
}

static_assert(COUNT_TOKENS == 30);
size_t parse_stmt(void)
{
    size_t node;
    Token token = lexer_next();

    switch (token.kind) {
    case TOKEN_LBRACE:
        node = node_new(NODE_BLOCK, token);
        for (size_t *list = &nodes[node].nodes[NODE_BLOCK_START]; !lexer_read(TOKEN_RBRACE); ) {
            list = node_list_push(list, parse_stmt());
        }
        break;

    case TOKEN_IF:
        node = node_new(NODE_IF, token);
        nodes[node].nodes[NODE_IF_COND] = parse_expr(POWER_SET);

        lexer_buffer(lexer_expect(TOKEN_LBRACE));
        nodes[node].nodes[NODE_IF_THEN] = parse_stmt();

        if (lexer_read(TOKEN_ELSE)) {
            lexer_buffer(lexer_expect(TOKEN_LBRACE));
            nodes[node].nodes[NODE_IF_ELSE] = parse_stmt();
        }
        break;

    case TOKEN_FOR:
        node = node_new(NODE_FOR, token);
        nodes[node].nodes[NODE_FOR_COND] = parse_expr(POWER_SET);

        lexer_buffer(lexer_expect(TOKEN_LBRACE));
        nodes[node].nodes[NODE_FOR_BODY] = parse_stmt();
        break;

    case TOKEN_LET:
        node = node_new(NODE_LET, lexer_expect(TOKEN_IDENT));

        if (lexer_read(TOKEN_SET)) {
            nodes[node].nodes[NODE_LET_EXPR] = parse_expr(POWER_SET);
        } else {
            nodes[node].nodes[NODE_LET_TYPE] = parse_type();
        }
        break;

    case TOKEN_PRINT:
        node = node_new(NODE_PRINT, token);
        nodes[node].nodes[NODE_PRINT_VALUE] = parse_expr(POWER_SET);
        break;

    default:
        lexer_buffer(token);
        node = parse_expr(POWER_NIL);
    }

    return node;
}

// Type
bool type_eq(Type a, Type b)
{
    return a.kind == b.kind && a.ref == b.ref;
}

Type type_new(int kind, size_t ref)
{
    Type type;
    type.kind = kind;
    type.ref = ref;
    return type;
}

Type type_ref(Type type)
{
    type.ref += 1;
    return type;
}

Type type_deref(Type type)
{
    assert(type.ref > 0);
    type.ref -= 1;
    return type;
}

static_assert(COUNT_TYPES == 3);
void print_type(FILE *file, Type type)
{
    for (size_t i = 0; i < type.ref; ++i) {
        fprintf(file, "*");
    }

    switch (type.kind) {
    case TYPE_NIL:
        fprintf(file, "nil");
        break;

    case TYPE_INT:
        fprintf(file, "int");
        break;

    case TYPE_BOOL:
        fprintf(file, "bool");
        break;

    default: assert(0 && "unreachable");
    }
}

// Scope
#define SCOPE_CAP 1024
size_t variables[SCOPE_CAP];
size_t variables_count;

void variables_push(size_t node)
{
    assert(variables_count < SCOPE_CAP);
    variables[variables_count] = node;
    variables_count += 1;
}

bool variables_find(Str name, size_t *index)
{
    for (size_t i = 0; i < variables_count; i += 1) {
        if (str_eq(nodes[variables[i]].token.str, name)) {
            *index = i;
            return true;
        }
    }
    return false;
}

// Checker
void error_undefined(size_t node, char *name)
{
    print_pos(stderr, nodes[node].token.pos);
    fprintf(stderr, "error: undefined %s '", name);
    print_str(stderr, nodes[node].token.str);
    fprintf(stderr, "'\n");
    exit(1);
}

void error_redefinition(size_t node, size_t prev, char *name)
{
    print_pos(stderr, nodes[node].token.pos);
    fprintf(stderr, "error: redefinition of %s '", name);
    print_str(stderr, nodes[node].token.str);
    fprintf(stderr, "'\n");
    print_pos(stderr, nodes[prev].token.pos);
    fprintf(stderr, "note: defined here\n");
    exit(1);
}

void ref_prevent(size_t node, bool ref)
{
    if (ref) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: cannot take reference to value not in memory\n");
        exit(1);
    }
}

Type type_assert(size_t node, Type expected)
{
    Type actual = nodes[node].type;
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

Type type_assert_arith(size_t node)
{
    Type actual = nodes[node].type;
    if (actual.kind != TYPE_INT && actual.ref == 0) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: expected arithmetic type, got '");
        print_type(stderr, actual);
        fprintf(stderr, "'\n");
        exit(1);
    }
    return actual;
}

Type type_assert_pointer(size_t node)
{
    Type actual = nodes[node].type;
    if (actual.ref == 0) {
        print_pos(stderr, nodes[node].token.pos);
        fprintf(stderr, "error: expected pointer type, got '");
        print_type(stderr, actual);
        fprintf(stderr, "'\n");
        exit(1);
    }
    return actual;
}

void check_type(size_t node)
{
    switch (nodes[node].kind) {
    case NODE_ATOM:
        if (str_eq(nodes[node].token.str, str_from_cstr("int"))) {
            nodes[node].type = type_new(TYPE_INT, 0);
        } else if (str_eq(nodes[node].token.str, str_from_cstr("bool"))) {
            nodes[node].type = type_new(TYPE_BOOL, 0);
        } else {
            error_undefined(node, "type");
        }
        break;

    case NODE_UNARY: {
        size_t value = nodes[node].nodes[NODE_UNARY_VALUE];
        check_type(value);
        nodes[node].type = type_ref(nodes[value].type);
    } break;

    default: assert(0 && "unreachable");
    }
}

static_assert(COUNT_NODES == 8);
static_assert(COUNT_TOKENS == 30);
void check_expr(size_t node, bool ref)
{
    switch (nodes[node].kind) {
    case NODE_ATOM:
        switch (nodes[node].token.kind) {
        case TOKEN_INT:
            ref_prevent(node, ref);
            nodes[node].type = type_new(TYPE_INT, 0);
            break;

        case TOKEN_BOOL:
            ref_prevent(node, ref);
            nodes[node].type = type_new(TYPE_BOOL, 0);
            break;

        case TOKEN_IDENT: {
            size_t index;
            if (variables_find(nodes[node].token.str, &index)) {
                nodes[node].token.data = variables[index];
                nodes[node].type = nodes[variables[index]].type;
            } else {
                error_undefined(node, "identifier");
            }
        } break;

        default: assert(0 && "unreachable");
        }
        break;

    case NODE_UNARY: {
        size_t value = nodes[node].nodes[NODE_UNARY_VALUE];

        switch (nodes[node].token.kind) {
        case TOKEN_SUB:
        case TOKEN_BNOT:
            ref_prevent(node, ref);
            check_expr(value, false);
            nodes[node].type = type_assert_arith(value);
            break;

        case TOKEN_LNOT:
            ref_prevent(node, ref);
            check_expr(value, false);
            nodes[node].type = type_assert(value, type_new(TYPE_BOOL, 0));
            break;

        case TOKEN_MUL:
            check_expr(value, false);
            nodes[node].type = type_deref(type_assert_pointer(value));
            break;

        case TOKEN_BAND:
            ref_prevent(node, ref);
            check_expr(value, true);
            nodes[node].type = type_ref(nodes[value].type);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    case NODE_BINARY: {
        size_t lhs = nodes[node].nodes[NODE_BINARY_LHS];
        size_t rhs = nodes[node].nodes[NODE_BINARY_RHS];

        switch (nodes[node].token.kind) {
        case TOKEN_LBRACKET:
            check_expr(lhs, false);
            check_expr(rhs, false);
            nodes[node].type = type_deref(type_assert_pointer(lhs));
            type_assert(rhs, type_new(TYPE_INT, 0));
            break;

        case TOKEN_ADD:
        case TOKEN_SUB:
        case TOKEN_MUL:
        case TOKEN_DIV:
        case TOKEN_BOR:
        case TOKEN_BAND:
            ref_prevent(node, ref);
            check_expr(lhs, false);
            check_expr(rhs, false);
            nodes[node].type = type_assert(rhs, type_assert_arith(lhs));
            break;

        case TOKEN_GT:
        case TOKEN_GE:
        case TOKEN_LT:
        case TOKEN_LE:
        case TOKEN_EQ:
        case TOKEN_NE:
            ref_prevent(node, ref);
            check_expr(lhs, false);
            check_expr(rhs, false);
            type_assert(rhs, type_assert_arith(lhs));
            nodes[node].type = type_new(TYPE_BOOL, 0);
            break;

        case TOKEN_SET:
            ref_prevent(node, ref);
            check_expr(lhs, true);
            check_expr(rhs, false);
            type_assert(rhs, nodes[lhs].type);
            nodes[node].type = type_new(TYPE_NIL, 0);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    default: assert(0 && "unreachable");
    }
}

static_assert(COUNT_NODES == 8);
static_assert(COUNT_TOKENS == 30);
void check_stmt(size_t node)
{
    switch (nodes[node].kind) {
    case NODE_BLOCK:
        for (node = nodes[node].nodes[NODE_BLOCK_START]; node != 0; node = nodes[node].next) {
            check_stmt(node);
        }
        break;

    case NODE_IF: {
        size_t cond = nodes[node].nodes[NODE_IF_COND];
        check_expr(cond, false);
        type_assert(cond, type_new(TYPE_BOOL, 0));

        check_stmt(nodes[node].nodes[NODE_IF_THEN]);

        size_t ante = nodes[node].nodes[NODE_IF_ELSE];
        if (ante != 0) {
            check_stmt(ante);
        }
    } break;

    case NODE_FOR: {
        size_t cond = nodes[node].nodes[NODE_FOR_COND];
        check_expr(cond, false);
        type_assert(cond, type_new(TYPE_BOOL, 0));

        check_stmt(nodes[node].nodes[NODE_FOR_BODY]);
    } break;

    case NODE_LET: {
        size_t prev;
        if (variables_find(nodes[node].token.str, &prev)) {
            error_redefinition(node, variables[prev], "variable");
        }

        size_t expr = nodes[node].nodes[NODE_LET_EXPR];
        if (expr != 0) {
            check_expr(expr, false);
            nodes[node].type = nodes[expr].type;
        } else {
            expr = nodes[node].nodes[NODE_LET_TYPE];
            check_type(expr);
            nodes[node].type = nodes[expr].type;
        }

        variables_push(node);
    } break;

    case NODE_PRINT:
        check_expr(nodes[node].nodes[NODE_PRINT_VALUE], false);
        break;

    default: check_expr(node, false);
    }
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

    OP_BOR,
    OP_BAND,
    OP_BNOT,
    OP_LNOT,

    OP_GT,
    OP_GE,
    OP_LT,
    OP_LE,
    OP_EQ,
    OP_NE,

    OP_GPTR,
    OP_LOAD,
    OP_STORE,

    OP_INDEX,

    OP_GOTO,
    OP_ELSE,

    OP_HALT,
    OP_PRINT,
    COUNT_OPS
};

typedef struct {
    int kind;
    size_t data;
} Op;

static_assert(COUNT_OPS == 25);
void print_op(FILE *file, Op op)
{
    switch (op.kind) {
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

    case OP_BOR:
        fprintf(file, "bor\n");
        break;

    case OP_BAND:
        fprintf(file, "band\n");
        break;

    case OP_BNOT:
        fprintf(file, "bnot\n");
        break;

    case OP_LNOT:
        fprintf(file, "lnot\n");
        break;

    case OP_GT:
        fprintf(file, "gt\n");
        break;

    case OP_GE:
        fprintf(file, "ge\n");
        break;

    case OP_LT:
        fprintf(file, "lt\n");
        break;

    case OP_LE:
        fprintf(file, "le\n");
        break;

    case OP_EQ:
        fprintf(file, "eq\n");
        break;

    case OP_NE:
        fprintf(file, "ne\n");
        break;

    case OP_GPTR:
        fprintf(file, "gptr %zu\n", op.data);
        break;

    case OP_LOAD:
        fprintf(file, "load %zu\n", op.data);
        break;

    case OP_STORE:
        fprintf(file, "store %zu\n", op.data);
        break;

    case OP_INDEX:
        fprintf(file, "index %zu\n", op.data);
        break;

    case OP_GOTO:
        fprintf(file, "goto %zu\n", op.data);
        break;

    case OP_ELSE:
        fprintf(file, "else %zu\n", op.data);
        break;

    case OP_HALT:
        fprintf(file, "halt\n");
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

void ops_push(int kind, size_t data)
{
    assert(ops_count < OPS_CAP);
    ops[ops_count].kind = kind;
    ops[ops_count].data = data;
    ops_count += 1;
}

void print_ops(FILE *file)
{
    for (size_t i = 0; i < ops_count; i += 1) {
        print_op(file, ops[i]);
    }
}

// Compiler
size_t align(size_t n)
{
    return (n + 7) & -8;
}

static_assert(COUNT_TYPES == 3);
size_t type_size(Type type)
{
    if (type.ref > 0) {
        return 8;
    }

    switch (type.kind) {
    case TYPE_NIL:
        return 0;

    case TYPE_INT:
        return 8;

    case TYPE_BOOL:
        return 1;

    default: assert(0 && "unreachable");
    }
}

size_t global_size;
size_t global_alloc(size_t size)
{
    global_size += size;
    return global_size - size;
}

static_assert(COUNT_NODES == 8);
static_assert(COUNT_TOKENS == 30);
void compile_expr(size_t node, bool ref)
{
    switch (nodes[node].kind) {
    case NODE_ATOM:
        switch (nodes[node].token.kind) {
        case TOKEN_INT:
        case TOKEN_BOOL:
            ops_push(OP_PUSH, nodes[node].token.data);
            break;

        case TOKEN_IDENT: {
            size_t real = nodes[node].token.data;
            ops_push(OP_GPTR, nodes[real].token.data);

            if (!ref) {
                ops_push(OP_LOAD, type_size(nodes[real].type));
            }
        } break;

        default: assert(0 && "unreachable");
        }
        break;

    case NODE_UNARY: {
        size_t value = nodes[node].nodes[NODE_UNARY_VALUE];

        switch (nodes[node].token.kind) {
        case TOKEN_SUB:
            compile_expr(value, false);
            ops_push(OP_NEG, 0);
            break;

        case TOKEN_MUL:
            compile_expr(value, false);
            if (!ref) {
                ops_push(OP_LOAD, type_size(nodes[node].type));
            }
            break;

        case TOKEN_BAND:
            compile_expr(value, true);
            break;

        case TOKEN_BNOT:
            compile_expr(value, false);
            ops_push(OP_BNOT, 0);
            break;

        case TOKEN_LNOT:
            compile_expr(value, false);
            ops_push(OP_LNOT, 0);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    case NODE_BINARY: {
        size_t lhs = nodes[node].nodes[NODE_BINARY_LHS];
        size_t rhs = nodes[node].nodes[NODE_BINARY_RHS];

        switch (nodes[node].token.kind) {
        case TOKEN_LBRACKET:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_INDEX, type_size(nodes[node].type));
            break;

        case TOKEN_ADD:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_ADD, 0);
            break;

        case TOKEN_SUB:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_SUB, 0);
            break;

        case TOKEN_MUL:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_MUL, 0);
            break;

        case TOKEN_DIV:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_DIV, 0);
            break;

        case TOKEN_BOR:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_BOR, 0);
            break;

        case TOKEN_BAND:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_BAND, 0);
            break;

        case TOKEN_SET:
            compile_expr(lhs, true);
            compile_expr(rhs, false);
            ops_push(OP_STORE, type_size(nodes[rhs].type));
            break;

        case TOKEN_GT:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_GT, 0);
            break;

        case TOKEN_GE:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_GE, 0);
            break;

        case TOKEN_LT:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_LT, 0);
            break;

        case TOKEN_LE:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_LE, 0);
            break;

        case TOKEN_EQ:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_EQ, 0);
            break;

        case TOKEN_NE:
            compile_expr(lhs, false);
            compile_expr(rhs, false);
            ops_push(OP_NE, 0);
            break;

        default: assert(0 && "unreachable");
        }
    } break;

    default:
        assert(0 && "unreachable");
    }
}

static_assert(COUNT_NODES == 8);
static_assert(COUNT_TOKENS == 30);
void compile_stmt(size_t node)
{
    switch (nodes[node].kind) {
    case NODE_BLOCK:
        for (node = nodes[node].nodes[NODE_BLOCK_START]; node != 0; node = nodes[node].next) {
            compile_stmt(node);
        }
        break;

    case NODE_IF: {
        compile_expr(nodes[node].nodes[NODE_IF_COND], false);

        size_t then_addr = ops_count;
        ops_push(OP_ELSE, 0);
        compile_stmt(nodes[node].nodes[NODE_IF_THEN]);

        if (nodes[node].nodes[NODE_IF_ELSE] != 0) {
            size_t else_addr = ops_count;
            ops_push(OP_GOTO, 0);
            ops[then_addr].data = ops_count;
            compile_stmt(nodes[node].nodes[NODE_IF_ELSE]);
            ops[else_addr].data = ops_count;
        } else {
            ops[then_addr].data = ops_count;
        }
    } break;

    case NODE_FOR: {
        size_t loop_addr = ops_count;
        compile_expr(nodes[node].nodes[NODE_FOR_COND], false);

        size_t body_addr = ops_count;
        ops_push(OP_ELSE, 0);
        compile_stmt(nodes[node].nodes[NODE_FOR_BODY]);

        ops_push(OP_GOTO, loop_addr);
        ops[body_addr].data = ops_count;
    } break;

    case NODE_LET: {
        size_t size = type_size(nodes[node].type);
        nodes[node].token.data = global_alloc(align(size));

        size_t expr = nodes[node].nodes[NODE_LET_EXPR];
        if (expr != 0) {
            ops_push(OP_GPTR, nodes[node].token.data);
            compile_expr(expr, false);
            ops_push(OP_STORE, size);
        }
    } break;

    case NODE_PRINT:
        compile_expr(nodes[node].nodes[NODE_PRINT_VALUE], false);
        ops_push(OP_PRINT, 0);
        break;

    default:
        compile_expr(node, false);
        ops_push(OP_DROP, align(type_size(nodes[node].type)));
    }
}

// Generator
static_assert(COUNT_OPS == 25);
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

        switch (op.kind) {
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

        case OP_BOR:
            fprintf(file, "pop rax\n");
            fprintf(file, "or [rsp], rax\n");
            break;

        case OP_BAND:
            fprintf(file, "pop rax\n");
            fprintf(file, "and [rsp], rax\n");
            break;

        case OP_BNOT:
            fprintf(file, "not qword [rsp]\n");
            break;

        case OP_LNOT:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "test rax, rax\n");
            fprintf(file, "sete bl\n");
            fprintf(file, "push rbx\n");
            break;

        case OP_GT:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setg bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_GE:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setge bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_LT:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setl bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_LE:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setle bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_EQ:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "sete bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_NE:
            fprintf(file, "pop rax\n");
            fprintf(file, "xor rbx, rbx\n");
            fprintf(file, "cmp [rsp], rax\n");
            fprintf(file, "setne bl\n");
            fprintf(file, "mov [rsp], rbx\n");
            break;

        case OP_GPTR:
            fprintf(file, "push memory+%zu\n", op.data);
            break;

        case OP_LOAD:
            fprintf(file, "pop rax\n");
            switch (op.data) {
            case 1:
                fprintf(file, "xor rbx, rbx\n");
                fprintf(file, "mov bl, [rax]\n");
                fprintf(file, "push rbx\n");
                break;

            case 8:
                fprintf(file, "push qword [rax]\n");
                break;

            default:
                fprintf(file, "sub rsp, %zu\n", op.data);
                for (size_t j = 0; j < op.data; j += 8) {
                    fprintf(file, "mov rbx, [rax+%zu]\n", j);
                    fprintf(file, "mov [rsp+%zu], rbx\n", j);
                }
            }
            break;

        case OP_STORE:
            switch (op.data) {
            case 1:
                fprintf(file, "pop rbx\n");
                fprintf(file, "pop rax\n");
                fprintf(file, "mov [rax], bl\n");
                break;

            case 8:
                fprintf(file, "pop rbx\n");
                fprintf(file, "pop rax\n");
                fprintf(file, "mov [rax], rbx\n");
                break;

            default:
                fprintf(file, "mov rax, [rsp+%zu]\n", op.data);
                for (size_t j = 0; j < op.data; j += 8) {
                    fprintf(file, "mov rbx, [rsp+%zu]\n", j);
                    fprintf(file, "mov [rax+%zu], rbx\n", j);
                }
                fprintf(file, "add rsp, %zu\n", op.data + 8);
            }
            break;

        case OP_INDEX:
            fprintf(file, "pop rax\n");
            fprintf(file, "mov rbx, %zu\n", op.data);
            fprintf(file, "mul rbx\n");
            fprintf(file, "add [rsp], rax\n");
            break;

        case OP_GOTO:
            fprintf(file, "jmp I%zu\n", op.data);
            break;

        case OP_ELSE:
            fprintf(file, "pop rax\n");
            fprintf(file, "test rax, rax\n");
            fprintf(file, "jz I%zu\n", op.data);
            break;

        case OP_HALT:
            fprintf(file, "mov rax, 60\n");
            fprintf(file, "xor rdi, rdi\n");
            fprintf(file, "syscall\n");
            break;

        case OP_PRINT:
            fprintf(file, "pop rdi\n");
            fprintf(file, "call PRINT\n");
            break;

        default: assert(0 && "unreachable");
        }
    }

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

    if (global_size) {
        fprintf(file, "segment readable writeable\n");
        fprintf(file, "memory: rb %zu\n", global_size);
    }

    fclose(file);

    char *args[] = {"fasm", path, NULL};
    if (execute_command(args, true) == 0) {
        unlink(path);
    } else {
        fprintf(stderr, "error: could not generate executable\n");
        exit(1);
    }
}

// Tester
typedef struct {
    Str out;
    Str err;
    int exit;
    bool debug;
} Test;

void print_test(FILE *file, Test test)
{
    if (test.out.size != 0) {
        if (test.debug) {
            fprintf(file, "stdout: %zu\n", test.out.size);
        } else {
            fprintf(file, "stdout:\n");
        }
        print_str(file, test.out);
        fprintf(file, "\n");
    }

    if (test.err.size != 0) {
        if (test.debug) {
            fprintf(file, "stderr: %zu\n", test.err.size);
        } else {
            fprintf(file, "stderr:\n");
        }
        print_str(file, test.err);
        fprintf(file, "\n");
    }

    if (test.exit != 0) {
        fprintf(file, "exit: %d\n", test.exit);
    }
}

void test_parse_data(char *path, Str *contents, Str *out)
{
    Str value = str_split_by(contents, '\n');
    if (!int_from_str(value, &out->size)) {
        fprintf(stderr, "%s: error: invalid number '", path);
        print_str(stderr, value);
        fprintf(stderr, "'\n");
        exit(1);
    }

    if (contents->size <= out->size) {
        fprintf(stderr, "%s: error: not enough bytes in file\n", path);
        exit(1);
    }

    out->data = contents->data;
    *contents = str_drop_left(*contents, out->size);
}

bool test_file(char *program, char *path)
{
    Test expected = {0};

    Str contents;
    if (!read_file(&contents, path)) {
        fprintf(stderr, "error: could not read file '%s'\n", path);
        exit(1);
    }

    if (!str_starts_with(contents, str_from_cstr("##\n"))) {
        fprintf(stderr, "%s: note: testing information not found\n", path);
        exit(1);
    }
    contents = str_drop_left(contents, 3);

    Str key = str_split_by(&contents, ' ');
    if (str_eq(key, str_from_cstr("stdout:"))) {
        test_parse_data(path, &contents, &expected.out);
        contents = str_trim_left(contents, '\n');
        key = str_split_by(&contents, ' ');
    }

    if (str_eq(key, str_from_cstr("stderr:"))) {
        test_parse_data(path, &contents, &expected.err);
        contents = str_trim_left(contents, '\n');
        key = str_split_by(&contents, ' ');
    }

    if (str_eq(key, str_from_cstr("exit:"))) {
        size_t value;
        if (!int_from_str(str_split_by(&contents, '\n'), &value)) {
            fprintf(stderr, "error: expected number of bytes of stdout\n");
            exit(1);
        }
        expected.exit = value;
    }

    char *args[] = {program, "-r", path, NULL};
    Test actual = {0};
    actual.exit = capture_command(args, &actual.out, &actual.err);

    if (actual.exit != expected.exit || !str_eq(actual.out, expected.out) || !str_eq(actual.err, expected.err)) {
        fprintf(stderr, "%s: fail\n", path);
        fprintf(stderr, "\n----------- Actual -----------\n");
        print_test(stderr, actual);
        fprintf(stderr, "------------------------------\n");
        fprintf(stderr, "\n---------- Expected ----------\n");
        print_test(stderr, expected);
        fprintf(stderr, "------------------------------\n");
        return false;
    }
    return true;
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
    fprintf(file, "  -t    Run the test for the program\n");
    fprintf(file, "  -T    Run the program and print test information\n");
}

enum {
    MODE_COM,
    MODE_RUN,
    MODE_TEST_RUN,
    MODE_TEST_CHECK,
};

int main(int argc, char **argv)
{
    int mode = MODE_COM;
    char *path = argv[1];
    if (path != NULL) {
        if (strcmp(path, "-h") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(path, "-r") == 0) {
            mode = MODE_RUN;
            path = argv[2];
        } else if (strcmp(path, "-T") == 0) {
            mode = MODE_TEST_RUN;
            path = argv[2];
        } else if (strcmp(path, "-t") == 0) {
            mode = MODE_TEST_CHECK;
            path = argv[2];
        }
    }

    switch (mode) {
    case MODE_TEST_RUN: {
        if (path == NULL) {
            usage(stderr);
            fprintf(stderr, "\nerror: file path not provided\n");
            exit(1);
        }

        char *args[] = {*argv, "-r", path, NULL};
        Test test = {0};
        test.exit = capture_command(args, &test.out, &test.err);
        test.debug = true;

        fprintf(stderr, "----------- Result -----------\n");
        print_test(stderr, test);
        fprintf(stderr, "------------------------------\n");
    } break;

    case MODE_TEST_CHECK: {
        size_t total = 0;
        size_t failed = 0;
        for (int i = 2; i < argc; ++i) {
            if (str_ends_with(str_from_cstr(argv[i]), str_from_cstr(".glos"))) {
                total += 1;
                if (!test_file(*argv, argv[i])) {
                    failed += 1;
                }
            }
        }

        if (failed > 0) {
            fprintf(stderr, "\n");
        }

        fprintf(stderr, "Total: %zu, Passed: %zu, Failed: %zu\n", total, total - failed, failed);
    } break;

    default:
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
        ops_push(OP_HALT, 0);

        path_arena.size = 0;
        arena_push(&path_arena, "./", 2);
        arena_push(&path_arena, path, strlen(path));

        path_arena.size -= 5;
        path_arena.data[path_arena.size] = '\0';
        unlink(path_arena.data);

        arena_push(&path_arena, ".fasm", 6);
        generate(path_arena.data);

        if (mode == MODE_RUN) {
            path_arena.size -= 6;
            path_arena.data[path_arena.size] = '\0';

            argv[2] = path_arena.data;
            exit(execute_command(argv + 2, false));
        }
    }
}
