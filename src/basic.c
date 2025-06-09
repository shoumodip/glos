#include <stdarg.h>
#include <sys/wait.h>
#include <unistd.h>

#include "basic.h"

#define return_defer(value)                                                                                            \
    do {                                                                                                               \
        result = (value);                                                                                              \
        goto defer;                                                                                                    \
    } while (0)

// String View
bool sv_eq(SV a, SV b) {
    return a.count == b.count && memcmp(&a.data[a.count - b.count], b.data, b.count) == 0;
}

bool sv_match(SV a, const char *b) {
    return a.count == strlen(b) && memcmp(b, a.data, a.count) == 0;
}

bool sv_has_suffix(SV a, SV b) {
    return a.count >= b.count && memcmp(&a.data[a.count - b.count], b.data, b.count) == 0;
}

SV sv_from_cstr(const char *cstr) {
    return (SV) {.data = cstr, .count = strlen(cstr)};
}

SV sv_strip_suffix(SV a, SV b) {
    while (sv_has_suffix(a, b)) {
        a.count -= b.count;
    }
    return a;
}

// String Builder
void sb_sprintf(SB *sb, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    da_grow(sb, n + 1);

    va_start(args, fmt);
    vsnprintf(&sb->data[sb->count], n + 1, fmt, args);
    sb->count += n;
    va_end(args);
}

// Temporary Allocator
static char   temp_data[16 * 1000 * 1000];
static size_t temp_count;

void *temp_alloc(size_t n) {
    assert(temp_count + n <= len(temp_data));
    char *result = &temp_data[temp_count];
    temp_count += n;
    return result;
}

char *temp_sprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    const int n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    assert(n >= 0);
    char *result = temp_alloc(n + 1);

    va_start(args, fmt);
    vsnprintf(result, n + 1, fmt, args);
    va_end(args);

    return result;
}

char *temp_sv_to_cstr(SV sv) {
    char *p = memcpy(temp_alloc(sv.count + 1), sv.data, sv.count);
    p[sv.count] = '\0';
    return p;
}

// Arena Allocator
#define ARENA_MINIMUM_CAPACITY 16000

struct ArenaRegion {
    ArenaRegion *next;
    size_t       count;
    size_t       capacity;
    char         data[];
};

void arena_free(Arena *a) {
    ArenaRegion *it = a->head;
    while (it) {
        ArenaRegion *next = it->next;
        free(it);
        it = next;
    }
}

void *arena_alloc(Arena *a, size_t size) {
    ArenaRegion *region = NULL;
    for (ArenaRegion *it = a->head; it; it = it->next) {
        if (it->count + size <= it->capacity) {
            region = it;
            break;
        }
    }

    size = (size + 7) & -8; // Alignment
    if (!region) {
        size_t capacity = size;
        if (capacity < ARENA_MINIMUM_CAPACITY) {
            capacity = ARENA_MINIMUM_CAPACITY;
        }

        region = malloc(sizeof(ArenaRegion) + capacity);
        region->next = a->head;
        region->count = 0;
        region->capacity = capacity;
        a->head = region;
    }

    void *ptr = &region->data[region->count];
    memset(ptr, 0, size);
    region->count += size;
    return ptr;
}

// OS
bool read_file(SV *out, const char *path) {
    char *data = NULL;
    bool  result = true;

    FILE *f = fopen(path, "r");
    if (!f) {
        return_defer(false);
    }

    if (fseek(f, 0, SEEK_END)) {
        return_defer(false);
    }

    const long count = ftell(f);
    if (count < 0) {
        return_defer(false);
    }
    rewind(f);

    data = malloc(count);
    if (!data) {
        return_defer(false);
    }

    if (fread(data, 1, count, f) != (size_t) count) {
        return_defer(false);
    }

    out->data = data;
    out->count = count;

defer:
    if (f) {
        fclose(f);
    }

    if (!result) {
        free(data);
    }

    return result;
}

Proc cmd_run_async(Cmd *c, CmdStdio stdio) {
    int in[2] = {-1, -1};
    int out[2] = {-1, -1};
    int err[2] = {-1, -1};

    if (stdio.in && pipe(in) < 0) {
        goto fail;
    }

    if (stdio.out && pipe(out) < 0) {
        goto fail;
    }

    if (stdio.err && pipe(err) < 0) {
        goto fail;
    }

    Proc proc = fork();
    if (proc < 0) {
        goto fail;
    }

    if (!proc) {
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
        execvp(*c->data, (char *const *) c->data);
        exit(127);
    }

    c->count = 0;

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

    return proc;

fail:
    if (in[0] != -1) {
        close(in[0]);
    }

    if (in[1] != -1) {
        close(in[1]);
    }

    if (out[0] != -1) {
        close(out[0]);
    }

    if (out[1] != -1) {
        close(out[1]);
    }

    if (err[0] != -1) {
        close(err[0]);
    }

    if (err[1] != -1) {
        close(err[1]);
    }

    return -1;
}

int cmd_run_sync(Cmd *c, CmdStdio stdio) {
    return cmd_wait(cmd_run_async(c, stdio));
}

int cmd_wait(Proc proc) {
    if (proc == -1) {
        return 1;
    }

    int status = 0;
    if (waitpid(proc, &status, 0) < 0) {
        return 1;
    }

    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }

    return WEXITSTATUS(status);
}
