// AST And Type Helpers
// --------------------
//
// The parser builds this intentionally simple AST. A Node has many optional
// fields instead of many tiny structs, which is less "pure C architecture" but
// easier to follow while the language design is still changing.
//
// `checked_type` is compiler metadata added by analyzer.c. Caster declarations
// are still explicit; expression types must still be computed so the emitter
// knows whether log(x) means flx_log_int, flx_log_bol, etc.

// ----------------------------- AST -----------------------------

typedef enum {
    NK_PROGRAM, NK_TYPE_ALIAS, NK_TYPE_STRUCT, NK_FIELD, NK_FN, NK_PARAM, NK_BLOCK, NK_VAR, NK_ASSIGN, NK_DESTRUCTURE, NK_EXPR_STMT,
    NK_RET, NK_IF, NK_ELX, NK_LOOP, NK_PASS, NK_THROW, NK_FREE, NK_AGG_STEP,
    NK_INT, NK_FLT, NK_BOL, NK_STR, NK_NIL, NK_NAME, NK_INIT, NK_ARRAY, NK_SHAPE, NK_MAP_LITERAL, NK_RECORD_LITERAL, NK_DOT, NK_UNARY, NK_BINARY, NK_CALL, NK_METHOD_CALL, NK_DECODE, NK_IF_EXPR
} NodeKind;

typedef struct Node Node;
struct Node {
    NodeKind kind;
    int line, col;
    char *checked_type;
    char *declared_type;
    char *name;
    char *alias;
    char *result_alias;
    char *sort_dir;
    char *text;
    char *op;
    char *c_expr;
    char *loop_kind;
    char *loop_hint;
    char *index_name;
    char *element_ptr;
    bool inferred_decl;
    bool ref_result_owned;
    bool method_value;
    bool implicit_deref;
    bool mutation_target;
    int64_t int_value;
    double float_value;
    bool bool_value;
    Node *body, *value, *target, *expr, *condition, *then_block, *else_block;
    Node *object, *index, *callee, *left, *right;
    PtrVec type_decls, globals, fields, functions, params, statements, args, elements, elx_branches;
};

static Node *node_new(NodeKind kind, Token tok) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = kind; n->line = tok.line; n->col = tok.col;
    return n;
}

static const char *node_kind_name(NodeKind k) {
    switch (k) {
        case NK_PROGRAM: return "program"; case NK_TYPE_ALIAS: return "type_alias"; case NK_TYPE_STRUCT: return "type_struct";
        case NK_FIELD: return "field"; case NK_FN: return "fn_decl"; case NK_PARAM: return "param";
        case NK_BLOCK: return "block"; case NK_VAR: return "var_decl"; case NK_ASSIGN: return "assign";
        case NK_DESTRUCTURE: return "destructure"; case NK_EXPR_STMT: return "expr_stmt"; case NK_RET: return "ret"; case NK_IF: return "if";
        case NK_ELX: return "elx"; case NK_LOOP: return "loop"; case NK_PASS: return "pass";
        case NK_THROW: return "throw"; case NK_FREE: return "free"; case NK_AGG_STEP: return "agg_step"; case NK_INT: return "int"; case NK_FLT: return "flt"; case NK_BOL: return "bol";
        case NK_STR: return "str"; case NK_NIL: return "NUL"; case NK_NAME: return "name"; case NK_INIT: return "init"; case NK_ARRAY: return "array_literal"; case NK_SHAPE: return "shape_literal";
        case NK_MAP_LITERAL: return "map_literal"; case NK_RECORD_LITERAL: return "record_literal";
        case NK_DOT: return "dot_access"; case NK_UNARY: return "unary"; case NK_BINARY: return "binary";
        case NK_CALL: return "call"; case NK_METHOD_CALL: return "method_call"; case NK_DECODE: return "decode";
        case NK_IF_EXPR: return "if_expr";
    }
    return "unknown";
}

// ----------------------------- Types -----------------------------

static bool type_eq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }
static bool is_array_type(const char *t) { return t && strncmp(t, "ARR[", 4) == 0 && t[strlen(t) - 1] == ']'; }
static bool is_map_type(const char *t) { return t && strncmp(t, "MAP[", 4) == 0 && t[strlen(t) - 1] == ']'; }
static bool is_ref_type(const char *t) { return t && strncmp(t, "REF[", 4) == 0 && t[strlen(t) - 1] == ']'; }
static bool is_task_type(const char *t) { return t && strncmp(t, "TSK[", 4) == 0 && t[strlen(t) - 1] == ']'; }
static bool is_fn_type(const char *t) { return t && strncmp(t, "FN[", 3) == 0 && t[strlen(t) - 1] == ']'; }
static char *array_type(const char *elem) { return strf("ARR[%s]", elem); }
static char *array_elem_type(const char *t) { size_t len = strlen(t); return xstrndup(t + 4, len - 5); }
static char *map_type(const char *key, const char *value) { return strf("MAP[%s,%s]", key, value); }
static char *ref_type(const char *target) { return strf("REF[%s]", target); }
static char *ref_target_type(const char *t) { size_t len = strlen(t); return xstrndup(t + 4, len - 5); }
static char *task_type(const char *target) { return strf("TSK[%s]", target); }
static char *task_result_type(const char *t) { size_t len = strlen(t); return xstrndup(t + 4, len - 5); }
static char *fn_type(const char *input, const char *output) { return strf("FN[%s->%s]", input, output); }
static void split_fn_type(const char *t, char **input_out, char **output_out) {
    int depth = 0;
    const char *start = t + 3;
    const char *end = t + strlen(t) - 1;
    for (const char *p = start; p < end - 1; p++) {
        if (*p == '[') depth++;
        else if (*p == ']') depth--;
        else if (*p == '-' && p[1] == '>' && depth == 0) {
            *input_out = xstrndup(start, (size_t)(p - start));
            *output_out = xstrndup(p + 2, (size_t)(end - (p + 2)));
            return;
        }
    }
    *input_out = NULL;
    *output_out = NULL;
}
static char *fn_input_type(const char *t) { char *input = NULL, *output = NULL; split_fn_type(t, &input, &output); return input; }
static char *fn_output_type(const char *t) { char *input = NULL, *output = NULL; split_fn_type(t, &input, &output); return output; }
