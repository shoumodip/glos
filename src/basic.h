#ifndef BASIC_H
#define BASIC_H

// Platforms
#if defined(__x86_64__) && defined(__linux__)
#define PLATFORM_X86_64_LINUX
#elif (defined(__x86_64__) || defined(_M_X64)) && (defined(_WIN32) || defined(_WIN64))
#define PLATFORM_X86_64_WINDOWS
#elif defined(__aarch64__) && defined(__APPLE__)
#define PLATFORM_ARM64_MACOS
#else
#error "Unsupported platform"
#endif

#ifdef PLATFORM_X86_64_WINDOWS
#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define OBJ_FILE_EXTENSION ".obj"
#define EXE_FILE_EXTENSION ".exe"
#else
#include <sys/mman.h>
#include <unistd.h>

#define OBJ_FILE_EXTENSION ".o"
#define EXE_FILE_EXTENSION ""

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif // PLATFORM_X86_64_WINDOWS

#include <assert.h>
#include <stdbool.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Usage:
//
// ```
// #include "basic.h"
//
// int main(int argc, char **argv) {
//     basic_init();
// }
// ```
void basic_init(void);

// AAAAAAAAAAAAAAAAAAAAAAAAHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHHH
//
// On Linux, long is 64 bits, long long is 64 bits, int64_t is long
// On macOS, long is 64 bits, long long is 64 bits, int64_t is long long
// On Windows, long is 32 bits, long long is 64 bits, int64_t is long long
//
// In printf, long long requires the format specifier %lld, and long requires %ld
//
// Literally ALL THREE of these platforms have different versions of the same damn thing.
// And the solution is to use those goofy ahh PRI*N macros.
//
// Thank you K&R. Very cool...
typedef ptrdiff_t i64;
typedef size_t    u64;
static_assert(sizeof(i64) == 8, "");
static_assert(sizeof(u64) == 8, "");

// Helper Macros
#define len(a)    (sizeof(a) / sizeof(*(a)))
#define unused(v) (void) (v)

#define panic(...) (fprintf(stderr, __VA_ARGS__), fflush(stdout), fflush(stderr), abort())

#ifdef _MSC_VER
#define Printf_Like(n)
#else
#define Printf_Like(n) __attribute__((format(printf, (n), (n) + 1)))
#endif // _MSC_VER

#ifdef unreachable
#undef unreachable
#endif

#define todo()        (panic("%s:%d: TODO\n", __FILE__, __LINE__))
#define unreachable() (panic("%s:%d: Unreachable\n", __FILE__, __LINE__))

#define return_defer(value)                                                                                            \
    do {                                                                                                               \
        result = (value);                                                                                              \
        goto defer;                                                                                                    \
    } while (0)

#define ll_foreach(it, ll) for (__typeof__((ll)->head) it = (ll)->head; it; it = it->next)
#define ll_foreach2(a, b, ll1, ll2)                                                                                    \
    for (__typeof__((ll1)->head) a = (ll1)->head, b = (ll2)->head; a && b; a = a->next, b = b->next)

#define add_trailing_s_if_plural(s, n) ((n) == 1 ? (s) : arena_sprintf(&temp_arena, "%ss", (s)))

// ANSI printing
typedef enum {
    ANSI_COLOR_DEFAULT,
    ANSI_COLOR_RED,
    ANSI_COLOR_GREEN,
    ANSI_COLOR_YELLOW,
    ANSI_COLOR_BLUE,
    ANSI_COLOR_MAGENTA,
    ANSI_COLOR_CYAN,
    ANSI_COLOR_MASK = 0xFF,

    ANSI_BOLD = 1 << 8,
    ANSI_ITALIC = 1 << 9,
    ANSI_UNDERLINE = 1 << 10,
} ANSI;

bool ansi_set(FILE *f, ANSI ansi);
void ansi_reset(FILE *f);

void afprintf(FILE *f, ANSI ansi, const char *fmt, ...) Printf_Like(3);

// Dynamic Array
#define DA_INIT_CAP 128

#define DA_Fields(T)                                                                                                   \
    T     *data;                                                                                                       \
    size_t count;                                                                                                      \
    size_t capacity

#define DA(T)                                                                                                          \
    struct {                                                                                                           \
        DA_Fields(T);                                                                                                  \
    }

#define da_grow(l, c) (da_resize((void **) &(l)->data, &(l)->capacity, sizeof(*(l)->data), c))
#define da_free(l)    (da_grow(l, 0), (l)->count = 0)
#define da_push(l, v) (da_grow(l, (l)->count + 1), (l)->data[(l)->count] = (v), (l)->count++)
#define da_push_many(l, v, c)                                                                                          \
    (da_grow(l, (l)->count + c), memcpy((l)->data + (l)->count, (v), (c) * sizeof(*(l)->data)), (l)->count += (c))

void da_resize(void **data, size_t *capacity, size_t size, size_t count);

// Hash Table
#define HT(K, V)                                                                                                       \
    struct {                                                                                                           \
        struct {                                                                                                       \
            uint8_t state;                                                                                             \
            K       key;                                                                                               \
            V       value;                                                                                             \
        } *data;                                                                                                       \
                                                                                                                       \
        size_t    count;                                                                                               \
        size_t    capacity;                                                                                            \
        HT_Hasheq hasheq;                                                                                              \
    }

// For providing custom hashing and equality functionality. The contract is:
//
// ```
// hasheq(&key, NULL, sizeof(key)) => Hash 'key'
// hasheq(&a, &b, sizeof(a))       => Compare 'a' and 'b' and return non-zero if equal
// ```
//
// By default, the bytes of the key are compared. For having C-strings as the key, the function 'ht_hasheq_cstr' is
// already provided.
// ```
// HT(const char *, int) ht = {.hasheq = ht_hasheq_cstr};
// ```
typedef uint64_t (*HT_Hasheq)(const void *a, const void *b, size_t n);

// V *ht_get(HT(K, V) *ht, K key)
#define ht_get(ht, k)                                                                                                  \
    ((__typeof__((ht)->data->value) *) ht_get_impl(                                                                    \
        (ht)->data, (ht)->capacity, HT_LAYOUT(ht), (ht)->hasheq, (__typeof__((ht)->data->key)[]) {k}))

// V *ht_set(HT(K, V) *ht, K key, V value)
#define ht_set(ht, k, v)                                                                                               \
    ((__typeof__((ht)->data->value) *) ht_set_impl(                                                                    \
        (void **) &(ht)->data,                                                                                         \
        &(ht)->count,                                                                                                  \
        &(ht)->capacity,                                                                                               \
        HT_LAYOUT(ht),                                                                                                 \
        (ht)->hasheq,                                                                                                  \
        (__typeof__((ht)->data->key)[]) {k},                                                                           \
        (__typeof__((ht)->data->value)[]) {v}))

// void ht_delete(HT(K, V) *ht, K key)
#define ht_delete(ht, k)                                                                                               \
    (ht_delete_impl(                                                                                                   \
        (ht)->data, &(ht)->count, (ht)->capacity, HT_LAYOUT(ht), (ht)->hasheq, (__typeof__((ht)->data->key)[]) {k}))

// void ht_clear(HT(K, V) *ht)
#define ht_clear(ht) ((ht)->data ? memset((ht)->data, 0, (ht)->capacity * sizeof(*(ht)->data)) : NULL, (ht)->count = 0)

// void ht_free(HT(K, V) *ht)
#define ht_free(ht) (free((ht)->data), memset((ht), 0, sizeof(*(ht))))

// HT(const char *, int) xs = {0};
// ...
// ht_foreach(x, &xs) {
//     printf("%s => %d\n", *x.key, *x.value);
// }
#define ht_foreach(it, ht)                                                                                             \
    MSVC_SUPPRESS_4116                                                                                                 \
    for (HT_Iter(__typeof__((ht)->data->key), __typeof__((ht)->data->value)) it = {0}; ht_iter(ht, it);)

#define HT_Iter(K, V)                                                                                                  \
    struct {                                                                                                           \
        const K *key;                                                                                                  \
        V       *value;                                                                                                \
                                                                                                                       \
        bool   started;                                                                                                \
        size_t index;                                                                                                  \
    }

// bool ht_iter(HT(K, V) *ht, HT_Iter(K, V) *it)
#define ht_iter(ht, it)                                                                                                \
    ht_iter_impl(                                                                                                      \
        (ht)->data,                                                                                                    \
        (ht)->capacity,                                                                                                \
        HT_LAYOUT(ht),                                                                                                 \
        &(it.started),                                                                                                 \
        &(it).index,                                                                                                   \
        (void *) &(it).key,                                                                                            \
        (void *) &(it).value)

typedef struct {
    size_t key_offset;
    size_t key_size;

    size_t value_offset;
    size_t value_size;

    size_t entry_size;
} HT_Layout;

#define HT_LAYOUT(ht)                                                                                                  \
    ((HT_Layout) {                                                                                                     \
        .key_offset = offsetof(__typeof__(*(ht)->data), key),                                                          \
        .key_size = sizeof((ht)->data->key), /* NOLINT(bugprone-sizeof-expression) */                                  \
                                                                                                                       \
        .value_offset = offsetof(__typeof__(*(ht)->data), value),                                                      \
        .value_size = sizeof((ht)->data->value), /* NOLINT(bugprone-sizeof-expression) */                              \
                                                                                                                       \
        .entry_size = sizeof(*(ht)->data),                                                                             \
    })

uint64_t ht_hasheq_bytes(const void *a, const void *b, size_t n);
uint64_t ht_hasheq_cstr(const void *a, const void *b, size_t n);
uint64_t ht_hasheq_sv(const void *va, const void *vb, size_t n);
uint64_t ht_hash_combine(uint64_t a, uint64_t b);

void *ht_find_impl(void *data, size_t capacity, HT_Layout layout, HT_Hasheq hasheq, const void *key);
void *ht_get_impl(void *data, size_t capacity, HT_Layout layout, HT_Hasheq hasheq, const void *key);
void *ht_set_impl(
    void      **ht_data,
    size_t     *ht_count,
    size_t     *ht_capacity,
    HT_Layout   layout,
    HT_Hasheq   hasheq,
    const void *key,
    const void *value);
void ht_delete_impl(void *data, size_t *count, size_t capacity, HT_Layout layout, HT_Hasheq hasheq, const void *key);

bool ht_iter_impl(
    void *data, size_t capacity, HT_Layout layout, bool *started, size_t *index, void **key, void **value);

#ifdef _MSC_VER
#define MSVC_SUPPRESS_4116 __pragma(warning(suppress : 4116))
#else
#define MSVC_SUPPRESS_4116
#endif // _MSC_VER

// Characters
bool is_space(char ch);
void print_char_safe(FILE *f, char ch);

// String View
typedef struct {
    const char *data;
    size_t      count;
} SV;

#define SV_Fmt    "%.*s"
#define SV_Arg(s) (int) ((s).count), ((s).data)

SV sv_from_cstr(const char *cstr);
SV sv_strip_suffix(SV a, SV b);

bool sv_eq(SV a, SV b);
int  sv_cmp(SV a, SV b);
bool sv_match(SV a, const char *b);
bool sv_has_prefix(SV a, SV b);
bool sv_has_suffix(SV a, SV b);
bool sv_find(SV s, char ch, size_t *index);

SV sv_trim(SV s, char ch);
SV sv_drop(SV s, size_t count);
SV sv_drop_mut(SV *s, size_t count);
SV sv_drop_while_mut(SV *s, bool (*f)(char ch));
SV sv_split(SV s, char ch);
SV sv_split_mut(SV *s, char ch);
SV sv_split_by(SV s, bool (*f)(char ch));
SV sv_split_by_mut(SV *s, bool (*f)(char ch));

// String Builder
typedef DA(char) SB;

#define sb_free      da_free
#define sb_grow      da_grow
#define sb_push      da_push
#define sb_push_many da_push_many

void sb_sprintf(SB *sb, const char *fmt, ...) Printf_Like(2);

// These are implemented as functions instead of macros to avoid evaluating the argument twice
void sb_push_sv(SB *sb, SV sv);
void sb_push_cstr(SB *sb, const char *cstr);

extern SB default_sb;

// Arena Allocator
typedef struct {
    char  *data;
    size_t head;
    size_t capacity;
} Arena;

void arena_free(Arena *a);
void arena_reset(Arena *a, const void *ptr);
void arena_reset_noalign(Arena *a, const void *ptr);

void *arena_alloc(Arena *a, size_t size);
void *arena_clone(Arena *a, const void *data, size_t size);

char *arena_sprintf(Arena *a, const char *fmt, ...) Printf_Like(2);
char *arena_sv_to_cstr(Arena *a, SV sv);
char *arena_sb_to_cstr(Arena *a, SB *sb, size_t start);
SV    arena_sb_to_sv(Arena *a, SB *sb, size_t start);

extern Arena temp_arena;
extern Arena default_arena;

// FS
bool read_fp(FILE *f, SV *out, Arena *a); // This appends a NULL at the end
bool read_file(const char *path, SV *out, Arena *a);

bool delete_file(const char *path);
bool create_directory(const char *path);

bool file_exists(const char *path);
bool directory_exists(const char *path);

size_t get_modified_time(const char *path);

bool is_cmd_available_in_path(const char *cmd);
bool is_lld_available_in_path(void);

const char *temp_replace_suffix(const char *path, const char *old, const char *new);

void temporary_files_push(const char *path);
void temporary_files_cleanup(void);

// Paths
const char *get_cwd(Arena *a);
const char *get_absolute_path(SV cwd, SV path, Arena *a);    // `cwd` must be absolute
const char *get_relative_path(SV cwd, SV path, Arena *a);    // `cwd` and `path` must be absolute
const char *get_parent_dir_path(const char *path, Arena *a); // `path` must be absolute

#ifdef PLATFORM_X86_64_WINDOWS
void unixify_path_separators_inplace(char *data, size_t count);
#endif // PLATFORM_X86_64_WINDOWS

// Processes
typedef DA(const char *) Cmd;

#define cmd_free da_free
#define cmd_grow da_grow

#define cmd_push(cmd, ...) da_push_many(cmd, ((const char *[]) {__VA_ARGS__}), len(((const char *[]) {__VA_ARGS__})))
#define cmd_push_many      da_push_many

void cmd_show(Cmd cmd, FILE *f);

typedef struct {
    FILE **in;
    FILE **out;
    FILE **err;
} Cmd_Stdio;

#ifdef PLATFORM_X86_64_WINDOWS
typedef struct {
    HANDLE id;
    FILE  *in;
    FILE  *out;
    FILE  *err;
} Proc;

#define PROC_INVALID INVALID_HANDLE_VALUE
#else
typedef struct {
    int   id;
    FILE *in;
    FILE *out;
    FILE *err;
} Proc;

#define PROC_INVALID -1
#endif // PLATFORM_X86_64_WINDOWS

Proc cmd_run_async(Cmd *c, Cmd_Stdio stdio);
int  cmd_run_sync(Cmd *c, Cmd_Stdio stdio);
int  cmd_wait(Proc proc);

typedef struct {
    Proc  *data;
    size_t count;
    size_t capacity;

    size_t nprocs;
    void (*callback_before_wait)(Proc proc);
} Procs;

bool procs_push(Procs *ps, Proc p);
bool procs_flush(Procs *ps);

// Profiling
// #define PROFILING
#ifdef PROFILING
#include <time.h>
extern clock_t global_profiler;

#define perf_begin()    (global_profiler = clock())
#define perf_end(label) (printf("%s: %g\n", label, (double) (clock() - global_profiler) / CLOCKS_PER_SEC))
#else
#define perf_begin()
#define perf_end(label)
#endif // PROFILING

#endif // BASIC_H
