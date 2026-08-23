#include "src/basic.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef PLATFORM_X86_64_WINDOWS
#define OBJ_FILE_EXTENSION ".obj"
#define EXE_FILE_EXTENSION ".exe"
#else
#define OBJ_FILE_EXTENSION ".o"
#define EXE_FILE_EXTENSION ""
#endif // PLATFORM_X86_64_WINDOWS

// 64MB
#define COMPILER_STACK_SIZE (64 * 1024 * 1024)

#if defined(PLATFORM_X86_64_LINUX) && !(defined(__GLIBC__) || defined(__UCLIBC__))
#define MUSL
#endif

#define TESTS_LIST_PATH "tests/tests.conf"

static void error(const char *fmt, ...) Printf_Like(1);
static void error_at(const char *path, size_t row, size_t col, const char *fmt, ...) Printf_Like(4);

static void error(const char *fmt, ...) {
    afprintf(stderr, ANSI_COLOR_RED | ANSI_BOLD, "ERROR:");
    fprintf(stderr, " ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void note(const char *fmt, ...) {
    afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "NOTE:");
    fprintf(stderr, " ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void error_at(const char *path, size_t row, size_t col, const char *fmt, ...) {
    afprintf(stderr, ANSI_BOLD | ANSI_UNDERLINE, "%s:%zu:%zu:", path, row, col);
    fprintf(stderr, " ");
    afprintf(stderr, ANSI_COLOR_RED | ANSI_BOLD, "ERROR:");
    fprintf(stderr, " ");
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void usage(FILE *f, const char *program) {
    afprintf(f, ANSI_COLOR_CYAN | ANSI_BOLD, "Usage:\n");
    afprintf(f, ANSI_COLOR_GREEN | ANSI_BOLD, "    %s", program);
    fprintf(
        f,
        " [FLAGS...]\n"
        "\n");

    afprintf(f, ANSI_COLOR_CYAN | ANSI_BOLD, "Flags:\n");

    // clang-format off
    static const struct {
        const char *flag;
        const char *desc;
    } flags[] = {
        {"h", "             Show this message"},
        {"t", "             Run tests"},
        {"T", "             Run tests in non-interactive mode"},
        {"O", "LEVEL        Run tests with this optimization level [0: None, 1: Less (Default), 2: Medium, 3: Aggressive]"},
        {"j", "NPROCS       Set the maximum number of parallel processes. Default is 5"},
    };
    // clang-format on

    for (size_t i = 0; i < len(flags); i++) {
        afprintf(f, ANSI_COLOR_MAGENTA, "    -%s", flags[i].flag);
        afprintf(f, ANSI_COLOR_DEFAULT, " %s\n", flags[i].desc);
    }
}

static const char *shift(int *argc, char ***argv, const char *program, const char *expected) {
    if (*argc <= 0) {
        error("%s not provided\n", expected);
        usage(stderr, program);
        exit(1);
    }

    (*argc)--;
    return *(*argv)++;
}

static SV run_cmd_and_read_stdout(Cmd *cmd) {
    const char *name = cmd->data[0];

    FILE *out = NULL;
    Proc  proc = cmd_run_async(cmd, (Cmd_Stdio) {.out = &out});
    if (proc.id == PROC_INVALID) {
        error("Could not execute command '%s'", name);
        exit(1);
    }

    if (!out) {
        error("Could not open standard output of command '%s'", name);
        exit(1);
    }

    SV sv = {0};
    if (!read_fp(out, &sv, &default_arena)) {
        error("Could not read standard output of command '%s'", name);
        exit(1);
    }

    int result = cmd_wait(proc);
    if (result) {
        error("Command '%s' exited abnormally with code %d", name, result);
        exit(1);
    }

    fclose(out);
    return sv;
}

#ifdef PLATFORM_X86_64_WINDOWS
static void filter_cl_exe_output(Proc proc) {
    SV sv = {0};
    if (!read_fp(proc.out, &sv, &default_arena)) {
        error("Could not read standard output of 'cl.exe'");
        exit(1);
    }
    fclose(proc.out);

    while (sv.count) {
        const SV line = sv_split_mut(&sv, '\n');
        if (sv_find(line, ' ', NULL)) {
            printf(SV_Fmt "\n", SV_Arg(line));
        }
    }
    arena_reset(&default_arena, sv.data);
}
#endif // PLATFORM_X86_64_WINDOWS

static void ensure_llvm(Cmd *cmd) {
#ifdef PLATFORM_X86_64_LINUX
#ifdef MUSL
    const char *url = "https://github.com/glos-lang/llvm/releases/download/22.1.4/llvm-linux-musl-x86_64.tar.xz";
#else
    const char *url = "https://github.com/glos-lang/llvm/releases/download/22.1.4/llvm-linux-glibc-x86_64.tar.xz";
#endif // MUSL
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    const char *url = "https://github.com/glos-lang/llvm/releases/download/22.1.4/llvm-macos-arm64.tar.xz";
#endif // PLATFORM_ARM64_MACOS

#ifdef PLATFORM_X86_64_WINDOWS
    const char *url = "https://github.com/glos-lang/llvm/releases/download/22.1.4/llvm-windows-x86_64.tar.xz";
#endif // PLATFORM_X86_64_WINDOWS

    if (directory_exists("llvm")) {
        return;
    }

    const char *llvm_dir_path = "llvm";
    const char *llvm_tar_path = "llvm.tar.xz";

    printf("Downloading '%s'...\n", url);
    fflush(stdout);

    cmd_push(cmd, "curl", "-o", llvm_tar_path, "-L", url);
    Proc proc = cmd_run_async(cmd, (Cmd_Stdio) {0});
    if (proc.id == PROC_INVALID) {
        error("Could not execute 'curl'");
        goto note;
    }

    int code = cmd_wait(proc);
    if (code) {
        error("Command 'curl' exited abnormally with code %d", code);
        goto note;
    }

    if (!create_directory(llvm_dir_path)) {
        error("Could not create directory '%s'", llvm_dir_path);
        goto note;
    }

    printf("Extracting into '%s'...\n", llvm_dir_path);
    fflush(stdout);

    cmd_push(cmd, "tar", "fx", llvm_tar_path, "-C", llvm_dir_path, "--strip-components=1");
    proc = cmd_run_async(cmd, (Cmd_Stdio) {0});
    if (proc.id == PROC_INVALID) {
        error("Could not execute 'tar'");
        goto note;
    }

    code = cmd_wait(proc);
    if (code) {
        error("Command 'tar' exited abnormally with code %d", code);
        goto note;
    }

    delete_file(llvm_tar_path);
    return;

note:
    fprintf(stderr, "NOTE:  Manually download '%s' and extract it into a directory named '%s'\n", url, llvm_dir_path);
    exit(1);
}

static void build_glos(Cmd *cmd, size_t nprocs) {
    ensure_llvm(cmd);

    // Check that the contract is OK
    {
        const char *glos_contract_path = "std/builtin/contract.glos";
        const char *c_contract_path = "src/contract.h";
        if (get_modified_time(glos_contract_path) > get_modified_time(c_contract_path)) {
            fprintf(stderr, "WARNING: The file '%s' was modified after '%s'\n", glos_contract_path, c_contract_path);
        }
    }

    static const char *headers[] = {
        "src/int128.h",
        "src/basic.h",
        "src/token.h",
        "src/node.h",

        "src/error.h",
        "src/lexer.h",
        "src/parser.h",

        "src/context.h",
        "src/checker.h",
        "src/checker/checker.h",

        "src/dwarf.h",
        "src/contract.h",
        "src/compiler.h",
        "src/compiler/compiler.h",
    };

    static const char *sources[] = {
        "src/int128.c",
        "src/basic.c",
        "src/token.c",
        "src/node.c",

        "src/error.c",
        "src/lexer.c",
        "src/parser.c",

        "src/context.c",
        "src/checker/utils.c",
        "src/checker/type_assertions.c",
        "src/checker/const_expr.c",
        "src/checker/definitions.c",
        "src/checker/control_flow.c",
        "src/checker/calls.c",
        "src/checker/methods.c",
        "src/checker/monomorphizer.c",
        "src/checker/expr.c",
        "src/checker/stmt.c",
        "src/checker.c",

        "src/compiler/utils.c",
        "src/compiler/type.c",
        "src/compiler/rtti.c",
        "src/compiler/abi.c",
        "src/compiler/const_value.c",
        "src/compiler/expr.c",
        "src/compiler/stmt.c",
        "src/compiler.c",

        "src/main.c",
    };

    const void *save = arena_alloc(&temp_arena, 0);
    Procs       procs = {.nprocs = nprocs};

    size_t headers_time_latest = 0;
    for (size_t i = 0; i < len(headers); i++) {
        const size_t time = get_modified_time(headers[i]);
        headers_time_latest = max(headers_time_latest, time);
    }

    bool need_linking = get_modified_time("glos" EXE_FILE_EXTENSION) == 0;
    for (size_t i = 0; i < len(sources); i++) {
        const char  *src = sources[i];
        const char  *obj = temp_replace_suffix(src, ".c", OBJ_FILE_EXTENSION);
        const size_t src_time = get_modified_time(src);
        const size_t obj_time = get_modified_time(obj);
        if (obj_time >= src_time && obj_time >= headers_time_latest) {
            continue;
        }

        fprintf(stderr, "Building '%s'\n", obj);
        need_linking = true;

        Cmd_Stdio proc_stdio = {0};

#ifdef PLATFORM_X86_64_WINDOWS
        proc_stdio.out = (FILE **) arena_alloc(&temp_arena, sizeof(FILE *));
        procs.callback_before_wait = filter_cl_exe_output;

        cmd_push(cmd, "cl", "/Z7", "/nologo", "/c");
        cmd_push(cmd, "/I", "./llvm/include");
        cmd_push(cmd, arena_sprintf(&temp_arena, "/Fo:%s", obj));
#else
        cmd_push(cmd, "cc", "-c");
        cmd_push(cmd, "-Wall", "-Wextra", "-Werror");
        cmd_push(cmd, "-ggdb");
        cmd_push(cmd, "-I./llvm/include");
        cmd_push(cmd, "-o", obj);
#endif // PLATFORM_X86_64_WINDOWS

        cmd_push(cmd, src);

        const char *proc_name = cmd->data[0];
        const Proc  proc = cmd_run_async(cmd, proc_stdio);
        if (proc.id == PROC_INVALID) {
            error("Could not start process '%s'", proc_name);
            exit(1);
        }

        if (!procs_push(&procs, proc)) {
            error("C Compiler exited abnormally");
            exit(1);
        }
    }

    if (!procs_flush(&procs)) {
        error("C Compiler exited abnormally");
        exit(1);
    }

    if (need_linking) {
        fprintf(stderr, "Building 'glos" EXE_FILE_EXTENSION "'\n");

        SV sv = {0};
        cmd_push(cmd, "./llvm/bin/llvm-config", "--ldflags", "--libs", "--system-libs", "--link-static");
        sv = run_cmd_and_read_stdout(cmd);

#ifdef PLATFORM_X86_64_LINUX
        cmd_push(cmd, "g++");
        if (is_lld_available_in_path()) {
            cmd_push(cmd, "-fuse-ld=lld");
        }

#ifdef MUSL
        cmd_push(cmd, "-static");
#endif // MUSL

        cmd_push(cmd, arena_sprintf(&temp_arena, "-Wl,-z,stack-size=%u", COMPILER_STACK_SIZE));
        cmd_push(cmd, "-o", "glos" EXE_FILE_EXTENSION);
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_X86_64_WINDOWS
        if (is_lld_available_in_path()) {
            cmd_push(cmd, "lld-link");
        } else {
            cmd_push(cmd, "link", "/nologo");
        }

        cmd_push(cmd, arena_sprintf(&temp_arena, "/stack:%u", COMPILER_STACK_SIZE));
        cmd_push(cmd, "/debug");
        cmd_push(cmd, "/out:glos.exe");
#endif // PLATFORM_X86_64_WINDOWS

#ifdef PLATFORM_ARM64_MACOS
        arena_reset_noalign(&default_arena, sv.data + sv.count);
        cmd_push(cmd, "pkg-config", "--libs-only-L", "zlib", "libzstd");
        sv.count += run_cmd_and_read_stdout(cmd).count;

        cmd_push(cmd, "g++");
        cmd_push(cmd, arena_sprintf(&temp_arena, "-Wl,-stack_size,0x%x", COMPILER_STACK_SIZE));
        cmd_push(cmd, "-o", "glos" EXE_FILE_EXTENSION);
#endif // PLATFORM_ARM64_MACOS

        for (size_t i = 0; i < len(sources); i++) {
            cmd_push(cmd, temp_replace_suffix(sources[i], ".c", OBJ_FILE_EXTENSION));
        }

        while (sv.count) {
            const SV arg = sv_split_by_mut(&sv, is_space);
            if (arg.count == 0) {
                continue;
            }
            cmd_push(cmd, arena_sv_to_cstr(&temp_arena, arg));
        }
        arena_reset(&default_arena, sv.data);

        const char *name = cmd->data[0];
        if (cmd_run_sync(cmd, (Cmd_Stdio) {0})) {
            error("Process '%s' exited abnormally", name);
            exit(1);
        }
    }

    da_free(&procs);
    arena_reset(&temp_arena, save);
}

static char single_char_prompt(FILE *in, FILE *out, const char *choices, const char **descriptions) {
    fprintf(out, " (");
    for (const char *p = choices, **d = descriptions; *p; p++, d++) {
        char it = *p;
        if (p == choices) {
            it = toupper(it);
        } else {
            it = tolower(it);
            fprintf(out, ", ");
        }
        fprintf(out, "%c: %s", it, *d);
    }
    fprintf(out, "): ");
    ansi_reset(out);

    char buffer[16];
    if (fgets(buffer, sizeof(buffer), in) == NULL) {
        return 0;
    }

    const size_t n = strlen(buffer);
    if (n > 0 && buffer[n - 1] == '\n') {
        buffer[n - 1] = '\0';
    }

    const char choice = tolower(*buffer);
    if (!choice) {
        return tolower(*choices);
    }

    if (strchr(choices, choice)) {
        return choice;
    }

    error("Invalid choice '%c'", choice);
    return 0;
}

static void print_lines_with_indent(FILE *f, SV sv, const char *indent) {
    bool not_empty = sv.count != 0;
    if (not_empty) {
        fputs(" {\n", f);
    } else {
        fputs(" {}\n", f);
    }

    while (sv.count) {
        const SV line = sv_split_mut(&sv, '\n');
        fputs(indent, f);
        fwrite(line.data, line.count, 1, f);
        fputc('\n', f);
    }

    if (not_empty) {
        fputs("  }\n", f);
    }
}

static bool parse_uint_from_sv(SV s, size_t *n) {
    if (s.data == NULL || n == NULL || s.count == 0) {
        return false;
    }

    size_t result = 0;
    for (size_t i = 0; i < s.count; ++i) {
        const char it = s.data[i];
        if (!isdigit(it)) {
            return false;
        }

        const int digit = it - '0';
        if (result > (SIZE_MAX - digit) / 10) {
            return false;
        }

        result = result * 10 + digit;
    }

    *n = result;
    return true;
}

static size_t parse_uint_value(SV value, const char *label, const char *path, size_t row, SV line) {
    size_t n = 0;
    if (!parse_uint_from_sv(value, &n)) {
        error_at(path, row, value.data - line.data + 1, "Invalid %s '" SV_Fmt "'", label, SV_Arg(value));
        exit(1);
    }

    return n;
}

static SV parse_bytes_value(SV value, SV *contents, const char *label, const char *path, size_t *row, SV line) {
    SV bytes = {0};

    const char *label_full = arena_sprintf(&temp_arena, "%s byte(s) count", label);
    bytes.count = parse_uint_value(value, label_full, path, (*row)++, line);
    arena_reset(&temp_arena, label_full);

    if (bytes.count >= contents->count) {
        error_at(
            path, *row, 1, "Expected %zu byte(s) and a newline, got %zu byte(s) instead", bytes.count, contents->count);
        exit(1);
    }

    bytes.data = contents->data;
    for (size_t i = 0; i < bytes.count; i++) {
        if (bytes.data[i] == '\n') {
            (*row)++;
        }
    }

    sv_drop_mut(contents, bytes.count + 1);
    (*row)++;

    return bytes;
}

typedef struct {
    int exit;
    SV  out;
    SV  err;
} Test_Info;

static bool test_info_diff(Test_Info expected, Test_Info actual, const char *name) {
    const bool exit_mismatch = expected.exit != actual.exit;

    const bool stdout_mismatch = !sv_eq(expected.out, actual.out);
    const bool stderr_mismatch = !sv_eq(expected.err, actual.err);

    if (exit_mismatch || stdout_mismatch || stderr_mismatch) {
        fprintf(stderr, "\n");
        error("Test case '%s' FAILED", name);

        if (exit_mismatch) {
            fprintf(stderr, "\n");
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "Exit Code:\n");
            afprintf(stderr, ANSI_COLOR_GREEN, "  Expected: %d\n", expected.exit);
            afprintf(stderr, ANSI_COLOR_RED, "  Actual:   %d\n", actual.exit);
        }

        if (stdout_mismatch) {
            fprintf(stderr, "\n");
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "Standard Output:\n");
            ansi_set(stderr, ANSI_COLOR_GREEN);
            fprintf(stderr, "  Expected: %zu byte(s)", expected.out.count);
            print_lines_with_indent(stderr, expected.out, "    ");
            ansi_reset(stderr);

            fprintf(stderr, "\n");
            ansi_set(stderr, ANSI_COLOR_RED);
            fprintf(stderr, "  Actual:   %zu byte(s)", actual.out.count);
            print_lines_with_indent(stderr, actual.out, "    ");
            ansi_reset(stderr);
        }

        if (stderr_mismatch) {
            fprintf(stderr, "\n");
            afprintf(stderr, ANSI_COLOR_YELLOW | ANSI_BOLD, "Standard Error:\n");
            ansi_set(stderr, ANSI_COLOR_GREEN);
            fprintf(stderr, "  Expected: %zu byte(s)", expected.err.count);
            print_lines_with_indent(stderr, expected.err, "    ");
            ansi_reset(stderr);

            fprintf(stderr, "\n");
            ansi_set(stderr, ANSI_COLOR_RED);
            fprintf(stderr, "  Actual:   %zu byte(s)", actual.err.count);
            print_lines_with_indent(stderr, actual.err, "    ");
            ansi_reset(stderr);
        }
        return false;
    } else {
        return true;
    }
}

typedef struct {
    const char *name;

    bool        record_exists;
    const char *record_path;

    Proc  proc;
    FILE *pout;
    FILE *perr;

    Test_Info expected;
} Test;

typedef DA(Test) Tests;

static void test_prepare_cmd(Test test, Cmd *cmd, const char *optimization_level) {
    cmd_push(cmd, "./glos" EXE_FILE_EXTENSION);
    cmd_push(cmd, "-r");
    cmd_push(cmd, test.name);
    if (optimization_level) {
        cmd_push(cmd, optimization_level);
    }
}

static void tests_flush(
    Tests *tests, Cmd *cmd, bool interactive, const char *optimization_level, Arena *arena, const void *arena_save) //
{
    size_t i = 0;
    while (i < tests->count) {
        Test *it = &tests->data[i];

        Test_Info actual = {0};
        if (it->pout) {
            if (!read_fp(it->pout, &actual.out, arena)) {
                error("Could not read standard output of test case '%s'", it->name);
                exit(1);
            }
            fclose(it->pout);
        } else {
            actual.out = (SV) {0};
        }

        if (it->perr) {
            if (!read_fp(it->perr, &actual.err, arena)) {
                error("Could not read standard error of test case '%s'", it->name);
                exit(1);
            }
            fclose(it->perr);
        } else {
            actual.err = (SV) {0};
        }
        actual.exit = cmd_wait(it->proc);

        bool need_to_record = false;
        if (it->record_exists) {
            if (!test_info_diff(it->expected, actual, it->name)) {
                if (!interactive) {
                    exit(1);
                }

                const char *descriptions[] = {
                    "Record",
                    "Skip",
                    "Rerun",
                    "Quit",
                };

                ansi_set(stderr, ANSI_COLOR_CYAN | ANSI_BOLD);
                fprintf(stderr, "\nWhat to do for test case '%s'", it->name);
                const char choice = single_char_prompt(stdin, stderr, "ynrq", descriptions);
                if (choice == 'y') {
                    need_to_record = true;
                } else if (choice == 'r') {
                    if (actual.out.data) {
                        arena_reset(arena, actual.out.data);
                    } else if (actual.err.data) {
                        arena_reset(arena, actual.err.data);
                    }

                    for (size_t j = i; j < tests->count; j++) {
                        Test *it = &tests->data[j];
                        if (j > i) {
                            static char junk[4096];
                            if (it->pout) {
                                while (fread(junk, sizeof(junk), 1, it->pout) > 0);
                                fclose(it->pout);
                            }

                            if (it->perr) {
                                while (fread(junk, sizeof(junk), 1, it->perr) > 0);
                                fclose(it->perr);
                            }
                            cmd_wait(it->proc);
                        };

                        fprintf(stderr, "Replaying %s\n", it->name);
                        test_prepare_cmd(*it, cmd, optimization_level);
                        it->proc = cmd_run_async(cmd, (Cmd_Stdio) {.out = &it->pout, .err = &it->perr});
                    }
                    continue;
                } else if (choice == 'q') {
                    exit(0);
                }
            }
        } else {
            need_to_record = true;
        }

        if (need_to_record) {
            FILE *f = fopen(it->record_path, "w");
            if (!f) {
                error("Could not write file '%s'", it->record_path);
                exit(1);
            }

            fprintf(f, "EXIT %d\n", actual.exit);

            fprintf(f, "STDOUT %zu\n", actual.out.count);
            fwrite(actual.out.data, actual.out.count, 1, f);
            fprintf(f, "\n");

            fprintf(f, "STDERR %zu\n", actual.err.count);
            fwrite(actual.err.data, actual.err.count, 1, f);
            fprintf(f, "\n");

            fclose(f);
        }

        i++;
    }

    tests->count = 0;
    arena_reset(arena, arena_save);
}

static void build_test_library(Cmd *cmd, const char *library_path, const char *source_path) {
    if (get_modified_time(library_path) > get_modified_time(source_path)) {
        return;
    }

    const char *object_path = temp_replace_suffix(source_path, ".c", OBJ_FILE_EXTENSION);

    Cmd_Stdio proc_stdio = {0};

#ifdef PLATFORM_X86_64_WINDOWS
    FILE *proc_stdout = NULL;
    proc_stdio.out = &proc_stdout;
    cmd_push(cmd, "cl", "/nologo", "/c");
    cmd_push(cmd, arena_sprintf(&temp_arena, "/Fo:%s", object_path));
#else
    cmd_push(cmd, "cc", "-c");
    cmd_push(cmd, "-ggdb");
    cmd_push(cmd, "-o", object_path);
#endif // PLATFORM_X86_64_WINDOWS

    cmd_push(cmd, source_path);

    const char *proc_name = cmd->data[0];
    const Proc  proc = cmd_run_async(cmd, proc_stdio);
    if (proc.id == PROC_INVALID) {
        error("Could not start process '%s'", proc_name);
        exit(1);
    }

#ifdef PLATFORM_X86_64_WINDOWS
    filter_cl_exe_output(proc);
#endif // PLATFORM_X86_64_WINDOWS

    int code = cmd_wait(proc);
    if (code) {
        error("C compiler exited abnormally");
        exit(1);
    }

    fprintf(stderr, "Building '%s'\n", library_path);

#ifdef PLATFORM_X86_64_WINDOWS
    cmd_push(cmd, "lib", "/nologo");
    cmd_push(cmd, arena_sprintf(&temp_arena, "/out:%s", library_path));
#else
    cmd_push(cmd, "ar");
    cmd_push(cmd, "rcs");
    cmd_push(cmd, library_path);
#endif // PLATFORM_X86_64_WINDOWS

    cmd_push(cmd, object_path);

    proc_name = cmd->data[0];
    code = cmd_run_sync(cmd, (Cmd_Stdio) {0});
    if (code) {
        error("Process '%s' exited abnormally", proc_name);
        exit(1);
    }

    delete_file(object_path);
    arena_reset(&temp_arena, object_path);
}

static void run_tests(Cmd *cmd, size_t nprocs, bool interactive, const char *optimization_level) {
    Tests       tests = {0};
    const char *temp_save = arena_alloc(&temp_arena, 0);

    {
#ifdef PLATFORM_X86_64_WINDOWS
        build_test_library(cmd, "tests/abi/abi.lib", "tests/abi/abi.c");
#else
        build_test_library(cmd, "tests/abi/libabi.a", "tests/abi/abi.c");
#endif // PLATFORM_X86_64_WINDOWS
    }

    SV contents = {0};
    if (!read_file(TESTS_LIST_PATH, &contents, &default_arena)) {
        error("Could not read file '%s'", TESTS_LIST_PATH);
        exit(1);
    }

    const void *arena_save = arena_alloc(&default_arena, 0);
    while (contents.count) {
        SV line = sv_trim(sv_split(sv_split_mut(&contents, '\n'), '#'), ' ');
        if (line.count == 0) {
            continue;
        }

        Test test = {0};
        test.name = arena_sv_to_cstr(&temp_arena, line);
        test_prepare_cmd(test, cmd, optimization_level);

        const char *record_path = temp_replace_suffix(test.name, ".glos", ".bin");

        SV         contents = {0};
        const bool record_exists = read_file(record_path, &contents, &default_arena);

        Test_Info expected = {0};
        if (record_exists) {
            for (size_t row = 1; contents.count; row++) {
                SV          line = sv_split_mut(&contents, '\n');
                const char *line_start = line.data;

                line = sv_trim(line, ' ');
                SV key = sv_split_mut(&line, ' ');
                SV value = sv_trim(line, ' ');
                if (sv_match(key, "EXIT")) {
                    expected.exit = parse_uint_value(value, "exit code", record_path, row, line);
                } else if (sv_match(key, "STDOUT")) {
                    expected.out = parse_bytes_value(value, &contents, "standard output", record_path, &row, line);
                } else if (sv_match(key, "STDERR")) {
                    expected.err = parse_bytes_value(value, &contents, "standard error", record_path, &row, line);
                } else {
                    error_at(record_path, row, key.data - line_start + 1, "Invalid key '" SV_Fmt "'", SV_Arg(key));
                    exit(1);
                }
            }

            fprintf(stderr, "Replaying");
        } else {
            fprintf(stderr, "Recording");
        }

        fprintf(stderr, " %s\n", cmd->data[2]);

        test.record_exists = record_exists;
        test.record_path = record_path;

        test.proc = cmd_run_async(cmd, (Cmd_Stdio) {.out = &test.pout, .err = &test.perr});
        test.expected = expected;
        da_push(&tests, test);

        if (tests.count >= nprocs) {
            tests_flush(&tests, cmd, interactive, optimization_level, &default_arena, arena_save);
        }
    }

    tests_flush(&tests, cmd, interactive, optimization_level, &default_arena, arena_save);

    da_free(&tests);
    arena_reset(&temp_arena, temp_save);
}

int main(int argc, char **argv) {
    basic_init();
    const char *program = shift(&argc, &argv, NULL, NULL);

    bool        tests = false;
    bool        interactive = true;
    size_t      nprocs = 5;
    const char *optimization_level = NULL;
    while (argc) {
        const char *arg = shift(&argc, &argv, program, "Input path");
        if (!strcmp(arg, "-h")) {
            usage(stdout, program);
            exit(0);
        } else if (!strcmp(arg, "-t")) {
            if (tests) {
                error("Multiple test flags provided");
                exit(1);
            }

            tests = true;
        } else if (!strcmp(arg, "-T")) {
            if (tests) {
                error("Multiple test flags provided");
                exit(1);
            }

            tests = true;
            interactive = false;
        } else if (arg[0] == '-' && arg[1] == 'O') {
            const char *level = &arg[2];
            if (*level == '\0') {
                level = shift(&argc, &argv, program, "Optimization Level");
            }

            if (strcmp(level, "0") && strcmp(level, "1") && strcmp(level, "2") && strcmp(level, "3")) {
                error("Invalid optimization level '%s'\n", level);
                usage(stderr, program);
                exit(1);
            }

            optimization_level = arg;
        } else if (arg[0] == '-' && arg[1] == 'j') {
            if (arg[2]) {
                arg += 2;
            } else {
                arg = shift(&argc, &argv, program, "Parallel process count");
            }

            if (!parse_uint_from_sv(sv_from_cstr(arg), &nprocs) || nprocs == 0) {
                error("Invalid parallel process count '%s'", arg);
                exit(1);
            }
        } else {
            error("Invalid flag '%s'\n", arg);
            usage(stderr, program);
            exit(1);
        }
    }

    if (optimization_level && !tests) {
        error("The optimization level for running the tests is provided, but the test runner is not used");
        note("When using '-O', you have to provide either '-t' or 'T' to run tests\n");
        usage(stderr, program);
        exit(1);
    }

    Cmd cmd = {0};
    build_glos(&cmd, nprocs);

    if (tests) {
        run_tests(&cmd, nprocs, interactive, optimization_level);
    }

    da_free(&cmd);
    return 0;
}

#include "src/basic.c"
