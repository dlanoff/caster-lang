static Node *record_entry(Node *record, const char *name) {
    for (int i = 0; i < record->fields.len; i++) {
        Node *entry = record->fields.items[i];
        if (entry->name && strcmp(entry->name, name) == 0) return entry;
    }
    return NULL;
}

static char *emit_host_adapter_name(Node *call) {
    return strf("caster_host_adapter_%d_%d", call->line, call->col);
}

static bool is_struct_constructor_expr(Node *value) {
    return value &&
        (value->kind == NK_CALL || value->kind == NK_METHOD_CALL) &&
        value->declared_type &&
        is_struct_type(value->declared_type) &&
        value->args.len == 1;
}

static char *emit_field_default(Node *field) {
    if (field->value && field->value->kind == NK_INIT) return strf("%s()", emit_named_value_fn_name(field->text));
    if (field->value) return emit_expr(field->value);
    TypeInfo *info = field->text ? type_find(g_types, field->text) : NULL;
    if (info && type_has_complete_named_value(g_types, info)) return strf("%s()", emit_named_value_fn_name(info->name));
    if (is_array_type(field->declared_type)) return strf("%s(NULL, 0)", arr_make(field->declared_type));
    if (is_map_type(field->declared_type)) return strf("%s(NULL, 0)", map_fn(field->declared_type, "make"));
    if (is_ref_type(field->declared_type) || is_fn_type(field->declared_type)) return xstrdup("NULL");
    return xstrdup("/*missing_default*/");
}

static bool field_value_should_copy(Node *value) {
    if (!value) return false;
    if (is_struct_constructor_expr(value)) return field_value_should_copy(value->args.items[0]);
    if (value->kind == NK_NAME || value->kind == NK_DOT) return true;
    if (value->kind == NK_UNARY && value->op && strcmp(value->op, "*") == 0) return true;
    return false;
}

static char *emit_heap_field_copy(const char *type, const char *value) {
    if (type_eq(type, "STR")) return strf("flx_str_copy(%s)", value);
    if (is_array_type(type)) return strf("%s(%s.data, %s.len)", arr_make(type), value, value);
    if (is_map_type(type)) return strf("%s(%s)", map_fn(type, "clone"), value);
    if (type_eq(type, "OPEN") || is_open_struct_type(type)) return strf("flx_objmap_clone(%s)", value);
    if (is_struct_type(type)) return strf("%s(%s)", struct_copy_fn_for_type(type), value);
    return xstrdup(value);
}

static char *emit_record_field_value(Node *field, Node *entry) {
    char *value = entry ? emit_expr(entry->value) : emit_field_default(field);
    if (entry && field_value_should_copy(entry->value)) {
        return emit_heap_field_copy(field->declared_type, value);
    }
    return value;
}

static char *emit_dynamic_field_value(Node *value_node) {
    char *value = emit_expr(value_node);
    if (field_value_should_copy(value_node)) return emit_heap_field_copy(value_node->checked_type, value);
    return value;
}

static char *emit_user_call_arg(Node *arg) {
    char *value = emit_expr(arg);
    if (!field_value_should_copy(arg)) return value;
    if (is_task_type(arg->checked_type)) return value;
    return emit_heap_field_copy(arg->checked_type, value);
}

static char *emit_user_call_args(PtrVec *args_vec) {
    char *args = xstrdup("");
    for (int i = 0; i < args_vec->len; i++) {
        args = strf("%s%s%s", args, i ? ", " : "", emit_user_call_arg(args_vec->items[i]));
    }
    return args;
}

static char *emit_open_anonymous_record_literal(Node *record) {
    if (record->fields.len == 0) return xstrdup("flx_objmap_make(NULL, 0)");

    char *values = xstrdup("");
    for (int i = 0; i < record->fields.len; i++) {
        Node *entry = record->fields.items[i];
        char *value = emit_dynamic_field_value(entry->value);
        values = strf("%s%s{ %s, %s }",
            values,
            i ? ", " : "",
            emit_key_literal(entry->name),
            emit_obj_value(entry->value->checked_type, value));
    }
    return strf("flx_objmap_make((FLX_OBJ_ENTRY[]){%s}, %d)", values, record->fields.len);
}

static char *emit_open_map_literal(Node *map) {
    if (map->fields.len == 0) return xstrdup("flx_objmap_make(NULL, 0)");

    char *values = xstrdup("");
    for (int i = 0; i < map->fields.len; i++) {
        Node *entry = map->fields.items[i];
        char *value = emit_dynamic_field_value(entry->value);
        values = strf("%s%s{ %s, %s }",
            values,
            i ? ", " : "",
            emit_expr(entry->target),
            emit_obj_value(entry->value->checked_type, value));
    }
    return strf("flx_objmap_make((FLX_OBJ_ENTRY[]){%s}, %d)", values, map->fields.len);
}

static char *emit_open_record_literal(Node *record, const char *type) {
    TypeInfo *info = type_find(g_types, type);
    if (!info || !info->node->fields.len) return xstrdup("flx_objmap_make(NULL, 0)");

    char *values = xstrdup("");
    for (int i = 0; i < info->node->fields.len; i++) {
        Node *field = info->node->fields.items[i];
        Node *entry = record_entry(record, field->name);
        char *value = emit_record_field_value(field, entry);
        values = strf("%s%s{ %s, %s }",
            values,
            i ? ", " : "",
            emit_key_literal(field->name),
            emit_obj_value(field->declared_type, value));
    }
    return strf("flx_objmap_make((FLX_OBJ_ENTRY[]){%s}, %d)", values, info->node->fields.len);
}

static char *emit_open_named_value(TypeInfo *info) {
    if (!info->node->fields.len) return xstrdup("flx_objmap_make(NULL, 0)");

    char *values = xstrdup("");
    for (int i = 0; i < info->node->fields.len; i++) {
        Node *field = info->node->fields.items[i];
        values = strf("%s%s{ %s, %s }",
            values,
            i ? ", " : "",
            emit_key_literal(field->name),
            emit_obj_value(field->declared_type, emit_field_default(field)));
    }
    return strf("flx_objmap_make((FLX_OBJ_ENTRY[]){%s}, %d)", values, info->node->fields.len);
}

static char *emit_record_overlay(Node *left, Node *record, const char *type) {
    if (is_open_struct_type(type)) {
        char *out = emit_obj(left);
        for (int i = 0; i < record->fields.len; i++) {
            Node *entry = record->fields.items[i];
            Node *field = struct_field(g_types, type, entry->name);
            out = emit_obj_set_typed(out, emit_key_literal(entry->name), field->declared_type, emit_record_field_value(field, entry));
        }
        return out;
    }

    TypeInfo *info = type_find(g_types, type);
    char *fields = xstrdup("");
    for (int i = 0; info && i < info->node->fields.len; i++) {
        Node *field = info->node->fields.items[i];
        Node *entry = record_entry(record, field->name);
        char *value = entry
            ? emit_record_field_value(field, entry)
            : emit_heap_field_copy(field->declared_type, strf("%s.%s", emit_obj(left), field->name));
        fields = strf("%s%s.%s = %s", fields, i ? ", " : "", field->name, value);
    }
    return strf("(%s){ %s }", ctype(type), fields);
}

static char *emit_join_expr(Node *left, Node *right, const char *type) {
    if (is_array_type(type)) {
        if (type_eq(right->checked_type, type)) {
            return strf("%s(%s, %s)", arr_concat(type), emit_expr(left), emit_expr(right));
        }
        return strf("%s(%s, %s)", arr_add(type), emit_expr(left), emit_expr(right));
    }

    if (is_map_type(type)) {
        return strf("%s(%s, %s)", map_fn(type, "join"), emit_expr(left), emit_expr(right));
    }

    if (type_eq(type, "OPEN")) {
        return strf("flx_objmap_join(%s, %s)", emit_expr(left), emit_expr(right));
    }

    if (is_struct_type(type)) {
        if (is_open_struct_type(type) && right->kind == NK_MAP_LITERAL) {
            return strf("flx_objmap_join(%s, %s)", emit_expr(left), emit_expr(right));
        }
        if (right->kind == NK_RECORD_LITERAL) return emit_record_overlay(left, right, type);
        return emit_expr(right);
    }

    return xstrdup("/*badjoin*/");
}

static char *emit_remove_expr(Node *left, Node *right, const char *type) {
    if (is_array_type(type)) return strf("%s(%s, %s)", arr_sub_value(type), emit_expr(left), emit_expr(right));
    if (is_map_type(type)) return strf("%s(%s, %s)", map_fn(type, "sub"), emit_expr(left), emit_expr(right));
    if (is_open_struct_type(type)) return strf("flx_objmap_sub(%s, %s)", emit_expr(left), emit_expr(right));
    return xstrdup("/*badremove*/");
}

static char *emit_arr_conversion_expr(Node *object, const char *result_type) {
    if (is_array_type(object->checked_type)) return emit_expr(object);

    char *out_name = strf("arr_wrap_%d_%d", object->line, object->col);
    return strf("({ %s %s = %s(NULL, 0); %s = %s(%s, %s); %s; })",
        ctype(result_type),
        out_name,
        arr_make(result_type),
        out_name,
        arr_add(result_type),
        out_name,
        emit_expr(object),
        out_name);
}

static char *emit_shape_fill_level(Node *call, const char *array_type_name, const char *out_name, int level) {
    Node *shape = call->object;
    int last = level == shape->elements.len - 1;
    char *idx_name = strf("fill_i_%d_%d_%d", call->line, call->col, level);
    char *dim_name = strf("fill_dim_%d_%d_%d", call->line, call->col, level);
    char *code = xstrdup("");

    code = strf("%s    for (int64_t %s = 0; %s < %s; %s++) {\n", code, idx_name, idx_name, dim_name, idx_name);
    if (last) {
        code = strf("%s        %s = %s(%s, %s);\n",
            code,
            out_name,
            arr_add(array_type_name),
            out_name,
            emit_expr(call->args.items[0]));
    } else {
        char *child_type = array_elem_type(array_type_name);
        char *child_name = strf("fill_child_%d_%d_%d", call->line, call->col, level);
        code = strf("%s        %s %s = %s(NULL, 0);\n",
            code,
            ctype(child_type),
            child_name,
            arr_make(child_type));
        code = strf("%s%s", code, emit_shape_fill_level(call, child_type, child_name, level + 1));
        code = strf("%s        %s = %s(%s, %s);\n",
            code,
            out_name,
            arr_add(array_type_name),
            out_name,
            child_name);
    }
    code = strf("%s    }\n", code);
    return code;
}

static char *emit_shape_fill_expr(Node *call) {
    Node *shape = call->object;
    char *out_name = strf("fill_out_%d_%d", call->line, call->col);
    char *code = xstrdup("({\n");

    for (int i = 0; i < shape->elements.len; i++) {
        char *dim_name = strf("fill_dim_%d_%d_%d", call->line, call->col, i);
        code = strf("%s    int64_t %s = %s;\n", code, dim_name, emit_expr(shape->elements.items[i]));
        code = strf("%s    if (%s < 0) { fprintf(stderr, \"caster: shape dimensions must be nonnegative\\n\"); exit(1); }\n", code, dim_name);
    }

    code = strf("%s    %s %s = %s(NULL, 0);\n",
        code,
        ctype(call->checked_type),
        out_name,
        arr_make(call->checked_type));
    code = strf("%s%s", code, emit_shape_fill_level(call, call->checked_type, out_name, 0));
    code = strf("%s    %s;\n})", code, out_name);
    return code;
}

static char *emit_obj_as_checked_type(const char *type, const char *obj_expr) {
    if (is_struct_type(type) || is_map_type(type)) return strf("%s(flx_obj_as_objmap(%s))", json_decode_fn(type), obj_expr);
    if (is_array_type(type)) return strf("%s(flx_obj_as_objarr(%s))", json_decode_fn(type), obj_expr);
    if (is_ref_type(type)) return strf("(%s)flx_obj_as_ref_nil(%s)", ctype(type), obj_expr);
    if (type_eq(type, "STR")) return strf("flx_str_copy(flx_obj_as_str(%s))", obj_expr);
    return strf("%s(%s)", obj_as_fn(type), obj_expr);
}

static char *emit_else_fallback_value(Node *fallback, const char *result_type) {
    char *value = emit_expr(fallback);
    if (field_value_should_copy(fallback)) return emit_heap_field_copy(result_type, value);
    return value;
}

static char *emit_truthy_expr(const char *type, const char *value_expr) {
    if (type_eq(type, "BOL")) return xstrdup(value_expr);
    if (type_eq(type, "INT")) return strf("(%s != 0)", value_expr);
    if (type_eq(type, "FLT")) return strf("(%s != 0.0)", value_expr);
    if (type_eq(type, "STR")) return strf("(%s.len != 0)", value_expr);
    if (type_eq(type, "OBJ")) return strf("flx_obj_truthy(%s)", value_expr);
    if (is_ref_type(type)) return strf("(%s != NULL)", value_expr);
    return xstrdup("true");
}

static char *emit_split_expr(Node *text, Node *sep);

static char *emit_cast_expr(Node *call) {
    const char *target = call->callee->name;
    Node *arg = call->args.items[0];
    const char *source = arg->checked_type;
    char *value = emit_expr(arg);

    if (strcmp(target, "ARR") == 0) {
        if (call->args.len == 2) return emit_split_expr(arg, call->args.items[1]);
        return strf("flx_str_chars(%s)", value);
    }

    if (strcmp(target, "INT") == 0) {
        if (type_eq(source, "INT")) return value;
        if (type_eq(source, "FLT")) return strf("((int64_t)(%s))", value);
        if (type_eq(source, "BOL")) return strf("((%s) ? 1 : 0)", value);
        return strf("flx_cast_str_to_int(%s)", value);
    }

    if (strcmp(target, "FLT") == 0) {
        if (type_eq(source, "FLT")) return value;
        if (type_eq(source, "INT")) return strf("((double)(%s))", value);
        if (type_eq(source, "BOL")) return strf("((%s) ? 1.0 : 0.0)", value);
        return strf("flx_cast_str_to_flt(%s)", value);
    }

    if (is_array_type(source) && type_eq(array_elem_type(source), "STR")) {
        if (call->args.len == 2) return strf("flx_arr_str_join_sep(%s, %s)", value, emit_expr(call->args.items[1]));
        return strf("flx_arr_str_join(%s)", value);
    }
    if (type_eq(source, "STR")) return strf("flx_str_copy(%s)", value);
    if (type_eq(source, "INT")) return strf("flx_cast_int_to_str(%s)", value);
    if (type_eq(source, "FLT")) return strf("flx_cast_flt_to_str(%s)", value);
    return strf("flx_cast_bol_to_str(%s)", value);
}

static char *emit_dynamic_dot_else_expr(Node *left, Node *fallback, const char *result_type) {
    if (left->kind != NK_DOT) return NULL;

    char *out = strf("fallback_%d_%d", left->line, left->col);

    if (left->index) {
        char *idx = strf("index_%d_%d", left->index->line, left->index->col);

        if (type_eq(left->object->checked_type, "OBJ")) {
            char *obj = strf("object_%d_%d", left->object->line, left->object->col);
            char *arr = strf("array_%d_%d", left->object->line, left->index->col);
            char *item = strf("item_%d_%d", left->line, left->col);
            return strf("({ %s %s; FLX_OBJ %s = %s; int64_t %s = %s; bool has_%s = false; if (flx_obj_truthy(%s)) { FLX_OBJARR %s = flx_obj_as_objarr(%s); if (%s >= 0 && %s < %s.len) { FLX_OBJ %s = %s.data[%s]; if (flx_obj_truthy(%s)) { %s = %s; has_%s = true; } } } if (!has_%s) %s = %s; %s; })",
                ctype(result_type), out,
                obj, emit_expr(left->object),
                idx, emit_expr(left->index),
                out,
                obj, arr, obj,
                idx, idx, arr,
                item, arr, idx,
                item, out, emit_obj_as_checked_type(result_type, item), out,
                out, out, emit_else_fallback_value(fallback, result_type), out);
        }

        if (is_array_type(left->object->checked_type)) {
            char *src = strf("array_%d_%d", left->object->line, left->object->col);
            char *item_type = array_elem_type(left->object->checked_type);
            char *item = strf("item_%d_%d", left->line, left->col);
            return strf("({ %s %s; %s %s = %s; int64_t %s = %s; bool has_%s = false; if (%s >= 0 && %s < %s.len) { %s %s = %s.data[%s]; if (%s) { %s = %s; has_%s = true; } } if (!has_%s) %s = %s; %s; })",
                ctype(result_type), out,
                ctype(left->object->checked_type), src, emit_expr(left->object),
                idx, emit_expr(left->index),
                out,
                idx, idx, src,
                ctype(item_type), item, src, idx,
                emit_truthy_expr(item_type, item),
                out, item, out,
                out, out, emit_else_fallback_value(fallback, result_type), out);
        }

        if (type_eq(left->object->checked_type, "STR")) {
            char *src = strf("text_%d_%d", left->object->line, left->object->col);
            return strf("({ %s %s; FLX_STR %s = %s; int64_t %s = %s; if (%s >= 0 && %s < %s.len) %s = flx_str_char_at(%s, %s); else %s = %s; %s; })",
                ctype(result_type), out,
                src, emit_expr(left->object),
                idx, emit_expr(left->index),
                idx, idx, src,
                out, src, idx,
                out, emit_else_fallback_value(fallback, result_type), out);
        }
    }

    if (left->target) {
        char *key = strf("key_%d_%d", left->target->line, left->target->col);

        if (is_map_type(left->object->checked_type)) {
            char *src = strf("map_%d_%d", left->object->line, left->object->col);
            char *item = strf("item_%d_%d", left->line, left->col);
            return strf("({ %s %s; %s %s = %s; FLX_STR %s = %s; bool has_%s = false; if (%s(%s, %s)) { %s %s = %s(%s, %s); if (%s) { %s = %s; has_%s = true; } } if (!has_%s) %s = %s; %s; })",
                ctype(result_type), out,
                ctype(left->object->checked_type), src, emit_expr(left->object),
                key, emit_expr(left->target),
                out,
                map_fn(left->object->checked_type, "has"), src, key,
                ctype(left->checked_type), item, map_fn(left->object->checked_type, "get"), src, key,
                emit_truthy_expr(left->checked_type, item),
                out, item, out,
                out, out, emit_else_fallback_value(fallback, result_type), out);
        }

        if (type_eq(left->object->checked_type, "OBJ")) {
            char *src = strf("map_%d_%d", left->object->line, left->object->col);
            char *item = strf("item_%d_%d", left->line, left->col);
            return strf("({ %s %s; FLX_OBJMAP %s = flx_obj_as_objmap(%s); FLX_STR %s = %s; bool has_%s = false; if (flx_objmap_has(%s, %s)) { FLX_OBJ %s = flx_objmap_get(%s, %s); if (flx_obj_truthy(%s)) { %s = %s; has_%s = true; } } if (!has_%s) %s = %s; %s; })",
                ctype(result_type), out,
                src, emit_expr(left->object),
                key, emit_expr(left->target),
                out,
                src, key,
                item, src, key,
                item, out, emit_obj_as_checked_type(result_type, item), out,
                out, out, emit_else_fallback_value(fallback, result_type), out);
        }

        if (type_eq(left->object->checked_type, "OPEN") || is_open_struct_type(left->object->checked_type)) {
            char *src = strf("map_%d_%d", left->object->line, left->object->col);
            char *item = strf("item_%d_%d", left->line, left->col);
            return strf("({ %s %s; FLX_OBJMAP %s = %s; FLX_STR %s = %s; bool has_%s = false; if (flx_objmap_has(%s, %s)) { FLX_OBJ %s = flx_objmap_get(%s, %s); if (flx_obj_truthy(%s)) { %s = %s; has_%s = true; } } if (!has_%s) %s = %s; %s; })",
                ctype(result_type), out,
                src, emit_expr(left->object),
                key, emit_expr(left->target),
                out,
                src, key,
                item, src, key,
                item, out, type_eq(result_type, "OBJ") ? xstrdup(item) : emit_obj_as_checked_type(result_type, item), out,
                out, out, emit_else_fallback_value(fallback, result_type), out);
        }
    }

    return NULL;
}

static char *emit_else_expr(Node *e) {
    char *special = emit_dynamic_dot_else_expr(e->left, e->right, e->checked_type);
    if (special) return special;

    char *left_type = e->left->checked_type;
    char *value = strf("value_%d_%d", e->left->line, e->left->col);
    if (type_eq(left_type, "OBJ")) {
        return strf("({ FLX_OBJ %s = %s; %s ? %s : %s; })",
            value,
            emit_expr(e->left),
            emit_truthy_expr(left_type, value),
            emit_obj_as_checked_type(e->checked_type, value),
            emit_expr(e->right));
    }

    return strf("({ %s %s = %s; %s ? %s : %s; })",
        ctype(left_type),
        value,
        emit_expr(e->left),
        emit_truthy_expr(left_type, value),
        value,
        emit_expr(e->right));
}

static char *emit_array_has_expr(Node *collection, Node *needle) {
    char *type = collection->checked_type;
    char *element_type = array_elem_type(type);
    char *src = strf("source_%d_%d", collection->line, collection->col);
    char *value = strf("needle_%d_%d", needle->line, needle->col);
    char *out = strf("found_%d_%d", collection->line, needle->col);
    char *eq = type_eq(element_type, "STR") ? strf("flx_str_eq(%s.data[i], %s)", src, value) : strf("%s.data[i] == %s", src, value);
    return strf("({ %s %s = %s; %s %s = %s; bool %s = false; for (int64_t i = 0; i < %s.len; i++) { if (%s) { %s = true; break; } } %s; })",
        ctype(type), src, emit_expr(collection),
        ctype(element_type), value, emit_expr(needle),
        out, src, eq, out, out);
}

static char *emit_array_find_expr(Node *collection, Node *needle) {
    char *type = collection->checked_type;
    char *element_type = array_elem_type(type);
    char *src = strf("source_%d_%d", collection->line, collection->col);
    char *value = strf("needle_%d_%d", needle->line, needle->col);
    char *out = strf("index_%d_%d", collection->line, needle->col);
    char *eq = type_eq(element_type, "STR") ? strf("flx_str_eq(%s.data[i], %s)", src, value) : strf("%s.data[i] == %s", src, value);
    return strf("({ %s %s = %s; %s %s = %s; int64_t %s = -1; for (int64_t i = 0; i < %s.len; i++) { if (%s) { %s = i; break; } } %s; })",
        ctype(type), src, emit_expr(collection),
        ctype(element_type), value, emit_expr(needle),
        out, src, eq, out, out);
}

static char *emit_minmax_expr(Node *collection, bool is_max) {
    char *type = collection->checked_type;
    if (type_eq(type, "STR")) {
        char *src = strf("source_%d_%d", collection->line, collection->col);
        char *idx = strf("best_%d_%d", collection->line, collection->col);
        char *cmp = is_max ? ">" : "<";
        return strf("({ FLX_STR %s = %s; int64_t %s = 0; for (int64_t i = 1; i < %s.len; i++) if (%s.data[i] %s %s.data[%s]) %s = i; %s.len ? flx_str_char_at(%s, %s) : flx_str_lit(\"\", 0); })",
            src, emit_expr(collection), idx, src, src, cmp, src, idx, idx, src, src, idx);
    }

    char *element_type = array_elem_type(type);
    char *src = strf("source_%d_%d", collection->line, collection->col);
    char *out = strf("best_%d_%d", collection->line, collection->col);
    char *cmp = is_max ? ">" : "<";
    if (type_eq(element_type, "STR")) {
        cmp = is_max ? "flx_str_compare_gt" : "flx_str_compare_lt";
        return strf("({ %s %s = %s; %s %s = %s.len ? %s.data[0] : flx_str_lit(\"\", 0); for (int64_t i = 1; i < %s.len; i++) if (%s(%s.data[i], %s)) %s = %s.data[i]; %s; })",
            ctype(type), src, emit_expr(collection), ctype(element_type), out, src, src, src, cmp, src, out, out, src, out);
    }
    return strf("({ %s %s = %s; %s %s = %s.len ? %s.data[0] : 0; for (int64_t i = 1; i < %s.len; i++) if (%s.data[i] %s %s) %s = %s.data[i]; %s; })",
        ctype(type), src, emit_expr(collection), ctype(element_type), out, src, src, src, src, cmp, out, out, src, out);
}

static char *emit_split_expr(Node *text, Node *sep) {
    char *src = strf("split_src_%d_%d", text->line, text->col);
    char *needle = strf("split_sep_%d_%d", sep->line, sep->col);
    char *out = strf("split_out_%d_%d", text->line, sep->col);
    return strf(
        "({ FLX_STR %s = %s; FLX_STR %s = %s; FLX_ARR_STR %s = flx_arr_str_make(NULL, 0); "
        "int64_t start = 0; for (int64_t i = 0; i <= %s.len; ) { "
        "bool match = %s.len > 0 && i <= %s.len - %s.len; "
        "for (int64_t j = 0; match && j < %s.len; j++) if (%s.data[i + j] != %s.data[j]) match = false; "
        "if (match || i == %s.len) { FLX_STR part = flx_str_alloc(i - start); for (int64_t k = start; k < i; k++) part.data[k - start] = %s.data[k]; %s = flx_arr_str_add(%s, part); i += match ? %s.len : 1; start = i; } "
        "else i++; } %s; })",
        src, emit_expr(text), needle, emit_expr(sep), out,
        src, needle, src, needle,
        needle, src, needle,
        src, src, out, out, needle, out);
}

static int cmp_record_field_name(const void *left_raw, const void *right_raw) {
    Node *left = *(Node **)left_raw;
    Node *right = *(Node **)right_raw;
    const char *left_name = left && left->name ? left->name : "";
    const char *right_name = right && right->name ? right->name : "";
    return strcmp(left_name, right_name);
}

static char *json_field_prefix_literal(const char *name) {
    return c_escape(strf("\"%s\":", name ? name : ""));
}

static bool can_emit_direct_json_value(Node *value) {
    if (!value) return false;
    if (type_eq(value->checked_type, "INT") || type_eq(value->checked_type, "FLT") ||
        type_eq(value->checked_type, "BOL") || type_eq(value->checked_type, "STR") ||
        type_eq(value->checked_type, "NUL")) {
        return true;
    }
    if (value->kind == NK_RECORD_LITERAL) {
        for (int i = 0; i < value->fields.len; i++) {
            Node *field = value->fields.items[i];
            if (!can_emit_direct_json_value(field->value)) return false;
        }
        return true;
    }
    if (value->kind == NK_ARRAY) {
        for (int i = 0; i < value->elements.len; i++) {
            if (!can_emit_direct_json_value(value->elements.items[i])) return false;
        }
        return true;
    }
    return false;
}

static char *emit_direct_json_value_statements(Node *value, const char *out_name, const char *buf_name);

static char *emit_direct_json_record_statements(Node *record, const char *out_name, const char *buf_name) {
    if (!can_emit_direct_json_value(record)) return NULL;

    char *code = strf("flx_json_out_append_c(&%s, '{');", out_name);
    Node **fields = record->fields.len ? xmalloc(sizeof(Node *) * (size_t)record->fields.len) : NULL;
    for (int i = 0; i < record->fields.len; i++) fields[i] = record->fields.items[i];
    if (record->fields.len > 1) qsort(fields, (size_t)record->fields.len, sizeof(Node *), cmp_record_field_name);

    for (int i = 0; i < record->fields.len; i++) {
        Node *field = fields[i];
        char *value_code = emit_direct_json_value_statements(field->value, out_name, buf_name);
        if (!value_code) return NULL;
        if (i) code = strf("%s flx_json_out_append_c(&%s, ',');", code, out_name);
        code = strf("%s flx_json_out_append_text(&%s, %s); %s",
            code,
            out_name,
            json_field_prefix_literal(field->name),
            value_code);
    }

    return strf("%s flx_json_out_append_c(&%s, '}');", code, out_name);
}

static char *emit_direct_json_array_statements(Node *array, const char *out_name, const char *buf_name) {
    if (!can_emit_direct_json_value(array)) return NULL;

    char *code = strf("flx_json_out_append_c(&%s, '[');", out_name);
    for (int i = 0; i < array->elements.len; i++) {
        char *value_code = emit_direct_json_value_statements(array->elements.items[i], out_name, buf_name);
        if (!value_code) return NULL;
        if (i) code = strf("%s flx_json_out_append_c(&%s, ',');", code, out_name);
        code = strf("%s %s", code, value_code);
    }
    return strf("%s flx_json_out_append_c(&%s, ']');", code, out_name);
}

static char *emit_direct_json_value_statements(Node *value, const char *out_name, const char *buf_name) {
    if (!can_emit_direct_json_value(value)) return NULL;

    if (value->kind == NK_RECORD_LITERAL) return emit_direct_json_record_statements(value, out_name, buf_name);
    if (value->kind == NK_ARRAY) return emit_direct_json_array_statements(value, out_name, buf_name);

    if (type_eq(value->checked_type, "STR")) {
        return strf("flx_json_stringify_string(&%s, %s);", out_name, emit_expr(value));
    }
    if (type_eq(value->checked_type, "INT")) {
        return strf("snprintf(%s, sizeof(%s), \"%%lld\", (long long)(%s)); flx_json_out_append_text(&%s, %s);",
            buf_name, buf_name, emit_expr(value), out_name, buf_name);
    }
    if (type_eq(value->checked_type, "FLT")) {
        return strf("snprintf(%s, sizeof(%s), \"%%.17g\", (double)(%s)); flx_json_out_append_text(&%s, %s);",
            buf_name, buf_name, emit_expr(value), out_name, buf_name);
    }
    if (type_eq(value->checked_type, "BOL")) {
        return strf("flx_json_out_append_text(&%s, (%s) ? \"true\" : \"false\");", out_name, emit_expr(value));
    }
    if (type_eq(value->checked_type, "NUL")) {
        return strf("flx_json_out_append_text(&%s, \"null\");", out_name);
    }
    return NULL;
}

static char *emit_direct_json_body_expr(Node *record) {
    char *out_name = strf("json_out_%d_%d", record->line, record->col);
    char *buf_name = strf("json_buf_%d_%d", record->line, record->col);
    char *code = emit_direct_json_record_statements(record, out_name, buf_name);
    if (!code) return NULL;
    return strf("({ FLX_STR %s = flx_str_alloc(0); char %s[64]; (void)%s; %s %s; })",
        out_name,
        buf_name,
        buf_name,
        code,
        out_name);
}

static char *emit_web_json_direct_response(Node *call) {
    if (!call->object || call->object->kind != NK_NAME || strcmp(call->object->name, "WEB") != 0) return NULL;
    if (strcmp(call->name, "json") != 0) return NULL;
    if (call->args.len < 1 || call->args.len > 2) return NULL;

    Node *body_arg = call->args.items[0];
    if (!body_arg || body_arg->kind != NK_RECORD_LITERAL || !can_emit_direct_json_value(body_arg)) return NULL;

    char *body_expr = emit_direct_json_body_expr(body_arg);
    if (!body_expr) return NULL;

    char *status_expr = call->args.len == 2 ? emit_expr(call->args.items[1]) : xstrdup("200");
    char *res_name = strf("json_res_%d_%d", call->line, call->col);
    char *headers_expr = strf(
        "%s((%s[]){{ flx_str_lit(\"content-type\", 12), flx_str_lit(\"application/json\", 16) }}, 1)",
        map_fn("MAP[STR,STR]", "make"),
        map_entry_ctype("MAP[STR,STR]"));

    return strf("({ FLX_HttpRes %s; %s.status = %s; %s.body = %s; %s.headers = %s; %s; })",
        res_name,
        res_name, status_expr,
        res_name, body_expr,
        res_name, headers_expr,
        res_name);
}

static char *emit_expr(Node *e) {
    switch (e->kind) {
        case NK_INT:
            return strf("%lld", (long long)e->int_value);
        case NK_FLT:
            return strf("%.17g", e->float_value);
        case NK_BOL:
            return xstrdup(e->bool_value ? "true" : "false");
        case NK_STR:
            return strf("flx_str_lit(%s, %zu)", c_escape(e->text), strlen(e->text));
        case NK_NIL:
            return xstrdup("NULL");
        case NK_NAME:
            return xstrdup(e->c_expr ? e->c_expr : e->name);
        case NK_INIT:
            return xstrdup("/*badinit*/");
        case NK_ARRAY: {
            if (type_eq(e->checked_type, "OBJ")) {
                char *values = xstrdup("");
                for (int i = 0; i < e->elements.len; i++) {
                    Node *item = e->elements.items[i];
                    values = strf("%s%s%s",
                        values,
                        i ? ", " : "",
                        emit_obj_value(item->checked_type, emit_expr(item)));
                }
                if (e->elements.len == 0) return xstrdup("flx_obj_from_objarr(flx_objarr_make(NULL, 0))");
                return strf("flx_obj_from_objarr(flx_objarr_make((FLX_OBJ[]){%s}, %d))", values, e->elements.len);
            }

            char *element_type = array_elem_type(e->checked_type);
            char *element_ctype = ctype(element_type);
            char *values = xstrdup("");
            for (int i = 0; i < e->elements.len; i++) {
                values = strf("%s%s%s", values, i ? ", " : "", emit_expr(e->elements.items[i]));
            }
            if (e->elements.len == 0) return strf("%s(NULL, 0)", arr_make(e->checked_type));
            return strf("%s((%s[]){%s}, %d)", arr_make(e->checked_type), element_ctype, values, e->elements.len);
        }
        case NK_SHAPE:
            return xstrdup("/* shape literal requires .fill(value) */");
        case NK_MAP_LITERAL: {
            if (is_struct_type(e->checked_type) && e->fields.len == 0) {
                TypeInfo *info = type_find(g_types, e->checked_type);
                char *fields = xstrdup("");
                for (int i = 0; info && i < info->node->fields.len; i++) {
                    Node *field = info->node->fields.items[i];
                    char *value = emit_field_default(field);
                    fields = strf("%s%s.%s = %s", fields, i ? ", " : "", field->name, value);
                }
                return strf("(%s){ %s }", ctype(e->checked_type), fields);
            }
            if (type_eq(e->checked_type, "OBJ")) return strf("flx_obj_from_objmap(%s)", emit_open_map_literal(e));
            if (is_open_struct_type(e->checked_type)) return emit_open_map_literal(e);

            char *entry_type = map_entry_ctype(e->checked_type);
            char *values = xstrdup("");
            for (int i = 0; i < e->fields.len; i++) {
                Node *entry = e->fields.items[i];
                values = strf("%s%s{ %s, %s }",
                    values,
                    i ? ", " : "",
                    emit_expr(entry->target),
                    emit_expr(entry->value));
            }
            return strf("%s((%s[]){%s}, %d)", map_fn(e->checked_type, "make"), entry_type, values, e->fields.len);
        }
        case NK_RECORD_LITERAL: {
            if (type_eq(e->checked_type, "OBJ")) return strf("flx_obj_from_objmap(%s)", emit_open_anonymous_record_literal(e));
            if (type_eq(e->checked_type, "OPEN")) return emit_open_anonymous_record_literal(e);
            if (is_open_struct_type(e->checked_type)) return emit_open_record_literal(e, e->checked_type);

            TypeInfo *info = type_find(g_types, e->checked_type);
            char *fields = xstrdup("");
            for (int i = 0; info && i < info->node->fields.len; i++) {
                Node *field = info->node->fields.items[i];
                Node *entry = record_entry(e, field->name);
                char *value = emit_record_field_value(field, entry);
                fields = strf("%s%s.%s = %s", fields, i ? ", " : "", field->name, value);
            }
            return strf("(%s){ %s }", ctype(e->checked_type), fields);
        }
        case NK_DOT:
            if (e->c_expr) return xstrdup(e->c_expr);
            if (e->index && type_eq(e->object->checked_type, "OBJ")) return strf("flx_objarr_get(flx_obj_as_objarr(%s), %s)", emit_expr(e->object), emit_expr(e->index));
            if (e->index && type_eq(e->object->checked_type, "STR")) return strf("flx_str_char_at(%s, %s)", emit_obj(e->object), emit_expr(e->index));
            if (e->index) return strf("%s.data[%s]", emit_obj(e->object), emit_expr(e->index));
            if (e->target && type_eq(e->object->checked_type, "OBJ")) {
                return strf("flx_objmap_get(flx_obj_as_objmap(%s), %s)", emit_expr(e->object), emit_expr(e->target));
            }
            if (e->target && is_map_type(e->object->checked_type)) {
                return strf("%s(%s, %s)", map_fn(e->object->checked_type, "get"), emit_expr(e->object), emit_expr(e->target));
            }
            if (e->target && type_eq(e->object->checked_type, "OPEN")) {
                return strf("flx_objmap_get(%s, %s)", emit_expr(e->object), emit_expr(e->target));
            }
            if (e->target && is_open_struct_type(e->object->checked_type)) {
                if (type_eq(e->checked_type, "OBJ")) return strf("flx_objmap_get(%s, %s)", emit_expr(e->object), emit_expr(e->target));
                return emit_obj_get_typed(e->checked_type, emit_obj(e->object), emit_expr(e->target));
            }
            if (e->name && type_eq(e->object->checked_type, "OPEN")) {
                return strf("flx_objmap_get(%s, %s)", emit_expr(e->object), emit_key_literal(e->name));
            }
            if (e->name && is_open_struct_type(e->object->checked_type)) {
                Node *field = struct_field(g_types, e->object->checked_type, e->name);
                if (field) return emit_obj_get_typed(field->declared_type, emit_obj(e->object), emit_key_literal(e->name));
            }
            return strf("%s.%s", emit_obj(e->object), e->name);
        case NK_UNARY:
            if (strcmp(e->op, "REF") == 0) return strf("(&%s)", emit_expr(e->expr));
            if (strcmp(e->op, "*") == 0) return strf("(*%s)", emit_expr(e->expr));
            if (strcmp(e->op, "hold") == 0) {
                char *source_type = e->expr->checked_type;
                if (is_task_type(source_type)) return strf("flx_hold_%s(%s)", lower_mangle(task_result_type(source_type)), emit_expr(e->expr));
                if (is_array_type(source_type)) {
                    char *element_type = array_elem_type(source_type);
                    return strf("flx_hold_all_%s(%s)", lower_mangle(task_result_type(element_type)), emit_expr(e->expr));
                }
            }
            return strf("(%s%s)", e->op, emit_expr(e->expr));
        case NK_BINARY:
            if (strcmp(e->op, "else") == 0) return emit_else_expr(e);
            if (strcmp(e->op, "+") == 0 &&
                (is_array_type(e->checked_type) || is_map_type(e->checked_type) || is_struct_type(e->checked_type) || is_open_data_type(e->checked_type))) {
                return emit_join_expr(e->left, e->right, e->checked_type);
            }
            if (type_eq(e->left->checked_type, "STR") && strcmp(e->op, "+") == 0) {
                return strf("flx_str_concat(%s, %s)", emit_expr(e->left), emit_expr(e->right));
            }
            if (type_eq(e->left->checked_type, "STR") && (strcmp(e->op, "==") == 0 || strcmp(e->op, "!=") == 0)) {
                char *eq = strf("flx_str_eq(%s, %s)", emit_expr(e->left), emit_expr(e->right));
                return strcmp(e->op, "==") == 0 ? eq : strf("(!%s)", eq);
            }
            return strf("(%s %s %s)", emit_expr(e->left), e->op, emit_expr(e->right));
        case NK_CALL:
            if (is_struct_constructor_expr(e)) return emit_expr(e->args.items[0]);
            if (e->callee->kind != NK_NAME) {
                char *args = emit_user_call_args(&e->args);
                return strf("%s(%s)", emit_expr(e->callee), args);
            }
            if (e->callee->kind == NK_NAME && e->callee->checked_type && is_fn_type(e->callee->checked_type)) {
                char *args = emit_user_call_args(&e->args);
                return strf("%s(%s)", emit_expr(e->callee), args);
            }
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "len") == 0) {
                Node *arg = e->args.items[0];
                if (is_map_type(arg->checked_type) || type_eq(arg->checked_type, "OPEN") || is_open_struct_type(arg->checked_type)) {
                    return strf("(int64_t)(%s.raw ? hashmap_count(%s.raw) : 0)", emit_expr(arg), emit_expr(arg));
                }
                return strf("%s.len", emit_obj(arg));
            }
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "has") == 0) {
                Node *collection = e->args.items[0];
                Node *needle = e->args.items[1];
                if (is_array_type(collection->checked_type)) return emit_array_has_expr(collection, needle);
                if (type_eq(collection->checked_type, "STR")) return strf("flx_str_has(%s, %s)", emit_expr(collection), emit_expr(needle));
                if (is_map_type(collection->checked_type)) return strf("%s(%s, %s)", map_fn(collection->checked_type, "has"), emit_expr(collection), emit_expr(needle));
                if (type_eq(collection->checked_type, "OBJ")) return strf("flx_objmap_has(flx_obj_as_objmap(%s), %s)", emit_expr(collection), emit_expr(needle));
                return strf("flx_objmap_has(%s, %s)", emit_expr(collection), emit_expr(needle));
            }
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "find") == 0) {
                Node *collection = e->args.items[0];
                Node *needle = e->args.items[1];
                if (is_array_type(collection->checked_type)) return emit_array_find_expr(collection, needle);
                return strf("flx_str_find(%s, %s)", emit_expr(collection), emit_expr(needle));
            }
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "min") == 0) return emit_minmax_expr(e->args.items[0], false);
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "max") == 0) return emit_minmax_expr(e->args.items[0], true);
            if (e->callee->kind == NK_NAME && is_cast_call_name(e->callee->name)) return emit_cast_expr(e);
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "trim") == 0) return strf("flx_str_trim(%s)", emit_expr(e->args.items[0]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "lower") == 0) return strf("flx_str_lower(%s)", emit_expr(e->args.items[0]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "upper") == 0) return strf("flx_str_upper(%s)", emit_expr(e->args.items[0]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "urlDecode") == 0) return strf("flx_str_url_decode(%s)", emit_expr(e->args.items[0]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "replace") == 0) return strf("flx_str_replace(%s, %s, %s)", emit_expr(e->args.items[0]), emit_expr(e->args.items[1]), emit_expr(e->args.items[2]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "starts") == 0) return strf("flx_str_starts(%s, %s)", emit_expr(e->args.items[0]), emit_expr(e->args.items[1]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "ends") == 0) return strf("flx_str_ends(%s, %s)", emit_expr(e->args.items[0]), emit_expr(e->args.items[1]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "isDigit") == 0) return strf("flx_str_all_digit(%s)", emit_expr(e->args.items[0]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "isAlpha") == 0) return strf("flx_str_all_alpha(%s)", emit_expr(e->args.items[0]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "isSpace") == 0) return strf("flx_str_all_space(%s)", emit_expr(e->args.items[0]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "JSON") == 0) return strf("flx_json_parse_dyn(%s)", emit_expr(e->args.items[0]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "RAND") == 0) return strf("flx_rand((double)(%s))", emit_expr(e->args.items[0]));
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "assert") == 0) {
                if (e->args.len == 1) return strf("flx_assert(%s)", emit_expr(e->args.items[0]));
                return strf("flx_assert_msg(%s, %s)", emit_expr(e->args.items[0]), emit_expr(e->args.items[1]));
            }
            if (e->callee->kind == NK_NAME && strcmp(e->callee->name, "log") == 0 && e->args.len == 1) {
                Node *arg = e->args.items[0];
                if (type_eq(arg->checked_type, "INT")) return strf("flx_log_int(%s)", emit_expr(arg));
                if (type_eq(arg->checked_type, "FLT")) return strf("flx_log_flt(%s)", emit_expr(arg));
                if (type_eq(arg->checked_type, "BOL")) return strf("flx_log_bol(%s)", emit_expr(arg));
                if (type_eq(arg->checked_type, "OBJ")) return strf("flx_log_obj(%s)", emit_expr(arg));
                return strf("flx_log_str(%s)", emit_expr(arg));
            }
            {
                char *args = emit_user_call_args(&e->args);
                return strf("%s(%s)", e->callee->c_expr ? e->callee->c_expr : e->callee->name, args);
            }
        case NK_METHOD_CALL:
            if (is_struct_constructor_expr(e)) return emit_expr(e->args.items[0]);
            {
                char *fast_web_json = emit_web_json_direct_response(e);
                if (fast_web_json) return fast_web_json;
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->name, "INIT") == 0) {
                const char *init_name = e->declared_type ? e->declared_type : e->object->name;
                return strf("%s()", emit_named_value_fn_name(init_name));
            }
            if (e->object && e->object->kind == NK_NAME &&
                (strcmp(e->object->name, "OS") == 0 || strcmp(e->object->name, "FS") == 0 ||
                 strcmp(e->object->name, "PATH") == 0 || strcmp(e->object->name, "PROC") == 0)) {
                char *args = xstrdup("");
                for (int i = 0; i < e->args.len; i++) args = strf("%s%s%s", args, i ? ", " : "", emit_expr(e->args.items[i]));
                return strf("%s_%s(%s)", e->object->name, e->name, args);
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->object->name, "BUF") == 0) {
                if (strcmp(e->name, "new") == 0) return xstrdup("BUF_new()");
                if (strcmp(e->name, "toStr") == 0) return strf("BUF_toStr(%s)", emit_expr(e->args.items[0]));
                if (strcmp(e->name, "len") == 0) return strf("BUF_len(%s)", emit_expr(e->args.items[0]));
                if (strcmp(e->name, "clear") == 0) return strf("BUF_clear(%s)", emit_expr(e->args.items[0]));
                if (strcmp(e->name, "free") == 0) return strf("BUF_free(%s)", emit_expr(e->args.items[0]));

                Node *value = e->args.items[1];
                const char *suffix = "str";
                if (type_eq(value->checked_type, "INT")) suffix = "int";
                else if (type_eq(value->checked_type, "FLT")) suffix = "flt";
                else if (type_eq(value->checked_type, "BOL")) suffix = "bol";
                return strf("BUF_%s_%s(%s, %s)", e->name, suffix, emit_expr(e->args.items[0]), emit_expr(value));
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->object->name, "SQL") == 0) {
                if (strcmp(e->name, "open") == 0) return strf("SQL_open(%s)", emit_expr(e->args.items[0]));
                if (strcmp(e->name, "close") == 0) return strf("SQL_close(%s)", emit_expr(e->args.items[0]));
                if (strcmp(e->name, "exec") == 0) {
                    return strf("SQL_exec(%s, %s, %s)", emit_expr(e->args.items[0]), emit_expr(e->args.items[1]), emit_expr(e->args.items[2]));
                }
                if (strcmp(e->name, "query") == 0) {
                    return strf("SQL_query(%s, %s, %s)", emit_expr(e->args.items[0]), emit_expr(e->args.items[1]), emit_expr(e->args.items[2]));
                }
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->object->name, "IO") == 0) {
                if (strcmp(e->name, "readLine") == 0 || strcmp(e->name, "stdin") == 0 ||
                    strcmp(e->name, "stdout") == 0 || strcmp(e->name, "stderr") == 0) {
                    return strf("IO_%s()", e->name);
                }
                if (strcmp(e->name, "print") == 0 || strcmp(e->name, "error") == 0) {
                    Node *arg = e->args.items[0];
                    char *suffix = NULL;
                    if (type_eq(arg->checked_type, "INT")) suffix = "int";
                    else if (type_eq(arg->checked_type, "FLT")) suffix = "flt";
                    else if (type_eq(arg->checked_type, "BOL")) suffix = "bol";
                    else suffix = "str";
                    return strf("IO_%s_%s(%s)", e->name, suffix, emit_expr(arg));
                }
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->object->name, "JSON") == 0 && strcmp(e->name, "stringify") == 0) {
                return strf("flx_json_stringify(%s)", emit_expr(e->args.items[0]));
            }
            if (e->object && e->object->kind == NK_NAME && strcmp(e->object->name, "REQ") == 0) {
                if (strcmp(e->name, "host") == 0) {
                    if (e->args.len == 3) {
                        Node *handler = e->args.items[2];
                        char *host_fn = handler->declared_type && type_eq(handler->declared_type, "REF[HttpReq]") ? "REQ_host_ctx_ref" : "REQ_host_ctx";
                        return strf("%s(%s, %s, %s)", host_fn, emit_expr(e->args.items[0]), emit_expr(e->args.items[1]), emit_host_adapter_name(e));
                    }
                    Node *handler = e->args.items[1];
                    char *host_fn = handler->declared_type && type_eq(handler->declared_type, "REF[HttpReq]") ? "REQ_host_ref" : "REQ_host";
                    return strf("%s(%s, %s)", host_fn, emit_expr(e->args.items[0]), handler->name);
                }
                if (strcmp(e->name, "hostTLS") == 0) {
                    if (e->args.len == 5) {
                        Node *handler = e->args.items[4];
                        char *host_fn = handler->declared_type && type_eq(handler->declared_type, "REF[HttpReq]") ? "REQ_hostTLS_ctx_ref" : "REQ_hostTLS_ctx";
                        return strf("%s(%s, %s, %s, %s, %s)", host_fn, emit_expr(e->args.items[0]), emit_expr(e->args.items[1]), emit_expr(e->args.items[2]), emit_expr(e->args.items[3]), emit_host_adapter_name(e));
                    }
                    Node *handler = e->args.items[3];
                    char *host_fn = handler->declared_type && type_eq(handler->declared_type, "REF[HttpReq]") ? "REQ_hostTLS_ref" : "REQ_hostTLS";
                    return strf("%s(%s, %s, %s, %s)", host_fn, emit_expr(e->args.items[0]), emit_expr(e->args.items[1]), emit_expr(e->args.items[2]), handler->name);
                }
                if (strcmp(e->name, "ws") == 0) {
                    Node *handler = e->args.items[1];
                    return strf("REQ_ws(%s, %s)", emit_expr(e->args.items[0]), handler->name);
                }
                if (strcmp(e->name, "wsTLS") == 0) {
                    Node *handler = e->args.items[3];
                    return strf("REQ_wsTLS(%s, %s, %s, %s)", emit_expr(e->args.items[0]), emit_expr(e->args.items[1]), emit_expr(e->args.items[2]), handler->name);
                }
                if (strcmp(e->name, "get") == 0 || strcmp(e->name, "delete") == 0) {
                    return strf("REQ_%s(%s)", e->name, emit_expr(e->args.items[0]));
                }
                if (strcmp(e->name, "post") == 0 || strcmp(e->name, "put") == 0) {
                    return strf("REQ_%s(%s, %s)", e->name, emit_expr(e->args.items[0]), emit_expr(e->args.items[1]));
                }
            }
            if (e->c_expr) {
                char *args = emit_user_call_args(&e->args);
                return strf("%s(%s)", e->c_expr, args);
            }
            if (e->object && is_shape_type(e->object->checked_type) && strcmp(e->name, "fill") == 0) {
                return emit_shape_fill_expr(e);
            }
            Node *fn_field = e->object ? struct_field(g_types, e->object->checked_type, e->name) : NULL;
            if (fn_field && is_fn_type(fn_field->declared_type)) {
                char *args = emit_user_call_args(&e->args);
                return strf("%s.%s(%s)", emit_obj(e->object), e->name, args);
            }
            if (strcmp(e->name, "ARR") == 0) {
                if (type_eq(e->object->checked_type, "STR")) {
                    if (e->args.len == 1) return emit_split_expr(e->object, e->args.items[0]);
                    return strf("flx_str_chars(%s)", emit_expr(e->object));
                }
                return emit_arr_conversion_expr(e->object, e->checked_type);
            }
            if (strcmp(e->name, "STR") == 0) {
                if (e->args.len == 1) return strf("flx_arr_str_join_sep(%s, %s)", emit_expr(e->object), emit_expr(e->args.items[0]));
                return strf("flx_arr_str_join(%s)", emit_expr(e->object));
            }
            if (is_array_type(e->object->checked_type) && strcmp(e->name, "add") == 0) {
                return emit_join_expr(e->object, e->args.items[0], e->checked_type);
            }
            if ((is_map_type(e->object->checked_type) || is_struct_type(e->object->checked_type) || is_open_data_type(e->object->checked_type)) &&
                strcmp(e->name, "upd") == 0 && e->args.len == 1) {
                return emit_join_expr(e->object, e->args.items[0], e->checked_type);
            }
            if (strcmp(e->name, "len") == 0) return strf("%s.len", emit_obj(e->object));
            if ((is_array_type(e->object->checked_type) || type_eq(e->object->checked_type, "STR") || is_map_type(e->object->checked_type)) &&
                is_collection_method_name(e->name)) return emit_collection_method(e);
            if (is_map_type(e->object->checked_type) && strcmp(e->name, "has") == 0) {
                return strf("%s(%s, %s)",
                    map_fn(e->object->checked_type, "has"),
                    emit_expr(e->object),
                    emit_expr(e->args.items[0]));
            }
            if (type_eq(e->object->checked_type, "OBJ") && strcmp(e->name, "has") == 0) {
                return strf("flx_objmap_has(flx_obj_as_objmap(%s), %s)", emit_expr(e->object), emit_expr(e->args.items[0]));
            }
            if ((is_open_struct_type(e->object->checked_type) || type_eq(e->object->checked_type, "OPEN")) && strcmp(e->name, "has") == 0) {
                return strf("flx_objmap_has(%s, %s)", emit_expr(e->object), emit_expr(e->args.items[0]));
            }
            return xstrdup("/*badmethod*/");
        case NK_DECODE:
            if (type_eq(e->expr->checked_type, "OBJ")) {
                return strf("%s(flx_obj_as_objmap(%s))", json_decode_fn(e->declared_type), emit_expr(e->expr));
            }
            return strf("%s(%s)", json_decode_fn(e->declared_type), emit_expr(e->expr));
        case NK_IF_EXPR:
            if (e->right) return strf("(%s ? %s : %s)", emit_expr(e->condition), emit_expr(e->value), emit_expr(e->right));
            return emit_expr(e->value);
        default:
            return xstrdup("/*badexpr*/");
    }
}
