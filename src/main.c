/*
 * main.c — ccc compiler driver
 *
 * Usage:
 *   ccc [-o output] [-S] [-c] [-v] [-ir] [-run] [-O1] input.c
 *
 *   -S        Emit assembly (AT&T syntax) to stdout or -o file
 *   -c        Compile to relocatable object file (.o)
 *   -o FILE   Output file name (default: a.out or input.o with -c)
 *   -v        Verbose: print pipeline stages
 *   -ir       Dump 3-address IR to stderr (after lowering)
 *   -O1       Enable IR optimizations (const folding + DCE)
 *   -run      JIT: compile to memory and execute immediately (no ELF written)
 *   (default) Compile + link to executable via system linker
 */

#define _POSIX_C_SOURCE 200809L
#include "../include/preprocessor.h"
#include "../include/lexer.h"
#include "../include/ast.h"
#include "../include/parser.h"
#include "../include/sema.h"
#include "../include/codegen.h"
#include "../include/ir.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* ---- Read entire file into a heap-allocated string ---- */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "ccc: cannot open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return NULL; }
    fread(buf, 1, (size_t)size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

/* ---- Replace extension of a path ---- */
static char *replace_ext(const char *path, const char *new_ext) {
    const char *dot = strrchr(path, '.');
    size_t base_len = dot ? (size_t)(dot - path) : strlen(path);
    size_t ext_len  = strlen(new_ext);
    char *result = malloc(base_len + ext_len + 1);
    memcpy(result, path, base_len);
    memcpy(result + base_len, new_ext, ext_len);
    result[base_len + ext_len] = '\0';
    return result;
}

/* ---- Link object file → executable using system linker ---- */
static int link_executable(const char *obj_file, const char *out_file, bool verbose) {
    /* Use gcc as linker frontend — handles crt0, dynamic linker, libc */
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "gcc -o \"%s\" \"%s\" -lm",
             out_file, obj_file);
    if (verbose) fprintf(stderr, "ccc: link: %s\n", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "ccc: link failed (exit %d)\n", ret);
        return 1;
    }
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr,
        "Usage: %s [-o output] [-S | -c] [-v] input.c\n"
        "\n"
        "Options:\n"
        "  -o FILE   Output file name\n"
        "  -S        Emit assembly to output (or stdout)\n"
        "  -c        Compile to object file (.o)\n"
        "  -v        Verbose output\n"
        "  -h        Show this help\n",
        prog);
}

int main(int argc, char **argv) {
    const char *input_file = NULL;
    const char *output_file = NULL;
    bool flag_S   = false;  /* emit assembly */
    bool flag_c   = false;  /* compile to .o only */
    bool flag_run = false;  /* JIT: compile+run in memory */
    bool flag_ir  = false;  /* dump IR to stderr */
    bool flag_O1  = false;  /* enable IR optimizations */
    bool verbose  = false;
    /* Preprocessor defines from command line */
    char *pp_defines[64]; int n_pp_defines = 0;
    char *pp_incpaths[64]; int n_pp_incpaths = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-S") == 0) {
            flag_S = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            flag_c = true;
        } else if (strcmp(argv[i], "-run") == 0) {
            flag_run = true;
        } else if (strcmp(argv[i], "-ir") == 0) {
            flag_ir = true;
        } else if (strcmp(argv[i], "-O1") == 0 || strcmp(argv[i], "-O2") == 0) {
            flag_O1 = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ccc: -o requires an argument\n");
                return 1;
            }
            output_file = argv[++i];
        } else if (strncmp(argv[i], "-D", 2) == 0) {
            if (n_pp_defines < 64) pp_defines[n_pp_defines++] = argv[i] + 2;
        } else if (strncmp(argv[i], "-I", 2) == 0) {
            if (n_pp_incpaths < 64) pp_incpaths[n_pp_incpaths++] = argv[i] + 2;
        } else if (argv[i][0] == '-') {
            /* Ignore unknown flags (could be -Wall, -std=c11, etc.) */
            if (verbose) fprintf(stderr, "ccc: ignoring flag '%s'\n", argv[i]);
        } else {
            if (input_file) {
                fprintf(stderr, "ccc: multiple input files not supported\n");
                return 1;
            }
            input_file = argv[i];
        }
    }

    if (!input_file) {
        fprintf(stderr, "ccc: no input file\n");
        usage(argv[0]);
        return 1;
    }

    /* Determine output filename if not given */
    char *default_out = NULL;
    if (!output_file) {
        if (flag_S) {
            /* Emit to stdout if no -o */
            output_file = NULL;  /* handled below */
        } else if (flag_c) {
            default_out  = replace_ext(input_file, ".o");
            output_file  = default_out;
        } else {
            output_file = "a.out";
        }
    }

    /* ---- Stage 1: Read source ---- */
    if (verbose) fprintf(stderr, "ccc: reading '%s'\n", input_file);
    char *src = read_file(input_file);
    if (!src) return 1;

    /* ---- Stage 1.5: Preprocess ---- */
    if (verbose) fprintf(stderr, "ccc: preprocessing\n");
    Preprocessor *pp = pp_new();
    for (int i = 0; i < n_pp_defines; i++) pp_define(pp, pp_defines[i], NULL);
    for (int i = 0; i < n_pp_incpaths; i++) pp_add_include_path(pp, pp_incpaths[i]);
    /* Add directory of input file as include search path */
    {
        char dir_buf[1024];
        strncpy(dir_buf, input_file, sizeof(dir_buf)-1); dir_buf[sizeof(dir_buf)-1] = '\0';
        char *slash = strrchr(dir_buf, '/');
        if (slash) { *slash = '\0'; pp_add_include_path(pp, dir_buf); }
        else        { pp_add_include_path(pp, "."); }
    }
    char *pp_src = pp_process_string(pp, src, input_file);
    free(src);
    if (!pp_src || pp->error_count > 0) {
        fprintf(stderr, "ccc: preprocessor error(s); aborting\n");
        pp_free(pp); free(pp_src);
        return 1;
    }
    pp_free(pp);
    src = pp_src;

    /* ---- Stage 2: Lex ---- */
    if (verbose) fprintf(stderr, "ccc: lexing\n");
    Lexer *lexer = lexer_new(src, input_file);

    /* ---- Stage 3: Parse ---- */
    if (verbose) fprintf(stderr, "ccc: parsing\n");
    Parser *parser = parser_new(lexer, input_file);
    Node   *ast    = parser_parse(parser);

    if (parser->error_count > 0) {
        fprintf(stderr, "ccc: %d parse error(s); aborting\n", parser->error_count);
        parser_free(parser);
        lexer_free(lexer);
        free(src);
        free(default_out);
        return 1;
    }

    /* ---- Stage 3b: Semantic analysis ---- */
    if (verbose) fprintf(stderr, "ccc: semantic analysis\n");
    Sema *sema = sema_new();
    sema->filename = input_file;
    int sema_errors = sema_analyze(sema, ast);
    if (sema_errors > 0 && verbose) {
        fprintf(stderr, "ccc: %d semantic error(s)\n", sema_errors);
    }
    sema_free(sema);

    /* ---- Stage 3c: IR lowering + optimization ---- */
    IrModule *irmod = NULL;
    if (flag_ir || flag_O1) {
        if (verbose) fprintf(stderr, "ccc: lowering IR\n");
        irmod = ir_module_new();
        ir_lower(irmod, ast);
        if (flag_O1) {
            if (verbose) fprintf(stderr, "ccc: const folding\n");
            ir_opt_const_fold(irmod);
            ir_opt_dce(irmod);
        }
        if (flag_ir) {
            fprintf(stderr, "\n=== IR dump ===\n");
            ir_print(irmod, stderr);
            fprintf(stderr, "===============\n\n");
        }
    }

    /* ---- Stage 4: Code generation ---- */
    if (verbose) fprintf(stderr, "ccc: generating code\n");
    CodeGen *cg = codegen_new();
    codegen_gen(cg, ast);

    if (cg->error_count > 0) {
        fprintf(stderr, "ccc: %d codegen error(s)\n", cg->error_count);
        /* Continue anyway and emit what we have */
    }

    /* ---- Stage 5: Emit ---- */
    if (flag_run) {
        /* JIT: map code into executable memory and run immediately */
        if (verbose) fprintf(stderr, "ccc: JIT running\n");
        int exit_code = codegen_jit_run(cg, verbose);
        if (irmod) ir_module_free(irmod);
        codegen_free(cg);
        parser_free(parser);
        lexer_free(lexer);
        free(src);
        free(default_out);
        return exit_code;
    } else if (flag_S) {
        /* Emit assembly */
        FILE *asm_out = stdout;
        if (output_file) {
            asm_out = fopen(output_file, "w");
            if (!asm_out) {
                fprintf(stderr, "ccc: cannot open '%s': %s\n", output_file, strerror(errno));
                codegen_free(cg);
                parser_free(parser);
                lexer_free(lexer);
                free(src);
                free(default_out);
                return 1;
            }
        }
        if (verbose) fprintf(stderr, "ccc: emitting assembly\n");
        codegen_emit_asm(cg, asm_out);
        if (asm_out != stdout) fclose(asm_out);
    } else {
        /* Emit object file */
        const char *obj_path = output_file;
        char *tmp_obj = NULL;

        if (!flag_c) {
            /* Need a temporary .o file for linking */
            tmp_obj  = strdup("/tmp/ccc_XXXXXX.o");
            /* mkstemp variant with suffix — just use a fixed temp name */
            snprintf(tmp_obj, strlen(tmp_obj) + 1, "/tmp/ccc_%d.o", (int)getpid());
            obj_path = tmp_obj;
        }

        if (verbose) fprintf(stderr, "ccc: writing object file '%s'\n", obj_path);
        codegen_emit_elf(cg, obj_path);

        if (!flag_c) {
            /* Link */
            if (verbose) fprintf(stderr, "ccc: linking\n");
            int ret = link_executable(obj_path, output_file ? output_file : "a.out", verbose);
            remove(tmp_obj);
            free(tmp_obj);
            if (ret != 0) {
                codegen_free(cg);
                parser_free(parser);
                lexer_free(lexer);
                free(src);
                free(default_out);
                return 1;
            }
        }
    }

    /* ---- Cleanup ---- */
    if (irmod) ir_module_free(irmod);
    codegen_free(cg);
    parser_free(parser);
    lexer_free(lexer);
    free(src);
    free(default_out);

    if (verbose) fprintf(stderr, "ccc: done\n");
    return 0;
}
