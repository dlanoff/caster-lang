static void collect_type_decls(EmitCtx *ctx) {
    for (int i = 0; g_types && i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (info->kind == TYPEINFO_ALIAS) {
            add_type_use(ctx, info->target);
            if (info->node->value) collect_expr(ctx, info->node->value);
        }
        if (info->kind == TYPEINFO_STRUCT) {
            if (info->open_runtime) add_type_use(ctx, info->name);
            for (int j = 0; j < info->node->fields.len; j++) {
                Node *field = info->node->fields.items[j];
                add_type_use(ctx, field->declared_type);
                if (field->value) collect_expr(ctx, field->value);
            }
        }
    }
}

static char *emit_named_value(TypeInfo *info) {
    if (info->kind == TYPEINFO_ALIAS) {
        if (info->node->value) return emit_expr(info->node->value);
        if (is_array_type(info->target)) return strf("%s(NULL, 0)", arr_make(info->target));
        if (is_map_type(info->target)) return strf("%s(NULL, 0)", map_fn(info->target, "make"));
        return xstrdup("/*missing_named_value*/");
    }

    if (info->open_runtime) return emit_open_named_value(info);

    char *fields = xstrdup("");
    for (int i = 0; i < info->node->fields.len; i++) {
        Node *field = info->node->fields.items[i];
        fields = strf("%s%s.%s = %s", fields, i ? ", " : "", field->name, emit_field_default(field));
    }
    return strf("(%s){ %s }", ctype(info->name), fields);
}

static void emit_named_value_helpers(FILE *f) {
    if (!g_types || !g_types->len) return;
    fprintf(f, "/* named MAP values */\n");
    for (int i = 0; i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (!type_has_complete_named_value(g_types, info)) continue;
        char *type = info->kind == TYPEINFO_STRUCT ? info->name : info->target;
        fprintf(f,
            "static FLX_UNUSED %s %s(void) {\n"
            "    return %s;\n"
            "}\n\n",
            ctype(type),
            emit_named_value_fn_name(info->name),
            emit_named_value(info));
    }
}

static void emit_global_declarations(FILE *f, Node *prog) {
    if (!prog->globals.len) return;
    fprintf(f, "/* globals */\n");
    for (int i = 0; i < prog->globals.len; i++) {
        Node *global = prog->globals.items[i];
        fprintf(f, "static %s %s;\n", ctype(global->declared_type), global->c_expr ? global->c_expr : global->name);
    }
    fprintf(f, "\n");
}

static void emit_named_value_storage(FILE *f) {
    if (!g_types || !g_types->len) return;
    fprintf(f, "/* named MAP value storage for REF */\n");
    for (int i = 0; i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (!type_has_complete_named_value(g_types, info)) continue;
        char *type = info->kind == TYPEINFO_STRUCT ? info->name : info->target;
        fprintf(f, "static %s %s;\n", ctype(type), emit_named_value_storage_name(info->name));
    }
    fprintf(f, "\n");
}

static void emit_named_value_storage_init(FILE *f) {
    if (!g_types || !g_types->len) return;
    fprintf(f, "static void flx_init_named_values(void) {\n");
    for (int i = 0; i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (!type_has_complete_named_value(g_types, info)) continue;
        fprintf(f, "    %s = %s();\n", emit_named_value_storage_name(info->name), emit_named_value_fn_name(info->name));
    }
    fprintf(f, "}\n\n");
}

static void emit_global_init(FILE *f, Node *prog) {
    if (!prog->globals.len) return;
    fprintf(f, "static void flx_init_globals(void) {\n");
    for (int i = 0; i < prog->globals.len; i++) {
        Node *global = prog->globals.items[i];
        if (global->value) fprintf(f, "    %s = %s;\n", global->c_expr ? global->c_expr : global->name, emit_expr(global->value));
    }
    fprintf(f, "}\n\n");
}

static void emit_cast_runtime(FILE *f, EmitCtx *ctx) {
    if (!ctx->uses_cast) return;

    fprintf(f,
        "/* cast helpers */\n"
        "static FLX_UNUSED char *flx_cast_str_to_cstr(FLX_STR text) {\n"
        "    char *out = malloc((size_t)text.len + 1);\n"
        "    if (!out) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
        "    for (int64_t i = 0; i < text.len; i++) out[i] = text.data[i];\n"
        "    out[text.len] = 0;\n"
        "    return out;\n"
        "}\n\n"
        "static FLX_UNUSED int64_t flx_cast_str_to_int(FLX_STR text) {\n"
        "    char *raw = flx_cast_str_to_cstr(text);\n"
        "    int64_t out = (int64_t)strtoll(raw, NULL, 10);\n"
        "    free(raw);\n"
        "    return out;\n"
        "}\n\n"
        "static FLX_UNUSED double flx_cast_str_to_flt(FLX_STR text) {\n"
        "    char *raw = flx_cast_str_to_cstr(text);\n"
        "    double out = strtod(raw, NULL);\n"
        "    free(raw);\n"
        "    return out;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_STR flx_cast_cstr_to_str(const char *text, int len) {\n"
        "    FLX_STR out = flx_str_alloc(len);\n"
        "    for (int i = 0; i < len; i++) out.data[i] = text[i];\n"
        "    return out;\n"
        "}\n\n"
        "static FLX_UNUSED FLX_STR flx_cast_int_to_str(int64_t value) {\n"
        "    char buf[64];\n"
        "    int len = snprintf(buf, sizeof(buf), \"%%lld\", (long long)value);\n"
        "    if (len < 0) { fprintf(stderr, \"caster: INT to STR cast failed\\n\"); exit(1); }\n"
        "    return flx_cast_cstr_to_str(buf, len);\n"
        "}\n\n"
        "static FLX_UNUSED FLX_STR flx_cast_flt_to_str(double value) {\n"
        "    char buf[128];\n"
        "    int len = snprintf(buf, sizeof(buf), \"%%.17g\", value);\n"
        "    if (len < 0) { fprintf(stderr, \"caster: FLT to STR cast failed\\n\"); exit(1); }\n"
        "    return flx_cast_cstr_to_str(buf, len);\n"
        "}\n\n"
        "static FLX_UNUSED FLX_STR flx_cast_bol_to_str(bool value) {\n"
        "    return value ? flx_cast_cstr_to_str(\"true\", 4) : flx_cast_cstr_to_str(\"false\", 5);\n"
        "}\n\n");
}

static void emit_rand_runtime(FILE *f, EmitCtx *ctx) {
    if (!ctx->uses_rand) return;

    fprintf(f,
        "/* random helper */\n"
        "static uint64_t flx_rand_state = 88172645463325252ull;\n"
        "static FLX_UNUSED double flx_rand(double scale) {\n"
        "    if (scale < 0) { fprintf(stderr, \"caster: RAND scale must be nonnegative\\n\"); exit(1); }\n"
        "    flx_rand_state ^= flx_rand_state << 7;\n"
        "    flx_rand_state ^= flx_rand_state >> 9;\n"
        "    flx_rand_state ^= flx_rand_state << 8;\n"
        "    double unit = (double)(flx_rand_state >> 11) * (1.0 / 9007199254740992.0);\n"
        "    return unit * scale;\n"
        "}\n\n");
}

static char *function_c_name(Node *fn) {
    return fn->c_expr ? fn->c_expr : fn->name;
}

static bool is_user_main(Node *fn) {
    return fn && !fn->c_expr && strcmp(fn->name, "main") == 0;
}

static char *function_return_ctype(Node *fn) {
    if (is_user_main(fn)) return xstrdup("int");
    return type_eq(fn->checked_type, "NUL") ? xstrdup("void") : ctype(fn->checked_type);
}

static void emit_function_signature(FILE *f, Node *fn) {
    if (is_user_main(fn) && g_emit_uses_sys) {
        fprintf(f, "int main(int argc, char **argv)");
        return;
    }
    fprintf(f, "%s %s(", function_return_ctype(fn), function_c_name(fn));
    if (fn->params.len == 0) {
        fprintf(f, "void");
    }
    for (int j = 0; j < fn->params.len; j++) {
        Node *param = fn->params.items[j];
        fprintf(f, "%s%s %s", j ? ", " : "", ctype(param->declared_type), param->name);
    }
    fprintf(f, ")");
}

static void emit_function_param_marks(FILE *f, Node *fn) {
    for (int j = 0; j < fn->params.len; j++) {
        Node *param = fn->params.items[j];
        fprintf(f, "    (void)%s;\n", param->name);
    }
}

static void emit_fn_type_decls(FILE *f, EmitCtx *ctx) {
    if (!ctx->fn_types.len) return;

    fprintf(f, "/* function pointer types */\n");
    for (int i = 0; i < ctx->fn_types.len; i++) {
        char *type = ctx->fn_types.items[i];
        char *input = fn_input_type(type);
        char *output = fn_output_type(type);
        fprintf(f, "typedef %s (*%s)(%s);\n", ctype(output), ctype(type), ctype(input));
    }
    fprintf(f, "\n");
}

static void emit_host_ctx_adapters(FILE *f, EmitCtx *ctx) {
    if (!ctx->host_ctx_calls.len) return;

    fprintf(f, "/* host context adapters */\n");
    for (int i = 0; i < ctx->host_ctx_calls.len; i++) {
        Node *call = ctx->host_ctx_calls.items[i];
        bool tls_host = strcmp(call->name, "hostTLS") == 0;
        Node *ctx_arg = call->args.items[tls_host ? 3 : 1];
        Node *handler = call->args.items[tls_host ? 4 : 2];
        if (handler->declared_type && type_eq(handler->declared_type, "REF[HttpReq]")) {
            fprintf(f,
                "static FLX_HttpRes %s(void *ctx, FLX_HttpReq *req) {\n"
                "    return %s((%s)ctx, req);\n"
                "}\n\n",
                emit_host_adapter_name(call),
                handler->name,
                ctype(ctx_arg->checked_type));
        } else {
            fprintf(f,
                "static FLX_HttpRes %s(void *ctx, FLX_HttpReq req) {\n"
                "    return %s((%s)ctx, req);\n"
                "}\n\n",
                emit_host_adapter_name(call),
                handler->name,
                ctype(ctx_arg->checked_type));
        }
    }
}

static void emit_function_definition(FILE *f, Node *fn, Node *prog) {
    owned_locals_reset();
    emit_function_signature(f, fn);
    fprintf(f, " {\n");
    emit_function_param_marks(f, fn);
    if (is_user_main(fn) && g_emit_uses_sys) fprintf(f, "    flx_sys_init(argc, argv);\n");
    if (is_user_main(fn)) fprintf(f, "    flx_init_named_values();\n");
    if (is_user_main(fn) && prog->globals.len) fprintf(f, "    flx_init_globals();\n");
    emit_block(f, fn->body, 1, fn);
    if (!block_ends_with_return(fn->body)) emit_owned_local_drops(f, 1, NULL);
    if (is_user_main(fn)) fprintf(f, "    return 0;\n");
    fprintf(f, "}\n\n");
}

static void collect_nested_functions(Node *b, PtrVec *out) {
    if (!b) return;
    for (int i = 0; i < b->statements.len; i++) {
        Node *st = b->statements.items[i];
        if (st->kind == NK_FN) {
            vec_push(out, st);
            collect_nested_functions(st->body, out);
            continue;
        }
        if (st->kind == NK_LOOP) {
            collect_nested_functions(st->body, out);
        } else if (st->kind == NK_IF) {
            collect_nested_functions(st->then_block, out);
            for (int j = 0; j < st->elx_branches.len; j++) {
                Node *branch = st->elx_branches.items[j];
                collect_nested_functions(branch->body, out);
            }
            collect_nested_functions(st->else_block, out);
        }
    }
}

static void emit_function_definition_with_nested(FILE *f, Node *fn, Node *prog);

static void emit_nested_function_definitions_in_block(FILE *f, Node *b, Node *prog) {
    if (!b) return;
    for (int i = 0; i < b->statements.len; i++) {
        Node *st = b->statements.items[i];
        if (st->kind == NK_FN) {
            emit_function_definition_with_nested(f, st, prog);
            continue;
        }
        if (st->kind == NK_LOOP) {
            emit_nested_function_definitions_in_block(f, st->body, prog);
        } else if (st->kind == NK_IF) {
            emit_nested_function_definitions_in_block(f, st->then_block, prog);
            for (int j = 0; j < st->elx_branches.len; j++) {
                Node *branch = st->elx_branches.items[j];
                emit_nested_function_definitions_in_block(f, branch->body, prog);
            }
            emit_nested_function_definitions_in_block(f, st->else_block, prog);
        }
    }
}

static void emit_function_definition_with_nested(FILE *f, Node *fn, Node *prog) {
    emit_nested_function_definitions_in_block(f, fn->body, prog);
    emit_function_definition(f, fn, prog);
}

static void emit_program(Node *prog, const char *out) {
    EmitCtx ctx = {0};
    collect_type_decls(&ctx);
    for (int i = 0; i < prog->globals.len; i++) {
        Node *global = prog->globals.items[i];
        add_type_use(&ctx, global->declared_type);
        if (global->value) collect_expr(&ctx, global->value);
    }
    if (prog->statements.len) collect_block(&ctx, prog);

    PtrVec nested_functions = {0};
    if (prog->statements.len) collect_nested_functions(prog, &nested_functions);
    for (int i = 0; i < prog->functions.len; i++) {
        Node *fn = prog->functions.items[i];
        add_type_use(&ctx, fn->checked_type);
        for (int j = 0; j < fn->params.len; j++) add_type_use(&ctx, ((Node *)fn->params.items[j])->declared_type);
        collect_block(&ctx, fn->body);
        collect_nested_functions(fn->body, &nested_functions);
    }

    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "caster: error: cannot write %s\n", out);
        exit(1);
    }

    g_emit_uses_req = ctx.uses_req;
    g_emit_uses_json = ctx.uses_json;
    g_emit_uses_sys = ctx.uses_sys;
    g_emit_uses_sql = ctx.uses_sql;

    fprintf(f, "#include <stdbool.h>\n#include <stdint.h>\n#include <stdio.h>\n");
    if (ctx.uses_str) fprintf(f, "#include <ctype.h>\n");
    if (ctx.array_types.len || ctx.uses_throw || ctx.uses_heap || ctx.uses_str || ctx.uses_str_concat || ctx.uses_map || ctx.uses_open || ctx.uses_json || ctx.uses_sys || ctx.uses_cast || ctx.uses_buf || ctx.uses_sql || ctx.uses_rand) fprintf(f, "#include <stdlib.h>\n");
    if (ctx.uses_open || ctx.uses_req || ctx.uses_json || ctx.uses_sys || ctx.uses_buf || ctx.uses_sql) fprintf(f, "#include <string.h>\n");
    if (ctx.uses_req) fprintf(f, "#include <signal.h>\n");
    if (ctx.uses_map || ctx.uses_open || ctx.uses_json) fprintf(f, "#include \"vendor/hashmap.c/hashmap.h\"\n");
    if (ctx.uses_json) fprintf(f, "#include \"vendor/cjson/cJSON.h\"\n");
    fprintf(f,
        "\n"
        "#if defined(__GNUC__) || defined(__clang__)\n"
        "#define FLX_UNUSED __attribute__((unused))\n"
        "#else\n"
        "#define FLX_UNUSED\n"
        "#endif\n\n");

    if (ctx.uses_str) {
        fprintf(f,
            "/* string runtime */\n"
            "typedef struct {\n"
            "    char* data;\n"
            "    int64_t len;\n"
            "    bool owned;\n"
            "} FLX_STR;\n\n"
            "static FLX_STR flx_str_lit(char* data, int64_t len) {\n"
            "    FLX_STR value;\n"
            "    value.data = data;\n"
            "    value.len = len;\n"
            "    value.owned = false;\n"
            "    return value;\n"
            "}\n\n");

        fprintf(f,
            "static FLX_STR flx_str_alloc(int64_t len) {\n"
            "    FLX_STR out;\n"
            "    out.len = len;\n"
            "    out.data = malloc((size_t)len + 1);\n"
            "    if (!out.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
            "    out.data[len] = 0;\n"
            "    out.owned = true;\n"
            "    return out;\n"
            "}\n\n"
            "static FLX_UNUSED void flx_drop_str(FLX_STR value) {\n"
            "    if (value.owned) free(value.data);\n"
            "}\n\n"
            "static FLX_UNUSED FLX_STR flx_str_copy(FLX_STR text) {\n"
            "    FLX_STR out = flx_str_alloc(text.len);\n"
            "    for (int64_t i = 0; i < text.len; i++) out.data[i] = text.data[i];\n"
            "    return out;\n"
            "}\n\n"
            "static FLX_UNUSED FLX_STR flx_str_char_at(FLX_STR text, int64_t index) {\n"
            "    if (index < 0 || index >= text.len) { fprintf(stderr, \"caster: string index out of bounds\\n\"); exit(1); }\n"
            "    FLX_STR out = flx_str_alloc(1);\n"
            "    out.data[0] = text.data[index];\n"
            "    return out;\n"
            "}\n\n"
            "static int64_t flx_str_find(FLX_STR text, FLX_STR needle) {\n"
            "    if (needle.len == 0) return 0;\n"
            "    if (needle.len > text.len) return -1;\n"
            "    for (int64_t i = 0; i <= text.len - needle.len; i++) {\n"
            "        bool ok = true;\n"
            "        for (int64_t j = 0; j < needle.len; j++) if (text.data[i + j] != needle.data[j]) { ok = false; break; }\n"
            "        if (ok) return i;\n"
            "    }\n"
            "    return -1;\n"
            "}\n\n"
            "static FLX_UNUSED bool flx_str_has(FLX_STR text, FLX_STR needle) {\n"
            "    return flx_str_find(text, needle) >= 0;\n"
            "}\n\n"
            "static FLX_UNUSED int flx_str_compare(FLX_STR left, FLX_STR right) {\n"
            "    int64_t n = left.len < right.len ? left.len : right.len;\n"
            "    for (int64_t i = 0; i < n; i++) {\n"
            "        unsigned char a = (unsigned char)left.data[i];\n"
            "        unsigned char b = (unsigned char)right.data[i];\n"
            "        if (a != b) return a < b ? -1 : 1;\n"
            "    }\n"
            "    if (left.len == right.len) return 0;\n"
            "    return left.len < right.len ? -1 : 1;\n"
            "}\n\n"
            "static FLX_UNUSED bool flx_str_compare_lt(FLX_STR left, FLX_STR right) { return flx_str_compare(left, right) < 0; }\n"
            "static FLX_UNUSED bool flx_str_compare_gt(FLX_STR left, FLX_STR right) { return flx_str_compare(left, right) > 0; }\n\n"
            "static FLX_UNUSED bool flx_str_starts(FLX_STR text, FLX_STR prefix) {\n"
            "    if (prefix.len > text.len) return false;\n"
            "    for (int64_t i = 0; i < prefix.len; i++) if (text.data[i] != prefix.data[i]) return false;\n"
            "    return true;\n"
            "}\n\n"
            "static FLX_UNUSED bool flx_str_ends(FLX_STR text, FLX_STR suffix) {\n"
            "    if (suffix.len > text.len) return false;\n"
            "    int64_t offset = text.len - suffix.len;\n"
            "    for (int64_t i = 0; i < suffix.len; i++) if (text.data[offset + i] != suffix.data[i]) return false;\n"
            "    return true;\n"
            "}\n\n"
            "static FLX_UNUSED FLX_STR flx_str_trim(FLX_STR text) {\n"
            "    int64_t start = 0;\n"
            "    int64_t end = text.len;\n"
            "    while (start < end && isspace((unsigned char)text.data[start])) start++;\n"
            "    while (end > start && isspace((unsigned char)text.data[end - 1])) end--;\n"
            "    FLX_STR out = flx_str_alloc(end - start);\n"
            "    for (int64_t i = start; i < end; i++) out.data[i - start] = text.data[i];\n"
            "    return out;\n"
            "}\n\n"
            "static FLX_UNUSED FLX_STR flx_str_lower(FLX_STR text) {\n"
            "    FLX_STR out = flx_str_alloc(text.len);\n"
            "    for (int64_t i = 0; i < text.len; i++) out.data[i] = (char)tolower((unsigned char)text.data[i]);\n"
            "    return out;\n"
            "}\n\n"
            "static FLX_UNUSED FLX_STR flx_str_upper(FLX_STR text) {\n"
            "    FLX_STR out = flx_str_alloc(text.len);\n"
            "    for (int64_t i = 0; i < text.len; i++) out.data[i] = (char)toupper((unsigned char)text.data[i]);\n"
            "    return out;\n"
            "}\n\n"
            "static FLX_UNUSED FLX_STR flx_str_replace(FLX_STR text, FLX_STR old, FLX_STR next) {\n"
            "    if (old.len == 0) return text;\n"
            "    int64_t count = 0;\n"
            "    for (int64_t i = 0; i <= text.len - old.len; ) {\n"
            "        bool ok = true;\n"
            "        for (int64_t j = 0; j < old.len; j++) if (text.data[i + j] != old.data[j]) { ok = false; break; }\n"
            "        if (ok) { count++; i += old.len; } else i++;\n"
            "    }\n"
            "    FLX_STR out = flx_str_alloc(text.len + count * (next.len - old.len));\n"
            "    int64_t pos = 0;\n"
            "    for (int64_t i = 0; i < text.len; ) {\n"
            "        bool ok = i <= text.len - old.len;\n"
            "        for (int64_t j = 0; ok && j < old.len; j++) if (text.data[i + j] != old.data[j]) ok = false;\n"
            "        if (ok) { for (int64_t j = 0; j < next.len; j++) out.data[pos++] = next.data[j]; i += old.len; }\n"
            "        else out.data[pos++] = text.data[i++];\n"
            "    }\n"
            "    return out;\n"
            "}\n\n"
            "static FLX_UNUSED int flx_hex_value(char ch) {\n"
            "    if (ch >= '0' && ch <= '9') return ch - '0';\n"
            "    if (ch >= 'a' && ch <= 'f') return 10 + ch - 'a';\n"
            "    if (ch >= 'A' && ch <= 'F') return 10 + ch - 'A';\n"
            "    return -1;\n"
            "}\n\n"
            "static FLX_UNUSED FLX_STR flx_str_url_decode(FLX_STR text) {\n"
            "    FLX_STR out = flx_str_alloc(text.len);\n"
            "    int64_t pos = 0;\n"
            "    for (int64_t i = 0; i < text.len; i++) {\n"
            "        if (text.data[i] == '+') {\n"
            "            out.data[pos++] = ' ';\n"
            "            continue;\n"
            "        }\n"
            "        if (text.data[i] == '%%' && i + 2 < text.len) {\n"
            "            int hi = flx_hex_value(text.data[i + 1]);\n"
            "            int lo = flx_hex_value(text.data[i + 2]);\n"
            "            if (hi >= 0 && lo >= 0) {\n"
            "                out.data[pos++] = (char)((hi << 4) | lo);\n"
            "                i += 2;\n"
            "                continue;\n"
            "            }\n"
            "        }\n"
            "        out.data[pos++] = text.data[i];\n"
            "    }\n"
            "    out.len = pos;\n"
            "    out.data[pos] = 0;\n"
            "    return out;\n"
            "}\n\n"
            "static FLX_UNUSED bool flx_str_all_digit(FLX_STR text) {\n"
            "    if (text.len == 0) return false;\n"
            "    for (int64_t i = 0; i < text.len; i++) if (!isdigit((unsigned char)text.data[i])) return false;\n"
            "    return true;\n"
            "}\n\n"
            "static FLX_UNUSED bool flx_str_all_alpha(FLX_STR text) {\n"
            "    if (text.len == 0) return false;\n"
            "    for (int64_t i = 0; i < text.len; i++) if (!isalpha((unsigned char)text.data[i])) return false;\n"
            "    return true;\n"
            "}\n\n"
            "static FLX_UNUSED bool flx_str_all_space(FLX_STR text) {\n"
            "    if (text.len == 0) return false;\n"
            "    for (int64_t i = 0; i < text.len; i++) if (!isspace((unsigned char)text.data[i])) return false;\n"
            "    return true;\n"
            "}\n\n");

        if (ctx.uses_str_eq) {
            fprintf(f,
                "static bool flx_str_eq(FLX_STR left, FLX_STR right) {\n"
                "    if (left.len != right.len) return false;\n"
                "    for (int64_t i = 0; i < left.len; i++) if (left.data[i] != right.data[i]) return false;\n"
                "    return true;\n"
                "}\n\n");
        }

        if (ctx.uses_str_concat) {
            fprintf(f,
                "static FLX_STR flx_str_concat(FLX_STR left, FLX_STR right) {\n"
                "    FLX_STR out;\n"
                "    out.len = left.len + right.len;\n"
                "    out.data = malloc((size_t)out.len + 1);\n"
                "    if (!out.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
                "    out.owned = true;\n"
                "    for (int64_t i = 0; i < left.len; i++) out.data[i] = left.data[i];\n"
                "    for (int64_t i = 0; i < right.len; i++) out.data[left.len + i] = right.data[i];\n"
                "    out.data[out.len] = 0;\n"
                "    return out;\n"
                "}\n\n");
        }
    }

    emit_cast_runtime(f, &ctx);
    emit_rand_runtime(f, &ctx);

    if (ctx.log_int) fprintf(f, "static void flx_log_int(int64_t value) {\n    printf(\"%%lld\\n\", (long long)value);\n}\n\n");
    if (ctx.log_flt) fprintf(f, "static void flx_log_flt(double value) {\n    printf(\"%%.17g\\n\", value);\n}\n\n");
    if (ctx.log_bol) fprintf(f, "static void flx_log_bol(bool value) {\n    printf(\"%%s\\n\", value ? \"true\" : \"false\");\n}\n\n");
    if (ctx.log_str) fprintf(f, "static void flx_log_str(FLX_STR value) {\n    printf(\"%%.*s\\n\", (int)value.len, value.data);\n}\n\n");
    if (ctx.uses_assert) {
        fprintf(f,
            "static FLX_UNUSED void flx_assert(bool condition) {\n"
            "    if (!condition) fprintf(stderr, \"\\033[31mASSERT FAILED\\033[0m\\n\");\n"
            "}\n\n");
    }
    if (ctx.uses_assert_msg) {
        fprintf(f,
            "static void flx_assert_msg(bool condition, FLX_STR message) {\n"
            "    if (!condition) fprintf(stderr, \"\\033[31mASSERT FAILED:\\033[0m %%.*s\\n\", (int)message.len, message.data);\n"
            "}\n\n");
    }

    emit_open_runtime(f, &ctx);
    emit_json_runtime(f, &ctx);
    if (ctx.log_obj) {
        fprintf(f,
            "static void flx_log_obj(FLX_OBJ value) {\n"
            "    switch (value.kind) {\n"
            "        case FLX_OBJ_INT: printf(\"%%lld\\n\", (long long)value.as.int_value); break;\n"
            "        case FLX_OBJ_FLT: printf(\"%%.17g\\n\", value.as.flt_value); break;\n"
            "        case FLX_OBJ_BOL: printf(\"%%s\\n\", value.as.bol_value ? \"true\" : \"false\"); break;\n"
            "        case FLX_OBJ_STR: printf(\"%%.*s\\n\", (int)value.as.str_value.len, value.as.str_value.data); break;\n"
            "        case FLX_OBJ_NIL: printf(\"NUL\\n\"); break;\n"
            "        case FLX_OBJ_MAP: printf(\"<object>\\n\"); break;\n"
            "        case FLX_OBJ_ARR: printf(\"<array>\\n\"); break;\n"
            "        case FLX_OBJ_BOX: printf(\"<object>\\n\"); break;\n"
            "    }\n"
            "}\n\n");
    }
    emit_struct_forwards(f);
    if (ctx.uses_req) {
        fprintf(f,
            "typedef enum {\n"
            "    FLX_TASK_PENDING,\n"
            "    FLX_TASK_RUNNING,\n"
            "    FLX_TASK_DONE,\n"
            "    FLX_TASK_CANCELLED\n"
            "} FLX_TaskStatus;\n\n"
            "typedef struct FLX_Task FLX_Task;\n\n"
            "struct FLX_Task {\n"
            "    FLX_TaskStatus status;\n"
            "    void *impl;\n"
            "    void *result;\n"
            "\n"
            "    void (*start)(FLX_Task *task);\n"
            "    void (*destroy)(FLX_Task *task);\n"
            "};\n\n"
            "typedef struct {\n"
            "    FLX_Task *task;\n"
            "} FLX_TSK_HTTPRES;\n\n");
    }

    emit_fn_type_decls(f, &ctx);
    qsort(ctx.array_types.items, (size_t)ctx.array_types.len, sizeof(void *), cmp_arr);
    if (ctx.array_types.len) fprintf(f, "/* array types */\n");
    for (int i = 0; i < ctx.array_types.len; i++) {
        char *type = ctx.array_types.items[i];
        char *element_type = array_elem_type(type);
        char *element_ctype = ctype(element_type);
        char *type_ctype = ctype(type);
        fprintf(f,
            "typedef struct {\n"
            "    %s* data;\n"
            "    int64_t len;\n"
            "    int64_t cap;\n"
            "} %s;\n\n",
            element_ctype, type_ctype);
    }

    emit_map_type_decls(f, &ctx);
    emit_struct_defs(f);
    if (ctx.uses_buf) fprintf(f, "#include \"src/caster/adapters/buf.c\"\n\n");
    if (ctx.uses_sql) fprintf(f, "#include \"src/caster/adapters/sql.c\"\n\n");
    emit_map_runtimes(f, &ctx);
    emit_struct_copy_helper_decls(f);
    emit_struct_drop_helper_decls(f);
    if (ctx.uses_req) fprintf(f, "#define FLX_HOST_READ_HEADERS %d\n", ctx.uses_req_headers ? 1 : 0);
    if (ctx.uses_req) fprintf(f, "#include \"src/caster/adapters/req.c\"\n\n");
    if (ctx.uses_req) {
        fprintf(f,
            "static FLX_UNUSED bool flx_task_is_finished(FLX_Task *task) {\n"
            "    return !task || task->status == FLX_TASK_DONE || task->status == FLX_TASK_CANCELLED;\n"
            "}\n\n"
            "static FLX_UNUSED void flx_task_start_if_needed(FLX_Task *task) {\n"
            "    if (task && task->status == FLX_TASK_PENDING && task->start) task->start(task);\n"
            "}\n\n"
            "static FLX_UNUSED void flx_hold_task(FLX_Task *task) {\n"
            "    flx_task_start_if_needed(task);\n"
            "    while (!flx_task_is_finished(task)) flx_scheduler_tick();\n"
            "}\n\n"
            "static FLX_UNUSED void flx_hold_all_tasks(FLX_Task **tasks, int64_t len) {\n"
            "    for (int64_t i = 0; i < len; i++) flx_task_start_if_needed(tasks[i]);\n"
            "    bool done = false;\n"
            "    while (!done) {\n"
            "        done = true;\n"
            "        for (int64_t i = 0; i < len; i++) {\n"
            "            if (!flx_task_is_finished(tasks[i])) { done = false; break; }\n"
            "        }\n"
            "        if (!done) flx_scheduler_tick();\n"
            "    }\n"
            "}\n\n"
            "static FLX_UNUSED FLX_HttpRes flx_hold_httpres(FLX_TSK_HTTPRES task) {\n"
            "    flx_hold_task(task.task);\n"
            "    if (!task.task || !task.task->result) {\n"
            "        FLX_HttpRes res;\n"
            "        res.status = 0;\n"
            "        res.body = flx_str_lit(\"task failed\", 11);\n"
            "        res.headers.raw = NULL;\n"
            "        return res;\n"
            "    }\n"
            "    return *(FLX_HttpRes *)task.task->result;\n"
            "}\n\n");
        fprintf(f,
            "static FLX_UNUSED void flx_drop_task(FLX_Task *task) {\n"
            "    if (task && task->destroy) task->destroy(task);\n"
            "}\n\n"
            "static FLX_UNUSED void flx_drop_tsk_httpres(FLX_TSK_HTTPRES task) {\n"
            "    flx_drop_task(task.task);\n"
            "}\n\n");
    }
    emit_open_box_helpers(f, &ctx);

    if (ctx.array_types.len) fprintf(f, "/* array helpers */\n");
    for (int i = 0; i < ctx.array_types.len; i++) {
        char *type = ctx.array_types.items[i];
        char *element_type = array_elem_type(type);
        char *element_ctype = ctype(element_type);
        char *type_ctype = ctype(type);
        char *make = arr_make(type);
        char *add = arr_add(type);
        char *drop = arr_drop(type);
        char *copy_values_item = emit_heap_field_copy(element_type, "values[i]");
        char *copy_src_item = emit_heap_field_copy(element_type, "src.data[i]");
        char *copy_left_item = emit_heap_field_copy(element_type, "left.data[i]");
        char *copy_right_item = emit_heap_field_copy(element_type, "right.data[i]");
        char *copy_value_item = emit_heap_field_copy(element_type, "value");
        char *element_drop_fn = type_has_drop_helper(element_type) ? drop_fn_for_type(element_type) : NULL;
        char *drop_elements = element_drop_fn
            ? strf("    for (int64_t i = 0; i < value.len; i++) %s(value.data[i]);\n", element_drop_fn)
            : xstrdup("");
        fprintf(f,
            "static FLX_UNUSED %s %s(const %s* values, int64_t len) {\n"
            "    %s arr;\n"
            "    arr.len = len;\n"
            "    arr.cap = len;\n"
            "    arr.data = NULL;\n"
            "    if (len > 0) {\n"
            "        arr.data = malloc(sizeof(%s) * (size_t)len);\n"
            "        if (!arr.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
            "        for (int64_t i = 0; i < len; i++) arr.data[i] = %s;\n"
            "    }\n"
            "    return arr;\n"
            "}\n\n"
            "static FLX_UNUSED void %s(%s value) {\n"
            "%s"
            "    free(value.data);\n"
            "}\n\n"
            "static FLX_UNUSED %s %s(%s src, %s value) {\n"
            "    %s arr;\n"
            "    arr.len = src.len + 1;\n"
            "    arr.cap = arr.len;\n"
            "    arr.data = malloc(sizeof(%s) * (size_t)arr.len);\n"
            "    if (!arr.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
            "    for (int64_t i = 0; i < src.len; i++) arr.data[i] = %s;\n"
            "    arr.data[src.len] = %s;\n"
            "    return arr;\n"
            "}\n\n",
            type_ctype, make, element_ctype, type_ctype, element_ctype, copy_values_item,
            drop, type_ctype, drop_elements,
            type_ctype, add, type_ctype, element_ctype,
            type_ctype,
            element_ctype,
            copy_src_item,
            copy_value_item);

        if (type_in_vec(&ctx.array_concat_types, type)) {
            fprintf(f,
                "static FLX_UNUSED %s %s(%s left, %s right) {\n"
                "    %s arr;\n"
                "    arr.len = left.len + right.len;\n"
                "    arr.cap = arr.len;\n"
                "    arr.data = arr.len > 0 ? malloc(sizeof(%s) * (size_t)arr.len) : NULL;\n"
                "    if (arr.len > 0 && !arr.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
                "    for (int64_t i = 0; i < left.len; i++) arr.data[i] = %s;\n"
                "    for (int64_t i = 0; i < right.len; i++) arr.data[left.len + i] = %s;\n"
                "    return arr;\n"
                "}\n\n",
                type_ctype, arr_concat(type), type_ctype, type_ctype,
                type_ctype,
                element_ctype,
                copy_left_item,
                copy_right_item);
        }

        if (type_in_vec(&ctx.array_sub_value_types, type)) {
            if (!type_eq(element_type, "INT") && !type_eq(element_type, "FLT") && !type_eq(element_type, "BOL") && !type_eq(element_type, "STR")) {
                fprintf(f, "/* array value removal equality is not implemented for ARR[%s] */\n\n", element_type);
            } else {
                char *eq_expr = type_eq(element_type, "STR") ? xstrdup("flx_str_eq(src.data[i], value)") : xstrdup("(src.data[i] == value)");
                fprintf(f,
                    "static FLX_UNUSED %s %s(%s src, %s value) {\n"
                    "    %s arr;\n"
                    "    arr.len = src.len;\n"
                    "    arr.cap = src.len;\n"
                    "    arr.data = src.len > 0 ? malloc(sizeof(%s) * (size_t)src.len) : NULL;\n"
                    "    if (src.len > 0 && !arr.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
                    "    bool removed = false;\n"
                    "    arr.len = 0;\n"
                    "    for (int64_t i = 0; i < src.len; i++) {\n"
                    "        if (!removed && %s) { removed = true; continue; }\n"
                    "        arr.data[arr.len++] = %s;\n"
                    "    }\n"
                    "    arr.cap = arr.len;\n"
                    "    return arr;\n"
                    "}\n\n",
                    type_ctype, arr_sub_value(type), type_ctype, element_ctype,
                    type_ctype,
                    element_ctype,
                    eq_expr,
                    copy_src_item);
            }
        }
    }

    if (ctx.uses_cast && type_in_vec(&ctx.array_types, "ARR[STR]")) {
        fprintf(f,
            "static FLX_UNUSED FLX_ARR_STR flx_str_chars(FLX_STR text) {\n"
            "    FLX_ARR_STR arr;\n"
            "    arr.len = text.len;\n"
            "    arr.cap = text.len;\n"
            "    arr.data = text.len > 0 ? malloc(sizeof(FLX_STR) * (size_t)text.len) : NULL;\n"
            "    if (text.len > 0 && !arr.data) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
            "    for (int64_t i = 0; i < text.len; i++) {\n"
            "        arr.data[i].data = text.data + i;\n"
            "        arr.data[i].len = 1;\n"
            "        arr.data[i].owned = false;\n"
            "    }\n"
            "    return arr;\n"
            "}\n\n"
            "static FLX_UNUSED FLX_STR flx_arr_str_join_sep(FLX_ARR_STR arr, FLX_STR delimiter) {\n"
            "    int64_t len = 0;\n"
            "    for (int64_t i = 0; i < arr.len; i++) len += arr.data[i].len;\n"
            "    if (arr.len > 1) len += delimiter.len * (arr.len - 1);\n"
            "    FLX_STR out = flx_str_alloc(len);\n"
            "    int64_t pos = 0;\n"
            "    for (int64_t i = 0; i < arr.len; i++) {\n"
            "        if (i > 0) for (int64_t j = 0; j < delimiter.len; j++) out.data[pos++] = delimiter.data[j];\n"
            "        for (int64_t j = 0; j < arr.data[i].len; j++) out.data[pos++] = arr.data[i].data[j];\n"
            "    }\n"
            "    return out;\n"
            "}\n\n"
            "static FLX_UNUSED FLX_STR flx_arr_str_join(FLX_ARR_STR arr) {\n"
            "    return flx_arr_str_join_sep(arr, flx_str_lit(\"\", 0));\n"
            "}\n\n");
    }

    emit_struct_copy_helpers(f);
    emit_struct_drop_helpers(f);

    if (ctx.uses_req && type_in_vec(&ctx.array_types, "ARR[TSK[HttpRes]]") && type_in_vec(&ctx.array_types, "ARR[HttpRes]")) {
        fprintf(f,
            "static FLX_UNUSED FLX_ARR_HTTPRES flx_hold_all_httpres(FLX_ARR_TSK_HTTPRES tasks) {\n"
            "    FLX_Task **raw = NULL;\n"
            "    if (tasks.len > 0) {\n"
            "        raw = malloc(sizeof(FLX_Task *) * (size_t)tasks.len);\n"
            "        if (!raw) { fprintf(stderr, \"caster: allocation failed\\n\"); exit(1); }\n"
            "        for (int64_t i = 0; i < tasks.len; i++) raw[i] = tasks.data[i].task;\n"
            "    }\n"
            "    flx_hold_all_tasks(raw, tasks.len);\n"
            "    FLX_ARR_HTTPRES out = flx_arr_httpres_make(NULL, 0);\n"
            "    for (int64_t i = 0; i < tasks.len; i++) {\n"
            "        out = flx_arr_httpres_add(out, *(FLX_HttpRes *)tasks.data[i].task->result);\n"
            "    }\n"
            "    free(raw);\n"
            "    return out;\n"
            "}\n\n");
    }

    emit_named_value_helpers(f);
    if (ctx.uses_sys) fprintf(f, "#include \"src/caster/adapters/sys.c\"\n\n");
    emit_json_decode_helpers(f, &ctx);

    emit_named_value_storage(f);
    emit_named_value_storage_init(f);
    emit_global_declarations(f, prog);
    emit_global_init(f, prog);

    if (prog->functions.len > 1 || prog->statements.len || nested_functions.len) {
        fprintf(f, "/* function declarations */\n");
        for (int i = 0; i < prog->functions.len; i++) {
            Node *fn = prog->functions.items[i];
            if (!prog->statements.len && strcmp(fn->name, "main") == 0) continue;
            emit_function_signature(f, fn);
            fprintf(f, ";\n");
        }
        for (int i = 0; i < nested_functions.len; i++) {
            emit_function_signature(f, nested_functions.items[i]);
            fprintf(f, ";\n");
        }
        fprintf(f, "\n");
    }

    emit_host_ctx_adapters(f, &ctx);

    Node *user_main = NULL;
    for (int i = 0; i < prog->functions.len; i++) {
        Node *fn = prog->functions.items[i];
        if (is_user_main(fn)) {
            user_main = fn;
            continue;
        }
        emit_function_definition_with_nested(f, fn, prog);
    }
    if (user_main) emit_function_definition_with_nested(f, user_main, prog);

    if (prog->statements.len) {
        emit_nested_function_definitions_in_block(f, prog, prog);
        Node script_fn = {0};
        script_fn.name = "main";
        owned_locals_reset();
        if (ctx.uses_sys) fprintf(f, "int main(int argc, char **argv) {\n");
        else fprintf(f, "int main(void) {\n");
        if (ctx.uses_sys) fprintf(f, "    flx_sys_init(argc, argv);\n");
        fprintf(f, "    flx_init_named_values();\n");
        if (prog->globals.len) fprintf(f, "    flx_init_globals();\n");
        emit_block(f, prog, 1, &script_fn);
        if (!block_ends_with_return(prog)) emit_owned_local_drops(f, 1, NULL);
        fprintf(f, "    return 0;\n");
        fprintf(f, "}\n\n");
    } else {
        bool has_main = false;
        for (int i = 0; i < prog->functions.len; i++) {
            Node *fn = prog->functions.items[i];
            if (strcmp(fn->name, "main") == 0) has_main = true;
        }
        if (!has_main) {
            if (ctx.uses_sys) fprintf(f, "int main(int argc, char **argv) {\n");
            else fprintf(f, "int main(void) {\n");
            if (ctx.uses_sys) fprintf(f, "    flx_sys_init(argc, argv);\n");
            fprintf(f, "    flx_init_named_values();\n");
            if (prog->globals.len) fprintf(f, "    flx_init_globals();\n");
            fprintf(f, "    return 0;\n");
            fprintf(f, "}\n\n");
        }
    }

    fclose(f);
}
