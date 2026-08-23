#include "basic.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>

#ifdef PLATFORM_X86_64_WINDOWS
#include <io.h>

static DWORD original_stdout_mode = 0;
static bool  stdout_mode_saved = false;

static DWORD original_stderr_mode = 0;
static bool  stderr_mode_saved = false;
#else
#include <sys/wait.h>
#endif // PLATFORM_X86_64_WINDOWS

static bool is_terminal(FILE *f) {
    return isatty(fileno(f));
}

static void basic_atexit(void) {
#ifdef PLATFORM_X86_64_WINDOWS
    if (stdout_mode_saved) {
        HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (handle != INVALID_HANDLE_VALUE) {
            fprintf(stdout, "\033[0m");
            fflush(stdout);
            SetConsoleMode(handle, original_stdout_mode);
        }
        stdout_mode_saved = false;
    }

    if (stderr_mode_saved) {
        HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
        if (handle != INVALID_HANDLE_VALUE) {
            fprintf(stderr, "\033[0m");
            fflush(stderr);
            SetConsoleMode(handle, original_stderr_mode);
        }
        stderr_mode_saved = false;
    }
#endif // PLATFORM_X86_64_WINDOWS

    temporary_files_cleanup();
    arena_free(&temp_arena);
    arena_free(&default_arena);
    sb_free(&default_sb);
}

void basic_init(void) {
    atexit(basic_atexit);

#ifdef PLATFORM_X86_64_WINDOWS
    if (is_terminal(stdout)) {
        HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
        if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &original_stdout_mode)) {
            stdout_mode_saved = true;
            DWORD new_mode = original_stdout_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(handle, new_mode);
        }
    }

    if (is_terminal(stderr)) {
        HANDLE handle = GetStdHandle(STD_ERROR_HANDLE);
        if (handle != INVALID_HANDLE_VALUE && GetConsoleMode(handle, &original_stderr_mode)) {
            stderr_mode_saved = true;
            DWORD new_mode = original_stderr_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(handle, new_mode);
        }
    }
#endif // PLATFORM_X86_64_WINDOWS
}

// ANSI printing
bool ansi_set(FILE *f, ANSI ansi) {
    bool has_ansi = false;
    if (is_terminal(f)) {
        switch (ansi & ANSI_COLOR_MASK) {
        case ANSI_COLOR_DEFAULT:
            break;

        case ANSI_COLOR_RED:
            fprintf(f, "\033[31m");
            has_ansi = true;
            break;

        case ANSI_COLOR_GREEN:
            fprintf(f, "\033[32m");
            has_ansi = true;
            break;

        case ANSI_COLOR_YELLOW:
            fprintf(f, "\033[33m");
            has_ansi = true;
            break;

        case ANSI_COLOR_BLUE:
            fprintf(f, "\033[34m");
            has_ansi = true;
            break;

        case ANSI_COLOR_MAGENTA:
            fprintf(f, "\033[35m");
            has_ansi = true;
            break;

        case ANSI_COLOR_CYAN:
            fprintf(f, "\033[36m");
            has_ansi = true;
            break;

        default:
            unreachable();
            break;
        }

        if (ansi & ANSI_BOLD) {
            fprintf(f, "\033[1m");
            has_ansi = true;
        }

        if (ansi & ANSI_ITALIC) {
            fprintf(f, "\033[3m");
            has_ansi = true;
        }

        if (ansi & ANSI_UNDERLINE) {
            fprintf(f, "\033[4m");
            has_ansi = true;
        }
    }
    return has_ansi;
}

void ansi_reset(FILE *f) {
    if (is_terminal(f)) {
        fprintf(f, "\033[0m");
    }
}

void afprintf(FILE *f, ANSI ansi, const char *fmt, ...) {
    const bool has_ansi = ansi_set(f, ansi);

    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);

    if (has_ansi) {
        fprintf(f, "\033[0m");
    }
}

// Dynamic Array
void da_resize(void **data, size_t *capacity, size_t size, size_t count) {
    if (!count) {
        free(*data);
        *data = NULL;
        *capacity = 0;
        return;
    }

    if (count > *capacity) {
        if (*capacity == 0) {
            *capacity = DA_INIT_CAP;
        }

        while (count > *capacity) {
            *capacity *= 2;
        }

        *data = realloc(*data, *capacity * size);
        assert(*data);
    }
}

// Hash Table
#define HT_EMPTY     0
#define HT_OCCUPIED  1
#define HT_TOMBSTONE 2

#define HT_LOAD     0.75
#define HT_INIT_CAP DA_INIT_CAP

uint64_t ht_hasheq_bytes(const void *a, const void *b, size_t n) {
    if (b) {
        return memcmp(a, b, n) == 0;
    }

    uint64_t hash = 14695981039346656037UL;
    for (size_t i = 0; i < n; i++) {
        hash ^= ((uint8_t *) a)[i];
        hash *= 1099511628211UL;
    }
    return hash;
}

uint64_t ht_hasheq_cstr(const void *va, const void *vb, size_t n) {
    unused(n);

    const char *a = *(const char **) va;
    if (vb) {
        return strcmp(a, *(const char **) vb) == 0;
    }

    uint64_t hash = 14695981039346656037UL;
    for (const char *p = a; *p; p++) {
        hash ^= *p;
        hash *= 1099511628211UL;
    }
    return hash;
}

void *ht_find_impl(void *data, size_t capacity, HT_Layout layout, HT_Hasheq hasheq, const void *key) {
    if (!hasheq) {
        hasheq = ht_hasheq_bytes;
    }

    const uint64_t start = hasheq(key, NULL, layout.key_size);
    void          *tombstone = NULL;
    for (size_t i = 0; i < capacity; i++) {
        const size_t index = (start + i) & (capacity - 1);
        uint8_t     *entry = (uint8_t *) data + index * layout.entry_size;

        switch (*entry) {
        case HT_EMPTY:
            return tombstone ? tombstone : entry;

        case HT_OCCUPIED:
            if (hasheq(key, entry + layout.key_offset, layout.key_size)) {
                return entry;
            }
            break;

        case HT_TOMBSTONE:
            if (!tombstone) {
                tombstone = entry;
            }
            break;
        }
    }
    return tombstone;
}

uint64_t ht_hasheq_sv(const void *va, const void *vb, size_t n) {
    unused(n);

    const SV a = *(const SV *) va;
    if (vb) {
        return sv_eq(a, *(const SV *) vb);
    }

    return ht_hasheq_bytes(a.data, NULL, a.count);
}

uint64_t ht_hash_combine(uint64_t a, uint64_t b) {
    return a ^ (b + 0x9E3779B97F4A7C15ULL + (a << 6) + (a >> 2));
}

void *ht_get_impl(void *data, size_t capacity, HT_Layout layout, HT_Hasheq hasheq, const void *key) {
    uint8_t *entry = (uint8_t *) ht_find_impl(data, capacity, layout, hasheq, key);
    return (entry && *entry == HT_OCCUPIED) ? entry + layout.value_offset : NULL;
}

void *ht_set_impl(
    void      **ht_data,
    size_t     *ht_count,
    size_t     *ht_capacity,
    HT_Layout   layout,
    HT_Hasheq   hasheq,
    const void *key,
    const void *value) //
{
    if (*ht_count >= *ht_capacity * HT_LOAD) {
        const size_t capacity = *ht_capacity ? *ht_capacity * 2 : HT_INIT_CAP;
        void        *data = calloc(capacity, layout.entry_size);
        for (size_t i = 0; i < *ht_capacity; i++) {
            uint8_t *src = (uint8_t *) *ht_data + i * layout.entry_size;
            if (*src == HT_OCCUPIED) {
                uint8_t *dst = (uint8_t *) ht_find_impl(data, capacity, layout, hasheq, src + layout.key_offset);
                assert(dst);

                *dst = HT_OCCUPIED;
                memcpy(dst + layout.key_offset, src + layout.key_offset, layout.key_size);
                memcpy(dst + layout.value_offset, src + layout.value_offset, layout.value_size);
            }
        }

        free(*ht_data);
        *ht_data = data;
        *ht_capacity = capacity;
    }

    uint8_t *dst = (uint8_t *) ht_find_impl(*ht_data, *ht_capacity, layout, hasheq, key);
    assert(dst);

    if (*dst != HT_OCCUPIED) {
        *dst = HT_OCCUPIED;
        memcpy(dst + layout.key_offset, key, layout.key_size);
        (*ht_count)++;
    }
    memcpy(dst + layout.value_offset, value, layout.value_size);
    return dst + layout.value_offset;
}

void ht_delete_impl(void *data, size_t *count, size_t capacity, HT_Layout layout, HT_Hasheq hasheq, const void *key) {
    uint8_t *entry = (uint8_t *) ht_find_impl(data, capacity, layout, hasheq, key);
    if (entry && *entry == HT_OCCUPIED) {
        *entry = HT_TOMBSTONE;
        (*count)--;
    }
}

bool ht_iter_impl(
    void *data, size_t capacity, HT_Layout layout, bool *started, size_t *index, void **key, void **value) {
    if (*started) {
        (*index)++;
    } else {
        *started = true;
    }

    for (size_t i = *index; i < capacity; i++) {
        uint8_t *entry = (uint8_t *) data + i * layout.entry_size;
        if (*entry == HT_OCCUPIED) {
            *index = i;
            *key = entry + layout.key_offset;
            *value = entry + layout.value_offset;
            return true;
        }
    }

    return false;
}

// Characters
bool is_space(char ch) {
    return isspace(ch);
}

void print_char_safe(FILE *f, char ch) {
    const uint8_t c = ch;
    if ((c >= 32 && c < 127) || (is_space(c) && c != 0xB)) {
        fputc(c, f);
    } else if (c < 32) {
        fputc('^', f);
        fputc(c + '@', f);
    } else if (c == 127) {
        fputs("^?", f);
    } else {
        fprintf(f, "<%02x>", c);
    }
}

// String View
SV sv_from_cstr(const char *cstr) {
    return (SV) {.data = cstr, .count = strlen(cstr)};
}

SV sv_strip_suffix(SV a, SV b) {
    if (b.count) {
        while (sv_has_suffix(a, b)) {
            a.count -= b.count;
        }
    }
    return a;
}

bool sv_eq(SV a, SV b) {
    return a.count == b.count && memcmp(a.data, b.data, b.count) == 0;
}

int sv_cmp(SV a, SV b) {
    const int diff = memcmp(a.data, b.data, min(a.count, b.count));
    if (diff) {
        return diff;
    }
    return (a.count > b.count) - (a.count < b.count);
}

bool sv_match(SV a, const char *b) {
    return a.count == strlen(b) && memcmp(b, a.data, a.count) == 0;
}

bool sv_has_prefix(SV a, SV b) {
    if (b.count == 0) {
        return true;
    }

    return a.count >= b.count && memcmp(a.data, b.data, b.count) == 0;
}

bool sv_has_suffix(SV a, SV b) {
    if (b.count == 0) {
        return true;
    }

    return a.count >= b.count && memcmp(&a.data[a.count - b.count], b.data, b.count) == 0;
}

bool sv_find(SV s, char ch, size_t *index) {
    const char *p = memchr(s.data, ch, s.count);
    if (!p) {
        return false;
    }

    if (index) {
        *index = p - s.data;
    }
    return true;
}

SV sv_trim(SV s, char ch) {
    while (s.count && *s.data == ch) {
        s.data++;
        s.count--;
    }

    while (s.count && s.data[s.count - 1] == ch) {
        s.count--;
    }

    return s;
}

SV sv_drop(SV s, size_t count) {
    return sv_drop_mut(&s, count);
}

SV sv_drop_mut(SV *s, size_t count) {
    count = min(count, s->count);
    const SV result = (SV) {.data = s->data, .count = count};
    s->data += count;
    s->count -= count;
    return result;
}

SV sv_drop_while_mut(SV *s, bool (*f)(char ch)) {
    SV result = *s;
    while (s->count) {
        if (!f(*s->data)) {
            break;
        }

        s->data++;
        s->count--;
    }

    result.count -= s->count;
    return result;
}

SV sv_split(SV s, char ch) {
    return sv_split_mut(&s, ch);
}

SV sv_split_mut(SV *s, char ch) {
    const char *p = memchr(s->data, ch, s->count);
    if (!p) {
        const SV result = *s;
        s->data += s->count;
        s->count = 0;
        return result;
    }

    const SV result = (SV) {.data = s->data, .count = p - s->data};
    s->data = p + 1;
    s->count -= result.count + 1;
    return result;
}

SV sv_split_by(SV s, bool (*f)(char ch)) {
    return sv_split_by_mut(&s, f);
}

SV sv_split_by_mut(SV *s, bool (*f)(char ch)) {
    for (size_t i = 0; i < s->count; i++) {
        if (f(s->data[i])) {
            const SV result = (SV) {.data = s->data, .count = i};
            s->data += i + 1;
            s->count -= i + 1;
            return result;
        }
    }

    const SV result = *s;
    s->data += s->count;
    s->count = 0;
    return result;
}

// String Builder
void sb_sprintf(SB *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    sb_grow(sb, sb->count + n + 1);

    va_start(args, fmt);
    vsnprintf(sb->data + sb->count, n + 1, fmt, args);
    sb->count += n;
    va_end(args);
}

void sb_push_sv(SB *sb, SV sv) {
    sb_push_many(sb, sv.data, sv.count);
}

void sb_push_cstr(SB *sb, const char *cstr) {
    sb_push_many(sb, cstr, strlen(cstr));
}

SB default_sb;

// Arena Allocator
void arena_free(Arena *a) {
    if (!a->data) {
        return;
    }

#ifdef PLATFORM_X86_64_WINDOWS
    VirtualFree(a->data, 0, MEM_RELEASE);
#else
    munmap(a->data, a->capacity);
#endif // PLATFORM_X86_64_WINDOWS

    memset(a, 0, sizeof(*a));
}

void arena_reset(Arena *a, const void *ptr) {
    assert((const char *) ptr >= a->data && (const char *) ptr <= a->data + a->head);
    a->head = (const char *) ptr - a->data;
    a->head = (a->head + 7) & -8; // Alignment
}

void arena_reset_noalign(Arena *a, const void *ptr) {
    assert((const char *) ptr >= a->data && (const char *) ptr <= a->data + a->head);
    a->head = (const char *) ptr - a->data;
}

void *arena_alloc(Arena *a, size_t size) {
    if (!a->data) {
        if (!a->capacity) {
            a->capacity = 256 * 1024 * 1024;
        }

#ifdef PLATFORM_X86_64_WINDOWS
        a->data = VirtualAlloc(NULL, a->capacity, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
        a->data = mmap(NULL, a->capacity, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (a->data == MAP_FAILED) {
            a->data = NULL;
        }
#endif // PLATFORM_X86_64_WINDOWS

        assert(a->data != NULL);
    }

    void *ptr = a->data + a->head;
    if (size == 0) {
        return ptr;
    }

    size = (size + 7) & -8; // Alignment
    assert(a->head + size <= a->capacity);

    a->head += size;
    return memset(ptr, 0, size);
}

void *arena_clone(Arena *a, const void *data, size_t size) {
    return memcpy(arena_alloc(a, size), data, size);
}

char *arena_sprintf(Arena *a, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    char *result = arena_alloc(a, n + 1);

    va_start(args, fmt);
    vsnprintf(result, n + 1, fmt, args);
    va_end(args);

    return result;
}

char *arena_sv_to_cstr(Arena *a, SV sv) {
    char *p = memcpy(arena_alloc(a, sv.count + 1), sv.data, sv.count);
    p[sv.count] = '\0';
    return p;
}

char *arena_sb_to_cstr(Arena *a, SB *sb, size_t start) {
    char *p = arena_sv_to_cstr(a, (SV) {.data = sb->data + start, .count = sb->count - start});
    sb->count = start;
    return p;
}

SV arena_sb_to_sv(Arena *a, SB *sb, size_t start) {
    SV sv = {.data = sb->data + start, .count = sb->count - start};
    sv.data = arena_clone(a, sv.data, sv.count);
    sb->count = start;
    return sv;
}

Arena temp_arena = {.capacity = 16 * 1024 * 1024};
Arena default_arena;

// FS
bool read_fp(FILE *f, SV *out, Arena *a) {
    bool result = true;

    char  *start = arena_alloc(a, 0);
    size_t count = 0;

    if (!f) {
        return_defer(false);
    }

    while (true) {
#define CHUNK_SIZE 4096
        arena_alloc(a, CHUNK_SIZE);
        const size_t n = fread(start + count, sizeof(*start), CHUNK_SIZE, f);
        count += n;

        if (n < CHUNK_SIZE) {
            if (feof(f)) {
                break;
            }

            if (ferror(f)) {
                return_defer(false);
            }
        }
#undef CHUNK_SIZE
    }

    size_t j = 0;
    for (size_t i = 0; i < count; i++) {
        char it = start[i];
        if (it == '\r' && i + 1 < count && start[i + 1] == '\n') {
            it = start[++i];
        }
        start[j++] = it;
    }

    count = j;
    arena_reset(a, start + count);

    if (count % 8 == 0) {
        // If it matches the alignment, then a further NULL byte is needed. Otherwise the alignment provides the NULL
        // terminator already.
        arena_alloc(a, 1);
    }

    out->data = start;
    out->count = count;

defer:
    if (!result) {
        arena_reset(a, start);
    }
    return result;
}

bool read_file(const char *path, SV *out, Arena *a) {
    bool result = true;

    FILE *f = fopen(path, "r");
    if (!f) {
        return_defer(false);
    }

    if (!read_fp(f, out, a)) {
        return_defer(false);
    }

defer:
    if (f) {
        fclose(f);
    }

    return result;
}

bool delete_file(const char *path) {
#ifdef PLATFORM_X86_64_WINDOWS
    return DeleteFileA(path);
#else
    return unlink(path) == 0;
#endif // PLATFORM_X86_64_WINDOWS
}

bool create_directory(const char *path) {
#ifdef PLATFORM_X86_64_WINDOWS
    const int result = _mkdir(path);
#else
    const int result = mkdir(path, 0755);
#endif // PLATFORM_X86_64_WINDOWS

    return result >= 0 || errno == EEXIST;
}

bool file_exists(const char *path) {
#ifdef PLATFORM_X86_64_WINDOWS
    return _access(path, 0) == 0;
#else
    return access(path, F_OK) == 0;
#endif // PLATFORM_X86_64_WINDOWS
}

bool directory_exists(const char *path) {
    struct stat info;
    if (stat(path, &info) != 0) {
        return false;
    }
    return (info.st_mode & S_IFDIR) != 0;
}

size_t get_modified_time(const char *path) {
#ifdef PLATFORM_X86_64_WINDOWS
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data)) {
        return 0;
    }

    ULARGE_INTEGER ft;
    ft.LowPart = data.ftLastWriteTime.dwLowDateTime;
    ft.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return ft.QuadPart;

#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }

    return st.st_mtime;
#endif // PLATFORM_X86_64_WINDOWS
}

bool is_cmd_available_in_path(const char *cmd) {
#ifdef PLATFORM_X86_64_WINDOWS
#define X_OK        0
#define PATH_DELIM  ';'
#define PATH_FORMAT SV_Fmt "\\%s.exe"
#else
#define PATH_DELIM  ':'
#define PATH_FORMAT SV_Fmt "/%s"
#endif // PLATFORM_X86_64_WINDOWS

    SV sv = sv_from_cstr(getenv("PATH"));
    while (sv.count) {
        SV dir = sv_split_mut(&sv, PATH_DELIM);
        if (!dir.count) {
            continue;
        }

        const char *path = arena_sprintf(&temp_arena, PATH_FORMAT, SV_Arg(dir), cmd);
        if (!access(path, X_OK)) {
            arena_reset(&temp_arena, path);
            return true;
        }
        arena_reset(&temp_arena, path);
    }

    return false;
}

bool is_lld_available_in_path(void) {
#ifdef PLATFORM_X86_64_WINDOWS
    return is_cmd_available_in_path("lld-link");
#endif // PLATFORM_X86_64_WINDOWS

#ifdef PLATFORM_X86_64_LINUX
    return is_cmd_available_in_path("ld.lld");
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    // The C compiler in macOS is a custom version of clang which does not fare well with lld.
    return false;
#endif // PLATFORM_ARM64_MACOS
}

const char *temp_replace_suffix(const char *path, const char *old, const char *new) {
    const SV base = sv_strip_suffix(sv_from_cstr(path), sv_from_cstr(old));
    return arena_sprintf(&temp_arena, SV_Fmt "%s", SV_Arg(base), new);
}

DA(const char *) temporary_files;

void temporary_files_push(const char *path) {
    da_push(&temporary_files, path);
}

void temporary_files_cleanup(void) {
    for (size_t i = 0; i < temporary_files.count; i++) {
        delete_file(temporary_files.data[i]);
    }
    da_free(&temporary_files);
}

// Paths
static bool path_is_rooted(SV path) {
#ifdef PLATFORM_X86_64_WINDOWS
    if (path.count >= 2 && isalpha(path.data[0]) && path.data[1] == ':') {
        return true;
    }
#endif // PLATFORM_X86_64_WINDOWS

    return path.count >= 1 && path.data[0] == '/';
}

const char *get_cwd(Arena *a) {
    const char  *result = NULL;
    const size_t start = default_sb.count;

#ifdef PLATFORM_X86_64_WINDOWS
    const DWORD count = GetCurrentDirectory(0, NULL);
    if (!count) {
        return_defer(NULL);
    }

    sb_grow(&default_sb, default_sb.count + count);
    if (!GetCurrentDirectory(count, default_sb.data + start)) {
        return_defer(NULL);
    }

    unixify_path_separators_inplace(default_sb.data + start, count);
#else
    sb_grow(&default_sb, default_sb.count + DA_INIT_CAP);
    while (!getcwd(default_sb.data + start, default_sb.capacity)) {
        if (errno != ERANGE) {
            return_defer(NULL);
        }

        default_sb.count = default_sb.capacity;
        sb_grow(&default_sb, default_sb.count + DA_INIT_CAP);
    }
#endif // PLATFORM_X86_64_WINDOWS

    return_defer(arena_sprintf(a, "%s", default_sb.data + start));

defer:
    default_sb.count = start;
    return result;
}

static bool is_path_separator(char ch) {
#ifdef PLATFORM_X86_64_WINDOWS
    return ch == '/' || ch == '\\';
#else
    return ch == '/';
#endif // PLATFORM_X86_64_WINDOWS
}

// `cwd` must be absolute
const char *get_absolute_path(SV cwd, SV path, Arena *a) {
    const size_t start = default_sb.count;

    if (path_is_rooted(path)) {
#ifdef PLATFORM_X86_64_WINDOWS
        if (path.count && path.data[0] == '/') {
            assert(cwd.count >= 2 && isalpha(cwd.data[0]) && cwd.data[1] == ':');
            sb_push_many(&default_sb, cwd.data, 2);
        }
#endif // PLATFORM_X86_64_WINDOWS
    } else {
        sb_push_many(&default_sb, cwd.data, cwd.count);
        if (default_sb.count > start && default_sb.data[default_sb.count - 1] == '/') {
            default_sb.count--;
        }
    }

    while (path.count) {
        SV component = sv_split_by_mut(&path, is_path_separator);
        if (!component.count) {
            continue;
        }

        if (sv_match(component, ".")) {
            continue;
        }

        if (sv_match(component, "..")) {
            for (size_t i = default_sb.count; i > start; i--) {
                if (default_sb.data[i - 1] == '/') {
                    default_sb.count = i - 1;
                    break;
                }
            }

            continue;
        }

        bool push_slash = true;

#ifdef PLATFORM_X86_64_WINDOWS
        if (default_sb.count == start) {
            assert(path_is_rooted(component));
            push_slash = false;
        }
#endif // PLATFORM_X86_64_WINDOWS

        if (push_slash) {
            sb_push(&default_sb, '/');
        }
        sb_push_many(&default_sb, component.data, component.count);
    }

#ifdef PLATFORM_X86_64_WINDOWS
    assert(path_is_rooted((SV) {.data = default_sb.data + start, .count = default_sb.count - start}));
    if (default_sb.count == start + 2) {
        sb_push(&default_sb, '/');
    }
#else
    if (default_sb.count == start) {
        sb_push(&default_sb, '/');
    }
#endif // PLATFORM_X86_64_WINDOWS

    return arena_sb_to_cstr(a, &default_sb, start);
}

// `cwd` and `path` must be absolute
const char *get_relative_path(SV cwd, SV path, Arena *a) {
    const size_t start = default_sb.count;
    while (cwd.count && path.count) {
        const SV cwd_component = sv_split(cwd, '/');
        const SV path_component = sv_split(path, '/');
        if (!sv_eq(cwd_component, path_component)) {
            break;
        }

        sv_drop_mut(&cwd, cwd_component.count + 1);
        sv_drop_mut(&path, path_component.count + 1);
    }

    while (cwd.count) {
        sv_split_mut(&cwd, '/');
        if (default_sb.count != start) {
            sb_push(&default_sb, '/');
        }
        sb_push_cstr(&default_sb, "..");
    }

    if (path.count) {
        if (default_sb.count != start) {
            sb_push(&default_sb, '/');
        }
        sb_push_many(&default_sb, path.data, path.count);
    }

    if (default_sb.count == start) {
        sb_push(&default_sb, '.');
    }

    return arena_sb_to_cstr(a, &default_sb, start);
}

// `path` must be absolute
const char *get_parent_dir_path(const char *path, Arena *a) {
    SV sv = sv_from_cstr(path);
    if (sv.count && sv.data[sv.count - 1] == '/') {
        // The way everything is designed, this means that `path` is root
        return arena_sv_to_cstr(a, sv);
    }

    for (size_t i = sv.count; i > 0; i--) {
        if (sv.data[i - 1] == '/') {
            sv.count = i - 1;

#ifdef PLATFORM_X86_64_WINDOWS
            if (sv.count < 3) {
                sv.count++;
            }
#else
            if (sv.count < 1) {
                sv.count++;
            }
#endif // PLATFORM_X86_64_WINDOWS

            break;
        }
    }

    return arena_sv_to_cstr(a, sv);
}

#ifdef PLATFORM_X86_64_WINDOWS
void unixify_path_separators_inplace(char *data, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (data[i] == '\\') {
            data[i] = '/';
        }
    }
}
#endif // PLATFORM_X86_64_WINDOWS

// Processes
void cmd_show(Cmd cmd, FILE *f) {
    // TODO: Escaping
    fprintf(f, "$");
    for (size_t i = 0; i < cmd.count; i++) {
        fprintf(f, " %s", cmd.data[i]);
    }
    fprintf(f, "\n");
}

Proc cmd_run_async(Cmd *c, Cmd_Stdio stdio) {
#ifdef PLATFORM_X86_64_WINDOWS
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(STARTUPINFO);

    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = NULL;

    HANDLE stdout_pipe_read = NULL, stdout_pipe_write = NULL;
    if (stdio.out) {
        if (!CreatePipe(&stdout_pipe_read, &stdout_pipe_write, &sa, 0)) {
            return (Proc) {.id = PROC_INVALID};
        }
        SetHandleInformation(stdout_pipe_read, HANDLE_FLAG_INHERIT, 0);
    } else {
        stdout_pipe_write = GetStdHandle(STD_OUTPUT_HANDLE);
    }

    HANDLE stderr_pipe_read = NULL, stderr_pipe_write = NULL;
    if (stdio.err) {
        if (!CreatePipe(&stderr_pipe_read, &stderr_pipe_write, &sa, 0)) {
            return (Proc) {.id = PROC_INVALID};
        }
        SetHandleInformation(stderr_pipe_read, HANDLE_FLAG_INHERIT, 0);
    } else {
        stderr_pipe_write = GetStdHandle(STD_ERROR_HANDLE);
    }

    HANDLE stdin_pipe_read = NULL, stdin_pipe_write = NULL;
    if (stdio.in) {
        if (!CreatePipe(&stdin_pipe_read, &stdin_pipe_write, &sa, 0)) {
            return (Proc) {.id = PROC_INVALID};
        }
        SetHandleInformation(stdin_pipe_write, HANDLE_FLAG_INHERIT, 0);
    } else {
        stdin_pipe_read = GetStdHandle(STD_INPUT_HANDLE);
    }

    si.hStdOutput = stdout_pipe_write;
    si.hStdError = stderr_pipe_write;
    si.hStdInput = stdin_pipe_read;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION piProcInfo;
    ZeroMemory(&piProcInfo, sizeof(PROCESS_INFORMATION));

    SB sb = {0};
    for (size_t i = 0; i < c->count; i++) {
        if (i != 0) {
            da_push(&sb, ' ');
        }

        const SV   it = sv_from_cstr(c->data[i]);
        const bool need_quoting =
            it.count == 0 || sv_find(it, '\t', NULL) || sv_find(it, '\v', NULL) || sv_find(it, ' ', NULL);

        if (need_quoting) {
            da_push(&sb, '"');
        }

        for (size_t j = 0; j < it.count; j++) {
            switch (it.data[j]) {
            default:
                break;

            case '\\':
                if (j + 1 < it.count && it.data[j + 1] == '"') {
                    da_push(&sb, '\\');
                }
                break;

            case '"':
                da_push(&sb, '\\');
                break;
            }

            if (!i && it.data[j] == '/') {
                da_push(&sb, '\\');
            } else {
                da_push(&sb, it.data[j]);
            }
        }

        if (need_quoting) {
            da_push(&sb, '"');
        }
    }

    c->count = 0;
    da_push(&sb, '\0');

    if (!CreateProcessA(NULL, sb.data, NULL, NULL, TRUE, 0, NULL, NULL, &si, &piProcInfo)) {
        return (Proc) {.id = PROC_INVALID};
    }

    sb_free(&sb);
    CloseHandle(piProcInfo.hThread);

    if (stdio.out) {
        CloseHandle(stdout_pipe_write);
        *stdio.out = _fdopen(_open_osfhandle((intptr_t) stdout_pipe_read, _O_RDONLY), "r");
    }

    if (stdio.err) {
        CloseHandle(stderr_pipe_write);
        *stdio.err = _fdopen(_open_osfhandle((intptr_t) stderr_pipe_read, _O_RDONLY), "r");
    }

    if (stdio.in) {
        CloseHandle(stdin_pipe_read);
        *stdio.in = _fdopen(_open_osfhandle((intptr_t) stdin_pipe_write, _O_WRONLY), "w");
    }

    return (Proc) {
        .id = piProcInfo.hProcess,
        .in = (stdio.in) ? *stdio.in : NULL,
        .out = (stdio.out) ? *stdio.out : NULL,
        .err = (stdio.err) ? *stdio.err : NULL,
    };
#else
    int in[2] = {0};
    int out[2] = {0};
    int err[2] = {0};
    int fail[2] = {0};
    if (pipe(fail) < 0 || fcntl(fail[1], F_SETFD, FD_CLOEXEC) < 0) {
        return (Proc) {.id = PROC_INVALID};
    }

    if (stdio.in && pipe(in) < 0) {
        return (Proc) {.id = PROC_INVALID};
    }

    if (stdio.out && pipe(out) < 0) {
        return (Proc) {.id = PROC_INVALID};
    }

    if (stdio.err && pipe(err) < 0) {
        return (Proc) {.id = PROC_INVALID};
    }

    int id = fork();
    if (id < 0) {
        return (Proc) {.id = PROC_INVALID};
    }

    if (!id) {
        if (stdio.in) {
            close(in[1]);
            dup2(in[0], STDIN_FILENO);
            close(in[0]);
        }

        if (stdio.out) {
            close(out[0]);
            dup2(out[1], STDOUT_FILENO);
            close(out[1]);
        }

        if (stdio.err) {
            close(err[0]);
            dup2(err[1], STDERR_FILENO);
            close(err[1]);
        }

        da_push(c, NULL);

        close(fail[0]);
        execvp(*c->data, (char *const *) c->data);
        write(fail[1], "E", 1);
        close(fail[1]);
        exit(127);
    }

    c->count = 0;

    close(fail[1]);
    char       buffer[1];
    const long count = read(fail[0], buffer, sizeof(buffer));
    close(fail[0]);

    if (count > 0) {
        waitpid(id, NULL, 0); // Wait for the child to kill itself so it doesn't become a zombie
        return (Proc) {.id = PROC_INVALID};
    }

    if (stdio.in) {
        close(in[0]);
        *stdio.in = fdopen(in[1], "w");
        if (!*stdio.in) {
            close(in[1]);
        }
    }

    if (stdio.out) {
        close(out[1]);
        *stdio.out = fdopen(out[0], "r");
        if (!*stdio.out) {
            close(out[0]);
        }
    }

    if (stdio.err) {
        close(err[1]);
        *stdio.err = fdopen(err[0], "r");
        if (!*stdio.err) {
            close(err[0]);
        }
    }

    return (Proc) {
        .id = id,
        .in = (stdio.in) ? *stdio.in : NULL,
        .out = (stdio.out) ? *stdio.out : NULL,
        .err = (stdio.err) ? *stdio.err : NULL,
    };
#endif // PLATFORM_X86_64_WINDOWS
}

int cmd_wait(Proc proc) {
#ifdef PLATFORM_X86_64_WINDOWS
    if (proc.id == PROC_INVALID) {
        return 1;
    }

    // TODO: Flush the files if any, otherwise it deadlocks on windows
    if (WaitForSingleObject(proc.id, INFINITE) == WAIT_FAILED) {
        return 1;
    }

    DWORD exit_code;
    if (!GetExitCodeProcess(proc.id, &exit_code)) {
        return 1;
    }

    CloseHandle(proc.id);

    switch (exit_code) {
    case 0x40000015:
    case 0xC0000409:
        exit_code = 134;
        break;
    }
    return exit_code;
#else
    if (proc.id == PROC_INVALID) {
        return 1;
    }

    int status = 0;
    if (waitpid(proc.id, &status, 0) < 0) {
        return 1;
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return WEXITSTATUS(status);
#endif // PLATFORM_X86_64_WINDOWS
}

int cmd_run_sync(Cmd *c, Cmd_Stdio stdio) {
    return cmd_wait(cmd_run_async(c, stdio));
}

bool procs_push(Procs *ps, Proc p) {
    if (p.id == PROC_INVALID) {
        return false;
    }

    da_push(ps, p);
    if (ps->count <= ps->nprocs) {
        return true;
    }

    return procs_flush(ps);
}

bool procs_flush(Procs *ps) {
    bool ok = true;
    for (size_t i = 0; i < ps->count; i++) {
        const Proc it = ps->data[i];
        if (ps->callback_before_wait) {
            ps->callback_before_wait(it);
        }

        const int code = cmd_wait(it);
        if (code != 0) {
            ok = false;
        }
    }

    ps->count = 0;
    return ok;
}

// Profiling
#ifdef PROFILING
clock_t global_profiler;
#endif // PROFILING
