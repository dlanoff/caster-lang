static char *sanitize_upper(const char *text) {
    char *out = xstrdup(text);
    for (char *p = out; *p; p++) {
        if (isalnum((unsigned char)*p)) *p = (char)toupper((unsigned char)*p);
        else *p = '_';
    }
    return out;
}

static char *mangle(const char *type) {
    if (type_eq(type, "INT") || type_eq(type, "FLT") || type_eq(type, "BOL") || type_eq(type, "STR")) return xstrdup(type);
    if (is_array_type(type)) {
        char *element_type = array_elem_type(type);
        char *element_mangle = mangle(element_type);
        return strf("ARR_%s", element_mangle);
    }
    if (is_task_type(type)) {
        char *target_type = task_result_type(type);
        char *target_mangle = mangle(target_type);
        return strf("TSK_%s", target_mangle);
    }
    if (is_fn_type(type)) {
        char *input_type = fn_input_type(type);
        char *output_type = fn_output_type(type);
        return strf("FN_%s_TO_%s", mangle(input_type), mangle(output_type));
    }
    return sanitize_upper(type);
}

static char *ctype(const char *type) {
    if (type_eq(type, "INT")) return xstrdup("int64_t");
    if (type_eq(type, "FLT")) return xstrdup("double");
    if (type_eq(type, "BOL")) return xstrdup("bool");
    if (type_eq(type, "STR")) return xstrdup("FLX_STR");
    if (type_eq(type, "OBJ")) return xstrdup("FLX_OBJ");
    if (type_eq(type, "OPEN")) return xstrdup("FLX_OBJMAP");
    if (type_eq(type, "NUL")) return xstrdup("void");
    if (is_ref_type(type)) return strf("%s*", ctype(ref_target_type(type)));
    if (is_task_type(type)) return strf("FLX_%s", mangle(type));
    if (is_fn_type(type)) return strf("FLX_%s", mangle(type));
    if (is_array_type(type)) return strf("FLX_%s", mangle(type));
    if (is_map_type(type)) return strf("FLX_%s", mangle(type));
    if (is_struct_type(type)) return strf("FLX_%s", type);
    return xstrdup("void");
}

static char *arr_make(const char *type) {
    char *name = mangle(type);
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strf("flx_%s_make", name);
}

static char *arr_add(const char *type) {
    char *name = mangle(type);
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strf("flx_%s_add", name);
}

static char *arr_concat(const char *type) {
    char *name = mangle(type);
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strf("flx_%s_concat", name);
}

static char *arr_sub_value(const char *type) {
    char *name = mangle(type);
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strf("flx_%s_sub_value", name);
}

static char *arr_drop(const char *type) {
    char *name = mangle(type);
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    return strf("flx_%s_drop", name);
}

static char *lower_mangle(const char *type) {
    char *name = mangle(type);
    for (char *p = name; *p; p++) *p = (char)tolower((unsigned char)*p);
    return name;
}

static char *struct_copy_fn_for_type(const char *type) {
    return strf("flx_copy_%s", lower_mangle(type));
}

static char *struct_promote_fn_for_type(const char *type) {
    return strf("flx_promote_%s", lower_mangle(type));
}

static char *struct_promote_ref_fn_for_type(const char *type) {
    return strf("flx_promote_ref_%s", lower_mangle(type));
}

static char *ref_owner_field_name(const char *name) {
    return strf("flx_owns_%s", name);
}

static char *map_entry_ctype(const char *type) {
    return strf("%s_ENTRY", ctype(type));
}

static char *map_fn(const char *type, const char *suffix) {
    return strf("flx_%s_%s", lower_mangle(type), suffix);
}

static char *json_decode_fn(const char *type) {
    return strf("flx_json_decode_%s", lower_mangle(type));
}

static char *c_escape(const char *s);

static bool is_obj_scalar_type(const char *type) {
    return type_eq(type, "INT") || type_eq(type, "FLT") || type_eq(type, "BOL") || type_eq(type, "STR");
}

static char *obj_from_fn(const char *type) {
    if (type_eq(type, "INT")) return xstrdup("flx_obj_from_int");
    if (type_eq(type, "FLT")) return xstrdup("flx_obj_from_flt");
    if (type_eq(type, "BOL")) return xstrdup("flx_obj_from_bol");
    if (type_eq(type, "STR")) return xstrdup("flx_obj_from_str");
    if (type_eq(type, "NUL")) return xstrdup("flx_obj_from_nil");
    if (type_eq(type, "OPEN")) return xstrdup("flx_obj_from_objmap");
    return strf("flx_obj_from_%s", lower_mangle(type));
}

static char *obj_as_fn(const char *type) {
    if (type_eq(type, "INT")) return xstrdup("flx_obj_as_int");
    if (type_eq(type, "FLT")) return xstrdup("flx_obj_as_flt");
    if (type_eq(type, "BOL")) return xstrdup("flx_obj_as_bol");
    if (type_eq(type, "STR")) return xstrdup("flx_obj_as_str");
    if (type_eq(type, "OPEN")) return xstrdup("flx_obj_as_objmap");
    return strf("flx_obj_as_%s", lower_mangle(type));
}

static char *emit_key_literal(const char *key) {
    return strf("flx_str_lit(%s, %zu)", c_escape(key), strlen(key));
}

static char *emit_obj_value(const char *type, const char *expr) {
    if (type_eq(type, "OBJ")) return xstrdup(expr);
    if (type_eq(type, "NUL")) return xstrdup("flx_obj_from_nil()");
    return strf("%s(%s)", obj_from_fn(type), expr);
}

static char *emit_obj_get_typed(const char *type, const char *object, const char *key_expr) {
    return strf("%s(flx_objmap_get(%s, %s))", obj_as_fn(type), object, key_expr);
}

static char *emit_obj_set_typed(const char *object, const char *key_expr, const char *type, const char *value_expr) {
    return strf("flx_objmap_add(%s, %s, %s)", object, key_expr, emit_obj_value(type, value_expr));
}

static char *c_escape(const char *s) {
    char *out = xstrdup("\"");
    for (const char *p = s; *p; p++) {
        char *piece = NULL;
        if (*p == '"') piece = "\\\"";
        else if (*p == '\\') piece = "\\\\";
        else if (*p == '\n') piece = "\\n";
        else if (*p == '\t') piece = "\\t";
        else {
            char tmp[2] = {*p, 0};
            piece = tmp;
        }
        out = strf("%s%s", out, piece);
    }
    return strf("%s\"", out);
}

static char *emit_obj(Node *e) {
    if (e->kind == NK_NAME || e->kind == NK_DOT) return emit_expr(e);
    if (e->kind == NK_UNARY && e->op && strcmp(e->op, "*") == 0) return emit_expr(e);
    return strf("(%s)", emit_expr(e));
}

static char *emit_named_value_fn_name(const char *name) {
    return strf("flx_value_%s", name);
}

static char *emit_named_value_storage_name(const char *name) {
    return strf("flx_named_%s", name);
}
