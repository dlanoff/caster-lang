static void outi(FILE *f, int n) {
    for (int i = 0; i < n; i++) fputs("    ", f);
}

static char *cond(Node *e) {
    char *x = emit_expr(e);
    size_t len = strlen(x);
    if (len > 1 && x[0] == '(' && x[len - 1] == ')') {
        x[len - 1] = 0;
        return x + 1;
    }
    return x;
}

static void emit_log_stmt(FILE *f, Node *arg, int in) {
    outi(f, in);
    if (type_eq(arg->checked_type, "INT")) fprintf(f, "flx_log_int(%s);\n", emit_expr(arg));
    else if (type_eq(arg->checked_type, "FLT")) fprintf(f, "flx_log_flt(%s);\n", emit_expr(arg));
    else if (type_eq(arg->checked_type, "BOL")) fprintf(f, "flx_log_bol(%s);\n", emit_expr(arg));
    else if (type_eq(arg->checked_type, "OBJ")) fprintf(f, "flx_log_obj(%s);\n", emit_expr(arg));
    else fprintf(f, "flx_log_str(%s);\n", emit_expr(arg));
}

static bool is_log_call(Node *expr) {
    return expr && expr->kind == NK_CALL && expr->callee->kind == NK_NAME && strcmp(expr->callee->name, "log") == 0;
}

static bool type_has_drop_helper(const char *type);
static char *drop_fn_for_type(const char *type);
static char *emit_promote_ref_target_expr(const char *target_type, const char *value);

static bool is_open_struct_field_dot(Node *target) {
    return target && target->kind == NK_DOT && target->name && is_open_struct_type(target->object->checked_type) &&
        struct_field(g_types, target->object->checked_type, target->name);
}

static bool is_ref_struct_field_dot(Node *target) {
    if (!target || target->kind != NK_DOT || !target->name || !target->object) return false;
    if (!is_struct_type(target->object->checked_type)) return false;
    Node *field = struct_field(g_types, target->object->checked_type, target->name);
    return field && is_ref_type(field->declared_type);
}

static char *emit_ref_field_owner_expr(Node *target) {
    return strf("%s.%s", emit_obj(target->object), ref_owner_field_name(target->name));
}

static bool ref_assignment_value_should_own(Node *value) {
    if (!value) return false;
    if (!value->ref_result_owned) return false;
    return true;
}

static char *emit_ref_field_value_expr(Node *value, const char *target_type) {
    if (value && value->kind == NK_NAME && value->ref_result_owned) {
        return emit_promote_ref_target_expr(target_type, emit_expr(value));
    }
    return emit_expr(value);
}

static char *emit_ref_field_assignment(Node *target, Node *value) {
    Node *field = struct_field(g_types, target->object->checked_type, target->name);
    char *target_expr = emit_expr(target);
    char *owner_expr = emit_ref_field_owner_expr(target);
    char *target_type = ref_target_type(field->declared_type);
    char *target_drop = type_has_drop_helper(target_type) ? drop_fn_for_type(target_type) : NULL;
    char *drop_stmt = target_drop
        ? strf("if (%s && %s) { %s(*%s); free(%s); }", owner_expr, target_expr, target_drop, target_expr, target_expr)
        : strf("if (%s && %s) free(%s);", owner_expr, target_expr, target_expr);
    return strf("{ %s %s = %s; %s = %s; }",
        drop_stmt,
        target_expr,
        emit_ref_field_value_expr(value, target_type),
        owner_expr,
        ref_assignment_value_should_own(value) ? "true" : "false");
}

static char *emit_open_field_assignment(Node *target, Node *value) {
    Node *field = struct_field(g_types, target->object->checked_type, target->name);
    return strf("%s = %s",
        emit_expr(target->object),
        emit_obj_set_typed(emit_expr(target->object), emit_key_literal(target->name), field->declared_type, emit_expr(value)));
}

static char *emit_open_field_update(Node *target, const char *value_expr) {
    Node *field = struct_field(g_types, target->object->checked_type, target->name);
    return strf("%s = %s",
        emit_expr(target->object),
        emit_obj_set_typed(emit_expr(target->object), emit_key_literal(target->name), field->declared_type, value_expr));
}

typedef struct {
    const char *method;
    char *out_name;
    char *out_type;
    char *iter_done_label;
    char *method_done_label;
    char *source_elem_expr;
    bool in_place;
    char *target_name;
    char *write_name;
    char *index_name;
} MethodEmitCtx;

static char *indent_text(int n) {
    char *out = xstrdup("");
    for (int i = 0; i < n; i++) out = strf("%s    ", out);
    return out;
}

static char *emit_block_text(Node *b, int in, Node *fn, MethodEmitCtx *method_ctx);
static char *emit_mutating_collection_method_stmt_text(Node *expr, int in);
static char *emit_mutating_collection_plus_stmt_text(Node *expr, int in);

static char *emit_method_value_text(Node *value_node, int in, MethodEmitCtx *ctx) {
    char *pad = indent_text(in);
    char *value = value_node ? emit_expr(value_node) : xstrdup("0");
    char *condition = value_node ? cond(value_node) : xstrdup("false");

    if (strcmp(ctx->method, "upd") == 0) {
        if (ctx->in_place) {
            return strf("%s%s = %s;\n%sgoto %s;\n",
                pad, ctx->source_elem_expr, value,
                pad, ctx->iter_done_label);
        }
        if (type_eq(ctx->out_type, "STR")) {
            return strf("%s%s = flx_str_concat(%s, %s);\n%sgoto %s;\n",
                pad, ctx->out_name, ctx->out_name, value,
                pad, ctx->iter_done_label);
        }
        return strf("%s%s = %s(%s, %s);\n%sgoto %s;\n",
            pad, ctx->out_name, arr_add(ctx->out_type), ctx->out_name, value,
            pad, ctx->iter_done_label);
    }

    if (strcmp(ctx->method, "filt") == 0) {
        if (ctx->in_place) {
            return strf(
                "%sif (%s) {\n"
                "%s    if (%s != %s) %s->data[%s] = %s->data[%s];\n"
                "%s    %s++;\n"
                "%s}\n"
                "%sgoto %s;\n",
                pad, condition,
                pad, ctx->write_name, ctx->index_name, ctx->target_name, ctx->write_name, ctx->target_name, ctx->index_name,
                pad, ctx->write_name,
                pad,
                pad, ctx->iter_done_label);
        }
        if (type_eq(ctx->out_type, "STR")) {
            return strf("%sif (%s) %s = flx_str_concat(%s, %s);\n%sgoto %s;\n",
                pad, condition, ctx->out_name, ctx->out_name, ctx->source_elem_expr,
                pad, ctx->iter_done_label);
        }
        return strf("%sif (%s) %s = %s(%s, %s);\n%sgoto %s;\n",
            pad, condition, ctx->out_name, arr_add(ctx->out_type), ctx->out_name, ctx->source_elem_expr,
            pad, ctx->iter_done_label);
    }

    if (strcmp(ctx->method, "agg") == 0) {
        if (type_eq(ctx->out_type, "STR")) {
            return strf("%s%s = flx_str_concat(%s, %s);\n%sgoto %s;\n", pad, ctx->out_name, ctx->out_name, value, pad, ctx->iter_done_label);
        }
        return strf("%s%s += %s;\n%sgoto %s;\n", pad, ctx->out_name, value, pad, ctx->iter_done_label);
    }

    return strf("%s/* invalid method ret */\n", pad);
}

static char *emit_agg_step_text(Node *step, int in, MethodEmitCtx *ctx) {
    char *pad = indent_text(in);
    if (step->value && step->value->kind == NK_IF_EXPR && !step->value->right) {
        char *inner_pad = indent_text(in + 1);
        char *condition = emit_expr(step->value->condition);
        char *value = emit_expr(step->value->value);
        char *body = NULL;
        if (step->op && strcmp(step->op, "-") == 0) {
            body = strf("%s%s -= %s;\n", inner_pad, ctx->out_name, value);
        } else if (type_eq(ctx->out_type, "STR")) {
            body = strf("%s%s = flx_str_concat(%s, %s);\n", inner_pad, ctx->out_name, ctx->out_name, value);
        } else {
            body = strf("%s%s += %s;\n", inner_pad, ctx->out_name, value);
        }
        return strf("%sif (%s) {\n%s%s}\n", pad, condition, body, pad);
    }

    char *value = emit_expr(step->value);
    if (step->op && strcmp(step->op, "-") == 0) {
        return strf("%s%s -= %s;\n", pad, ctx->out_name, value);
    }
    if (type_eq(ctx->out_type, "STR")) {
        return strf("%s%s = flx_str_concat(%s, %s);\n", pad, ctx->out_name, ctx->out_name, value);
    }
    return strf("%s%s += %s;\n", pad, ctx->out_name, value);
}

static char *emit_zero_value(const char *type) {
    if (type_eq(type, "INT")) return xstrdup("0");
    if (type_eq(type, "FLT")) return xstrdup("0.0");
    if (type_eq(type, "BOL")) return xstrdup("false");
    if (type_eq(type, "STR")) return xstrdup("flx_str_lit(\"\", 0)");
    if (is_ref_type(type) || is_fn_type(type)) return xstrdup("NULL");
    return strf("(%s){0}", ctype(type));
}

typedef struct {
    char *name;
    char *type;
} OwnedLocal;

static OwnedLocal *g_owned_locals = NULL;
static int g_owned_locals_len = 0;
static int g_owned_locals_cap = 0;
static int g_return_tmp_id = 0;
static int g_destructure_tmp_id = 0;

static bool type_needs_drop_depth(const char *type, int depth);

static bool field_type_needs_drop(const char *type, int depth) {
    if (is_ref_type(type)) return true;
    return type_needs_drop_depth(type, depth);
}

static bool type_needs_drop_depth(const char *type, int depth) {
    if (!type) return false;
    if (depth > 16) return false;
    if (type_eq(type, "STR") || type_eq(type, "OBJ") || type_eq(type, "OPEN")) return true;
    if (is_open_struct_type(type)) return true;
    if (is_array_type(type) || is_map_type(type) || is_task_type(type)) return true;
    if (is_ref_type(type)) return true;
    if (is_fn_type(type)) return false;
    if (is_struct_type(type)) {
        TypeInfo *info = type_find(g_types, type);
        if (!info || info->kind != TYPEINFO_STRUCT || info->open_runtime) return false;
        for (int i = 0; i < info->node->fields.len; i++) {
            Node *field = info->node->fields.items[i];
            if (field_type_needs_drop(field->declared_type, depth + 1)) return true;
        }
        return false;
    }
    return false;
}

static bool type_needs_drop(const char *type) {
    return type_needs_drop_depth(type, 0);
}

static bool type_has_drop_helper_depth(const char *type, int depth) {
    if (!type) return false;
    if (type_eq(type, "HttpReq") || type_eq(type, "HttpRes")) return true;
    if (depth > 16) return false;
    if (is_struct_type(type)) {
        TypeInfo *info = type_find(g_types, type);
        if (!info || info->kind != TYPEINFO_STRUCT || info->open_runtime) return false;
        for (int i = 0; i < info->node->fields.len; i++) {
            Node *field = info->node->fields.items[i];
            if (field_type_needs_drop(field->declared_type, depth + 1)) return true;
        }
        return false;
    }
    return type_needs_drop(type);
}

static bool type_has_drop_helper(const char *type) {
    return type_has_drop_helper_depth(type, 0);
}

static char *drop_fn_for_type(const char *type) {
    if (type_eq(type, "STR")) return xstrdup("flx_drop_str");
    if (type_eq(type, "OBJ")) return xstrdup("flx_drop_obj");
    if (type_eq(type, "OPEN") || is_open_struct_type(type)) return xstrdup("flx_drop_objmap");
    if (is_array_type(type)) return arr_drop(type);
    if (is_map_type(type)) return map_fn(type, "drop");
    if (is_task_type(type)) {
        char *target = task_result_type(type);
        if (type_eq(target, "HttpRes")) return xstrdup("flx_drop_tsk_httpres");
    }
    if (is_struct_type(type)) return strf("flx_drop_%s", lower_mangle(type));
    return NULL;
}

static void owned_locals_reset(void) {
    g_owned_locals_len = 0;
    g_return_tmp_id = 0;
    g_destructure_tmp_id = 0;
}

static bool owned_local_contains(const char *name) {
    for (int i = 0; i < g_owned_locals_len; i++) {
        if (strcmp(g_owned_locals[i].name, name) == 0) return true;
    }
    return false;
}

static void owned_local_remove(const char *name) {
    for (int i = 0; i < g_owned_locals_len; i++) {
        if (strcmp(g_owned_locals[i].name, name) != 0) continue;
        for (int j = i + 1; j < g_owned_locals_len; j++) {
            g_owned_locals[j - 1] = g_owned_locals[j];
        }
        g_owned_locals_len--;
        return;
    }
}

static void owned_local_add(const char *name, const char *type) {
    if (!name || !type || !type_needs_drop(type)) return;
    if (owned_local_contains(name)) return;
    if (g_owned_locals_len == g_owned_locals_cap) {
        g_owned_locals_cap = g_owned_locals_cap ? g_owned_locals_cap * 2 : 8;
        g_owned_locals = xrealloc(g_owned_locals, sizeof(OwnedLocal) * (size_t)g_owned_locals_cap);
    }
    g_owned_locals[g_owned_locals_len++] = (OwnedLocal){xstrdup(name), xstrdup(type)};
}

static bool initializer_may_own(Node *value) {
    if (!value) return false;
    if (is_struct_constructor_expr(value)) return initializer_may_own(value->args.items[0]);
    if (value->kind == NK_NAME) return value->c_expr && strncmp(value->c_expr, "flx_value_", 10) == 0;
    if (value->kind == NK_DOT) return false;
    if (value->kind == NK_UNARY && value->op && (strcmp(value->op, "REF") == 0 || strcmp(value->op, "*") == 0)) return false;
    return true;
}

static bool emit_expr_produces_owned_ref(Node *value) {
    return value && value->ref_result_owned;
}

static bool assignment_value_should_copy(Node *value) {
    if (!value) return false;
    if (is_struct_constructor_expr(value)) return assignment_value_should_copy(value->args.items[0]);
    if (value->kind == NK_NAME) return !(value->c_expr && strncmp(value->c_expr, "flx_value_", 10) == 0);
    if (value->kind == NK_DOT) return true;
    if (value->kind == NK_UNARY && value->op && strcmp(value->op, "*") == 0) return true;
    return false;
}

static bool assignment_initializer_may_own(const char *target_type, Node *value) {
    if (!value) return false;
    if (!type_needs_drop(target_type)) return false;
    if (is_ref_type(target_type)) return emit_expr_produces_owned_ref(value);
    if (is_task_type(target_type) && assignment_value_should_copy(value)) return false;
    if (assignment_value_should_copy(value)) return true;
    return initializer_may_own(value);
}

static char *emit_assignment_value_expr(const char *target_type, Node *value) {
    char *expr = emit_expr(value);
    if (!assignment_value_should_copy(value)) return expr;
    if (is_task_type(target_type)) return expr;
    return emit_heap_field_copy(target_type, expr);
}

static const char *returned_owned_local_name(Node *value) {
    if (!value) return NULL;
    if (is_struct_constructor_expr(value)) return returned_owned_local_name(value->args.items[0]);
    if (value->kind != NK_NAME) return NULL;
    if (!owned_local_contains(value->name)) return NULL;
    return value->name;
}

static bool return_expr_is_borrowed(Node *value) {
    if (!value) return false;
    if (is_struct_constructor_expr(value)) return return_expr_is_borrowed(value->args.items[0]);
    if (value->ref_result_owned) return false;
    if (value->kind == NK_NAME) {
        if (returned_owned_local_name(value)) return false;
        if (value->c_expr && strncmp(value->c_expr, "flx_value_", 10) == 0) return false;
        return true;
    }
    return value->kind == NK_DOT;
}

static char *emit_return_value_expr(Node *value) {
    char *expr = emit_expr(value);
    if (!return_expr_is_borrowed(value)) return expr;
    return emit_heap_field_copy(value->checked_type, expr);
}

static void emit_drop_stmt_with_prefix(FILE *f, int in, const char *type, const char *expr, bool already_indented) {
    if (is_ref_type(type)) {
        char *target_type = ref_target_type(type);
        char *target_drop = NULL;
        if (type_has_drop_helper(target_type) && !is_ref_type(target_type)) {
            target_drop = drop_fn_for_type(target_type);
        }

        if (!already_indented) outi(f, in);
        fprintf(f, "{\n");
        outi(f, in + 1);
        fprintf(f, "if (%s) {\n", expr);
        if (target_drop) {
            outi(f, in + 2);
            fprintf(f, "%s(*%s);\n", target_drop, expr);
        }
        outi(f, in + 2);
        fprintf(f, "free(%s);\n", expr);
        outi(f, in + 1);
        fprintf(f, "}\n");
        outi(f, in);
        fprintf(f, "}\n");
        return;
    }

    char *drop = drop_fn_for_type(type);
    if (!drop) return;
    if (!already_indented) outi(f, in);
    fprintf(f, "%s(%s);\n", drop, expr);
}

static void emit_drop_stmt(FILE *f, int in, const char *type, const char *expr) {
    emit_drop_stmt_with_prefix(f, in, type, expr, false);
}

static void emit_owned_local_drops(FILE *f, int in, const char *skip_name) {
    for (int i = g_owned_locals_len - 1; i >= 0; i--) {
        if (skip_name && strcmp(skip_name, g_owned_locals[i].name) == 0) continue;
        emit_drop_stmt(f, in, g_owned_locals[i].type, g_owned_locals[i].name);
    }
}

static char *emit_accumulate_stmt(const char *out_name, const char *out_type, const char *value_expr) {
    if (type_eq(out_type, "STR")) return strf("%s = flx_str_concat(%s, %s);", out_name, out_name, value_expr);
    return strf("%s += %s;", out_name, value_expr);
}

static char *emit_agg_value(Node *call, const char *elem_expr) {
    if (call->args.len == 0) return xstrdup(elem_expr);
    Node *arg = call->args.items[0];
    if (arg->kind == NK_IF_EXPR) return emit_expr(arg->value);
    return emit_expr(arg);
}

static char *emit_agg_condition(Node *call) {
    if (call->args.len == 0) return NULL;
    Node *arg = call->args.items[0];
    if (arg->kind == NK_IF_EXPR) return cond(arg->condition);
    return NULL;
}

static char *emit_key_eq_expr(const char *type, const char *left, const char *right) {
    if (type_eq(type, "STR")) return strf("flx_str_eq(%s, %s)", left, right);
    return strf("(%s == %s)", left, right);
}

static char *emit_sort_swap_condition(const char *type, const char *left, const char *right, const char *direction) {
    bool desc = direction && strcmp(direction, "DESC") == 0;
    if (type_eq(type, "STR")) {
        return desc ? strf("flx_str_compare_gt(%s, %s)", right, left) : strf("flx_str_compare_lt(%s, %s)", right, left);
    }
    return desc ? strf("(%s > %s)", right, left) : strf("(%s < %s)", right, left);
}

static char *emit_stmt_text(Node *st, int in, Node *fn, MethodEmitCtx *method_ctx) {
    char *pad = indent_text(in);

    switch (st->kind) {
        case NK_FN:
            return xstrdup("");
        case NK_VAR:
            return strf("%s%s %s%s%s;\n%s(void)%s;\n",
                pad,
                ctype(st->declared_type),
                st->name,
                st->value ? " = " : "",
                st->value ? emit_expr(st->value) : "",
                pad,
                st->name);
        case NK_DESTRUCTURE: {
            char *tmp = strf("__caster_destructure_%d", g_destructure_tmp_id++);
            char *out = strf("%s%s %s = %s;\n",
                pad,
                ctype(st->declared_type),
                tmp,
                emit_assignment_value_expr(st->declared_type, st->value));

            for (int i = 0; i < st->fields.len; i++) {
                Node *binding = st->fields.items[i];
                char *field_expr = strf("%s.%s", tmp, binding->text);
                out = strf("%s%s%s %s = %s;\n%s(void)%s;\n",
                    out,
                    pad,
                    ctype(binding->declared_type),
                    binding->name,
                    emit_heap_field_copy(binding->declared_type, field_expr),
                    pad,
                    binding->name);
            }
            return out;
        }
        case NK_ASSIGN:
            if (st->inferred_decl) {
                return strf("%s%s %s = %s;\n%s(void)%s;\n", pad, ctype(st->declared_type), st->name, emit_expr(st->value), pad, st->name);
            }
            if (st->op && strcmp(st->op, "++") == 0) return strf("%s%s++;\n", pad, emit_expr(st->target));
            if (st->op && strcmp(st->op, "+=") == 0) {
                if (is_open_struct_field_dot(st->target)) return strf("%s%s;\n", pad, emit_open_field_update(st->target, emit_join_expr(st->target, st->value, st->target->checked_type)));
                if (type_eq(st->target->checked_type, "INT") || type_eq(st->target->checked_type, "FLT")) return strf("%s%s += %s;\n", pad, emit_expr(st->target), emit_expr(st->value));
                if (type_eq(st->target->checked_type, "STR")) return strf("%s%s = flx_str_concat(%s, %s);\n", pad, emit_expr(st->target), emit_expr(st->target), emit_expr(st->value));
                return strf("%s%s = %s;\n", pad, emit_expr(st->target), emit_join_expr(st->target, st->value, st->target->checked_type));
            }
            if (st->op && strcmp(st->op, "-=") == 0) {
                if (is_open_struct_field_dot(st->target)) return strf("%s%s;\n", pad, emit_open_field_update(st->target, emit_remove_expr(st->target, st->value, st->target->checked_type)));
                if (type_eq(st->target->checked_type, "INT") || type_eq(st->target->checked_type, "FLT")) return strf("%s%s -= %s;\n", pad, emit_expr(st->target), emit_expr(st->value));
                return strf("%s%s = %s;\n", pad, emit_expr(st->target), emit_remove_expr(st->target, st->value, st->target->checked_type));
            }
            if (is_open_struct_field_dot(st->target)) return strf("%s%s;\n", pad, emit_open_field_assignment(st->target, st->value));
            if (is_ref_struct_field_dot(st->target)) return strf("%s%s;\n", pad, emit_ref_field_assignment(st->target, st->value));
            return strf("%s%s = %s;\n", pad, emit_expr(st->target), emit_expr(st->value));
        case NK_AGG_STEP:
            if (method_ctx && strcmp(method_ctx->method, "agg") == 0) return emit_agg_step_text(st, in, method_ctx);
            return strf("%s/* agg step outside agg body */\n", pad);
        case NK_EXPR_STMT: {
            if (method_ctx && st->method_value) return emit_method_value_text(st->expr, in, method_ctx);
            if (is_mutating_collection_plus_expr(st->expr)) return emit_mutating_collection_plus_stmt_text(st->expr, in);
            if (is_mutating_collection_method_expr(st->expr)) return emit_mutating_collection_method_stmt_text(st->expr, in);
            if (is_log_call(st->expr)) {
                char *out = xstrdup("");
                for (int i = 0; i < st->expr->args.len; i++) {
                    Node *arg = st->expr->args.items[i];
                    if (type_eq(arg->checked_type, "INT")) out = strf("%s%sflx_log_int(%s);\n", out, pad, emit_expr(arg));
                    else if (type_eq(arg->checked_type, "FLT")) out = strf("%s%sflx_log_flt(%s);\n", out, pad, emit_expr(arg));
                    else if (type_eq(arg->checked_type, "BOL")) out = strf("%s%sflx_log_bol(%s);\n", out, pad, emit_expr(arg));
                    else if (type_eq(arg->checked_type, "OBJ")) out = strf("%s%sflx_log_obj(%s);\n", out, pad, emit_expr(arg));
                    else out = strf("%s%sflx_log_str(%s);\n", out, pad, emit_expr(arg));
                }
                return out;
            }
            return strf("%s%s;\n", pad, emit_expr(st->expr));
        }
        case NK_RET:
            if (method_ctx) return strf("%s/* ret is not allowed in collection method bodies */\n", pad);
            if (st->value) return strf("%sreturn %s;\n", pad, emit_expr(st->value));
            if (fn && strcmp(fn->name, "main") == 0) return strf("%sreturn 0;\n", pad);
            return strf("%sreturn;\n", pad);
        case NK_PASS:
            return strf("%s;\n", pad);
        case NK_THROW:
            return strf(
                "%s{\n"
                "%s    FLX_STR throw_message = %s;\n"
                "%s    fprintf(stderr, \"%%.*s\\n\", (int)throw_message.len, throw_message.data);\n"
                "%s    exit(1);\n"
                "%s}\n",
                pad, pad, emit_expr(st->value), pad, pad, pad);
        case NK_IF: {
            char *out = strf("%sif (%s) {\n%s%s}", pad, cond(st->condition), emit_block_text(st->then_block, in + 1, fn, method_ctx), pad);
            for (int i = 0; i < st->elx_branches.len; i++) {
                Node *branch = st->elx_branches.items[i];
                out = strf("%s else if (%s) {\n%s%s}", out, cond(branch->condition), emit_block_text(branch->body, in + 1, fn, method_ctx), pad);
            }
            if (st->else_block) out = strf("%s else {\n%s%s}", out, emit_block_text(st->else_block, in + 1, fn, method_ctx), pad);
            return strf("%s\n", out);
        }
        case NK_LOOP:
            if (strcmp(st->loop_kind, "count") == 0) {
                return strf("%sfor (int64_t %s = 0; %s < %s; %s++) {\n%s%s}\n",
                    pad, st->index_name, st->index_name, emit_expr(st->args.items[0]), st->index_name,
                    emit_block_text(st->body, in + 1, fn, method_ctx), pad);
            }
            if (strcmp(st->loop_kind, "range") == 0) {
                char *step = st->args.len == 3 ? emit_expr(st->args.items[2]) : "1";
                return strf("%sfor (int64_t %s = %s; ((%s) >= 0 ? %s < %s : %s > %s); %s += %s) {\n%s%s}\n",
                    pad,
                    st->index_name,
                    emit_expr(st->args.items[0]),
                    step,
                    st->index_name,
                    emit_expr(st->args.items[1]),
                    st->index_name,
                    emit_expr(st->args.items[1]),
                    st->index_name,
                    step,
                    emit_block_text(st->body, in + 1, fn, method_ctx),
                    pad);
            }
            if (strcmp(st->loop_kind, "condition") == 0) {
                return strf("%sfor (int64_t %s = 0; %s; %s++) {\n%s    (void)%s;\n%s%s}\n",
                    pad, st->index_name, cond(st->args.items[0]), st->index_name,
                    pad, st->index_name,
                    emit_block_text(st->body, in + 1, fn, method_ctx), pad);
            }
            if (strcmp(st->loop_kind, "map") == 0 || strcmp(st->loop_kind, "open_map") == 0) {
                char *obj = emit_obj(st->args.items[0]);
                char *arg_type = ((Node *)st->args.items[0])->checked_type;
                if (strcmp(st->loop_kind, "open_map") == 0 && type_eq(arg_type, "OBJ")) obj = strf("flx_obj_as_objmap(%s)", obj);
                char *entry_type = strcmp(st->loop_kind, "open_map") == 0 ? xstrdup("FLX_OBJ_ENTRY") : map_entry_ctype(arg_type);
                return strf(
                    "%s{\n"
                    "%s    size_t iter_%s = 0;\n"
                    "%s    void* item_%s = NULL;\n"
                    "%s    for (int64_t %s = 0; %s.raw && hashmap_iter(%s.raw, &iter_%s, &item_%s); %s++) {\n"
                    "%s        %s* %s = item_%s;\n"
                    "%s        (void)%s;\n"
                    "%s        (void)%s;\n"
                    "%s%s    }\n"
                    "%s}\n",
                    pad,
                    pad, st->index_name,
                    pad, st->index_name,
                    pad, st->index_name, obj, obj, st->index_name, st->index_name, st->index_name,
                    pad, entry_type, st->element_ptr, st->index_name,
                    pad, st->index_name,
                    pad, st->element_ptr,
                    emit_block_text(st->body, in + 2, fn, method_ctx),
                    pad,
                    pad);
            }
            if (strcmp(st->loop_kind, "str") == 0) {
                char *obj = emit_obj(st->value ? st->value : st->args.items[0]);
                return strf("%sfor (int64_t %s = 0; %s < %s.len; %s++) {\n%s    FLX_STR %s = flx_str_char_at(%s, %s);\n%s    (void)%s;\n%s%s}\n",
                    pad, st->index_name, st->index_name, obj, st->index_name,
                    pad, st->element_ptr, obj, st->index_name,
                    pad, st->element_ptr,
                    emit_block_text(st->body, in + 1, fn, method_ctx), pad);
            }
            {
                char *obj = emit_obj(st->args.items[0]);
                char *element_type = array_elem_type(((Node *)st->args.items[0])->checked_type);
                return strf("%sfor (int64_t %s = 0; %s < %s.len; %s++) {\n%s    %s* %s = &%s.data[%s];\n%s    (void)%s;\n%s%s}\n",
                    pad, st->index_name, st->index_name, obj, st->index_name,
                    pad, ctype(element_type), st->element_ptr, obj, st->index_name,
                    pad, st->element_ptr,
                    emit_block_text(st->body, in + 1, fn, method_ctx), pad);
            }
        default:
            return strf("%s/* bad stmt */\n", pad);
    }
}

static char *emit_block_text(Node *b, int in, Node *fn, MethodEmitCtx *method_ctx) {
    char *out = xstrdup("");
    for (int i = 0; i < b->statements.len; i++) out = strf("%s%s", out, emit_stmt_text(b->statements.items[i], in, fn, method_ctx));
    return out;
}

static char *emit_collection_method(Node *e) {
    char *src_type = e->object->checked_type;
    char *src_name = strf("source_%s", e->index_name);
    char *out_name = strf("result_%s", e->index_name);
    char *iter_done = strf("next_%s", e->index_name);
    char *method_done = strf("done_%s", e->index_name);

    char *code = xstrdup("({\n");
    code = strf("%s    %s %s = %s;\n", code, ctype(src_type), src_name, emit_expr(e->object));

    if (strcmp(e->name, "tap") == 0) {
        code = strf("%s%s", code, emit_block_text(e->body, 1, NULL, NULL));
        code = strf("%s    %s;\n})", code, src_name);
        return code;
    }

    if (strcmp(e->name, "group") == 0) {
        char *group_type = array_elem_type(e->checked_type);
        char *key_type = e->declared_type;
        char *value_type = array_type(array_elem_type(src_type));
        char *key_name = strf("key_%s", e->index_name);
        char *found_name = strf("found_%s", e->index_name);
        char *group_i = strf("group_%s", e->index_name);
        char *new_group = strf("new_group_%s", e->index_name);
        char *elem_type = array_elem_type(src_type);
        char *elem_expr = strf("(*%s)", e->element_ptr);

        code = strf("%s    %s %s = %s(NULL, 0);\n", code, ctype(e->checked_type), out_name, arr_make(e->checked_type));
        code = strf("%s    for (int64_t %s = 0; %s < %s.len; %s++) {\n", code, e->index_name, e->index_name, src_name, e->index_name);
        code = strf("%s        %s* %s = &%s.data[%s];\n", code, ctype(elem_type), e->element_ptr, src_name, e->index_name);
        code = strf("%s        %s %s = %s;\n", code, ctype(key_type), key_name, emit_expr(e->args.items[0]));
        code = strf("%s        bool %s = false;\n", code, found_name);
        code = strf("%s        for (int64_t %s = 0; %s < %s.len; %s++) {\n", code, group_i, group_i, out_name, group_i);
        code = strf("%s            if (%s) {\n", code, emit_key_eq_expr(key_type, strf("%s.data[%s].key", out_name, group_i), key_name));
        code = strf("%s                %s.data[%s].val = %s(%s.data[%s].val, %s);\n", code, out_name, group_i, arr_add(value_type), out_name, group_i, elem_expr);
        code = strf("%s                %s = true;\n", code, found_name);
        code = strf("%s                break;\n", code);
        code = strf("%s            }\n", code);
        code = strf("%s        }\n", code);
        code = strf("%s        if (!%s) {\n", code, found_name);
        code = strf("%s            %s %s = { .key = %s, .val = %s(NULL, 0) };\n", code, ctype(group_type), new_group, key_name, arr_make(value_type));
        code = strf("%s            %s.val = %s(%s.val, %s);\n", code, new_group, arr_add(value_type), new_group, elem_expr);
        code = strf("%s            %s = %s(%s, %s);\n", code, out_name, arr_add(e->checked_type), out_name, new_group);
        code = strf("%s        }\n", code);
        code = strf("%s    }\n", code);
        code = strf("%s    %s;\n})", code, out_name);
        return code;
    }

    if (strcmp(e->name, "sort") == 0) {
        char *elem_type = array_elem_type(src_type);
        char *key_type = e->declared_type;
        char *left_name = strf("left_%s", e->index_name);
        char *right_name = strf("right_%s", e->index_name);
        char *outer_i = strf("sort_outer_%s", e->index_name);
        char *inner_i = strf("sort_inner_%s", e->index_name);
        char *tmp_name = strf("tmp_%s", e->index_name);

        code = strf("%s    %s %s = %s(%s.data, %s.len);\n", code, ctype(src_type), out_name, arr_make(src_type), src_name, src_name);
        code = strf("%s    for (int64_t %s = 0; %s < %s.len; %s++) {\n", code, outer_i, outer_i, out_name, outer_i);
        code = strf("%s        for (int64_t %s = %s + 1; %s < %s.len; %s++) {\n", code, inner_i, outer_i, inner_i, out_name, inner_i);
        code = strf("%s            %s* %s = &%s.data[%s];\n", code, ctype(elem_type), e->element_ptr, out_name, outer_i);
        code = strf("%s            %s %s = %s;\n", code, ctype(key_type), left_name, emit_expr(e->args.items[0]));
        code = strf("%s            %s = &%s.data[%s];\n", code, e->element_ptr, out_name, inner_i);
        code = strf("%s            %s %s = %s;\n", code, ctype(key_type), right_name, emit_expr(e->args.items[0]));
        code = strf("%s            if (%s) {\n", code, emit_sort_swap_condition(key_type, left_name, right_name, e->sort_dir));
        code = strf("%s                %s %s = %s.data[%s];\n", code, ctype(elem_type), tmp_name, out_name, outer_i);
        code = strf("%s                %s.data[%s] = %s.data[%s];\n", code, out_name, outer_i, out_name, inner_i);
        code = strf("%s                %s.data[%s] = %s;\n", code, out_name, inner_i, tmp_name);
        code = strf("%s            }\n", code);
        code = strf("%s        }\n", code);
        code = strf("%s    }\n", code);
        code = strf("%s    %s;\n})", code, out_name);
        return code;
    }

    if (is_map_type(src_type)) {
        char *entry_type = map_entry_ctype(src_type);
        char *elem_expr = strf("%s->value", e->element_ptr);
        code = strf("%s    %s %s = %s;\n", code, ctype(e->checked_type), out_name, emit_zero_value(e->checked_type));
        code = strf("%s    size_t iter_%s = 0;\n    void* item_%s = NULL;\n", code, e->index_name, e->index_name);
        code = strf("%s    for (int64_t %s = 0; %s.raw && hashmap_iter(%s.raw, &iter_%s, &item_%s); %s++) {\n",
            code, e->index_name, src_name, src_name, e->index_name, e->index_name, e->index_name);
        code = strf("%s        %s* %s = item_%s;\n        (void)%s;\n        (void)%s;\n",
            code, entry_type, e->element_ptr, e->index_name, e->element_ptr, e->index_name);
        if (e->body) {
            MethodEmitCtx method_ctx = {
                .method = e->name,
                .out_name = out_name,
                .out_type = e->checked_type,
                .iter_done_label = iter_done,
                .method_done_label = method_done,
                .source_elem_expr = elem_expr
            };
            code = strf("%s%s", code, emit_block_text(e->body, 2, NULL, &method_ctx));
            code = strf("%s%s:\n        ;\n", code, iter_done);
        } else {
            char *value = emit_agg_value(e, elem_expr);
            char *condition = emit_agg_condition(e);
            char *stmt = emit_accumulate_stmt(out_name, e->checked_type, value);
            if (condition) code = strf("%s        if (%s) %s\n", code, condition, stmt);
            else code = strf("%s        %s\n", code, stmt);
        }
        code = strf("%s    }\n    %s;\n})", code, out_name);
        return code;
    }

    bool string_source = type_eq(src_type, "STR");
    char *elem_type = string_source ? xstrdup("STR") : array_elem_type(src_type);
    char *elem_expr = string_source ? e->element_ptr : strf("(*%s)", e->element_ptr);

    if (strcmp(e->name, "upd") == 0 || strcmp(e->name, "filt") == 0) {
        if (string_source) code = strf("%s    FLX_STR %s = flx_str_lit(\"\", 0);\n", code, out_name);
        else code = strf("%s    %s %s = %s(NULL, 0);\n", code, ctype(e->checked_type), out_name, arr_make(e->checked_type));
    } else if (strcmp(e->name, "agg") == 0) {
        code = strf("%s    %s %s = %s;\n", code, ctype(e->checked_type), out_name, emit_zero_value(e->checked_type));
    } else if (strcmp(e->name, "any") == 0) {
        code = strf("%s    bool %s = false;\n", code, out_name);
    }

    code = strf("%s    for (int64_t %s = 0; %s < %s.len; %s++) {\n", code, e->index_name, e->index_name, src_name, e->index_name);
    if (string_source) code = strf("%s        FLX_STR %s = flx_str_char_at(%s, %s);\n", code, e->element_ptr, src_name, e->index_name);
    else code = strf("%s        %s* %s = &%s.data[%s];\n", code, ctype(elem_type), e->element_ptr, src_name, e->index_name);
    code = strf("%s        (void)%s;\n", code, e->element_ptr);

    MethodEmitCtx method_ctx = {
        .method = e->name,
        .out_name = out_name,
        .out_type = e->checked_type,
        .iter_done_label = iter_done,
        .method_done_label = method_done,
        .source_elem_expr = elem_expr
    };

    if (e->body) {
        code = strf("%s%s", code, emit_block_text(e->body, 2, NULL, &method_ctx));
        code = strf("%s%s:\n        ;\n", code, iter_done);
    } else if (strcmp(e->name, "upd") == 0) {
        if (string_source) code = strf("%s        %s = flx_str_concat(%s, %s);\n", code, out_name, out_name, emit_expr(e->args.items[0]));
        else code = strf("%s        %s = %s(%s, %s);\n", code, out_name, arr_add(e->checked_type), out_name, emit_expr(e->args.items[0]));
    } else if (strcmp(e->name, "filt") == 0) {
        if (string_source) code = strf("%s        if (%s) %s = flx_str_concat(%s, %s);\n", code, cond(e->args.items[0]), out_name, out_name, elem_expr);
        else code = strf("%s        if (%s) %s = %s(%s, %s);\n", code, cond(e->args.items[0]), out_name, arr_add(e->checked_type), out_name, elem_expr);
    } else if (strcmp(e->name, "agg") == 0) {
        char *value = emit_agg_value(e, elem_expr);
        char *condition = emit_agg_condition(e);
        char *stmt = emit_accumulate_stmt(out_name, e->checked_type, value);
        if (condition) code = strf("%s        if (%s) %s\n", code, condition, stmt);
        else code = strf("%s        %s\n", code, stmt);
    } else if (strcmp(e->name, "any") == 0) {
        if (e->body) {
            MethodEmitCtx any_ctx = {
                .method = e->name,
                .out_name = out_name,
                .out_type = e->checked_type,
                .iter_done_label = iter_done,
                .method_done_label = method_done,
                .source_elem_expr = elem_expr
            };
            code = strf("%s%s", code, emit_block_text(e->body, 2, NULL, &any_ctx));
            code = strf("%s%s:\n        ;\n", code, iter_done);
        } else {
            code = strf("%s        if (%s) { %s = true; break; }\n", code, cond(e->args.items[0]), out_name);
        }
    }

    code = strf("%s    }\n", code);
    code = strf("%s    %s;\n})", code, out_name);
    return code;
}

static char *emit_mutation_target_expr(Node *target) {
    if (target && target->checked_type && is_ref_type(target->checked_type)) {
        return strf("(*%s)", emit_expr(target));
    }
    return emit_expr(target);
}

static char *emit_join_from_target_expr(Node *target, Node *right, const char *type) {
    char *target_expr = emit_mutation_target_expr(target);

    if (is_array_type(type)) {
        if (type_eq(right->checked_type, type)) {
            return strf("%s(%s, %s)", arr_concat(type), target_expr, emit_expr(right));
        }
        return strf("%s(%s, %s)", arr_add(type), target_expr, emit_expr(right));
    }

    if (is_map_type(type)) {
        return strf("%s(%s, %s)", map_fn(type, "join"), target_expr, emit_expr(right));
    }

    if (type_eq(type, "OPEN")) {
        return strf("flx_objmap_join(%s, %s)", target_expr, emit_expr(right));
    }

    if (is_ref_type(target->checked_type) && is_struct_type(type)) {
        return xstrdup("/* mutating ref struct join is not implemented */");
    }

    return emit_join_expr(target, right, type);
}

static char *emit_mutating_collection_plus_stmt_text(Node *expr, int in) {
    Node *target = expr->left->expr;
    char *target_type = target->checked_type;
    char *type = is_ref_type(target_type) ? ref_target_type(target_type) : target_type;
    char *pad = indent_text(in);
    return strf("%s%s = %s;\n", pad, emit_mutation_target_expr(target), emit_join_from_target_expr(target, expr->right, type));
}

static char *emit_mutating_collection_add_stmt_text(Node *expr, int in) {
    Node *call = expr->expr;
    char *receiver_type = call->object->checked_type;
    char *array_type = is_ref_type(receiver_type) ? ref_target_type(receiver_type) : receiver_type;
    char *pad = indent_text(in);
    return strf("%s%s = %s;\n", pad, emit_mutation_target_expr(call->object), emit_join_from_target_expr(call->object, call->args.items[0], array_type));
}

static char *emit_mutating_collection_method_stmt_text(Node *expr, int in) {
    Node *call = expr->expr;
    if (strcmp(call->name, "add") == 0) return emit_mutating_collection_add_stmt_text(expr, in);

    char *receiver_type = call->object->checked_type;
    bool ref_receiver = is_ref_type(receiver_type);
    char *array_type = ref_receiver ? ref_target_type(receiver_type) : receiver_type;
    char *element_type = array_elem_type(array_type);
    char *target_name = strf("target_%s", call->index_name);
    char *iter_done = strf("next_%s", call->index_name);
    char *write_name = strf("write_%s", call->index_name);
    char *receiver_expr = emit_expr(call->object);
    char *target_expr = ref_receiver ? receiver_expr : strf("&%s", receiver_expr);
    char *elem_expr = strf("(*%s)", call->element_ptr);
    char *pad = indent_text(in);
    char *loop_pad = indent_text(in + 1);
    char *body_pad = indent_text(in + 2);

    char *code = strf("%s{\n", pad);
    code = strf("%s%s%s* %s = %s;\n", code, loop_pad, ctype(array_type), target_name, target_expr);
    if (strcmp(call->name, "filt") == 0) {
        code = strf("%s%sint64_t %s = 0;\n", code, loop_pad, write_name);
    }
    code = strf("%s%sfor (int64_t %s = 0; %s < %s->len; %s++) {\n",
        code, loop_pad, call->index_name, call->index_name, target_name, call->index_name);
    code = strf("%s%s%s* %s = &%s->data[%s];\n", code, body_pad, ctype(element_type), call->element_ptr, target_name, call->index_name);
    code = strf("%s%s(void)%s;\n", code, body_pad, call->element_ptr);

    if (call->body) {
        MethodEmitCtx method_ctx = {
            .method = call->name,
            .out_name = NULL,
            .out_type = array_type,
            .iter_done_label = iter_done,
            .method_done_label = NULL,
            .source_elem_expr = elem_expr,
            .in_place = true,
            .target_name = target_name,
            .write_name = write_name,
            .index_name = call->index_name
        };
        code = strf("%s%s", code, emit_block_text(call->body, in + 2, NULL, &method_ctx));
        code = strf("%s%s%s:\n%s    ;\n", code, body_pad, iter_done, body_pad);
    } else if (strcmp(call->name, "upd") == 0) {
        code = strf("%s%s%s = %s;\n", code, body_pad, elem_expr, emit_expr(call->args.items[0]));
    } else {
        code = strf(
            "%s%sif (%s) {\n"
            "%s    if (%s != %s) %s->data[%s] = %s->data[%s];\n"
            "%s    %s++;\n"
            "%s}\n",
            code, body_pad, cond(call->args.items[0]),
            body_pad, write_name, call->index_name, target_name, write_name, target_name, call->index_name,
            body_pad, write_name,
            body_pad);
    }

    code = strf("%s%s}\n", code, loop_pad);
    if (strcmp(call->name, "filt") == 0) {
        code = strf("%s%s%s->len = %s;\n", code, loop_pad, target_name, write_name);
    }
    code = strf("%s%s}\n", code, pad);
    return code;
}

static bool block_ends_with_return(Node *b) {
    if (!b || !b->statements.len) return false;
    Node *last = b->statements.items[b->statements.len - 1];
    return last && last->kind == NK_RET;
}

static void emit_block(FILE *f, Node *b, int in, Node *fn) {
    for (int i = 0; i < b->statements.len; i++) {
        Node *st = b->statements.items[i];
        if (st->kind == NK_FN) continue;
        emit_stmt(f, st, in, fn);
    }
}

static void emit_stmt(FILE *f, Node *st, int in, Node *fn) {
    if (st->kind == NK_EXPR_STMT && is_mutating_collection_plus_expr(st->expr)) {
        fprintf(f, "%s", emit_mutating_collection_plus_stmt_text(st->expr, in));
        return;
    }

    if (st->kind == NK_EXPR_STMT && is_mutating_collection_method_expr(st->expr)) {
        fprintf(f, "%s", emit_mutating_collection_method_stmt_text(st->expr, in));
        return;
    }

    outi(f, in);
    switch (st->kind) {
        case NK_FN:
            break;
        case NK_VAR:
            fprintf(f, "%s %s", ctype(st->declared_type), st->name);
            if (st->value) fprintf(f, " = %s", emit_assignment_value_expr(st->declared_type, st->value));
            fprintf(f, ";\n");
            if (in == 1 && assignment_initializer_may_own(st->declared_type, st->value)) owned_local_add(st->name, st->declared_type);
            outi(f, in);
            fprintf(f, "(void)%s;\n", st->name);
            break;
        case NK_DESTRUCTURE: {
            char *tmp = strf("__caster_destructure_%d", g_destructure_tmp_id++);
            fprintf(f, "%s %s = %s;\n", ctype(st->declared_type), tmp, emit_assignment_value_expr(st->declared_type, st->value));
            if (in == 1 && assignment_initializer_may_own(st->declared_type, st->value)) owned_local_add(tmp, st->declared_type);

            for (int i = 0; i < st->fields.len; i++) {
                Node *binding = st->fields.items[i];
                char *field_expr = strf("%s.%s", tmp, binding->text);
                outi(f, in);
                fprintf(f, "%s %s = %s;\n",
                    ctype(binding->declared_type),
                    binding->name,
                    emit_heap_field_copy(binding->declared_type, field_expr));
                if (in == 1 && type_needs_drop(binding->declared_type)) owned_local_add(binding->name, binding->declared_type);
                outi(f, in);
                fprintf(f, "(void)%s;\n", binding->name);
            }
            break;
        }
        case NK_ASSIGN:
            if (st->inferred_decl) {
                fprintf(f, "%s %s = %s;\n", ctype(st->declared_type), st->name, emit_assignment_value_expr(st->declared_type, st->value));
                if (in == 1 && assignment_initializer_may_own(st->declared_type, st->value)) owned_local_add(st->name, st->declared_type);
                outi(f, in);
                fprintf(f, "(void)%s;\n", st->name);
            } else if (st->op && strcmp(st->op, "++") == 0) {
                fprintf(f, "%s++;\n", emit_expr(st->target));
            } else if (st->op && strcmp(st->op, "+=") == 0) {
                if (in == 1 && st->target && st->target->kind == NK_NAME) owned_local_add(st->target->name, st->target->checked_type);
                if (is_open_struct_field_dot(st->target)) {
                    fprintf(f, "%s;\n", emit_open_field_update(st->target, emit_join_expr(st->target, st->value, st->target->checked_type)));
                } else if (type_eq(st->target->checked_type, "INT") || type_eq(st->target->checked_type, "FLT")) fprintf(f, "%s += %s;\n", emit_expr(st->target), emit_expr(st->value));
                else if (type_eq(st->target->checked_type, "STR")) fprintf(f, "%s = flx_str_concat(%s, %s);\n", emit_expr(st->target), emit_expr(st->target), emit_expr(st->value));
                else fprintf(f, "%s = %s;\n", emit_expr(st->target), emit_join_expr(st->target, st->value, st->target->checked_type));
            } else if (st->op && strcmp(st->op, "-=") == 0) {
                if (in == 1 && st->target && st->target->kind == NK_NAME) owned_local_add(st->target->name, st->target->checked_type);
                if (is_open_struct_field_dot(st->target)) {
                    fprintf(f, "%s;\n", emit_open_field_update(st->target, emit_remove_expr(st->target, st->value, st->target->checked_type)));
                } else if (type_eq(st->target->checked_type, "INT") || type_eq(st->target->checked_type, "FLT")) {
                    fprintf(f, "%s -= %s;\n", emit_expr(st->target), emit_expr(st->value));
                } else {
                    fprintf(f, "%s = %s;\n", emit_expr(st->target), emit_remove_expr(st->target, st->value, st->target->checked_type));
                }
            } else {
                if (in == 1 && st->target && st->target->kind == NK_NAME && assignment_initializer_may_own(st->target->checked_type, st->value)) {
                    owned_local_add(st->target->name, st->target->checked_type);
                }
                if (is_open_struct_field_dot(st->target)) fprintf(f, "%s;\n", emit_open_field_assignment(st->target, st->value));
                else if (is_ref_struct_field_dot(st->target)) fprintf(f, "%s;\n", emit_ref_field_assignment(st->target, st->value));
                else fprintf(f, "%s = %s;\n", emit_expr(st->target), emit_assignment_value_expr(st->target->checked_type, st->value));
            }
            break;
        case NK_FREE: {
            emit_drop_stmt_with_prefix(f, in, st->expr->checked_type, emit_expr(st->expr), true);
            if (st->expr->kind == NK_NAME) owned_local_remove(st->expr->name);
            outi(f, in);
            fprintf(f, "%s = %s;\n", emit_expr(st->expr), emit_zero_value(st->expr->checked_type));
            break;
        }
        case NK_EXPR_STMT:
            if (is_log_call(st->expr)) {
                for (int i = 0; i < st->expr->args.len; i++) emit_log_stmt(f, st->expr->args.items[i], in);
            } else {
                fprintf(f, "%s;\n", emit_expr(st->expr));
            }
            break;
        case NK_RET:
            if (st->value) {
                char *tmp = strf("flx_return_value_%d", g_return_tmp_id++);
                fprintf(f, "%s %s = %s;\n", function_return_ctype(fn), tmp, emit_return_value_expr(st->value));
                emit_owned_local_drops(f, in, returned_owned_local_name(st->value));
                outi(f, in);
                fprintf(f, "return %s;\n", tmp);
            } else {
                emit_owned_local_drops(f, in, NULL);
                outi(f, in);
                if (strcmp(fn->name, "main") == 0) fprintf(f, "return 0;\n");
                else fprintf(f, "return;\n");
            }
            break;
        case NK_PASS:
            fprintf(f, ";\n");
            break;
        case NK_THROW:
            fprintf(f, "{\n");
            outi(f, in + 1);
            fprintf(f, "FLX_STR throw_message = %s;\n", emit_expr(st->value));
            outi(f, in + 1);
            fprintf(f, "fprintf(stderr, \"%%.*s\\n\", (int)throw_message.len, throw_message.data);\n");
            outi(f, in + 1);
            fprintf(f, "exit(1);\n");
            outi(f, in);
            fprintf(f, "}\n");
            break;
        case NK_IF:
            fprintf(f, "if (%s) {\n", cond(st->condition));
            emit_block(f, st->then_block, in + 1, fn);
            outi(f, in);
            fprintf(f, "}");
            for (int i = 0; i < st->elx_branches.len; i++) {
                Node *branch = st->elx_branches.items[i];
                fprintf(f, " else if (%s) {\n", cond(branch->condition));
                emit_block(f, branch->body, in + 1, fn);
                outi(f, in);
                fprintf(f, "}");
            }
            if (st->else_block) {
                fprintf(f, " else {\n");
                emit_block(f, st->else_block, in + 1, fn);
                outi(f, in);
                fprintf(f, "}");
            }
            fprintf(f, "\n");
            break;
        case NK_LOOP:
            if (strcmp(st->loop_kind, "count") == 0) {
                fprintf(f, "for (int64_t %s = 0; %s < %s; %s++) {\n", st->index_name, st->index_name, emit_expr(st->args.items[0]), st->index_name);
                emit_block(f, st->body, in + 1, fn);
                outi(f, in);
                fprintf(f, "}\n");
            } else if (strcmp(st->loop_kind, "range") == 0) {
                char *step = st->args.len == 3 ? emit_expr(st->args.items[2]) : "1";
                fprintf(f, "for (int64_t %s = %s; ((%s) >= 0 ? %s < %s : %s > %s); %s += %s) {\n",
                    st->index_name,
                    emit_expr(st->args.items[0]),
                    step,
                    st->index_name,
                    emit_expr(st->args.items[1]),
                    st->index_name,
                    emit_expr(st->args.items[1]),
                    st->index_name,
                    step);
                emit_block(f, st->body, in + 1, fn);
                outi(f, in);
                fprintf(f, "}\n");
            } else if (strcmp(st->loop_kind, "condition") == 0) {
                fprintf(f, "for (int64_t %s = 0; %s; %s++) {\n", st->index_name, cond(st->args.items[0]), st->index_name);
                outi(f, in + 1);
                fprintf(f, "(void)%s;\n", st->index_name);
                emit_block(f, st->body, in + 1, fn);
                outi(f, in);
                fprintf(f, "}\n");
            } else if (strcmp(st->loop_kind, "map") == 0 || strcmp(st->loop_kind, "open_map") == 0) {
                char *obj = emit_obj(st->args.items[0]);
                char *arg_type = ((Node *)st->args.items[0])->checked_type;
                if (strcmp(st->loop_kind, "open_map") == 0 && type_eq(arg_type, "OBJ")) obj = strf("flx_obj_as_objmap(%s)", obj);
                char *entry_type = strcmp(st->loop_kind, "open_map") == 0 ? xstrdup("FLX_OBJ_ENTRY") : map_entry_ctype(arg_type);
                fprintf(f, "{\n");
                outi(f, in + 1);
                fprintf(f, "size_t iter_%s = 0;\n", st->index_name);
                outi(f, in + 1);
                fprintf(f, "void* item_%s = NULL;\n", st->index_name);
                outi(f, in + 1);
                fprintf(f, "for (int64_t %s = 0; %s.raw && hashmap_iter(%s.raw, &iter_%s, &item_%s); %s++) {\n",
                    st->index_name, obj, obj, st->index_name, st->index_name, st->index_name);
                outi(f, in + 2);
                fprintf(f, "%s* %s = item_%s;\n", entry_type, st->element_ptr, st->index_name);
                outi(f, in + 2);
                fprintf(f, "(void)%s;\n", st->index_name);
                outi(f, in + 2);
                fprintf(f, "(void)%s;\n", st->element_ptr);
                emit_block(f, st->body, in + 2, fn);
                outi(f, in + 1);
                fprintf(f, "}\n");
                outi(f, in);
                fprintf(f, "}\n");
            } else if (strcmp(st->loop_kind, "str") == 0) {
                char *obj = emit_obj(st->value ? st->value : st->args.items[0]);
                fprintf(f, "for (int64_t %s = 0; %s < %s.len; %s++) {\n", st->index_name, st->index_name, obj, st->index_name);
                outi(f, in + 1);
                fprintf(f, "FLX_STR %s = flx_str_char_at(%s, %s);\n", st->element_ptr, obj, st->index_name);
                outi(f, in + 1);
                fprintf(f, "(void)%s;\n", st->element_ptr);
                emit_block(f, st->body, in + 1, fn);
                outi(f, in + 1);
                fprintf(f, "flx_drop_str(%s);\n", st->element_ptr);
                outi(f, in);
                fprintf(f, "}\n");
            } else {
                char *obj = emit_obj(st->args.items[0]);
                char *element_type = array_elem_type(((Node *)st->args.items[0])->checked_type);
                fprintf(f, "for (int64_t %s = 0; %s < %s.len; %s++) {\n", st->index_name, st->index_name, obj, st->index_name);
                outi(f, in + 1);
                fprintf(f, "%s* %s = &%s.data[%s];\n", ctype(element_type), st->element_ptr, obj, st->index_name);
                outi(f, in + 1);
                fprintf(f, "(void)%s;\n", st->element_ptr);
                emit_block(f, st->body, in + 1, fn);
                outi(f, in);
                fprintf(f, "}\n");
            }
            break;
        default:
            fprintf(f, "/* bad stmt */\n");
            break;
    }
}
