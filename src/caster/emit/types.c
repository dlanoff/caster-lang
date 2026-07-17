static int depth_type(const char *type) {
    int depth = 0;
    while (is_array_type(type)) {
        depth++;
        type = array_elem_type(type);
    }
    return depth;
}

static int cmp_arr(const void *a, const void *b) {
    char *left = *(char **)a;
    char *right = *(char **)b;
    return depth_type(left) - depth_type(right);
}

static void emit_struct_forwards(FILE *f) {
    for (int i = 0; g_types && i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (info->kind != TYPEINFO_STRUCT) continue;
        if (info->open_runtime) fprintf(f, "typedef FLX_OBJMAP FLX_%s;\n", info->name);
        else fprintf(f, "typedef struct FLX_%s FLX_%s;\n", info->name, info->name);
    }
    if (g_types && g_types->len) fprintf(f, "\n");
}

static void emit_struct_defs(FILE *f) {
    bool any = false;
    for (int i = 0; g_types && i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (info->kind != TYPEINFO_STRUCT) continue;
        if (info->open_runtime) continue;
        any = true;
        fprintf(f, "struct FLX_%s {\n", info->name);
        for (int j = 0; j < info->node->fields.len; j++) {
            Node *field = info->node->fields.items[j];
            fprintf(f, "    %s %s;\n", ctype(field->declared_type), field->name);
            if (is_ref_type(field->declared_type)) {
                fprintf(f, "    bool %s;\n", ref_owner_field_name(field->name));
            }
        }
        fprintf(f, "};\n\n");
    }
    if (any) fprintf(f, "\n");
}

static char *emit_promote_ref_target_expr(const char *target_type, const char *value) {
    if (is_struct_type(target_type)) {
        return strf("%s(%s)", struct_promote_ref_fn_for_type(target_type), value);
    }

    char *target_ctype = ctype(target_type);
    char *copy_value = emit_heap_field_copy(target_type, strf("(*%s)", value));
    char *ptr = strf("promoted_ref_field_%s", lower_mangle(target_type));
    return strf(
        "({ %s* %s = NULL; "
        "if (%s) { "
        "%s = malloc(sizeof(%s)); "
        "if (!%s) { fprintf(stderr, \"caster: REF field promotion allocation failed\\n\"); exit(1); } "
        "*%s = %s; "
        "} "
        "%s; })",
        target_ctype,
        ptr,
        value,
        ptr,
        target_ctype,
        ptr,
        ptr,
        copy_value,
        ptr);
}

static void emit_struct_init_entry(FILE *f, bool *first, const char *name, const char *value) {
    fprintf(f, "%s        .%s = %s", *first ? "" : ",\n", name, value);
    *first = false;
}

static bool emit_struct_copy_helper_decls(FILE *f) {
    bool any = false;
    for (int i = 0; g_types && i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (info->kind != TYPEINFO_STRUCT || info->open_runtime) continue;
        if (!any) fprintf(f, "/* fixed MAP copy helpers */\n");
        any = true;
        fprintf(f, "static FLX_UNUSED FLX_%s %s(FLX_%s value);\n",
            info->name,
            struct_copy_fn_for_type(info->name),
            info->name);
        fprintf(f, "static FLX_UNUSED FLX_%s %s(FLX_%s value);\n",
            info->name,
            struct_promote_fn_for_type(info->name),
            info->name);
        fprintf(f, "static FLX_UNUSED FLX_%s* %s(FLX_%s* value);\n",
            info->name,
            struct_promote_ref_fn_for_type(info->name),
            info->name);
    }
    if (any) fprintf(f, "\n");
    return any;
}

static void emit_struct_copy_helpers(FILE *f) {
    for (int i = 0; g_types && i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (info->kind != TYPEINFO_STRUCT || info->open_runtime) continue;

        fprintf(f, "static FLX_UNUSED FLX_%s* %s(FLX_%s* value) {\n",
            info->name,
            struct_promote_ref_fn_for_type(info->name),
            info->name);
        fprintf(f, "    if (!value) return NULL;\n");
        fprintf(f, "    FLX_%s* out = malloc(sizeof(FLX_%s));\n", info->name, info->name);
        fprintf(f, "    if (!out) { fprintf(stderr, \"caster: REF copy allocation failed\\n\"); exit(1); }\n");
        fprintf(f, "    *out = %s(*value);\n", struct_promote_fn_for_type(info->name));
        fprintf(f, "    return out;\n");
        fprintf(f, "}\n\n");

        fprintf(f, "static FLX_UNUSED FLX_%s %s(FLX_%s value) {\n",
            info->name,
            struct_copy_fn_for_type(info->name),
            info->name);
        if (!info->node->fields.len) {
            fprintf(f, "    return (FLX_%s){0};\n", info->name);
            fprintf(f, "}\n\n");
            continue;
        }

        fprintf(f, "    return (FLX_%s){\n", info->name);
        bool first_init = true;
        for (int j = 0; j < info->node->fields.len; j++) {
            Node *field = info->node->fields.items[j];
            char *field_value = strf("value.%s", field->name);
            char *copy_value = NULL;
            if (is_ref_type(field->declared_type)) {
                char *target_type = ref_target_type(field->declared_type);
                copy_value = strf("value.%s && value.%s ? %s : value.%s",
                    ref_owner_field_name(field->name),
                    field->name,
                    emit_promote_ref_target_expr(target_type, field_value),
                    field->name);
            } else {
                copy_value = emit_heap_field_copy(field->declared_type, field_value);
            }
            emit_struct_init_entry(f, &first_init, field->name, copy_value);
            if (is_ref_type(field->declared_type)) {
                emit_struct_init_entry(f,
                    &first_init,
                    ref_owner_field_name(field->name),
                    strf("value.%s && value.%s", field->name, ref_owner_field_name(field->name)));
            }
        }
        fprintf(f, "\n    };\n");
        fprintf(f, "}\n\n");

        fprintf(f, "static FLX_UNUSED FLX_%s %s(FLX_%s value) {\n",
            info->name,
            struct_promote_fn_for_type(info->name),
            info->name);
        if (!info->node->fields.len) {
            fprintf(f, "    return (FLX_%s){0};\n", info->name);
            fprintf(f, "}\n\n");
            continue;
        }

        fprintf(f, "    return (FLX_%s){\n", info->name);
        first_init = true;
        for (int j = 0; j < info->node->fields.len; j++) {
            Node *field = info->node->fields.items[j];
            char *field_value = strf("value.%s", field->name);
            char *copy_value = NULL;
            if (is_ref_type(field->declared_type)) {
                char *target_type = ref_target_type(field->declared_type);
                copy_value = emit_promote_ref_target_expr(target_type, field_value);
            } else {
                copy_value = emit_heap_field_copy(field->declared_type, field_value);
            }
            emit_struct_init_entry(f, &first_init, field->name, copy_value);
            if (is_ref_type(field->declared_type)) {
                emit_struct_init_entry(f,
                    &first_init,
                    ref_owner_field_name(field->name),
                    strf("value.%s != NULL", field->name));
            }
        }
        fprintf(f, "\n    };\n");
        fprintf(f, "}\n\n");
    }
}

static bool emit_struct_drop_helper_decls(FILE *f) {
    bool any = false;
    for (int i = 0; g_types && i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (info->kind != TYPEINFO_STRUCT || info->open_runtime) continue;
        if (!type_has_drop_helper(info->name)) continue;
        if (!any) fprintf(f, "/* fixed MAP drop helpers */\n");
        any = true;
        fprintf(f, "static FLX_UNUSED void flx_drop_%s(FLX_%s value);\n", lower_mangle(info->name), info->name);
    }
    if (any) fprintf(f, "\n");
    return any;
}

static void emit_struct_drop_helpers(FILE *f) {
    emit_struct_drop_helper_decls(f);

    for (int i = 0; g_types && i < g_types->len; i++) {
        TypeInfo *info = &g_types->items[i];
        if (info->kind != TYPEINFO_STRUCT || info->open_runtime) continue;
        if (!type_has_drop_helper(info->name)) continue;

        fprintf(f, "static FLX_UNUSED void flx_drop_%s(FLX_%s value) {\n", lower_mangle(info->name), info->name);
        bool emitted = false;
        for (int j = 0; j < info->node->fields.len; j++) {
            Node *field = info->node->fields.items[j];
            if (!field_type_needs_drop(field->declared_type, 0)) continue;
            if (is_ref_type(field->declared_type)) {
                char *target_type = ref_target_type(field->declared_type);
                char *target_drop = type_has_drop_helper(target_type) ? drop_fn_for_type(target_type) : NULL;
                emitted = true;
                fprintf(f, "    if (value.%s && value.%s) {\n", ref_owner_field_name(field->name), field->name);
                if (target_drop) fprintf(f, "        %s(*value.%s);\n", target_drop, field->name);
                fprintf(f, "        free(value.%s);\n", field->name);
                fprintf(f, "    }\n");
                continue;
            }
            char *drop = drop_fn_for_type(field->declared_type);
            if (!drop) continue;
            emitted = true;
            fprintf(f, "    %s(value.%s);\n", drop, field->name);
        }
        if (!emitted) fprintf(f, "    (void)value;\n");
        fprintf(f, "}\n\n");
    }
}
