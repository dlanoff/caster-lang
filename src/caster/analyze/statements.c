// Analyzer statement, block, local declaration, and global mutation checks.

// Recognizes built-in module names, which blocks accidental local declarations from shadowing
// native adapters because imported modules are value-like namespaces.
static bool is_native_module_name(const char *name) {
    return type_eq(name, "BUF") ||
        type_eq(name, "FS") ||
        type_eq(name, "IO") ||
        type_eq(name, "OS") ||
        type_eq(name, "PATH") ||
        type_eq(name, "PROC") ||
        type_eq(name, "REQ") ||
        type_eq(name, "SQL") ||
        type_eq(name, "TEST") ||
        type_eq(name, "WEB");
}

// Recognizes native qualified function values, which supports WEB/REQ callback wiring because
// adapter functions can be passed across typed boundaries.
static bool is_native_qualified_fn_name(const char *name) {
    if (!name) return false;
    const char *sep = strchr(name, '_');
    if (!sep) return false;
    char *prefix = xstrndup(name, (size_t)(sep - name));
    bool native = is_native_module_name(prefix);
    free(prefix);
    return native;
}

// Finds user function calls inside local initializers, which enforces explicit local type
// checkpoints because call-derived values cross typed function boundaries.
static bool expr_contains_required_typed_call(Node *expr, AnalyzeCtx *ctx) {
    if (!expr) return false;

    switch (expr->kind) {
        case NK_CALL: {
            if (!expr->callee || expr->callee->kind != NK_NAME) return false;
            TypeInfo *info = ctx && ctx->types ? type_find(ctx->types, expr->callee->name) : NULL;
            if (info && info->kind == TYPEINFO_STRUCT) return true;
            FnInfo *fn = ctx && ctx->fns ? fn_find(ctx->fns, expr->callee->name) : NULL;
            return fn && !is_native_qualified_fn_name(expr->callee->name);
        }
        case NK_METHOD_CALL: {
            if (expr->object && expr->object->kind == NK_NAME) {
                char *qualified = strf("%s_%s", expr->object->name, expr->name);
                TypeInfo *info = ctx && ctx->types ? type_find(ctx->types, qualified) : NULL;
                if (info && info->kind == TYPEINFO_STRUCT) return true;
                FnInfo *fn = ctx && ctx->fns ? fn_find(ctx->fns, qualified) : NULL;
                if (fn && !is_native_module_name(expr->object->name)) return true;
            }
            return false;
        }
        default:
            return false;
    }
}

// Analyzes FREE statements, which rejects invalid releases because cleanup must not free borrowed
// or non-owned values.
static void analyze_free_stmt(Node *st, Scope *s, AnalyzeCtx *ctx) {
    Node *expr = st->expr;
    if (!expr || expr->kind != NK_NAME) {
        die_at(st->line, st->col, "FREE requires a plain owned local name");
    }

    Symbol *sym = scope_try_resolve(s, expr->name);
    if (!sym) {
        TypeInfo *info = type_find(ctx->types, expr->name);
        if (info) die_at(st->line, st->col, "FREE cannot release top-level named MAP value '%s'", expr->name);
        die_at(st->line, st->col, "unknown name '%s'", expr->name);
    }

    if (sym->is_param) die_at(st->line, st->col, "FREE cannot release borrowed parameter '%s'", expr->name);
    if (sym->is_global) die_at(st->line, st->col, "FREE cannot release global value '%s'", expr->name);
    if (!type_is_manual_freeable(sym->type)) die_at(st->line, st->col, "FREE requires a heap-backed owned value, got %s", sym->type);
    if (sym->ownership == OWNERSHIP_FREED) die_at(st->line, st->col, "value '%s' is already freed", expr->name);
    if (sym->ownership != OWNERSHIP_OWNED) die_at(st->line, st->col, "FREE requires a provably owned local value; '%s' is borrowed or has unknown ownership", expr->name);

    set_type(expr, sym->type);
    set_type(st, "NUL");
    sym->ownership = OWNERSHIP_FREED;
}

static TypeInfo *destructure_struct_info(Node *st, AnalyzeCtx *ctx, const char *source_type) {
    TypeInfo *info = type_find(ctx->types, source_type);
    if (!info || info->kind != TYPEINFO_STRUCT || info->open_runtime) {
        die_at(st->value->line, st->value->col, "destructure expects a fixed MAP result, got %s", source_type);
    }
    return info;
}

static bool destructure_uses_property_fields(Node *st) {
    if (!st->fields.len) return false;
    Node *first = st->fields.items[0];
    bool property = first->text != NULL;
    for (int i = 1; i < st->fields.len; i++) {
        Node *field = st->fields.items[i];
        if ((field->text != NULL) != property) {
            die_at(field->line, field->col, "destructure pattern cannot mix property fields and positional fields");
        }
    }
    return property;
}

static void analyze_destructure_stmt(Node *st, Scope *s, AnalyzeCtx *ctx) {
    if (!st->fields.len) die_at(st->line, st->col, "destructure pattern cannot be empty");

    char *source_type = analyze_expr(st->value, s, ctx);
    TypeInfo *info = destructure_struct_info(st, ctx, source_type);
    st->declared_type = xstrdup(source_type);

    bool property = destructure_uses_property_fields(st);
    if (!property && st->fields.len != info->node->fields.len) {
        die_at(st->line, st->col, "positional destructure of %s expects %d bindings, got %d", source_type, info->node->fields.len, st->fields.len);
    }

    for (int i = 0; i < st->fields.len; i++) {
        Node *binding = st->fields.items[i];
        char *declared = resolve_type(ctx->types, binding->declared_type, binding);
        binding->declared_type = declared;

        Node *source_field = property
            ? struct_field(ctx->types, source_type, binding->text)
            : info->node->fields.items[i];
        if (!source_field) {
            die_at(binding->line, binding->col, "type %s has no field '%s'", source_type, binding->text ? binding->text : "<positional>");
        }
        if (!binding->text) binding->text = xstrdup(source_field->name);

        if (!type_assignable_to_expected(source_field->declared_type, declared)) {
            die_at(binding->line, binding->col, "destructure local '%s' must be %s, got %s from %s.%s",
                binding->name,
                declared,
                source_field->declared_type,
                source_type,
                source_field->name);
        }

        Symbol *sym = scope_define(s, binding->name, declared, NULL, binding);
        if (is_ref_type(declared)) sym->ownership = OWNERSHIP_BORROWED;
        else symbol_mark_owned_if_freeable(sym);
        set_type(binding, declared);
    }

    set_type(st, "NUL");
}

// Analyzes statements, which resolves declaration-vs-assignment, control flow, mutation, and
// returns because statement syntax drives most local type state.
static void analyze_stmt(Node *st, Scope *s, AnalyzeCtx *ctx) {
    switch (st->kind) {
        case NK_VAR: {
            char *declared = resolve_type(ctx->types, st->declared_type, st);
            st->declared_type = declared;
            if (ctx->enforce_local_style && (!st->value || !expr_contains_required_typed_call(st->value, ctx))) {
                die_at(st->line, st->col, "explicit local type for '%s' is only allowed when initializing from a user-written function call or MAP constructor", st->name);
            }
            if (st->value) {
                char *value_type = analyze_expr_expected(st->value, s, ctx, declared);
                if (!type_assignable_to_expected(value_type, declared)) {
                    die_at(st->value->line, st->value->col, "cannot initialize %s as %s with %s", st->name, declared, value_type);
                }
            }
            Symbol *sym = scope_define(s, st->name, declared, NULL, st);
            symbol_set_assignment_ownership(sym, st->value, s, ctx);
            if (st->value && is_map_type(declared)) {
                symbol_set_source_literal_keys_from_expr(sym, st->value, s, ctx->types);
            }
            set_type(st, "NUL");
            break;
        }

        case NK_ASSIGN: {
            const char *op = st->op ? st->op : "=";

            if (st->target->kind == NK_NAME && strcmp(op, "=") == 0) {
                Symbol *existing = scope_try_resolve(s, st->target->name);

                if (!existing) {
                    char *value_type = analyze_expr(st->value, s, ctx);
                    if (ctx->enforce_local_style && expr_contains_required_typed_call(st->value, ctx)) {
                        die_at(st->line, st->col, "local '%s' starts from a user-written function call or MAP constructor; declare the local type explicitly", st->target->name);
                    }
                    if (type_eq(value_type, "NUL")) {
                        die_at(st->value->line, st->value->col, "cannot infer a local variable from NUL");
                    }
                    st->inferred_decl = true;
                    st->name = xstrdup(st->target->name);
                    st->declared_type = xstrdup(value_type);
                    Symbol *sym = scope_define(s, st->name, value_type, NULL, st);
                    symbol_set_assignment_ownership(sym, st->value, s, ctx);
                    if (is_map_type(value_type) || is_struct_type(value_type) || is_open_data_type(value_type)) {
                        symbol_set_source_literal_keys_from_expr(sym, st->value, s, ctx->types);
                    }
                    set_type(st, "NUL");
                    break;
                }

                if (existing->ownership != OWNERSHIP_FREED) {
                    analyze_name(st->target, s);
                } else {
                    if (existing->c_expr) st->target->c_expr = xstrdup(existing->c_expr);
                    set_type(st->target, existing->type);
                }
                char *value_type = analyze_expr_expected(st->value, s, ctx, existing->type);
                set_type(st->target, existing->type);

                if (is_unknown_array_type(existing->type) && is_array_type(value_type) && !is_unknown_array_type(value_type)) {
                    refine_symbol_type(existing, value_type);
                    set_type(st->target, value_type);
                    set_type(st, "NUL");
                    break;
                }

                if (!type_assignable_to_expected(value_type, existing->type)) {
                    die_at(st->value->line, st->value->col, "cannot assign %s to %s target", value_type, existing->type);
                }
                if (existing->is_global && is_ref_type(existing->type)) mark_ref_escape_expr(st->value, s, "global assignment");
                if (is_map_type(existing->type) || is_struct_type(existing->type) || is_open_data_type(existing->type)) {
                    symbol_set_source_literal_keys_from_expr(existing, st->value, s, ctx->types);
                }
                symbol_set_assignment_ownership(existing, st->value, s, ctx);
                set_type(st, "NUL");
                break;
            }

            char *target_type = analyze_lvalue(st->target, s, ctx);
            if (strcmp(op, "++") == 0) {
                if (!type_eq(target_type, "INT")) die_at(st->target->line, st->target->col, "++ requires INT target, got %s", target_type);
                set_type(st, "NUL");
                break;
            }

            if (strcmp(op, "+=") == 0) {
                if (type_eq(target_type, "INT")) {
                    char *value_type = analyze_expr(st->value, s, ctx);
                    if (!type_eq(value_type, "INT")) die_at(st->line, st->col, "+= on INT requires INT value");
                    set_type(st, "NUL");
                    break;
                }

                if (type_eq(target_type, "FLT")) {
                    char *value_type = analyze_expr(st->value, s, ctx);
                    if (!is_numeric_type(value_type)) die_at(st->line, st->col, "+= on FLT requires numeric value");
                    set_type(st, "NUL");
                    break;
                }

                if (type_eq(target_type, "STR")) {
                    char *value_type = analyze_expr(st->value, s, ctx);
                    if (!type_eq(value_type, "STR")) die_at(st->line, st->col, "+= on STR requires STR value");
                    if (st->target->kind == NK_NAME) symbol_mark_owned_if_freeable(scope_resolve(s, st->target->name, st->target));
                    set_type(st, "NUL");
                    break;
                }

                if (is_array_type(target_type) || is_map_type(target_type) ||
                    is_struct_type(target_type) || is_open_data_type(target_type)) {
                    die_at(st->line, st->col, "+= only supports INT, FLT, and STR targets");
                }

                char *result_type = analyze_join_right(st, target_type, st->value, s, ctx);
                refine_assignment_target(st->target, s, result_type);
                if (st->target->kind == NK_NAME && (is_map_type(result_type) || is_struct_type(result_type))) {
                    Symbol *target_sym = scope_resolve(s, st->target->name, st->target);
                    symbol_add_source_literal_keys_from_expr(target_sym, st->value, s, ctx->types);
                }
                if (st->target->kind == NK_NAME) symbol_mark_owned_if_freeable(scope_resolve(s, st->target->name, st->target));
                if (is_unknown_array_type(target_type)) target_type = result_type;
                if (!type_eq(target_type, result_type)) die_at(st->line, st->col, "+= cannot assign %s to %s", result_type, target_type);
                set_type(st, "NUL");
                break;
            }

            if (strcmp(op, "-=") == 0) {
                if (type_eq(target_type, "INT")) {
                    char *value_type = analyze_expr(st->value, s, ctx);
                    if (!type_eq(value_type, "INT")) die_at(st->line, st->col, "-= on INT requires INT value");
                    set_type(st, "NUL");
                    break;
                }

                if (type_eq(target_type, "FLT")) {
                    char *value_type = analyze_expr(st->value, s, ctx);
                    if (!is_numeric_type(value_type)) die_at(st->line, st->col, "-= on FLT requires numeric value");
                    set_type(st, "NUL");
                    break;
                }

                if (is_array_type(target_type)) {
                    char *element_type = array_elem_type(target_type);
                    char *value_type = analyze_expr(st->value, s, ctx);
                    if (!type_eq(value_type, element_type)) die_at(st->value->line, st->value->col, "array remove needs %s, got %s", element_type, value_type);
                    if (st->target->kind == NK_NAME) symbol_mark_owned_if_freeable(scope_resolve(s, st->target->name, st->target));
                    set_type(st, "NUL");
                    break;
                }

                if (is_map_type(target_type)) {
                    char *key_type = map_key_type(target_type);
                    char *value_type = analyze_expr(st->value, s, ctx);
                    if (!type_eq(value_type, key_type)) die_at(st->value->line, st->value->col, "map remove needs %s key, got %s", key_type, value_type);
                    if (st->target->kind == NK_NAME && st->value->kind == NK_STR) {
                        Symbol *target_sym = scope_resolve(s, st->target->name, st->target);
                        symbol_remove_source_literal_key(target_sym, st->value->text);
                    }
                    if (st->target->kind == NK_NAME) symbol_mark_owned_if_freeable(scope_resolve(s, st->target->name, st->target));
                    set_type(st, "NUL");
                    break;
                }

                if (is_struct_type(target_type)) {
                    if (st->value->kind == NK_NAME && !scope_try_resolve(s, st->value->name) &&
                        struct_field(ctx->types, target_type, st->value->name)) {
                        die_at(st->line, st->col, "known-field MAP -= %s is not supported in v0.1", st->value->name);
                    }

                    mark_struct_open(ctx->types, target_type);

                    if (st->value->kind == NK_NAME && !scope_try_resolve(s, st->value->name)) {
                        Node *key = node_new(NK_STR, (Token){TK_STRING_LITERAL, st->value->name, st->value->line, st->value->col});
                        key->text = xstrdup(st->value->name);
                        set_type(key, "STR");
                        st->value = key;
                    } else {
                        char *value_type = analyze_expr(st->value, s, ctx);
                        if (!type_eq(value_type, "STR")) die_at(st->value->line, st->value->col, "open MAP remove needs STR key, got %s", value_type);
                    }

                    if (st->target->kind == NK_NAME && st->value->kind == NK_STR) {
                        Symbol *target_sym = scope_resolve(s, st->target->name, st->target);
                        symbol_remove_source_literal_key(target_sym, st->value->text);
                    }
                    if (st->target->kind == NK_NAME) symbol_mark_owned_if_freeable(scope_resolve(s, st->target->name, st->target));
                    set_type(st, "NUL");
                    break;
                }

                die_at(st->line, st->col, "-= is not implemented for %s", target_type);
            }

            char *value_type = analyze_expr_expected(st->value, s, ctx, target_type);
            if (!type_assignable_to_expected(value_type, target_type)) {
                die_at(st->value->line, st->value->col, "cannot assign %s to %s target", value_type, target_type);
            }
            if (is_ref_type(target_type) && ref_assignment_target_may_escape(st->target, s)) mark_ref_escape_expr(st->value, s, "stored reference assignment");
            set_type(st, "NUL");
            break;
        }

        case NK_DESTRUCTURE:
            analyze_destructure_stmt(st, s, ctx);
            break;

        case NK_FREE:
            analyze_free_stmt(st, s, ctx);
            break;

        case NK_EXPR_STMT:
            if (is_mutating_collection_plus_expr(st->expr)) {
                analyze_mutating_collection_plus_stmt(st->expr, s, ctx);
            } else if (is_mutating_collection_method_expr(st->expr)) {
                analyze_mutating_collection_method_stmt(st->expr, s, ctx);
            } else {
                analyze_expr(st->expr, s, ctx);
            }
            set_type(st, "NUL");
            break;

        case NK_RET: {
            bool old_allow_json_decode_boundary = ctx->allow_json_decode_boundary;
            ctx->allow_json_decode_boundary = true;
            char *return_type = st->value ? analyze_expr_expected(st->value, s, ctx, ctx->fn->declared_return_type) : "NUL";
            ctx->allow_json_decode_boundary = old_allow_json_decode_boundary;
            if (ctx->fn->declared_return_type && !type_assignable_to_expected(return_type, ctx->fn->declared_return_type)) {
                die_at(st->line, st->col, "function '%s' must return %s, got %s", ctx->fn->name, ctx->fn->declared_return_type, return_type);
            }
            char *effective_return_type = (ctx->fn->declared_return_type && type_assignable_to_expected(return_type, ctx->fn->declared_return_type))
                ? ctx->fn->declared_return_type
                : return_type;
            if (is_ref_type(effective_return_type) && mark_ref_escape_expr(st->value, s, "return")) {
                ctx->fn->returns_owned_ref = true;
            }
            if (!ctx->saw_return) {
                ctx->saw_return = true;
                ctx->return_type = xstrdup(effective_return_type);
            } else if (!type_eq(ctx->return_type, effective_return_type)) {
                die_at(st->line, st->col, "function '%s' cannot return both %s and %s", ctx->fn->name, ctx->return_type, effective_return_type);
            }
            set_type(st, effective_return_type);
            break;
        }

        case NK_IF: {
            char *condition_type = analyze_expr(st->condition, s, ctx);
            if (!type_eq(condition_type, "BOL")) die_at(st->condition->line, st->condition->col, "if condition must be BOL, got %s", condition_type);
            analyze_block(st->then_block, s, ctx);
            for (int i = 0; i < st->elx_branches.len; i++) {
                Node *branch = st->elx_branches.items[i];
                char *branch_type = analyze_expr(branch->condition, s, ctx);
                if (!type_eq(branch_type, "BOL")) die_at(branch->condition->line, branch->condition->col, "elx condition must be BOL, got %s", branch_type);
                analyze_block(branch->body, s, ctx);
                set_type(branch, "NUL");
            }
            if (st->else_block) analyze_block(st->else_block, s, ctx);
            set_type(st, "NUL");
            break;
        }

        case NK_LOOP: {
            if (st->args.len != 1 && st->args.len != 2 && st->args.len != 3) {
                die_at(st->line, st->col, "loop expects 1 argument or 2-3 range arguments");
            }

            char *types[3] = {0};
            for (int i = 0; i < st->args.len; i++) types[i] = analyze_expr(st->args.items[i], s, ctx);

            int id = ctx->next_loop_id++;
            st->index_name = st->name ? strf("%s_i", st->name) : strf("loop_i_%d", id);
            Scope *loop_scope = scope_new(s);
            scope_define(loop_scope, "i", "INT", st->index_name, st);

            if (st->args.len == 2 || st->args.len == 3) {
                if (st->loop_hint) die_at(st->line, st->col, "loop collection wrappers only take one expression");
                for (int i = 0; i < st->args.len; i++) {
                    if (!type_eq(types[i], "INT")) die_at(st->line, st->col, "range loop arguments must all be INT");
                }
                st->loop_kind = xstrdup("range");
                scope_define(loop_scope, "e", "INT", st->index_name, st);
                if (st->name) scope_define_ctx(loop_scope, st->name, loop_ctx_type("INT"), st->index_name, st->index_name, st);
                analyze_loop_body(st->body, loop_scope, ctx);
                set_type(st, "NUL");
                break;
            }

            if (type_eq(types[0], "INT")) {
                if (st->loop_hint) die_at(st->line, st->col, "loop collection wrapper cannot wrap INT");
                st->loop_kind = xstrdup("count");
                scope_define(loop_scope, "e", "INT", st->index_name, st);
                if (st->name) scope_define_ctx(loop_scope, st->name, loop_ctx_type("INT"), st->index_name, st->index_name, st);
                analyze_loop_body(st->body, loop_scope, ctx);
                set_type(st, "NUL");
                break;
            }

            if (type_eq(types[0], "BOL")) {
                if (st->loop_hint) die_at(st->line, st->col, "loop collection wrapper cannot wrap BOL");
                st->loop_kind = xstrdup("condition");
                if (st->name) scope_define_ctx(loop_scope, st->name, loop_ctx_type("INT"), st->index_name, NULL, st);
                analyze_loop_body(st->body, loop_scope, ctx);
                set_type(st, "NUL");
                break;
            }

            if (is_array_type(types[0])) {
                if (st->loop_hint && !type_eq(st->loop_hint, "array")) {
                    die_at(st->line, st->col, "loop({value}) expects a MAP/open object, got %s", types[0]);
                }
                Node *arg = st->args.items[0];
                if (!st->loop_hint && arg && arg->kind != NK_ARRAY) {
                    die_at(arg->line, arg->col, "ARR values use loop([value])");
                }
                if (st->loop_hint && arg->kind == NK_CALL && arg->callee && arg->callee->kind == NK_NAME &&
                    strcmp(arg->callee->name, "ARR") == 0 && arg->args.len == 1 &&
                    type_eq(((Node *)arg->args.items[0])->checked_type, "STR")) {
                    if (st->name) die_at(st->line, st->col, "STR sequence loop does not take 'as'; use e and i");
                    st->loop_kind = xstrdup("str");
                    st->value = arg->args.items[0];
                    st->element_ptr = strf("char_%d", id);
                    scope_define(loop_scope, "e", "STR", st->element_ptr, st);
                    analyze_loop_body(st->body, loop_scope, ctx);
                    set_type(st, "NUL");
                    break;
                }
                st->loop_kind = xstrdup("array");
                st->element_ptr = st->name ? strf("%s_e", st->name) : strf("loop_e_%d", id);
                char *element_type = array_elem_type(types[0]);
                char *element_expr = strf("(*%s)", st->element_ptr);
                scope_define(loop_scope, "e", element_type, element_expr, st);
                if (st->name) scope_define_ctx(loop_scope, st->name, loop_ctx_type(element_type), st->index_name, element_expr, st);
                scope_define_loop_source_ctx(loop_scope, s, arg, st->index_name, element_expr, element_type, ctx->loop_depth + 2);
                analyze_loop_body(st->body, loop_scope, ctx);
                set_type(st, "NUL");
                break;
            }

            if (type_eq(types[0], "STR")) {
                if (st->loop_hint) die_at(st->line, st->col, "loop([value]) expects an ARR value; use ARR(text) to iterate a STR");
                if (st->args.items[0] && ((Node *)st->args.items[0])->kind != NK_STR) {
                    die_at(((Node *)st->args.items[0])->line, ((Node *)st->args.items[0])->col, "STR variables must use loop([ARR(text)])");
                }
                if (st->name) die_at(st->line, st->col, "STR loop does not take 'as'; use e and i");
                st->loop_kind = xstrdup("str");
                st->element_ptr = strf("char_%d", id);
                scope_define(loop_scope, "e", "STR", st->element_ptr, st);
                analyze_loop_body(st->body, loop_scope, ctx);
                set_type(st, "NUL");
                break;
            }

            if (is_open_struct_type(types[0]) || is_open_data_type(types[0]) || type_eq(types[0], "OBJ")) {
                if (st->loop_hint && !type_eq(st->loop_hint, "map")) {
                    die_at(st->line, st->col, "loop([value]) expects an ARR value, got %s", types[0]);
                }
                if (!st->loop_hint && st->args.items[0] && ((Node *)st->args.items[0])->kind != NK_MAP_LITERAL) {
                    die_at(((Node *)st->args.items[0])->line, ((Node *)st->args.items[0])->col, "MAP/open values use loop({value})");
                }
                if (st->name) die_at(st->line, st->col, "open MAP loop does not take 'as'; use e, val, and i");
                st->loop_kind = xstrdup("open_map");
                st->element_ptr = strf("entry_%d", id);
                scope_define(loop_scope, "e", "STR", strf("%s->key", st->element_ptr), st);
                scope_define(loop_scope, "val", "OBJ", strf("%s->value", st->element_ptr), st);
                analyze_loop_body(st->body, loop_scope, ctx);
                set_type(st, "NUL");
                break;
            }

            if (is_map_type(types[0])) {
                if (st->loop_hint && !type_eq(st->loop_hint, "map")) {
                    die_at(st->line, st->col, "loop([value]) expects an ARR value, got %s", types[0]);
                }
                if (!st->loop_hint && st->args.items[0] && ((Node *)st->args.items[0])->kind != NK_MAP_LITERAL) {
                    die_at(((Node *)st->args.items[0])->line, ((Node *)st->args.items[0])->col, "MAP values use loop({value})");
                }
                if (st->name) die_at(st->line, st->col, "map loop does not take 'as'; use e, val, and i");
                st->loop_kind = xstrdup("map");
                st->element_ptr = strf("entry_%d", id);
                char *key_type = map_key_type(types[0]);
                char *value_type = map_value_type(types[0]);
                scope_define(loop_scope, "e", key_type, strf("%s->key", st->element_ptr), st);
                scope_define(loop_scope, "val", value_type, strf("%s->value", st->element_ptr), st);
                analyze_loop_body(st->body, loop_scope, ctx);
                set_type(st, "NUL");
                break;
            }

            die_at(((Node *)st->args.items[0])->line, ((Node *)st->args.items[0])->col, "loop does not support %s", types[0]);
        }

        case NK_PASS:
            set_type(st, "NUL");
            break;

        case NK_THROW: {
            char *type = analyze_expr(st->value, s, ctx);
            if (!type_eq(type, "STR")) die_at(st->value->line, st->value->col, "throw expects STR, got %s", type);
            set_type(st, "NUL");
            break;
        }

        default:
            die_at(st->line, st->col, "internal analyzer: bad statement %s", node_kind_name(st->kind));
    }
}

// Finds the root of an assignment target, which lets block analysis track local initialization
// because nested field writes still belong to one owning value.
static Node *assignment_root_name(Node *target) {
    if (!target) return NULL;
    if (target->kind == NK_NAME) return target;
    if (target->kind == NK_DOT) return assignment_root_name(target->object);
    if (target->kind == NK_UNARY && target->op && strcmp(target->op, "*") == 0) return NULL;
    return NULL;
}

// Analyzes a block with its own scope, which keeps locals and nested helpers bounded because
// Caster ownership is function/block scoped.
static void analyze_block(Node *b, Scope *parent, AnalyzeCtx *ctx) {
    Scope *scope = scope_new(parent);

    for (int i = 0; i < b->statements.len; i++) {
        Node *st = b->statements.items[i];
        if (st->kind != NK_FN) continue;

        if (!st->c_expr) st->c_expr = nested_function_c_name(ctx && ctx->fn && ctx->fn->node ? ctx->fn->node->name : NULL, st);

        Symbol *sym = scope_define(scope, st->name, "NUL", st->c_expr, st);
        sym->ownership = OWNERSHIP_BORROWED;
        set_type(st, st->declared_type);
    }

    for (int i = 0; i < b->statements.len; i++) {
        Node *st = b->statements.items[i];
        if (st->kind == NK_FN) {
            FnInfo *local_fn = fn_find(ctx->fns, st->c_expr);
            if (local_fn && local_fn->state != 2) analyze_fn(local_fn, ctx->fns, ctx->types, ctx->global_scope);
            continue;
        }
        if (st->kind == NK_ASSIGN && st->target && st->target->kind != NK_NAME) {
            Node *root = assignment_root_name(st->target);
            if (root && root->name && !scope_try_resolve(scope, root->name)) {
                TypeInfo *info = type_find(ctx->types, root->name);
                if (info && type_has_complete_named_value(ctx->types, info)) {
                    die_at(root->line, root->col, "direct mutation of top-level named value '%s' inside a function is rejected; pass REF %s to make mutation visible in the signature", root->name, root->name);
                }
            }
        }
        analyze_stmt(b->statements.items[i], scope, ctx);
    }
    set_type(b, "NUL");
}

// Checks that inference has resolved to concrete types, which catches leftover unknowns before
// emission because generated C cannot represent unresolved Caster types.
static void validate_concrete_types(Node *n) {
    if (!n) return;

    if (n->inferred_decl && is_unknown_type(n->declared_type)) {
        die_at(n->line, n->col, "could not infer a concrete type for local '%s' before C emission", n->name ? n->name : "<anonymous>");
    }

    if (n->checked_type && is_unknown_type(n->checked_type)) {
        die_at(n->line, n->col, "could not infer a concrete type for %s before C emission", node_kind_name(n->kind));
    }

    switch (n->kind) {
        case NK_PROGRAM:
            for (int i = 0; i < n->globals.len; i++) validate_concrete_types(n->globals.items[i]);
            for (int i = 0; i < n->functions.len; i++) validate_concrete_types(n->functions.items[i]);
            for (int i = 0; i < n->statements.len; i++) validate_concrete_types(n->statements.items[i]);
            break;
        case NK_FN:
            validate_concrete_types(n->body);
            break;
        case NK_BLOCK:
            for (int i = 0; i < n->statements.len; i++) validate_concrete_types(n->statements.items[i]);
            break;
        case NK_VAR:
        case NK_ASSIGN:
            validate_concrete_types(n->target);
            validate_concrete_types(n->value);
            break;
        case NK_DESTRUCTURE:
            for (int i = 0; i < n->fields.len; i++) validate_concrete_types(n->fields.items[i]);
            validate_concrete_types(n->value);
            break;
        case NK_AGG_STEP:
            validate_concrete_types(n->value);
            break;
        case NK_RET:
        case NK_THROW:
        case NK_EXPR_STMT:
            validate_concrete_types(n->value);
            validate_concrete_types(n->expr);
            break;
        case NK_IF:
            validate_concrete_types(n->condition);
            validate_concrete_types(n->then_block);
            for (int i = 0; i < n->elx_branches.len; i++) validate_concrete_types(n->elx_branches.items[i]);
            validate_concrete_types(n->else_block);
            break;
        case NK_ELX:
            validate_concrete_types(n->condition);
            validate_concrete_types(n->body);
            break;
        case NK_LOOP:
            for (int i = 0; i < n->args.len; i++) validate_concrete_types(n->args.items[i]);
            validate_concrete_types(n->body);
            break;
        case NK_ARRAY:
        case NK_SHAPE:
            for (int i = 0; i < n->elements.len; i++) validate_concrete_types(n->elements.items[i]);
            break;
        case NK_MAP_LITERAL:
        case NK_RECORD_LITERAL:
            for (int i = 0; i < n->fields.len; i++) validate_concrete_types(n->fields.items[i]);
            break;
        case NK_FIELD:
            validate_concrete_types(n->target);
            validate_concrete_types(n->value);
            break;
        case NK_DOT:
            validate_concrete_types(n->object);
            validate_concrete_types(n->index);
            validate_concrete_types(n->target);
            break;
        case NK_UNARY:
            validate_concrete_types(n->expr);
            break;
        case NK_BINARY:
            validate_concrete_types(n->left);
            validate_concrete_types(n->right);
            break;
        case NK_CALL:
            validate_concrete_types(n->callee);
            for (int i = 0; i < n->args.len; i++) validate_concrete_types(n->args.items[i]);
            break;
        case NK_METHOD_CALL:
            validate_concrete_types(n->object);
            for (int i = 0; i < n->args.len; i++) validate_concrete_types(n->args.items[i]);
            validate_concrete_types(n->body);
            break;
        case NK_DECODE:
            validate_concrete_types(n->expr);
            break;
        case NK_IF_EXPR:
            validate_concrete_types(n->value);
            validate_concrete_types(n->condition);
            validate_concrete_types(n->right);
            break;
        default:
            break;
    }
}
