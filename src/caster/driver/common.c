// Shared Runtime For The Compiler Itself
// --------------------------------------
//
// These helpers are used by every compiler phase. They intentionally exit on
// fatal errors instead of returning nullable values everywhere; this keeps the
// early compiler code direct while Caster syntax and semantics are still moving.

// ----------------------------- Utilities -----------------------------

static bool g_diagnostics_json = false;
static const char *g_diagnostics_source_path = NULL;

static void json_write_escaped(FILE *out, const char *value) {
    fputc('"', out);
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        switch (*p) {
            case '"': fputs("\\\"", out); break;
            case '\\': fputs("\\\\", out); break;
            case '\b': fputs("\\b", out); break;
            case '\f': fputs("\\f", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default:
                if (*p < 0x20) fprintf(out, "\\u%04x", *p);
                else fputc(*p, out);
                break;
        }
    }
    fputc('"', out);
}

static void print_json_diagnostic(int line, int col, const char *message) {
    const char *file = g_diagnostics_source_path ? g_diagnostics_source_path : "";
    int safe_line = line > 0 ? line : 1;
    int safe_col = col > 0 ? col : 1;
    int end_col = safe_col + 1;

    fputs("[\n  {\n    \"file\": ", stdout);
    json_write_escaped(stdout, file);
    fprintf(stdout,
        ",\n    \"line\": %d,\n    \"col\": %d,\n    \"endLine\": %d,\n    \"endCol\": %d,\n    \"severity\": \"error\",\n    \"message\": ",
        safe_line, safe_col, safe_line, end_col);
    json_write_escaped(stdout, message);
    fputs("\n  }\n]\n", stdout);
}

static void *xmalloc(size_t size) {
    void *ptr = malloc(size ? size : 1);
    if (!ptr) {
        fprintf(stderr, "caster: out of memory\n");
        exit(1);
    }
    return ptr;
}

static void *xrealloc(void *ptr, size_t size) {
    void *next = realloc(ptr, size ? size : 1);
    if (!next) {
        fprintf(stderr, "caster: out of memory\n");
        exit(1);
    }
    return next;
}

static char *xstrdup(const char *s) {
    size_t len = strlen(s);
    char *out = xmalloc(len + 1);
    memcpy(out, s, len + 1);
    return out;
}

static char *xstrndup(const char *s, size_t n) {
    char *out = xmalloc(n + 1);
    memcpy(out, s, n);
    out[n] = '\0';
    return out;
}

static char *strf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (len < 0) {
        fprintf(stderr, "caster: format error\n");
        exit(1);
    }
    char *out = xmalloc((size_t)len + 1);
    vsnprintf(out, (size_t)len + 1, fmt, args);
    va_end(args);
    return out;
}

static void die_at(int line, int col, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (len < 0) {
        fprintf(stderr, "caster: format error\n");
        va_end(args);
        exit(1);
    }
    char *message = xmalloc((size_t)len + 1);
    vsnprintf(message, (size_t)len + 1, fmt, args);
    va_end(args);

    if (g_diagnostics_json) {
        print_json_diagnostic(line, col, message);
    } else {
        fprintf(stderr, "caster: error: %d:%d: %s\n", line, col, message);
    }
    exit(1);
}

static void die_message(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (len < 0) {
        fprintf(stderr, "caster: format error\n");
        va_end(args);
        exit(1);
    }
    char *message = xmalloc((size_t)len + 1);
    vsnprintf(message, (size_t)len + 1, fmt, args);
    va_end(args);

    if (g_diagnostics_json) {
        print_json_diagnostic(1, 1, message);
    } else {
        fprintf(stderr, "caster: error: %s\n", message);
    }
    exit(1);
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        die_message("cannot open %s", path);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)len + 1);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        die_message("cannot read %s", path);
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

// ----------------------------- Vectors -----------------------------

typedef struct {
    void **items;
    int len;
    int cap;
} PtrVec;

static void vec_push(PtrVec *vec, void *item) {
    if (vec->len == vec->cap) {
        vec->cap = vec->cap ? vec->cap * 2 : 8;
        vec->items = xrealloc(vec->items, sizeof(void *) * (size_t)vec->cap);
    }
    vec->items[vec->len++] = item;
}
