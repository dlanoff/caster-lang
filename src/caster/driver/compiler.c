// Caster C compiler entrypoint.
//
// The compiler is intentionally split by phase:
//
//   driver/common.c -> shared allocation, file, formatting, and vector helpers
//   lex/lexer.c     -> source text to tokens
//   ast/ast.c       -> AST node/type helpers
//   parse/parser.c  -> tokens to AST
//   analyze/*.c     -> scope/type checking and loop contextual names
//   ast/ast_printer.c -> checked AST console output
//   emit/*.c        -> checked AST to readable C
//
// These phase files are included into one translation unit for now. That keeps
// the C version easy to build while the compiler shape is still changing.
// Once the language stabilizes, this can move to normal .h/.c separate
// compilation without changing the compiler pipeline.

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.c"
#include "../lex/lexer.c"
#include "../ast/ast.c"
#include "../parse/parser.c"
#include "../analyze/analyzer.c"
#include "../ast/ast_printer.c"
#include "../emit/emitter.c"

typedef struct {
    char **items;
    int len;
    int cap;
} StringVec;

static const char *g_repo_root = ".";

static void stringvec_push(StringVec *vec, char *value) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = xrealloc(vec->items, sizeof(char *) * (size_t)vec->cap);
    }
    vec->items[vec->len++] = value;
}

static bool stringvec_contains(StringVec *vec, const char *value) {
    for (int i = 0; i < vec->len; i++) {
        if (strcmp(vec->items[i], value) == 0) return true;
    }
    return false;
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool path_is_absolute(const char *path) {
    return path && path[0] == '/';
}

static char *path_join(const char *dir, const char *name) {
    if (path_is_absolute(name)) return xstrdup(name);
    if (!dir || !dir[0] || strcmp(dir, ".") == 0) return xstrdup(name);
    size_t len = strlen(dir);
    if (len > 0 && dir[len - 1] == '/') return strf("%s%s", dir, name);
    return strf("%s/%s", dir, name);
}

static bool path_ends_with(const char *path, const char *suffix) {
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

static char *path_dirname(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash) return xstrdup(".");
    if (slash == path) return xstrdup("/");
    return xstrndup(path, (size_t)(slash - path));
}

static bool has_caster_ext(const char *path) {
    size_t len = strlen(path);
    return len >= 5 && strcmp(path + len - 5, ".cast") == 0;
}

static char *path_with_caster_ext(const char *path) {
    return has_caster_ext(path) ? xstrdup(path) : strf("%s.cast", path);
}

static char *canonical_existing_path(const char *path) {
    char *resolved = realpath(path, NULL);
    return resolved ? resolved : xstrdup(path);
}

static const char *path_basename(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *module_name_from_path(const char *path) {
    const char *base = path_basename(path);
    size_t len = strlen(base);
    if (len >= 5 && strcmp(base + len - 5, ".cast") == 0) len -= 5;

    bool all_upper = false;
    bool saw_alpha = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)base[i];
        if (isalpha(ch)) {
            saw_alpha = true;
            if (!isupper(ch)) {
                all_upper = false;
                break;
            }
            all_upper = true;
        }
    }

    if (all_upper && saw_alpha) return xstrndup(base, len);

    char *out = xstrdup("");
    bool cap_next = true;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)base[i];
        if (!isalnum(ch)) {
            cap_next = true;
            continue;
        }
        if (cap_next && isalpha(ch)) {
            out = strf("%s%c", out, (char)toupper(ch));
        } else {
            out = strf("%s%c", out, (char)ch);
        }
        cap_next = false;
    }

    return out[0] ? out : xstrndup(base, len);
}

static char *module_prefix_from_path(const char *path) {
    char *name = module_name_from_path(path);
    char *prefix = strf("%s_", name);
    free(name);
    return prefix;
}

static bool is_native_import_path(const char *path) {
    return strncmp(path, "native_libs/", 12) == 0 ||
           strncmp(path, "./native_libs/", 14) == 0 ||
           strstr(path, "/native_libs/") != NULL;
}

static char *default_exe_path_from_output(const char *output_path) {
    if (path_ends_with(output_path, ".C")) {
        size_t len = strlen(output_path) - 2;
        return xstrndup(output_path, len);
    }
    return strf("%s.out", output_path);
}

static char *shell_quote(const char *value) {
    char *out = xstrdup("'");
    for (const char *p = value; *p; p++) {
        if (*p == '\'') out = strf("%s'\\''", out);
        else out = strf("%s%c", out, *p);
    }
    return strf("%s'", out);
}

static char *cmake_cache_value(const char *cache_path, const char *key) {
    char *src = read_file(cache_path);
    size_t key_len = strlen(key);
    char *line = src;

    while (*line) {
        char *line_end = strchr(line, '\n');
        if (!line_end) line_end = line + strlen(line);
        char *colon = memchr(line, ':', (size_t)(line_end - line));
        char *equals = memchr(line, '=', (size_t)(line_end - line));
        if (colon && equals && colon < equals && (size_t)(colon - line) == key_len && strncmp(line, key, key_len) == 0) {
            char *value = xstrndup(equals + 1, (size_t)(line_end - equals - 1));
            free(src);
            return value;
        }
        line = *line_end ? line_end + 1 : line_end;
    }

    free(src);
    return xstrdup("");
}

static bool cmake_cache_missing_value(const char *value) {
    return !value || !value[0] || strstr(value, "-NOTFOUND") != NULL;
}

static char *cmake_cache_first_value(const char *cache_path, const char *first_key, const char *second_key) {
    char *value = cmake_cache_value(cache_path, first_key);
    if (!cmake_cache_missing_value(value)) return value;
    return cmake_cache_value(cache_path, second_key);
}

static char *link_flags_from_cmake_list(const char *value) {
    if (cmake_cache_missing_value(value)) return xstrdup("");

    char *copy = xstrdup(value);
    char *flags = xstrdup("");
    char *cursor = copy;

    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) *next = 0;
        if (cursor[0]) {
            if (cursor[0] == '/' || cursor[0] == '-') {
                char *quoted = shell_quote(cursor);
                flags = strf("%s %s", flags, quoted);
            } else {
                flags = strf("%s -l%s", flags, cursor);
            }
        }
        cursor = next ? next + 1 : NULL;
    }

    free(copy);
    return flags;
}

static char *h2o_include_flags(const char *repo_root, const char *openssl_include, const char *wslay_include, bool enable_websocket) {
    const char *dirs[] = {
        "vendor/h2o/include",
        "vendor/h2o/deps/cloexec",
        "vendor/h2o/deps/golombset",
        "vendor/h2o/deps/hiredis",
        "vendor/h2o/deps/libgkc",
        "vendor/h2o/deps/libyrmcds",
        "vendor/h2o/deps/klib",
        "vendor/h2o/deps/neverbleed",
        "vendor/h2o/deps/picohttpparser",
        "vendor/h2o/deps/picotest",
        "vendor/h2o/deps/picotls/deps/cifra/src/ext",
        "vendor/h2o/deps/picotls/deps/cifra/src",
        "vendor/h2o/deps/picotls/deps/micro-ecc",
        "vendor/h2o/deps/picotls/include",
        "vendor/h2o/deps/quicly/include",
        "vendor/h2o/deps/yaml/include",
        "vendor/h2o/deps/yoml",
        "vendor/h2o/build",
    };

    char *flags = xstrdup("");
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        char *path = path_join(repo_root, dirs[i]);
        char *quoted = shell_quote(path);
        flags = strf("%s -isystem %s", flags, quoted);
    }
    if (!cmake_cache_missing_value(openssl_include)) {
        char *quoted = shell_quote(openssl_include);
        flags = strf("%s -isystem %s", flags, quoted);
    }
    if (enable_websocket && !cmake_cache_missing_value(wslay_include)) {
        char *quoted = shell_quote(wslay_include);
        flags = strf("%s -isystem %s -DFLX_ENABLE_WEBSOCKET=1", flags, quoted);
    }
    return flags;
}

static char *h2o_link_flags(const char *repo_root) {
    char *cache = path_join(repo_root, "vendor/h2o/build/CMakeCache.txt");
    char *h2o_lib = path_join(repo_root, "vendor/h2o/build/libh2o-evloop.a");
    if (access(h2o_lib, R_OK) != 0 || access(cache, R_OK) != 0) {
        fprintf(stderr, "caster: error: REQ requires H2O at %s; run `make -f build/Makefile h2o` first\n", h2o_lib);
        exit(1);
    }

    char *openssl_ssl = cmake_cache_value(cache, "OPENSSL_SSL_LIBRARY");
    char *openssl_crypto = cmake_cache_value(cache, "OPENSSL_CRYPTO_LIBRARY");
    char *zlib = cmake_cache_value(cache, "ZLIB_LIBRARY_RELEASE");
    if (cmake_cache_missing_value(zlib)) zlib = cmake_cache_value(cache, "ZLIB_LIBRARY");
    char *wslay = cmake_cache_first_value(cache, "WSLAY_LIBRARIES", "WSLAY_LIBS");

    if (cmake_cache_missing_value(openssl_ssl) || cmake_cache_missing_value(openssl_crypto)) {
        fprintf(stderr, "caster: error: H2O build is missing OpenSSL libraries; rerun `make -f build/Makefile h2o`\n");
        exit(1);
    }

    char *h2o_q = shell_quote(h2o_lib);
    char *ssl_q = shell_quote(openssl_ssl);
    char *crypto_q = shell_quote(openssl_crypto);
    char *flags = strf("%s %s %s", h2o_q, ssl_q, crypto_q);
    if (!cmake_cache_missing_value(zlib)) {
        char *zlib_q = shell_quote(zlib);
        flags = strf("%s %s", flags, zlib_q);
    }
    if (!cmake_cache_missing_value(wslay)) {
        char *wslay_flags = link_flags_from_cmake_list(wslay);
        flags = strf("%s %s", flags, wslay_flags);
    }
    flags = strf("%s -lm -pthread", flags);

    return flags;
}

static char *h2o_compile_flags(const char *repo_root) {
    char *cache = path_join(repo_root, "vendor/h2o/build/CMakeCache.txt");
    char *h2o_lib = path_join(repo_root, "vendor/h2o/build/libh2o-evloop.a");
    if (access(h2o_lib, R_OK) != 0 || access(cache, R_OK) != 0) {
        fprintf(stderr, "caster: error: REQ requires H2O at %s; run `make -f build/Makefile h2o` first\n", h2o_lib);
        exit(1);
    }
    char *openssl_include = cmake_cache_value(cache, "OPENSSL_INCLUDE_DIR");
    char *wslay_include = cmake_cache_first_value(cache, "WSLAY_INCLUDE_DIR", "WSLAY_INCLUDEDIR");
    char *wslay = cmake_cache_first_value(cache, "WSLAY_LIBRARIES", "WSLAY_LIBS");
    bool enable_websocket = !cmake_cache_missing_value(wslay_include) && !cmake_cache_missing_value(wslay);
    return h2o_include_flags(repo_root, openssl_include, wslay_include, enable_websocket);
}

static bool is_ident_start(char ch) {
    return isalpha((unsigned char)ch) || ch == '_';
}

static bool is_ident_char(char ch) {
    return isalnum((unsigned char)ch) || ch == '_';
}

static void collect_native_function_names(const char *src, StringVec *names) {
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    bool in_raw = false;

    for (int i = 0; src[i]; ) {
        char ch = src[i];

        if (in_line_comment) {
            if (ch == '\n' || ch == '\r') in_line_comment = false;
            i++;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && src[i + 1] == '/') {
                in_block_comment = false;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (in_string) {
            if (ch == '\\' && src[i + 1]) {
                i += 2;
            } else if (ch == '"') {
                in_string = false;
                i++;
            } else {
                i++;
            }
            continue;
        }
        if (in_raw) {
            if (ch == '`') {
                in_raw = false;
            }
            i++;
            continue;
        }

        if (ch == '/' && src[i + 1] == '/') { in_line_comment = true; i += 2; continue; }
        if (ch == '/' && src[i + 1] == '*') { in_block_comment = true; i += 2; continue; }
        if (ch == '"') { in_string = true; i++; continue; }
        if (ch == '`') { in_raw = true; i++; continue; }

        if (is_ident_start(ch)) {
            int start = i;
            while (is_ident_char(src[i])) i++;
            size_t len = (size_t)(i - start);
            if (len == 2 && strncmp(src + start, "FN", 2) == 0) {
                int j = i;
                while (src[j] == ' ' || src[j] == '\t' || src[j] == '\r' || src[j] == '\n') j++;
                if (is_ident_start(src[j])) {
                    int name_start = j;
                    while (is_ident_char(src[j])) j++;
                    stringvec_push(names, xstrndup(src + name_start, (size_t)(j - name_start)));
                }
            }
            continue;
        }

        i++;
    }
}

static int prev_nonspace_index(const char *src, int before) {
    int i = before - 1;
    while (i >= 0 && isspace((unsigned char)src[i])) i--;
    return i;
}

static int next_nonspace_index(const char *src, int after) {
    int i = after;
    while (src[i] && isspace((unsigned char)src[i])) i++;
    return i;
}

static int skip_balanced_brackets(const char *src, int start) {
    int depth = 0;
    int i = start;
    while (src[i]) {
        if (src[i] == '[') depth++;
        if (src[i] == ']') {
            depth--;
            if (depth == 0) return i + 1;
        }
        i++;
    }
    return i;
}

static void collect_ident_after_type_head(const char *src, int *idx, StringVec *names) {
    int j = *idx;
    while (isspace((unsigned char)src[j])) j++;
    if (src[j] == '[') {
        j = skip_balanced_brackets(src, j);
        while (isspace((unsigned char)src[j])) j++;
    }
    if (!is_ident_start(src[j])) return;

    int name_start = j;
    while (is_ident_char(src[j])) j++;
    stringvec_push(names, xstrndup(src + name_start, (size_t)(j - name_start)));
}

static void collect_ident_after_keyword(const char *src, int idx, StringVec *names) {
    int j = idx;
    while (isspace((unsigned char)src[j])) j++;
    if (!is_ident_start(src[j])) return;

    int name_start = j;
    while (is_ident_char(src[j])) j++;
    stringvec_push(names, xstrndup(src + name_start, (size_t)(j - name_start)));
}

static void collect_local_module_export_names(const char *src, StringVec *names) {
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    bool in_raw = false;
    int paren_depth = 0;
    int brace_depth = 0;
    int bracket_depth = 0;

    for (int i = 0; src[i]; ) {
        char ch = src[i];

        if (in_line_comment) {
            if (ch == '\n' || ch == '\r') in_line_comment = false;
            i++;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && src[i + 1] == '/') {
                in_block_comment = false;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (in_string) {
            if (ch == '\\' && src[i + 1]) i += 2;
            else {
                if (ch == '"') in_string = false;
                i++;
            }
            continue;
        }
        if (in_raw) {
            if (ch == '`') in_raw = false;
            i++;
            continue;
        }

        if (ch == '/' && src[i + 1] == '/') { in_line_comment = true; i += 2; continue; }
        if (ch == '/' && src[i + 1] == '*') { in_block_comment = true; i += 2; continue; }
        if (ch == '"') { in_string = true; i++; continue; }
        if (ch == '`') { in_raw = true; i++; continue; }

        if (is_ident_start(ch)) {
            int start = i;
            while (is_ident_char(src[i])) i++;
            size_t len = (size_t)(i - start);
            bool top_level = paren_depth == 0 && brace_depth == 0 && bracket_depth == 0;

            if (top_level) {
                if (len == 2 && strncmp(src + start, "FN", 2) == 0) {
                    collect_ident_after_type_head(src, &i, names);
                } else if (len == 4 && strncmp(src + start, "TYPE", 4) == 0) {
                    collect_ident_after_keyword(src, i, names);
                } else if (len == 3 && strncmp(src + start, "MAP", 3) == 0) {
                    collect_ident_after_type_head(src, &i, names);
                } else if (len == 3 && strncmp(src + start, "ARR", 3) == 0) {
                    collect_ident_after_type_head(src, &i, names);
                } else if (len == 3 && strncmp(src + start, "REF", 3) == 0) {
                    collect_ident_after_type_head(src, &i, names);
                } else if (len == 3 && strncmp(src + start, "TSK", 3) == 0) {
                    collect_ident_after_type_head(src, &i, names);
                } else if (len == 3 && strncmp(src + start, "INT", 3) == 0) {
                    collect_ident_after_keyword(src, i, names);
                } else if (len == 3 && strncmp(src + start, "FLT", 3) == 0) {
                    collect_ident_after_keyword(src, i, names);
                } else if (len == 3 && strncmp(src + start, "BOL", 3) == 0) {
                    collect_ident_after_keyword(src, i, names);
                } else if (len == 3 && strncmp(src + start, "STR", 3) == 0) {
                    collect_ident_after_keyword(src, i, names);
                } else if (len == 3 && strncmp(src + start, "DYN", 3) == 0) {
                    collect_ident_after_keyword(src, i, names);
                }
            }
            continue;
        }

        if (ch == '(') paren_depth++;
        else if (ch == ')' && paren_depth > 0) paren_depth--;
        else if (ch == '{') brace_depth++;
        else if (ch == '}' && brace_depth > 0) brace_depth--;
        else if (ch == '[') bracket_depth++;
        else if (ch == ']' && bracket_depth > 0) bracket_depth--;
        i++;
    }
}

static bool ident_starts_with_prefix(const char *ident, const char *prefix) {
    return strncmp(ident, prefix, strlen(prefix)) == 0;
}

static bool should_prefix_local_ident(const char *src, int ident_start, int ident_end, StringVec *names, const char *prefix, const char *ident) {
    if (!stringvec_contains(names, ident)) return false;
    if (ident_starts_with_prefix(ident, prefix)) return false;

    int prev = prev_nonspace_index(src, ident_start);
    if (prev >= 0 && src[prev] == '.') return false;

    int next = next_nonspace_index(src, ident_end);
    if (src[next] == '.') return false;

    return true;
}

static char *rewrite_local_module_source(const char *src, const char *prefix) {
    StringVec names = {0};
    collect_local_module_export_names(src, &names);
    if (names.len == 0) return xstrdup(src);

    char *out = xstrdup("");
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    bool in_raw = false;

    for (int i = 0; src[i]; ) {
        char ch = src[i];

        if (in_line_comment) {
            out = strf("%s%c", out, ch);
            if (ch == '\n' || ch == '\r') in_line_comment = false;
            i++;
            continue;
        }
        if (in_block_comment) {
            out = strf("%s%c", out, ch);
            if (ch == '*' && src[i + 1] == '/') {
                out = strf("%s/", out);
                in_block_comment = false;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (in_string) {
            out = strf("%s%c", out, ch);
            if (ch == '\\' && src[i + 1]) {
                out = strf("%s%c", out, src[i + 1]);
                i += 2;
            } else {
                if (ch == '"') in_string = false;
                i++;
            }
            continue;
        }
        if (in_raw) {
            out = strf("%s%c", out, ch);
            if (ch == '`') in_raw = false;
            i++;
            continue;
        }

        if (ch == '/' && src[i + 1] == '/') {
            out = strf("%s//", out);
            in_line_comment = true;
            i += 2;
            continue;
        }
        if (ch == '/' && src[i + 1] == '*') {
            out = strf("%s/*", out);
            in_block_comment = true;
            i += 2;
            continue;
        }
        if (ch == '"') {
            out = strf("%s%c", out, ch);
            in_string = true;
            i++;
            continue;
        }
        if (ch == '`') {
            out = strf("%s%c", out, ch);
            in_raw = true;
            i++;
            continue;
        }

        if (is_ident_start(ch)) {
            int start = i;
            while (is_ident_char(src[i])) i++;
            int end = i;
            char *ident = xstrndup(src + start, (size_t)(i - start));
            if (should_prefix_local_ident(src, start, end, &names, prefix, ident)) out = strf("%s%s%s", out, prefix, ident);
            else out = strf("%s%s", out, ident);
            continue;
        }

        out = strf("%s%c", out, ch);
        i++;
    }

    return out;
}

static bool is_module_ident(const char *ident) {
    return ident && ident[0] && isupper((unsigned char)ident[0]);
}

static void validate_qualified_modules(const char *path, const char *src, StringVec *visible_modules, StringVec *local_names) {
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    bool in_raw = false;

    for (int i = 0; src[i]; ) {
        char ch = src[i];

        if (in_line_comment) {
            if (ch == '\n' || ch == '\r') in_line_comment = false;
            i++;
            continue;
        }
        if (in_block_comment) {
            if (ch == '*' && src[i + 1] == '/') {
                in_block_comment = false;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (in_string) {
            if (ch == '\\' && src[i + 1]) i += 2;
            else {
                if (ch == '"') in_string = false;
                i++;
            }
            continue;
        }
        if (in_raw) {
            if (ch == '`') in_raw = false;
            i++;
            continue;
        }

        if (ch == '/' && src[i + 1] == '/') { in_line_comment = true; i += 2; continue; }
        if (ch == '/' && src[i + 1] == '*') { in_block_comment = true; i += 2; continue; }
        if (ch == '"') { in_string = true; i++; continue; }
        if (ch == '`') { in_raw = true; i++; continue; }

        if (is_ident_start(ch)) {
            int start = i;
            while (is_ident_char(src[i])) i++;
            int end = i;
            int next = next_nonspace_index(src, end);
            if (src[next] == '.') {
                char *module = xstrndup(src + start, (size_t)(end - start));
                if (is_module_ident(module) && !stringvec_contains(visible_modules, module) && !stringvec_contains(local_names, module)) {
                    die_message("%s: module '%s' is not imported", path, module);
                }
            }
            continue;
        }

        i++;
    }
}

static bool is_native_fn_declaration(const char *src, int ident_start) {
    int prev = prev_nonspace_index(src, ident_start);
    if (prev < 1) return false;
    if (src[prev] != 'N' || src[prev - 1] != 'F') return false;
    int before_fn = prev_nonspace_index(src, prev - 1);
    return before_fn < 0 || !is_ident_char(src[before_fn]);
}

static bool should_prefix_native_ident(const char *src, int ident_start, int ident_end, StringVec *fn_names, const char *ident) {
    if (!stringvec_contains(fn_names, ident)) return false;
    if (is_native_fn_declaration(src, ident_start)) return true;

    int next = next_nonspace_index(src, ident_end);
    if (src[next] == '(') return true;

    int prev = prev_nonspace_index(src, ident_start);
    return prev >= 0 && src[prev] == ',' && src[next] == ')';
}

static char *rewrite_native_module_source(const char *src, const char *prefix) {
    StringVec fn_names = {0};
    collect_native_function_names(src, &fn_names);
    if (fn_names.len == 0) return xstrdup(src);

    char *out = xstrdup("");
    bool in_line_comment = false;
    bool in_block_comment = false;
    bool in_string = false;
    bool in_raw = false;

    for (int i = 0; src[i]; ) {
        char ch = src[i];

        if (in_line_comment) {
            out = strf("%s%c", out, ch);
            if (ch == '\n' || ch == '\r') in_line_comment = false;
            i++;
            continue;
        }
        if (in_block_comment) {
            out = strf("%s%c", out, ch);
            if (ch == '*' && src[i + 1] == '/') {
                out = strf("%s/", out);
                in_block_comment = false;
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (in_string) {
            out = strf("%s%c", out, ch);
            if (ch == '\\' && src[i + 1]) {
                out = strf("%s%c", out, src[i + 1]);
                i += 2;
            } else {
                if (ch == '"') in_string = false;
                i++;
            }
            continue;
        }
        if (in_raw) {
            out = strf("%s%c", out, ch);
            if (ch == '`') in_raw = false;
            i++;
            continue;
        }

        if (ch == '/' && src[i + 1] == '/') {
            out = strf("%s//", out);
            in_line_comment = true;
            i += 2;
            continue;
        }
        if (ch == '/' && src[i + 1] == '*') {
            out = strf("%s/*", out);
            in_block_comment = true;
            i += 2;
            continue;
        }
        if (ch == '"') {
            out = strf("%s%c", out, ch);
            in_string = true;
            i++;
            continue;
        }
        if (ch == '`') {
            out = strf("%s%c", out, ch);
            in_raw = true;
            i++;
            continue;
        }

        if (is_ident_start(ch)) {
            int start = i;
            while (is_ident_char(src[i])) i++;
            int end = i;
            char *ident = xstrndup(src + start, (size_t)(i - start));
            if (should_prefix_native_ident(src, start, end, &fn_names, ident)) out = strf("%s%s%s", out, prefix, ident);
            else out = strf("%s%s", out, ident);
            continue;
        }

        out = strf("%s%c", out, ch);
        i++;
    }

    return out;
}

static char *resolve_import_path(const char *from_path, const char *spec) {
    if (spec[0] == '.' || spec[0] == '/') {
        char *base = spec[0] == '/' ? path_with_caster_ext(spec) : NULL;
        if (!base) {
            char *dir = path_dirname(from_path);
            char *joined = strf("%s/%s", dir, spec);
            base = path_with_caster_ext(joined);
        }
        if (!file_exists(base)) {
            die_message("cannot resolve local import '%s' from %s", spec, from_path);
        }
        return base;
    }

    char *native_rel = strf("native_libs/%s.cast", spec);
    char *native = path_join(g_repo_root, native_rel);
    if (!file_exists(native)) {
        die_message("cannot resolve native import '%s' at %s", spec, native);
    }
    return native;
}

static char *trim_span(char *start, char **end_out) {
    while (*start == ' ' || *start == '\t') start++;
    char *end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) end--;
    *end_out = end;
    return start;
}

static bool parse_use_line(char *line, char **spec_out) {
    char *end = NULL;
    char *trimmed = trim_span(line, &end);
    if (end - trimmed < 4 || strncmp(trimmed, "use ", 4) != 0) return false;

    char *spec = trimmed + 4;
    while (*spec == ' ' || *spec == '\t') spec++;
    char *spec_end = spec;
    while (spec_end < end && *spec_end != ' ' && *spec_end != '\t') spec_end++;
    char *token_end = spec_end;
    if (spec == spec_end) {
        die_message("empty use import");
    }
    while (spec_end < end && (*spec_end == ' ' || *spec_end == '\t')) spec_end++;
    if (spec_end < end && !(spec_end + 1 < end && spec_end[0] == '/' && spec_end[1] == '/')) {
        die_message("unexpected text after import '%.*s'", (int)(spec_end - spec), spec);
    }

    *spec_out = xstrndup(spec, (size_t)(token_end - spec));
    return true;
}

static char *expand_imports_from(const char *raw_path, StringVec *loaded, StringVec *loading, bool rewrite_as_module) {
    char *path = canonical_existing_path(raw_path);
    if (stringvec_contains(loading, path)) {
        die_message("cyclic import involving %s", path);
    }
    if (stringvec_contains(loaded, path)) return xstrdup("");

    stringvec_push(loading, xstrdup(path));
    char *src = read_file(path);
    char *imports = xstrdup("");
    char *body = xstrdup("");
    StringVec visible_modules = {0};

    const char *cursor = src;
    while (*cursor) {
        const char *line_start = cursor;
        while (*cursor && *cursor != '\n') cursor++;
        size_t line_len = (size_t)(cursor - line_start);
        char *line = xstrndup(line_start, line_len);
        char *spec = NULL;
        if (parse_use_line(line, &spec)) {
            char *import_path = resolve_import_path(path, spec);
            char *module_name = module_name_from_path(import_path);
            stringvec_push(&visible_modules, module_name);
            char *expanded = expand_imports_from(import_path, loaded, loading, !is_native_import_path(import_path));
            imports = strf("%s\n%s", imports, expanded);
        } else {
            body = strf("%s%.*s\n", body, (int)line_len, line_start);
        }
        if (*cursor == '\n') cursor++;
    }

    if (!is_native_import_path(path)) {
        StringVec local_names = {0};
        collect_local_module_export_names(body, &local_names);
        validate_qualified_modules(path, body, &visible_modules, &local_names);
    }

    if (is_native_import_path(path)) {
        char *prefix = module_prefix_from_path(path);
        char *rewritten = rewrite_native_module_source(body, prefix);
        free(body);
        body = rewritten;
        free(prefix);
    } else if (rewrite_as_module) {
        char *prefix = module_prefix_from_path(path);
        char *rewritten = rewrite_local_module_source(body, prefix);
        free(body);
        body = rewritten;
        free(prefix);
    }

    loading->len--;
    stringvec_push(loaded, xstrdup(path));
    if (!imports[0]) return body;
    if (!body[0]) return imports;
    return strf("%s\n%s", imports, body);
}

static char *expand_imports(const char *source_path) {
    StringVec loaded = {0};
    StringVec loading = {0};
    return expand_imports_from(source_path, &loaded, &loading, false);
}

int main(int argc, char **argv) {
    const char *source_path = "tests/authoritative.cast";
    const char *output_path = "output/output.C";
    const char *exe_path = NULL;
    bool check_only = false;
    bool json_output = false;
    bool run_output = true;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "check") == 0 || strcmp(argv[i], "--check") == 0) {
            check_only = true;
            run_output = false;
        } else if (strcmp(argv[i], "--json") == 0) {
            json_output = true;
        } else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "--exe") == 0 && i + 1 < argc) {
            exe_path = argv[++i];
        } else if (strcmp(argv[i], "--repo-root") == 0 && i + 1 < argc) {
            g_repo_root = argv[++i];
        } else if (strcmp(argv[i], "--no-run") == 0) {
            run_output = false;
        } else {
            source_path = argv[i];
        }
    }

    g_diagnostics_json = json_output;
    g_diagnostics_source_path = source_path;

    char *src = expand_imports(source_path);
    TokenVec toks = lex_source(src);
    Node *prog = parse_program(toks);

    analyze_program(prog);

    if (check_only) {
        if (json_output) printf("[]\n");
        else printf("caster: check passed\n");
        return 0;
    }

    printf("AST:\n");
    print_node(prog, 0);
    printf("\n");

    emit_program(prog, output_path);

    if (run_output) {
        // Flush before system() so the AST appears before generated-program
        // output even when stdout is block-buffered by the parent process.
        fflush(stdout);

        char *exe = exe_path ? xstrdup(exe_path) : default_exe_path_from_output(output_path);
        char *repo_q = shell_quote(g_repo_root);
        char *output_q = shell_quote(output_path);
        char *exe_q = shell_quote(exe);
        char *hashmap_path = path_join(g_repo_root, "vendor/hashmap.c/hashmap.c");
        char *hashmap_q = shell_quote(hashmap_path);
        char *cjson_path = path_join(g_repo_root, "vendor/cjson/cJSON.c");
        char *cjson_q = shell_quote(cjson_path);
        char *sqlite_path = path_join(g_repo_root, "vendor/sqlite/sqlite3.c");
        char *sqlite_q = shell_quote(sqlite_path);
        char *runtime_sources = g_emit_uses_json ? strf("%s %s", hashmap_q, cjson_q) : hashmap_q;
        if (g_emit_uses_sql) runtime_sources = strf("%s %s", runtime_sources, sqlite_q);
        const char *generated_cflags = getenv("CASTER_CFLAGS");
        if (!generated_cflags || !generated_cflags[0]) generated_cflags = "-O3 -DNDEBUG -std=c11 -Wall -Wextra";
        char *sql_cflags = g_emit_uses_sql ? xstrdup("-DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION") : xstrdup("");
        char *cmd = NULL;
        if (g_emit_uses_req) {
            char *h2o_cflags = h2o_compile_flags(g_repo_root);
            char *h2o_ldflags = h2o_link_flags(g_repo_root);
            cmd = strf("cc %s %s -I%s %s -DH2O_USE_LIBUV=0 -x c %s -x none %s %s -o %s", generated_cflags, sql_cflags, repo_q, h2o_cflags, output_q, runtime_sources, h2o_ldflags, exe_q);
        } else {
            cmd = strf("cc %s %s -I%s -x c %s -x none %s -o %s", generated_cflags, sql_cflags, repo_q, output_q, runtime_sources, exe_q);
        }

        printf("\nCompiling %s...\n", output_path);
        fflush(stdout);
        int rc = system(cmd);
        if (rc != 0) return 1;

        printf("Running generated program:\n");
        fflush(stdout);
        rc = system(exe_q);
        if (rc != 0) return 1;
    }

    return 0;
}
