#include "basic.h"
#include "checker.h"
#include "compiler.h"
#include "error.h"
#include "parser.h"
#include <stdio.h>
#include <stdlib.h>

static void usage(FILE *f, const char *program) {
    afprintf(f, ANSI_COLOR_CYAN | ANSI_BOLD, "Usage:\n");
    afprintf(f, ANSI_COLOR_GREEN | ANSI_BOLD, "    %s", program);
    fprintf(
        f,
        " [FLAGS...] [FILE|DIRECTORY]\n"
        "\n");

    afprintf(f, ANSI_COLOR_CYAN | ANSI_BOLD, "Flags:\n");

    static const struct {
        const char *flag;
        const char *desc;
    } flags[] = {
        {"h", "             Show this message"},
        {"r", "             Run the program"},
        {"o", "OUTPUT       Set the output path"},
        {"O", "LEVEL        Set the optimization level [0: None, 1: Less (Default), 2: Medium, 3: Aggressive]"},
        {"L", "PATH         Add a library path"},
        {"l", "NAME         Add a library"},
        {"-", "             End of compiler options. All following arguments are passed to the program if ran"},
    };

    for (size_t i = 0; i < len(flags); i++) {
        afprintf(f, ANSI_COLOR_MAGENTA, "    -%s", flags[i].flag);
        afprintf(f, ANSI_COLOR_DEFAULT, " %s\n", flags[i].desc);
    }
}

static const char *shift(int *argc, char ***argv, const char *program, const char *expected) {
    if (*argc <= 0) {
        error_standalone(EK_ERROR, "%s not provided\n", expected);
        usage(stderr, program);
        exit(1);
    }

    (*argc)--;
    return *(*argv)++;
}

static const char *get_path_last(const char *path) {
    SV sv = sv_from_cstr(path);
    if (sv.count && sv.data[sv.count - 1] == '/') {
        sv.count--;
    }

    for (size_t i = sv.count; i > 0; i--) {
        if (sv.data[i - 1] == '/') {
            return path + i;
        }
    }

    return NULL;
}

static const char *get_temp_file_path(void) {
#ifdef PLATFORM_X86_64_WINDOWS
    static char dir[MAX_PATH + 1];
    static char path[MAX_PATH + 1];

    DWORD count = GetTempPathA(sizeof(dir), dir);
    if (count == 0 || count >= sizeof(dir)) {
        return NULL;
    }

    if (GetTempFileNameA(dir, "glos", 0, path) == 0) {
        return NULL;
    }

    unixify_path_separators_inplace(path, strlen(path));
    return path;
#else
    static char path[] = "/tmp/glos_XXXXXX";

    int fd = mkstemp(path);
    if (fd < 0) {
        return NULL;
    }

    close(fd);
    return path;
#endif // PLATFORM_X86_64_WINDOWS
}

static const char *get_std_dir_path(Arena *a) {
    const void *checkpoint = arena_alloc(&temp_arena, 0);
    const char *result = NULL;

#ifdef PLATFORM_X86_64_LINUX
    i64   count = 0;
    char *data = NULL;

    for (size_t capacity = DA_INIT_CAP; true; capacity *= 2) {
        data = arena_alloc(&temp_arena, capacity);
        count = readlink("/proc/self/exe", data, capacity);

        if (count == -1) {
            return_defer(NULL);
        }

        if ((size_t) count < capacity) {
            break;
        }

        arena_reset(&temp_arena, checkpoint);
    }
#endif // PLATFORM_X86_64_LINUX

#ifdef PLATFORM_ARM64_MACOS
    uint32_t count = DA_INIT_CAP;
    char    *data = arena_alloc(&temp_arena, count);

    int _NSGetExecutablePath(char *buffer, uint32_t *size);
    if (_NSGetExecutablePath(data, &count) == -1) {
        arena_reset(&temp_arena, data);
        data = arena_alloc(&temp_arena, count);

        if (_NSGetExecutablePath(data, &count) == -1) {
            arena_reset(&temp_arena, data);
            return NULL;
        }
    }

    count = strlen(data);
#endif // PLATFORM_ARM64_MACOS

#ifdef PLATFORM_X86_64_WINDOWS
    i64   count = 0;
    char *data = arena_alloc(&temp_arena, 0);

    for (size_t capacity = DA_INIT_CAP; true; capacity *= 2) {
        data = arena_alloc(&temp_arena, capacity);
        count = GetModuleFileNameA(NULL, data, capacity);

        if (count == 0) {
            return_defer(NULL);
        }

        if (count < capacity - 1) {
            break;
        }
    }

    unixify_path_separators_inplace(data, count);
#endif // PLATFORM_X86_64_WINDOWS

    SV sv = {.data = data, .count = count};
    for (size_t i = sv.count; i; i--) {
        if (sv.data[i - 1] == '/') {
            sv.count = i;
            break;
        }
    }

    return_defer(arena_sprintf(a, SV_Fmt "std", SV_Arg(sv)));

defer:
    arena_reset(&temp_arena, checkpoint);
    return result;
}

int main(int argc, char **argv) {
    basic_init();
    atexit(warnings_flush);
    const char *program = shift(&argc, &argv, NULL, NULL);

    int result = 0;
    Cmd cmd = {0};

    bool        run = false;
    const char *cwd = get_cwd(&default_arena);
    const char *input_path = NULL;
    const char *output_path = NULL;
    Link_Flags  link_flags = {0};

    Compiler compiler = {.optimization_level = O1};
    while (argc) {
        const char *arg = shift(&argc, &argv, program, "Input path");
        if (*arg == '-') {
            if (!strcmp(arg, "-h")) {
                usage(stdout, program);
                exit(0);
            } else if (!strcmp(arg, "-r")) {
                run = true;
            } else if (!strcmp(arg, "-o")) {
                output_path = shift(&argc, &argv, program, "Output path");
            } else if (!strcmp(arg, "--")) {
                break;
            } else if (arg[1] == 'O') {
                const char *level = &arg[2];
                if (*level == '\0') {
                    level = shift(&argc, &argv, program, "Optimization Level");
                }

                if (!strcmp(level, "0")) {
                    compiler.optimization_level = O0;
                } else if (!strcmp(level, "1")) {
                    compiler.optimization_level = O1;
                } else if (!strcmp(level, "2")) {
                    compiler.optimization_level = O2;
                } else if (!strcmp(level, "3")) {
                    compiler.optimization_level = O3;
                } else {
                    error_standalone(EK_ERROR, "Invalid optimization level '%s'\n", level);
                    usage(stderr, program);
                    exit(1);
                }
            } else if (arg[1] == 'L') {
                const char *libpath = &arg[2];
                if (*libpath == '\0') {
                    libpath = shift(&argc, &argv, program, "Library path");
                }

                link_flags_add_libpath(&link_flags, sv_from_cstr(libpath));
            } else if (arg[1] == 'l') {
                const char *libname = &arg[2];
                if (*libname == '\0') {
                    libname = shift(&argc, &argv, program, "Library name");
                }

                link_flags_add_libname(&link_flags, sv_from_cstr(libname));
            } else {
                error_standalone(EK_ERROR, "Invalid flag '%s'\n", arg);
                usage(stderr, program);
                exit(1);
            }
        } else {
            if (input_path) {
                error_standalone(EK_ERROR, "Multiple input paths provided");
                if (run) {
                    error_standalone(EK_NOTE, "When using '-r', pass program arguments after '--'");
                }
                exit(1);
            }

            input_path = arg;
        }
    }

    if (!input_path) {
        input_path = ".";
    }
    input_path = get_absolute_path(sv_from_cstr(cwd), sv_from_cstr(input_path), &default_arena);

    if (!output_path) {
        if (directory_exists(input_path)) {
            output_path = get_path_last(input_path);
            if (!output_path) {
                error_standalone(EK_ERROR, "ERROR: Could not infer output path. Provide it manually via '-o'");
                exit(1);
            }
        } else {
            output_path =
                arena_sv_to_cstr(&temp_arena, sv_strip_suffix(sv_from_cstr(input_path), sv_from_cstr(".glos")));
        }

        if (run) {
            const char *temp_path = get_temp_file_path();
            if (temp_path) {
                temporary_files_push(temp_path);
                output_path = temp_path;
            }
        }
    }

    if (directory_exists(output_path)) {
        error_standalone(EK_ERROR, "The output path '%s' exists and is a directory", output_path);
        exit(1);
    }

    Modules       modules = {0};
    Embed_Interns embed_interns = {0};

    Parser parser = {
        .cwd = sv_from_cstr(cwd),
        .std = sv_from_cstr(get_std_dir_path(&default_arena)),
        .modules = &modules,
        .embed_interns = &embed_interns,
    };

    compiler.parser = &parser;
    compiler.modules = &modules;

    compiler.cmd = &cmd;
    compiler.link_flags = &link_flags;

    compiler.main_module = module_get(&parser, input_path);
    compiler.main_module->name = sv_from_cstr("main");

    const bool input_is_directory = directory_exists(input_path);
    if (input_is_directory) {
        parser.root = sv_from_cstr(input_path);
        input_path = get_relative_path(parser.cwd, parser.root, &default_arena);
    } else {
        parser.root = sv_from_cstr(get_parent_dir_path(input_path, &default_arena));
        input_path = get_relative_path(parser.cwd, sv_from_cstr(input_path), &default_arena);
    }

    perf_begin();

    // Import the builtin module
    {
        const SV    name = sv_from_cstr("builtin");
        const SV    root = parser.std;
        const char *absolute_path = get_absolute_path(root, name, &default_arena);
        assert(directory_exists(absolute_path));

        compiler.builtin_module = module_get(&parser, absolute_path);
        compiler.builtin_module->name = name;

        parser.module_current = compiler.builtin_module;

        switch (parse_directory(&parser, compiler.builtin_module->relative_path)) {
        case PARSE_OK:
            // Pass
            break;

        case PARSE_FAILURE:
            error_standalone(EK_ERROR, "Could not read directory '%s'", compiler.builtin_module->relative_path);
            exit(1);
            break;

        case PARSE_EMPTY_DIRECTORY:
            error_standalone(
                EK_ERROR, "Directory '%s' does not contain any glos files", compiler.builtin_module->relative_path);
            exit(1);
            break;

        default:
            unreachable();
        }

        parser.module_current = NULL;
    }

    perf_end("builtin module");

    perf_begin();

    parser.module_current = compiler.main_module;
    if (input_is_directory) {
        switch (parse_directory(&parser, input_path)) {
        case PARSE_OK:
            // Pass
            break;

        case PARSE_FAILURE:
            error_standalone(EK_ERROR, "Could not read directory '%s'", input_path);
            exit(1);
            break;

        case PARSE_EMPTY_DIRECTORY:
            error_standalone(EK_ERROR, "Directory '%s' does not contain any glos files", input_path);
            exit(1);
            break;

        default:
            unreachable();
        }
    } else {
        if (parse_file(&parser, input_path) != PARSE_OK) {
            error_standalone(EK_ERROR, "Could not read file '%s'", input_path);
            exit(1);
        }
    }

    perf_end("main module");

#ifdef PLATFORM_X86_64_WINDOWS
    if (!sv_has_suffix(sv_from_cstr(output_path), sv_from_cstr(".exe"))) {
        output_path = arena_sprintf(&temp_arena, "%s.exe", output_path);
    }
#endif // PLATFORM_X86_64_WINDOWS

    if (run) {
        temporary_files_push(output_path);
    }

    perf_begin();
    check_nodes(&compiler);
    perf_end("analysis");

    ll_foreach(it, &modules) {
        SV path = {0};
        if (it == compiler.main_module) {
            path = parser.root;
        } else {
            path = sv_from_cstr(it->absolute_path);
        }
        link_flags_add_libpath(&link_flags, path);
    }

    compiler_build(&compiler, output_path);

#ifdef PROFILING
    fprintf(stderr, "Compiled %zu line(s) of code\n", total_lines_processed);
#endif // PROFILING

    if (run) {
        const char *child_name = output_path;

#ifndef PLATFORM_X86_64_WINDOWS
        if (!sv_find(sv_from_cstr(child_name), '/', NULL)) {
            child_name = arena_sprintf(&temp_arena, "./%s", child_name);
        }
#endif // PLATFORM_X86_64_WINDOWS

        cmd.count = 0;
        cmd_push(&cmd, child_name);
        cmd_push_many(&cmd, argv, argc);

        const Proc child_proc = cmd_run_async(&cmd, (Cmd_Stdio) {0});
        if (child_proc.id == PROC_INVALID) {
            error_standalone(EK_ERROR, "Could not start process '%s'", child_name);
            exit(1);
        }

        result = cmd_wait(child_proc);
    }

    warnings_flush();
    modules_free(&modules);
    parser_free(&parser);
    arena_free(&default_arena);
    ht_free(&embed_interns);
    cmd_free(&cmd);
    da_free(&link_flags);
    return result;
}
