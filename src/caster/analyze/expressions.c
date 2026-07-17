// Analyzer expression typing and operator semantics.

// Preserves raw REF identity only when equality is directly comparing references, because
// nested value expressions such as len(refValue) still need normal read semantics.
static bool equality_operand_preserves_ref_identity(Node *e, Scope *s) {
    if (!e) return false;
    if (e->kind == NK_UNARY && e->op && strcmp(e->op, "REF") == 0) return true;
    if (e->kind == NK_NAME) {
        Symbol *sym = scope_try_resolve(s, e->name);
        return sym && is_ref_type(sym->type);
    }
    return e->kind == NK_DOT;
}

// Analyzes an expression with optional expected type, which lets literals and open structures
// conform at boundaries because locals stay inferred until context requires precision.
static char *analyze_expr_expected(Node *e, Scope *s, AnalyzeCtx *ctx, const char *expected) {
    switch (e->kind) {
        case NK_INT:
            if (expected && type_eq(expected, "FLT")) return set_type(e, "FLT");
            return set_type(e, "INT");
        case NK_FLT:
            return set_type(e, "FLT");
        case NK_BOL:
            return set_type(e, "BOL");
        case NK_STR:
            return set_type(e, "STR");
        case NK_NIL:
            return set_type(e, "NUL");
        case NK_INIT:
            die_at(e->line, e->col, "INIT is only allowed inside MAP definitions");
        case NK_NAME: {
            if (expected && is_fn_type(expected) && !scope_try_resolve(s, e->name)) {
                FnInfo *fn = ctx->fns ? fn_find(ctx->fns, e->name) : NULL;
                if (fn) {
                    char *input = fn_input_type(expected);
                    char *output = fn_output_type(expected);
                    if (fn->param_count != 1 || !type_eq(fn->param_types[0], input) || !type_eq(fn->return_type, output)) {
                        die_at(e->line, e->col, "function '%s' does not match expected %s", e->name, expected);
                    }
                    e->c_expr = xstrdup(e->name);
                    return set_type(e, expected);
                }
            }

            Symbol *sym = scope_try_resolve(s, e->name);
            char *actual = analyze_name(e, s);
            if (expected && sym && is_unknown_array_type(sym->type) && is_array_type(expected)) {
                refine_symbol_type(sym, expected);
                return set_type(e, expected);
            }
            if (expected && is_json_decode_target_type(expected) && is_json_dynamic_type(actual)) {
                conform_inferred_open_local(e, s, ctx, expected);
                return e->checked_type;
            }
            if (expected) {
                actual = implicit_ref_read_if_assignable(e, actual, expected);
            } else if (!ctx || !ctx->preserve_ref_identity) {
                actual = implicit_ref_read_value(e, actual);
            }
            if (expected && !type_assignable_to_expected(actual, expected)) {
                die_at(e->line, e->col, "expected %s, got %s", expected, actual);
            }
            return e->checked_type;
        }

        case NK_SHAPE:
            if (e->elements.len < 2) die_at(e->line, e->col, "shape literal needs at least two dimensions");
            for (int i = 0; i < e->elements.len; i++) {
                Node *dim = e->elements.items[i];
                char *dim_type = analyze_expr(dim, s, ctx);
                if (!type_eq(dim_type, "INT")) {
                    die_at(dim->line, dim->col, "shape dimensions must be INT, got %s", dim_type);
                }
            }
            if (expected && !is_shape_type(expected)) die_at(e->line, e->col, "shape literal must be used with .fill(value)");
            return set_type(e, "SHAPE");

        case NK_ARRAY: {
            if (expected && type_eq(expected, "OBJ")) {
                for (int i = 0; i < e->elements.len; i++) {
                    analyze_expr(e->elements.items[i], s, ctx);
                }
                return set_type(e, "OBJ");
            }

            if (e->elements.len == 0) {
                if (expected && is_array_type(expected)) return set_type(e, expected);
                return set_type(e, "ARR[?]");
            }

            char *expected_element = expected && is_array_type(expected) ? array_elem_type(expected) : NULL;
            char *element_type = analyze_expr_expected(e->elements.items[0], s, ctx, expected_element);
            for (int i = 1; i < e->elements.len; i++) {
                Node *item = e->elements.items[i];
                char *item_type = analyze_expr_expected(item, s, ctx, expected_element ? expected_element : element_type);
                if (!type_eq(item_type, element_type)) {
                    die_at(item->line, item->col, "array literal elements must all have type %s, got %s", element_type, item_type);
                }
            }
            char *actual = array_type(element_type);
            if (expected && is_array_type(expected) && !type_eq(actual, expected)) {
                die_at(e->line, e->col, "array literal must be %s, got %s", expected, actual);
            }
            return set_type(e, actual);
        }

        case NK_MAP_LITERAL: {
            char *key_type = expected && is_map_type(expected) ? map_key_type(expected) : NULL;
            char *value_type = expected && is_map_type(expected) ? map_value_type(expected) : NULL;

            if (expected && type_eq(expected, "OBJ")) {
                for (int i = 0; i < e->fields.len; i++) {
                    Node *entry = e->fields.items[i];
                    char *entry_key_type = analyze_expr(entry->target, s, ctx);
                    if (!type_eq(entry_key_type, "STR")) {
                        die_at(entry->target->line, entry->target->col, "dynamic object literal keys currently must be STR, got %s", entry_key_type);
                    }
                    analyze_expr(entry->value, s, ctx);
                    set_type(entry, "NUL");
                }
                return set_type(e, "OBJ");
            }

            if (expected && is_struct_type(expected)) {
                if (e->fields.len == 0) {
                    TypeInfo *info = type_find(ctx->types, expected);
                    for (int i = 0; info && i < info->node->fields.len; i++) {
                        Node *field = info->node->fields.items[i];
                        if (!field_has_default(ctx->types, field)) {
                            die_at(e->line, e->col, "missing required field '%s' for %s", field->name, expected);
                        }
                    }
                    return set_type(e, expected);
                }
                mark_struct_open(ctx->types, expected);
                return analyze_open_map_literal(e, s, ctx, expected);
            }

            if (expected && !is_map_type(expected)) {
                die_at(e->line, e->col, "dynamic map literal cannot initialize %s", expected);
            }

            if (key_type && !type_eq(key_type, "STR")) {
                die_at(e->line, e->col, "dynamic map literals currently require STR keys, got %s", key_type);
            }

            char *inferred_value = NULL;
            for (int i = 0; i < e->fields.len; i++) {
                Node *entry = e->fields.items[i];
                set_type(entry, "NUL");

                char *entry_key_type = key_type
                    ? analyze_expr_expected(entry->target, s, ctx, key_type)
                    : analyze_expr(entry->target, s, ctx);
                if (!type_eq(entry_key_type, "STR")) {
                    die_at(entry->target->line, entry->target->col, "dynamic map literal keys currently must be STR, got %s", entry_key_type);
                }

                char *entry_value_type = analyze_expr_expected(entry->value, s, ctx, value_type);
                if (value_type) {
                    if (!type_eq(entry_value_type, value_type)) {
                        die_at(entry->value->line, entry->value->col, "map value must be %s, got %s", value_type, entry_value_type);
                    }
                } else if (!inferred_value) {
                    inferred_value = xstrdup(entry_value_type);
                } else if (!type_eq(inferred_value, entry_value_type)) {
                    die_at(entry->value->line, entry->value->col, "map literal values must all have type %s, got %s", inferred_value, entry_value_type);
                }
            }

            if (!value_type) {
                if (!inferred_value) die_at(e->line, e->col, "empty map literal needs an expected MAP type");
                value_type = inferred_value;
            }
            return set_type(e, expected ? expected : map_type("STR", value_type));
        }

        case NK_RECORD_LITERAL: {
            if (expected && type_eq(expected, "OBJ")) {
                for (int i = 0; i < e->fields.len; i++) {
                    Node *entry = e->fields.items[i];
                    analyze_expr(entry->value, s, ctx);
                    set_type(entry, "NUL");
                }
                return set_type(e, "OBJ");
            }

            if (!expected) {
                for (int i = 0; i < e->fields.len; i++) {
                    Node *entry = e->fields.items[i];
                    analyze_expr(entry->value, s, ctx);
                    set_type(entry, "NUL");
                }
                return set_type(e, "OPEN");
            }

            if (!is_struct_type(expected)) {
                die_at(e->line, e->col, "fixed-map literal needs an expected fixed MAP type");
            }

            TypeInfo *info = type_find(ctx->types, expected);
            for (int i = 0; i < e->fields.len; i++) {
                Node *entry = e->fields.items[i];
                Node *field = struct_field(ctx->types, expected, entry->name);
                if (!field) die_at(entry->line, entry->col, "type %s has no field '%s'", expected, entry->name);
                char *entry_type = analyze_expr_expected(entry->value, s, ctx, field->declared_type);
                if (!type_assignable_to_expected(entry_type, field->declared_type)) {
                    die_at(entry->value->line, entry->value->col, "field '%s' must be %s, got %s", entry->name, field->declared_type, entry_type);
                }
                set_type(entry, "NUL");
            }

            for (int i = 0; i < info->node->fields.len; i++) {
                Node *field = info->node->fields.items[i];
                bool present = false;
                for (int j = 0; j < e->fields.len; j++) {
                    Node *entry = e->fields.items[j];
                    if (strcmp(entry->name, field->name) == 0) present = true;
                }
                if (!present && !field->value) {
                    if (!field_has_default(ctx->types, field)) {
                        die_at(e->line, e->col, "missing required field '%s' for %s", field->name, expected);
                    }
                }
            }

            return set_type(e, expected);
        }

        case NK_DOT: {
            char *object_type = analyze_expr(e->object, s, ctx);
            char *access_type = object_type;

            if (is_ref_type(access_type)) {
                access_type = wrap_expr_as_implicit_deref(e->object, access_type);
            }

            if (e->object->kind == NK_NAME && e->name) {
                Symbol *sym = scope_try_resolve(s, e->object->name);
                if (sym && (sym->ctx_index || sym->ctx_element) &&
                    (strcmp(e->name, "i") == 0 || strcmp(e->name, "e") == 0)) {
                    if (sym->ctx_min_depth > 0 && ctx && ctx->loop_depth < sym->ctx_min_depth) {
                        die_at(e->line, e->col, "qualified loop source '%s.%s' is only needed inside nested loops; use %s in the current loop",
                            e->object->name,
                            e->name,
                            e->name);
                    }

                    if (strcmp(e->name, "i") == 0) {
                        if (!sym->ctx_index) die_at(e->line, e->col, "loop context '%s' has no .i", e->object->name);
                        e->c_expr = xstrdup(sym->ctx_index);
                        return set_type(e, "INT");
                    }

                    if (!sym->ctx_element) die_at(e->line, e->col, "loop context '%s' has no .e", e->object->name);
                    e->c_expr = xstrdup(sym->ctx_element);
                    return set_type(e, sym->ctx_element_type ? sym->ctx_element_type : loop_ctx_element_type(access_type));
                }
            }

            if (is_loop_ctx_type(access_type)) {
                if (e->object->kind != NK_NAME) die_at(e->line, e->col, "loop context access requires a named context");
                if (!e->name) die_at(e->line, e->col, "loop context supports .i and .e");

                Symbol *sym = scope_resolve(s, e->object->name, e->object);
                if (strcmp(e->name, "i") == 0) {
                    if (!sym->ctx_index) die_at(e->line, e->col, "loop context '%s' has no .i", e->object->name);
                    e->c_expr = xstrdup(sym->ctx_index);
                    return set_type(e, "INT");
                }

                if (strcmp(e->name, "e") == 0) {
                    if (!sym->ctx_element) die_at(e->line, e->col, "loop context '%s' has no .e", e->object->name);
                    e->c_expr = xstrdup(sym->ctx_element);
                    return set_type(e, loop_ctx_element_type(access_type));
                }

                die_at(e->line, e->col, "loop context '%s' has no field '%s'", e->object->name, e->name);
            }

            if (e->index) {
                AnalyzeCtx index_ctx = *ctx;
                index_ctx.preserve_ref_identity = false;
                char *index_type = analyze_expr(e->index, s, &index_ctx);
                ctx->next_loop_id = index_ctx.next_loop_id;

                if (type_eq(access_type, "OBJ")) {
                    if (type_eq(index_type, "INT")) return set_type(e, "OBJ");
                    if (type_eq(index_type, "STR")) {
                        e->target = e->index;
                        e->index = NULL;
                        return set_type(e, "OBJ");
                    }
                    die_at(e->index->line, e->index->col, "DYN computed access needs INT index or STR key, got %s", index_type);
                }

                if (is_open_struct_type(access_type) || is_open_data_type(access_type)) {
                    if (!type_eq(index_type, "STR")) die_at(e->index->line, e->index->col, "computed open MAP access needs STR key, got %s", index_type);
                    char *source_type = (e->index->kind == NK_STR)
                        ? expr_source_literal_key_type(e->object, s, ctx->types, e->index->text)
                        : NULL;
                    e->target = e->index;
                    e->index = NULL;
                    return set_type(e, source_type ? source_type : "OBJ");
                }

                if (is_map_type(access_type)) {
                    char *key_type = map_key_type(access_type);
                    char *value_type = map_value_type(access_type);
                    if (!type_eq(index_type, key_type)) die_at(e->index->line, e->index->col, "computed map access needs %s key, got %s", key_type, index_type);
                    e->target = e->index;
                    e->index = NULL;
                    return set_type(e, value_type);
                }

                if (!is_array_type(access_type) && !type_eq(access_type, "STR")) {
                    die_at(e->line, e->col, "computed dot access requires ARR, STR, DYN, or runtime-key MAP, got %s", access_type);
                }
                if (!type_eq(index_type, "INT")) die_at(e->index->line, e->index->col, "computed sequence access needs INT index, got %s", index_type);
                if (type_eq(access_type, "STR")) return set_type(e, "STR");
                return set_type(e, array_elem_type(access_type));
            }

            if (e->name) {
                Node *field = struct_field(ctx->types, access_type, e->name);
                if (field) return set_type(e, field->declared_type);

                Symbol *sym = scope_try_resolve(s, e->name);
                if (sym && is_array_type(access_type)) {
                    if (!type_eq(sym->type, "INT")) die_at(e->line, e->col, "computed array access needs INT local '%s', got %s", e->name, sym->type);
                    Node *idx = node_new(NK_NAME, (Token){TK_NAME, e->name, e->line, e->col});
                    idx->name = xstrdup(e->name);
                    if (sym->c_expr) idx->c_expr = xstrdup(sym->c_expr);
                    set_type(idx, "INT");
                    e->index = idx;
                    e->name = NULL;
                    return set_type(e, array_elem_type(access_type));
                }

                if (sym && type_eq(access_type, "STR")) {
                    if (!type_eq(sym->type, "INT")) die_at(e->line, e->col, "computed STR access needs INT local '%s', got %s", e->name, sym->type);
                    Node *idx = node_new(NK_NAME, (Token){TK_NAME, e->name, e->line, e->col});
                    idx->name = xstrdup(e->name);
                    if (sym->c_expr) idx->c_expr = xstrdup(sym->c_expr);
                    set_type(idx, "INT");
                    e->index = idx;
                    e->name = NULL;
                    return set_type(e, "STR");
                }

                if (is_open_struct_type(access_type) || is_open_data_type(access_type)) {
                    if (sym) {
                        if (!is_open_data_type(access_type) && expr_has_source_literal_key(e->object, s, ctx->types, e->name)) {
                            die_at(e->line, e->col, "ambiguous dot access '.%s': source-visible map key conflicts with local '%s'", e->name, e->name);
                        }
                        if (!type_eq(sym->type, "STR")) die_at(e->line, e->col, "computed open MAP access needs STR local '%s', got %s", e->name, sym->type);
                        Node *key = node_new(NK_NAME, (Token){TK_NAME, e->name, e->line, e->col});
                        key->name = xstrdup(e->name);
                        if (sym->c_expr) key->c_expr = xstrdup(sym->c_expr);
                        set_type(key, "STR");
                        e->target = key;
                        return set_type(e, "OBJ");
                    }

                    Node *key = node_new(NK_STR, (Token){TK_STRING_LITERAL, e->name, e->line, e->col});
                    key->text = xstrdup(e->name);
                    set_type(key, "STR");
                    e->target = key;

                    char *source_type = expr_source_literal_key_type(e->object, s, ctx->types, e->name);
                    return set_type(e, source_type ? source_type : "OBJ");
                }

                if (type_eq(access_type, "OBJ")) {
                    if (sym) {
                        if (!type_eq(sym->type, "STR")) die_at(e->line, e->col, "computed object access needs STR local '%s', got %s", e->name, sym->type);
                        Node *key = node_new(NK_NAME, (Token){TK_NAME, e->name, e->line, e->col});
                        key->name = xstrdup(e->name);
                        if (sym->c_expr) key->c_expr = xstrdup(sym->c_expr);
                        set_type(key, "STR");
                        e->target = key;
                        return set_type(e, "OBJ");
                    }

                    Node *key = node_new(NK_STR, (Token){TK_STRING_LITERAL, e->name, e->line, e->col});
                    key->text = xstrdup(e->name);
                    set_type(key, "STR");
                    e->target = key;
                    return set_type(e, "OBJ");
                }

                if (is_map_type(access_type)) {
                    char *key_type = map_key_type(access_type);
                    char *value_type = map_value_type(access_type);

                    if (sym) {
                        if (expr_has_source_literal_key(e->object, s, ctx->types, e->name)) {
                            die_at(e->line, e->col, "ambiguous dot access '.%s': source-visible map key conflicts with local '%s'", e->name, e->name);
                        }
                        if (!type_eq(sym->type, key_type)) die_at(e->line, e->col, "computed map access needs %s local '%s', got %s", key_type, e->name, sym->type);
                        Node *key = node_new(NK_NAME, (Token){TK_NAME, e->name, e->line, e->col});
                        key->name = xstrdup(e->name);
                        if (sym->c_expr) key->c_expr = xstrdup(sym->c_expr);
                        set_type(key, sym->type);
                        e->target = key;
                        return set_type(e, value_type);
                    }

                    if (!type_eq(key_type, "STR")) die_at(e->line, e->col, "literal dot map access requires STR keys, got %s", key_type);
                    Node *key = node_new(NK_STR, (Token){TK_STRING_LITERAL, e->name, e->line, e->col});
                    key->text = xstrdup(e->name);
                    set_type(key, "STR");
                    e->target = key;
                    return set_type(e, value_type);
                }

                die_at(e->line, e->col, "type %s has no field '%s'", access_type, e->name);
            }

            die_at(e->line, e->col, "invalid dot access");
        }

        case NK_UNARY: {
            if (is_mutating_collection_method_expr(e)) {
                Node *call = e->expr;
                if (strcmp(call->name, "agg") == 0) {
                    die_at(call->line, call->col, "mutation operator does not apply to .agg(); use .agg() as a read-only expression");
                }
                die_at(e->line, e->col, "*.%s() is statement-only; use copy-producing .%s() when you need a value", call->name, call->name);
            }

            if (strcmp(e->op, "REF") == 0) {
                if (e->expr->kind == NK_NAME && !scope_try_resolve(s, e->expr->name)) {
                    TypeInfo *info = type_find(ctx->types, e->expr->name);
                    if (info) {
                        if (!type_has_complete_named_value(ctx->types, info)) die_at(e->expr->line, e->expr->col, "MAP '%s' has no complete named value", e->expr->name);
                        char *target_type = info->kind == TYPEINFO_STRUCT ? info->name : info->target;
                        e->expr->c_expr = named_value_storage_name(info->name);
                        set_type(e->expr, target_type);
                        return set_type(e, ref_type(target_type));
                    }
                }
                char *target_type = analyze_lvalue(e->expr, s, ctx);
                return set_type(e, ref_type(target_type));
            }
            if (strcmp(e->op, "*") == 0) {
                if (!e->implicit_deref && !e->mutation_target) {
                    die_at(e->line, e->col, "mutation operator '*' is statement-only; read REF values without '*'");
                }
                char *type = e->mutation_target ? analyze_lvalue(e->expr, s, ctx) : analyze_expr(e->expr, s, ctx);
                if (!is_ref_type(type)) die_at(e->line, e->col, "dereference needs REF, got %s", type);
                return set_type(e, ref_target_type(type));
            }
            if (strcmp(e->op, "hold") == 0) {
                char *type = analyze_expr(e->expr, s, ctx);
                if (is_task_type(type)) return set_type(e, task_result_type(type));
                if (is_array_type(type)) {
                    char *element_type = array_elem_type(type);
                    if (!is_task_type(element_type)) die_at(e->line, e->col, "hold array expects ARR[TSK[T]], got %s", type);
                    return set_type(e, array_type(task_result_type(element_type)));
                }
                die_at(e->line, e->col, "hold expects TSK[T] or ARR[TSK[T]], got %s", type);
            }
            char *type = analyze_expr(e->expr, s, ctx);
            if (strcmp(e->op, "-") == 0 || strcmp(e->op, "!") == 0) {
                type = implicit_ref_read_value(e->expr, type);
            }
            if (strcmp(e->op, "-") == 0 && type_eq(type, "INT")) return set_type(e, "INT");
            if (strcmp(e->op, "-") == 0 && type_eq(type, "FLT")) return set_type(e, "FLT");
            if (strcmp(e->op, "!") == 0 && type_eq(type, "BOL")) return set_type(e, "BOL");
            die_at(e->line, e->col, "operator '%s' cannot be applied to %s", e->op, type);
        }

        case NK_BINARY: {
            if (is_mutating_collection_plus_expr(e)) {
                die_at(e->line, e->col, "mutating '+' is statement-only; use it as '*target + value'");
            }

            if (strcmp(e->op, "else") == 0) {
                char *left = NULL;
                char *right = NULL;

                left = analyze_expr(e->left, s, ctx);
                if (type_eq(left, "NUL")) die_at(e->left->line, e->left->col, "else left side cannot be NUL");

                if (type_eq(left, "OBJ")) {
                    right = analyze_expr(e->right, s, ctx);
                    if (type_eq(right, "NUL") || type_eq(right, "OBJ")) {
                        die_at(e->right->line, e->right->col, "dynamic else fallback needs a concrete non-OBJ type, got %s", right);
                    }
                    return set_type(e, right);
                }

                right = analyze_expr_expected(e->right, s, ctx, left);
                if (!type_conforms_to_expected(right, left) && !type_assignable_to_expected(right, left)) {
                    die_at(e->right->line, e->right->col, "else fallback must be %s, got %s", left, right);
                }
                return set_type(e, left);
            }

            char *left = NULL;
            char *right = NULL;
            if ((strcmp(e->op, "==") == 0 || strcmp(e->op, "!=") == 0) && equality_operand_preserves_ref_identity(e->left, s)) {
                AnalyzeCtx ref_ctx = *ctx;
                ref_ctx.preserve_ref_identity = true;
                left = analyze_expr(e->left, s, &ref_ctx);
                ctx->next_loop_id = ref_ctx.next_loop_id;
            } else {
                left = analyze_expr(e->left, s, ctx);
            }

            if (strcmp(e->op, "+") == 0) {
                left = implicit_ref_read_value(e->left, left);
                if (is_array_type(left) || is_map_type(left) || is_struct_type(left) || is_open_data_type(left)) {
                    char *result_type = analyze_collection_plus_right(e, left, e->left, e->right, s, ctx);
                    return set_type(e, result_type);
                }
                right = analyze_expr(e->right, s, ctx);
                right = implicit_ref_read_value(e->right, right);
                if (type_eq(left, "STR") && type_eq(right, "STR")) return set_type(e, "STR");
            } else if ((strcmp(e->op, "==") == 0 || strcmp(e->op, "!=") == 0) && equality_operand_preserves_ref_identity(e->right, s)) {
                AnalyzeCtx ref_ctx = *ctx;
                ref_ctx.preserve_ref_identity = true;
                right = analyze_expr(e->right, s, &ref_ctx);
                ctx->next_loop_id = ref_ctx.next_loop_id;
            } else {
                right = analyze_expr(e->right, s, ctx);
            }

            if (strstr("+-*/%", e->op) && strlen(e->op) == 1) {
                left = implicit_ref_read_value(e->left, left);
                right = implicit_ref_read_value(e->right, right);
                if (strcmp(e->op, "%") == 0) {
                    if (type_eq(left, "INT") && type_eq(right, "INT")) return set_type(e, "INT");
                    die_at(e->line, e->col, "operator '%%' requires INT operands, got %s and %s", left, right);
                }
                char *numeric = numeric_binary_type(left, right);
                if (numeric) return set_type(e, numeric);
                die_at(e->line, e->col, "operator '%s' requires numeric operands, got %s and %s", e->op, left, right);
            }

            if (strcmp(e->op, "<") == 0 || strcmp(e->op, "<=") == 0 || strcmp(e->op, ">") == 0 || strcmp(e->op, ">=") == 0) {
                left = implicit_ref_read_value(e->left, left);
                right = implicit_ref_read_value(e->right, right);
                if (is_numeric_type(left) && is_numeric_type(right)) return set_type(e, "BOL");
                die_at(e->line, e->col, "operator '%s' requires numeric operands, got %s and %s", e->op, left, right);
            }

            if (strcmp(e->op, "==") == 0 || strcmp(e->op, "!=") == 0) {
                if ((type_eq(left, "NUL") && is_ref_type(right)) || (is_ref_type(left) && type_eq(right, "NUL"))) return set_type(e, "BOL");
                if (!(is_ref_type(left) && is_ref_type(right))) {
                    left = implicit_ref_read_value(e->left, left);
                    right = implicit_ref_read_value(e->right, right);
                }
                if (type_eq(left, right) && (type_eq(left, "INT") || type_eq(left, "FLT") || type_eq(left, "BOL") || type_eq(left, "STR") || is_ref_type(left))) return set_type(e, "BOL");
                if (is_numeric_type(left) && is_numeric_type(right)) return set_type(e, "BOL");
                die_at(e->line, e->col, "operator '%s' requires matching INT, FLT, BOL, STR, or REF operands", e->op);
            }

            if (strcmp(e->op, "&&") == 0 || strcmp(e->op, "||") == 0) {
                left = implicit_ref_read_value(e->left, left);
                right = implicit_ref_read_value(e->right, right);
                if (type_eq(left, "BOL") && type_eq(right, "BOL")) return set_type(e, "BOL");
                die_at(e->line, e->col, "operator '%s' requires BOL operands, got %s and %s", e->op, left, right);
            }

            die_at(e->line, e->col, "unknown binary operator '%s'", e->op);
        }

        case NK_CALL:
            return analyze_call(e, s, ctx);
        case NK_METHOD_CALL:
            return analyze_method_call(e, s, ctx);
        case NK_DECODE: {
            char *source_type = analyze_expr(e->expr, s, ctx);
            if (!is_open_data_type(source_type) && !type_eq(source_type, "OBJ")) die_at(e->expr->line, e->expr->col, "decode source must be JSON/DYN data, got %s", source_type);
            char *target_type = resolve_type(ctx->types, e->declared_type, e);
            e->declared_type = target_type;
            if (!is_struct_type(target_type) && !is_map_type(target_type)) {
                die_at(e->line, e->col, "JSON decode target must be a named MAP value type, got %s", target_type);
            }
            return set_type(e, target_type);
        }

        case NK_IF_EXPR: {
            char *value_type = analyze_expr(e->value, s, ctx);
            char *condition_type = analyze_expr(e->condition, s, ctx);
            if (!type_eq(condition_type, "BOL")) die_at(e->condition->line, e->condition->col, "conditional expression if must be BOL, got %s", condition_type);
            if (e->right) {
                char *else_type = analyze_expr(e->right, s, ctx);
                if (!type_eq(value_type, else_type)) {
                    char *numeric = numeric_binary_type(value_type, else_type);
                    if (!numeric) {
                        die_at(e->right->line, e->right->col, "if expression branches must produce the same type, got %s and %s", value_type, else_type);
                    }
                    value_type = numeric;
                }
            }
            return set_type(e, value_type);
        }

        default:
            die_at(e->line, e->col, "internal analyzer: bad expression %s", node_kind_name(e->kind));
    }

    return NULL;
}

// Analyzes an expression without external context, which is the default path because most local
// expressions should infer from their own shape.
static char *analyze_expr(Node *e, Scope *s, AnalyzeCtx *ctx) {
    return analyze_expr_expected(e, s, ctx, NULL);
}
