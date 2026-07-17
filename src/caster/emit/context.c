typedef struct {
    bool log_int;
    bool log_flt;
    bool log_bol;
    bool log_str;
    bool log_obj;
    bool uses_assert;
    bool uses_assert_msg;
    bool uses_json;
    bool uses_str;
    bool uses_str_eq;
    bool uses_str_concat;
    bool uses_throw;
    bool uses_heap;
    bool uses_map;
    bool uses_open;
    bool uses_req;
    bool uses_sys;
    bool uses_cast;
    bool uses_buf;
    bool uses_sql;
    bool uses_rand;
    bool uses_req_headers;
    PtrVec array_types;
    PtrVec array_concat_types;
    PtrVec array_sub_value_types;
    PtrVec map_types;
    PtrVec fn_types;
    PtrVec json_decode_types;
    PtrVec host_ctx_calls;
} EmitCtx;

static bool g_emit_uses_req = false;
static bool g_emit_uses_json = false;
static bool g_emit_uses_sys = false;
static bool g_emit_uses_sql = false;

static char *emit_expr(Node *e);
static void emit_stmt(FILE *f, Node *st, int in, Node *fn);
static char *emit_collection_method(Node *e);
static char *function_return_ctype(Node *fn);

static bool type_in_vec(PtrVec *v, const char *type) {
    for (int i = 0; i < v->len; i++) {
        if (strcmp(v->items[i], type) == 0) return true;
    }
    return false;
}

static void add_type_use(EmitCtx *ctx, const char *type);

static void add_json_decode_type(EmitCtx *ctx, const char *type) {
    if (!type) return;
    if (!is_struct_type(type) && !is_map_type(type) && !is_array_type(type)) return;

    if (!type_in_vec(&ctx->json_decode_types, type)) vec_push(&ctx->json_decode_types, xstrdup(type));
    add_type_use(ctx, type);

    if (is_struct_type(type)) {
        TypeInfo *info = type_find(g_types, type);
        for (int i = 0; info && i < info->node->fields.len; i++) {
            Node *field = info->node->fields.items[i];
            add_json_decode_type(ctx, field->declared_type);
        }
        return;
    }

    if (is_map_type(type)) {
        add_json_decode_type(ctx, map_value_type(type));
        return;
    }

    if (is_array_type(type)) {
        add_json_decode_type(ctx, array_elem_type(type));
    }
}

static void add_type_use(EmitCtx *ctx, const char *type) {
    if (!type) return;
    if (is_unknown_type(type)) return;
    if (is_shape_type(type)) return;

    if (type_eq(type, "STR")) {
        ctx->uses_str = true;
        return;
    }

    if (type_eq(type, "OBJ") || type_eq(type, "OPEN")) {
        ctx->uses_open = true;
        ctx->uses_str = true;
        return;
    }

    if (is_open_struct_type(type)) {
        ctx->uses_open = true;
        ctx->uses_map = true;
        ctx->uses_str = true;
        return;
    }

    if (is_ref_type(type)) {
        add_type_use(ctx, ref_target_type(type));
        return;
    }

    if (is_task_type(type)) {
        ctx->uses_req = true;
        ctx->uses_str = true;
        add_type_use(ctx, task_result_type(type));
        return;
    }

    if (is_fn_type(type)) {
        if (!type_in_vec(&ctx->fn_types, type)) vec_push(&ctx->fn_types, xstrdup(type));
        add_type_use(ctx, fn_input_type(type));
        add_type_use(ctx, fn_output_type(type));
        return;
    }

    if (is_array_type(type)) {
        if (!type_in_vec(&ctx->array_types, type)) vec_push(&ctx->array_types, xstrdup(type));
        char *element_type = array_elem_type(type);
        add_type_use(ctx, element_type);
    }

    if (is_map_type(type)) {
        ctx->uses_map = true;
        if (!type_in_vec(&ctx->map_types, type)) vec_push(&ctx->map_types, xstrdup(type));
        add_type_use(ctx, map_key_type(type));
        add_type_use(ctx, map_value_type(type));
    }
}

static void collect_expr(EmitCtx *ctx, Node *e);

static void collect_block(EmitCtx *ctx, Node *b) {
    for (int i = 0; i < b->statements.len; i++) {
        Node *st = b->statements.items[i];
        add_type_use(ctx, st->checked_type);

        switch (st->kind) {
            case NK_FN:
                add_type_use(ctx, st->checked_type);
                for (int j = 0; j < st->params.len; j++) add_type_use(ctx, ((Node *)st->params.items[j])->declared_type);
                collect_block(ctx, st->body);
                break;
            case NK_VAR:
                add_type_use(ctx, st->declared_type);
                if (st->value) collect_expr(ctx, st->value);
                break;
            case NK_DESTRUCTURE:
                add_type_use(ctx, st->declared_type);
                if (st->value) collect_expr(ctx, st->value);
                for (int j = 0; j < st->fields.len; j++) {
                    Node *binding = st->fields.items[j];
                    add_type_use(ctx, binding->declared_type);
                }
                break;
            case NK_ASSIGN:
                add_type_use(ctx, st->declared_type);
                collect_expr(ctx, st->target);
                if (st->value) collect_expr(ctx, st->value);
                if (st->op && strcmp(st->op, "+=") == 0 && st->target && st->value &&
                    is_array_type(st->target->checked_type) && is_array_type(st->value->checked_type)) {
                    if (!type_in_vec(&ctx->array_concat_types, st->target->checked_type)) {
                        vec_push(&ctx->array_concat_types, xstrdup(st->target->checked_type));
                    }
                }
                if (st->op && strcmp(st->op, "+=") == 0 && st->target && type_eq(st->target->checked_type, "STR")) {
                    ctx->uses_str_concat = true;
                }
                if (st->op && strcmp(st->op, "-=") == 0 && st->target && is_array_type(st->target->checked_type)) {
                    if (!type_in_vec(&ctx->array_sub_value_types, st->target->checked_type)) {
                        vec_push(&ctx->array_sub_value_types, xstrdup(st->target->checked_type));
                    }
                    if (type_eq(array_elem_type(st->target->checked_type), "STR")) ctx->uses_str_eq = true;
                }
                break;
            case NK_AGG_STEP:
                if (st->value) collect_expr(ctx, st->value);
                if (st->op && strcmp(st->op, "+=") == 0 && st->value && type_eq(st->value->checked_type, "STR")) {
                    ctx->uses_str_concat = true;
                }
                break;
            case NK_EXPR_STMT:
                collect_expr(ctx, st->expr);
                break;
            case NK_RET:
                if (st->value) collect_expr(ctx, st->value);
                break;
            case NK_THROW:
                ctx->uses_throw = true;
                collect_expr(ctx, st->value);
                break;
            case NK_LOOP:
                for (int j = 0; j < st->args.len; j++) collect_expr(ctx, st->args.items[j]);
                collect_block(ctx, st->body);
                break;
            case NK_IF:
                collect_expr(ctx, st->condition);
                collect_block(ctx, st->then_block);
                for (int j = 0; j < st->elx_branches.len; j++) {
                    Node *branch = st->elx_branches.items[j];
                    collect_expr(ctx, branch->condition);
                    collect_block(ctx, branch->body);
                }
                if (st->else_block) collect_block(ctx, st->else_block);
                break;
            default:
                break;
        }
    }
}

static void collect_expr(EmitCtx *ctx, Node *e) {
    add_type_use(ctx, e->checked_type);
    if (e->c_expr && e->declared_type &&
        (is_struct_type(e->declared_type) || is_map_type(e->declared_type) || is_array_type(e->declared_type))) {
        ctx->uses_json = true;
        ctx->uses_open = true;
        ctx->uses_str = true;
        add_type_use(ctx, e->declared_type);
        add_json_decode_type(ctx, e->declared_type);
    }

    switch (e->kind) {
        case NK_BINARY:
            if (strcmp(e->op, "else") == 0 && e->left && type_eq(e->left->checked_type, "OBJ")) {
                ctx->uses_open = true;
                if (is_struct_type(e->checked_type) || is_map_type(e->checked_type) || is_array_type(e->checked_type)) {
                    ctx->uses_json = true;
                    add_json_decode_type(ctx, e->checked_type);
                }
            }
            if (strcmp(e->op, "+") == 0) {
                char *join_type = e->checked_type;
                if (is_mutating_collection_plus_expr(e) && e->left && e->left->kind == NK_UNARY) {
                    char *target_type = e->left->expr ? e->left->expr->checked_type : NULL;
                    if (target_type && is_ref_type(target_type)) target_type = ref_target_type(target_type);
                    if (target_type) join_type = target_type;
                }
                if (join_type && is_array_type(join_type) && e->right && type_eq(e->right->checked_type, join_type)) {
                    if (!type_in_vec(&ctx->array_concat_types, join_type)) {
                        vec_push(&ctx->array_concat_types, xstrdup(join_type));
                    }
                }
            }
            if (type_eq(e->left->checked_type, "STR") && strcmp(e->op, "+") == 0) ctx->uses_str_concat = true;
            if (type_eq(e->left->checked_type, "STR") && (strcmp(e->op, "==") == 0 || strcmp(e->op, "!=") == 0)) ctx->uses_str_eq = true;
            collect_expr(ctx, e->left);
            collect_expr(ctx, e->right);
            break;
        case NK_UNARY:
            collect_expr(ctx, e->expr);
            break;
        case NK_ARRAY:
            for (int i = 0; i < e->elements.len; i++) collect_expr(ctx, e->elements.items[i]);
            break;
        case NK_SHAPE:
            for (int i = 0; i < e->elements.len; i++) collect_expr(ctx, e->elements.items[i]);
            break;
        case NK_MAP_LITERAL:
        case NK_RECORD_LITERAL:
            for (int i = 0; i < e->fields.len; i++) {
                Node *field = e->fields.items[i];
                if (field->target) collect_expr(ctx, field->target);
                if (field->value) collect_expr(ctx, field->value);
            }
            break;
        case NK_DOT:
            collect_expr(ctx, e->object);
            if (e->index) collect_expr(ctx, e->index);
            if (e->target) collect_expr(ctx, e->target);
            if (e->index && e->object && type_eq(e->object->checked_type, "STR")) ctx->uses_str = true;
            if (e->name && strcmp(e->name, "headers") == 0 && e->object && type_eq(e->object->checked_type, "HttpReq")) {
                ctx->uses_req_headers = true;
            }
            break;
        case NK_CALL:
            if (e->callee->kind != NK_NAME) collect_expr(ctx, e->callee);
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "log") == 0) {
                for (int i = 0; i < e->args.len; i++) {
                    Node *arg = e->args.items[i];
                    if (type_eq(arg->checked_type, "INT")) ctx->log_int = true;
                    else if (type_eq(arg->checked_type, "FLT")) ctx->log_flt = true;
                    else if (type_eq(arg->checked_type, "BOL")) ctx->log_bol = true;
                    else if (type_eq(arg->checked_type, "STR")) ctx->log_str = true;
                    else if (type_eq(arg->checked_type, "OBJ")) ctx->log_obj = true;
                }
            }
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "assert") == 0) {
                ctx->uses_assert = true;
                if (e->args.len == 2) {
                    ctx->uses_assert_msg = true;
                    ctx->uses_str = true;
                }
            }
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "JSON") == 0) {
                ctx->uses_json = true;
                ctx->uses_open = true;
                ctx->uses_str = true;
            }
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "RAND") == 0) {
                ctx->uses_rand = true;
            }
            if (e->callee->kind == NK_NAME) {
                char *name = e->callee->name;
                if (is_cast_call_name(name)) {
                    ctx->uses_cast = true;
                    ctx->uses_str = true;
                }
                if (strcmp(name, "trim") == 0 || strcmp(name, "lower") == 0 ||
                    strcmp(name, "upper") == 0 || strcmp(name, "urlDecode") == 0 || strcmp(name, "replace") == 0 || strcmp(name, "starts") == 0 ||
                    strcmp(name, "ends") == 0 || strcmp(name, "isDigit") == 0 || strcmp(name, "isAlpha") == 0 ||
                    strcmp(name, "isSpace") == 0 || strcmp(name, "has") == 0 || strcmp(name, "find") == 0 ||
                    strcmp(name, "min") == 0 || strcmp(name, "max") == 0) {
                    ctx->uses_str = true;
                }
                if ((strcmp(name, "has") == 0 || strcmp(name, "find") == 0) && e->args.len == 2) {
                    Node *collection = e->args.items[0];
                    if (collection->checked_type && is_array_type(collection->checked_type) && type_eq(array_elem_type(collection->checked_type), "STR")) {
                        ctx->uses_str_eq = true;
                    }
                }
            }
            for (int i = 0; i < e->args.len; i++) collect_expr(ctx, e->args.items[i]);
            break;
        case NK_METHOD_CALL:
            if (e->object && e->object->kind == NK_NAME &&
                (strcmp(e->object->name, "OS") == 0 || strcmp(e->object->name, "FS") == 0 ||
                 strcmp(e->object->name, "PATH") == 0 || strcmp(e->object->name, "IO") == 0 ||
                 strcmp(e->object->name, "PROC") == 0)) {
                ctx->uses_sys = true;
                ctx->uses_str = true;
                ctx->uses_str_concat = true;
                add_type_use(ctx, "ARR[STR]");
                add_type_use(ctx, "ARR[INT]");
                add_type_use(ctx, "FileStat");
                add_type_use(ctx, "ProcResult");
                for (int i = 0; i < e->args.len; i++) collect_expr(ctx, e->args.items[i]);
                break;
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->object->name, "BUF") == 0) {
                ctx->uses_buf = true;
                ctx->uses_str = true;
                add_type_use(ctx, "Buffer");
                for (int i = 0; i < e->args.len; i++) collect_expr(ctx, e->args.items[i]);
                break;
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->object->name, "SQL") == 0) {
                ctx->uses_sql = true;
                ctx->uses_open = true;
                ctx->uses_str = true;
                add_type_use(ctx, "SQL_DB");
                add_type_use(ctx, "SQL_Exec");
                for (int i = 0; i < e->args.len; i++) collect_expr(ctx, e->args.items[i]);
                break;
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->object->name, "JSON") == 0 && strcmp(e->name, "stringify") == 0) {
                ctx->uses_json = true;
                ctx->uses_open = true;
                ctx->uses_str = true;
                for (int i = 0; i < e->args.len; i++) collect_expr(ctx, e->args.items[i]);
                break;
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->object->name, "REQ") == 0) {
                ctx->uses_req = true;
                ctx->uses_str = true;
                ctx->uses_map = true;
                add_type_use(ctx, "HttpReq");
                add_type_use(ctx, "HttpRes");
                if ((strcmp(e->name, "host") == 0 && e->args.len == 3) || (strcmp(e->name, "hostTLS") == 0 && e->args.len == 5)) vec_push(&ctx->host_ctx_calls, e);
                for (int i = 0; i < e->args.len; i++) {
                    if (strcmp(e->name, "host") == 0 && ((e->args.len == 2 && i == 1) || (e->args.len == 3 && i == 2))) continue;
                    if (strcmp(e->name, "hostTLS") == 0 && ((e->args.len == 4 && i == 3) || (e->args.len == 5 && i == 4))) continue;
                    if (strcmp(e->name, "ws") == 0 && e->args.len == 2 && i == 1) continue;
                    if (strcmp(e->name, "wsTLS") == 0 && e->args.len == 4 && i == 3) continue;
                    collect_expr(ctx, e->args.items[i]);
                }
                break;
            }
            if (strcmp(e->name, "ARR") == 0 || strcmp(e->name, "STR") == 0) {
                ctx->uses_cast = true;
                ctx->uses_str = true;
                add_type_use(ctx, "ARR[STR]");
                collect_expr(ctx, e->object);
                for (int i = 0; i < e->args.len; i++) collect_expr(ctx, e->args.items[i]);
                break;
            }
            if (e->c_expr) {
                for (int i = 0; i < e->args.len; i++) collect_expr(ctx, e->args.items[i]);
                break;
            }
            collect_expr(ctx, e->object);
            for (int i = 0; i < e->args.len; i++) collect_expr(ctx, e->args.items[i]);
            if (e->body) collect_block(ctx, e->body);
            if (strcmp(e->name, "add") == 0 && e->object && is_array_type(e->checked_type) && e->args.len == 1) {
                Node *arg = e->args.items[0];
                if (type_eq(arg->checked_type, e->checked_type) && !type_in_vec(&ctx->array_concat_types, e->checked_type)) {
                    vec_push(&ctx->array_concat_types, xstrdup(e->checked_type));
                }
            }
            if (type_eq(e->object->checked_type, "STR")) {
                ctx->uses_str = true;
                if (strcmp(e->name, "upd") == 0 || strcmp(e->name, "filt") == 0 || strcmp(e->name, "agg") == 0) {
                    ctx->uses_str_concat = true;
                }
            }
            if (strcmp(e->name, "agg") == 0 && type_eq(e->checked_type, "STR")) ctx->uses_str_concat = true;
            if (strcmp(e->name, "group") == 0 && e->declared_type && type_eq(e->declared_type, "STR")) ctx->uses_str_eq = true;
            if (strcmp(e->name, "sort") == 0 && e->declared_type && type_eq(e->declared_type, "STR")) ctx->uses_str = true;
            break;
        case NK_DECODE:
            ctx->uses_json = true;
            ctx->uses_open = true;
            ctx->uses_str = true;
            add_type_use(ctx, e->declared_type);
            add_json_decode_type(ctx, e->declared_type);
            collect_expr(ctx, e->expr);
            break;
        case NK_IF_EXPR:
            collect_expr(ctx, e->value);
            collect_expr(ctx, e->condition);
            if (e->right) collect_expr(ctx, e->right);
            break;
        default:
            break;
    }
}
