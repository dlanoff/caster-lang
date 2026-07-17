// Analyzer type compatibility, ownership, ref escape, and JSON conformance helpers.

// Checks direct assignment compatibility, which keeps boundary conformance simple because only a
// small set of implicit conversions are language rules.
static bool type_assignable_to_expected(const char *actual, const char *expected) {
    if (!expected) return true;
    if (type_eq(actual, expected)) return true;
    if (type_eq(actual, "NUL") && is_ref_type(expected)) return true;
    if (type_eq(actual, "NUL") && is_fn_type(expected)) return true;
    if (is_open_data_type(actual) && is_struct_type(expected)) return true;
    return false;
}

// Checks numeric scalar types, which centralizes INT/FLT promotion decisions because arithmetic
// paths share the same rules.
static bool is_numeric_type(const char *type) {
    return type_eq(type, "INT") || type_eq(type, "FLT");
}

// Checks built-in cast names, which keeps capitalized conversion calls distinct because
// INT/FLT/STR/ARR double as type syntax and functions.
static bool is_cast_call_name(const char *name) {
    return strcmp(name, "INT") == 0 || strcmp(name, "FLT") == 0 || strcmp(name, "STR") == 0 || strcmp(name, "ARR") == 0;
}

// Checks scalar cast sources, which limits runtime conversions because casts should be explicit
// but predictable.
static bool is_cast_source_type(const char *type) {
    return type_eq(type, "INT") || type_eq(type, "FLT") || type_eq(type, "BOL") || type_eq(type, "STR");
}

// Checks ARR[STR], which gates string joining because only string arrays can become STR without
// element conversion.
static bool is_str_array_type(const char *type) {
    return is_array_type(type) && type_eq(array_elem_type(type), "STR");
}

// Builds nested array types for shapes, which lets [10x10].fill infer dimensions because each
// shape axis adds one ARR layer.
static char *nested_array_type(const char *element_type, int dimensions) {
    char *type = xstrdup(element_type);
    for (int i = 0; i < dimensions; i++) type = array_type(type);
    return type;
}

// Checks values that may be manually released, which protects FREE because primitives and borrowed
// references have no owned runtime allocation.
static bool type_is_manual_freeable(const char *type) {
    if (!type) return false;
    if (type_eq(type, "STR") || type_eq(type, "OBJ") || type_eq(type, "OPEN")) return true;
    if (is_array_type(type) || is_map_type(type) || is_task_type(type)) return true;
    if (is_ref_type(type)) return true;
    if (is_open_struct_type(type)) return true;
    return false;
}

// Detects REF name borrows, which records alias sources because borrowed references must not
// escape their owner unsafely.
static bool expr_is_ref_borrow_of_name(Node *expr) {
    return expr && expr->kind == NK_UNARY && expr->op && strcmp(expr->op, "REF") == 0 &&
        expr->expr && expr->expr->kind == NK_NAME;
}

// Detects owned REF results, which distinguishes returned or allocated ownership because borrowed
// and owned refs share the same visible type.
static bool expr_produces_owned_ref(Node *expr) {
    return expr && expr->ref_result_owned;
}

// Clears stale borrow-source metadata, which prevents old alias facts from surviving reassignment
// because a variable can change ownership state.
static void symbol_clear_ref_source(Symbol *sym) {
    if (!sym) return;
    sym->ref_source_name = NULL;
    sym->ref_source_type = NULL;
}

// Records where a borrowed REF came from, which enables escape checks because the compiler tracks
// lifetimes by source symbol.
static void symbol_set_ref_source(Symbol *sym, const char *source_name, const char *source_type) {
    if (!sym) return;
    sym->ref_source_name = source_name ? xstrdup(source_name) : NULL;
    sym->ref_source_type = source_type ? xstrdup(source_type) : NULL;
}

// Copies borrow-source metadata from an expression, which propagates lifetime facts because
// aliases can flow through locals.
static void symbol_set_ref_source_from_expr(Symbol *sym, Node *value, Scope *s) {
    symbol_clear_ref_source(sym);
    if (!sym || !is_ref_type(sym->type) || !value) return;

    if (expr_is_ref_borrow_of_name(value)) {
        Symbol *source = scope_try_resolve(s, value->expr->name);
        if (source && !source->is_param && !source->is_global) {
            symbol_set_ref_source(sym, source->name, source->type);
        }
        return;
    }

    if (value->kind == NK_NAME) {
        Symbol *source_ref = scope_try_resolve(s, value->name);
        if (source_ref && is_ref_type(source_ref->type) && source_ref->ref_source_name) {
            symbol_set_ref_source(sym, source_ref->ref_source_name, source_ref->ref_source_type);
        }
    }
}

// Detects expressions that own independent runtime storage, which drives cleanup decisions because
// arrays, maps, strings, and calls may allocate.
static bool expr_produces_independent_heap_value(Node *expr, Scope *s, AnalyzeCtx *ctx) {
    if (!expr) return false;

    if (expr->kind == NK_NIL || expr->kind == NK_STR) return false;

    if (expr->kind == NK_UNARY && expr->op) {
        if (strcmp(expr->op, "REF") == 0 || strcmp(expr->op, "*") == 0) return false;
        return true;
    }

    if (expr->kind == NK_NAME) {
        if (expr->c_expr && strncmp(expr->c_expr, "flx_value_", 10) == 0) return true;
        Symbol *sym = scope_try_resolve(s, expr->name);
        if (sym) return sym->ownership != OWNERSHIP_FREED;
        TypeInfo *info = ctx && ctx->types ? type_find(ctx->types, expr->name) : NULL;
        return info && type_has_complete_named_value(ctx->types, info);
    }

    if (expr->kind == NK_DOT) return true;
    if (expr->kind == NK_ARRAY || expr->kind == NK_MAP_LITERAL || expr->kind == NK_RECORD_LITERAL) return true;
    if (expr->kind == NK_CALL || expr->kind == NK_METHOD_CALL || expr->kind == NK_DECODE) return true;
    if (expr->kind == NK_BINARY) return true;
    return false;
}

// Updates ownership after assignment, which keeps FREE and auto-drop correct because ownership
// follows the value-producing expression.
static void symbol_set_assignment_ownership(Symbol *sym, Node *value, Scope *s, AnalyzeCtx *ctx) {
    if (!sym) return;
    if (is_ref_type(sym->type)) {
        sym->ownership = expr_produces_owned_ref(value)
            ? OWNERSHIP_OWNED
            : OWNERSHIP_BORROWED;
        symbol_set_ref_source_from_expr(sym, value, s);
        return;
    }
    symbol_clear_ref_source(sym);
    if (!type_is_manual_freeable(sym->type)) {
        sym->ownership = OWNERSHIP_UNKNOWN;
        return;
    }

    sym->ownership = expr_produces_independent_heap_value(value, s, ctx)
        ? OWNERSHIP_OWNED
        : OWNERSHIP_BORROWED;
}

// Marks reassigned values as owned when appropriate, which makes later cleanup deterministic
// because runtime allocations need a responsible local.
static void symbol_mark_owned_if_freeable(Symbol *sym) {
    if (sym && type_is_manual_freeable(sym->type)) sym->ownership = OWNERSHIP_OWNED;
}

// Rejects escaping borrowed locals, which prevents dangling references because Caster does not
// expose manual lifetime annotations.
static void reject_local_ref_escape(Node *expr, Symbol *source, const char *escape_site) {
    if (!source || source->is_param || source->is_global) return;
    die_at(expr->line, expr->col,
        "REF escape through %s is rejected: '%s' is a function-local borrow; keep ownership in the caller and pass it to nested helpers with REF",
        escape_site,
        source->name);
}

// Marks or rejects REF escapes in an expression, which enforces borrow rules across returns,
// globals, and stored fields because aliases can hide inside structures.
static bool mark_ref_escape_expr(Node *expr, Scope *s, const char *escape_site) {
    if (!expr || !expr->checked_type || !is_ref_type(expr->checked_type)) return false;
    if (expr->kind == NK_NIL || expr->ref_result_owned) return expr->ref_result_owned;

    if (expr_is_ref_borrow_of_name(expr)) {
        Symbol *source = scope_try_resolve(s, expr->expr->name);
        if (source && !source->is_param && !source->is_global) {
            reject_local_ref_escape(expr, source, escape_site);
        }
        return false;
    }

    if (expr->kind == NK_NAME) {
        Symbol *sym = scope_try_resolve(s, expr->name);
        if (!sym || !is_ref_type(sym->type)) return false;
        if (sym->ownership == OWNERSHIP_OWNED) {
            expr->ref_result_owned = true;
            return true;
        }
        if (sym->ref_source_name) {
            Symbol *source = scope_try_resolve(s, sym->ref_source_name);
            if (source && !source->is_param && !source->is_global) {
                reject_local_ref_escape(expr, source, escape_site);
            }
        }
        return false;
    }

    return false;
}

// Checks whether storing into a target can outlive the source, which decides if a REF assignment
// is safe because heap/global fields can persist longer than locals.
static bool ref_assignment_target_may_escape(Node *target, Scope *s) {
    if (!target) return false;

    if (target->kind == NK_NAME) {
        Symbol *sym = scope_try_resolve(s, target->name);
        return sym && sym->is_global;
    }

    if (target->kind == NK_DOT) {
        Node *object = target->object;
        if (!object) return false;

        if (object->kind == NK_NAME) {
            Symbol *sym = scope_try_resolve(s, object->name);
            return sym && !sym->is_param;
        }

        if (object->kind == NK_UNARY && object->op && strcmp(object->op, "*") == 0 && object->expr && object->expr->kind == NK_NAME) {
            Symbol *sym = scope_try_resolve(s, object->expr->name);
            return sym && (sym->is_param || sym->is_global || sym->ownership == OWNERSHIP_OWNED);
        }
    }

    return false;
}

// Computes arithmetic result type, which keeps promotion consistent because INT plus FLT should
// become FLT everywhere.
static char *numeric_binary_type(const char *left, const char *right) {
    if (!is_numeric_type(left) || !is_numeric_type(right)) return NULL;
    return type_eq(left, "FLT") || type_eq(right, "FLT") ? "FLT" : "INT";
}

// Checks structural conformance, which allows open/anonymous data at typed boundaries because
// local inference can be looser than APIs.
static bool type_conforms_to_expected(const char *actual, const char *expected) {
    if (type_eq(actual, expected)) return true;
    if (type_eq(actual, "NUL") && is_ref_type(expected)) return true;
    if (type_eq(actual, "NUL") && is_fn_type(expected)) return true;
    if (type_eq(actual, "INT") && type_eq(expected, "FLT")) return true;
    return false;
}

// Finds the expression for a known literal key, which lets dynamic MAP dot reads stay typed when the key set was proven earlier.
static Node *expr_source_literal_key_expr(Node *expr, Scope *s, TypeTable *types, const char *key) {
    if (!expr || !key) return NULL;

    if (expr->kind == NK_MAP_LITERAL) {
        for (int i = 0; i < expr->fields.len; i++) {
            Node *entry = expr->fields.items[i];
            if (entry->target && entry->target->kind == NK_STR && strcmp(entry->target->text, key) == 0) return entry->value;
        }
        return NULL;
    }

    if (expr->kind == NK_RECORD_LITERAL) {
        for (int i = 0; i < expr->fields.len; i++) {
            Node *entry = expr->fields.items[i];
            if (entry->name && strcmp(entry->name, key) == 0) return entry->value;
        }
        return NULL;
    }

    if (expr->kind == NK_NAME) {
        Symbol *sym = scope_try_resolve(s, expr->name);
        if (sym && sym->node && sym->node->value) return expr_source_literal_key_expr(sym->node->value, s, types, key);

        TypeInfo *info = type_find(types, expr->name);
        if (info && info->kind == TYPEINFO_ALIAS && info->node->value && is_map_type(info->target)) {
            return expr_source_literal_key_expr(info->node->value, s, types, key);
        }
    }

    return NULL;
}

static bool validate_expr_conforms_to_type(Node *expr, Scope *s, AnalyzeCtx *ctx, const char *target_type);

// Validates open data against a fixed MAP, which makes JSON/DYN decoding explicit because runtime
// objects must match typed fields at the boundary.
static void validate_open_conforms_to_struct(Node *expr, Scope *s, AnalyzeCtx *ctx, const char *target_type) {
    TypeInfo *info = type_find(ctx->types, target_type);
    if (!info || info->kind != TYPEINFO_STRUCT) die_at(expr->line, expr->col, "expected fixed MAP type, got %s", target_type);

    for (int i = 0; i < info->node->fields.len; i++) {
        Node *field = info->node->fields.items[i];
        char *source_type = expr_source_literal_key_type(expr, s, ctx->types, field->name);
        if (!source_type) {
            if (!field_has_default(ctx->types, field)) {
                die_at(expr->line, expr->col, "missing required field '%s' for %s", field->name, target_type);
            }
            continue;
        }
        if (!type_conforms_to_expected(source_type, field->declared_type)) {
            Node *source_expr = expr_source_literal_key_expr(expr, s, ctx->types, field->name);
            if (source_expr && validate_expr_conforms_to_type(source_expr, s, ctx, field->declared_type)) continue;
            die_at(expr->line, expr->col, "field '%s' must be %s, got %s", field->name, field->declared_type, source_type);
        }
    }

    if (!is_open_struct_type(target_type)) {
        PtrVec keys = {0};
        collect_source_literal_keys(expr, s, ctx->types, &keys);
        for (int i = 0; i < keys.len; i++) {
            char *key = keys.items[i];
            if (!struct_field(ctx->types, target_type, key)) {
                die_at(expr->line, expr->col, "type %s has no field '%s'", target_type, key);
            }
        }
    }
}

// Validates expression conformance recursively, which protects typed returns and arguments because
// anonymous structures need to match their declared target.
static bool validate_expr_conforms_to_type(Node *expr, Scope *s, AnalyzeCtx *ctx, const char *target_type) {
    if (!expr || !target_type || !expr->checked_type) return false;

    if (type_conforms_to_expected(expr->checked_type, target_type)) return true;

    if (expr->kind == NK_NAME) {
        Symbol *sym = scope_try_resolve(s, expr->name);
        if (sym && sym->node && sym->node->value) return validate_expr_conforms_to_type(sym->node->value, s, ctx, target_type);
    }

    if (is_struct_type(target_type) && is_open_data_type(expr->checked_type)) {
        validate_open_conforms_to_struct(expr, s, ctx, target_type);
        return true;
    }

    if (is_array_type(target_type) && is_array_type(expr->checked_type) && expr->kind == NK_ARRAY) {
        char *target_element = array_elem_type(target_type);
        for (int i = 0; i < expr->elements.len; i++) {
            Node *item = expr->elements.items[i];
            if (!validate_expr_conforms_to_type(item, s, ctx, target_element)) return false;
        }
        return true;
    }

    if (is_map_type(target_type) && is_map_type(expr->checked_type) && expr->kind == NK_MAP_LITERAL) {
        char *target_key = map_key_type(target_type);
        char *target_value = map_value_type(target_type);
        if (!type_eq(target_key, "STR")) return false;
        for (int i = 0; i < expr->fields.len; i++) {
            Node *entry = expr->fields.items[i];
            if (!validate_expr_conforms_to_type(entry->value, s, ctx, target_value)) return false;
        }
        return true;
    }

    return false;
}

// Checks JSON/DYN source types, which keeps decoding limited to runtime objects because closed
// MAPs are already typed.
static bool is_json_dynamic_type(const char *type) {
    return type_eq(type, "OBJ") || is_open_data_type(type);
}

// Checks valid JSON decode targets, which prevents decoding into unsupported scalars because
// boundary decoders need shaped output.
static bool is_json_decode_target_type(const char *type) {
    return is_struct_type(type) || is_map_type(type) || is_array_type(type);
}

// Builds the decode boundary expression name, which lets the emitter call generated decoders
// because each target type needs a concrete C helper.
static char *json_decode_boundary_expr(const char *target_type, const char *source_type, const char *source_expr) {
    if (is_array_type(target_type)) {
        if (!type_eq(source_type, "OBJ")) return strf("flx_json_decode_%s(%s)", analyzer_lower_mangle(target_type), source_expr);
        return strf("flx_json_decode_%s(flx_obj_as_objarr(%s))", analyzer_lower_mangle(target_type), source_expr);
    }
    if (!type_eq(source_type, "OBJ")) return strf("flx_json_decode_%s(%s)", analyzer_lower_mangle(target_type), source_expr);
    return strf("flx_json_decode_%s(flx_obj_as_objmap(%s))", analyzer_lower_mangle(target_type), source_expr);
}

// Conforms an inferred open local to a target type, which avoids losing local flexibility because
// typed boundaries can pin the final shape.
static void conform_inferred_open_local(Node *expr, Scope *s, AnalyzeCtx *ctx, const char *target_type) {
    if (!expr || expr->kind != NK_NAME || !target_type || !is_json_decode_target_type(target_type)) return;
    Symbol *sym = scope_try_resolve(s, expr->name);
    if (!sym || !is_json_dynamic_type(sym->type)) return;
    if (type_eq(sym->type, "OBJ") && (!ctx || !ctx->allow_json_decode_boundary)) return;

    if (is_open_data_type(sym->type) && is_struct_type(target_type)) {
        validate_open_conforms_to_struct(expr, s, ctx, target_type);
    }
    expr->declared_type = xstrdup(target_type);
    expr->c_expr = json_decode_boundary_expr(target_type, sym->type, sym->c_expr ? sym->c_expr : sym->name);
    set_type(expr, target_type);
}

static char *analyze_expr(Node *e, Scope *s, AnalyzeCtx *ctx);
static char *analyze_expr_expected(Node *e, Scope *s, AnalyzeCtx *ctx, const char *expected);
static void analyze_stmt(Node *st, Scope *s, AnalyzeCtx *ctx);
static char *analyze_array_add_result(Node *call, const char *object_type, Scope *s, AnalyzeCtx *ctx);
static char *analyze_collection_plus_right(Node *at, const char *left_type, Node *left, Node *right, Scope *s, AnalyzeCtx *ctx);
static void refine_assignment_target(Node *target, Scope *s, const char *type);
static void analyze_block(Node *b, Scope *parent, AnalyzeCtx *ctx);
